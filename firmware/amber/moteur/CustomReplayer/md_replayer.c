#include <nds.h>

#include "md_replayer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../DS/ds_voix.h"
#include "../DS/opl_voix.h"
#include "xt_midi.h"
#include "md_lock.h"

// ── Plus de puce Mega Drive ─────────────────────────────────────────────────
// Le son sort des voies de la DS (ds_voix) et du MIDI (xt_midi). Les appels à
// la puce qui subsistent ne sont plus que dans des branches mortes — voies PSG,
// patchs FM — et ne font donc plus rien.
#define md_chip_reset(r)             ((void)(r))
#define md_chip_ym_write(p, r, v)    ((void)(p), (void)(r), (void)(v))
#define md_chip_ym_key(c, on)        ((void)(c), (void)(on))
#define md_chip_psg_set_period(c, p) ((void)(c), (void)(p))
#define md_chip_psg_set_volume(c, v) ((void)(c), (void)(v))
#define md_chip_psg_set_noise(m)     ((void)(m))
#define md_chip_psg_set_pan(c, p)    ((void)(c), (void)(p))
#define md_chip_pcm_stop()           ((void)0)
#define md_chip_pcm_enable(on)       ((void)(on))
#define md_chip_set_ladder(e)        ((void)(e))
#define md_chip_get_ladder()         (false)

// Horloge VIRTUELLE du séquenceur. Le moteur compte son tempo en
// « échantillons par tick » ; il n'y a plus d'échantillons à produire, mais on
// garde cette unité, avec une horloge assez fine pour que l'arrondi ne fausse
// pas le tempo : un tick à 50 Hz en vaut 20 000.
#define MD_HORLOGE_VIRTUELLE 1000000

md_lock_t replayer_lock = MD_LOCK_INIT;

static md_module_t *current_module = NULL;
// La banque d'échantillons, hors du module : un morceau neuf ou ouvert ne
// réalloue pas deux mégaoctets, et il n'en existe jamais deux copies.
static uint8_t md_banque[MD_PCM_BANK_BYTES];
static md_play_status_t play_status = MD_STOPPED;
static int replay_sample_rate = 44100;

// ============================================================================
// Représentation interne de la hauteur
//
// On CONSERVE la représentation « block(3 bits) | F-Num(10 bits) » héritée du
// moteur d'origine : tous les effets (slides, portamento, vibrato, arpège) ont
// été écrits autour d'elle et continuent donc de fonctionner à l'identique.
// Elle est traduite en Hz puis en registres YM2612 / SN76489 au moment de
// l'écriture (md_apply_pitch).
// ============================================================================

// ⚠️ XM MIDI TRACKER : LA REPRÉSENTATION BLOCK/F-NUM A ÉTÉ REMPLACÉE.
// Les variables *_fnum gardent leur nom, mais portent une HAUTEUR LINÉAIRE au
// 1/128 de demi-ton : note × 128 + désaccord de l'instrument. C'est la
// finesse du finetune XM. La cadence de lecture d'un échantillon s'en déduit
// exactement (md_hz_echantillon), sans les arrondis de la puce FM, et un
// glissando avance par pas égaux sur toute l'octave au lieu de pas qui
// doublaient d'un bout à l'autre.
#define MD_PAS_DEMITON 128
#define MD_PAS_OCTAVE  (12 * MD_PAS_DEMITON)
// Un pas de glissando de l'ancien moteur valait 2,5 à 5 cents selon la place
// dans l'octave ; quatre pas de hauteur font 3,1 cents, partout pareils. Les
// commandes gardent ainsi leur vitesse d'origine.
#define MD_PAS_GLISSE  4
#define MD_HAUTEUR_MAX (MD_MAX_NOTE * MD_PAS_DEMITON + MD_PAS_OCTAVE)

// 2^(i / MD_PAS_OCTAVE) en virgule fixe Q16 : de 65 536 à 131 012. Le calcul
// de hauteur se fait sous interruption, en entiers seulement.
static uint32_t md_puiss2[MD_PAS_OCTAVE];

static void md_prepare_puiss2(void) {
  const double pas = 1.0004513695322617;   // 2^(1/1536)
  double r = 1.0;
  for (int i = 0; i < MD_PAS_OCTAVE; i++) {
    md_puiss2[i] = (uint32_t)(r * 65536.0 + 0.5);
    r *= pas;
  }
}

// La table de sinus du KICK, préparée par md_replayer_init : elle est définie
// avec le reste de la grosse caisse, tout en bas.
static void md_prepare_sinus(void);

// Hauteur d'une note, désaccord compris, en 1/128 de demi-ton.
// La hauteur d'une note, EXACTEMENT comme AT2 et Amber : bloc (l'octave,
// plafonnee a 7) et F-Num de la table, plus le F-TUNE de l'instrument ajoute au
// F-Num. Codee (bloc << 10) | F-Num, comme le registre B0/A0 de l'OPL3 : tous
// les effets (glissandos, portamento, vibrato, macros) travaillent dessus,
// sans conversion, donc sans arrondi.
static const uint16_t md_fnums_at2[12] = {0x157, 0x16B, 0x181, 0x198, 0x1B0, 0x1CA,
                                          0x1E5, 0x202, 0x220, 0x241, 0x263, 0x287};
static uint16_t md_hauteur(int note, int ftune) {
  if (note < 1) note = 1;
  if (note > MD_MAX_NOTE) note = MD_MAX_NOTE;
  const int octave = (note - 1) / 12;
  const int bloc = octave < 8 ? octave : 7;
  const int fnum = (int)md_fnums_at2[(note - 1) % 12] + ftune;
  return (uint16_t)((bloc << 10) | (fnum & 0x3FF));
}

// Transposition propre aux voies PSG.
//
// Le compteur de période du SN76489 fait 10 bits : à 1023 il est au bout, soit
// 3579545/(32*1023) = 109.35 Hz. La puce ne descend pas plus bas, point.
// Sans correction, tout ce qui est écrit sous cette limite sature sur la même
// note et C-0 = C-1 = C-2 = un bourdon plat.
//
// Les canaux PSG ont donc leur propre transposition : 2 octaves vers l'aigu.
// La transposition est appliquée au tout dernier moment, à l'écriture du
// registre : la grille de notes, les slides, le portamento et le vibrato
// continuent de travailler sur la même représentation que le FM.
//
// Pourquoi 2 et pas 3 : avec 3, une note écrite sur une voie PSG sonnait une
// octave au-dessus de la même note dans DefleMask — il fallait écrire C-3 pour
// obtenir son C-4. Vérifié à l'oreille sur un morceau importé.
//
// ⚠️ À numéro de note égal, une voie PSG sonne donc 2 octaves au-dessus d'une
// voie FM, et les neuf demi-tons sous A-0 tapent le plancher de la puce et
// sonnent tous pareil. C'est le prix à payer pour que les noms de notes du PSG
// veuillent dire la même chose ici et dans DefleMask.
// Vérifié à l'oreille contre DefleMask : avec 3, une note écrite sur une voie
// PSG sonnait UNE OCTAVE TROP HAUT — il fallait écrire C-3 pour obtenir le C-4
// du fichier d'origine. 2 remet les noms de notes du PSG en accord avec ce que
// joue DefleMask.
//
// Contrepartie assumée : le SN76489 ne descend pas sous 109 Hz (période 10 bits
// à 3,58 MHz). Avec ce décalage, les notes en dessous de A-0 tapent ce plancher
// et sonnent toutes pareil. Ça ne concerne plus que les neuf demi-tons les plus
// graves, contre presque trois octaves si on retirait une octave de plus.
#define MD_PSG_OCTAVE_SHIFT 2

// ── État par canal logique ──────────────────────────────────────────────────
// bit 5 (0x20) = key-on, bits 0-4 = poids forts de block/F-Num. Format conservé
// tel quel : de nombreux effets le lisent/écrivent directement.
static uint8_t ch_b0_reg[MD_TOTAL_VOICES] = {0};
static uint16_t ch_cur_fnum[MD_TOTAL_VOICES] = {0}; // hauteur réellement émise
static int ch_current_instr[MD_TOTAL_VOICES] = {0};
static uint8_t ch_current_note[MD_TOTAL_VOICES] = {0};

// ── Tables d'instrument en cours de lecture ─────────────────────────────────
// Une ligne par tick, rebouclée tant que la note dure. Le canal mémorise aussi
// la transposition courante, pour ne réécrire la hauteur que lorsqu'elle change
// — sinon on écraserait à chaque tick le travail des slides et du vibrato.
static int ch_table[MD_TOTAL_VOICES];       // table en cours, -1 = aucune
static bool ch_table_running[MD_TOTAL_VOICES];
static int8_t ch_table_tsp[MD_TOTAL_VOICES];
// Position de chacun des trois flux (voir MD_TABLE_STREAMS dans l'en-tête).
static uint8_t ch_table_pos[MD_TOTAL_VOICES][MD_TABLE_STREAMS];
// Compteur de répétitions restantes pour chaque H rencontré. Il est indexé par
// la LIGNE qui porte le saut, ce qui permet d'imbriquer les boucles : chaque H
// compte ses propres tours sans écraser ceux d'un H englobant.
static uint8_t ch_hop_left[MD_TOTAL_VOICES][MD_TABLE_STREAMS][MD_TABLE_ROWS];
// La table de ce canal a-t-elle été imposée par une commande A ? Dans ce cas
// elle l'emporte sur celle de l'instrument, et le reste jusqu'au prochain A —
// une note suivante ne doit pas la reprendre.
static bool ch_table_forced[MD_TOTAL_VOICES];
static uint8_t ch_panning[MD_TOTAL_VOICES];
// La vélocité de la ligne, telle quelle, sur 7 bits. L'atténuation interne du
// moteur d'effets ne compte que 64 crans ; quand aucun effet de volume n'est
// venu la modifier, on sort la vélocité EXACTE, donc 128 crans.
static uint8_t ch_vel_fin[MD_TOTAL_VOICES];
// Retard des échantillons, en millisecondes, pour se caler sur le MIDI. Réglage
// global ; les accesseurs sont tout en bas, avec le reste des réglages.
static int md_sync_ms = 0;

// Voie matérielle utilisée par chaque canal logique.
//   0-5 → FM 1-6 (YM2612)   6-9 → PSG (3 tons + bruit)
// Le canal d'audition (MD_TEST_CHANNEL) vole dynamiquement une voie FM libre.
static uint8_t ch_hw_voice[MD_TOTAL_VOICES];

// Volumes abstraits (atténuation 0 = fort … 63 = silence), exactement comme
// dans le moteur d'origine : les effets de volume n'ont pas bougé d'une ligne.
typedef struct {
  uint8_t volC;         // atténuation appliquée aux opérateurs PORTEURS
  uint8_t volM;         // atténuation appliquée aux opérateurs MODULATEURS
  uint8_t feedbackConn; // bit 0 = algorithme additif (ALG >= 4), bits 1-3 = FB
} md_voice_par_t;
static md_voice_par_t ch_fmpar[MD_TOTAL_VOICES];

// Copie de travail de l'instrument par canal : les effets « live » (EXTENDED3,
// set algorithm…) la modifient sans toucher à l'instrument du module.
static md_instr_t ch_instr_shadow[MD_TOTAL_VOICES];

// ── Réglages MD CMD encore actifs sur le canal ──────────────────────────────
// Une commande MD CMD règle un REGISTRE de la puce, pas une note : elle doit
// donc tenir tant qu'on ne la change pas, y compris sur les notes suivantes.
//
// Or chaque note recharge l'instrument dans la copie de travail et effaçait la
// retouche : un « 20 10 » posé une ligne plus haut était bien appliqué, puis
// annulé par la note d'après, qui sonnait donc sans lui.
//
// On mémorise donc ce qui a été posé, et on le rejoue après chaque
// rechargement d'instrument.
#define MD_MDCMD_SLOTS 64
// Vrai quand la colonne porte un instrument destiné à l'autre puce : elle
// reste muette jusqu'à ce qu'un instrument compatible y soit posé.
static bool ch_instr_blocked[MD_TOTAL_VOICES];
static uint8_t ch_mdcmd_val[MD_TOTAL_VOICES][MD_MDCMD_SLOTS];
static bool ch_mdcmd_on[MD_TOTAL_VOICES][MD_MDCMD_SLOTS];
// Ce réglage vient-il d'une TABLE (et non d'une phrase) ? Voir md_refresh_table.
static bool ch_mdcmd_from_table[MD_TOTAL_VOICES][MD_MDCMD_SLOTS];

// Rejoue sur le canal tous les réglages MD CMD encore actifs.
static void md_apply_mdcmd(int c, uint8_t cmd, uint8_t val,
                           uint8_t row_cmd, int slot);
// Le mode de bruit est-il tenu par une commande sur ce canal ?
//
// Une commande MD CMD règle un registre et tient jusqu'à ce qu'on la change ;
// l'instrument, lui, se recharge à chaque note, et sa macro de bruit se rejoue
// à chaque tick. Sans ce garde-fou, l'instrument reprenait la main sur ce que
// la commande avait posé — c'est bien la COMMANDE qui doit primer.
static bool md_noise_forced(int c);   // défini avec la table des commandes

// Un CC MIDI est un message ponctuel : il n'y a rien à rejouer après une note.
static void md_reapply_mdcmds(int c) { (void)c; }

// Runtime Effect Tracking
static uint8_t ch_volslide_val[MD_TOTAL_VOICES] = {0};
static uint16_t ch_porta_target[MD_TOTAL_VOICES] = {0};
static uint8_t ch_porta_speed[MD_TOTAL_VOICES] = {0};
static uint8_t ch_fslide_speed[MD_TOTAL_VOICES] = {0};
static uint8_t ch_vibrato_speed[MD_TOTAL_VOICES] = {0};
static uint8_t ch_vibrato_depth[MD_TOTAL_VOICES] = {0};
static uint8_t ch_vibrato_pos[MD_TOTAL_VOICES] = {0};
static uint8_t ch_tremolo_speed[MD_TOTAL_VOICES] = {0};
static uint8_t ch_tremolo_depth[MD_TOTAL_VOICES] = {0};
static uint8_t ch_tremolo_pos[MD_TOTAL_VOICES] = {0};
static uint8_t ch_tremor_val[MD_TOTAL_VOICES] = {0};
static uint8_t ch_tremor_count[MD_TOTAL_VOICES] = {0};
static uint8_t ch_arpeggio_val[MD_TOTAL_VOICES] = {0};
static uint8_t ch_arpeggio_state[MD_TOTAL_VOICES] = {0};
static uint16_t ch_base_fnum[MD_TOTAL_VOICES] = {0}; // hauteur de référence

// Arpège de prévisualisation (audition piano). -1 = inactif.
static int test_arp_channel = -1;

// Canal de la chanson sur lequel se trouve le curseur d'édition. L'audition
// (piano, saisie d'une note) emprunte une voie du MÊME type que ce canal :
// sans ça, écrire une note sur PSG1 ou sur NOIS faisait entendre du FM, ce qui
// n'a aucun sens — sur Mega Drive ces colonnes ne sortent que du SN76489.
static int audition_channel = 0;

// Globals
static uint8_t replay_global_vol = 63;
// Emplacements d'effet actifs par canal :
//   0-1 → les commandes de la ligne de PHRASE (le moteur d'origine)
//   2-3 → les deux colonnes CMD de la TABLE de l'instrument
// Les séparer évite qu'une table écrase la commande écrite dans la phrase.
#define MD_EFF_SLOTS 8
// Emplacement de chaque commande de la ligne : CMD, puis FX1-FX4.
static const uint8_t md_slot_ev[MD_EV_EFFETS] = {0, 1, 4, 6, 7};
#define MD_EFF_SLOT_TABLE 2
// La colonne MD CMD peut porter un effet DefleMask : il lui faut son propre
// emplacement, sinon elle écraserait la commande CMD de la même ligne.
#define MD_EFF_SLOT_MDCMD 4        // MD CMD d'une ligne de phrase
#define MD_EFF_SLOT_TABLE_MD 5     // MD CMD d'une ligne de table
static uint8_t ch_active_eff[MD_TOTAL_VOICES][MD_EFF_SLOTS] = {
    [0 ... MD_TOTAL_VOICES - 1] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
static uint8_t ch_active_eff_val[MD_TOTAL_VOICES][MD_EFF_SLOTS] = {0};
static uint8_t ch_note_delay[MD_TOTAL_VOICES] = {0};
static uint8_t ch_note_cut[MD_TOTAL_VOICES] = {0};
static uint8_t ch_retrig_speed[MD_TOTAL_VOICES] = {0};
static uint8_t ch_retrig_count[MD_TOTAL_VOICES] = {0};

// Global effect states
static int8_t global_volslide_val = 0;
static int16_t global_fslide_speed = 0;

static bool md_channel_muted[32] = {false};

static const uint8_t vibtrem_table[32] = {
    0,   24,  49,  74,  97,  120, 141, 161, 180, 197, 212,
    224, 235, 244, 250, 253, 255, 253, 250, 244, 235, 224,
    212, 197, 180, 161, 141, 120, 97,  74,  49,  24};

// ============================================================================
// Couche YM2612 / SN76489
// ============================================================================

// Plus aucune voie PSG : les branches qui les traitaient restent, mortes.
static inline bool md_hw_is_psg(uint8_t hw) { (void)hw; return false; }
static inline uint8_t md_hw_psg_index(uint8_t hw) { (void)hw; return 0; }

// ── MIDI : la note qui sonne sur chaque piste ───────────────────────────────
// Une piste MIDI est MONOPHONIQUE, comme une voie de LSDJ : une nouvelle note
// éteint la précédente. On retient donc laquelle sonne, et sur quel canal.
static uint8_t ch_midi_note[MD_TOTAL_VOICES];    // numéro MIDI, 0xFF = rien
static uint8_t ch_midi_canal[MD_TOTAL_VOICES];
static uint8_t ch_midi_ticks[MD_TOTAL_VOICES];   // ticks depuis le départ
// La note du TRACKER (1-108) que la piste doit faire entendre maintenant,
// transpositions comprises. Une note MIDI ne glisse pas : quand elle change
// (arpège, TSP de table), on la rejoue.
static uint8_t ch_note_emise[MD_TOTAL_VOICES];
// Dernier programme envoyé sur chaque canal MIDI, pour ne pas le renvoyer à
// chaque note. 0xFF = inconnu.
static uint8_t midi_programme[16];

// Définies avec l'horloge, plus bas.
static void md_ds_silence(void);
static void md_ds_horloge_repart(void);

// ── Enveloppe des voies PSG ─────────────────────────────────────────────────
// Le SN76489 n'a pas d'enveloppe matérielle : son unique contrôle de niveau est
// un registre d'atténuation sur 4 bits. Sur une vraie Megadrive, c'est le CPU
// qui le réécrit image par image en déroulant une table de volumes — c'est ce
// que font SMPS, GEMS ou Echo. On fait exactement pareil : un pas de table par
// tick du replayer, valeurs 0-15, avec un point de boucle et un point de
// release. Rien ici n'emprunte quoi que ce soit aux paramètres FM.
typedef struct {
  uint8_t active;    // 0 = voie au repos
  uint8_t point;     // point de rupture courant de l'enveloppe (0 … 2)
  int32_t level256;  // amplitude courante en 1/256e d'unité (0 … 15*256)
  uint8_t instr;     // instrument dont on lit les réglages (0 = aucun)
  uint8_t noise_mode;
  uint8_t nz_pos;    // index courant dans la macro de mode de bruit
  uint8_t vol_pos;   // index courant dans la macro de VOLUME (v9)
  uint8_t arp_pos;   // index courant dans la macro d'ARPÈGE (v9)
} md_psg_env_t;
static md_psg_env_t psg_env[MD_NUM_PSG_CHANNELS];

// Index suivant d'une macro de longueur `len` : on reboucle si un point de
// bouclage est défini, sinon on tient la dernière valeur.
static int md_macro_next(int pos, int len, uint8_t loop) {
  int next = pos + 1;
  if (next < len)
    return next;
  if (loop != MD_EMPTY && loop < len)
    return loop;
  return len - 1;
}

// Amplitude audible courante de la voie, de 0 (silence) à 15 (maximum).
static uint8_t md_env_level(const md_psg_env_t *e) {
  int v = e->level256 >> 8;
  return (uint8_t)(v < 0 ? 0 : (v > 15 ? 15 : v));
}

// Vitesse de rampe, en 1/256e d'unité d'amplitude par tick.
//
// Le manuel de LSDJ dit « Speed 1 is fastest, F is slowest, 0 means hold ».
// On traduit ça par un temps de parcours de toute l'échelle qui croît comme le
// carré de la vitesse : 1 → un tick (instantané), 8 → un peu plus d'une
// seconde, F → environ quatre secondes et demie. Plus le chiffre monte, plus le
// segment s'étire — c'est bien le sens de l'écran.
static int32_t md_env_rate(uint8_t speed) {
  if (speed == 0)
    return 0;                       // 0 = tenir : on ne bouge pas
  int32_t r = (15 * 256) / ((int32_t)speed * speed);
  return r < 1 ? 1 : r;
}

// Amplitude de départ de l'enveloppe (point 0). Un instrument sans enveloppe
// définie sonne au maximum, comme une voie dont on écrirait l'atténuation une
// seule fois.
static int32_t md_env_start_level(const md_instr_t *ins) {
  if (!ins || ins->env_amp[0] == MD_EMPTY)
    return 15 * 256;
  uint8_t a = ins->env_amp[0];
  return (int32_t)(a > 15 ? 15 : a) * 256;
}

// Instrument dont la voie PSG `p` lit ses réglages (NULL si aucun).
static const md_instr_t *md_psg_env_instr(const md_psg_env_t *e) {
  if (!current_module || e->instr == 0 || e->instr > MD_MAX_INSTRUMENTS)
    return NULL;
  return &current_module->instruments[e->instr - 1];
}

static bool md_channel_is_pcm(int c);
static bool md_channel_is_opl(int c);

// Le désaccord de l'instrument joué sur la piste, en 1/128 de demi-ton.
static int md_ftune_de(int ch) {
  if (current_module && ch_current_instr[ch] > 0 &&
      ch_current_instr[ch] <= MD_MAX_INSTRUMENTS)
    return current_module->instruments[ch_current_instr[ch] - 1].fine_tune;
  return 0;
}

// L'échantillon joué par l'instrument de la piste, ou NULL.
static const md_sample_t *md_sample_de_piste(int ch) {
  if (!current_module) return NULL;
  const int si = ch_instr_shadow[ch].pcm_sample;
  if (si < 0 || si >= MD_MAX_SAMPLES) return NULL;
  const md_sample_t *sm = &current_module->samples[si];
  return sm->length ? sm : NULL;
}

// La hauteur d'un échantillon, c'est sa VITESSE DE LECTURE : il n'y a pas
// d'oscillateur à accorder. On étire la cadence d'enregistrement selon l'écart
// entre la hauteur voulue et la note de référence de l'échantillon — dans la
// même représentation que les effets, pour que glissandos et vibrato suivent.
// La hauteur est celle de l'OPL3 (bloc et F-Num, comme AT2) : on la convertit
// en frequence, et la cadence de lecture suit le rapport entre cette
// frequence et celle de la note de reference de l'echantillon.
static double md_hz_opl(uint16_t fnum_bloc) {
  const int bloc = (fnum_bloc >> 10) & 7, fnum = fnum_bloc & 0x3FF;
  return fnum * 49716.0 / (double)(1 << (20 - bloc));
}
static uint16_t md_hauteur(int note, int ftune);
static uint32_t md_hz_echantillon(int ch, const md_sample_t *sm) {
  const int base = sm->base_note ? sm->base_note : 49;
  const double ref = md_hz_opl(md_hauteur(base, 0));
  const double hz = (double)(sm->rate ? sm->rate : MD_PCM_RATE) *
                    md_hz_opl(ch_cur_fnum[ch]) / (ref > 0 ? ref : 1);
  return hz > 4.0e9 ? 0xFFFFFFFFu : (uint32_t)(hz + 0.5);
}

// Écrit la hauteur sur la voie de la piste. Un échantillon change de cadence ;
// une note MIDI, elle, ne se rejoue qu'en changeant de NOTE (voir
// md_midi_suit), pas pour un glissando.
static void md_apply_pitch(int ch) {
  if (ch < 0 || ch >= MD_TOTAL_VOICES)
    return;
  if (md_channel_is_opl(ch)) {
    opl_voix_hauteur(ch_hw_voice[ch], ch_cur_fnum[ch], false);
    return;
  }
  if (!md_channel_is_pcm(ch))
    return;
  const md_sample_t *sm = md_sample_de_piste(ch);
  if (sm) ds_voix_freq(ch_hw_voice[ch], md_hz_echantillon(ch, sm));
}

// Met à jour la hauteur courante (et le cache ch_b0_reg) puis l'envoie à la puce.
static void md_write_freq(int ch, uint16_t fnum_block) {
  if (ch < 0 || ch >= MD_TOTAL_VOICES)
    return;
  ch_cur_fnum[ch] = fnum_block;
  ch_b0_reg[ch] = (uint8_t)((ch_b0_reg[ch] & 0xE0) | ((fnum_block >> 8) & 0x1F));
  md_apply_pitch(ch);
}

// Atténuation finale d'un canal : volume abstrait + volume global + mute.
static uint8_t md_effective_atten(int ch, uint8_t atten) {
  if (ch >= 0 && ch < 32 && md_channel_muted[ch])
    return 63;
  if (replay_global_vol >= 63)
    return atten;
  int loud = 63 - (int)atten;
  loud = (loud * ((int)replay_global_vol + 1)) >> 6;
  int res = 63 - loud;
  if (res > 63)
    res = 63;
  if (res < 0)
    res = 0;
  return (uint8_t)res;
}

// Position stéréo : 0-255 côté tracker (128 = centre), 0-127 côté DS.
static int md_pan_ds(int ch) {
  return ((int)ch_panning[ch] * 127 + 127) / 255;
}

// Force 0-127 de la piste : colonne VEL et commandes de volume (l'atténuation
// 0-63 du moteur), volume général, mute, puis volume propre de l'instrument.
static int md_force_piste(int ch, uint8_t atten) {
  const int loud = 63 - (int)md_effective_atten(ch, (uint8_t)(atten & 0x3F));
  int iv = ch_instr_shadow[ch].pcm_volume;
  if (iv > 127) iv = 127;   // la voie de la DS ne sait pas « pousser » au-delà
  // ── 128 crans quand c'est possible ────────────────────────────────────────
  // L'atténuation interne n'a que 64 crans : la ramener en 0-127 saute un cran
  // sur deux. Or, tant qu'aucun effet n'a touché au volume, la valeur exacte
  // est connue — c'est la vélocité de la ligne. On la sort telle quelle.
  int force;
  const int loudVel = ((int)ch_vel_fin[ch] * 63 + 63) / 127;
  if (loud == loudVel && replay_global_vol >= 63 &&
      !(ch >= 0 && ch < 32 && md_channel_muted[ch]))
    force = ch_vel_fin[ch];
  else
    force = loud * 127 / 63;
  const int v = force * iv / 127;
  return v < 0 ? 0 : (v > 127 ? 127 : v);
}

// Applique une atténuation (0-63) sur la piste. `atten` peut être temporaire
// (trémolo, tremor) sans être mémorisée dans ch_fmpar. Une note MIDI garde la
// vélocité de son départ : il n'y a rien à réécrire pour elle.
static void md_write_carrier_volume(int ch, uint8_t atten) {
  if (ch < 0 || ch >= MD_TOTAL_VOICES || !current_module)
    return;
  if (md_channel_is_opl(ch)) {
    // Comme AT2 : la porteuse prend le volume, le modulateur ne le prend que
    // si l'algorithme est additif (sinon il changerait le timbre).
    const bool additif = (ch_fmpar[ch].feedbackConn & 1) != 0;
    opl_voix_volume(ch_hw_voice[ch], md_effective_atten(ch, atten),
                    additif ? md_effective_atten(ch, ch_fmpar[ch].volM)
                            : ch_fmpar[ch].volM);
    return;
  }
  if (!md_channel_is_pcm(ch))
    return;
  ds_voix_vol(ch_hw_voice[ch], md_force_piste(ch, atten), md_pan_ds(ch));
}

// Réécrit le volume courant du canal (porteuses + modulateurs).
static void md_write_volume(int ch) {
  md_write_carrier_volume(ch, ch_fmpar[ch].volC);
}

// Arme la table de l'instrument du canal (si l'instrument en a une). Appelée à
// chaque note : la table repart toujours de sa première ligne.
static void md_table_start(int ch) {
  ch_table_tsp[ch] = 0;
  // Les effets que la table avait armés repartent de zéro. Le moteur ne réarme
  // un effet que s'il CHANGE — sans cet oubli volontaire, un vibrato continu
  // serait relancé à chaque tick — mais du coup, une table modifiée pendant un
  // silence gardait l'ancien couple (effet, valeur) et la note suivante ne
  // rejouait rien de neuf.
  for (int sl = 0; sl < MD_TABLE_STREAMS - 1; sl++)
    ch_active_eff[ch][MD_EFF_SLOT_TABLE + sl] = 0xFF;
  for (int s = 0; s < MD_TABLE_STREAMS; s++) {
    ch_table_pos[ch][s] = 0;
    for (int r = 0; r < MD_TABLE_ROWS; r++)
      ch_hop_left[ch][s][r] = 0;
  }
  // Une commande A a imposé la table : elle prime sur celle de l'instrument.
  // On repart simplement de sa première ligne — mais il faut la RÉARMER, car
  // le note-off précédent a éteint la lecture. En sortant d'ici sans le faire,
  // la table se taisait dès la deuxième note et ne revenait qu'au prochain
  // départ de lecture : en composant, on modifiait la table sans plus rien
  // entendre bouger.
  if (ch_table_forced[ch]) {
    ch_table_running[ch] = (ch_table[ch] >= 0 && ch_table[ch] < MD_MAX_TABLES);
    return;
  }

  ch_table[ch] = -1;
  ch_table_running[ch] = false;
  if (!current_module)
    return;
  int ins_idx = ch_current_instr[ch];
  if (ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS)
    return;
  uint8_t t = current_module->instruments[ins_idx - 1].table;
  if (t == MD_EMPTY || t >= MD_MAX_TABLES)
    return;
  ch_table[ch] = t;
  ch_table_running[ch] = true;
}

// Commande A : démarre la table donnée sur ce canal, ou l'arrête (valeur >= 20
// d'après le manuel de LSDJ). C'est la seule commande qui touche à la table
// elle-même plutôt qu'au son.
static void md_cmd_table_select(int ch, uint8_t val) {
  if (ch < 0 || ch >= MD_TOTAL_VOICES)
    return;
  // À partir d'ici c'est A qui décide, plus l'instrument.
  ch_table_forced[ch] = true;
  if (val >= 0x20 || val >= MD_MAX_TABLES) {
    ch_table_running[ch] = false;
    return;
  }
  ch_table[ch] = val;
  ch_table_running[ch] = true;
  ch_table_tsp[ch] = 0;
  for (int s = 0; s < MD_TABLE_STREAMS; s++) {
    ch_table_pos[ch][s] = 0;
    for (int r = 0; r < MD_TABLE_ROWS; r++)
      ch_hop_left[ch][s][r] = 0;
  }
}

// ── MIDI ────────────────────────────────────────────────────────────────────
static void md_midi_note_off(int ch) {
  if (ch_midi_note[ch] == 0xFF) return;
  xt_midi_send((uint8_t)(0x80 | ch_midi_canal[ch]), ch_midi_note[ch], 0);
  ch_midi_note[ch] = 0xFF;
}

// Joue ch_note_emise sur le canal de l'instrument. La vélocité vient de la
// force de la piste au moment du départ, comme sur un clavier.
static void md_midi_note_on(int ch) {
  const md_instr_t *ins = &ch_instr_shadow[ch];
  const uint8_t canal = ins->midi_channel & 0x0F;
  md_midi_note_off(ch);
  if (ins->midi_program < 128 && midi_programme[canal] != ins->midi_program) {
    xt_midi_send((uint8_t)(0xC0 | canal), ins->midi_program, 0);
    midi_programme[canal] = ins->midi_program;
  }
  int note = (int)ch_note_emise[ch] + 11;   // C-0 du tracker = note MIDI 12
  if (note < 0) note = 0;
  if (note > 127) note = 127;
  int vel = md_force_piste(ch, ch_fmpar[ch].volC);
  if (vel < 1) vel = 1;
  xt_midi_send((uint8_t)(0x90 | canal), (uint8_t)note, (uint8_t)vel);
  ch_midi_note[ch] = (uint8_t)note;
  ch_midi_canal[ch] = canal;
  ch_midi_ticks[ch] = 0;
}

// La note à faire entendre a changé (arpège, transposition de table) : une
// voie MIDI qui sonne rejoue la nouvelle note.
static void md_midi_suit(int ch, int note) {
  ch_note_emise[ch] = (uint8_t)note;
  if (md_channel_is_pcm(ch) || ch_midi_note[ch] == 0xFF) return;
  if ((int)ch_midi_note[ch] != note + 11) md_midi_note_on(ch);
}

// Démarre l'échantillon de l'instrument sur la voie de la piste.
static void md_voix_demarre(int ch) {
  const md_sample_t *sm = md_sample_de_piste(ch);
  if (!sm || md_channel_muted[ch]) { ds_voix_stop(ch_hw_voice[ch]); return; }
  ds_voix_joue(ch_hw_voice[ch], md_banque + sm->offset, sm->length, sm->bits,
               sm->loop, md_hz_echantillon(ch, sm),
               md_force_piste(ch, ch_fmpar[ch].volC), md_pan_ds(ch));
  ds_voix_disto(ch_hw_voice[ch], MD_DISTO(&ch_instr_shadow[ch]));
}

// ── Les macros FM d'AT2 ─────────────────────────────────────────────────────
// Reprise du moteur d'Amber OPL3 Tracker (process_macros), lui-meme calque sur
// le macro_poll_proc d'AdLib Tracker 2. Les macros vivent a cote du module :
// elles ne sont pas (encore) enregistrees dans les .XT.
static md_macro_fm_t *md_macros[MD_MAX_INSTRUMENTS];
static uint32_t md_macros_off[MD_MAX_INSTRUMENTS];

typedef struct {
  uint8_t ins;        // instrument 1-255, 0 = aucune macro en cours
  uint8_t pos;        // pas en cours, 1-255 ; 0 = pas encore parti / fini
  uint8_t duree;      // ticks restants sur le pas
} md_macro_etat_t;
static md_macro_etat_t md_macro_etat[MD_TOTAL_VOICES];

void md_replayer_set_instr_macro(int ins_idx, const md_macro_fm_t *macro,
                                 uint32_t desactive) {
  if (ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return;
  md_macro_fm_t *m = md_macros[ins_idx - 1];
  if (!macro || macro->length == 0) {
    free(m);
    md_macros[ins_idx - 1] = NULL;
    return;
  }
  if (!m) m = (md_macro_fm_t *)malloc(sizeof(md_macro_fm_t));
  if (!m) return;
  *m = *macro;
  md_macros[ins_idx - 1] = m;
  md_macros_off[ins_idx - 1] = desactive;
}

bool md_replayer_get_instr_macro(int ins_idx, md_macro_fm_t *out, uint32_t *desactive) {
  if (ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS || !md_macros[ins_idx - 1]) return false;
  if (out) *out = *md_macros[ins_idx - 1];
  if (desactive) *desactive = md_macros_off[ins_idx - 1];
  return true;
}

bool md_replayer_instr_a_macro(int ins_idx) {
  return ins_idx >= 1 && ins_idx <= MD_MAX_INSTRUMENTS && md_macros[ins_idx - 1];
}

static void md_macros_vide(void) {
  for (int i = 0; i < MD_MAX_INSTRUMENTS; i++) {
    free(md_macros[i]);
    md_macros[i] = NULL;
  }
  memset(md_macro_etat, 0, sizeof(md_macro_etat));
}

static uint16_t md_slide_frequency(uint16_t hauteur, int16_t slide_amount);

static void md_opl_macro_tick(int ch) {
  md_macro_etat_t *e = &md_macro_etat[ch];
  if (!e->ins || ch_instr_shadow[ch].kind != MD_INSTR_KIND_OPL) return;
  const md_macro_fm_t *mac = md_macros[e->ins - 1];
  if (!mac || mac->length == 0) return;

  if (e->duree > 1) { e->duree--; return; }   // on reste sur ce pas

  // Le pas suivant : AT2 compte a partir de 1, 0 = fini.
  if (e->pos <= mac->length) {
    if (mac->loop_begin && mac->loop_length) {
      if (e->pos == mac->loop_begin + mac->loop_length - 1) e->pos = mac->loop_begin;
      else if (e->pos < mac->length) e->pos++;
      else e->pos = 0;
    } else {
      e->pos = e->pos < mac->length ? e->pos + 1 : 0;
    }
  }
  // KEY-OFF : tant que la note tient, la macro s'arrete avant ce pas ; a la
  // relache, elle y saute.
  const bool tenue = (ch_b0_reg[ch] & 0x20) != 0;
  if (tenue && mac->keyoff_pos && e->pos >= mac->keyoff_pos) return;
  if (!tenue && mac->keyoff_pos && e->pos && e->pos < mac->keyoff_pos)
    e->pos = mac->keyoff_pos;
  if (e->pos == 0 || e->pos > mac->length) return;

  const md_macro_pas_t *pas = &mac->data[e->pos - 1];
  e->duree = pas->duration;
  if (e->duree == 0) return;

  // Chaque colonne de la macro peut etre desactivee (un bit chacune, dans
  // l'ordre d'AT2) : on ne recopie que celles qui sont actives.
  const uint32_t off = md_macros_off[e->ins - 1];
  uint8_t *p = MD_OPL_FM(&ch_instr_shadow[ch]);
  const uint8_t *s = pas->fm;
#ifdef FM1
  // (bit par bit, dans un tableau de mots : le test du bit 7 ou 15 du masque
  // devenait un sextra, que l'emulateur en ligne ne lit pas)
  uint32_t coupe[28];
  for (int b = 0; b < 28; b++) coupe[b] = (off >> b) & 1u;
  #define OFF(bit) coupe[bit]
#else
  #define OFF(bit) (off & (1u << (bit)))
#endif
  #define COL(bit, octet, masque) \
    if (!OFF(bit)) p[octet] = (uint8_t)((p[octet] & ~(masque)) | (s[octet] & (masque)))
  COL(0,  MD_OPL_AD_M, 0xF0);   COL(1,  MD_OPL_AD_M, 0x0F);
  COL(2,  MD_OPL_SR_M, 0xF0);   COL(3,  MD_OPL_SR_M, 0x0F);
  COL(4,  MD_OPL_WAVE_M, 0x07); COL(6,  MD_OPL_VOL_M, 0xC0);
  COL(7,  MD_OPL_FLAGS_M, 0x0F); COL(8, MD_OPL_FLAGS_M, 0x80);
  COL(9,  MD_OPL_FLAGS_M, 0x40); COL(10, MD_OPL_FLAGS_M, 0x10);
  COL(11, MD_OPL_FLAGS_M, 0x20);
  COL(12, MD_OPL_AD_C, 0xF0);   COL(13, MD_OPL_AD_C, 0x0F);
  COL(14, MD_OPL_SR_C, 0xF0);   COL(15, MD_OPL_SR_C, 0x0F);
  COL(16, MD_OPL_WAVE_C, 0x07); COL(18, MD_OPL_VOL_C, 0xC0);
  COL(19, MD_OPL_FLAGS_C, 0x0F); COL(20, MD_OPL_FLAGS_C, 0x80);
  COL(21, MD_OPL_FLAGS_C, 0x40); COL(22, MD_OPL_FLAGS_C, 0x10);
  COL(23, MD_OPL_FLAGS_C, 0x20);
  COL(24, MD_OPL_FB_CONN, 0x01); COL(25, MD_OPL_FB_CONN, 0x0E);
  #undef COL
  // Les volumes passent par le moteur, qui y ajoute le volume general.
  if (!OFF(5))  ch_fmpar[ch].volM = s[MD_OPL_VOL_M] & 0x3F;
  if (!OFF(17)) ch_fmpar[ch].volC = s[MD_OPL_VOL_C] & 0x3F;
  ch_fmpar[ch].feedbackConn = p[MD_OPL_FB_CONN];
  if (!OFF(27))
    ch_panning[ch] = pas->panning == 1 ? 0 : (pas->panning == 2 ? 255 : 128);

  opl_voix_instrument(ch_hw_voice[ch], p, ch_panning[ch]);
  md_write_volume(ch);
  if (!OFF(26) && pas->freq_slide) {
    ch_base_fnum[ch] = md_slide_frequency(ch_base_fnum[ch], pas->freq_slide);
    md_write_freq(ch, ch_base_fnum[ch]);
  }
}
#undef OFF

static void md_key_on(int ch) {
  if (ch < 0 || ch >= MD_TOTAL_VOICES)
    return;
  md_table_start(ch);
  ch_b0_reg[ch] |= 0x20;
  if (!current_module || ch_current_instr[ch] < 1 ||
      ch_current_instr[ch] > MD_MAX_INSTRUMENTS)
    return;
  if (md_channel_is_opl(ch)) {
    md_midi_note_off(ch);
    if (md_channel_muted[ch]) { ds_voix_stop(ch_hw_voice[ch]); return; }
    opl_voix_instrument(ch_hw_voice[ch], MD_OPL_FM(&ch_instr_shadow[ch]),
                        ch_panning[ch]);
    md_write_volume(ch);
    opl_voix_hauteur(ch_hw_voice[ch], ch_cur_fnum[ch], true);
    // La macro de l'instrument repart de son premier pas, au tick suivant.
    md_macro_etat[ch].ins = (uint8_t)ch_current_instr[ch];
    md_macro_etat[ch].pos = 0;
    md_macro_etat[ch].duree = 0;
  } else if (md_channel_is_pcm(ch)) {
    md_midi_note_off(ch);          // l'instrument d'avant était peut-être MIDI
    md_voix_demarre(ch);
  } else {
    ds_voix_stop(ch_hw_voice[ch]);  // et celui d'avant, un échantillon
    md_midi_note_on(ch);
  }
}

// ── Voies DESACTIVEES ──────────────────────────────────────────────────────
// A ne pas confondre avec le silence (md_replayer_mute_channel), qui se
// contente de mettre le volume a zero : la voie continue alors d'etre jouee et
// calculee, donc elle coute autant de processeur qu'avant.
//
// Une voie desactivee, elle, ne recoit plus aucune note. Elle n'est donc plus
// active dans le YM2612, et ymfm cesse completement de la calculer — c'est la
// que se trouve l'economie : une voie FM, ce sont quatre operateurs sur les
// vingt-quatre, soit un sixieme du gros du travail.
//
// Les donnees du morceau ne sont JAMAIS touchees : on refuse de les jouer, on
// ne les efface pas. Le meme fichier rouvert sur iPad sonne complet.
bool md_channel_disabled[MD_TOTAL_VOICES] = {false};

// Le note-off coupe net, échantillon comme note MIDI : c'est la table de
// l'instrument qui sert à faire mieux, comme dans LSDJ.
static void md_key_off(int ch) {
  if (ch < 0 || ch >= MD_TOTAL_VOICES)
    return;
  ch_table_running[ch] = false;
  ch_b0_reg[ch] &= ~0x20;
  ds_voix_stop(ch_hw_voice[ch]);
  md_midi_note_off(ch);
}

// Fait avancer les enveloppes logicielles des voies PSG (appelé à chaque tick).
// Définies plus bas.
static void md_write_note_offset(int c, int semitones);
static void md_apply_mdcmd(int c, uint8_t cmd, uint8_t val,
                           uint8_t row_cmd, int slot);

// Canal du morceau qui pilote cette voie PSG (-1 si aucune).
static int md_psg_channel(int p) {
  for (int c = 0; c < MD_TOTAL_VOICES; c++)
    if (md_hw_is_psg(ch_hw_voice[c]) && md_hw_psg_index(ch_hw_voice[c]) == p)
      return c;
  return -1;
}

static void md_psg_env_tick(void) {
  for (int p = 0; p < MD_NUM_PSG_CHANNELS; p++) {
    md_psg_env_t *e = &psg_env[p];
    if (!e->active)
      continue;
    const md_instr_t *ins = md_psg_env_instr(e);
    if (!ins) {
      e->level256 = 15 * 256;
      continue;
    }

    // ── Macro d'arpège (DefleMask) ────────────────────────────────────────
    // Un demi-ton par tick. La colonne TSP d'une table s'AJOUTE par-dessus :
    // la macro donne le dessin de l'instrument, la table le retouche.
    if (ins->psg_arp_len > 0) {
      int c = md_psg_channel(p);
      if (c >= 0) {
        int v = ins->psg_arp_mac[e->arp_pos];
        // « Fixed Arpeggio » : la valeur est une note ABSOLUE, pas un écart.
        int off = ins->psg_arp_fixed ? (v + 1 - (int)ch_current_note[c]) : v;
        md_write_note_offset(c, off);   // le TSP est ajouté par la fonction
      }
      e->arp_pos = (uint8_t)md_macro_next(e->arp_pos, ins->psg_arp_len,
                                          ins->psg_arp_loop);
    }

    // ── Macro de volume (DefleMask) ───────────────────────────────────────
    // Un niveau 0-15 par tick, écrit tel quel. Quand elle existe, elle
    // REMPLACE l'enveloppe à trois points : les deux décriraient le même
    // niveau et se contrediraient.
    if (ins->psg_vol_len > 0) {
      uint8_t lvl = ins->psg_vol_mac[e->vol_pos];
      if (lvl > 15) lvl = 15;
      e->level256 = (int32_t)lvl * 256;
      e->vol_pos = (uint8_t)md_macro_next(e->vol_pos, ins->psg_vol_len,
                                          ins->psg_vol_loop);
      // Un niveau nul en fin de macro éteint la voie, comme pour l'enveloppe.
      if (e->level256 == 0 && ins->psg_vol_loop == MD_EMPTY &&
          e->vol_pos == ins->psg_vol_len - 1)
        e->active = 0;
      continue;
    }

    // ── Enveloppe : trois points de rupture reliés par deux rampes ─────────
    // La vitesse d'un point dit à quelle allure rejoindre l'amplitude du point
    // SUIVANT. Vitesse 0, ou absence de point suivant : on tient l'amplitude
    // courante.
    int cur = e->point;
    if (cur >= MD_ENV_POINTS)
      cur = MD_ENV_POINTS - 1;
    int32_t rate = md_env_rate(ins->env_speed[cur]);
    int next = cur + 1;
    bool has_next = (next < MD_ENV_POINTS) && (ins->env_amp[next] != MD_EMPTY);

    // Cible de la rampe : l'amplitude du point suivant, ou le SILENCE quand il
    // n'y a pas de point suivant. C'est ce qui donne son sens à la vitesse du
    // dernier point : 0 tient l'amplitude indéfiniment (le « infinite sustain »
    // du manuel), toute autre valeur fait retomber la note vers zéro, d'autant
    // plus lentement que le chiffre est grand.
    int32_t target = 0;
    if (has_next) {
      uint8_t a = ins->env_amp[next];
      target = (int32_t)(a > 15 ? 15 : a) * 256;
    }

    if (rate > 0) {
      if (e->level256 < target) {
        e->level256 += rate;
        if (e->level256 > target) e->level256 = target;
      } else if (e->level256 > target) {
        e->level256 -= rate;
        if (e->level256 < target) e->level256 = target;
      }
      if (e->level256 == target && has_next)
        e->point = (uint8_t)next;      // point atteint : on passe au suivant
    }

    // Amplitude nulle sans espoir de remonter = la note s'est éteinte d'elle-
    // même. C'est ce qui dispense d'écrire des note-off partout.
    if (e->level256 <= 0 && !(rate > 0 && target > 0)) {
      e->level256 = 0;
      e->active = 0;
    }

    // ── Macro de mode de bruit ────────────────────────────────────────────
    if (p == 3 && ins->psg_noise_len > 0) {
      int nlen = ins->psg_noise_len > MD_PSG_ENV_MAX ? MD_PSG_ENV_MAX
                                                     : ins->psg_noise_len;
      e->nz_pos = (uint8_t)md_macro_next(e->nz_pos, nlen, ins->psg_noise_loop);
      uint8_t m = (uint8_t)(ins->psg_noise_mac[e->nz_pos] & 0x07);
      // La macro continue d'avancer, mais ne pose rien tant qu'une commande
      // tient le mode : sinon elle le reprendrait à chaque tick.
      if (m != e->noise_mode && !md_noise_forced(md_psg_channel(p))) {
        e->noise_mode = m;
        md_chip_psg_set_noise(m);
      }
    }
  }
  for (int ch = MD_PSG_FIRST_CHANNEL; ch < MD_MAX_CHANNELS; ch++)
    md_write_volume(ch);
  if (md_hw_is_psg(ch_hw_voice[MD_TEST_CHANNEL]))
    md_write_volume(MD_TEST_CHANNEL);
}

// ── Glissandos ──────────────────────────────────────────────────────────────
// On glisse sur la hauteur linéaire : chaque pas de commande vaut le même
// intervalle, en haut comme en bas de l'octave.
// ── Glisser comme AdLib Tracker 2 : sur le F-NUM ───────────────────────────
// Reprise de opl3_slide_frequency d'Amber OPL3 Tracker. La vitesse s'ajoute au
// F-Num ; au-dela de 0x2AE on monte d'un bloc (F-Num - 0x158), en dessous de
// 0x156 on descend (F-Num + 0x158), avec les memes butees qu'AT2.
static uint16_t md_slide_frequency(uint16_t fnum_bloc, int16_t pas) {
  int32_t fnum = fnum_bloc & 0x03FF;
  int32_t bloc = (fnum_bloc >> 10) & 7;
  fnum += pas;
  while (fnum >= 0x2AE && bloc < 7) { bloc++; fnum -= (0x2AE - 0x156); }
  while (fnum <= 0x156 && bloc > 0) { bloc--; fnum += (0x2AE - 0x156); }
  if (fnum > 0x2AE) fnum = 0x2AE;
  if (fnum < 0x156) fnum = 0x156;
  return (uint16_t)((bloc << 10) | (fnum & 0x03FF));
}

// La voie d'audition joue sur la voie matérielle de la piste où est le curseur :
// c'est la seule façon d'entendre ce que cette piste jouera.
static void md_assign_test_voice(void) {
  ch_hw_voice[MD_TEST_CHANNEL] = (uint8_t)(audition_channel % MD_MAX_CHANNELS);
}

// La position stéréo part avec le volume, dans le même ordre à l'ARM7.
static void md_apply_panning(int ch) {
  if (ch < 0 || ch >= MD_TOTAL_VOICES)
    return;
  md_write_volume(ch);
}

// Plus de registres à écrire : un instrument, c'est une position stéréo et un
// volume, le reste se décide au départ de la note.
static void md_apply_instrument(uint8_t ch) {
  if (ch >= MD_TOTAL_VOICES)
    return;
  md_apply_panning(ch);
}

// Charge un instrument sur un canal (copie dans le shadow puis écriture puce).
static void md_set_instrument(uint8_t ch, const md_instr_t *ins) {
  if (ch >= MD_TOTAL_VOICES || !ins)
    return;
  ch_instr_shadow[ch] = *ins;
  ch_panning[ch] = ins->panning;
  if (ins->kind == MD_INSTR_KIND_OPL) {
    // Comme AT2 : poser un instrument remet les volumes a ceux de l'instrument.
    const uint8_t *fm = MD_OPL_FM(ins);
    ch_fmpar[ch].volC = fm[MD_OPL_VOL_C] & 0x3F;
    ch_fmpar[ch].volM = fm[MD_OPL_VOL_M] & 0x3F;
    ch_fmpar[ch].feedbackConn = fm[MD_OPL_FB_CONN];
  }
  md_apply_instrument(ch);
}

// L'instrument chargé sur ce canal joue-t-il un échantillon ? Tout ce qui n'est
// pas MIDI en est un : un instrument neuf joue la banque.
static bool md_channel_is_pcm(int c) {
  if (!current_module || c < 0 || c >= MD_TOTAL_VOICES) return true;
  return ch_instr_shadow[c].kind != MD_INSTR_KIND_MIDI &&
         ch_instr_shadow[c].kind != MD_INSTR_KIND_OPL;
}

// L'instrument charge sur ce canal est-il un instrument FM de l'OPL3 ?
static bool md_channel_is_opl(int c) {
  if (!current_module || c < 0 || c >= MD_TOTAL_VOICES) return false;
  return ch_instr_shadow[c].kind == MD_INSTR_KIND_OPL;
}

static void md_play_note(uint8_t ch, uint8_t note) {
  if (ch >= MD_TOTAL_VOICES || note == 0 || note > MD_MAX_NOTE)
    return;
  if (md_channel_disabled[ch])
    return;   // desactivee : aucune note

  ch_current_note[ch] = note;

  // Coupe la note précédente pour repartir proprement. Échantillon et note
  // MIDI suivent le même chemin : la hauteur de référence est calculée comme
  // pour la FM de GeneTrackerDS, puis le départ de note choisit la sortie.
  md_key_off(ch);

  // La transposition de la table entre dans la hauteur de RÉFÉRENCE, pas
  // seulement dans l'écriture du moment : c'est elle que le moteur restaure au
  // début de chaque ligne, et sans ça le TSP disparaissait dès qu'une commande
  // était posée sur la même ligne.
  {
    int tn = (int)note + (int)ch_table_tsp[ch];
    if (tn < 1) tn = 1;
    if (tn > MD_MAX_NOTE) tn = MD_MAX_NOTE;
    note = (uint8_t)tn;
  }

  ch_base_fnum[ch] = md_hauteur(note, md_ftune_de(ch));

  md_write_freq(ch, ch_base_fnum[ch]);
  ch_note_emise[ch] = note;
  md_key_on(ch);
}

// ── Position de lecture, INDÉPENDANTE PAR CANAL (modèle LSDJ) ─────────────
// Chaque canal descend sa propre colonne du SONG, entre dans son CHAIN, puis
// dans sa PHRASE. Deux canaux ne sont donc pas forcément sur la même ligne de
// song : une basse peut boucler sur un seul chain pendant qu'une lead en
// enchaîne dix.
static int ch_song_row[MD_MAX_CHANNELS];    // ligne du SONG
static int ch_chain[MD_MAX_CHANNELS];       // chain en cours, -1 = canal muet
static int ch_chain_row[MD_MAX_CHANNELS];   // ligne dans le chain
static int ch_phrase[MD_MAX_CHANNELS];      // phrase en cours, -1 = aucune
static int ch_phrase_row[MD_MAX_CHANNELS];  // ligne dans la phrase
static int8_t ch_row_transpose[MD_MAX_CHANNELS];
static bool ch_running[MD_MAX_CHANNELS];    // ce canal a-t-il encore à jouer ?

// Ligne du SONG où démarre la lecture (F5 = 0, F8 = ligne du curseur).
static int song_start_row = 0;
// Ligne de départ dans l'écran CHAIN : « START on the CHAIN screen begins at
// the row the cursor is on (not row 0) » (SamplePanel.swift, toggleTransport).
static int scope_start_row = 0;
// Haut du bloc contigu sur lequel chaque canal boucle.
static int ch_block_top[MD_MAX_CHANNELS] = {0};

// Portée de la lecture : chanson entière, un chain, ou une seule phrase.
// Voir md_replayer_set_play_scope() dans l'en-tête.
static md_play_scope_t play_scope = MD_SCOPE_SONG;
static int scope_channel = 0;
static int scope_id = 0;
// PHRASE : la ligne de CHAIN d'où l'on est descendu (sa transposition).
static int scope_tsp_chain = -1, scope_tsp_row = 0;

// ── Mode LIVE ───────────────────────────────────────────────────────────────
// En mode SONG, la lecture lance les 10 colonnes ensemble. En mode LIVE, elles
// restent muettes jusqu'à ce qu'on les arme une par une ; l'armement prend effet
// à la ligne suivante, ce qui garde tout le monde en phase.
static bool live_mode = false;
static int ch_pending_row[MD_MAX_CHANNELS];   // -1 = rien en attente

uint8_t md_current_row = 0;   // conservé : l'UI hérité l'utilise encore
bool md_loop_pattern = false;

static inline uint16_t read16le(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline void write16le(uint8_t *p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
}

static inline void write32le(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

// ============================================================================
// Format de fichier .mdm (GeneTracker Module) — version 2
//
// Format maison, non compressé. La version 2 abandonne les patterns au profit
// de la structure LSDJ : SONG (256 lignes × 10 canaux de numéros de chain),
// CHAIN (16 lignes : phrase + transposition), PHRASE (16 lignes : note,
// instrument, vélocité, commande, valeur).
// Les champs sont écrits un par un (pas de memcpy de struct) pour rester
// indépendant de l'alignement du compilateur.
//
//   0   char   magic[8]      "MDMTRACK"
//   8   u16    version       (2)
//  10   u8     num_channels
//  11   u8     initial_tempo
//  12   u8     initial_speed
//  13   u8     common_flag
//  14   u16    macro_speedup
//  16   i16    bpm_tempo_finetune
//  18   u8     bpm_rows_per_beat
//  19   u8     réservé
//  20   char   songname[43]
//  63   char   composer[43]
// 106   song      : 10 canaux × 256 lignes × 1 octet   (numéro de chain)
// 2666  chains    : 128 × 16 lignes × 2 octets         (phrase, transpose)
// 6762  phrases   : 255 × 16 lignes × 5 octets         (note, ins, vel, cmd, val)
// 27162 instruments : 255 × 64 octets
// 43482 noms d'instruments : 255 × 43 octets
// ============================================================================

// v7 : seconde colonne de commande (MD CMD) dans les phrases et les tables.
// v6 : l'enveloppe PSG devient un ADSR à six paramètres, comme l'écran ENV de
//      LSDJ, à la place de la table de volumes.
// v5 : tables d'instrument façon LSDJ (32 tables × 16 lignes) ; l'arpégiateur
//      cède sa place au numéro de table dans l'instrument.
// v4 : macros d'arpège et de mode de bruit (256 octets/instrument).
// v3 : table de volumes PSG (128 octets). v2 : enveloppe dérivée d'OP4 (64).
// v12 : volume propre à l'instrument PCM. Les fichiers antérieurs le prennent
//       au maximum, donc rien ne change pour eux.
// v11 : la vélocité va de 0 à 7F, l'échelle de DefleMask, au lieu de 0 à 3F.
//       Le moteur écrêtait à 3F, si bien qu'un volume DefleMask supérieur à la
//       moitié sonnait au maximum et que tout le reste sonnait deux fois trop
//       bas. Les fichiers antérieurs sont remis à l'échelle en les chargeant.
// v10 : banque d'échantillons pour la voie PCM (le DAC du YM2612). Le fichier
//       n'est plus de taille fixe : une section variable est ajoutée à la fin.
// v9 : macros PSG façon DefleMask (volume et arpège, un pas par tick) ;
//      l'enregistrement d'instrument passe de 256 à 512 octets.
// v8 : la colonne MD CMD porte tout le jeu d'effets DefleMask (44 commandes)
//      et le fichier range désormais le CODE de l'effet, pas son rang dans la
//      table — réordonner la table ne change plus le sens d'un morceau.
// v13 : le tracker XM MIDI. Nouvelle signature « XMMTRACK » : 16 pistes, la
//       colonne MD CMD porte un CC MIDI brut, et l'instrument gagne son canal
//       et son programme MIDI. Les fichiers de GeneTrackerDS ne s'ouvrent pas.
// v14 : les échantillons gardent leur forme d'origine — 8 ou 16 bits, à leur
//       cadence — et leur en-tête passe à 44 octets. Longueur et boucle se
//       comptent en images. Les v13 se lisent comme du 8 bits à 32 kHz.
// v15 : l'instrument MIDI gagne la durée de ses notes (LENGTH).
// v21 : ce bloc porte une fiche PAR CANAL MIDI — seize fois le logiciel, son
//       projet et sa table de CC. Un morceau peut donc viser plusieurs
//       applications à la fois.
// v20 : ce bloc porte TROIS noms — le logiciel, son projet, et la table de CC.
// v19 : un bloc APRÈS la banque note à quoi le MIDI est branché — le logiciel
//       et son projet. Il suit les données, donc aucun décalage ne bouge.
// v18 : l'instrument KICK et ses cinq réglages de synthèse.
// v17 : la stéréo d'un instrument tient sur 0-255 (128 = centre), comme dans
//       un module XM, au lieu des trois positions L C R.
// v16 : quatre colonnes CC par ligne de phrase au lieu d'une.
#define MD_FILE_VERSION 21
// Le bloc MIDI qui suit la banque : sa signature, puis une fiche par canal.
// Les deux signatures précédentes restent lisibles : « XMMIDIN2 » portait
// trois noms pour tout le morceau, « XMMIDINF » n'en portait que deux.
#define MD_MIDINFO_BYTES (8 + MD_MIDI_CANAUX * 48)
#define MD_MIDINFO_BYTES_V20 56
#define MD_MIDINFO_BYTES_V19 40
// v9 : l'enregistrement passe à 512 octets pour loger les macros PSG.
#define MD_INSTR_RECORD_SIZE 512
#define MD_TABLE_ROW_BYTES 8
#define MD_TABLE_ROW_BYTES_V6 6
#define MD_TABLES_BYTES (MD_MAX_TABLES * MD_TABLE_ROWS * MD_TABLE_ROW_BYTES)

// Taille d'enregistrement d'une version donnée, 0 si la version est inconnue.
// ── La colonne CC dans le fichier ───────────────────────────────────────────
// Le numéro de contrôleur est rangé tel quel.
static uint8_t md_mdcmd_to_file(uint8_t idx) { return idx; }

static uint8_t md_mdcmd_from_file(uint8_t stored, uint16_t version) {
  (void)version;
  return (stored > 127) ? MD_EMPTY : stored;
}

static uint32_t md_instr_record_size(uint16_t version) {
  // Depuis la v9 l'enregistrement fait 512 octets et n'a plus changé de
  // taille : les ajouts se logent dans la place restante. On répond donc pour
  // TOUTE version récente — sans ça, chaque changement de version rendait les
  // fichiers illisibles, ce qui est exactement ce qui venait d'arriver.
  if (version >= 9) return 512;
  switch (version) {
  case 2: return 64;
  case 3: return 128;
  case 4: return 256;
  case 5: return 256;
  case 6: return 256;
  case 7: return 256;
  case 8: return 256;
  case 9: return 512;
  case 10: return 512;
  default: return 0;
  }
}
#define MD_SONG_OFFSET 106
#define MD_SONG_BYTES (MD_MAX_CHANNELS * MD_SONG_ROWS)
#define MD_CHAIN_OFFSET (MD_SONG_OFFSET + MD_SONG_BYTES)
#define MD_CHAIN_BYTES (MD_MAX_CHAINS * MD_ROWS_PER_CHAIN * 2)
#define MD_PHRASE_OFFSET (MD_CHAIN_OFFSET + MD_CHAIN_BYTES)
// v16 : 13 octets, avec les colonnes CC 2 à 4.
#define MD_PHRASE_ROW_BYTES 13
#define MD_PHRASE_ROW_BYTES_V15 7
#define MD_PHRASE_ROW_BYTES_V6 5
#define MD_PHRASE_BYTES (MD_MAX_PHRASES * MD_ROWS_PER_PHRASE * MD_PHRASE_ROW_BYTES)
#define MD_INSTR_TABLE_OFFSET (MD_PHRASE_OFFSET + MD_PHRASE_BYTES)
#define MD_INSTR_TABLE_SIZE (MD_MAX_INSTRUMENTS * MD_INSTR_RECORD_SIZE)
#define MD_NAMES_TABLE_OFFSET (MD_INSTR_TABLE_OFFSET + MD_INSTR_TABLE_SIZE)
#define MD_NAMES_TABLE_SIZE (MD_MAX_INSTRUMENTS * 43)
#define MD_TABLES_OFFSET (MD_NAMES_TABLE_OFFSET + MD_NAMES_TABLE_SIZE)
#define MD_FILE_SIZE (MD_TABLES_OFFSET + MD_TABLES_BYTES)
// ── Section des échantillons (v10) ──────────────────────────────────────────
// Ajoutée APRÈS le bloc de taille fixe : un en-tête par emplacement, puis les
// données brutes bout à bout. C'est ce qui rend le fichier de taille variable.
// v14 : 44 octets, avec la résolution (octet 37) et la cadence (octets 40-43).
#define MD_SAMPLE_HDR_BYTES 44
#define MD_SAMPLE_HDR_BYTES_V13 40
#define MD_SAMPLES_HDR_TOTAL (MD_MAX_SAMPLES * MD_SAMPLE_HDR_BYTES)

static void md_write_instrument(uint8_t *p, const md_instr_t *ins) {
  memset(p, 0, MD_INSTR_RECORD_SIZE);
  int o = 0;
  for (int i = 0; i < 4; i++) {
    const md_op_params_t *op = &ins->op[i];
    p[o++] = op->detune;
    p[o++] = op->multiple;
    p[o++] = op->total_level;
    p[o++] = op->key_scale;
    p[o++] = op->attack;
    p[o++] = op->decay;
    p[o++] = op->sustain_rate;
    p[o++] = op->sustain_level;
    p[o++] = op->release;
    p[o++] = op->am_enable;
    p[o++] = op->ssg_eg;
  }
  p[o++] = ins->algorithm;
  p[o++] = ins->feedback;
  p[o++] = ins->ams;
  p[o++] = ins->pms;
  p[o++] = ins->lfo_enable;
  p[o++] = ins->lfo_freq;
  p[o++] = ins->panning;
  p[o++] = (uint8_t)ins->fine_tune;
  p[o++] = ins->psg_noise;
  for (int i = 0; i < MD_ENV_POINTS; i++) {
    p[o++] = ins->env_amp[i];
    p[o++] = ins->env_speed[i];
  }
  o += 29; // ancien emplacement de la table de volumes
  p[o++] = ins->table;
  memcpy(p + o, ins->table_pad, 33);   // l'instrument OPL (MD_OPL_FM)
  o += 33;
  p[o++] = ins->psg_noise_len;
  p[o++] = ins->psg_noise_loop;
  for (int i = 0; i < MD_PSG_ENV_MAX; i++)
    p[o++] = ins->psg_noise_mac[i];
  // ── Macros PSG (v9) ────────────────────────────────────────────────────
  p[o++] = ins->psg_vol_len;
  p[o++] = ins->psg_vol_loop;
  for (int i = 0; i < MD_PSG_MACRO_MAX; i++)
    p[o++] = ins->psg_vol_mac[i];
  p[o++] = ins->psg_arp_len;
  p[o++] = ins->psg_arp_loop;
  p[o++] = ins->psg_arp_fixed;
  for (int i = 0; i < MD_PSG_MACRO_MAX; i++)
    p[o++] = (uint8_t)ins->psg_arp_mac[i];
  p[o++] = ins->kind;
  p[o++] = ins->pcm_sample;
  p[o++] = ins->pcm_volume;
  // ── Instrument MIDI (v13) ──────────────────────────────────────────────
  p[o++] = ins->midi_channel;
  p[o++] = ins->midi_program;
  p[o++] = ins->midi_length;   // v15
  // ── Instrument KICK (v18) ──────────────────────────────────────────────
  p[o++] = ins->kick_drop;
  p[o++] = ins->kick_time;
  p[o++] = ins->kick_decay;
  p[o++] = ins->kick_click;
  p[o++] = ins->kick_drive;
}

static void md_read_instrument(const uint8_t *p, md_instr_t *ins, uint16_t version) {
  memset(ins, 0, sizeof(*ins));
  int o = 0;
  for (int i = 0; i < 4; i++) {
    md_op_params_t *op = &ins->op[i];
    op->detune = p[o++];
    op->multiple = p[o++];
    op->total_level = p[o++];
    op->key_scale = p[o++];
    op->attack = p[o++];
    op->decay = p[o++];
    op->sustain_rate = p[o++];
    op->sustain_level = p[o++];
    op->release = p[o++];
    op->am_enable = p[o++];
    op->ssg_eg = p[o++];
  }
  ins->algorithm = p[o++];
  ins->feedback = p[o++];
  ins->ams = p[o++];
  ins->pms = p[o++];
  ins->lfo_enable = p[o++];
  ins->lfo_freq = p[o++];
  ins->panning = p[o++];
  // Avant la v17, la stéréo ne valait que 0 centre, 1 gauche, 2 droite.
  if (version < 17)
    ins->panning = (ins->panning == 1) ? 0 : (ins->panning == 2) ? 255 : 128;
  ins->fine_tune = (int8_t)p[o++];
  ins->psg_noise = p[o++];

  // Ce qui n'existe pas dans une ancienne version reste neutre.
  ins->table = MD_EMPTY;
  ins->psg_noise_loop = MD_EMPTY;

  // v2 : l'enveloppe PSG était dérivée d'OP4. On ne tente pas de la
  // reconstituer — on repart d'une table neutre (volume tenu).
  if (version < 3)
    return;

  // v3-v5 rangeaient ici une table de volumes ; v6 y range l'ADSR. On ne relit
  // les six paramètres que si le fichier est bien en v6 — sinon on laisse
  // l'enveloppe par défaut plutôt que d'interpréter d'anciens octets à tort.
  if (version >= 6) {
    for (int i = 0; i < MD_ENV_POINTS; i++) {
      ins->env_amp[i] = p[o + i * 2];
      ins->env_speed[i] = p[o + i * 2 + 1];
    }
  }
  o += 35;

  if (version < 4)
    return;

  // v4 rangeait ici la macro d'arpège ; v5 y range le numéro de table. Une v4
  // relue donne donc une longueur d'arpège comme numéro de table — on ne la
  // prend que si le fichier est bien en v5.
  if (version >= 5) {
    ins->table = p[o];
    memcpy(ins->table_pad, p + o + 1, 33);   // l'instrument OPL (MD_OPL_FM)
  }
  o += 34;
  ins->psg_noise_len = p[o++];
  ins->psg_noise_loop = p[o++];
  for (int i = 0; i < MD_PSG_ENV_MAX; i++)
    ins->psg_noise_mac[i] = p[o++];

  // Les macros PSG n'existent qu'à partir de la v9 ; avant, l'enregistrement
  // s'arrêtait là et les octets suivants n'existent pas.
  if (version < 9)
    return;
  ins->psg_vol_len = p[o++];
  ins->psg_vol_loop = p[o++];
  for (int i = 0; i < MD_PSG_MACRO_MAX; i++)
    ins->psg_vol_mac[i] = p[o++];
  ins->psg_arp_len = p[o++];
  ins->psg_arp_loop = p[o++];
  ins->psg_arp_fixed = p[o++];
  for (int i = 0; i < MD_PSG_MACRO_MAX; i++)
    ins->psg_arp_mac[i] = (int8_t)p[o++];
  // Écrit à zéro par les toutes premières v9 : MD_INSTR_KIND_ANY, l'ancien
  // comportement.
  ins->kind = p[o++];
  // L'échantillon choisi n'existe qu'à partir de la v10 ; avant, l'octet est à
  // zéro, ce qui désigne le premier emplacement — sans conséquence puisqu'il
  // n'y avait pas d'instrument PCM.
  ins->pcm_sample = (version >= 10) ? p[o++] : 0;
  // Avant la v12 il n'y avait pas de volume PCM : plein, comme avant.
  ins->pcm_volume = (version >= 12) ? p[o++] : 127;
  // Le canal et le programme MIDI n'existent qu'à partir de la v13.
  ins->midi_channel = (version >= 13) ? (p[o++] & 0x0F) : 0;
  ins->midi_program = (version >= 13) ? p[o++] : MD_EMPTY;
  // La durée des notes MIDI n'existe qu'à partir de la v15 : AUTO avant.
  ins->midi_length = (version >= 15) ? p[o++] : 0;
  // Les réglages du KICK n'existent qu'à partir de la v18.
  if (version >= 18) {
    ins->kick_drop = p[o++];
    ins->kick_time = p[o++];
    ins->kick_decay = p[o++];
    ins->kick_click = p[o++];
    ins->kick_drive = p[o++];
  } else {
    ins->kick_drop = 96; ins->kick_time = 40; ins->kick_decay = 90;
    ins->kick_click = 40; ins->kick_drive = 60;
  }
  // La seconde zone n'est pas dans le fichier : elle sera réservée au premier
  // réglage, quand on en aura besoin.
  ins->kick_alt = MD_EMPTY;
}

// Sérialise le module courant au format .mdm v2.
// L'appelant doit free() le tampon renvoyé.
uint8_t *md_replayer_save_mem(uint32_t *out_size) {
  if (!current_module || !out_size)
    return NULL;

  md_module_t *mod = current_module;
  // Taille = bloc fixe + en-têtes d'échantillons. Les données de la banque
  // suivent dans le fichier, écrites directement par md_replayer_save_file.
  const uint32_t pcm_bytes = mod->pcm_used;
  const uint32_t total = MD_FILE_SIZE + 4 + MD_SAMPLES_HDR_TOTAL;
  uint8_t *buf = (uint8_t *)calloc(1, total);
  if (!buf)
    return NULL;

  memcpy(buf, "XMMTRACK", 8);
  write16le(buf + 8, MD_FILE_VERSION);
  buf[10] = mod->num_channels;
  buf[11] = mod->initial_tempo;
  buf[12] = mod->initial_speed;
  buf[13] = mod->common_flag;
  write16le(buf + 14, mod->macro_speedup);
  write16le(buf + 16, (uint16_t)mod->bpm_tempo_finetune);
  buf[18] = mod->bpm_rows_per_beat;
  memcpy(buf + 20, mod->songname, 43);
  memcpy(buf + 63, mod->composer, 43);

  for (int c = 0; c < MD_MAX_CHANNELS; c++)
    memcpy(buf + MD_SONG_OFFSET + c * MD_SONG_ROWS, mod->song[c], MD_SONG_ROWS);

  for (int i = 0; i < MD_MAX_CHAINS; i++) {
    for (int r = 0; r < MD_ROWS_PER_CHAIN; r++) {
      uint8_t *d = buf + MD_CHAIN_OFFSET + (i * MD_ROWS_PER_CHAIN + r) * 2;
      d[0] = mod->chains[i].rows[r].phrase;
      d[1] = (uint8_t)mod->chains[i].rows[r].transpose;
    }
  }

  for (int i = 0; i < MD_MAX_PHRASES; i++) {
    for (int r = 0; r < MD_ROWS_PER_PHRASE; r++) {
      uint8_t *d = buf + MD_PHRASE_OFFSET +
                   (i * MD_ROWS_PER_PHRASE + r) * MD_PHRASE_ROW_BYTES;
      const md_phrase_row_t *pr = &mod->phrases[i].rows[r];
      d[0] = pr->note;
      d[1] = pr->instr;
      d[2] = pr->vel;
      d[3] = pr->cmd;
      d[4] = pr->cmdval;
      d[5] = md_mdcmd_to_file(pr->mdcmd);
      d[6] = pr->mdval;
      for (int k = 0; k < 3; k++) {          // CC 2 à 4 (v16)
        d[7 + 2 * k] = pr->cc[k];
        d[8 + 2 * k] = pr->ccval[k];
      }
    }
  }

  for (int i = 0; i < MD_MAX_INSTRUMENTS; i++) {
    md_write_instrument(buf + MD_INSTR_TABLE_OFFSET + i * MD_INSTR_RECORD_SIZE,
                        &mod->instruments[i]);
    memcpy(buf + MD_NAMES_TABLE_OFFSET + i * 43, mod->instr_names[i], 43);
  }

  // Les tables. Elles manquaient purement et simplement du fichier depuis
  // leur introduction : tout ce qu'on y écrivait était perdu à l'ouverture
  // suivante.
  for (int i = 0; i < MD_MAX_TABLES; i++) {
    for (int r = 0; r < MD_TABLE_ROWS; r++) {
      uint8_t *d = buf + MD_TABLES_OFFSET +
                   (i * MD_TABLE_ROWS + r) * MD_TABLE_ROW_BYTES;
      const md_table_row_t *tr = &mod->tables[i].rows[r];
      d[0] = tr->vol;
      d[1] = (uint8_t)tr->transpose;
      d[2] = tr->cmd1;  d[3] = tr->val1;
      d[4] = tr->cmd2;  d[5] = tr->val2;
      d[6] = md_mdcmd_to_file(tr->mdcmd); d[7] = tr->mdval;
    }
  }

  // ── Banque d'échantillons ───────────────────────────────────────────────
  {
    uint8_t *p = buf + MD_FILE_SIZE;
    write32le(p, pcm_bytes); p += 4;
    for (int i = 0; i < MD_MAX_SAMPLES; i++) {
      const md_sample_t *sm = &mod->samples[i];
      memcpy(p, sm->name, 24);
      write32le(p + 24, sm->offset);
      write32le(p + 28, sm->length);
      write32le(p + 32, (uint32_t)sm->loop);
      p[36] = sm->base_note;
      p[37] = sm->bits;
      write32le(p + 40, sm->rate);
      p += MD_SAMPLE_HDR_BYTES;
    }
  }

  *out_size = total;
  return buf;
}

const uint8_t *md_replayer_banque(uint32_t *octets) {
  if (octets) *octets = current_module ? current_module->pcm_used : 0;
  return md_banque;
}

#ifdef FM1
// ── FM-1 : le meme fichier, ecrit EN FLUX ───────────────────────────────────
// md_replayer_save_mem construit tout le bloc fixe (214 Ko) en memoire : le
// FM-1 n'a pas cette place a cote du module. On ecrit donc les memes octets,
// section par section, dans l'ordre du fichier, avec un petit tampon.
static bool md_sauve_flux(FILE *f, const md_module_t *mod, uint32_t pcm_bytes) {
  static uint8_t t[MD_INSTR_RECORD_SIZE];
  bool ok = true;
#define MD_ECRIT(p, n) (ok = ok && fwrite((p), 1, (n), f) == (size_t)(n))
  memset(t, 0, MD_SONG_OFFSET);
  memcpy(t, "XMMTRACK", 8);
  write16le(t + 8, MD_FILE_VERSION);
  t[10] = mod->num_channels;
  t[11] = mod->initial_tempo;
  t[12] = mod->initial_speed;
  t[13] = mod->common_flag;
  write16le(t + 14, mod->macro_speedup);
  write16le(t + 16, (uint16_t)mod->bpm_tempo_finetune);
  t[18] = mod->bpm_rows_per_beat;
  memcpy(t + 20, mod->songname, 43);
  memcpy(t + 63, mod->composer, 43);
  MD_ECRIT(t, MD_SONG_OFFSET);

  for (int c = 0; c < MD_MAX_CHANNELS; c++)
    MD_ECRIT(mod->song[c], MD_SONG_ROWS);

  for (int i = 0; i < MD_MAX_CHAINS; i++) {
    for (int r = 0; r < MD_ROWS_PER_CHAIN; r++) {
      t[r * 2] = mod->chains[i].rows[r].phrase;
      t[r * 2 + 1] = (uint8_t)mod->chains[i].rows[r].transpose;
    }
    MD_ECRIT(t, MD_ROWS_PER_CHAIN * 2);
  }

  for (int i = 0; i < MD_MAX_PHRASES; i++) {
    for (int r = 0; r < MD_ROWS_PER_PHRASE; r++) {
      uint8_t *d = t + r * MD_PHRASE_ROW_BYTES;
      const md_phrase_row_t *pr = &mod->phrases[i].rows[r];
      d[0] = pr->note;
      d[1] = pr->instr;
      d[2] = pr->vel;
      d[3] = pr->cmd;
      d[4] = pr->cmdval;
      d[5] = md_mdcmd_to_file(pr->mdcmd);
      d[6] = pr->mdval;
      for (int k = 0; k < 3; k++) {
        uint8_t *dk = WEB_PTR(d + 7 + 2 * k);
        dk[0] = pr->cc[k];
        dk[1] = pr->ccval[k];
      }
    }
    MD_ECRIT(t, MD_ROWS_PER_PHRASE * MD_PHRASE_ROW_BYTES);
  }

  for (int i = 0; i < MD_MAX_INSTRUMENTS; i++) {
    md_write_instrument(t, &mod->instruments[i]);
    MD_ECRIT(t, MD_INSTR_RECORD_SIZE);
  }
  for (int i = 0; i < MD_MAX_INSTRUMENTS; i++)
    MD_ECRIT(mod->instr_names[i], 43);

  for (int i = 0; i < MD_MAX_TABLES; i++) {
    for (int r = 0; r < MD_TABLE_ROWS; r++) {
      uint8_t *d = t + r * MD_TABLE_ROW_BYTES;
      const md_table_row_t *tr = &mod->tables[i].rows[r];
      d[0] = tr->vol;
      d[1] = (uint8_t)tr->transpose;
      d[2] = tr->cmd1;  d[3] = tr->val1;
      d[4] = tr->cmd2;  d[5] = tr->val2;
      d[6] = md_mdcmd_to_file(tr->mdcmd); d[7] = tr->mdval;
    }
    MD_ECRIT(t, MD_TABLE_ROWS * MD_TABLE_ROW_BYTES);
  }

  write32le(t, pcm_bytes);
  MD_ECRIT(t, 4);
  for (int i = 0; i < MD_MAX_SAMPLES; i++) {
    const md_sample_t *sm = &mod->samples[i];
    memset(t, 0, MD_SAMPLE_HDR_BYTES);
    memcpy(t, sm->name, 24);
    write32le(t + 24, sm->offset);
    write32le(t + 28, sm->length);
    write32le(t + 32, (uint32_t)sm->loop);
    t[36] = sm->base_note;
    t[37] = sm->bits;
    write32le(t + 40, sm->rate);
    MD_ECRIT(t, MD_SAMPLE_HDR_BYTES);
  }
#undef MD_ECRIT
  return ok;
}
#endif

bool md_replayer_save_file(const char *path) {
#ifdef FM1
  if (!current_module)
    return false;
#else
  uint32_t taille = 0;
  uint8_t *entete = md_replayer_save_mem(&taille);
  if (!entete)
    return false;
#endif
  const uint32_t pcm_bytes = current_module->pcm_used;
  // Le bloc « à quoi le MIDI est branché » vient APRÈS la banque : les
  // décalages du fichier ne changent pas, et un lecteur plus ancien l'ignore.
  static uint8_t info[MD_MIDINFO_BYTES];
  memset(info, 0, sizeof(info));
  memcpy(info, "XMMIDIN3", 8);
  for (int c = 0; c < MD_MIDI_CANAUX; c++) {
    uint8_t *p = info + 8 + c * 48;
    memcpy(p, current_module->midi_cibles[c].soft, 16);
    memcpy(p + 16, current_module->midi_cibles[c].proj, 16);
    memcpy(p + 32, current_module->midi_cibles[c].map, 16);
  }
  bool ok = false;
  FILE *f = fopen(path, "wb");
  if (f) {
#ifdef FM1
    ok = md_sauve_flux(f, current_module, pcm_bytes) &&
#else
    ok = fwrite(entete, 1, taille, f) == taille &&
#endif
         (pcm_bytes == 0 || fwrite(md_banque, 1, pcm_bytes, f) == pcm_bytes) &&
         fwrite(info, 1, sizeof(info), f) == sizeof(info);
#ifdef FM1
    { // La DRUM MACHINE, apres le bloc MIDI : un lecteur plus ancien l'ignore.
      static uint8_t d[8 + 32 + 16];             // (statique : sur la pile, ses octets s'ecrivaient en EE53)
      memcpy(d, "OBTDRUM2", 8);
      for (int k = 0; k < 16; k++) { d[8 + 2 * k] = (uint8_t)current_module->drum_pas[k];
                                     d[9 + 2 * k] = (uint8_t)(current_module->drum_pas[k] >> 8);
                                     d[40 + k] = (uint8_t)current_module->drum_trans[k]; }
      ok = ok && fwrite(d, 1, sizeof d, f) == sizeof d;
    }
#endif
    ok = (fclose(f) == 0) && ok;
  }
#ifndef FM1
  free(entete);
#endif
  return ok;
}

void md_replayer_set_midi_cible(int canal, const char *soft, const char *proj,
                                const char *map) {
  if (!current_module || canal < 0 || canal >= MD_MIDI_CANAUX) return;
  md_midi_cible_t *t = &current_module->midi_cibles[canal];
  memset(t, 0, sizeof(*t));
  if (soft) strncpy(t->soft, soft, sizeof(t->soft) - 1);
  if (proj) strncpy(t->proj, proj, sizeof(t->proj) - 1);
  if (map)  strncpy(t->map,  map,  sizeof(t->map) - 1);
}

void md_replayer_get_midi_cible(int canal, char *soft, char *proj, char *map,
                                int cap) {
  if (cap <= 0) return;
  const md_midi_cible_t *t =
      (current_module && canal >= 0 && canal < MD_MIDI_CANAUX)
          ? &current_module->midi_cibles[canal] : NULL;
  if (soft) { soft[0] = 0; if (t) strncpy(soft, t->soft, (size_t)cap - 1);
              soft[cap - 1] = 0; }
  if (proj) { proj[0] = 0; if (t) strncpy(proj, t->proj, (size_t)cap - 1);
              proj[cap - 1] = 0; }
  if (map)  { map[0] = 0;  if (t) strncpy(map,  t->map,  (size_t)cap - 1);
              map[cap - 1] = 0; }
}

bool md_replayer_canal_midi_utilise(int canal) {
  if (!current_module || canal < 0 || canal >= MD_MIDI_CANAUX) return false;
  for (int i = 0; i < MD_MAX_INSTRUMENTS; i++) {
    const md_instr_t *ins = &current_module->instruments[i];
    if (ins->kind == MD_INSTR_KIND_MIDI && (ins->midi_channel & 0x0F) == canal)
      return true;
  }
  return false;
}

// Internal playback state
static int replay_ticks_left = 0;
static int replay_speed = 6;
static int replay_tempo = 50;
static int samples_per_tick = 0;
static int samples_until_next_tick = 0;

// ── Timing AT2 : fréquence de base de l'IRQ + finetune ──────────────────────
// AT2 ne joue pas à `tempo` Hz : il choisit une fréquence d'IRQ de base (à
// partir de 250, multiple de tempo*macro_speedup, plafonnée à 1000), puis
// AJOUTE `bpm_tempo_finetune` à cette fréquence. Le tempo réel est donc mis à
// l'échelle par (base+finetune)/base. On ignorait ce finetune → lecture trop
// lente et BPM faux. (cf. calc_bpm_speed / set_timer dans AT2.)
static int at2_base_irq_freq(int tempo, int macro) {
  if (tempo <= 0) tempo = 50;
  if (macro <= 0) macro = 1;
  int div = tempo * macro;
  if (div <= 0) div = tempo > 0 ? tempo : 50;
  int irq = 250;
  while (irq % div != 0) irq++;
  if (irq > 1000) irq = 1000;
  return irq;
}

// Échelle de tempo due au finetune : (base+finetune)/base. 1.0 si pas de module.
static double md_finetune_scale(void) {
  if (!current_module) return 1.0;
  int tempo = current_module->initial_tempo > 0 ? current_module->initial_tempo : 50;
  int macro = current_module->macro_speedup > 0 ? current_module->macro_speedup : 1;
  int base = at2_base_irq_freq(tempo, macro);
  if (base <= 0) return 1.0;
  double s = ((double)base + (double)current_module->bpm_tempo_finetune) / (double)base;
  if (s < 0.05) s = 0.05;   // garde-fou
  return s;
}

void md_replayer_init(int sample_rate) {
  // L'argument ne sert plus : il n'y a pas d'échantillons à produire, et la
  // cadence du séquenceur se compte sur l'horloge virtuelle.
  (void)sample_rate;
  replay_sample_rate = MD_HORLOGE_VIRTUELLE;
  md_prepare_puiss2();
  md_prepare_sinus();
  ds_voix_init();
  opl_voix_init();

  // Chaque piste est câblée sur la voie matérielle de même numéro.
  for (int c = 0; c < MD_MAX_CHANNELS; c++)
    ch_hw_voice[c] = (uint8_t)c;
  ch_hw_voice[MD_TEST_CHANNEL] = 0;
  memset(psg_env, 0, sizeof(psg_env));
  memset(ch_table_forced, 0, sizeof(ch_table_forced));
  memset(ch_midi_note, 0xFF, sizeof(ch_midi_note));
  memset(midi_programme, 0xFF, sizeof(midi_programme));

  // Le volume global est modifiable par la commande M. Sans cette remise à
  // zéro il survivait à un changement de morceau : une seule commande M
  // suffisait à laisser TOUTES les lectures suivantes assourdies, y compris
  // après chargement d'un autre fichier.
  replay_global_vol = 63;
  memset(ch_fmpar, 0, sizeof(ch_fmpar));

  // Une cadence par défaut, pour que les tables tournent avant la première
  // lecture (audition d'une note) : 50 ticks par seconde.
  samples_per_tick = replay_sample_rate / 50;
}

void md_replayer_shut() {
  md_replayer_stop();
  if (current_module) {
    free(current_module);
    current_module = NULL;
  }
}

// Le morceau courant perd ses échantillons (en-têtes compris) : sa banque n'a
// pas pu être lue.
static void md_banque_vide(void) {
  if (!current_module) return;
  memset(current_module->samples, 0, sizeof(current_module->samples));
  current_module->pcm_used = 0;
}

// ── FM-1 : le morceau se lit EN FLUX ──────────────────────────────────────
// Le FM-1 n'a pas la RAM d'un tampon de 214 Ko a cote du module : quand
// md_flux est pose, md_charge_entete lit ses octets dans ce fichier, par
// fenetres d'un kilo-octet, au lieu de `data`. Ailleurs, MD_SRC(off, n) n'est
// que `data + off`.
#ifdef FM1
static FILE *md_flux;
static uint8_t md_flux_tampon[1024];
static uint32_t md_flux_debut = 0xFFFFFFFFu, md_flux_long;
static const uint8_t *md_src(const uint8_t *data, uint32_t off, uint32_t n) {
  if (!md_flux) return data + off;
  if (md_flux_debut != 0xFFFFFFFFu && off >= md_flux_debut &&
      off + n <= md_flux_debut + md_flux_long)
    return md_flux_tampon + (off - md_flux_debut);
  memset(md_flux_tampon, 0, sizeof(md_flux_tampon));
  md_flux_long = 0;
  if (fseek(md_flux, (long)off, SEEK_SET) == 0)
    md_flux_long = (uint32_t)fread(md_flux_tampon, 1, sizeof(md_flux_tampon), md_flux);
  md_flux_debut = off;
  if (md_flux_long < n) md_flux_long = n;   // au-dela de la fin : des zeros
  return md_flux_tampon;
}
#define MD_SRC(off, n) md_src(data, (uint32_t)(off), (uint32_t)(n))
#define MD_FLUX (md_flux != NULL)
#else
#define MD_SRC(off, n) (data + (off))
#define MD_FLUX 0
#endif

// Lit tout le morceau SAUF les données de la banque, et remplace le module
// courant. `pcm_off` / `pcm_len` disent où ces données se trouvent dans le
// fichier : c'est à l'appelant de les verser dans la banque.
static bool md_charge_entete(const uint8_t *data, uint32_t size,
                             uint32_t *pcm_off, uint32_t *pcm_len) {
  *pcm_off = 0;
  *pcm_len = 0;
  if ((!data && !MD_FLUX) || size < 20 || memcmp(MD_SRC(0, 20), "XMMTRACK", 8) != 0)
    return false;

  // Toutes les versions restent lisibles : seule la table des instruments a
  // changé de taille d'enregistrement au fil des ajouts. Tout ce qui précède
  // est identique, donc seuls les deux derniers blocs se calculent autrement.
  uint16_t version = read16le(MD_SRC(8, 2));
  const uint32_t rec = md_instr_record_size(version);
  if (rec == 0)
    return false;
  // Les lignes de phrase et de table se sont allongées en v7.
  const uint32_t prow = (version >= 16) ? MD_PHRASE_ROW_BYTES
                      : (version >= 7)  ? MD_PHRASE_ROW_BYTES_V15
                                        : MD_PHRASE_ROW_BYTES_V6;

  const uint32_t instr_off = MD_PHRASE_OFFSET +
                             MD_MAX_PHRASES * MD_ROWS_PER_PHRASE * prow;
  const uint32_t names_off = instr_off + MD_MAX_INSTRUMENTS * rec;
  if (size < names_off + MD_NAMES_TABLE_SIZE)
    return false;

  md_lock(&replayer_lock);

  md_replayer_shut();

  md_module_t *mod = (md_module_t *)calloc(1, sizeof(md_module_t));
  if (!mod) {
    md_unlock(&replayer_lock);
    return false;
  }

  const uint8_t *h = MD_SRC(0, MD_SONG_OFFSET);   // l'en-tete
  mod->num_channels = h[10];
  if (mod->num_channels == 0 || mod->num_channels > MD_MAX_CHANNELS)
    mod->num_channels = MD_MAX_CHANNELS;
  mod->initial_tempo = h[11];
  mod->initial_speed = h[12];
  mod->common_flag = h[13];
  mod->macro_speedup = read16le(h + 14);
  if (mod->macro_speedup == 0)
    mod->macro_speedup = 1;
  mod->bpm_tempo_finetune = (int16_t)read16le(h + 16);
  mod->bpm_rows_per_beat = h[18];
  if (mod->bpm_rows_per_beat == 0)
    mod->bpm_rows_per_beat = 4;

  memcpy(mod->songname, h + 20, 43);
  mod->songname[42] = '\0';
  memcpy(mod->composer, h + 63, 43);
  mod->composer[42] = '\0';

  for (int c = 0; c < MD_MAX_CHANNELS; c++)
    memcpy(mod->song[c], MD_SRC(MD_SONG_OFFSET + c * MD_SONG_ROWS, MD_SONG_ROWS), MD_SONG_ROWS);

  for (int i = 0; i < MD_MAX_CHAINS; i++) {
    for (int r = 0; r < MD_ROWS_PER_CHAIN; r++) {
      const uint8_t *d = MD_SRC(MD_CHAIN_OFFSET + (i * MD_ROWS_PER_CHAIN + r) * 2, 2);
      mod->chains[i].rows[r].phrase = d[0];
      mod->chains[i].rows[r].transpose = (int8_t)d[1];
    }
  }

  for (int i = 0; i < MD_MAX_PHRASES; i++) {
    for (int r = 0; r < MD_ROWS_PER_PHRASE; r++) {
      const uint8_t *d = MD_SRC(MD_PHRASE_OFFSET +
                         (i * MD_ROWS_PER_PHRASE + r) * prow, prow);
      md_phrase_row_t *pr = &mod->phrases[i].rows[r];
      pr->note = d[0];
      pr->instr = d[1];
      // Avant la v11 la vélocité tenait sur 0-3F : on la remet sur 0-7F pour
      // qu'un morceau ancien garde exactement le même volume.
      pr->vel = (d[2] == MD_EMPTY || version >= 11)
                    ? d[2]
                    : (uint8_t)((d[2] > 63 ? 63 : d[2]) * 127 / 63);
      pr->cmd = d[3];
      pr->cmdval = d[4];
      // La colonne MD n'existe qu'à partir de la v7.
      pr->mdcmd = (prow >= 7) ? md_mdcmd_from_file(d[5], version) : MD_EMPTY;
      pr->mdval = (prow >= 7) ? d[6] : 0;
      // Les colonnes CC 2 à 4 n'existent qu'à partir de la v16. Le module est
      // alloué à zéro, et zéro est un CC valide : on les pose VIDES.
      for (int k = 0; k < 3; k++) {
        pr->cc[k] = (prow >= 13) ? md_mdcmd_from_file(d[7 + 2 * k], version)
                                 : MD_EMPTY;
        pr->ccval[k] = (prow >= 13) ? d[8 + 2 * k] : 0;
      }
    }
  }

  // Les tables ne sont pas encore dans le fichier : on les crée vides plutôt
  // qu'à zéro, « VOL 0 » voulant dire silence et non « pas de consigne ».
  for (int i = 0; i < MD_MAX_TABLES; i++)
    for (int r = 0; r < MD_TABLE_ROWS; r++) {
      md_table_row_t *tr = &mod->tables[i].rows[r];
      tr->vol = 0;
      tr->cmd1 = MD_EMPTY;
      tr->cmd2 = MD_EMPTY;
      tr->mdcmd = MD_EMPTY;
    }

  // Les tables ne sont dans le fichier qu'à partir de la v7 ; avant, elles
  // restent celles qu'on vient d'initialiser à vide.
  const uint32_t tables_off = names_off + MD_NAMES_TABLE_SIZE;
  if (version >= 7 && size >= tables_off + MD_TABLES_BYTES) {
    for (int i = 0; i < MD_MAX_TABLES; i++) {
      for (int r = 0; r < MD_TABLE_ROWS; r++) {
        const uint8_t *d = MD_SRC(tables_off +
                           (i * MD_TABLE_ROWS + r) * MD_TABLE_ROW_BYTES, MD_TABLE_ROW_BYTES);
        md_table_row_t *tr = &mod->tables[i].rows[r];
        tr->vol = d[0];
        tr->transpose = (int8_t)d[1];
        tr->cmd1 = d[2];  tr->val1 = d[3];
        tr->cmd2 = d[4];  tr->val2 = d[5];
        tr->mdcmd = md_mdcmd_from_file(d[6], version); tr->mdval = d[7];
      }
    }
  }

  for (int i = 0; i < MD_MAX_INSTRUMENTS; i++) {
    md_read_instrument(MD_SRC(instr_off + i * rec, rec),
                       &mod->instruments[i], version);
    memcpy(mod->instr_names[i], MD_SRC(names_off + i * 43, 43), 43);
    mod->instr_names[i][42] = '\0';
  }

  // ── En-têtes de la banque d'échantillons (v10) ──────────────────────────
  // Absente des versions antérieures : le morceau se charge alors sans
  // échantillon, ce qui est le bon comportement. Avant la v14, tout était en
  // 8 bits à 32 kHz et l'en-tête faisait 40 octets.
  if (version >= 10) {
    const uint32_t base = tables_off + MD_TABLES_BYTES;
    const uint32_t hdr = (version >= 14) ? MD_SAMPLE_HDR_BYTES
                                         : MD_SAMPLE_HDR_BYTES_V13;
    if (size >= base + 4 + MD_MAX_SAMPLES * hdr) {
      uint32_t pcm_bytes = read32le(MD_SRC(base, 4));
      if (pcm_bytes > MD_PCM_BANK_BYTES) pcm_bytes = MD_PCM_BANK_BYTES;
      for (int i = 0; i < MD_MAX_SAMPLES; i++) {
        md_sample_t *sm = &mod->samples[i];
        const uint8_t *p = MD_SRC(base + 4 + (uint32_t)i * hdr, hdr);
        memcpy(sm->name, p, 24);
        sm->name[23] = 0;
        sm->offset = read32le(p + 24);
        sm->length = read32le(p + 28);
        sm->loop = (int32_t)read32le(p + 32);
        sm->base_note = p[36];
        sm->bits = (version >= 14 && p[37] == 16) ? 16 : 8;
        sm->cap = sm->length;   // la place réservée n'est pas dans le fichier
        sm->rate = (version >= 14) ? read32le(p + 40) : MD_PCM_RATE;
        if (sm->rate == 0) sm->rate = MD_PCM_RATE;
        // Un en-tête qui sortirait de la banque est ignoré plutôt que de
        // faire lire n'importe quoi au moteur.
        const uint32_t octets = sm->length * (uint32_t)(sm->bits / 8);
        if (sm->length > MD_PCM_BANK_BYTES || sm->offset > pcm_bytes ||
            octets > pcm_bytes - sm->offset) {
          memset(sm, 0, sizeof(*sm));
        }
      }
      *pcm_off = base + 4 + MD_MAX_SAMPLES * hdr;
      *pcm_len = pcm_bytes;
    }
  }

  current_module = mod;

  md_unlock(&replayer_lock);
  return true;
}

bool md_replayer_load_mem(const uint8_t *data, uint32_t size) {
  md_macros_vide();   // les macros d'un A2M importe ne suivent pas
  uint32_t off = 0, lg = 0;
  if (!md_charge_entete(data, size, &off, &lg))
    return false;
  if (lg && off + lg <= size) {
    memcpy(md_banque, data + off, lg);
    current_module->pcm_used = lg;
    // L'ARM7 lit la banque sans passer par le cache de l'ARM9.
    DC_FlushRange(md_banque, lg);
  } else if (lg) {
    md_banque_vide();
  }
  return true;
}

// Ouvre un morceau EN FLUX : seul le bloc fixe passe par un tampon, les
// données de la banque sont lues directement à leur place. Lire le fichier en
// entier coûterait deux mégaoctets de plus, que la DS Lite n'a pas.
bool md_replayer_load_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;
  fseek(f, 0, SEEK_END);
  const long taille = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (taille < 20) {
    fclose(f);
    return false;
  }
#ifdef FM1
  // FM-1 : en flux, directement dans le fichier (voir md_src).
  uint32_t off = 0, lg = 0;
  md_flux = f;
  md_flux_debut = 0xFFFFFFFFu;
  const bool ok = md_charge_entete(NULL, (uint32_t)taille, &off, &lg);
  md_flux = NULL;
#else
  // Le bloc fixe de la version courante est le plus grand de tous.
  uint32_t avance = MD_FILE_SIZE + 4 + MD_SAMPLES_HDR_TOTAL;
  if (avance > (uint32_t)taille) avance = (uint32_t)taille;
  uint8_t *entete = (uint8_t *)malloc(avance);
  if (!entete) {
    fclose(f);
    return false;
  }
  uint32_t off = 0, lg = 0;
  const bool ok = fread(entete, 1, avance, f) == avance &&
                  md_charge_entete(entete, avance, &off, &lg);
  free(entete);
#endif
  if (ok && lg) {
    if (off + lg <= (uint32_t)taille && fseek(f, (long)off, SEEK_SET) == 0 &&
        fread(md_banque, 1, lg, f) == lg) {
      current_module->pcm_used = lg;
      DC_FlushRange(md_banque, lg);
    } else {
      md_banque_vide();
    }
  }
  // Le bloc MIDI, s'il est là, suit la banque. Un fichier plus ancien n'en a
  // pas : les deux noms restent alors vides.
  if (ok) {
    memset(current_module->midi_cibles, 0, sizeof(current_module->midi_cibles));
    const uint32_t apres = off + lg;
    static uint8_t info[MD_MIDINFO_BYTES];
    uint8_t magie[8];
    if (off > 0 && (uint32_t)taille >= apres + 8 &&
        fseek(f, (long)apres, SEEK_SET) == 0 &&
        fread(magie, 1, 8, f) == 8) {
      const uint32_t reste = (uint32_t)taille - apres - 8;
      if (memcmp(magie, "XMMIDIN3", 8) == 0 &&
          reste >= MD_MIDINFO_BYTES - 8 &&
          fread(info, 1, MD_MIDINFO_BYTES - 8, f) == MD_MIDINFO_BYTES - 8) {
        // Une fiche par canal : on les prend telles quelles.
        for (int c = 0; c < MD_MIDI_CANAUX; c++) {
          md_midi_cible_t *t = &current_module->midi_cibles[c];
          const uint8_t *p = info + c * 48;
          memcpy(t->soft, p, 16);      WEB_PTR(t->soft)[15] = 0;
          memcpy(t->proj, p + 16, 16); WEB_PTR(t->proj)[15] = 0;
          memcpy(t->map,  p + 32, 16); WEB_PTR(t->map)[15] = 0;
        }
#ifdef FM1
        { // La DRUM MACHINE, si le fichier l'a (apres le bloc MIDI)
          static uint8_t d[8 + 32 + 16];
          memset(current_module->drum_pas, 0, sizeof current_module->drum_pas);
          memset(current_module->drum_trans, 0, sizeof current_module->drum_trans);
          if (reste >= MD_MIDINFO_BYTES - 8 + 40 && fread(d, 1, 40, f) == 40 &&
              (!memcmp(d, "OBTDRUM1", 8) || !memcmp(d, "OBTDRUM2", 8))) {
            for (int k = 0; k < 16; k++)
              current_module->drum_pas[k] = (uint16_t)(d[8 + 2 * k] | d[9 + 2 * k] << 8);
            if (!memcmp(d, "OBTDRUM2", 8) && reste >= MD_MIDINFO_BYTES - 8 + sizeof d && fread(d + 40, 1, 16, f) == 16)
              for (int k = 0; k < 16; k++)
                current_module->drum_trans[k] = (int8_t)d[40 + k];
          }
        }
#endif
      } else {
        // Un fichier plus ancien : UNE fiche pour tout le morceau. On la
        // recopie sur les canaux que ses instruments MIDI emploient, faute de
        // quoi l'information serait perdue.
        const bool v20 = memcmp(magie, "XMMIDIN2", 8) == 0;
        const bool v19 = memcmp(magie, "XMMIDINF", 8) == 0;
        const uint32_t lgVieux = (v20 ? MD_MIDINFO_BYTES_V20
                                      : MD_MIDINFO_BYTES_V19) - 8;
        if ((v20 || v19) && reste >= lgVieux &&
            fread(info, 1, lgVieux, f) == lgVieux) {
          char soft[16], proj[16], map[16];
          memcpy(soft, info, 16);      soft[15] = 0;
          memcpy(proj, info + 16, 16); proj[15] = 0;
          map[0] = 0;
          if (v20) { memcpy(map, info + 32, 16); map[15] = 0; }
          bool pose = false;
          for (int c = 0; c < MD_MIDI_CANAUX; c++)
            if (md_replayer_canal_midi_utilise(c)) {
              md_replayer_set_midi_cible(c, soft, proj, map);
              pose = true;
            }
          // Aucun instrument MIDI : on la garde sur le premier canal.
          if (!pose) md_replayer_set_midi_cible(0, soft, proj, map);
        }
      }
    }
  }
  fclose(f);
  return ok;
}


// Cherche, à partir de `from`, la prochaine ligne du SONG qui porte un chain
// sur ce canal. Renvoie -1 si la colonne est vide jusqu'en bas.
static int md_next_song_row(int c, int from) {
  for (int r = from; r < MD_SONG_ROWS; r++)
    if (current_module->song[c][r] != MD_EMPTY)
      return r;
  return -1;
}

// Place le canal sur la première phrase utilisable à partir de sa position
// courante, en sautant les lignes de chain vides et en passant au chain suivant
// du song quand le chain est épuisé. Boucle en fin de colonne (comme LSDJ).
// ── Blocs de la grille SONG ─────────────────────────────────────────────────
// Modèle repris d'un tracker de samples à deux canaux pris pour référence
// (soloSongLocs, blockTopRow, addChainLocs, buildSongLocations).
//
// Chaque canal joue le BLOC CONTIGU de lignes remplies qui contient le curseur,
// et boucle sur CE bloc. Trois règles, dans les mots du fichier :
//   « Play CONTIGUOUS non-empty song rows; stop at the first empty (loops there). »
//   « A song loop must return to the TOP of its block, not to wherever playback
//     entered it. »
//   « enter on the cursor row within the block »
// Et pour les chaînes : « STOPS at the first empty row (it never skips an empty
// to reach a later value) ».
//
// Les canaux sont INDÉPENDANTS : chacun boucle sur son propre bloc, qui n'a pas
// forcément la même longueur que celui du voisin.

// Haut du bloc contigu qui contient `from` : on remonte tant que c'est rempli.
static int md_block_top_row(int c, int from) {
  if (!current_module || c < 0 || c >= MD_MAX_CHANNELS)
    return 0;
  if (from < 0 || from >= MD_SONG_ROWS)
    return 0;
  if (current_module->song[c][from] == MD_EMPTY)
    return from;
  int t = from;
  while (t > 0 && current_module->song[c][t - 1] != MD_EMPTY)
    t--;
  return t;
}

static void md_channel_seek(int c) {
  for (int guard = 0; guard < 4096; guard++) {
    if (ch_chain[c] < 0) { // il faut (re)prendre un chain dans le song
      int r = ch_song_row[c];
      // On joue les lignes CONTIGUËS. À la première case vide — ou au bas de la
      // grille — on reboucle en HAUT DU BLOC, pas en ligne 0. Sans ça, un
      // morceau qui démarre en ligne 02 remontait toujours en 00.
      if (r < 0 || r >= MD_SONG_ROWS ||
          current_module->song[c][r] == MD_EMPTY) {
        r = ch_block_top[c];
        if (r < 0 || r >= MD_SONG_ROWS ||
            current_module->song[c][r] == MD_EMPTY) {
          ch_running[c] = false; ch_phrase[c] = -1; return;
        }
      }
      ch_song_row[c] = r;
      ch_chain[c] = current_module->song[c][r];
      ch_chain_row[c] = 0;
    }
    if (ch_chain_row[c] >= MD_ROWS_PER_CHAIN) { // chain fini → ligne suivante
      ch_song_row[c]++;
      ch_chain[c] = -1;
      continue;
    }
    const md_chain_t *ch = &current_module->chains[ch_chain[c] & (MD_MAX_CHAINS - 1)];
    const md_chain_row_t *cr = &ch->rows[ch_chain_row[c]];
    if (cr->phrase == MD_EMPTY) {   // la chaîne s'arrête ici, on ne saute pas
      ch_song_row[c]++;
      ch_chain[c] = -1;
      continue;
    }
    ch_phrase[c] = cr->phrase;
    ch_row_transpose[c] = cr->transpose;
    return;
  }
  ch_running[c] = false;
}

// Cherche la prochaine ligne non vide À L'INTÉRIEUR du chain courant, en
// rebouclant sur lui : utilisé quand la lecture est limitée à un chain, où l'on
// ne doit jamais retourner chercher une ligne dans le song.
static void md_chain_seek_in_scope(int c) {
  const md_chain_t *ch =
      &current_module->chains[ch_chain[c] & (MD_MAX_CHAINS - 1)];
  // Écran CHAIN : la lecture part de la ligne du CURSEUR, avance sur les slots
  // contigus, s'arrête au premier vide — et reboucle sur LA LIGNE DU CURSEUR,
  // pas sur le slot 0.
  //
  // C'est ce que fait `addChainLocs(cid, fromRow: chainRow)` du tracker
  // d'exemple : la séquence commence à `fromRow`, « STOPS at the first empty
  // row », et boucle sur elle-même — donc elle revient à `fromRow`. Revenir au
  // slot 0 rejouait des lignes situées AVANT le curseur, séparées de lui par
  // un trou.
  int base = (scope_start_row >= 0 && scope_start_row < MD_ROWS_PER_CHAIN)
                 ? scope_start_row : 0;
  if (ch_chain_row[c] >= MD_ROWS_PER_CHAIN ||
      ch->rows[ch_chain_row[c]].phrase == MD_EMPTY)
    ch_chain_row[c] = base;
  const md_chain_row_t *cr = &ch->rows[ch_chain_row[c]];
  if (cr->phrase == MD_EMPTY) {
    ch_running[c] = false;   // chaîne entièrement vide
    ch_phrase[c] = -1;
    return;
  }
  ch_phrase[c] = cr->phrase;
  ch_row_transpose[c] = cr->transpose;
}

// ── H : le SAUT, aussi dans une PHRASE ─────────────────────────────────────
// Il n'existait que dans les tables. Dans une phrase, poser H ne faisait
// strictement rien — la commande se résolvait en « aucun effet », et le journal
// des essais le confirmait : H était l'une des deux seules lettres sans action.
//
// Mêmes conventions que dans une table, pour qu'il n'y ait qu'un H à retenir :
//   chiffre de gauche  combien de fois sauter avant de passer outre, 0 = sans fin
//   chiffre de droite  la ligne visée dans la phrase
// H00 fait donc boucler la phrase sur elle-même, indéfiniment.
//
// Le compteur est indexé par la LIGNE qui porte le H : deux boucles imbriquées
// comptent chacune leurs tours, comme dans une table.
//
// ⚠️ DIVERGENCE assumée avec le projet iPad, qui n'a pas encore ce saut-là.
static uint8_t ch_phrase_hop[MD_TOTAL_VOICES][MD_ROWS_PER_PHRASE];

void md_replayer_reset_phrase_hops(int c) {
  if (c < 0 || c >= MD_TOTAL_VOICES) return;
  for (int r = 0; r < MD_ROWS_PER_PHRASE; r++) ch_phrase_hop[c][r] = 0;
}

// La ligne courante porte-t-elle un H ?
static bool md_phrase_row_is_hop(int c) {
  if (!current_module || ch_phrase[c] < 0) return false;
  const int row = ch_phrase_row[c];
  if (row < 0 || row >= MD_ROWS_PER_PHRASE) return false;
  const md_phrase_row_t *pr =
      &current_module->phrases[ch_phrase[c] % MD_MAX_PHRASES].rows[row];
  return pr->cmd != MD_EMPTY && md_table_cmd_kind(pr->cmd) == MD_CMD_HOP;
}

static void md_channel_pas(int c);

// Effectue le saut de la ligne courante. Renvoie vrai s'il a eu lieu ; faux
// quand la boucle est terminée et qu'il faut simplement passer outre.
static bool md_phrase_hop_prend(int c) {
  const int row = ch_phrase_row[c];
  const md_phrase_row_t *pr =
      &current_module->phrases[ch_phrase[c] % MD_MAX_PHRASES].rows[row];
  const int fois = (pr->cmdval >> 4) & 0x0F;
  const int cible = pr->cmdval & 0x0F;
  if (fois == 0) {
    // H0y, comme LSDJ : la phrase s'arrete la et l'on passe a la SUIVANTE, a
    // sa ligne y. Sur l'ecran PHRASE seulement, ou la phrase boucle sur
    // elle-meme, la « suivante » est la meme : on revient a sa ligne y.
    if (play_scope == MD_SCOPE_PHRASE) { ch_phrase_row[c] = cible; return true; }
    ch_phrase_row[c] = MD_ROWS_PER_PHRASE - 1;
    md_channel_pas(c);                       // la phrase suivante, ligne 0
    if (ch_running[c] && ch_phrase[c] >= 0) ch_phrase_row[c] = cible;
    return true;
  }
  if (ch_phrase_hop[c][row] < fois) {
    ch_phrase_hop[c][row]++;
    ch_phrase_row[c] = cible;
    return true;
  }
  ch_phrase_hop[c][row] = 0;                  // boucle terminée
  return false;
}

// Avance d'UNE ligne, en cascadant sur le chain puis sur le song quand on
// arrive au bout — sauf si la lecture est limitée à un chain ou une phrase.

#ifdef FM1
// FM-1, REC : la boucle de ce qui a ete enregistre. Quand le canal `rec_boucle_c` finit la phrase de la
// ligne de SONG `de_sr`, ligne de chain `de_cr`, il repart a `vers_sr` / `vers_cr`. -1 : pas de boucle.
static int rec_boucle_c = -1, rec_boucle_de_sr, rec_boucle_de_cr, rec_boucle_vers_sr, rec_boucle_vers_cr;
void md_replayer_rec_boucle(int c, int de_sr, int de_cr, int vers_sr, int vers_cr) {
  rec_boucle_de_sr = de_sr; rec_boucle_de_cr = de_cr;
  rec_boucle_vers_sr = vers_sr; rec_boucle_vers_cr = vers_cr;
  rec_boucle_c = (de_sr < 0) ? -1 : c;
}
// REC pendant la lecture, sur une piste qui ne jouait pas : elle rejoint la
// lecture a la position de la piste ref (meme ligne de SONG, de chain, de
// phrase). Faux si ref ne joue pas ou que c n'a rien a cette ligne de SONG.
bool md_replayer_rejoint(int c, int ref) {
  if (c < 0 || c >= MD_MAX_CHANNELS || ref < 0 || ref >= MD_MAX_CHANNELS || !current_module)
    return false;
  bool ok = false;
  md_lock(&replayer_lock);
  if (play_status == MD_PLAYING && ch_running[ref] && ch_phrase[ref] >= 0) {
    const int sr = ch_song_row[ref], cr = ch_chain_row[ref];
    const uint8_t ch = sr >= 0 && sr < MD_SONG_ROWS ? current_module->song[c][sr] : MD_EMPTY;
    if (ch != MD_EMPTY && cr >= 0 && cr < MD_ROWS_PER_CHAIN) {
      const md_chain_row_t *r = &current_module->chains[ch % MD_MAX_CHAINS].rows[cr];
      if (r->phrase != MD_EMPTY) {
        ch_song_row[c] = sr;
        ch_block_top[c] = ch_block_top[ref];
        ch_chain[c] = ch;
        ch_chain_row[c] = cr;
        ch_phrase[c] = r->phrase;
        ch_row_transpose[c] = r->transpose;
        ch_phrase_row[c] = ch_phrase_row[ref];
        ch_pending_row[c] = -1;
        ch_running[c] = true;
        ok = true;
      }
    }
  }
  md_unlock(&replayer_lock);
  return ok;
}
#endif

static void md_channel_pas(int c) {
  ch_phrase_row[c]++;
  if (ch_phrase_row[c] < MD_ROWS_PER_PHRASE)
    return;
  ch_phrase_row[c] = 0;

  // Écran PHRASE : la phrase boucle sur elle-même, on ne remonte pas au chain.
  if (play_scope == MD_SCOPE_PHRASE)
    return;
#ifdef FM1
  if (c == rec_boucle_c && ch_song_row[c] == rec_boucle_de_sr && ch_chain_row[c] == rec_boucle_de_cr &&
      rec_boucle_vers_sr >= 0 && rec_boucle_vers_sr < MD_SONG_ROWS &&
      current_module->song[c][rec_boucle_vers_sr] != MD_EMPTY) {
    ch_song_row[c] = rec_boucle_vers_sr;
    ch_chain[c] = current_module->song[c][rec_boucle_vers_sr];
    ch_chain_row[c] = rec_boucle_vers_cr;
    ch_phrase[c] = -1;
    md_channel_seek(c);
    return;
  }
#endif

  ch_chain_row[c]++;
  ch_phrase[c] = -1;
  // Écran CHAIN : on reste dans ce chain, qui boucle sur lui-même.
  if (play_scope == MD_SCOPE_CHAIN) {
    md_chain_seek_in_scope(c);
    return;
  }
  md_channel_seek(c);
}

// Fait avancer le canal, puis FRANCHIT tout de suite un éventuel H.
//
// Le repère de lecture ne doit JAMAIS s'arrêter sur la ligne qui porte le
// saut : la boucle se referme AVANT elle. Avec H00 en ligne 3, on parcourt
// 0 1 2 0 1 2… et la ligne 3 n'est ni affichée ni jouée. C'est ce que fait
// déjà la table quelques dizaines de lignes plus bas — la phrase, elle,
// s'arrêtait dessus une image, ce qui s'entendait et se voyait.
static void md_channel_advance(int c) {
  if (!ch_running[c]) return;
  md_channel_pas(c);
  // La borne évite de tourner sans fin si toutes les lignes portent un H.
  for (int garde = 0; garde <= MD_ROWS_PER_PHRASE; garde++) {
    if (!md_phrase_row_is_hop(c)) return;
    if (!md_phrase_hop_prend(c)) md_channel_pas(c);   // boucle finie : on passe
  }
}

// Construit l'événement de la ligne courante du canal, dans le format attendu
// par le moteur d'effets hérité (qui n'a pas changé d'une ligne).
// Dxx : place le canal, deja sur son chain, a la ligne `ligne` du pattern —
// la phrase ligne/16 du chain, ligne%16 de cette phrase.
static void md_va_en_ligne(int c, int ligne) {
  if (ligne <= 0) return;
  ch_chain_row[c] = (ligne / MD_ROWS_PER_PHRASE) % MD_ROWS_PER_CHAIN;
  ch_phrase[c] = -1;
  md_channel_seek(c);
  if (ch_running[c] && ch_phrase[c] >= 0)
    ch_phrase_row[c] = ligne % MD_ROWS_PER_PHRASE;
}

static md_event_t md_channel_event(int c) {
  md_event_t ev;
  ev.note = 0; ev.instr = 0;
  for (int k = 0; k < MD_EV_EFFETS; k++) {
    ev.effects[k].type = 0xFF; ev.effects[k].val = 0;
  }
  if (!ch_running[c] || ch_phrase[c] < 0) return ev;

  const md_phrase_t *ph = &current_module->phrases[ch_phrase[c] % MD_MAX_PHRASES];
  const md_phrase_row_t *pr = &ph->rows[ch_phrase_row[c]];

  ev.note = pr->note;
  // Transposition du chain : elle décale la note, pas le note-off.
  if (ev.note > 0 && ev.note <= MD_MAX_NOTE && ch_row_transpose[c] != 0) {
    int n = (int)ev.note + ch_row_transpose[c];
    ev.note = (uint8_t)(n < 1 ? 1 : (n > MD_MAX_NOTE ? MD_MAX_NOTE : n));
  }
  ev.instr = pr->instr;
  if (pr->cmd != MD_EMPTY) {
    // La colonne CMD stocke l'INDICE de la commande (la lettre affichée) ; le
    // moteur d'effets attend un numéro d'effet. Même traduction que dans les
    // tables, par la liste partagée — il n'y a qu'un vocabulaire de commandes
    // dans tout le tracker.
    int pe = -1;
    uint8_t pv = 0;
    if (pr->cmd != MD_EMPTY)
      md_table_cmd_resolve(pr->cmd, pr->cmdval, &pe, &pv);
    ev.effects[0].type = (pe < 0) ? 0xFF : (uint8_t)pe;
    ev.effects[0].val = (pe < 0) ? 0 : pv;
  }
  // FX1-FX4 : effets d'AdLib Tracker 2, numerotes comme ceux du moteur.
  for (int k = 0; k < MD_PHRASE_FX; k++) {
    const uint8_t t = *md_fx_type((md_phrase_row_t *)pr, k);
    if (t != MD_EMPTY && t <= MD_EFF_GLOBAL_FSLIDE_DOWN) {
      ev.effects[1 + k].type = t;
      ev.effects[1 + k].val = *md_fx_val((md_phrase_row_t *)pr, k);
    }
  }
  return ev;
}

// Un effet qui module le son en continu (hauteur ou volume), par opposition à
// une action instantanée comme couper la note ou changer le tempo. Seuls les
// premiers survivent au changement de ligne.
// L'instrument convient-il à la colonne ?
//
// Un instrument PSG n'a aucun paramètre FM et réciproquement. DefleMask laisse
// la voie MUETTE quand les deux ne s'accordent pas ; sans ce garde-fou, notre
// moteur chargeait la moitié FM restée aux valeurs d'usine et jouait un son
// que le morceau d'origine ne produit jamais.
//
// Les instruments écrits à la main (MD_INSTR_KIND_ANY) passent partout.
static bool md_instr_fits_channel(int c, int instr) {
  (void)c;
  (void)instr;
  return true;   // échantillon ou MIDI : toutes les pistes acceptent les deux
}

static bool md_effect_is_continuous(uint8_t eff) {
  switch (eff) {
  case MD_EFF_ARPEGGIO:
  case MD_EFF_FSLIDE_UP:
  case MD_EFF_FSLIDE_DOWN:
  case MD_EFF_FSLIDE_UP_FINE:
  case MD_EFF_FSLIDE_DOWN_FINE:
  case MD_EFF_TONE_PORTAMENTO:
  case MD_EFF_VIBRATO:
  case MD_EFF_TREMOLO:
    return true;
  default:
    return false;
  }
}

static void md_process_row() {

  if (!current_module)
    return;

  int next_order_index = -1;
  int next_row_index = -1; // conservés : les effets B/C les écrivent encore

  // Process all channels
  for (int c = 0; c < current_module->num_channels; c++) {
    if (md_channel_disabled[c]) continue;
    md_event_t event = md_channel_event(c);
    md_event_t *ev = &event;

    // Vélocité : elle fixe le volume du canal au déclenchement de la ligne.
    if (ch_running[c] && ch_phrase[c] >= 0) {
      uint8_t v = current_module->phrases[ch_phrase[c] % MD_MAX_PHRASES]
                      .rows[ch_phrase_row[c]].vel;
      if (v != MD_EMPTY) {
        // La colonne va de 0 à 7F comme DefleMask ; l'atténuation interne, elle,
        // tient sur six bits (les glissandos de volume la manipulent partout en
        // 0x3F). On ramène donc l'échelle ici, à l'unique endroit qui la lit.
        int vv = v > 127 ? 127 : v;
        ch_fmpar[c].volC = (uint8_t)(63 - (vv * 63 + 63) / 127);
        ch_vel_fin[c] = (uint8_t)vv;      // la valeur exacte, sur 7 bits
        md_write_volume(c);
      }
    }

    if (ev->instr) {
      // Instrument destiné à l'autre puce : la colonne se tait, comme dans
      // DefleMask. On ne charge rien et on retiendra la note plus bas.
      if (!md_instr_fits_channel(c, ev->instr)) {
        ch_instr_blocked[c] = true;
      } else {
        ch_instr_blocked[c] = false;
        ch_current_instr[c] = ev->instr;
        // Poser un instrument FM sur la voie PCM la rebranche à la synthèse —
        // c'est le mode « FMP » : la colonne redevient une sixième voie FM.
        if (c == MD_PCM_CHANNEL &&
            current_module->instruments[ev->instr - 1].kind != MD_INSTR_KIND_PCM) {
          md_chip_pcm_stop();
          md_chip_pcm_enable(false);
        }
        md_set_instrument(c, &current_module->instruments[ev->instr - 1]);
        // La note vient d'écraser la copie de travail : on remet par-dessus
        // les réglages MD CMD encore actifs sur ce canal.
        md_reapply_mdcmds(c);
      }
    }

    // In trackers, most tick-based effects (Arpeggio, F-Slide, Vibrato) ONLY
    // run on the row they are defined. If the next row does not re-define them
    // (e.g. it is empty), the effect stops.
    // Some states (like base frequency, current volume) persist, but the
    // *action* of the effect does not. We must clear active tick effects per
    // row.
    for (int k = 0; k < MD_EV_EFFETS; k++) {
      const int e = md_slot_ev[k];   // l'emplacement de cette colonne
      uint8_t eff_type = ev->effects[k].type;
      uint8_t eff_val = ev->effects[k].val;

      // FX1-FX4 suivent AdLib Tracker 2 : un effet ne dure que sa ligne.
      // (La colonne CMD garde la convention de LSDJ, juste en dessous.)
      if (k > 0) {
        ch_active_eff[c][e] = 0xFF;
        ch_active_eff_val[c][e] = 0;
      }

      // Comme LSDJ : une NOUVELLE note, sans commande sur sa ligne (colonne
      // CMD), arrete le pitch bend (P) qui tournait : elle joue a sa hauteur.
      if (k == 0 && ev->note >= 1 && ev->note <= MD_MAX_NOTE &&
          (eff_type == 0xFF || (eff_type == 0 && eff_val == 0)) &&
          (ch_active_eff[c][e] == MD_EFF_FSLIDE_UP || ch_active_eff[c][e] == MD_EFF_FSLIDE_DOWN)) {
        ch_active_eff[c][e] = 0xFF;
        ch_active_eff_val[c][e] = 0;
      }

      // Un effet CONTINU survit au changement de ligne.
      //
      // Dans un tracker classique, un effet ne dure que la ligne où il est
      // écrit. Ici on suit la convention de LSDJ : un pitch bend, un vibrato
      // ou un arpège continue de tourner jusqu'à ce qu'on l'arrête — sinon le
      // glissando s'interrompt sèchement au bout d'une ligne au lieu de courir
      // sur toute la phrase. On l'arrête en réécrivant la commande avec la
      // valeur 00.
      if (!md_effect_is_continuous(ch_active_eff[c][e])) {
        ch_active_eff[c][e] = 0xFF;
        ch_active_eff_val[c][e] = 0;
      } else if (eff_type == ch_active_eff[c][e] && eff_val == 0) {
        ch_active_eff[c][e] = 0xFF;   // même commande à 00 = on coupe
        ch_active_eff_val[c][e] = 0;
      }

      // ⚠️ UNE VALEUR 00 ARRÊTE UN EFFET CONTINU, elle ne le relance pas.
      // On coupait bien juste au-dessus, puis on le RÉARMAIT plus bas avec la
      // valeur nulle. Et comme les paramètres sont mémorisés
      // (md_table_latch_effect ne retient que les valeurs NON nulles), le
      // glissando repartait à la vitesse de la fois d'avant : « 02 00 » après
      // « 02 56 » continuait de glisser au lieu de s'arrêter.
      //
      // ⚠️ SEULEMENT LES CONTINUS. Le zéro est une valeur légitime ailleurs :
      // « J00 » veut dire « saute à la ligne 0 », pas « n'y va pas ».
      // (Colonne CMD seulement. Dans FX1-FX4, comme AT2, une valeur 00
      // REPREND le dernier parametre de l'effet : c'est sa memoire.)
      if (k == 0 && eff_val == 0 && md_effect_is_continuous(eff_type)) {
        ch_active_eff[c][e] = 0xFF;
        ch_active_eff_val[c][e] = 0;
        continue;
      }

      // In AT2, 0 is Arpeggio. But '000' is an empty cell.
      // We must explicitly skip '000' so it doesn't trigger the PERSISTENCE
      // of the last Arpeggio value.
      if (eff_type == 0 && eff_val == 0) {
        continue;
      }

      if (eff_type == 0xFF) {
        continue;
      }

      // (log [ARP] retiré — même raison que [NOTE].)

      if (eff_type == MD_EFF_ARPEGGIO || eff_type == MD_EFF_VIBRATO ||
          eff_type == MD_EFF_VOL_SLIDE || eff_type == MD_EFF_FSLIDE_UP ||
          eff_type == MD_EFF_FSLIDE_DOWN ||
          eff_type == MD_EFF_FSLIDE_UP_FINE ||
          eff_type == MD_EFF_FSLIDE_DOWN_FINE ||
          eff_type == MD_EFF_VOL_SLIDE_FINE ||
          eff_type == MD_EFF_TONE_PORTAMENTO ||
          eff_type == MD_EFF_TPORTA_VSLIDE ||
          eff_type == MD_EFF_TPORTA_VSLIDE_FINE ||
          eff_type == MD_EFF_VIBRATO_VSLIDE ||
          eff_type == MD_EFF_VIBRATO_VSLIDE_FINE ||
          eff_type == MD_EFF_ARPEGGIO_VSLIDE ||
          eff_type == MD_EFF_ARPEGGIO_VSLIDE_FINE ||
          eff_type == MD_EFF_FSLIDE_UP_VSLIDE ||
          eff_type == MD_EFF_FSLIDE_DOWN_VSLIDE ||
          eff_type == MD_EFF_FSLIDE_UP_FINE_VSLIDE ||
          eff_type == MD_EFF_FSLIDE_DOWN_FINE_VSLIDE ||
          eff_type == MD_EFF_FSLIDE_UP_VSLF ||
          eff_type == MD_EFF_FSLIDE_DOWN_VSLF ||
          eff_type == MD_EFF_FSLIDE_UP_FINE_VSLF ||
          eff_type == MD_EFF_FSLIDE_DOWN_FINE_VSLF ||
          eff_type == MD_EFF_TREMOLO ||
          eff_type == MD_EFF_TREMOR || eff_type == MD_EFF_RETRIG_NOTE ||
          eff_type == MD_EFF_MULTI_RETRIG_NOTE ||
          eff_type == MD_EFF_GLOBAL_FSLIDE_UP ||
          eff_type == MD_EFF_GLOBAL_FSLIDE_DOWN ||
          (eff_type == MD_EFF_EXTENDED2 &&
           ((eff_val >> 4) == MD_EX2_NOTE_DELAY ||
            (eff_val >> 4) == MD_EX2_NOTE_CUT ||
            (eff_val >> 4) == MD_EX2_GL_VOL_SLIDE_UP ||
            (eff_val >> 4) == MD_EX2_GL_VOL_SLIDE_DN ||
            (eff_val >> 4) == MD_EX2_GL_VOL_SLIDE_UP_F ||
            (eff_val >> 4) == MD_EX2_GL_VOL_SLIDE_DN_F))) {
        ch_active_eff[c][e] = eff_type;
        ch_active_eff_val[c][e] = eff_val;
      }

      switch (eff_type) {
      case MD_EFF_POSITION_JUMP: // Bxx
        next_order_index = eff_val;
        break;
      case MD_EFF_PATTERN_BREAK: // Dxx
        // In AT2, parameter is the row number (0-255). It is usually in hex in
        // UI but stored as int.
        next_row_index = eff_val;
        break;
      // ⚠️ Ces deux commandes ne touchent QUE l'état de lecture, jamais les
      // réglages du MORCEAU. Elles y écrivaient (initial_speed / initial_tempo),
      // or c'est exactement de là que le départ de lecture relit ses valeurs :
      // une commande survolée en éditant changeait donc le tempo du morceau
      // POUR DE BON. À une vitesse de 0xFF, la musique avançait d'une ligne
      // toutes les cinq secondes — on croit tout entendre s'arrêter — et le
      // stop/play n'y changeait rien puisqu'il rechargeait la valeur écrasée.
      case MD_EFF_SET_SPEED: // Axx with x < 0x20 usually (not tempo)
        if (eff_val > 0) {
          replay_speed = eff_val;
        }
        break;
      case MD_EFF_SET_TEMPO: // Nxx or Axx >= 0x20
        if (eff_val >= 18) {  // Minimum tempo
          replay_tempo = eff_val;
          // Update irq timings
          int irq_freq = replay_tempo * (current_module->macro_speedup > 0
                                             ? current_module->macro_speedup
                                             : 1);
          if (irq_freq == 0)
            irq_freq = 50;
          samples_per_tick = (int)((double)replay_sample_rate / ((double)irq_freq * md_finetune_scale()));
        if (samples_per_tick < 1) samples_per_tick = 1;
        }
        break;
      case MD_EFF_VOL_SLIDE:
      case MD_EFF_VOL_SLIDE_FINE:        // 20 : idem, mais appliqué au tick 0
        if (eff_val != 0)
          ch_volslide_val[c] = eff_val;
        break;
      case MD_EFF_FSLIDE_UP:
      case MD_EFF_FSLIDE_DOWN:
      case MD_EFF_FSLIDE_UP_FINE:        // 7 : fine = appliqué au tick 0
      case MD_EFF_FSLIDE_DOWN_FINE:      // 8
        if (eff_val != 0)
          ch_fslide_speed[c] = eff_val;
        break;

      // ── Effets COMBINÉS « X + volume slide » ───────────────────────────────
      // Pour TOUS ces combos, le paramètre alimente le VOLUME slide ; la partie
      // X (freq slide / arpège / vibrato) réutilise sa vitesse mémorisée.
      case MD_EFF_ARPEGGIO_VSLIDE:        // 24
      case MD_EFF_ARPEGGIO_VSLIDE_FINE:   // 25
      case MD_EFF_VIBRATO_VSLIDE_FINE:    // 17
      case MD_EFF_FSLIDE_UP_VSLIDE:       // 27
      case MD_EFF_FSLIDE_DOWN_VSLIDE:     // 28
      case MD_EFF_FSLIDE_UP_FINE_VSLIDE:  // 29
      case MD_EFF_FSLIDE_DOWN_FINE_VSLIDE:// 30
      case MD_EFF_FSLIDE_UP_VSLF:         // 31
      case MD_EFF_FSLIDE_DOWN_VSLF:       // 32
      case MD_EFF_FSLIDE_UP_FINE_VSLF:    // 33
      case MD_EFF_FSLIDE_DOWN_FINE_VSLF:  // 34
        if (eff_val != 0)
          ch_volslide_val[c] = eff_val;
        ch_arpeggio_state[c] = 0; // (utile pour 24/25 ; inoffensif sinon)
        break;

      case MD_EFF_TPORTA_VSLIDE_FINE:     // 16 : porta (mémorisé) + fine vol slide
        if (eff_val != 0)
          ch_volslide_val[c] = eff_val;
        if (ev->note > 0 && ev->note <= MD_MAX_NOTE)
          ch_porta_target[c] = md_hauteur(ev->note, md_ftune_de(c));
        break;
      case MD_EFF_TONE_PORTAMENTO:
        if (eff_val != 0)
          ch_porta_speed[c] = eff_val;
        // La cible est une hauteur absolue : le portamento traverse les
        // octaves sans rien de particulier à gérer.
        if (ev->note > 0 && ev->note <= MD_MAX_NOTE)
          ch_porta_target[c] = md_hauteur(ev->note, md_ftune_de(c));
        break;
      case MD_EFF_TPORTA_VSLIDE:
        if (eff_val != 0)
          ch_volslide_val[c] = eff_val;
        break;
      case MD_EFF_VIBRATO:
      case MD_EFF_VIBRATO_VSLIDE:
        if (eff_val != 0) {
          if (eff_val >> 4)
            ch_vibrato_speed[c] = eff_val >> 4;
          if (eff_val & 0x0F)
            ch_vibrato_depth[c] = eff_val & 0x0F;
        }
        if (eff_type == MD_EFF_VIBRATO_VSLIDE && eff_val != 0)
          ch_volslide_val[c] = eff_val;
        break;
      case MD_EFF_TREMOLO:
        if (eff_val != 0) {
          if (eff_val >> 4)
            ch_tremolo_speed[c] = eff_val >> 4;
          if (eff_val & 0x0F)
            ch_tremolo_depth[c] = eff_val & 0x0F;
        }
        break;
      case MD_EFF_TREMOR:
        if (eff_val != 0)
          ch_tremor_val[c] = eff_val;
        break;
      case MD_EFF_ARPEGGIO:
        if (eff_val != 0)
          ch_arpeggio_val[c] = eff_val;
        ch_arpeggio_state[c] = 0; // reset
        break;
      case MD_EFF_SET_GLOBAL_VOLUME:
        replay_global_vol = eff_val;
        if (replay_global_vol > 63)
          replay_global_vol = 63;
        break;
      case MD_EFF_RETRIG_NOTE:
        if (eff_val != 0)
          ch_retrig_speed[c] = eff_val;
        ch_retrig_count[c] = 0;
        break;
      case MD_EFF_MULTI_RETRIG_NOTE:
        // Multi-retrig : intervalle = nibble HAUT, opération volume = nibble BAS
        // (cf. AdlibTracker2 ef_MultiRetrigNote). On (re)démarre le compteur.
        ch_retrig_count[c] = 0;
        break;

      // ── Effets « set » instantanés (appliqués au déclenchement de la row) ──
      // Sémantique conservée du moteur d'origine : le paramètre est la LOUDNESS
      // (0..63) ; l'atténuation interne vaut 63 - paramètre.
      case MD_EFF_SET_CAR_VOL: { // 18 : volume des opérateurs porteurs
        ch_fmpar[c].volC = (uint8_t)((63 - (eff_val & 0x3F)) & 0x3F);
        md_write_volume(c);
        break;
      }
      case MD_EFF_SET_MOD_VOL: { // 9 : volume des opérateurs modulateurs
        ch_fmpar[c].volM = (uint8_t)((63 - (eff_val & 0x3F)) & 0x3F);
        md_write_volume(c);
        break;
      }
      case MD_EFF_SET_INS_VOLUME: { // 12 : volume instrument
        uint8_t atten = (uint8_t)((63 - (eff_val & 0x3F)) & 0x3F);
        ch_fmpar[c].volC = atten;
        if (ch_fmpar[c].feedbackConn & 1) // algorithme additif → aussi les mod
          ch_fmpar[c].volM = atten;
        md_write_volume(c);
        break;
      }
      case MD_EFF_SET_ALGORITHM: { // 19 : algorithme (nibble haut) / feedback (bas)
        // Sur une voie OPL, c'est le Jxy d'AT2 : forme d'onde de la porteuse
        // (x) et du modulateur (y), F = inchange.
        if (md_channel_is_opl(c)) {
          uint8_t *fm = MD_OPL_FM(&ch_instr_shadow[c]);
          if ((eff_val >> 4) <= 7) fm[MD_OPL_WAVE_C] = eff_val >> 4;
          if ((eff_val & 15) <= 7) fm[MD_OPL_WAVE_M] = eff_val & 15;
          opl_voix_instrument(ch_hw_voice[c], fm, ch_panning[c]);
          break;
        }
        uint8_t alg = eff_val >> 4, fb = eff_val & 0x0F;
        md_instr_t *sh = &ch_instr_shadow[c];
        if (alg <= 7)
          sh->algorithm = alg;
        if (fb <= 7)
          sh->feedback = fb;
        md_apply_instrument((uint8_t)c);
        break;
      }
      case MD_EFF_FORCE_INS_VOLUME: { // 40 : force le volume
        uint8_t atten = (uint8_t)((63 - (eff_val & 0x3F)) & 0x3F);
        ch_fmpar[c].volC = atten;
        if (ch_fmpar[c].feedbackConn & 1)
          ch_fmpar[c].volM = atten;
        md_write_volume(c);
        break;
      }
      case MD_EFF_EXTENDED3: { // 41 : réglages YM2612 en direct
        // nibble haut = sous-commande, nibble bas = valeur (0-15).
        // Ces réglages n'affectent que la VOIE en cours, pas l'instrument.
        uint8_t sub = eff_val >> 4;
        uint8_t v = eff_val & 0x0F;
        md_instr_t *sh = &ch_instr_shadow[c];
        switch (sub) {
        case 0: // algorithme (0-7)
          sh->algorithm = (v <= 7) ? v : 7;
          break;
        case 1: // feedback (0-7)
          sh->feedback = (v <= 7) ? v : 7;
          break;
        case 2: // multiple OP1
        case 3: // multiple OP2
        case 4: // multiple OP3
        case 5: // multiple OP4
          WEB_PTR(&sh->op[sub - 2])->multiple = v;   // (WEB_PTR : md_replayer.h)
          break;
        case 6: // detune OP1
        case 7: // detune OP2
        case 8: // detune OP3
        case 9: // detune OP4
          WEB_PTR(&sh->op[sub - 6])->detune = (uint8_t)(v & 0x07);
          break;
        case 10: // attaque (tous les opérateurs)
          for (int o = 0; o < 4; o++)
            sh->op[o].attack = (uint8_t)(v * 2 + 1);
          break;
        case 11: // decay (tous les opérateurs)
          for (int o = 0; o < 4; o++)
            sh->op[o].decay = (uint8_t)(v * 2);
          break;
        case 12: // niveau de maintien (tous les opérateurs)
          for (int o = 0; o < 4; o++)
            sh->op[o].sustain_level = v;
          break;
        case 13: // release (tous les opérateurs)
          for (int o = 0; o < 4; o++)
            sh->op[o].release = v;
          break;
        case 14: // AMS (0-3)
          sh->ams = (uint8_t)(v & 0x03);
          break;
        case 15: // PMS (0-7)
          sh->pms = (uint8_t)(v & 0x07);
          break;
        default:
          break;
        }
        md_apply_instrument((uint8_t)c);
        break;
      }
      case MD_EFF_EXTENDED: {
        uint8_t ext_type = eff_val >> 4;
        uint8_t ext_val = eff_val & 0x0F;
        // Zxy d'AdLib Tracker 2 sur une voie OPL : profondeur du tremolo et
        // du vibrato (0, 1), puis l'enveloppe du modulateur (2-5) et de la
        // porteuse (6-9), et le feedback (A). La copie de travail de
        // l'instrument est modifiee : la note suivante la garde aussi.
        if (md_channel_is_opl(c) && ext_type <= 10) {
          uint8_t *fm = WEB_PTR(MD_OPL_FM(&ch_instr_shadow[c]));
          static const struct { uint8_t octet, haut; } kZ[11] = {
            {0, 0}, {0, 0},
            {MD_OPL_AD_M, 1}, {MD_OPL_AD_M, 0}, {MD_OPL_SR_M, 1}, {MD_OPL_SR_M, 0},
            {MD_OPL_AD_C, 1}, {MD_OPL_AD_C, 0}, {MD_OPL_SR_C, 1}, {MD_OPL_SR_C, 0},
            {MD_OPL_FB_CONN, 0}};
          if (ext_type == 0) opl_voix_profondeurs(ext_val ? 1 : 0, -1);
          else if (ext_type == 1) opl_voix_profondeurs(-1, ext_val ? 1 : 0);
          else if (ext_type == 10)
            fm[MD_OPL_FB_CONN] = (uint8_t)((fm[MD_OPL_FB_CONN] & 0xF1) | ((ext_val & 7) << 1));
          else if (kZ[ext_type].haut)
            fm[kZ[ext_type].octet] = (uint8_t)((fm[kZ[ext_type].octet] & 0x0F) | (ext_val << 4));
          else
            fm[kZ[ext_type].octet] = (uint8_t)((fm[kZ[ext_type].octet] & 0xF0) | ext_val);
          if (ext_type >= 2)
            opl_voix_instrument(ch_hw_voice[c], fm, ch_panning[c]);
          break;
        }
        if (ext_type == MD_EX_SET_PANNING_POS) {
          if (md_channel_is_opl(c)) {
            // ZBx d'AT2 : 1 gauche, 2 droite, toute autre valeur au centre.
            ch_panning[c] = ext_val == 1 ? 0 : (ext_val == 2 ? 255 : 128);
            opl_voix_instrument(ch_hw_voice[c], MD_OPL_FM(&ch_instr_shadow[c]),
                                ch_panning[c]);
          } else {
            // La commande n'a qu'un chiffre : 0 à gauche, 8 au centre, F à
            // droite, sur toute la largeur stéréo.
            ch_panning[c] = (uint8_t)(ext_val * 17);
            md_apply_panning(c);
          }
        }
        break;
      }
      case MD_EFF_EXTENDED2: {
        uint8_t ext_type = eff_val >> 4;
        uint8_t ext_val = eff_val & 0x0F;
        if (ext_type == MD_EX2_NOTE_DELAY) {
          ch_note_delay[c] = ext_val;
        } else if (ext_type == MD_EX2_NOTE_CUT) {
          ch_note_cut[c] = ext_val;
        } else if (ext_type == MD_EX2_FINE_TUNE_UP) {     // &4x : F-Num + x
          ch_base_fnum[c] = md_slide_frequency(ch_base_fnum[c], ext_val);
        } else if (ext_type == MD_EX2_FINE_TUNE_DOWN) {   // &5x : F-Num - x
          ch_base_fnum[c] = md_slide_frequency(ch_base_fnum[c], -ext_val);
        }
        break;
      }
      case MD_EFF_GLOBAL_FSLIDE_UP:
      case MD_EFF_GLOBAL_FSLIDE_DOWN:
        // These are parsed into ch_active_eff for tick processing
        break;
      }
    }

    // Check if Tone Portamento prevented the actual note-on. If so, don't
    // retrigger.
    bool has_tone_porta = false;
    bool has_note_delay = false;
    for (int e = 0; e < MD_EV_EFFETS; e++) {
      if (ev->effects[e].type == MD_EFF_TONE_PORTAMENTO ||
          ev->effects[e].type == MD_EFF_TPORTA_VSLIDE ||
          ev->effects[e].type == MD_EFF_TPORTA_VSLIDE_FINE) {
        has_tone_porta = true;
      }
      if (ev->effects[e].type == MD_EFF_EXTENDED2 &&
          (ev->effects[e].val >> 4) == MD_EX2_NOTE_DELAY) {
        has_note_delay = true;
      }
    }

    // (La colonne CC ne porte aucun effet de séquence : rien à anticiper.)

    if (ev->note > 0 && ev->note <= MD_MAX_NOTE && !has_tone_porta &&
        !has_note_delay && !ch_instr_blocked[c]) {
      md_play_note(c, ev->note);
      // New notes usually reset Arpeggio and tone portamento state in trackers
      // For AT2 specifically, a new note resets continuous effect trackers
      // if they aren't explicitly re-triggered.
      ch_arpeggio_state[c] = 0;
    } else if (ev->note == 0xFF || ev->note == 0x80) {
      if (md_channel_is_pcm(c)) md_chip_pcm_stop();
      md_key_off(c);
      for (int e = 0; e < MD_EFF_SLOTS; e++) {
        ch_active_eff[c][e] = 0xFF;
        ch_active_eff_val[c][e] = 0;
      }
    }

    // ── Commande A : choisir la table jouée par ce canal ──────────────────
    // Elle n'est pas un effet — elle pilote la lecture de table. Il faut donc
    // la lire directement dans la ligne de phrase, et surtout l'appliquer
    // APRÈS le déclenchement de la note : md_key_on() réarme la table de
    // l'instrument et écraserait notre choix.
    if (ch_running[c] && ch_phrase[c] >= 0) {
      const md_phrase_row_t *pr =
          &current_module->phrases[ch_phrase[c] % MD_MAX_PHRASES]
               .rows[ch_phrase_row[c]];
      if (pr->cmd != MD_EMPTY && md_table_cmd_kind(pr->cmd) == MD_CMD_TABLE)
        md_cmd_table_select(c, pr->cmdval);
      // Les anciennes colonnes CC portent maintenant FX1-FX4, lues avec les
      // autres effets (md_channel_event) : plus rien a envoyer ici.
    }
  }

  // ── Avance : chaque canal progresse dans SA colonne ──
  // Position Jump (B) et Pattern Break (C) : dans le modèle LSDJ il n'y a plus
  // de pattern global. B relance TOUS les canaux à la ligne de song demandée,
  // C saute à la ligne de phrase demandée sur le canal qui l'a posée — le plus
  // proche équivalent utile.
  //
  // Dxx, comme AT2 : le pattern s'arrete apres cette ligne et TOUTES les
  // pistes reprennent au pattern suivant — la ligne de SONG suivante — a sa
  // ligne xx, soit la phrase xx/16 du chain, ligne xx%16. Avec un Bxx sur la
  // meme ligne, c'est la ligne de SONG du Bxx. Sur l'ecran CHAIN, le chain
  // reprend en boucle a la ligne xx ; sur l'ecran PHRASE, Dxx ne fait rien.
  if (next_order_index >= 0 && next_order_index < MD_SONG_ROWS &&
      play_scope == MD_SCOPE_SONG) {
    for (int c = 0; c < current_module->num_channels; c++) {
      ch_song_row[c] = next_order_index;
      ch_block_top[c] = md_block_top_row(c, next_order_index);
      ch_chain[c] = -1; ch_chain_row[c] = 0;
      ch_phrase[c] = -1; ch_phrase_row[c] = 0;
      ch_running[c] = true;
      md_channel_seek(c);
      if (next_row_index >= 0) md_va_en_ligne(c, next_row_index);
    }
  } else if (next_row_index >= 0 && play_scope == MD_SCOPE_SONG) {
    for (int c = 0; c < current_module->num_channels; c++) {
      if (!ch_running[c]) continue;
      ch_song_row[c]++;
      ch_chain[c] = -1; ch_chain_row[c] = 0;
      ch_phrase[c] = -1; ch_phrase_row[c] = 0;
      md_channel_seek(c);
      md_va_en_ligne(c, next_row_index);
    }
  } else if (next_row_index >= 0 && play_scope == MD_SCOPE_CHAIN) {
    for (int c = 0; c < current_module->num_channels; c++) {
      if (!ch_running[c] || ch_chain[c] < 0) continue;
      ch_chain_row[c] = (next_row_index / MD_ROWS_PER_PHRASE) % MD_ROWS_PER_CHAIN;
      ch_phrase[c] = -1;
      md_chain_seek_in_scope(c);
      if (ch_phrase[c] >= 0) ch_phrase_row[c] = next_row_index % MD_ROWS_PER_PHRASE;
    }
  } else {
    for (int c = 0; c < current_module->num_channels; c++)
      md_channel_advance(c);
  }

  // Mode LIVE : les canaux armés démarrent maintenant, tous sur la même ligne.
  if (live_mode) {
    for (int c = 0; c < current_module->num_channels; c++) {
      if (ch_pending_row[c] < 0) continue;
      ch_song_row[c] = ch_pending_row[c];
      ch_pending_row[c] = -1;
      ch_chain[c] = -1; ch_chain_row[c] = 0;
      ch_phrase[c] = -1; ch_phrase_row[c] = 0;
      ch_running[c] = true;
      md_channel_seek(c);
    }
  }

  // Ligne affichée par l'UI hérité : celle du premier canal encore en lecture.
  for (int c = 0; c < current_module->num_channels; c++) {
    if (ch_running[c]) { md_current_row = (uint8_t)ch_phrase_row[c]; break; }
  }
}

// Advance the Arpeggio (0xy) effect by one tick on channel `c`: cycles the
// pitch base -> base+x -> base+y while keeping the note keyed-on (no envelope
// retrigger). Used both by the sequencer tick and by the piano preview so the
// preview sounds IDENTICAL to the 0xy command.
// Réécrit la hauteur du canal à partir de sa note courante décalée de
// `semitones`. Partagé par l'effet d'arpège et par la macro d'arpège PSG.
// L'état key-on/off n'est pas touché : la hauteur continue donc de bouger
// pendant la phase de release d'une note relâchée.
// Écrit la hauteur à `semitones` de la note en cours. La transposition de la
// TABLE (colonne TSP) est TOUJOURS comprise dedans : c'est un décalage de la
// voie, pas un événement ponctuel. Sans ça, la moindre commande qui recalculait
// la hauteur — un arpège, un slide, un vibrato — effaçait le TSP posé sur la
// même ligne, et la transposition semblait ne pas marcher.
// Refait la hauteur de référence du canal à partir de sa note et de la
// transposition de table en cours, et l'envoie à la puce. Sans key-on : on
// transpose une note qui sonne, on ne la redéclenche pas.
static void md_rebuild_base_pitch(int c) {
  if (c < 0 || c >= MD_TOTAL_VOICES || ch_current_note[c] == 0)
    return;
  int note = (int)ch_current_note[c] + (int)ch_table_tsp[c];
  if (note < 1) note = 1;
  if (note > MD_MAX_NOTE) note = MD_MAX_NOTE;
  ch_base_fnum[c] = md_hauteur(note, md_ftune_de(c));
  md_write_freq(c, ch_base_fnum[c]);
  md_midi_suit(c, note);
}

static void md_write_note_offset(int c, int semitones) {
  if (c < 0 || c >= MD_TOTAL_VOICES)
    return;
  int note = (int)ch_current_note[c] + semitones + (int)ch_table_tsp[c];
  if (note < 1)
    note = 1;
  if (note > MD_MAX_NOTE)
    note = MD_MAX_NOTE;

  md_write_freq(c, md_hauteur(note, md_ftune_de(c)));
  md_midi_suit(c, note);
}

static void apply_arpeggio_tick(int c) {
  if (c < 0 || c >= MD_TOTAL_VOICES)
    return;
  uint8_t val = ch_arpeggio_val[c];
  if (val == 0)
    return;

  ch_arpeggio_state[c] = (ch_arpeggio_state[c] + 1) % 3;
  int semitones = 0;
  if (ch_arpeggio_state[c] == 1)
    semitones = val >> 4;
  else if (ch_arpeggio_state[c] == 2)
    semitones = val & 0x0F;

  uint8_t note = ch_current_note[c];
  if (note == 0 || note > MD_MAX_NOTE)
    return;

  md_write_note_offset(c, semitones);
}

// Déroule d'une ligne la table de l'instrument du canal, si elle en a une.
// Une ligne par tick, rebouclée tant que la note dure — c'est le mécanisme des
// tables de LSDJ, et il vaut pour les voies FM comme pour les voies PSG.
// Fait franchir au flux `s` tous les H qu'il rencontre, SANS rien jouer.
//
// Un saut ne consomme pas de tick : la ligne qui porte le H n'est jamais
// jouée, et le repère de lecture ne s'y arrête pas non plus. La boucle se
// déroule donc entièrement AU-DESSUS du H.
// Le garde-fou évite de tourner en rond si un H se pointe lui-même.
// Mémorise les paramètres d'un effet posé par une TABLE.
//
// Le moteur d'effets fonctionne en deux temps : la LIGNE mémorise les
// paramètres (vitesse de vibrato, valeur d'arpège…), puis chaque TICK les
// applique. Une table ne passe jamais par le traitement de ligne : sans cette
// fonction, elle armait l'effet sans lui donner ses paramètres, et il ne se
// passait rien. On reproduit donc ici la mémorisation, pour les seules
// commandes qu'une table peut poser.
static void md_table_latch_effect(int c, uint8_t eff, uint8_t val) {
  switch (eff) {
  case MD_EFF_ARPEGGIO:
    if (val) ch_arpeggio_val[c] = val;
    ch_arpeggio_state[c] = 0;
    break;
  case MD_EFF_VIBRATO:
    if (val) {
      if (val >> 4) ch_vibrato_speed[c] = val >> 4;
      if (val & 0x0F) ch_vibrato_depth[c] = val & 0x0F;
    }
    break;
  case MD_EFF_TREMOLO:
    if (val) {
      if (val >> 4) ch_tremolo_speed[c] = val >> 4;
      if (val & 0x0F) ch_tremolo_depth[c] = val & 0x0F;
    }
    break;
  case MD_EFF_TONE_PORTAMENTO:
    if (val) ch_porta_speed[c] = val;
    break;
  case MD_EFF_FSLIDE_UP:
  case MD_EFF_FSLIDE_DOWN:
  case MD_EFF_FSLIDE_UP_FINE:
  case MD_EFF_FSLIDE_DOWN_FINE:
    // Le glissando lit sa vitesse dans ch_fslide_speed : sans cette ligne,
    // l'effet était armé mais glissait de zéro.
    if (val) ch_fslide_speed[c] = val;
    break;
  case MD_EFF_RETRIG_NOTE:
    if (val) ch_retrig_speed[c] = val;
    ch_retrig_count[c] = 0;
    break;
  case MD_EFF_SET_GLOBAL_VOLUME:
    replay_global_vol = (val > 63) ? 63 : val;
    break;
  case MD_EFF_VOL_SLIDE:
  case MD_EFF_TPORTA_VSLIDE:
  case MD_EFF_VIBRATO_VSLIDE:
    if (val) ch_volslide_val[c] = val;
    break;
  case MD_EFF_SET_SPEED:
    if (val > 0) replay_speed = val;
    break;
  case MD_EFF_SET_TEMPO:
    if (val >= 18 && current_module) {
      replay_tempo = val;
      int mul = current_module->macro_speedup > 0 ? current_module->macro_speedup : 1;
      int irq = replay_tempo * mul;
      if (irq == 0) irq = 50;
      samples_per_tick =
          (int)((double)replay_sample_rate / ((double)irq * md_finetune_scale()));
      if (samples_per_tick < 1) samples_per_tick = 1;
    }
    break;
  case MD_EFF_EXTENDED: {
    uint8_t t = val >> 4, v = val & 0x0F;
    if (t == MD_EX_SET_PANNING_POS) {
      // Même échelle que dans une phrase : 0 gauche, 8 centre, F droite.
      ch_panning[c] = (uint8_t)(v * 17);
      md_apply_panning(c);
    }
    break;
  }
  case MD_EFF_EXTENDED2: {
    uint8_t t = val >> 4, v = val & 0x0F;
    if (t == MD_EX2_NOTE_CUT) ch_note_cut[c] = v;
    else if (t == MD_EX2_NOTE_DELAY) ch_note_delay[c] = v;
    break;
  }
  default:
    break;
  }
}

static void md_table_resolve_hops(int c, int s, const md_table_t *tbl) {
  for (int guard = 0; guard < MD_TABLE_ROWS + 1; guard++) {
    int row = ch_table_pos[c][s] & (MD_TABLE_ROWS - 1);
    const md_table_row_t *r = &tbl->rows[row];
    uint8_t cmd = (s == 1) ? r->cmd1 : r->cmd2;
    uint8_t val = (s == 1) ? r->val1 : r->val2;
    if (s == 0 || cmd == MD_EMPTY || md_table_cmd_kind(cmd) != MD_CMD_HOP)
      return;

    // Premier chiffre : combien de fois sauter avant de passer outre
    // (0 = à l'infini). Second chiffre : la ligne visée. Le compteur est
    // indexé par la ligne qui porte le H, ce qui permet d'imbriquer les
    // boucles — chacune compte ses propres tours.
    int times = (val >> 4) & 0x0F;
    int dest = val & 0x0F;
    if (times == 0) {
      ch_table_pos[c][s] = (uint8_t)dest;
    } else if (ch_hop_left[c][s][row] < times) {
      ch_hop_left[c][s][row]++;
      ch_table_pos[c][s] = (uint8_t)dest;
    } else {
      ch_hop_left[c][s][row] = 0;   // boucle terminée
      ch_table_pos[c][s] = (uint8_t)((row + 1) & (MD_TABLE_ROWS - 1));
    }
  }
}

static void md_table_tick(int c) {
  if (c < 0 || c >= MD_TOTAL_VOICES || !current_module)
    return;
  if (!ch_table_running[c])
    return;
  int t = ch_table[c];
  if (t < 0 || t >= MD_MAX_TABLES) {
    ch_table_running[c] = false;
    return;
  }
  const md_table_t *tbl = &current_module->tables[t];

  // Les trois flux avancent chacun pour leur compte : c'est ce qui permet à un
  // H de faire boucler la colonne TSP pendant que la seconde CMD continue.
  for (int s = 0; s < MD_TABLE_STREAMS; s++) {
    md_table_resolve_hops(c, s, tbl);
    int row = ch_table_pos[c][s] & (MD_TABLE_ROWS - 1);
    const md_table_row_t *r = &tbl->rows[row];

    if (s == 0) {
      // ── Flux 0 : VOL ────────────────────────────────────────────────────
      // 0 = on n'y touche pas.
      if (r->vol != 0) {
        static const uint8_t vol_to_atten[256] = {
        63, 32, 28, 26, 24, 23, 22, 21, 20, 19, 19, 18, 18, 17, 17, 16,
        16, 16, 15, 15, 15, 14, 14, 14, 14, 13, 13, 13, 13, 12, 12, 12,
        12, 12, 12, 11, 11, 11, 11, 11, 11, 10, 10, 10, 10, 10, 10, 10,
        10,  9,  9,  9,  9,  9,  9,  9,  9,  9,  9,  8,  8,  8,  8,  8,
         8,  8,  8,  8,  8,  8,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,
         7,  7,  7,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
         6,  6,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,
         5,  5,  5,  5,  5,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
         4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  3,  3,  3,  3,  3,
         3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
         3,  3,  3,  3,  3,  3,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
         2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,
         2,  2,  2,  2,  2,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
         1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
         1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
        };
        ch_fmpar[c].volC = vol_to_atten[r->vol];
        md_write_volume(c);
      }
    } else {
      // ── Flux 1 : TSP + première CMD · Flux 2 : seconde CMD ──────────────
      if (s == 1 && r->transpose != ch_table_tsp[c]) {
        // On ne réécrit la hauteur que lorsque la transposition CHANGE, sinon
        // on écraserait à chaque tick le travail des slides et du vibrato.
        //
        // Elle est reportée dans la hauteur de RÉFÉRENCE : c'est celle que le
        // moteur restaure au début de chaque ligne. En se contentant d'écrire
        // la hauteur du moment, le TSP était effacé à la ligne suivante dès
        // qu'une commande l'accompagnait.
        ch_table_tsp[c] = r->transpose;
        md_rebuild_base_pitch(c);
      }

      // La colonne MD voyage avec le second flux : c'est un réglage ponctuel,
      // il n'a pas besoin d'un flux à lui.
      if (s == 2 && r->mdcmd != MD_EMPTY)
        md_apply_mdcmd(c, r->mdcmd, r->mdval, r->cmd2, MD_EFF_SLOT_TABLE_MD);

      uint8_t cmd = (s == 1) ? r->cmd1 : r->cmd2;
      uint8_t val = (s == 1) ? r->val1 : r->val2;
      int eff = -1;
      uint8_t rval = 0;
      if (cmd != MD_EMPTY)
        md_table_cmd_resolve(cmd, val, &eff, &rval);

      // A : bascule vers une autre table (ou arrête la lecture de table).
      if (cmd != MD_EMPTY && md_table_cmd_kind(cmd) == MD_CMD_TABLE) {
        md_cmd_table_select(c, val);
        return;   // la table a changé sous nos pieds : on s'arrête ici
      }

      // Les commandes partent vers le moteur d'effets, dans l'emplacement
      // réservé à ce flux.
      //
      // Une ligne SANS commande ne désarme rien : la commande posée plus haut
      // continue d'agir, comme dans LSDJ. Sans ça, un retrig armé une ligne sur
      // seize voyait son compteur remis à zéro à chaque tour et n'atteignait
      // jamais son intervalle. Pour arrêter une commande, on la réécrit avec la
      // valeur 00.
      int slot = MD_EFF_SLOT_TABLE + (s - 1);
      if (cmd != MD_EMPTY) {
        uint8_t e = (eff < 0) ? 0xFF : (uint8_t)eff;
        uint8_t v = (eff < 0) ? 0 : rval;
        // Une valeur 00 ARRÊTE un effet continu, dans une table comme dans une
        // phrase — sinon les paramètres mémorisés le relancent à la vitesse
        // d'avant.
        if (eff >= 0 && v == 0 && md_effect_is_continuous(e)) {
          ch_active_eff[c][slot] = 0xFF;
          ch_active_eff_val[c][slot] = 0;
          ch_table_pos[c][s] = (uint8_t)((row + 1) & (MD_TABLE_ROWS - 1));
          md_table_resolve_hops(c, s, tbl);
          continue;
        }
        // On ne réarme que ce qui change, pour ne pas relancer à chaque tick un
        // effet continu comme le vibrato.
        if (ch_active_eff[c][slot] != e || ch_active_eff_val[c][slot] != v) {
          ch_active_eff[c][slot] = e;
          ch_active_eff_val[c][slot] = v;
          if (eff >= 0)
            md_table_latch_effect(c, e, v);  // sans ça, l'effet n'a rien à jouer
        }
      }
    }

    ch_table_pos[c][s] = (uint8_t)((row + 1) & (MD_TABLE_ROWS - 1));
    // On franchit tout de suite un éventuel H : le repère ne doit jamais
    // s'arrêter sur la ligne qui le porte.
    md_table_resolve_hops(c, s, tbl);
  }
}

static void md_process_tick() {
  if (replay_ticks_left == 0) {
    md_process_row();
    replay_ticks_left = replay_speed;

    // Restore base frequency and volume for all channels in case the previous
    // row had temporary tick effects (Arpeggio, Vibrato, Tremolo, Tremor) that
    // normally do not persist into the next empty row.
    if (current_module) {
      for (int c = 0; c < current_module->num_channels; ++c) {
        md_write_freq(c, ch_base_fnum[c]);
        md_write_volume(c);
        // Une voie MIDI revient elle aussi à sa note de référence.
        if (ch_current_note[c])
          md_midi_suit(c, (int)ch_current_note[c] + (int)ch_table_tsp[c]);
      }
    }
  } else {
    // Process continuous effects (ticks 1 to speed-1)
    if (current_module) {
      int current_tick = replay_speed - replay_ticks_left;

      // Les tables d'instrument avancent d'une ligne, avant le traitement des
      // effets : ce qu'elles posent dans leurs emplacements est donc pris en
      // compte dès ce tick-ci.
      for (int c = 0; c < MD_TOTAL_VOICES; ++c)
        md_table_tick(c);

      // Pre-tick Global Effects Accumulation
      int8_t tick_gl_volslide = 0;
      int16_t tick_gl_fslide =
          0; // Renamed from temposlide to match usage below

      for (int c = 0; c < current_module->num_channels; ++c) {
        for (int e = 0; e < MD_EFF_SLOTS; e++) {
          uint8_t eff = ch_active_eff[c][e];
          uint8_t val = ch_active_eff_val[c][e];
          if (eff == MD_EFF_GLOBAL_FSLIDE_UP)
            tick_gl_fslide -= val; // logic uses - for UP
          else if (eff == MD_EFF_GLOBAL_FSLIDE_DOWN)
            tick_gl_fslide += val;
          else if (eff == MD_EFF_EXTENDED2) {
            uint8_t ext_t = val >> 4;
            uint8_t ext_v = val & 0x0F;
            if (ext_t == MD_EX2_GL_VOL_SLIDE_UP)
              tick_gl_volslide += ext_v;
            else if (ext_t == MD_EX2_GL_VOL_SLIDE_DN)
              tick_gl_volslide -= ext_v;
            else if (ext_t == MD_EX2_GL_VOL_SLIDE_UP_F && current_tick == 0)
              tick_gl_volslide += ext_v;
            else if (ext_t == MD_EX2_GL_VOL_SLIDE_DN_F && current_tick == 0)
              tick_gl_volslide -= ext_v;
            else if (ext_t == MD_EX2_GL_VOL_SLIDE_UP_XF) {
              // Extra Fine: applies every 4 ticks or similar?
              // Actually, in AT2 XF is / 4 per tick.
              if ((current_tick & 3) == 0)
                tick_gl_volslide += ext_v;
            } else if (ext_t == MD_EX2_GL_VOL_SLIDE_DN_XF) {
              if ((current_tick & 3) == 0)
                tick_gl_volslide -= ext_v;
            }
          }
        }
      }

      // Apply accumulated global slides
      if (tick_gl_volslide != 0) {
        int new_vol = (int)replay_global_vol + tick_gl_volslide;
        if (new_vol > 63)
          new_vol = 63;
        if (new_vol < 0)
          new_vol = 0;
        replay_global_vol = (uint8_t)new_vol;
      }
      if (tick_gl_fslide != 0) {
        int new_tempo = (int)replay_tempo + tick_gl_fslide;
        if (new_tempo < 18)
          new_tempo = 18;
        if (new_tempo > 255)
          new_tempo = 255;
        replay_tempo = (uint8_t)new_tempo;
        // Update irq timings
        int irq_freq = replay_tempo * (current_module->macro_speedup > 0
                                           ? current_module->macro_speedup
                                           : 1);
        if (irq_freq == 0)
          irq_freq = 50;
        samples_per_tick = (int)((double)replay_sample_rate / ((double)irq_freq * md_finetune_scale()));
        if (samples_per_tick < 1) samples_per_tick = 1;
      }

      for (int c = 0; c < current_module->num_channels; ++c) {
        // Les QUATRE emplacements : 0-1 pour la phrase, 2-3 pour les deux
        // colonnes CMD de la table. Les oublier revenait à ignorer toutes les
        // commandes posées dans une table.
        for (int e = 0; e < MD_EFF_SLOTS; ++e) {
          uint8_t eff = ch_active_eff[c][e];
          if (eff == 0xFF)
            continue;

          // ── Décomposition des effets COMBINÉS AT2 ──────────────────────────
          // Un combo « X + volume slide » fait tourner DEUX comportements : le
          // volume slide (nibbles du paramètre) et X (freq slide / arpège /
          // porta / vibrato, dont la vitesse vient de la MÉMOIRE, pas du param).
          //   • vsl_cont / vsl_fine : volume slide continu (tick>0) ou fin (tick0)
          //   • fsl_*_cont / fsl_*_fine : freq slide montant/descendant, continu/fin
          bool vsl_cont = (eff == MD_EFF_VOL_SLIDE || eff == MD_EFF_VIBRATO_VSLIDE ||
                           eff == MD_EFF_TPORTA_VSLIDE || eff == MD_EFF_ARPEGGIO_VSLIDE ||
                           eff == MD_EFF_FSLIDE_UP_VSLIDE || eff == MD_EFF_FSLIDE_DOWN_VSLIDE ||
                           eff == MD_EFF_FSLIDE_UP_FINE_VSLIDE ||
                           eff == MD_EFF_FSLIDE_DOWN_FINE_VSLIDE);
          bool vsl_fine = (eff == MD_EFF_VOL_SLIDE_FINE || eff == MD_EFF_ARPEGGIO_VSLIDE_FINE ||
                           eff == MD_EFF_TPORTA_VSLIDE_FINE || eff == MD_EFF_VIBRATO_VSLIDE_FINE ||
                           eff == MD_EFF_FSLIDE_UP_VSLF || eff == MD_EFF_FSLIDE_DOWN_VSLF ||
                           eff == MD_EFF_FSLIDE_UP_FINE_VSLF || eff == MD_EFF_FSLIDE_DOWN_FINE_VSLF);
          bool fsl_up_cont = (eff == MD_EFF_FSLIDE_UP || eff == MD_EFF_FSLIDE_UP_VSLIDE ||
                              eff == MD_EFF_FSLIDE_UP_VSLF);
          bool fsl_dn_cont = (eff == MD_EFF_FSLIDE_DOWN || eff == MD_EFF_FSLIDE_DOWN_VSLIDE ||
                              eff == MD_EFF_FSLIDE_DOWN_VSLF);
          bool fsl_up_fine = (eff == MD_EFF_FSLIDE_UP_FINE || eff == MD_EFF_FSLIDE_UP_FINE_VSLIDE ||
                              eff == MD_EFF_FSLIDE_UP_FINE_VSLF);
          bool fsl_dn_fine = (eff == MD_EFF_FSLIDE_DOWN_FINE || eff == MD_EFF_FSLIDE_DOWN_FINE_VSLIDE ||
                              eff == MD_EFF_FSLIDE_DOWN_FINE_VSLF);

          if (eff == MD_EFF_RETRIG_NOTE) {
            uint8_t speed = ch_retrig_speed[c];
            if (speed > 0) {
              ch_retrig_count[c]++;
              if (ch_retrig_count[c] >= speed) {
                ch_retrig_count[c] = 0;
                md_key_off(c);
                md_key_on(c);
              }
            }
          }

          // Multi retrigger note (effet 26) — spec AdlibTracker2 ef_MultiRetrigNote :
          //   • intervalle de retrig = nibble HAUT du paramètre
          //   • opération de volume  = nibble BAS (table 0..15)
          // Implémentation maison (aucune ligne copiée d'AT2). volC&0x3F est une
          // ATTÉNUATION (0 = fort, 63 = silence) → on travaille en "loudness"
          // (loud = 63 - atténuation) pour appliquer +/- et les ratios.
          if (eff == MD_EFF_MULTI_RETRIG_NOTE) {
            uint8_t param = ch_active_eff_val[c][e];
            uint8_t interval = param >> 4;
            uint8_t vol_op = param & 0x0F;
            if (interval > 0) {
              ch_retrig_count[c]++;
              if (ch_retrig_count[c] >= interval) {
                ch_retrig_count[c] = 0;

                // Applique l'opération de volume sur la porteuse.
                int loud = 63 - (ch_fmpar[c].volC & 0x3F);
                switch (vol_op) {
                  case 1:  loud -= 1;  break;
                  case 2:  loud -= 2;  break;
                  case 3:  loud -= 4;  break;
                  case 4:  loud -= 8;  break;
                  case 5:  loud -= 16; break;
                  case 9:  loud += 1;  break;
                  case 10: loud += 2;  break;
                  case 11: loud += 4;  break;
                  case 12: loud += 8;  break;
                  case 13: loud += 16; break;
                  case 6:  loud = (loud * 2) / 3; break;
                  case 7:  loud = loud / 2;       break;
                  case 14: loud = (loud * 3) / 2; break;
                  case 15: loud = loud * 2;       break;
                  default: break; // 0 et 8 : pas de changement
                }
                if (loud < 0) loud = 0;
                if (loud > 63) loud = 63;
                ch_fmpar[c].volC = (ch_fmpar[c].volC & 0xC0) | (uint8_t)(63 - loud);

                // Réécrit le volume, puis retrigger (key off → key on).
                md_write_volume(c);
                md_key_off(c);
                md_key_on(c);
              }
            }
          }

          if (eff == MD_EFF_EXTENDED2) {
            uint8_t ext_type = ch_active_eff_val[c][e] >> 4;
            uint8_t ext_val = ch_active_eff_val[c][e] & 0x0F;
            if (ext_type == MD_EX2_NOTE_CUT && current_tick == ext_val) {
              md_key_off(c); // note cut
            }
            if (ext_type == MD_EX2_NOTE_DELAY && current_tick == ext_val) {
              md_event_t ev_tick = md_channel_event(c);
              if (ev_tick.note > 0 && ev_tick.note <= MD_MAX_NOTE) {
                md_play_note(c, ev_tick.note);
              }
            }
          }

          if (vsl_cont || vsl_fine) {
            uint8_t val = ch_volslide_val[c];
            uint8_t up = val >> 4;
            uint8_t down = val & 0x0F;

            // Fine Volume Slide ONLY on tick 0, Standard Volume Slide ONLY on
            // tick > 0
            bool should_process = vsl_fine ? (current_tick == 0)
                                           : (current_tick > 0);

            if (!should_process) {
              up = 0;
              down = 0;
            }

            uint8_t current_vol = ch_fmpar[c].volC & 0x3F;
            if (up > 0) {
              if (current_vol >= up)
                current_vol -= up;
              else
                current_vol = 0;
            } else if (down > 0) {
              current_vol += down;
              if (current_vol > 63)
                current_vol = 63;
            }
            ch_fmpar[c].volC = (ch_fmpar[c].volC & 0xC0) | current_vol;

            md_write_volume(c);
          }

          if (fsl_up_cont || fsl_dn_cont || fsl_up_fine || fsl_dn_fine) {
            uint8_t speed = ch_fslide_speed[c];
            bool is_up = fsl_up_cont || fsl_up_fine;
            bool is_fine = fsl_up_fine || fsl_dn_fine;

            // Fine slides ONLY on tick 0, Standard slides ONLY on tick > 0
            bool should_process = is_fine ? (current_tick == 0)
                                          : (current_tick > 0);

            if (should_process) {
              // The F-Slide speed parameter in AT2 is added exactly linearly
              // per tick
              int16_t scaled_speed = speed;

              uint16_t current_fnum = ch_base_fnum[c];
              if (is_up) {
                // AT2 UP means increasing frequency number
                current_fnum = md_slide_frequency(current_fnum, scaled_speed);
              } else {
                // AT2 DOWN means decreasing frequency number
                current_fnum =
                    md_slide_frequency(current_fnum, -scaled_speed);
              }
              ch_base_fnum[c] = current_fnum;
            }
            uint16_t out_fnum = ch_base_fnum[c];
            // potential modification

            md_write_freq(c, out_fnum);
          }

          if (eff == MD_EFF_TONE_PORTAMENTO || eff == MD_EFF_TPORTA_VSLIDE ||
              eff == MD_EFF_TPORTA_VSLIDE_FINE) {
            uint8_t speed = ch_porta_speed[c];
            uint16_t current_fnum = ch_base_fnum[c];
            uint16_t target = ch_porta_target[c];

            // AT2 slides use absolute shift comparisons natively supported by
            // `md_slide_frequency`
            int16_t scaled_speed = speed;

            if (current_fnum > target) {
              // Current Pitch > Target Pitch, go UP (negative speed for F-num)
              current_fnum = md_slide_frequency(current_fnum, -scaled_speed);
              if (current_fnum < target)
                current_fnum = target;
            } else if (current_fnum < target) {
              // Current Pitch < Target Pitch, go DOWN (positive speed for
              // F-num)
              current_fnum = md_slide_frequency(current_fnum, scaled_speed);
              if (current_fnum > target)
                current_fnum = target;
            }
            ch_base_fnum[c] = current_fnum;

            uint16_t out_fnum = current_fnum;

            md_write_freq(c, out_fnum);
          }

          if (eff == MD_EFF_VIBRATO || eff == MD_EFF_VIBRATO_VSLIDE ||
              eff == MD_EFF_VIBRATO_VSLIDE_FINE) {
            uint8_t speed = ch_vibrato_speed[c];
            uint8_t depth = ch_vibrato_depth[c];
            ch_vibrato_pos[c] += speed;

            uint8_t pos = ch_vibrato_pos[c];
            uint8_t table_val = vibtrem_table[pos & 31];
            uint8_t dir = (pos & 32) ? 1 : 0;

            uint16_t shift = (depth * table_val) >> 7;

            uint16_t fnum = ch_base_fnum[c];
            if (dir == 0) {
              fnum = md_slide_frequency(fnum, -(int16_t)shift);
            } else {
              fnum = md_slide_frequency(fnum, (int16_t)shift);
            }

            md_write_freq(c, fnum);
          }

          if (eff == MD_EFF_TREMOLO) {
            uint8_t speed = ch_tremolo_speed[c];
            uint8_t depth = ch_tremolo_depth[c];
            ch_tremolo_pos[c] += speed;

            uint8_t pos = ch_tremolo_pos[c];
            uint8_t table_val = vibtrem_table[pos & 31];
            uint8_t dir = (pos & 32) ? 1 : 0;

            uint16_t shift = (depth * table_val) >> 7;

            uint8_t current_vol = ch_fmpar[c].volC & 0x3F;
            if (dir == 0) { // Vol down -> Attenuation UP
              current_vol += shift;
              if (current_vol > 63)
                current_vol = 63;
            } else {
              if (current_vol >= shift)
                current_vol -= shift;
              else
                current_vol = 0;
            }

            md_write_carrier_volume(c, current_vol);
          }

          if (eff == MD_EFF_TREMOR) {
            uint8_t val = ch_tremor_val[c];
            uint8_t on_time = val >> 4;
            uint8_t off_time = val & 0x0F;

            if (on_time > 0 && off_time > 0) {
              ch_tremor_count[c]++;
              if (ch_tremor_count[c] >= on_time + off_time)
                ch_tremor_count[c] = 0;

              if (ch_tremor_count[c] >= on_time) {
                md_write_carrier_volume(c, 63); // phase OFF : silence
              } else {
                md_write_volume(c);             // phase ON : volume normal
              }
            }
          }

          if (eff == MD_EFF_ARPEGGIO || eff == MD_EFF_ARPEGGIO_VSLIDE ||
              eff == MD_EFF_ARPEGGIO_VSLIDE_FINE) {
            apply_arpeggio_tick(c);
          }
        }
      }
    }
  }

  // Process instrument macros every tick
  md_psg_env_tick();

  replay_ticks_left--;
}

bool md_replayer_play() {
  md_lock(&replayer_lock);

  if (!current_module) {
    md_unlock(&replayer_lock);
    return false;
  }
  // Chaque canal repart de la ligne de song demandée (F5 = 0, F8 = curseur),
  // puis suit SA propre colonne.
  memset(ch_table_forced, 0, sizeof(ch_table_forced));
  for (int c = 0; c < MD_MAX_CHANNELS; c++) {
    ch_song_row[c] = song_start_row;
    ch_chain[c] = -1;
    ch_chain_row[c] = 0;
    ch_phrase[c] = -1;
    ch_phrase_row[c] = 0;
    ch_row_transpose[c] = 0;
    ch_pending_row[c] = -1;

    if (play_scope != MD_SCOPE_SONG) {
      // Lecture limitée : seule la colonne du curseur sonne, et le mode LIVE
      // ne s'applique pas — on veut entendre immédiatement ce qu'on édite.
      if (c != scope_channel) {
        ch_running[c] = false;
        continue;
      }
      ch_running[c] = true;
      if (play_scope == MD_SCOPE_PHRASE) {
        ch_phrase[c] = scope_id;
        // Le TSP de la ligne de chain d'où l'on est descendu, si elle joue bien
        // cette phrase : on l'entend transposée comme dans le morceau.
        ch_row_transpose[c] =
            (scope_tsp_chain >= 0 &&
             current_module->chains[scope_tsp_chain].rows[scope_tsp_row].phrase == scope_id)
                ? current_module->chains[scope_tsp_chain].rows[scope_tsp_row].transpose : 0;
      } else {
        ch_chain[c] = scope_id;
        ch_chain_row[c] = (scope_start_row >= 0 &&
                           scope_start_row < MD_ROWS_PER_CHAIN)
                              ? scope_start_row : 0;
        md_chain_seek_in_scope(c);
      }
      continue;
    }

    // En LIVE, rien ne démarre tout seul : chaque colonne est lancée à la main.
    *WEB_PTR(&ch_running[c]) = !live_mode;
    if (!ch_running[c]) continue;

    // La lecture ENTRE sur la ligne du curseur, mais son bloc — donc sa boucle
    // — commence en haut du bloc contigu qui la contient. Et si la case du
    // curseur est vide pour ce canal, ce canal-là ne joue rien du tout.
    ch_block_top[c] = md_block_top_row(c, song_start_row);
    if (song_start_row < 0 || song_start_row >= MD_SONG_ROWS ||
        current_module->song[c][song_start_row] == MD_EMPTY) {
      ch_running[c] = false;
      ch_phrase[c] = -1;
      continue;
    }
    md_channel_seek(c);
  }
  {
    // Trace de démarrage : d'où part chaque colonne, et sur quel bloc.
    MD_TRACE("[PLAY] moteur: portee=%d song_start_row=%d\n", play_scope, song_start_row);
    for (int c = 0; c < current_module->num_channels; c++)
      MD_TRACE("[PLAY]   canal %d : case=%02X hautDeBloc=%d ligne=%d chaine=%d "
             "phrase=%d actif=%d\n",
             c,
             (song_start_row >= 0 && song_start_row < MD_SONG_ROWS)
                 ? current_module->song[c][song_start_row] : 0xFF,
             ch_block_top[c], ch_song_row[c], ch_chain[c], ch_phrase[c],
             ch_running[c] ? 1 : 0);
    fflush(stdout);
  }
  md_current_row = 0;

  replay_speed = current_module->initial_speed;
  replay_tempo = current_module->initial_tempo;
  if (replay_tempo < 18)
    replay_tempo = 18;

  int irq_freq = replay_tempo * current_module->macro_speedup;
  if (irq_freq == 0)
    irq_freq = 50; // Fallback

  samples_per_tick = (int)((double)replay_sample_rate / ((double)irq_freq * md_finetune_scale()));
        if (samples_per_tick < 1) samples_per_tick = 1;
  if (samples_per_tick < 1) samples_per_tick = 1;
  samples_until_next_tick = 0;
  replay_ticks_left = 0; // Trigger row immediately

  play_status = MD_PLAYING;

  // Silence complet — voies de la DS et notes MIDI —, puis l'horloge repart
  // d'un tick entier pour que la première ligne tombe juste.
  md_ds_silence();
  memset(midi_programme, 0xFF, sizeof(midi_programme));
  memset(psg_env, 0, sizeof(psg_env));
  md_ds_horloge_repart();

  memset(ch_b0_reg, 0, sizeof(ch_b0_reg));
  memset(ch_panning, 128, sizeof(ch_panning));   // centre
  memset(ch_vel_fin, 127, sizeof(ch_vel_fin));

  // Reset persistent effect states so they don't leak into new playbacks
  memset(ch_fslide_speed, 0, sizeof(ch_fslide_speed));
  memset(ch_porta_speed, 0, sizeof(ch_porta_speed));
  memset(ch_porta_target, 0, sizeof(ch_porta_target));
  memset(ch_vibrato_speed, 0, sizeof(ch_vibrato_speed));
  memset(ch_vibrato_depth, 0, sizeof(ch_vibrato_depth));
  memset(ch_vibrato_pos, 0, sizeof(ch_vibrato_pos));
  memset(ch_arpeggio_val, 0, sizeof(ch_arpeggio_val));
  memset(ch_arpeggio_state, 0, sizeof(ch_arpeggio_state));
  memset(ch_volslide_val, 0, sizeof(ch_volslide_val));
  memset(ch_active_eff_val, 0, sizeof(ch_active_eff_val));
  memset(ch_active_eff, 0xFF, sizeof(ch_active_eff));
  memset(ch_mdcmd_on, 0, sizeof(ch_mdcmd_on));
  // Le volume GLOBAL revient au maximum. Il n'était remis qu'au démarrage de
  // l'application : une commande M survolée en éditant — M 00 met tout à zéro —
  // assourdissait donc TOUTES les voies, et aucun stop/play n'y pouvait rien.
  replay_global_vol = 63;
  memset(ch_instr_blocked, 0, sizeof(ch_instr_blocked));
  memset(ch_mdcmd_val, 0, sizeof(ch_mdcmd_val));

  // Le volume du canal fait partie de cet état-là. Sans cette ligne, un
  // glissando de volume descendu jusqu'au silence n'était JAMAIS relevé :
  // ni par un stop, ni par un nouveau morceau, ni même par un redémarrage du
  // moteur — ch_fmpar n'était remis à zéro nulle part. Le tracker restait
  // muet jusqu'à la fermeture de l'application.
  memset(ch_fmpar, 0, sizeof(ch_fmpar));

  global_volslide_val = 0;
  global_fslide_speed = 0;

  md_unlock(&replayer_lock);
  return true;
}

void md_replayer_stop() {
  md_lock(&replayer_lock);
  play_status = MD_STOPPED;
  md_current_row = 0;
  for (int c = 0; c < MD_MAX_CHANNELS; c++) {
    ch_running[c] = false;
    ch_phrase[c] = -1;
    ch_chain[c] = -1;
    ch_pending_row[c] = -1;
  }

  // Toutes les voies se taisent et toutes les notes MIDI s'éteignent,
  // audition comprise.
  for (int c = 0; c < MD_TOTAL_VOICES; c++) {
    md_key_off(c);
  }
  // Les arrêts de voies partent MAINTENANT, retard compris : sinon la file
  // continuerait de lâcher des notes après l'arrêt.
  ds_voix_retard_actif(false);
  ds_voix_envoie();
  ds_voix_purge();
  md_unlock(&replayer_lock);
}

// ── L'horloge du séquenceur ────────────────────────────────────────────────
// Le timer 0 de l'ARM9 bat une fois par tick. Sa période se déduit de
// samples_per_tick, compté sur l'horloge virtuelle : un tick dure
// samples_per_tick / MD_HORLOGE_VIRTUELLE secondes. Le séquenceur ne dépend
// donc plus de la boucle d'affichage : une note part au tick près, quoi que
// l'écran soit en train de faire.
#define DS_HORLOGE_BUS 33513982u
static int ds_diviseur = -1;
static int ds_spt_programme = -1;

static void md_ds_horloge(void) {
  const int spt = samples_per_tick > 0 ? samples_per_tick : replay_sample_rate / 50;
  // RG350 : la periode exacte, sans l'arrondi du timer de la DS.
  rg_regle_periode_tick((double)spt / (double)replay_sample_rate);
  if (spt == ds_spt_programme) return;
  ds_spt_programme = spt;

  // Le diviseur le plus fin qui laisse la période tenir sur 16 bits.
  static const struct { int diviseur; uint32_t cadence; } k[3] = {
    { ClockDivider_64,   DS_HORLOGE_BUS / 64 },
    { ClockDivider_256,  DS_HORLOGE_BUS / 256 },
    { ClockDivider_1024, DS_HORLOGE_BUS / 1024 },
  };
  uint64_t n = 0;
  int d = 0;
  for (; d < 3; d++) {
    n = ((uint64_t)k[d].cadence * (uint64_t)spt + (uint64_t)replay_sample_rate / 2)
        / (uint64_t)replay_sample_rate;
    if (n <= 0xFFFF) break;
  }
  if (d == 3) { d = 2; n = 0xFFFF; }
  if (n < 1) n = 1;

  const u16 recharge = (u16)(0x10000u - (uint32_t)n);
  if (k[d].diviseur != ds_diviseur) {
    TIMER_CR(0) = 0;
    TIMER_DATA(0) = recharge;
    TIMER_CR(0) = TIMER_ENABLE | TIMER_IRQ_REQ | k[d].diviseur;
    ds_diviseur = k[d].diviseur;
  } else {
    TIMER_DATA(0) = recharge;   // pris en compte au prochain débordement
  }
}

static void md_ds_horloge_repart(void) {
  ds_diviseur = -1;
  ds_spt_programme = -1;
  md_ds_horloge();
}

// Toutes les voies et toutes les notes MIDI se taisent.
static void md_ds_silence(void) {
  for (int c = 0; c < MD_TOTAL_VOICES; c++) {
    ds_voix_stop(ch_hw_voice[c]);
    md_midi_note_off(c);
  }
  ds_voix_envoie();
}

// Un tick, sous l'interruption du timer. Les interruptions sont déjà coupées :
// la boucle principale ne peut pas toucher au moteur pendant ce temps.
// ── Relâcher les notes MIDI ─────────────────────────────────────────────────
// Un tracker façon LSDJ n'écrit pas de note off. Mais un synthé monophonique
// qui reçoit la note suivante SANS avoir vu la touche se relever joue legato :
// son enveloppe ne redémarre pas, et on n'entend aucune attaque.
//
// LA RÈGLE : une note MIDI est relâchée À LA FIN DE SA PROPRE LIGNE, comme un
// doigt qui se lève. Elle ne tient plus longtemps que dans deux cas :
//   — un « OFF » est écrit plus loin dans la phrase, avant la prochaine note :
//     c'est LUI qui relâche, et la note tient jusque-là ;
//   — la note suivante porte un portamento, qui glisse sans redéclencher.
// LENGTH, sur l'instrument, permet en plus de raccourcir à volonté.
//
// Juste après une ligne, chaque piste pointe déjà sur la ligne qui suit : au
// dernier tick, la phrase se lit à partir de là sans rien avoir à simuler.

// Un « OFF » est-il écrit avant la prochaine note, sur cette piste ?
static bool md_off_attendu(int c) {
  if (!current_module || !ch_running[c] || ch_phrase[c] < 0)
    return false;
  const md_phrase_t *ph =
      &current_module->phrases[ch_phrase[c] % MD_MAX_PHRASES];
  for (int r = ch_phrase_row[c]; r < MD_ROWS_PER_PHRASE; r++) {
    const uint8_t n = ph->rows[r].note;
    if (n == MD_EMPTY) return true;                 // « OFF » : il s'en charge
    if (n >= 1 && n <= MD_MAX_NOTE) return false;   // une note : on relâche
  }
  return false;
}

static void md_midi_relache_avant(void) {
  for (int c = 0; c < current_module->num_channels; c++) {
    if (ch_midi_note[c] == 0xFF || md_channel_disabled[c]) continue;
    if (md_off_attendu(c)) continue;
    // Un portamento glisse vers la note suivante sans la redéclencher.
    const md_event_t ev = md_channel_event(c);
    if (ev.note > 0 && ev.note <= MD_MAX_NOTE) {
      bool porta = false;
      for (int e = 0; e < MD_EV_EFFETS; e++)
        if (ev.effects[e].type == MD_EFF_TONE_PORTAMENTO ||
            ev.effects[e].type == MD_EFF_TPORTA_VSLIDE ||
            ev.effects[e].type == MD_EFF_TPORTA_VSLIDE_FINE)
          porta = true;
      if (porta) continue;
    }
    md_midi_note_off(c);
  }
}

// LENGTH : une note MIDI se relâche d'elle-même au bout de ce nombre de ticks.
// Compté AVANT le tick courant, pour qu'une note partie à ce tick dure bien
// ses LENGTH ticks entiers.
static void md_midi_longueurs(void) {
  for (int c = 0; c < MD_TOTAL_VOICES; c++) {
    if (ch_midi_note[c] == 0xFF) continue;
    const uint8_t lg = ch_instr_shadow[c].midi_length;
    if (lg && ch_midi_ticks[c] < 255 && ++ch_midi_ticks[c] >= lg)
      md_midi_note_off(c);
  }
}

static void md_ds_irq(void) {
  if (current_module) {
    // Pendant la lecture, les sorties sont datées de l'heure du tick (et le
    // retard SYNC s'y ajoute) : une note d'audition, elle, part sans attendre.
    ds_voix_retard_actif(play_status == MD_PLAYING);
    md_midi_longueurs();
    if (play_status == MD_PLAYING) {
      md_process_tick();
      // Le prochain appel lira une ligne : c'est le moment de relâcher. Pas à
      // la vitesse 1, où chaque tick est une ligne — la note n'aurait pas duré.
      if (replay_ticks_left == 0 && replay_speed > 1)
        md_midi_relache_avant();
    } else {
      // À l'arrêt, les tables et l'arpège d'audition tournent quand même :
      // c'est ce qui permet d'entendre la table qu'on édite en jouant une note.
      md_psg_env_tick();
      for (int c = 0; c < MD_TOTAL_VOICES; c++)
        md_table_tick(c);
      if (test_arp_channel >= 0)
        apply_arpeggio_tick(test_arp_channel);
    }
  }
  // Les macros FM APRES la ligne, comme le macro_poll_proc d'AT2 : une note
  // qui part a ce tick joue deja le premier pas de sa macro.
  if (current_module)
    for (int c = 0; c < MD_TOTAL_VOICES; c++) md_opl_macro_tick(c);
  ds_voix_envoie();
  xt_midi_irq();
  md_ds_horloge();   // une commande T a pu changer le tempo pendant ce tick
}

void md_replayer_ds_demarre(void) {
  irqSet(IRQ_TIMER0, md_ds_irq);
  irqEnable(IRQ_TIMER0);
  md_ds_horloge_repart();
}

    const char *md_replayer_get_instr_name(int index) {
      if (!current_module || index < 1 || index > MD_MAX_INSTRUMENTS)
        return "";
      return current_module->instr_names[index - 1];
    }

    void md_replayer_set_instr_name(int index, const char *name) {
      if (!current_module || index < 1 || index > MD_MAX_INSTRUMENTS || !name)
        return;
      strncpy(current_module->instr_names[index - 1], name, 42);
      current_module->instr_names[index - 1][42] = '\0';
    }

    // Recopie un instrument dans un autre emplacement, réglages ET nom. Sert au
    // clonage profond (SELECT+B+A) : dupliquer un motif n'a d'intérêt que si les
    // instruments qu'il emploie sont dupliqués aussi, sinon on retouche l'original.
    void md_replayer_copy_instrument(int src, int dst) {
      if (!current_module || src < 1 || src > MD_MAX_INSTRUMENTS ||
          dst < 1 || dst > MD_MAX_INSTRUMENTS || src == dst)
        return;
      current_module->instruments[dst - 1] = current_module->instruments[src - 1];
      memcpy(current_module->instr_names[dst - 1],
             current_module->instr_names[src - 1],
             sizeof(current_module->instr_names[0]));
    }

    md_module_t *md_replayer_get_module(void) { return current_module; }

    // ── Accès SONG / CHAIN / PHRASE pour l'interface ────────────────────────

    uint8_t md_replayer_get_song(int channel, int row) {
      if (!current_module || channel < 0 || channel >= MD_MAX_CHANNELS ||
          row < 0 || row >= MD_SONG_ROWS)
        return MD_EMPTY;
      return current_module->song[channel][row];
    }

    void md_replayer_set_song(int channel, int row, uint8_t chain) {
      if (!current_module || channel < 0 || channel >= MD_MAX_CHANNELS ||
          row < 0 || row >= MD_SONG_ROWS)
        return;
      if (chain != MD_EMPTY && chain >= MD_MAX_CHAINS)
        return;
      current_module->song[channel][row] = chain;
    }

    void md_replayer_get_chain(int chain, int row, uint8_t *phrase,
                               int8_t *transpose) {
      if (phrase) *phrase = MD_EMPTY;
      if (transpose) *transpose = 0;
      if (!current_module || chain < 0 || chain >= MD_MAX_CHAINS ||
          row < 0 || row >= MD_ROWS_PER_CHAIN)
        return;
      if (phrase) *phrase = current_module->chains[chain].rows[row].phrase;
      if (transpose) *transpose = current_module->chains[chain].rows[row].transpose;
    }

    void md_replayer_set_chain(int chain, int row, uint8_t phrase,
                               int8_t transpose) {
      if (!current_module || chain < 0 || chain >= MD_MAX_CHAINS ||
          row < 0 || row >= MD_ROWS_PER_CHAIN)
        return;
      if (phrase != MD_EMPTY && phrase >= MD_MAX_PHRASES)
        return;
      current_module->chains[chain].rows[row].phrase = phrase;
      current_module->chains[chain].rows[row].transpose = transpose;
    }

    void md_replayer_get_phrase(int phrase, int row, uint8_t *note,
                                uint8_t *instr, uint8_t *vel, uint8_t *cmd,
                                uint8_t *cmdval, uint8_t *mdcmd,
                                uint8_t *mdval) {
      if (note) *note = 0;
      if (instr) *instr = 0;
      if (vel) *vel = MD_EMPTY;
      if (cmd) *cmd = MD_EMPTY;
      if (cmdval) *cmdval = 0;
      if (mdcmd) *mdcmd = MD_EMPTY;
      if (mdval) *mdval = 0;
      if (!current_module || phrase < 0 || phrase >= MD_MAX_PHRASES ||
          row < 0 || row >= MD_ROWS_PER_PHRASE)
        return;
      const md_phrase_row_t *pr = &current_module->phrases[phrase].rows[row];
      if (note) *note = pr->note;
      if (instr) *instr = pr->instr;
      if (vel) *vel = pr->vel;
      if (cmd) *cmd = pr->cmd;
      if (cmdval) *cmdval = pr->cmdval;
      if (mdcmd) *mdcmd = pr->mdcmd;
      if (mdval) *mdval = pr->mdval;
    }

    void md_replayer_set_phrase(int phrase, int row, uint8_t note, uint8_t instr,
                                uint8_t vel, uint8_t cmd, uint8_t cmdval,
                                uint8_t mdcmd, uint8_t mdval) {
      if (!current_module || phrase < 0 || phrase >= MD_MAX_PHRASES ||
          row < 0 || row >= MD_ROWS_PER_PHRASE)
        return;
      md_phrase_row_t *pr = &current_module->phrases[phrase].rows[row];
      pr->note = note;
      pr->instr = instr;
      pr->vel = vel;
      pr->cmd = cmd;
      pr->cmdval = cmdval;
      pr->mdcmd = mdcmd;
      pr->mdval = mdval;
    }

    // ── Les quatre colonnes CC ─────────────────────────────────────────────
    void md_replayer_get_phrase_cc(int phrase, int row, int k, uint8_t *cc,
                                   uint8_t *val) {
      if (cc) *cc = MD_EMPTY;
      if (val) *val = 0;
      if (!current_module || phrase < 0 || phrase >= MD_MAX_PHRASES ||
          row < 0 || row >= MD_ROWS_PER_PHRASE || k < 0 || k >= MD_PHRASE_CC_COLS)
        return;
      const md_phrase_row_t *pr = &current_module->phrases[phrase].rows[row];
      if (cc) *cc = (k == 0) ? pr->mdcmd : pr->cc[k - 1];
      if (val) *val = (k == 0) ? pr->mdval : pr->ccval[k - 1];
    }

    void md_replayer_set_phrase_cc(int phrase, int row, int k, uint8_t cc,
                                   uint8_t val) {
      if (!current_module || phrase < 0 || phrase >= MD_MAX_PHRASES ||
          row < 0 || row >= MD_ROWS_PER_PHRASE || k < 0 || k >= MD_PHRASE_CC_COLS)
        return;
      md_phrase_row_t *pr = &current_module->phrases[phrase].rows[row];
      if (k == 0) { pr->mdcmd = cc; pr->mdval = val; }
      else        { pr->cc[k - 1] = cc; pr->ccval[k - 1] = val; }
    }

    void md_replayer_clear_phrase_row(int phrase, int row) {
      if (!current_module || phrase < 0 || phrase >= MD_MAX_PHRASES ||
          row < 0 || row >= MD_ROWS_PER_PHRASE)
        return;
      md_phrase_row_t *pr = &current_module->phrases[phrase].rows[row];
      pr->note = 0; pr->instr = 0;
      pr->vel = MD_EMPTY; pr->cmd = MD_EMPTY; pr->cmdval = 0;
      pr->mdcmd = MD_EMPTY; pr->mdval = 0;
      for (int k = 0; k < 3; k++) { pr->cc[k] = MD_EMPTY; pr->ccval[k] = 0; }
    }

    bool md_replayer_get_phrase_row(int phrase, int row, md_phrase_row_t *out) {
      if (!out) return false;
      if (!current_module || phrase < 0 || phrase >= MD_MAX_PHRASES ||
          row < 0 || row >= MD_ROWS_PER_PHRASE) {
        memset(out, 0, sizeof(*out));
        out->vel = MD_EMPTY; out->cmd = MD_EMPTY; out->mdcmd = MD_EMPTY;
        for (int k = 0; k < 3; k++) out->cc[k] = MD_EMPTY;
        return false;
      }
      *out = current_module->phrases[phrase].rows[row];
      return true;
    }

    void md_replayer_set_phrase_row(int phrase, int row, const md_phrase_row_t *in) {
      if (!in || !current_module || phrase < 0 || phrase >= MD_MAX_PHRASES ||
          row < 0 || row >= MD_ROWS_PER_PHRASE)
        return;
      current_module->phrases[phrase].rows[row] = *in;
    }

    // ── Têtes de lecture ────────────────────────────────────────────────────

    static bool md_channel_live(int c) {
      return play_status == MD_PLAYING && c >= 0 && c < MD_MAX_CHANNELS &&
             ch_running[c];
    }
    int md_replayer_play_song_row(int c) {
      return md_channel_live(c) ? ch_song_row[c] : -1;
    }
    int md_replayer_play_chain(int c) {
      return md_channel_live(c) ? ch_chain[c] : -1;
    }
    int md_replayer_play_chain_row(int c) {
      return md_channel_live(c) ? ch_chain_row[c] : -1;
    }
    int md_replayer_play_phrase(int c) {
      return md_channel_live(c) ? ch_phrase[c] : -1;
    }
    int md_replayer_play_phrase_row(int c) {
      return md_channel_live(c) ? ch_phrase_row[c] : -1;
    }

    void md_replayer_set_live_mode(bool live) {
      live_mode = live;
      for (int c = 0; c < MD_MAX_CHANNELS; c++) ch_pending_row[c] = -1;
    }
    bool md_replayer_is_live_mode(void) { return live_mode; }

    void md_replayer_live_arm(int channel, int song_row) {
      if (channel < 0 || channel >= MD_MAX_CHANNELS) return;
      if (song_row < 0) {                      // désarmer / couper la colonne
        ch_pending_row[channel] = -1;
        ch_running[channel] = false;
        ch_phrase[channel] = -1;
        md_key_off(channel);
        return;
      }
      if (song_row >= MD_SONG_ROWS) song_row = MD_SONG_ROWS - 1;
      ch_pending_row[channel] = song_row;
      // Si rien ne tourne encore, le canal démarre sans attendre de quantisation.
      if (play_status != MD_PLAYING) {
        ch_song_row[channel] = song_row;
        ch_pending_row[channel] = -1;
        ch_chain[channel] = -1; ch_chain_row[channel] = 0;
        ch_phrase[channel] = -1; ch_phrase_row[channel] = 0;
        ch_running[channel] = true;
        md_channel_seek(channel);
      }
    }

    int md_replayer_live_pending(int channel) {
      if (channel < 0 || channel >= MD_MAX_CHANNELS) return -1;
      return ch_pending_row[channel];
    }

    // Ligne du curseur dans l'écran CHAIN, d'où START doit partir.
void md_replayer_set_scope_row(int row) {
  scope_start_row = row;
}

void md_replayer_set_scope_chain(int chain, int row) {
  const bool ok = chain >= 0 && chain < MD_MAX_CHAINS && row >= 0 && row < MD_ROWS_PER_CHAIN;
  scope_tsp_chain = ok ? chain : -1;
  scope_tsp_row = ok ? row : 0;
}

void md_replayer_play_from(int song_row) {
  MD_TRACE("[PLAY] md_replayer_play_from(%d)\n", song_row);
      if (song_row < 0) song_row = 0;
      if (song_row >= MD_SONG_ROWS) song_row = MD_SONG_ROWS - 1;
      song_start_row = song_row;
      // Les boucles H des phrases repartent a zero : sans ca, une boucle deja
      // consommee au passage precedent ne rejouerait pas.
      for (int c = 0; c < MD_TOTAL_VOICES; c++)
        md_replayer_reset_phrase_hops(c);
    }

    bool md_replayer_is_playing() { return play_status == MD_PLAYING; }

    // Instrument YM2612 par défaut : algorithme 4 (deux piles de 2 opérateurs
    // en parallèle), feedback moyen. Les quatre opérateurs ont une enveloppe
    // active pour que TOUTE modification faite dans l'éditeur s'entende tout de
    // suite.
    //
    // ⚠️ POINT IMPORTANT — pourquoi D2R (« sustain rate ») n'est PAS à zéro :
    // sur OPL3, le bit EG-TYP de chaque opérateur permet une enveloppe dite
    // « percussive » : la note redescend toute seule jusqu'au silence même si
    // la touche reste enfoncée. C'est ce que fait l'instrument par défaut du
    // tracker OPL3, et c'est pour ça qu'on n'y écrit jamais de note-off.
    // Le YM2612 n'a pas ce bit, mais il a D2R : une fois le niveau de maintien
    // (D1L) atteint, l'enveloppe continue de descendre à la vitesse D2R, touche
    // toujours enfoncée. D2R = 0 → la note tient indéfiniment (il faut alors un
    // note-off) ; D2R > 0 → elle s'éteint seule. On part donc d'un D2R non nul,
    // pour retrouver exactement le confort d'écriture d'OPL3-Tracker.
    // (Mesuré : D2R 14 ≈ 1,9 s d'extinction, D2R 18 ≈ 1,4 s, D2R 9 ≈ 5,4 s.)
    void md_replayer_init_default_instrument(int index) {
      if (!current_module || index < 0 || index >= MD_MAX_INSTRUMENTS)
        return;

      md_instr_t *ins = &current_module->instruments[index];
      memset(ins, 0, sizeof(*ins));
      ins->pcm_volume = 127;            // un échantillon neuf sort à plein
      // AUCUN échantillon choisi. Le zéro désignait le PREMIER de la banque :
      // un instrument PCM neuf naissait donc avec le son du précédent — le kick
      // déjà chargé — au lieu d'être vierge.
      ins->pcm_sample = MD_EMPTY;
      // Instrument MIDI : canal 1, et aucun changement de programme — le synthé
      // garde le son qu'on lui a choisi.
      ins->midi_channel = 0;
      ins->midi_program = MD_EMPTY;

      // OP1 — modulateur de la première pile.
      // Son niveau est volontairement assez haut : sur le YM2612 le FEEDBACK
      // n'agit QUE sur OP1, et son intensité est proportionnelle au niveau de
      // sortie d'OP1. Avec un OP1 trop atténué, bouger le fader Feedback ne
      // s'entend pas. (Mesuré : TL 28 → 49 % d'écart entre feedback 0 et 7 ;
      // TL 22 → 104 %.)
      ins->op[0] = (md_op_params_t){.detune = 0,
                                    .multiple = 1,
                                    .total_level = 22,
                                    .key_scale = 0,
                                    .attack = 31,
                                    .decay = 8,
                                    .sustain_rate = 12,
                                    .sustain_level = 2,
                                    .release = 8,
                                    .am_enable = 0,
                                    .ssg_eg = 0};
      // OP2 — porteuse de la première pile
      ins->op[1] = (md_op_params_t){.detune = 0,
                                    .multiple = 1,
                                    .total_level = 4,
                                    .key_scale = 0,
                                    .attack = 31,
                                    .decay = 6,
                                    .sustain_rate = 14,
                                    .sustain_level = 2,
                                    .release = 8,
                                    .am_enable = 0,
                                    .ssg_eg = 0};
      // OP3 — modulateur de la seconde pile (légèrement désaccordé)
      ins->op[2] = (md_op_params_t){.detune = 1,
                                    .multiple = 2,
                                    .total_level = 30,
                                    .key_scale = 0,
                                    .attack = 31,
                                    .decay = 8,
                                    .sustain_rate = 12,
                                    .sustain_level = 2,
                                    .release = 8,
                                    .am_enable = 0,
                                    .ssg_eg = 0};
      // OP4 — porteuse de la seconde pile
      ins->op[3] = (md_op_params_t){.detune = 0,
                                    .multiple = 1,
                                    .total_level = 4,
                                    .key_scale = 0,
                                    .attack = 31,
                                    .decay = 6,
                                    .sustain_rate = 14,
                                    .sustain_level = 2,
                                    .release = 8,
                                    .am_enable = 0,
                                    .ssg_eg = 0};

      ins->algorithm = 4;
      ins->feedback = 4;
      ins->ams = 0;
      ins->pms = 0;
      ins->lfo_enable = 0;
      ins->lfo_freq = 0;
      ins->panning = 128; // centre
      // Un KICK par défaut qui sonne déjà juste : chute franche, coup court,
      // un peu de clic et de saturation.
      ins->kick_drop = 96;
      ins->kick_time = 40;
      ins->kick_decay = 90;
      ins->kick_click = 40;
      ins->kick_drive = 60;
      ins->kick_alt = MD_EMPTY;
      ins->fine_tune = 0;
      // Bruit blanc « verrouillé sur le ton 3 » (mode 7). C'est le SEUL mode du
      // SN76489 où la hauteur du bruit suit la note : les trois autres périodes
      // sont fixes, et toutes les notes y sonnent identiques. Le générateur de
      // bruit est alors cadencé par le registre de période de la voie PSG3.
      ins->psg_noise = 0x07;

      // Enveloppe PSG par défaut : attaque immédiate au maximum, léger déclin
      // vers un maintien nul — la note s'éteint donc toute seule, sans qu'on
      // ait à semer des note-off dans les phrases.
      // ENV. D8 / 00 / -- : on part fort et on descend vers le silence à
      // vitesse moyenne, puis on y reste. La note s'éteint donc toute seule,
      // sans qu'on ait à semer des note-off dans les phrases.
      ins->env_amp[0] = 0xD; ins->env_speed[0] = 8;
      ins->env_amp[1] = 0;   ins->env_speed[1] = 0;
      ins->env_amp[2] = MD_EMPTY; ins->env_speed[2] = 0;

      // Aucune table attachée : 0 est un numéro de table VALIDE, l'absence se
      // note MD_EMPTY. Le memset ci-dessus aurait sinon collé la table 00 à
      // tous les instruments.
      ins->table = MD_EMPTY;

      // Sur la RG350, un instrument neuf est un instrument FM de l'OPL3 : un
      // son simple, modulateur doux sur porteuse sinus, qui s'eteint seul.
      ins->kind = MD_INSTR_KIND_OPL;
      ins->panning = 128;
      static const uint8_t kOplDefaut[MD_OPL_OCTETS] = {
#ifdef FM1
          // FM-1 : EGT eteint, la note s'eteint seule : DECAY et RELEASE
          // s'entendent tout de suite (LENGTH, KNOB 2).
          0x01, 0x01,   // EG non tenue, MULT 1
#else
          0x21, 0x21,   // EG tenue, MULT 1
#endif
#ifdef FM1
          // FM-1 : un son de depart plus crasseux : le modulateur plus fort
          // (TL 18 : OUT MOD 2D) et plus de feedback (5).
          0x12, 0x00,   // TL modulateur 18, porteuse a fond
#else
          0x1C, 0x00,   // TL modulateur 28, porteuse a fond
#endif
          0xF4, 0xF3,   // attaque rapide, declin moyen
          0x56, 0x56,   // sustain 5, release 6
          0x00, 0x00,   // sinus
#ifdef FM1
          0x0A};        // feedback 5, FM
#else
          0x06};        // feedback 3, FM
#endif
      memcpy(MD_OPL_FM(ins), kOplDefaut, MD_OPL_OCTETS);

      snprintf(current_module->instr_names[index], 43, "Instrument %02d",
               index + 1);
    }

    void md_replayer_new_empty() {
      md_lock(&replayer_lock);

      md_replayer_shut();
      md_macros_vide();
      current_module = (md_module_t *)calloc(1, sizeof(md_module_t));
      if (current_module) {
        current_module->num_channels = MD_PISTES_OPL;
        current_module->initial_tempo = 125;
        current_module->initial_speed = 6;
        current_module->macro_speedup = 1;
        current_module->bpm_rows_per_beat = 4;
        current_module->bpm_tempo_finetune = 0;

        // Morceau vierge : toutes les cases du SONG et tous les chains/phrases
        // sont marqués « vide » (0xFF), pas zéro — 00 est un numéro valide.
        memset(current_module->song, MD_EMPTY, sizeof(current_module->song));
        for (int i = 0; i < MD_MAX_CHAINS; i++)
          for (int r = 0; r < MD_ROWS_PER_CHAIN; r++) {
            current_module->chains[i].rows[r].phrase = MD_EMPTY;
            current_module->chains[i].rows[r].transpose = 0;
          }
        // Idem pour les tables : « VOL 0 » est le silence, pas l'absence de
        // consigne. Une table neuve doit donc être remplie de MD_EMPTY.
        for (int i = 0; i < MD_MAX_TABLES; i++)
          for (int r = 0; r < MD_TABLE_ROWS; r++) {
            md_table_row_t *tr = &current_module->tables[i].rows[r];
            tr->vol = 0;          // 0 = ne touche pas au volume
            tr->transpose = 0;
            tr->cmd1 = MD_EMPTY;
            tr->val1 = 0;
            tr->cmd2 = MD_EMPTY;
            tr->val2 = 0;
            tr->mdcmd = MD_EMPTY;
            tr->mdval = 0;
          }

        for (int i = 0; i < MD_MAX_PHRASES; i++)
          for (int r = 0; r < MD_ROWS_PER_PHRASE; r++) {
            md_phrase_row_t *pr = &current_module->phrases[i].rows[r];
            pr->note = 0; pr->instr = 0;
            pr->vel = MD_EMPTY; pr->cmd = MD_EMPTY; pr->cmdval = 0;
            // Sans cette ligne, mdcmd restait à 0 — l'indice de la PREMIÈRE
            // commande, pas « vide ». Chaque ligne portait donc un « A 00 »
            // fantôme qui forçait l'algorithme 0 à chaque note et écrasait
            // celui de l'instrument.
            pr->mdcmd = MD_EMPTY; pr->mdval = 0;
            // Idem pour les colonnes CC 2 à 4 : zéro serait le CC 00.
            for (int k = 0; k < 3; k++) { pr->cc[k] = MD_EMPTY; pr->ccval[k] = 0; }
          }
        song_start_row = 0;

        // Silence, et chaque piste sur sa voie.
        md_ds_silence();
        memset(psg_env, 0, sizeof(psg_env));
        for (int c = 0; c < MD_MAX_CHANNELS; c++)
          ch_hw_voice[c] = (uint8_t)c;
        ch_hw_voice[MD_TEST_CHANNEL] = 0;

        memset(ch_b0_reg, 0, sizeof(ch_b0_reg));
        memset(ch_current_instr, 0, sizeof(ch_current_instr));

        for (int i = 0; i < MD_MAX_INSTRUMENTS; i++)
          md_replayer_init_default_instrument(i);

        // Pré-affecte l'instrument 1 à la voie d'audition pour que les réglages
        // soient audibles avant même la première note jouée.
        ch_current_instr[MD_TEST_CHANNEL] = 1;
        md_set_instrument(MD_TEST_CHANNEL, &current_module->instruments[0]);
      }

      md_unlock(&replayer_lock);
    }

    int md_replayer_get_tempo() {
      return current_module ? current_module->initial_tempo : 125;
    }

    void md_replayer_set_tempo(int tempo) {
      if (tempo < 18)
        tempo = 18;
      if (current_module)
        current_module->initial_tempo = tempo;
      replay_tempo = tempo;
      // IMPORTANT : recalculer la cadence de tick, sinon la vitesse de lecture
      // ne change pas (samples_per_tick restait figé à l'ancien tempo).
      int macro = (current_module && current_module->macro_speedup > 0)
                      ? current_module->macro_speedup : 1;
      int irq_freq = replay_tempo * macro;
      if (irq_freq == 0)
        irq_freq = 50; // Fallback
      if (replay_sample_rate > 0)
        samples_per_tick = (int)((double)replay_sample_rate / ((double)irq_freq * md_finetune_scale()));
        if (samples_per_tick < 1) samples_per_tick = 1;
    }

    int md_replayer_get_speed() {
      return current_module ? current_module->initial_speed : 6;
    }

    int md_replayer_get_rows_per_beat() {
      // Sert au calcul du BPM réel (BPM = tempo*60/(speed*rows_per_beat)).
      if (!current_module || current_module->bpm_rows_per_beat == 0)
        return 4; // défaut AT2
      return current_module->bpm_rows_per_beat;
    }

    // BPM réel AT2 = tempo*60/(speed*rows_per_beat) * (base+finetune)/base.
    double md_replayer_get_bpm() {
      if (!current_module) return 125.0;
      int tempo = current_module->initial_tempo > 0 ? current_module->initial_tempo : 50;
      int speed = current_module->initial_speed > 0 ? current_module->initial_speed : 6;
      int rpb = md_replayer_get_rows_per_beat();
      double baseBPM = (double)tempo * 60.0 / ((double)speed * (double)rpb);
      return baseBPM * md_finetune_scale();
    }

    // Règle le BPM en ajustant le finetune (tempo/speed/rows_per_beat inchangés),
    // puis recalcule immédiatement la cadence de lecture.
    void md_replayer_set_bpm(double bpm) {
      if (!current_module) return;
      int tempo = current_module->initial_tempo > 0 ? current_module->initial_tempo : 50;
      int speed = current_module->initial_speed > 0 ? current_module->initial_speed : 6;
      int macro = current_module->macro_speedup > 0 ? current_module->macro_speedup : 1;
      int rpb = md_replayer_get_rows_per_beat();
      int base = at2_base_irq_freq(tempo, macro);
      double baseBPM = (double)tempo * 60.0 / ((double)speed * (double)rpb);
      if (baseBPM < 0.001) return;
      double ft = (double)base * (bpm / baseBPM - 1.0);
      int finetune = (int)(ft < 0 ? ft - 0.5 : ft + 0.5);
      if (base + finetune < 50) finetune = 50 - base;     // IRQ >= 50 Hz
      if (base + finetune > 1000) finetune = 1000 - base; // IRQ <= 1000 Hz
      current_module->bpm_tempo_finetune = (int16_t)finetune;
      int irq_freq = tempo * macro;
      if (irq_freq == 0) irq_freq = 50;
      if (replay_sample_rate > 0)
        samples_per_tick = (int)((double)replay_sample_rate / ((double)irq_freq * md_finetune_scale()));
        if (samples_per_tick < 1) samples_per_tick = 1;
      if (samples_per_tick < 1) samples_per_tick = 1;
    }

    // Lignes par temps : sert au calcul du BPM affiché. DefleMask range la
    // même idée dans son « surlignage A ».
    void md_replayer_set_rows_per_beat(int rpb) {
      if (!current_module) return;
      if (rpb < 1) rpb = 1;
      if (rpb > 32) rpb = 32;
      current_module->bpm_rows_per_beat = (uint8_t)rpb;
    }

    void md_replayer_set_speed(int speed) {
      if (current_module)
        current_module->initial_speed = speed;
      replay_speed = speed;
    }

    // Écoute d'une note AVEC sa force, 0-127 (-1 = laisser celle en place).
    // L'audition partait toujours à la force que la voie traînait : la colonne
    // VEL ne s'entendait donc pas à l'arrêt, ni en MIDI (la vélocité de la
    // note-on en découle) ni sur un échantillon. On la pose ici, exactement
    // comme le séquenceur le fait au passage d'une ligne.
    void md_replayer_play_test_note_vel(int note, int instr, int channel,
                                        int vel) {
      if (!current_module || instr < 1 || instr > MD_MAX_INSTRUMENTS ||
          channel < 0 || channel >= MD_TOTAL_VOICES)
        return;
      // Sous verrou : le séquenceur tourne sous interruption et touche aux
      // mêmes voies.
      md_lock(&replayer_lock);
      if (channel == MD_TEST_CHANNEL)
        md_assign_test_voice();
      if (vel >= 0) {
        const int vv = vel > 127 ? 127 : vel;
        ch_fmpar[channel].volC = (uint8_t)(63 - (vv * 63 + 63) / 127);
        ch_vel_fin[channel] = (uint8_t)vv;
      }
      ch_current_instr[channel] = instr;
      md_set_instrument((uint8_t)channel, &current_module->instruments[instr - 1]);
      md_play_note((uint8_t)channel, (uint8_t)note);
      ds_voix_envoie();
      md_unlock(&replayer_lock);
    }

    void md_replayer_play_test_note(int note, int instr, int channel) {
      // Sans force donnée, on écoute à fond : c'est ce qu'on veut en essayant
      // un instrument depuis sa page.
      md_replayer_play_test_note_vel(note, instr, channel, 127);
    }

    // Enable/disable the preview arpeggio on a test channel. `val` is the raw
    // 0xy param (high nibble = 1st semitone offset, low nibble = 2nd); 0 = off.
    void md_replayer_set_test_arpeggio(int channel, int val) {
      if (channel < 0 || channel >= MD_TOTAL_VOICES) {
        test_arp_channel = -1;
        return;
      }
      if (val == 0) {
        ch_arpeggio_val[channel] = 0;
        ch_arpeggio_state[channel] = 0;
        if (test_arp_channel == channel)
          test_arp_channel = -1;
        return;
      }
      ch_arpeggio_val[channel] = (uint8_t)(val & 0xFF);
      ch_arpeggio_state[channel] = 0;
      // Force the free-play loop to apply the arpeggio on its VERY NEXT audio
      // chunk (instead of waiting up to ~20 ms for the previous tick window to
      // expire). Without this, a fresh-project first note didn't audibly tick
      // the arpeggio before F5 had ever run.
      samples_until_next_tick = 0;
      test_arp_channel = channel;
    }

    void md_replayer_stop_test_note(int channel) {
      if (channel < 0 || channel >= MD_TOTAL_VOICES)
        return;
      md_lock(&replayer_lock);
      // La voie d'audition PARTAGE son matériel avec une piste du morceau :
      // pendant la lecture, relâcher A ne doit pas couper la note du MORCEAU.
      if (channel == MD_TEST_CHANNEL) {
        uint8_t hw = ch_hw_voice[MD_TEST_CHANNEL];
        for (int c = 0; c < MD_MAX_CHANNELS; c++)
          if (ch_hw_voice[c] == hw && ch_running[c]) {
            md_unlock(&replayer_lock);
            return;                       // cette voix n'est pas la nôtre
          }
      }
      // Un échantillon joué une fois va jusqu'au bout, comme la voie PCM de
      // GeneTrackerDS que le relâchement n'a jamais coupée. Un échantillon
      // bouclé et une note MIDI, eux, s'arrêtent : sinon ils tiendraient
      // indéfiniment.
      const md_sample_t *sm =
          md_channel_is_pcm(channel) ? md_sample_de_piste(channel) : NULL;
      if (sm && sm->loop < 0)
        ch_table_running[channel] = false;
      else
        md_key_off(channel);
      ds_voix_envoie();
      md_unlock(&replayer_lock);
    }

    // Pré-affecte un instrument à la voie d'audition pour que les réglages de
    // l'éditeur soient audibles avant même la première note jouée.
    void md_replayer_prepare_test_channel(int instr) {
      if (!current_module || instr < 1 || instr > MD_MAX_INSTRUMENTS)
        return;
      // La voie doit être choisie AVANT d'y écrire l'instrument, sinon on
      // enverrait un patch FM sur une voie PSG (ou l'inverse).
      md_assign_test_voice();
      ch_current_instr[MD_TEST_CHANNEL] = instr;
      md_set_instrument(MD_TEST_CHANNEL, &current_module->instruments[instr - 1]);
    }

    // ── Propriétés d'instrument pilotées par l'éditeur ──────────────────────
    // Les identifiants (md_op_prop_t / md_gen_prop_t) sont ceux attendus par
    // l'UI. Toute modification est immédiatement repoussée vers les canaux qui
    // utilisent l'instrument, y compris la voie d'audition.

    static void md_push_instrument_update(int ins_idx) {
      if (!current_module)
        return;
      md_lock(&replayer_lock);
      for (int c = 0; c < MD_TOTAL_VOICES; c++) {
        if (ch_current_instr[c] == ins_idx)
          md_set_instrument((uint8_t)c,
                            &current_module->instruments[ins_idx - 1]);
      }
      ds_voix_envoie();
      md_unlock(&replayer_lock);
    }

    static uint8_t md_clamp_u8(int v, int max) {
      if (v < 0)
        v = 0;
      if (v > max)
        v = max;
      return (uint8_t)v;
    }

    void md_replayer_set_instr_op_val(int ins_idx, int op_idx, int prop_id,
                                      uint8_t val) {
      if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS)
        return;
      if (op_idx < 0 || op_idx > 3)
        return;
      md_op_params_t *o = &current_module->instruments[ins_idx - 1].op[op_idx];

      switch (prop_id) {
      case MD_OP_PROP_MULTIPLE:
        o->multiple = md_clamp_u8(val, 15);
        break;
      case MD_OP_PROP_KEY_SCALE:
        o->key_scale = md_clamp_u8(val, 3);
        break;
      case MD_OP_PROP_AM:
        o->am_enable = val ? 1 : 0;
        break;
      case MD_OP_PROP_DETUNE:
        o->detune = md_clamp_u8(val, 7);
        break;
      case MD_OP_PROP_SSG_EG:
        o->ssg_eg = md_clamp_u8(val, 15);
        break;
      case MD_OP_PROP_SUSTAIN_RATE:
        o->sustain_rate = md_clamp_u8(val, 31);
        break;
      case MD_OP_PROP_TOTAL_LEVEL:
        o->total_level = md_clamp_u8(val, 127);
        break;
      case MD_OP_PROP_ATTACK:
        o->attack = md_clamp_u8(val, 31);
        break;
      case MD_OP_PROP_DECAY:
        o->decay = md_clamp_u8(val, 31);
        break;
      case MD_OP_PROP_SUSTAIN_LEVEL:
        o->sustain_level = md_clamp_u8(val, 15);
        break;
      case MD_OP_PROP_RELEASE:
        o->release = md_clamp_u8(val, 15);
        break;
      default:
        return;
      }

      md_push_instrument_update(ins_idx);
    }

    uint8_t md_replayer_get_instr_op_val(int ins_idx, int op_idx, int prop_id) {
      if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS)
        return 0;
      if (op_idx < 0 || op_idx > 3)
        return 0;
      const md_op_params_t *o =
          &current_module->instruments[ins_idx - 1].op[op_idx];

      switch (prop_id) {
      case MD_OP_PROP_MULTIPLE:
        return o->multiple;
      case MD_OP_PROP_KEY_SCALE:
        return o->key_scale;
      case MD_OP_PROP_AM:
        return o->am_enable;
      case MD_OP_PROP_DETUNE:
        return o->detune;
      case MD_OP_PROP_SSG_EG:
        return o->ssg_eg;
      case MD_OP_PROP_SUSTAIN_RATE:
        return o->sustain_rate;
      case MD_OP_PROP_TOTAL_LEVEL:
        return o->total_level;
      case MD_OP_PROP_ATTACK:
        return o->attack;
      case MD_OP_PROP_DECAY:
        return o->decay;
      case MD_OP_PROP_SUSTAIN_LEVEL:
        return o->sustain_level;
      case MD_OP_PROP_RELEASE:
        return o->release;
      default:
        return 0;
      }
    }

    void md_replayer_set_instr_gen_val(int ins_idx, int prop_id, int val) {
      if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS)
        return;
      md_instr_t *ins = &current_module->instruments[ins_idx - 1];

      switch (prop_id) {
      case MD_GEN_PROP_ALGORITHM:
        ins->algorithm = md_clamp_u8(val, 7);
        break;
      case MD_GEN_PROP_FEEDBACK:
        ins->feedback = md_clamp_u8(val, 7);
        break;
      case MD_GEN_PROP_PANNING:
        ins->panning = md_clamp_u8(val, 255);
        break;
      case MD_GEN_PROP_FINE_TUNE:
        ins->fine_tune = (int8_t)val;
        break;
      case MD_GEN_PROP_AMS:
        ins->ams = md_clamp_u8(val, 3);
        break;
      case MD_GEN_PROP_PMS:
        ins->pms = md_clamp_u8(val, 7);
        break;
      case MD_GEN_PROP_LFO_ENABLE:
        ins->lfo_enable = val ? 1 : 0;
        break;
      case MD_GEN_PROP_LFO_FREQ:
        ins->lfo_freq = md_clamp_u8(val, 7);
        break;
      case MD_GEN_PROP_PSG_NOISE:
        ins->psg_noise = md_clamp_u8(val, 7);
        break;
      default:
        return;
      }

      md_push_instrument_update(ins_idx);
    }

    int md_replayer_get_instr_gen_val(int ins_idx, int prop_id) {
      if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS)
        return 0;
      const md_instr_t *ins = &current_module->instruments[ins_idx - 1];

      switch (prop_id) {
      case MD_GEN_PROP_ALGORITHM:
        return ins->algorithm;
      case MD_GEN_PROP_FEEDBACK:
        return ins->feedback;
      case MD_GEN_PROP_PANNING:
        return ins->panning;
      case MD_GEN_PROP_FINE_TUNE:
        return ins->fine_tune;
      case MD_GEN_PROP_AMS:
        return ins->ams;
      case MD_GEN_PROP_PMS:
        return ins->pms;
      case MD_GEN_PROP_LFO_ENABLE:
        return ins->lfo_enable;
      case MD_GEN_PROP_LFO_FREQ:
        return ins->lfo_freq;
      case MD_GEN_PROP_PSG_NOISE:
        return ins->psg_noise;
      default:
        return 0;
      }
    }

    void md_replayer_set_audition_channel(int channel) {
      if (channel < 0 || channel >= MD_MAX_CHANNELS)
        channel = 0;
      audition_channel = channel;
    }

    // Active ou desactive une voie. Desactivee, elle est coupee net puis
    // ignoree : plus aucune note ne l'atteint, donc le YM2612 cesse de la
    // calculer. Le morceau, lui, garde ses notes intactes.
    // Coupe net les effets qui tournent sur une voie.
    //
    // Un effet continu — vibrato, glissando, arpege — dure jusqu'a ce qu'on le
    // reecrive avec la valeur 00. EFFACER la case, elle, laisse 0xFF sur la
    // ligne, que le moteur ignore : l'effet continuait donc de tourner jusqu'a
    // l'arret de la lecture. L'interface appelle ceci quand on efface une
    // commande, pour que la suppression s'entende tout de suite.
    void md_replayer_clear_active_effects(int channel) {
      if (channel < 0 || channel >= MD_TOTAL_VOICES)
        return;
      for (int e = 0; e < MD_EFF_SLOTS; e++) {
        ch_active_eff[channel][e] = 0xFF;
        ch_active_eff_val[channel][e] = 0;
      }
    }

    void md_replayer_disable_channel(int channel, bool disabled) {
      if (channel < 0 || channel >= MD_TOTAL_VOICES)
        return;
      md_channel_disabled[channel] = disabled;
      if (disabled) {
        md_key_off(channel);
        if (channel == MD_PCM_CHANNEL)
          md_chip_pcm_stop();
      }
    }

    bool md_replayer_is_channel_disabled(int channel) {
      if (channel < 0 || channel >= MD_TOTAL_VOICES)
        return false;
      return md_channel_disabled[channel];
    }

    void md_replayer_mute_channel(int channel, bool muted) {
      if (channel >= 0 && channel < 32) {
        md_channel_muted[channel] = muted;
        // Le convertisseur n'a pas de registre de volume et ne passe pas par
        // md_write_volume : on l'arrête net, sinon la voie PCM continuait de
        // jouer alors qu'elle était coupée.
        if (muted && channel == MD_PCM_CHANNEL)
          md_chip_pcm_stop();
        // If we just muted it while it's playing, force the current volume to 0x3F
        // immediately
        if (channel < MD_TOTAL_VOICES) {
          // Applique (ou lève) immédiatement l'atténuation sur la voie.
          md_write_volume(channel);
        }
      }
    }

// ── Table de volumes des voies PSG (accès pour l'éditeur d'instrument) ──────
// L'index d'instrument est 1…MD_MAX_INSTRUMENTS, comme pour les accesseurs FM.
// Les écritures se font sous le verrou : la table est relue à chaque tick par
// le thread audio.

// ── Banque d'échantillons ───────────────────────────────────────────────────

// Les voies de la DS lisent par MOTS de 32 bits : chaque échantillon commence
// sur une frontière de 4 octets.
uint8_t *md_replayer_zone_libre(uint32_t *capacite) {
  if (!current_module) {
    if (capacite) *capacite = 0;
    return NULL;
  }
  const uint32_t debut = (current_module->pcm_used + 3u) & ~3u;
  if (capacite)
    *capacite = (debut < MD_PCM_BANK_BYTES) ? MD_PCM_BANK_BYTES - debut : 0;
  return md_banque + (debut < MD_PCM_BANK_BYTES ? debut : MD_PCM_BANK_BYTES - 4);
}

int md_replayer_add_sample(const char *name, const uint8_t *data,
                           uint32_t images, int bits, uint32_t cadence,
                           int loop, int base_note) {
  if (!current_module || !data || images == 0)
    return -1;
  if (bits != 16) bits = 8;
  int slot = -1;
  for (int i = 0; i < MD_MAX_SAMPLES; i++)
    if (current_module->samples[i].length == 0) { slot = i; break; }
  if (slot < 0)
    return -1;                                  // plus d'emplacement
  const uint32_t debut = (current_module->pcm_used + 3u) & ~3u;
  const uint32_t octets = images * (uint32_t)(bits / 8);
  if (images > MD_PCM_BANK_BYTES || debut > MD_PCM_BANK_BYTES ||
      octets > MD_PCM_BANK_BYTES - debut)
    return -1;                                  // banque pleine
  // La copie et le vidage de cache se font HORS verrou : cette zone n'est
  // encore référencée par aucun échantillon, le séquenceur n'y lit rien.
  if (data != md_banque + debut)
    memmove(md_banque + debut, data, octets);
  // L'ARM7 lit la mémoire principale sans passer par le cache de l'ARM9.
  DC_FlushRange(md_banque + debut, octets);
  md_lock(&replayer_lock);
  md_sample_t *sm = &current_module->samples[slot];
  memset(sm, 0, sizeof(*sm));
  if (name) { strncpy(sm->name, name, sizeof(sm->name) - 1); }
  sm->offset = debut;
  sm->length = images;
  sm->cap = images;          // la place réservée, que le KICK fera grandir
  sm->bits = (uint8_t)bits;
  sm->rate = cadence ? cadence : MD_PCM_RATE;
  sm->loop = (loop >= 0 && (uint32_t)loop < images) ? loop : -1;
  sm->base_note = (uint8_t)(base_note > 0 && base_note <= MD_MAX_NOTE ? base_note : 49);
  current_module->pcm_used = debut + octets;
  md_unlock(&replayer_lock);
  return slot;
}

void md_replayer_set_sample_length(int idx, uint32_t images) {
  if (!current_module || idx < 0 || idx >= MD_MAX_SAMPLES) return;
  md_sample_t *sm = &current_module->samples[idx];
  if (sm->cap && images > sm->cap) images = sm->cap;
  md_lock(&replayer_lock);
  sm->length = images;
  if (sm->loop >= 0 && (uint32_t)sm->loop >= images) sm->loop = -1;
  md_unlock(&replayer_lock);
}

bool md_replayer_get_sample_format(int idx, int *bits, uint32_t *cadence) {
  if (!current_module || idx < 0 || idx >= MD_MAX_SAMPLES) return false;
  const md_sample_t *sm = &current_module->samples[idx];
  if (bits) *bits = sm->bits;
  if (cadence) *cadence = sm->rate;
  return sm->length > 0;
}

// Feuilleter un dossier d'échantillons en ajoute un à chaque pas : sans ce
// retrait, la banque serait pleine en quelques pressions. Seul le dernier
// rangé peut rendre sa place — les autres laisseraient un trou.
bool md_replayer_libere_sample(int idx, int sauf_instr) {
  if (!current_module || idx < 0 || idx >= MD_MAX_SAMPLES)
    return false;
  md_sample_t *sm = &current_module->samples[idx];
  if (!sm->length)
    return false;
  for (int i = 1; i <= MD_MAX_INSTRUMENTS; i++)
    if (i != sauf_instr && current_module->instruments[i - 1].pcm_sample == idx)
      return false;
  // On compare sur la place RÉSERVÉE : un KICK en garde plus que sa longueur
  // utile, et il resterait sinon coincé dans la banque.
  const uint32_t reserve = sm->cap ? sm->cap : sm->length;
  const uint32_t fin = sm->offset + reserve * (uint32_t)(sm->bits / 8);
  if (fin != current_module->pcm_used)
    return false;
  md_lock(&replayer_lock);
  // La place va être réécrite : aucune voie ne doit plus y lire.
  for (int c = 0; c < MD_TOTAL_VOICES; c++)
    if (ch_instr_shadow[c].pcm_sample == idx)
      ds_voix_stop(ch_hw_voice[c]);
  ds_voix_envoie();
  current_module->pcm_used = sm->offset;
  memset(sm, 0, sizeof(*sm));
  md_unlock(&replayer_lock);
  return true;
}

int md_replayer_sample_count(void) {
  if (!current_module) return 0;
  int n = 0;
  for (int i = 0; i < MD_MAX_SAMPLES; i++)
    if (current_module->samples[i].length) n++;
  return n;
}

bool md_replayer_get_sample(int idx, char *name, int name_cap,
                            uint32_t *length, int *loop, int *base_note) {
  if (!current_module || idx < 0 || idx >= MD_MAX_SAMPLES) return false;
  const md_sample_t *sm = &current_module->samples[idx];
  if (name && name_cap > 0) {
    strncpy(name, sm->name, (size_t)name_cap - 1);
    name[name_cap - 1] = 0;
  }
  if (length) *length = sm->length;
  if (loop) *loop = sm->loop;
  if (base_note) *base_note = sm->base_note;
  return sm->length > 0;
}

const uint8_t *md_replayer_sample_data(int idx, uint32_t *length) {
  if (!current_module || idx < 0 || idx >= MD_MAX_SAMPLES) return NULL;
  const md_sample_t *sm = &current_module->samples[idx];
  if (length) *length = sm->length;
  return sm->length ? (md_banque + sm->offset) : NULL;
}

void md_replayer_set_sample_base(int idx, int base_note) {
  if (!current_module || idx < 0 || idx >= MD_MAX_SAMPLES) return;
  if (base_note < 1) base_note = 1;
  if (base_note > MD_MAX_NOTE) base_note = MD_MAX_NOTE;
  current_module->samples[idx].base_note = (uint8_t)base_note;
}

void md_replayer_set_sample_loop(int idx, int loop) {
  if (!current_module || idx < 0 || idx >= MD_MAX_SAMPLES) return;
  md_sample_t *sm = &current_module->samples[idx];
  sm->loop = (loop >= 0 && (uint32_t)loop < sm->length) ? loop : -1;
}

void md_replayer_clear_samples(void) {
  if (!current_module) return;
  md_chip_pcm_stop();
  memset(current_module->samples, 0, sizeof(current_module->samples));
  current_module->pcm_used = 0;
}

uint32_t md_replayer_pcm_used(void) {
  return current_module ? current_module->pcm_used : 0;
}
uint32_t md_replayer_pcm_capacity(void) { return MD_PCM_BANK_BYTES; }

void md_replayer_set_instr_sample(int ins_idx, int sample_idx) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return;
  // MD_EMPTY = aucun échantillon, et c'est une valeur légitime.
  if (sample_idx != MD_EMPTY && (sample_idx < 0 || sample_idx >= MD_MAX_SAMPLES))
    sample_idx = MD_EMPTY;
  current_module->instruments[ins_idx - 1].pcm_sample = (uint8_t)sample_idx;
  md_push_instrument_update(ins_idx);
}
void md_replayer_set_instr_pcm_volume(int ins_idx, int volume) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return;
  if (volume < 0) volume = 0;
  if (volume > 255) volume = 255;   // 7F = unité, jusqu'à FF pour pousser
  current_module->instruments[ins_idx - 1].pcm_volume = (uint8_t)volume;
  md_push_instrument_update(ins_idx);
}
int md_replayer_get_instr_pcm_volume(int ins_idx) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return 127;
  return current_module->instruments[ins_idx - 1].pcm_volume;
}
int md_replayer_get_instr_sample(int ins_idx) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return 0;
  return current_module->instruments[ins_idx - 1].pcm_sample;
}

void md_replayer_set_instr_kind(int ins_idx, int kind) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return;
  if (kind < 0 || kind > MD_INSTR_KIND_OPL) kind = MD_INSTR_KIND_ANY;
  current_module->instruments[ins_idx - 1].kind = (uint8_t)kind;
  // Une piste qui joue cet instrument doit changer de sortie tout de suite.
  md_push_instrument_update(ins_idx);
}

// ── Instrument OPL : ses onze octets, au format A2M (MD_OPL_*) ───────────────
uint8_t md_replayer_get_instr_opl(int ins_idx, int octet) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS ||
      octet < 0 || octet >= MD_OPL_OCTETS)
    return 0;
  return MD_OPL_FM(&current_module->instruments[ins_idx - 1])[octet];
}

void md_replayer_set_instr_opl(int ins_idx, int octet, uint8_t valeur) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS ||
      octet < 0 || octet >= MD_OPL_OCTETS)
    return;
  MD_OPL_FM(&current_module->instruments[ins_idx - 1])[octet] = valeur;
  // Une piste qui joue cet instrument l'entend changer a la note suivante.
  md_push_instrument_update(ins_idx);
}

// ── Instrument MIDI ─────────────────────────────────────────────────────────
void md_replayer_set_instr_midi_channel(int ins_idx, int channel) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return;
  if (channel < 0) channel = 0;
  if (channel > 15) channel = 15;
  current_module->instruments[ins_idx - 1].midi_channel = (uint8_t)channel;
  md_push_instrument_update(ins_idx);
}

int md_replayer_get_instr_midi_channel(int ins_idx) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return 0;
  return current_module->instruments[ins_idx - 1].midi_channel & 0x0F;
}

// MD_EMPTY (ou toute valeur hors 00-7F) = pas de changement de programme.
void md_replayer_set_instr_midi_program(int ins_idx, int program) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return;
  current_module->instruments[ins_idx - 1].midi_program =
      (program < 0 || program > 127) ? MD_EMPTY : (uint8_t)program;
  md_push_instrument_update(ins_idx);
}

int md_replayer_get_instr_midi_program(int ins_idx) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS)
    return MD_EMPTY;
  return current_module->instruments[ins_idx - 1].midi_program;
}

// ── Le KICK de synthèse ─────────────────────────────────────────────────────
// Une grosse caisse de techno, c'est une sinusoïde dont la hauteur plonge très
// vite, avec un clic au départ et de la saturation. On la FABRIQUE une fois
// dans la banque : la puce la joue ensuite comme n'importe quel échantillon,
// donc sans un cycle de processeur pendant la lecture, et les notes en
// changent simplement la hauteur.
#define MD_KICK_SIN 1024
static int16_t md_sinus[MD_KICK_SIN];

static void md_prepare_sinus(void) {
  // Une sinusoïde sans bibliothèque mathématique : on part de l'angle nul et
  // on tourne d'un pas à chaque point. Le sinus et le cosinus de ce pas
  // viennent de leur développement, exact au millionième pour un angle aussi
  // petit.
  const double w = 2.0 * 3.14159265358979323846 / (double)MD_KICK_SIN;
  const double w2 = w * w;
  const double sw = w * (1.0 - w2 / 6.0 + w2 * w2 / 120.0);
  const double cw = 1.0 - w2 / 2.0 + w2 * w2 / 24.0;
  double s = 0.0, c = 1.0;
  for (int i = 0; i < MD_KICK_SIN; i++) {
    md_sinus[i] = (int16_t)(s * 32767.0);
    const double ns = s * cw + c * sw;
    const double nc = c * cw - s * sw;
    s = ns; c = nc;
  }
}

// La place réservée à un kick : la plus longue qu'il puisse prendre, déclin au
// maximum. On la réserve UNE FOIS, pour pouvoir le refabriquer sur place — même
// en pleine lecture — sans faire grossir la banque à chaque cran de réglage.
#define MD_KICK_IMAGES_MAX ((uint32_t)(MD_PCM_RATE / 10) + \
                            255u * (uint32_t)(MD_PCM_RATE / 256))

int md_replayer_rend_kick(int ins_idx) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS)
    return -1;
  md_instr_t *ins = &current_module->instruments[ins_idx - 1];
  if (ins->kind != MD_INSTR_KIND_KICK)
    return -1;

  // Longueur utile : un dixième de seconde, plus le déclin. De 0,1 s à 1,1 s.
  const uint32_t images = (uint32_t)(MD_PCM_RATE / 10) +
                          (uint32_t)ins->kick_decay * (MD_PCM_RATE / 256);

  // ── DEUX zones, pour ne JAMAIS couper ce qui sonne ──────────────────────
  // Un kick dure jusqu'à une seconde : quand on tourne un bouton, la puce est
  // peut-être en train de lire le coup précédent. On fabrique donc le nouveau
  // dans la zone que PERSONNE ne lit, et on ne bascule l'instrument dessus
  // qu'une fois le calcul fini. Le coup en cours va au bout, le suivant sort
  // avec le nouveau réglage.
  const uint32_t octets_max = MD_KICK_IMAGES_MAX * 2u;
  int idxA = ins->pcm_sample;
  if (idxA < 0 || idxA >= MD_MAX_SAMPLES ||
      current_module->samples[idxA].cap < MD_KICK_IMAGES_MAX ||
      current_module->samples[idxA].bits != 16)
    idxA = -1;
  int idxB = ins->kick_alt;
  if (idxB < 0 || idxB >= MD_MAX_SAMPLES ||
      current_module->samples[idxB].cap < MD_KICK_IMAGES_MAX ||
      current_module->samples[idxB].bits != 16)
    idxB = -1;
  // Les zones qui manquent sont réservées une fois pour toutes.
  for (int essai = 0; essai < 2; essai++) {
    int *cible = (essai == 0) ? &idxA : &idxB;
    if (*cible >= 0) continue;
    uint32_t capacite = 0;
    uint8_t *libre = md_replayer_zone_libre(&capacite);
    if (!libre || octets_max > capacite)
      break;                              // banque pleine : on fera avec une
    const int n = md_replayer_add_sample("KICK", libre, MD_KICK_IMAGES_MAX, 16,
                                         MD_PCM_RATE, -1, 49);
    if (n < 0) break;
    if (essai == 1) md_replayer_set_sample_length(n, 0);   // vide en attendant
    *cible = n;
  }
  if (idxA < 0 && idxB < 0)
    return -1;

  // On écrit dans la zone inactive si elle est libre, sinon dans l'autre — et
  // si les deux sonnent encore, l'appelant redemandera dans un instant.
  int idx = -1;
  if (idxB >= 0 &&
      !ds_voix_lit_zone(md_banque + current_module->samples[idxB].offset,
                        octets_max))
    idx = idxB;
  else if (idxA >= 0 &&
           !ds_voix_lit_zone(md_banque + current_module->samples[idxA].offset,
                             octets_max))
    idx = idxA;
  if (idx < 0)
    return -2;

  md_sample_t *sm = &current_module->samples[idx];
  uint8_t *zone = md_banque + sm->offset;
  // Si c'est la zone ACTIVE qu'on réécrit, aucune note ne doit démarrer dessus
  // pendant le calcul : sa longueur passe à zéro le temps de l'écriture.
  if (idx == (int)ins->pcm_sample) {
    md_lock(&replayer_lock);
    sm->length = 0;
    md_unlock(&replayer_lock);
  }

  // La hauteur plonge vers 50 Hz depuis 50 Hz multipliés par la profondeur.
  const int32_t f_fin = 50 << 8;                        // Q8, en hertz
  int32_t f = f_fin + (int32_t)ins->kick_drop * (f_fin / 24);
  // Durées, en nombre d'échantillons : chaque pas retire au reste sa fraction,
  // ce qui donne une descente exponentielle sans exponentielle à calculer.
  const uint32_t nChute = 32u + (uint32_t)ins->kick_time * 8u;
  const uint32_t nDeclin = 64u + (uint32_t)ins->kick_decay * 64u;
  const uint32_t nClic = 24u;
  const int32_t gain = 32 + (int32_t)ins->kick_drive;   // 32 = pas de saturation

  uint32_t phase = 0;
  int32_t env = 65535;                                  // Q16
  int32_t clic = (int32_t)ins->kick_click << 8;
  uint32_t alea = 0x13579BDFu;

  for (uint32_t i = 0; i < images; i++) {
    const int32_t sinus = md_sinus[(phase >> 22) & (MD_KICK_SIN - 1)];
    int32_t v = (int32_t)(((int64_t)sinus * env) >> 16);
    // Le clic d'attaque : un grain de bruit, très court, sur le départ.
    if (clic > 0) {
      alea = alea * 1664525u + 1013904223u;
      v += (int32_t)((((int64_t)(int16_t)(alea >> 16)) * clic) >> 16);
      clic -= clic / (int32_t)nClic + 1;
    }
    // La saturation : on pousse, puis on écrête. C'est elle qui fait le gros
    // kick, et l'écrêtage franc est voulu.
    v = (int32_t)(((int64_t)v * gain) >> 5);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    zone[i * 2]     = (uint8_t)(v & 0xFF);
    zone[i * 2 + 1] = (uint8_t)((v >> 8) & 0xFF);

    // Hauteur et amplitude avancent d'un pas.
    phase += (uint32_t)(((uint64_t)f << 24) / MD_PCM_RATE);
    f -= (f - f_fin) / (int32_t)nChute + 1;
    if (f < f_fin) f = f_fin;
    env -= env / (int32_t)nDeclin + 1;
    if (env < 0) env = 0;
  }

  // L'ARM7 lit la banque sans passer par le cache de l'ARM9.
  DC_FlushRange(zone, images * 2);
  md_replayer_set_sample_length(idx, images);
  // On bascule : les notes suivantes joueront le nouveau coup, tandis que
  // celle qui sonne finit tranquillement sur l'autre zone.
  md_lock(&replayer_lock);
  const int autre = (idx == idxA) ? idxB : idxA;
  ins->pcm_sample = (uint8_t)idx;
  ins->kick_alt = (autre >= 0) ? (uint8_t)autre : MD_EMPTY;
  md_unlock(&replayer_lock);
  md_push_instrument_update(ins_idx);
  return idx;
}

// ── Le calage avec le MIDI ──────────────────────────────────────────────────
void md_replayer_set_sync_ms(int ms) {
  // Signe : > 0 les samples attendent, < 0 c'est l'OPL3 qui attend.
  if (ms < -250) ms = -250;
  if (ms > 250) ms = 250;
  md_sync_ms = ms;
  ds_voix_regle_retard(ms);
}

int md_replayer_get_sync_ms(void) { return md_sync_ms; }

void md_replayer_set_instr_kick_val(int ins_idx, int which, int val) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return;
  if (val < 0) val = 0;
  if (val > 255) val = 255;
  md_instr_t *ins = &current_module->instruments[ins_idx - 1];
  switch (which) {
  case MD_KICK_DROP:  ins->kick_drop = (uint8_t)val; break;
  case MD_KICK_TIME:  ins->kick_time = (uint8_t)val; break;
  case MD_KICK_DECAY: ins->kick_decay = (uint8_t)val; break;
  case MD_KICK_CLICK: ins->kick_click = (uint8_t)val; break;
  case MD_KICK_DRIVE: ins->kick_drive = (uint8_t)val; break;
  default: return;
  }
  // La fabrication est demandée par l'appelant : lui seul peut redemander si
  // les deux zones sonnent encore (md_replayer_rend_kick rend alors -2).
}

int md_replayer_get_instr_kick_val(int ins_idx, int which) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return 0;
  const md_instr_t *ins = &current_module->instruments[ins_idx - 1];
  switch (which) {
  case MD_KICK_DROP:  return ins->kick_drop;
  case MD_KICK_TIME:  return ins->kick_time;
  case MD_KICK_DECAY: return ins->kick_decay;
  case MD_KICK_CLICK: return ins->kick_click;
  case MD_KICK_DRIVE: return ins->kick_drive;
  default: return 0;
  }
}

void md_replayer_set_instr_midi_length(int ins_idx, int ticks) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return;
  if (ticks < 0) ticks = 0;
  if (ticks > 255) ticks = 255;
  current_module->instruments[ins_idx - 1].midi_length = (uint8_t)ticks;
  md_push_instrument_update(ins_idx);
}

int md_replayer_get_instr_midi_length(int ins_idx) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return 0;
  return current_module->instruments[ins_idx - 1].midi_length;
}

int md_replayer_get_instr_kind(int ins_idx) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS)
    return MD_INSTR_KIND_ANY;
  return current_module->instruments[ins_idx - 1].kind;
}

void md_replayer_set_psg_vol_macro(int ins_idx, const uint8_t *vals, int len,
                                   int loop) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return;
  md_instr_t *ins = &current_module->instruments[ins_idx - 1];
  if (len < 0) len = 0;
  if (len > MD_PSG_MACRO_MAX) len = MD_PSG_MACRO_MAX;
  ins->psg_vol_len = (uint8_t)len;
  ins->psg_vol_loop = (loop >= 0 && loop < len) ? (uint8_t)loop : MD_EMPTY;
  for (int i = 0; i < MD_PSG_MACRO_MAX; i++)
    ins->psg_vol_mac[i] = (vals && i < len) ? (vals[i] > 15 ? 15 : vals[i]) : 0;
}

int md_replayer_get_psg_vol_macro(int ins_idx, uint8_t *out, int max_len,
                                  int *loop) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return 0;
  const md_instr_t *ins = &current_module->instruments[ins_idx - 1];
  int n = ins->psg_vol_len;
  if (out) for (int i = 0; i < n && i < max_len; i++) out[i] = ins->psg_vol_mac[i];
  if (loop) *loop = ins->psg_vol_loop;
  return n;
}

void md_replayer_set_psg_arp_macro(int ins_idx, const int8_t *vals, int len,
                                   int loop, bool fixed) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return;
  md_instr_t *ins = &current_module->instruments[ins_idx - 1];
  if (len < 0) len = 0;
  if (len > MD_PSG_MACRO_MAX) len = MD_PSG_MACRO_MAX;
  ins->psg_arp_len = (uint8_t)len;
  ins->psg_arp_loop = (loop >= 0 && loop < len) ? (uint8_t)loop : MD_EMPTY;
  ins->psg_arp_fixed = fixed ? 1 : 0;
  for (int i = 0; i < MD_PSG_MACRO_MAX; i++)
    ins->psg_arp_mac[i] = (vals && i < len) ? vals[i] : 0;
}

int md_replayer_get_psg_arp_macro(int ins_idx, int8_t *out, int max_len,
                                  int *loop, bool *fixed) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return 0;
  const md_instr_t *ins = &current_module->instruments[ins_idx - 1];
  int n = ins->psg_arp_len;
  if (out) for (int i = 0; i < n && i < max_len; i++) out[i] = ins->psg_arp_mac[i];
  if (loop) *loop = ins->psg_arp_loop;
  if (fixed) *fixed = ins->psg_arp_fixed != 0;
  return n;
}

static md_instr_t *md_psg_instr_at(int ins_idx) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS)
    return NULL;
  return &current_module->instruments[ins_idx - 1];
}

// ── Enveloppe des voies PSG (écran ENV.) ────────────────────────────────────
// Trois points « amplitude / vitesse ». Amplitude MD_EMPTY = point désactivé,
// ce qui désactive aussi ceux qui suivent. Vitesse 0 = tenir, 1 = le plus
// rapide, 15 = le plus lent.

int md_replayer_get_env_amp(int ins_idx, int point) {
  const md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (!ins || point < 0 || point >= MD_ENV_POINTS)
    return MD_EMPTY;
  return ins->env_amp[point];
}

int md_replayer_get_env_speed(int ins_idx, int point) {
  const md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (!ins || point < 0 || point >= MD_ENV_POINTS)
    return 0;
  return ins->env_speed[point];
}

void md_replayer_set_env_amp(int ins_idx, int point, int amp) {
  if (point < 0 || point >= MD_ENV_POINTS)
    return;
  md_lock(&replayer_lock);
  md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (ins) {
    if (amp == MD_EMPTY || amp < 0) {
      // Désactiver un point désactive la suite : une enveloppe ne peut pas
      // avoir de trou.
      if (point == 0) {
        ins->env_amp[0] = 0;          // le premier point existe toujours
      } else {
        for (int i = point; i < MD_ENV_POINTS; i++)
          ins->env_amp[i] = MD_EMPTY;
      }
    } else {
      ins->env_amp[point] = (uint8_t)(amp > 15 ? 15 : amp);
      // Réactiver un point réactive ceux qui le précèdent.
      for (int i = 0; i < point; i++)
        if (ins->env_amp[i] == MD_EMPTY)
          ins->env_amp[i] = 0;
    }
  }
  md_unlock(&replayer_lock);
}

void md_replayer_set_env_speed(int ins_idx, int point, int speed) {
  if (point < 0 || point >= MD_ENV_POINTS)
    return;
  if (speed < 0) speed = 0;
  if (speed > 15) speed = 15;
  md_lock(&replayer_lock);
  md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (ins)
    ins->env_speed[point] = (uint8_t)speed;
  md_unlock(&replayer_lock);
}

// ── Macro de mode de bruit (accès pour l'éditeur) ──────────────────────────
// Même contrat que la table de volumes : écritures sous verrou, `loop` à
// MD_EMPTY quand il n'y a pas de bouclage.

int md_replayer_get_psg_noise_macro(int ins_idx, uint8_t *out, int max_len,
                                    uint8_t *loop) {
  if (loop) *loop = MD_EMPTY;
  const md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (!ins)
    return 0;
  int len = ins->psg_noise_len;
  if (len > MD_PSG_ENV_MAX)
    len = MD_PSG_ENV_MAX;
  if (out && max_len > 0) {
    int n = len < max_len ? len : max_len;
    for (int i = 0; i < n; i++)
      out[i] = ins->psg_noise_mac[i];
  }
  if (loop) *loop = ins->psg_noise_loop;
  return len;
}

void md_replayer_set_psg_noise_step(int ins_idx, int index, int value) {
  if (index < 0 || index >= MD_PSG_ENV_MAX)
    return;
  md_lock(&replayer_lock);
  md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (ins) {
    ins->psg_noise_mac[index] = (uint8_t)md_clamp_u8(value, 7);
    if (index >= ins->psg_noise_len)
      ins->psg_noise_len = (uint8_t)(index + 1);
  }
  md_unlock(&replayer_lock);
}

void md_replayer_set_psg_noise_len(int ins_idx, int len) {
  if (len < 0) len = 0;
  if (len > MD_PSG_ENV_MAX) len = MD_PSG_ENV_MAX;
  md_lock(&replayer_lock);
  md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (ins) {
    ins->psg_noise_len = (uint8_t)len;
    if (ins->psg_noise_loop != MD_EMPTY && ins->psg_noise_loop >= len)
      ins->psg_noise_loop = MD_EMPTY;
  }
  md_unlock(&replayer_lock);
}

void md_replayer_set_psg_noise_loop(int ins_idx, int loop) {
  md_lock(&replayer_lock);
  md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (ins)
    ins->psg_noise_loop =
        (loop < 0 || loop >= ins->psg_noise_len) ? MD_EMPTY : (uint8_t)loop;
  md_unlock(&replayer_lock);
}

// ── Tables d'instrument ─────────────────────────────────────────────────────
// Une table se déroule d'une ligne par tick tant que la note dure, puis
// reboucle. Elle s'applique à n'importe quel canal, FM comme PSG : la colonne
// VOL fixe le volume, TSP transpose la note, et les deux colonnes CMD passent
// leurs commandes au moteur d'effets par son second emplacement (le premier
// reste à la phrase, exactement comme dans un tracker).

// ── Commandes des colonnes CMD ──────────────────────────────────────────────
// L'ordre fixe la valeur stockée dans les fichiers : ne pas réordonner sans
// convertir les .mdm existants.
//
// Les lettres sont celles de LSDJ (manuel 9.2.6, chapitre 4) ; l'effet est
// celui du moteur Mega Drive. Certaines commandes doivent transformer leur
// valeur avant de la passer au moteur : les effets « étendus » codent leur
// sous-commande dans le quartet HAUT, si bien qu'un K03 (couper après 3 ticks)
// doit devenir 0x33. D'où `val_mask` et `val_or`.
//
// `special` marque les commandes que la table ou la phrase traitent
// elles-mêmes, sans passer par le moteur d'effets.
static const struct {
  char letter;
  int effect;          // -1 si `kind` != MD_CMD_PLAIN
  uint8_t val_mask;    // masque appliqué à la valeur saisie
  uint8_t val_or;      // bits ajoutés ensuite (sous-commande étendue)
  md_cmd_kind_t kind;
} md_table_cmds[] = {
    {'A', -1,                        0xFF, 0x00, MD_CMD_TABLE},
    {'C', MD_EFF_ARPEGGIO,           0xFF, 0x00, MD_CMD_PLAIN},
    {'D', MD_EFF_EXTENDED2,          0x0F, MD_EX2_NOTE_DELAY << 4, MD_CMD_PLAIN},
    {'H', -1,                        0xFF, 0x00, MD_CMD_HOP},
    {'K', MD_EFF_EXTENDED2,          0x0F, MD_EX2_NOTE_CUT << 4, MD_CMD_PLAIN},
    {'L', MD_EFF_TONE_PORTAMENTO,    0xFF, 0x00, MD_CMD_PLAIN},
    {'M', MD_EFF_SET_GLOBAL_VOLUME,  0xFF, 0x00, MD_CMD_PLAIN},
    {'O', MD_EFF_EXTENDED,           0x0F, MD_EX_SET_PANNING_POS << 4, MD_CMD_PLAIN},
    {'P', -1,                        0xFF, 0x00, MD_CMD_PITCH},
    {'R', MD_EFF_RETRIG_NOTE,        0xFF, 0x00, MD_CMD_PLAIN},
    {'T', MD_EFF_SET_TEMPO,          0xFF, 0x00, MD_CMD_PLAIN},
    {'V', MD_EFF_VIBRATO,            0xFF, 0x00, MD_CMD_PLAIN},
    {'Z', MD_EFF_TREMOLO,            0xFF, 0x00, MD_CMD_PLAIN},
    // ── Effets standard de DefleMask sans équivalent LSDJ ────────────────
    {'B', MD_EFF_VOL_SLIDE,          0xFF, 0x00, MD_CMD_PLAIN},  // Axy
    {'E', MD_EFF_TPORTA_VSLIDE,      0xFF, 0x00, MD_CMD_PLAIN},  // 5xy
    {'F', MD_EFF_VIBRATO_VSLIDE,     0xFF, 0x00, MD_CMD_PLAIN},  // 6xy
    {'S', MD_EFF_SET_SPEED,          0xFF, 0x00, MD_CMD_PLAIN},  // 9xx / Fxx
    {'J', MD_EFF_POSITION_JUMP,      0xFF, 0x00, MD_CMD_PLAIN},  // Bxx
    {'N', MD_EFF_PATTERN_BREAK,      0xFF, 0x00, MD_CMD_PLAIN},  // Dxx
    {'W', MD_EFF_EXTENDED,           0x0F, MD_EX_SET_VIB_DEPTH << 4, MD_CMD_PLAIN}, // E4xx
    {'U', -1,                        0xFF, 0x00, MD_CMD_FINETUNE},                  // E5xx
};

int md_table_cmd_count(void) {
  return (int)(sizeof(md_table_cmds) / sizeof(md_table_cmds[0]));
}

char md_table_cmd_letter(int i) {
  if (i < 0 || i >= md_table_cmd_count())
    return 0;
  return md_table_cmds[i].letter;
}

int md_table_cmd_effect(int i) {
  if (i < 0 || i >= md_table_cmd_count())
    return -1;
  return md_table_cmds[i].effect;
}

// Valeur réellement transmise au moteur d'effets pour cette commande.
uint8_t md_table_cmd_value(int i, uint8_t val) {
  if (i < 0 || i >= md_table_cmd_count())
    return val;
  return (uint8_t)((val & md_table_cmds[i].val_mask) | md_table_cmds[i].val_or);
}

// Résout une commande en (effet, valeur) réellement transmis au moteur.
//
// Nécessaire pour P, dont la valeur est SIGNÉE comme dans LSDJ : 01-7F fait
// monter la hauteur, 80-FF la fait descendre (FE = -2). Une seule commande
// suffit donc pour les deux sens.
void md_table_cmd_resolve(int i, uint8_t val, int *out_eff, uint8_t *out_val) {
  int eff = md_table_cmd_effect(i);
  uint8_t v = md_table_cmd_value(i, val);
  if (i >= 0 && i < md_table_cmd_count() &&
      md_table_cmds[i].kind == MD_CMD_FINETUNE) {
    // Comme le E5xx de DefleMask : 80 est le centre, au-dessus on monte, en
    // dessous on descend. Notre effet étendu n'a que 4 bits, l'écart sature.
    int d = (int)val - 0x80;
    int mag = d < 0 ? -d : d;
    if (mag > 15) mag = 15;
    if (d == 0) {
      eff = -1; v = 0;
    } else {
      eff = MD_EFF_EXTENDED2;
      v = (uint8_t)(((d > 0 ? MD_EX2_FINE_TUNE_UP : MD_EX2_FINE_TUNE_DOWN) << 4)
                    | mag);
    }
  } else if (i >= 0 && i < md_table_cmd_count() &&
      md_table_cmds[i].kind == MD_CMD_PITCH) {
    if (val == 0) {
      eff = -1;                       // P00 arrête le bend
      v = 0;
    } else if (val < 0x80) {
      eff = MD_EFF_FSLIDE_UP;
      v = val;
    } else {
      eff = MD_EFF_FSLIDE_DOWN;
      v = (uint8_t)(256 - val);
    }
  }
  if (out_eff) *out_eff = eff;
  if (out_val) *out_val = v;
}

// La commande est-elle traitée par la table elle-même plutôt que par le moteur
// d'effets ? (H pour les sauts, A pour démarrer/arrêter une table.)
int md_table_cmd_kind(int i) {
  if (i < 0 || i >= md_table_cmd_count())
    return MD_CMD_PLAIN;
  return (int)md_table_cmds[i].kind;
}

void md_replayer_get_table_row(int table, int row, uint8_t *vol,
                               int8_t *transpose, uint8_t *cmd1, uint8_t *val1,
                               uint8_t *cmd2, uint8_t *val2,
                               uint8_t *mdcmd, uint8_t *mdval) {
  if (vol) *vol = MD_EMPTY;
  if (transpose) *transpose = 0;
  if (cmd1) *cmd1 = MD_EMPTY;
  if (val1) *val1 = 0;
  if (cmd2) *cmd2 = MD_EMPTY;
  if (val2) *val2 = 0;
  if (mdcmd) *mdcmd = MD_EMPTY;
  if (mdval) *mdval = 0;
  if (!current_module || table < 0 || table >= MD_MAX_TABLES || row < 0 ||
      row >= MD_TABLE_ROWS)
    return;
  const md_table_row_t *r = &current_module->tables[table].rows[row];
  if (vol) *vol = r->vol;
  if (transpose) *transpose = r->transpose;
  if (cmd1) *cmd1 = r->cmd1;
  if (val1) *val1 = r->val1;
  if (cmd2) *cmd2 = r->cmd2;
  if (val2) *val2 = r->val2;
  if (mdcmd) *mdcmd = r->mdcmd;
  if (mdval) *mdval = r->mdval;
}

// Une table vient d'être modifiée : les canaux qui la jouent doivent repartir
// de l'instrument PROPRE, sinon un réglage de registre posé par la ligne qu'on
// vient de changer reste écrit dans la copie de travail. On n'entendait alors
// sa modification — ou son retrait — qu'après un stop/play, qui seul remettait
// tout à plat.
//
// Ce qui vient d'une PHRASE est réappliqué juste après : lui doit survivre,
// c'est la règle des commandes qui tiennent d'une note à l'autre.
static void md_refresh_table(int table) {
  if (!current_module) return;

  // La vitesse et le tempo reviennent à ceux du MORCEAU. Une commande de
  // vitesse survolée en éditant ralentit la lecture tant qu'elle est écrite —
  // c'est normal, une Game Boy fait pareil — mais dès qu'on passe à autre
  // chose tout doit repartir, sans avoir à relancer la lecture.
  // Le volume global aussi : une commande M qu'on vient de retirer ne doit pas
  // laisser le morceau assourdi.
  replay_global_vol = 63;
  replay_speed = current_module->initial_speed > 0 ? current_module->initial_speed : 6;
  replay_tempo = current_module->initial_tempo >= 18 ? current_module->initial_tempo : 125;
  {
    int macro = current_module->macro_speedup > 0 ? current_module->macro_speedup : 1;
    int irq = replay_tempo * macro;
    if (irq <= 0) irq = 50;
    samples_per_tick =
        (int)((double)replay_sample_rate / ((double)irq * md_finetune_scale()));
    if (samples_per_tick < 1) samples_per_tick = 1;
    // La ligne EN COURS garde son décompte : après un « S FF » elle tiendrait
    // encore 255 ticks avant de céder, et la lecture semblerait rester bloquée
    // malgré le retour à la vitesse normale.
    if (replay_ticks_left > replay_speed) replay_ticks_left = replay_speed;
  }

  for (int c = 0; c < MD_TOTAL_VOICES; c++) {
    // Une voie DÉTOURNÉE compte aussi, même si elle ne joue plus la table
    // éditée. En modifiant une commande on passe forcément par le premier cran
    // de la liste — la commande A, qui change de table — et un « A 00 » survolé
    // au passage verrouillait la voie sur la table 00 : les repères
    // disparaissaient, l'effet ne s'entendait plus, et seul un stop/play
    // défaisait le verrou. On rend donc la voie à la table de son instrument ;
    // si un A y est encore écrit, il se réappliquera au prochain passage.
    if (ch_table_forced[c]) {
      ch_table_forced[c] = false;
      md_table_start(c);
    }
    if (ch_table[c] != table || !ch_table_running[c]) continue;
    for (int i = 0; i < MD_MDCMD_SLOTS; i++)
      if (ch_mdcmd_from_table[c][i]) { ch_mdcmd_on[c][i] = false; ch_mdcmd_from_table[c][i] = false; }
    int ins = ch_current_instr[c];
    if (ins >= 1 && ins <= MD_MAX_INSTRUMENTS)
      md_set_instrument((uint8_t)c, &current_module->instruments[ins - 1]);
    md_reapply_mdcmds(c);
    // Et les effets de la table se réarmeront au prochain passage.
    for (int sl = 0; sl < MD_TABLE_STREAMS - 1; sl++)
      ch_active_eff[c][MD_EFF_SLOT_TABLE + sl] = 0xFF;
  }
}

void md_replayer_set_table_row(int table, int row, uint8_t vol,
                               int8_t transpose, uint8_t cmd1, uint8_t val1,
                               uint8_t cmd2, uint8_t val2,
                               uint8_t mdcmd, uint8_t mdval) {
  if (table < 0 || table >= MD_MAX_TABLES || row < 0 || row >= MD_TABLE_ROWS)
    return;
  md_lock(&replayer_lock);
  if (current_module) {
    md_table_row_t *r = &current_module->tables[table].rows[row];
    r->vol = vol;
    r->transpose = transpose;
    r->cmd1 = cmd1;
    r->val1 = val1;
    r->cmd2 = cmd2;
    r->val2 = val2;
    r->mdcmd = mdcmd;
    r->mdval = mdval;
    md_refresh_table(table);
  }
  md_unlock(&replayer_lock);
}

bool md_replayer_table_is_empty(int table) {
  if (!current_module || table < 0 || table >= MD_MAX_TABLES)
    return true;
  const md_table_t *t = &current_module->tables[table];
  for (int r = 0; r < MD_TABLE_ROWS; r++) {
    const md_table_row_t *row = &t->rows[r];
    if (row->vol != 0 || row->transpose != 0 ||
        row->cmd1 != MD_EMPTY || row->cmd2 != MD_EMPTY ||
        row->mdcmd != MD_EMPTY)
      return false;
  }
  return true;
}

int md_replayer_get_instr_table(int ins_idx) {
  const md_instr_t *ins = md_psg_instr_at(ins_idx);
  return ins ? ins->table : MD_EMPTY;
}

void md_replayer_set_instr_table(int ins_idx, int table) {
  md_lock(&replayer_lock);
  md_instr_t *ins = md_psg_instr_at(ins_idx);
  if (ins)
    ins->table = (table < 0 || table >= MD_MAX_TABLES) ? MD_EMPTY
                                                       : (uint8_t)table;
  md_unlock(&replayer_lock);
}

int md_replayer_play_table(int channel) {
  if (channel < 0 || channel >= MD_TOTAL_VOICES)
    return -1;
  return ch_table_running[channel] ? ch_table[channel] : -1;
}

int md_replayer_play_table_pos(int channel, int stream) {
  if (channel < 0 || channel >= MD_TOTAL_VOICES || stream < 0 ||
      stream >= MD_TABLE_STREAMS)
    return -1;
  return ch_table_running[channel] ? ch_table_pos[channel][stream] : -1;
}

// ── Portée de la lecture ────────────────────────────────────────────────────
// Appelée par l'interface juste avant de lancer la lecture, selon l'écran où se
// trouve le curseur. Ne prend effet qu'au prochain démarrage.
void md_replayer_set_play_scope(int scope, int channel, int id) {
  scope_tsp_chain = -1;   // (md_replayer_set_scope_chain, après)
  md_lock(&replayer_lock);
  if (scope < MD_SCOPE_SONG || scope > MD_SCOPE_PHRASE)
    scope = MD_SCOPE_SONG;
  play_scope = (md_play_scope_t)scope;
  scope_channel = (channel < 0 || channel >= MD_MAX_CHANNELS) ? 0 : channel;
  if (play_scope == MD_SCOPE_PHRASE)
    scope_id = (id < 0 || id >= MD_MAX_PHRASES) ? 0 : id;
  else if (play_scope == MD_SCOPE_CHAIN)
    scope_id = (id < 0 || id >= MD_MAX_CHAINS) ? 0 : id;
  else
    scope_id = 0;
  md_unlock(&replayer_lock);
}

// ── Modèle de puce FM ───────────────────────────────────────────────────────
// Simple relais vers la couche puce, pour que l'interface n'ait pas à inclure
// md_chip.h (qui est du C++ derrière son extern "C").
void md_replayer_set_ladder(bool enabled) { md_chip_set_ladder(enabled); }
bool md_replayer_get_ladder(void) { return md_chip_get_ladder(); }

// ── Commandes « MD » : les réglages de la machine ────────────────────────────
//
// Elles reprennent les paramètres que GenMDM expose en MIDI CC — algorithme,
// feedback, niveaux et enveloppes des quatre opérateurs, LFO, stéréo, bruit
// PSG — mais écrits « lettre + deux chiffres » comme le reste du tracker.
//
// Pour un paramètre d'OPÉRATEUR, le premier chiffre est le numéro d'opérateur
// (1-4) et le second la valeur ; pour un paramètre de CANAL, les deux chiffres
// forment la valeur. Un paramètre d'opérateur n'a donc que 4 bits de
// résolution : le Total Level, qui va de 0 à 127, avance par pas de 8. C'est le
// prix d'une case à trois caractères, et ça reste largement suffisant pour
// balayer un timbre en cours de note.
typedef enum {
  // ── Le jeu de DefleMask pour Megadrive (manuel v2.0.0) ────────────────
  MD_P_LFO = 0,   // 10xy
  MD_P_FB,        // 11xx
  MD_P_TL1, MD_P_TL2, MD_P_TL3, MD_P_TL4,   // 12xx…15xx
  MD_P_MUL,       // 16xy
  MD_P_ARALL,     // 19xx
  MD_P_AR1, MD_P_AR2, MD_P_AR3, MD_P_AR4,   // 1Axx…1Dxx
  MD_P_NOISE,     // 20xy
  // ── Au-delà de DefleMask : ce que la puce sait faire et qu'ils n'ont pas
  //    prévu. Rien n'oblige à s'en servir, mais rien ne justifiait de les
  //    retirer — ils n'entrent en conflit avec aucun effet du .dmf.
  MD_P_ALG, MD_P_PAN, MD_P_PMS, MD_P_AMS,
  MD_P_DT, MD_P_RS, MD_P_D1R, MD_P_D2R, MD_P_D1L, MD_P_RR, MD_P_AM, MD_P_SSG,
  MD_P_EFFECT,   // pas un registre : un effet, traité par le moteur d'effets
} md_param_t;

static const struct {
  uint8_t code;        // code d'effet DefleMask, tel qu'écrit dans son manuel
  md_param_t param;    // MD_P_EFFECT : effet, sinon registre de la puce
  bool per_op;         // le premier chiffre est un numéro d'opérateur
  char equiv;          // commande CMD qui fait doublon (0 : aucune)
  int effect;          // les quatre champs suivants ne servent qu'aux effets
  uint8_t val_mask;
  uint8_t val_or;
  md_cmd_kind_t kind;
} md_mdcmds[] = {
    // ── Effets standard de DefleMask ─────────────────────────────────────
    // `equiv` nomme la commande CMD qui fait déjà la même chose. Quand les
    // deux colonnes la portent sur une même ligne, c'est CMD qui gagne et
    // celle-ci est ignorée : un .dmf importé peut donc tout déverser ici
    // sans jamais entrer en conflit avec ce qui est écrit à la main.
    {0x00, MD_P_EFFECT, false, 'C', MD_EFF_ARPEGGIO,        0xFF, 0x00, MD_CMD_PLAIN},
    {0x01, MD_P_EFFECT, false, 'P', MD_EFF_FSLIDE_UP,       0xFF, 0x00, MD_CMD_PLAIN},
    {0x02, MD_P_EFFECT, false, 'P', MD_EFF_FSLIDE_DOWN,     0xFF, 0x00, MD_CMD_PLAIN},
    {0x03, MD_P_EFFECT, false, 'L', MD_EFF_TONE_PORTAMENTO, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x04, MD_P_EFFECT, false, 'V', MD_EFF_VIBRATO,         0xFF, 0x00, MD_CMD_PLAIN},
    {0x05, MD_P_EFFECT, false, 'E', MD_EFF_TPORTA_VSLIDE,   0xFF, 0x00, MD_CMD_PLAIN},
    {0x06, MD_P_EFFECT, false, 'F', MD_EFF_VIBRATO_VSLIDE,  0xFF, 0x00, MD_CMD_PLAIN},
    {0x07, MD_P_EFFECT, false, 'Z', MD_EFF_TREMOLO,         0xFF, 0x00, MD_CMD_PLAIN},
    {0x08, MD_P_PAN,    false, 'O', -1,                     0xFF, 0x00, MD_CMD_PLAIN},
    {0x09, MD_P_EFFECT, false, 'S', MD_EFF_SET_SPEED,       0xFF, 0x00, MD_CMD_PLAIN},
    {0x0A, MD_P_EFFECT, false, 'B', MD_EFF_VOL_SLIDE,       0xFF, 0x00, MD_CMD_PLAIN},
    {0x0B, MD_P_EFFECT, false, 'J', MD_EFF_POSITION_JUMP,   0xFF, 0x00, MD_CMD_PLAIN},
    {0x0C, MD_P_EFFECT, false, 'R', MD_EFF_RETRIG_NOTE,     0xFF, 0x00, MD_CMD_PLAIN},
    {0x0D, MD_P_EFFECT, false, 'N', MD_EFF_PATTERN_BREAK,   0xFF, 0x00, MD_CMD_PLAIN},
    {0x0F, MD_P_EFFECT, false, 'S', MD_EFF_SET_SPEED,       0xFF, 0x00, MD_CMD_PLAIN},
    // ── Commandes étendues ───────────────────────────────────────────────
    {0xE4, MD_P_EFFECT, false, 'W', MD_EFF_EXTENDED,  0x0F, MD_EX_SET_VIB_DEPTH << 4, MD_CMD_PLAIN},
    {0xE5, MD_P_EFFECT, false, 'U', -1,               0xFF, 0x00, MD_CMD_FINETUNE},
    {0xEC, MD_P_EFFECT, false, 'K', MD_EFF_EXTENDED2, 0x0F, MD_EX2_NOTE_CUT << 4,     MD_CMD_PLAIN},
    {0xED, MD_P_EFFECT, false, 'D', MD_EFF_EXTENDED2, 0x0F, MD_EX2_NOTE_DELAY << 4,   MD_CMD_PLAIN},
    // ── Effets Megadrive de DefleMask ────────────────────────────────────
    {0x10, MD_P_LFO,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},  // x marche, y vitesse
    {0x11, MD_P_FB,    false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},  // feedback 0-7
    {0x12, MD_P_TL1,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},  // TL op 1, 00-7F
    {0x13, MD_P_TL2,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x14, MD_P_TL3,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x15, MD_P_TL4,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x16, MD_P_MUL,   true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},  // x opérateur, y multiple
    {0x19, MD_P_ARALL, false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},  // attaque des quatre
    {0x1A, MD_P_AR1,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x1B, MD_P_AR2,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x1C, MD_P_AR3,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x1D, MD_P_AR4,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x20, MD_P_NOISE, false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},  // mode de bruit PSG
    // ── En plus : la puce le sait, DefleMask ne l'expose pas ─────────────
    // Codes pris au-dessus de 20, que DefleMask n'utilise pas : aucun risque
    // de collision avec un effet d'un fichier importé.
    {0x30, MD_P_ALG,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},  // algorithme 0-7
    {0x31, MD_P_PAN,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},  // 0 centre, 1 g., 2 d.
    {0x32, MD_P_PMS,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x33, MD_P_AMS,   false, 0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x34, MD_P_DT,    true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x35, MD_P_RS,    true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x36, MD_P_D1R,   true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x37, MD_P_D2R,   true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x38, MD_P_D1L,   true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x39, MD_P_RR,    true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x3A, MD_P_AM,    true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
    {0x3B, MD_P_SSG,   true,  0, -1, 0xFF, 0x00, MD_CMD_PLAIN},
};

int md_mdcmd_count(void) {
  return (int)(sizeof(md_mdcmds) / sizeof(md_mdcmds[0]));
}
// Le code DefleMask de la commande : c'est lui qui s'affiche, sur deux
// chiffres hexadécimaux. Vingt-cinq lettres ne pouvaient pas porter les
// quarante-quatre effets, et ce codage est de toute façon celui du manuel.
uint8_t md_mdcmd_code(int i) {
  return (i < 0 || i >= md_mdcmd_count()) ? 0xFF : md_mdcmds[i].code;
}

// Retrouve une commande par son code DefleMask (-1 si inconnue).
int md_mdcmd_index_of_code(uint8_t code) {
  for (int i = 0; i < md_mdcmd_count(); i++)
    if (md_mdcmds[i].code == code)
      return i;
  return -1;
}

// Vrai si la commande passe par le moteur d'effets plutôt que par les
// registres de la puce.
bool md_mdcmd_is_effect(int i) {
  return (i < 0 || i >= md_mdcmd_count()) ? false
                                          : md_mdcmds[i].param == MD_P_EFFECT;
}

// La commande CMD qui ferait doublon avec celle-ci, 0 si aucune.
char md_mdcmd_equivalent(int i) {
  return (i < 0 || i >= md_mdcmd_count()) ? 0 : md_mdcmds[i].equiv;
}

// Même travail que md_table_cmd_resolve, pour la colonne MD CMD.
void md_mdcmd_resolve(int i, uint8_t val, int *out_eff, uint8_t *out_val) {
  int eff = -1;
  uint8_t v = val;
  if (i >= 0 && i < md_mdcmd_count() && md_mdcmds[i].param == MD_P_EFFECT) {
    eff = md_mdcmds[i].effect;
    v = (uint8_t)((val & md_mdcmds[i].val_mask) | md_mdcmds[i].val_or);
    if (md_mdcmds[i].kind == MD_CMD_FINETUNE) {
      // E5xx : 80 est le centre, au-dessus on monte, en dessous on descend.
      int d = (int)val - 0x80;
      int mag = d < 0 ? -d : d;
      if (mag > 15) mag = 15;
      if (d == 0) {
        eff = -1; v = 0;
      } else {
        eff = MD_EFF_EXTENDED2;
        v = (uint8_t)(((d > 0 ? MD_EX2_FINE_TUNE_UP : MD_EX2_FINE_TUNE_DOWN) << 4)
                      | mag);
      }
    }
  }
  if (out_eff) *out_eff = eff;
  if (out_val) *out_val = v;
}
bool md_mdcmd_per_operator(int i) {
  return (i < 0 || i >= md_mdcmd_count()) ? false : md_mdcmds[i].per_op;
}

// Applique une commande MD au canal : on modifie la copie de travail de
// l'instrument puis on repousse les registres. Le réglage vaut jusqu'à la
// prochaine note, qui rechargera l'instrument — c'est le comportement de
// GenMDM, où un CC agit sur la voix en cours.
static bool md_noise_forced(int c) {
  if (c < 0 || c >= MD_TOTAL_VOICES) return false;
  for (int i = 0; i < MD_MDCMD_SLOTS && i < md_mdcmd_count(); i++)
    if (ch_mdcmd_on[c][i] && md_mdcmds[i].param == MD_P_NOISE) return true;
  return false;
}

// ── La colonne CC ───────────────────────────────────────────────────────────
// Un CC part sur le canal de l'instrument MIDI de la piste. Sur une piste qui
// joue un échantillon il n'a pas de destinataire : on ne l'envoie pas.
static void md_apply_mdcmd(int c, uint8_t cmd, uint8_t val,
                           uint8_t row_cmd, int slot) {
  (void)row_cmd;
  (void)slot;
  if (c < 0 || c >= MD_TOTAL_VOICES || cmd == MD_EMPTY || cmd > 127)
    return;
  if (md_channel_is_pcm(c))
    return;
  xt_midi_send((uint8_t)(0xB0 | (ch_instr_shadow[c].midi_channel & 0x0F)),
               cmd, (uint8_t)(val & 0x7F));
}

// ── Correspondance des effets DefleMask ─────────────────────────────────────
// Depuis que MD CMD porte les codes du manuel, la traduction se réduit à
// chercher le code. Renvoie false pour ce que le moteur ne sait pas faire :
// 17 (DAC), 18 (mode EXT. CH3), E0, E1, E2, E3, EA, EB, EE, EF.
bool md_dmf_effect_map(uint8_t dmf_eff, uint8_t dmf_val,
                       int *out_cmd, uint8_t *out_val, bool *out_is_md) {
  int i = md_mdcmd_index_of_code(dmf_eff);
  if (i < 0)
    return false;
  if (out_cmd) *out_cmd = i;
  if (out_val) *out_val = dmf_val;
  if (out_is_md) *out_is_md = true;   // tout va dans la colonne MD CMD
  return true;
}

// ── La disto d'un instrument sample ─────────────────────────────────────────
int md_replayer_get_instr_disto(int ins_idx) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return 0;
  return MD_DISTO(&current_module->instruments[ins_idx - 1]);
}
void md_replayer_set_instr_disto(int ins_idx, int v) {
  if (!current_module || ins_idx < 1 || ins_idx > MD_MAX_INSTRUMENTS) return;
  MD_DISTO(&current_module->instruments[ins_idx - 1]) = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
  md_push_instrument_update(ins_idx);
}

// ── Longueur des patterns ───────────────────────────────────────────────────
int md_replayer_get_patt_len(void) { return current_module ? current_module->patt_len : 0; }
void md_replayer_set_patt_len(int n) {
  if (!current_module) return;
  if (n < 0) n = 0;
  if (n > MD_ROWS_PER_CHAIN * MD_ROWS_PER_PHRASE) n = MD_ROWS_PER_CHAIN * MD_ROWS_PER_PHRASE;
  current_module->patt_len = (uint16_t)n;
}

// ── Nombre de pistes ────────────────────────────────────────────────────────
int md_replayer_get_num_channels(void) {
  return current_module ? current_module->num_channels : MD_PISTES_OPL;
}

void md_replayer_set_num_channels(int n) {
  if (!current_module) return;
  if (n < 1) n = 1;
  if (n > MD_MAX_CHANNELS) n = MD_MAX_CHANNELS;
  md_lock(&replayer_lock);
  // Une piste retiree se tait.
  for (int c = n; c < current_module->num_channels; c++) {
    md_key_off(c);
    ch_running[c] = false;
  }
  current_module->num_channels = (uint8_t)n;
  md_unlock(&replayer_lock);
}
