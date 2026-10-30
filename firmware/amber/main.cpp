// ============================================================================
//  XM MIDI Tracker DS — le tracker de GeneTrackerDS, sur des echantillons et du
//  MIDI USB au lieu des puces Mega Drive.
//
//  Ecran du HAUT : l'ecran CRT, plein cadre, comme LSDJ.
//  Ecran du BAS : le nom du morceau et le nom des commandes.
//
//  Tout se pilote a la croix et aux boutons, a la LSDJ. L'ecran tactile n'est
//  pas utilise.
//
//  Copyright (C) 2026 Audren Thibault. Tous droits reserves.
// ============================================================================
#include <nds.h>
#include <fat.h>
#include <dirent.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

// Decode un WAV directement dans la banque du morceau, en gardant sa forme :
// 8 ou 16 bits, a sa cadence d'origine. Defini dans md_audio.cpp.
//   0 = charge, 1 = fichier illisible, 2 = pas assez de place dans la banque.
extern "C" int md_audio_charge_wav(const char *chemin, uint8_t *dest,
                                   uint32_t capacite, uint32_t *images,
                                   uint32_t *cadence, int *bits,
                                   int32_t *boucle, int *note_base);
// La sortie de debogage no$gba : on ecrit le POINTEUR de la chaine en
// 0x04FFFA18 et l'emulateur la recopie sur sa sortie standard, avec un retour
// a la ligne. melonDS l'implemente ; sur une vraie console cette adresse n'est
// pas cablee et l'ecriture ne fait rien. C'est le seul moyen d'obtenir un
// journal COPIABLE quand on teste dans un emulateur, ou il n'y a pas de carte.
// Sur la RG350, la meme ligne part sur la sortie d'erreur.
static inline void debogLigne(const char *m) {
  fputs(m, stderr);
  fputc('\n', stderr);
}
#include <stdio.h>
#include <string.h>

#include "md_font.h"

extern "C" {
#include "CustomReplayer/md_replayer.h"
#include "xt_midi.h"
#include "a2m_import.h"
}
#include "rg_son.h"

// L'ecran DS fait 256 x 192. Avec une cellule de 4 x 6 on obtient 64 colonnes
// sur 32 lignes — la densite qu'il faut pour dix canaux.
// Sur la RG350 : 320 x 209 (voir RG_ECRAN_L dans compat/nds.h).
static const int kEcranL = RG_ECRAN_L;
static const int kEcranH = RG_ECRAN_H;
static const int kCols   = kEcranL / MD_CELL_W;   // 64
static const int kLignes = kEcranH / MD_CELL_H;   // 32

static u16 *g_fond = nullptr;
static const char kHex[] = "0123456789ABCDEF";

// ── Couleurs du CRT ─────────────────────────────────────────────────────────
// Le vert du tracker, sur un noir tres legerement bleute : un noir pur fait
// « ecran eteint », alors qu'un tube en a toujours un peu sous la main.
static inline u16 rvb(int r, int v, int b) { return ARGB16(1, r, v, b); }
// ── La palette du CRT, reprise de LSDJPal cote iPad ────────────────────────
// Ce n'est pas une invention : ce sont les valeurs du tracker de reference,
// converties en cinq bits par composante. Le fond est noir, le texte un violet
// clair, et le repere de lecture un rouge franc.
//   fg      #C4AAFF a 71 %   accent #E94754
static const u16 kFond    = rvb(0, 0, 0);
// Les valeurs de l'iPad sont conservees en TEINTE mais remontees en clarte :
// l'ecran de la DS est bien moins lumineux qu'une dalle d'iPad, et un violet a
// 71 % y devient sourd — les lettres semblent baver faute de contraste avec le
// noir. On garde donc le meme violet, plus franc.
static const u16 kTexte   = rvb(29, 27, 31);
static const u16 kAttenue = rvb(23, 20, 30);
static const u16 kEntete  = rvb(29, 27, 31);
static const u16 kAccent  = rvb(28, 9, 10);

// Les numeros de ligne alternent par GROUPE DE QUATRE — une teinte de texte ET
// de fond par groupe, exactement comme LSDJ. C'est ce qui donne le rythme a
// l'oeil : sans ca, deux cent cinquante-six lignes identiques se ressemblent.
//   A  texte #71BED5 sur fond #8C00FF        B  texte #757DC2 sur fond presque noir
static const u16 kNumTexteA = rvb(14, 23, 26);
static const u16 kNumFondA  = rvb(17,  0, 31);
static const u16 kNumTexteB = rvb(16, 17, 24);
static const u16 kNumFondB  = rvb( 0,  1,  1);
// La selection : un jaune franc, qui ne se confond ni avec le vert du texte ni
// avec le rouge du chevron de lecture.
static const u16 kSelection = rvb(31, 29, 10);
// Un chain qui ne contient rien (aucune note, aucune commande) : plus sombre,
// pour voir d'un coup d'oeil ou le morceau joue vraiment quelque chose.
static const u16 kVide = rvb(11, 10, 16);
static const u16 kVert = rvb(8, 28, 10);   // FM-1 : une piste ou agit un effet (CIRCUIT BENDING)
#ifdef FM1
// LE THEME AUBERGINE (les couleurs du boitier du FM-1) : les couleurs ci-dessus restent celles de l'image,
// l'afficheur les traduit (fm1_plateforme.c, theme). Celles-ci, propres au FM-1, sont deja les siennes.
// Une couleur par molette, la meme sur CIRCUIT BENDING, KNOB GENERAL et MASTER FX : K1 rose, K2 violet,
// K3 peche, K4 bleu, SEL jaune, PRE turquoise, ALG vert-jaune.
static const u16 kEffet[7] = { rvb(31, 13, 23), rvb(22, 17, 31), rvb(31, 20, 14), rvb(9, 19, 31),
                               rvb(31, 27, 13), rvb(13, 28, 28), rvb(23, 30, 13) };
#endif

// ── LES TROIS ROLES, et rien d'autre ───────────────────────────────────────
// Toute page passe par ces trois-la : un intitule, une valeur, le curseur.
// Ils existent pour qu'on ne puisse PAS habiller une page autrement qu'une
// autre — c'est arrive, et les pages PSG et PCM ne ressemblaient plus a la
// page FM. Changer l'apparence se fait ici, une fois, pour tout le tracker.
static const u16 kTitre = kEntete;    // les intitules fixes
static const u16 kData  = kAttenue;   // les valeurs qu'on modifie
// kAccent tient le troisieme role : ce que le curseur designe.

// L'ecran vise par les primitives. La police doublee ne laisse que 32
// colonnes : les canaux qui n'y tiennent pas vont sur l'ecran du bas.
static u16 *g_cible = nullptr;
// Decalage horizontal, en COLONNES. La grille du tracker s'en sert pour se
// centrer : les bandes noires se repartissent alors de part et d'autre au lieu
// de tout laisser a droite. En-tete, titre de page et fenetres le remettent a
// zero — eux se calent sur les bords.
static int g_colOrigine = 0;
// Decalage vertical, en POINTS. Il sert a poser les noms de colonnes entre
// deux lignes plutot que sur l'une d'elles : les espaces au-dessus et en
// dessous deviennent alors egaux, ce qu'un decalage d'une ligne entiere ne
// permet pas — il n'y a pas assez de lignes pour s'en offrir une de plus.
static int g_decalYpx = 0;
static inline void ecran(u16 *c) { g_cible = c; }

// La colonne des reperes de la ligne du haut (« PH 00 », « INS 01 ») : sur le
// FM-1, en haut a droite de l'en-tete ; le nom de la page s'ecrit a gauche.
#ifdef FM1
// Cale tout a droite de l'en-tete (34 cellules de 7 points), quelle que soit
// l'origine de la page ; kColRepere : la zone a effacer avant (8 cellules).
// (fm1_entete_cellules : 34, ou 24 en ZOOM, ou l'en-tete est plus gros)
extern "C" int fm1_entete_cellules(void);
#define kColRepere (fm1_entete_cellules() - 8 - g_colOrigine)
#define COL_REPERE(t) (fm1_entete_cellules() - (int)strlen(t) - g_colOrigine)
#else
static const int kColRepere = 14;
#define COL_REPERE(t) kColRepere
#endif

#ifdef FM1
// FM-1 : l'image est en 8 bits avec une palette (compat/fm1_plateforme.c) —
// la RAM ne tient pas 320 x 240 en 16 bits —, et seule une fenetre de 240
// points de large se voit : elle suit le curseur, repere a sa couleur.
extern "C" {
void fm1_px(int i, u16 c);
u16 fm1_px_lit(int i);
void fm1_vu_accent(int x);
void fm1_vu_page(void);
}
static bool fm1_dans_chevron = false;   // le repere de lecture ne fait pas defiler
extern "C" void fm1_titres_colonnes(int rangee);   // cette rangee : les titres de colonnes, en petit
extern "C" void fm1_titres_colonnes_gros(int rangee);   // ... un peu moins petits
extern "C" void fm1_rangee_espace(int rangee);     // une rangee vide gardee : un espace
extern "C" void fm1_rangee_moyenne(int rangee);    // une rangee en police un peu plus grande
extern "C" void fm1_rangee_vide(int rangee);       // une rangee vide gardee, de la hauteur d'une ligne
extern "C" void fm1_sans_entete(int oui);          // pas d'en-tete (page FX)
extern "C" int fm1_largeur_cellule(void);          // une cellule des valeurs a l'ecran, en points
extern "C" void fm1_espace_sous_entete(int points); // un espace sous l'en-tete (cette page)
extern "C" void fm1_mot_entete(int col_mot, int n_lettres, int col_valeurs, int n_cases);   // centre au point pres
extern "C" void fm1_decales_efface(void);          // plus aucune valeur decalee
extern "C" void fm1_decale_demi(int rangee, int col, int n);   // ces cellules : une demi-cellule a droite
extern "C" int fm1_largeur_cellule_entete(void);   // ... et une de l'en-tete
extern "C" void fm1_zoom(int niveau);              // ZOOM (menu PROJECT) : 0 OFF, 1 4 COL, 2 3 COL (le plus gros)
extern "C" void fm1_luminosite(int v);             // LIGHT (menu PROJECT) : 0 le plus sombre, 15 le plus lumineux
extern "C" void fm1_mots_entete_efface(void);      // oublie les titres places dans l'en-tete (fm1_mot_entete)
extern "C" void fm1_molettes_instr(int on);        // SONG, CHAIN, PHRASE : KNOB 1..4 reglent l'instrument (fm1_fx_pas)
extern "C" int fm1_page_perdue(void);              // un message du systeme a pris l'ecran : la page est a refaire
extern "C" int fm1_clavier(void);
extern "C" void fm1_traits(int n, const int16_t *g, const int16_t *d, const u16 *coul, int y0, int y1);   // les traits entre les colonnes
extern "C" void fm1_gif(int k);                    // un GIF de la page CIRCUIT BENDING (fm1_plateforme.c)
extern "C" int fm1_drum(void);                     // le mode DRUM MACHINE (SEQ, troisieme mode)
extern "C" void fm1_led_touches(const uint8_t *niv);   // ... la lumiere des 27 touches (0 : la normale)
extern "C" int fm1_sel_appuis(void);
extern "C" int fm1_save_appuis(void);              // SAVE appuye depuis la derniere fois (la fenetre de sauvegarde)
extern "C" void fm1_clavier_suspendu(int on);      // une fenetre de texte ouverte : le mode KEYS s'efface
extern "C" int fm1_fx_appuis(void);                // FX appuye depuis la derniere fois (la page CIRCUIT BENDING)
extern "C" int fm1_env_appuis(void);               // ENV : les pages d'aide (HELP), puis la suivante
extern "C" void fm1_rangee_normale(int rangee);    // une rangee redevient normale (son style d'avant oublie)
extern "C" void fm1_rangee_aeree(int rangee);      // une rangee et un filet de fond dessous (l'aide, sans ZOOM)
extern "C" void fm1_rangee_petite(int rangee);     // une rangee a la taille de l'en-tete, et un filet (l'aide du D-PAD)
extern "C" void fm1_aide_clavier(int on);          // le clavier du FM-1 en haut de l'ecran (l'aide du D-PAD)
extern "C" void fm1_vue_fixe(int on);              // la fenetre reste a gauche (une page faite pour l'ecran)
extern "C" void fm1_rangee_drum(int rangee);       // une rangee de la grille de la DRUM MACHINE (a sa mesure)
extern "C" void fm1_rangee_loop(int rangee);       // ... celle du LOOPER, avec de l'air dessous
extern "C" void fm1_bend(int bits);                // les sept effets du circuit bending (src/opl3.c)
extern "C" void fm1_bend_pistes(int effet, uint32_t pistes);   // ... chacun sur ses pistes (bit n : la piste n + 1)
extern "C" void fm1_page_bend(int on);             // sur la page CIRCUIT BENDING : SEL, PRESETS, ALGORITHM (mode KEYS)
static const char *kBendNom[7] = { "DETUNE", "GLITCH", "CROSS", "DRONE", "GLIDE", "NOISE", "CHAOS" };
static const char *kBendCourt[7] = { "DET", "GLT", "CRS", "DRN", "GLD", "NOI", "CHA" };   // (sur la page)
// GLITCH (FX, second appui : src/glitch.c) : 16 pas, un effet chacun, synchronises sur les lignes jouees.
extern "C" void fm1_glitch_pas(int k, int effet);
extern "C" int fm1_glitch_etape(void);
extern "C" void fm1_glitch_tempo(uint32_t echantillons_par_ligne);
extern "C" void fm1_glitch_tampon(void *p);      // le tampon du son recent (32 Ko), 0 : plus d'effet
extern "C" void fm1_glitch_ligne(int ligne);
// MASTER FX (FX, troisieme appui : src/masterfx.c) : sept effets sur tout le son.
extern "C" void fm1_mfx_dose(int k, int dose);    // la dose d'un effet, 0 (coupe) a 15
extern "C" void fm1_mfx_tampon(void *p);          // la ligne du DELAY (32 Ko), 0 : plus de DELAY
static const char *kMfxNom[7] = { "DISTORTION", "RING MOD", "PHASER", "FLANGER", "DELAY", "CRUSH", "FILTER" };
static const char *kMfxCourt[7] = { "DIST", "RING", "PHAS", "FLNG", "DLY", "CRSH", "FILT" };
static const char *kGlitchCourt[9] = { "---", "RTG", "STU", "REV", "TAP", "OCT", "GAT", "CRU", "FLT" };
static const char *kGlitchNom[9] = { "OFF", "RETRIGGER", "STUTTER", "REVERSE", "TAPE STOP", "OCTAVE UP", "GATE",
                                     "CRUSH", "FILTER" };
// Le motif de depart (la premiere fois qu'on ouvre la page, sur un morceau) :
// on entend tout de suite ce que fait le GLITCH.
static const int kGlitchDefaut[16] = { 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 6, 3, 0, 0, 5, 4 };
extern "C" int fm1_home_appuis(void);              // HOME appuye depuis la derniere fois : retour a SONG               // SEL appuye depuis la derniere fois (la page PRESET)
extern "C" void fm1_page_presets(int on);          // sur la page PRESET : PRESETS fait defiler les presets
extern "C" void fm1_select_preset(int on);         // sur la page PRESET : SELECT aussi (pas le tempo)
extern "C" int fm1_looper(void);                   // le mode LOOPER (SEQ, troisieme mode)
extern "C" void fm1_mode_tracker(void);            // SEQ en TRACKER (un morceau charge)
extern "C" void fm1_led_clignote(uint32_t touches); // des touches du clavier qui clignotent
extern "C" int fm1_drumkeys(void);                 // le mode DRUM KEYBOARD (GLO, troisieme mode)
extern "C" int fm1_rec_tenu(void);                 // REC tenu (LOOPER : il efface, note par note)
extern "C" void fm1_horloge(uint32_t q16);         // CIRCUIT BENDING 2 : le pitch de toutes les voix, Q16
extern "C" void fm1_horloge_vitesse(uint32_t q16); // ... CLOCK PITCH : l'horloge (le pitch ET la vitesse), Q16
extern "C" int fm1_rec_appuis(void);               // REC appuye depuis la derniere fois
extern "C" void fm1_led_niveau(int id, int niv);   // une LED : 0 eteinte, 1 faible (il sert), 2 forte (page ouverte, actif)
extern "C" void fm1_led_play_vert(int niv);        // la LED verte de PLAY (a part ; celle de la touche est rouge)
enum { LED_FX = 2, LED_SEL = 3, LED_GLO = 7, LED_SAVE = 9, LED_HOME = 8, LED_PLAY = 12, LED_REC = 13 };   // (fm1_touches.h FM1_B_*)                  // le mode KEYS (SEQ clignote) : les 27 touches sont un clavier
extern "C" void fm1_annonce2(const char *l1, const char *l2);   // une ou deux lignes en gros au milieu de l'ecran
extern "C" void fm1_annonce2_evite(const char *l1, const char *l2, int rangee);   // la meme, sans cacher cette rangee
extern "C" void fm1_annonce3(const char *l1, const char *l2, const char *petite);   // et une aide en petit dessous
extern "C" int fm1_edit_appuis(void);              // EDIT appuye depuis la derniere fois
extern "C" void fm1_edit_clignote(int on);         // EDIT clignote (des molettes ont change un son)
extern "C" void fm1_suivre_lecture(int on);        // la vue suit le repere de lecture
extern "C" void fm1_vu_chevron(int i);             // un point du repere de lecture
extern "C" void fm1_ecran_chargement(void);        // « LOADING... », envoye tout de suite a l'afficheur
extern "C" void fm1_ecran_attente(const char *mot); // ... un autre mot (« EXPORTING... »)
extern "C" bool fm1_exporte_a2m(const char *chemin, char *msg, int cap);   // EXPORT A2M (compat/fm1_a2m_export.c)
extern "C" bool fm1_morceau_tient_en_a2m(void);    // rien de ce qu'AT2 ne connait (SAVE en .A2M)
extern "C" const char *fm1_fs_erreur(void);        // pourquoi la derniere ecriture a echoue (FLASH FULL...)
extern "C" int fm1_fs_plein(void);                 // ... faute de place
extern "C" void fm1_fs_raz_erreur(void);
static char g_raisonSave[24];                      // FM-1 : la raison du dernier SAVE FAILED
extern "C" void fm1_sortie_mono(int on);           // OUT : MONO (haut-parleur) ou STEREO (casque)
extern "C" void fm1_debut_mot(int col, int lig);   // un texte commence ici (les titres en petit s'y calent)
extern "C" void fm1_nom_page(const char *nom);     // le nom de la page, en haut a gauche de l'ecran
extern "C" int fm1_octave_pas(void);               // OCT+ / OCT- appuyes depuis la derniere fois
extern "C" u32 fm1_piano_appuis(void);             // touches du piano (libres) appuyees depuis la derniere fois
extern "C" u32 fm1_piano_tenues(void);             // ... et tenues
extern "C" int fm1_fx_pas(int k);                  // les crans de KNOB k+1 depuis la derniere fois (fm1_molettes_instr)
// Les titres de colonnes se posent sur leur rangee, sans le decalage d'une
// demi-ligne : l'ecran les affiche dans une rangee a eux, plus basse.
#define FM1_DECAL_NOMS(x) 0
#else
#define FM1_DECAL_NOMS(x) (x)
#endif

static void pixel(int x, int y, u16 c) {
  if ((unsigned)x < (unsigned)kEcranL && (unsigned)y < (unsigned)kEcranH) {
#ifdef FM1
    fm1_px(y * kEcranL + x, c);
    if (c == kAccent) {
      if (!fm1_dans_chevron) fm1_vu_accent(y * kEcranL + x);
      else fm1_vu_chevron(y * kEcranL + x);   // (la vue le suit pendant la lecture)
    }
#else
    g_cible[y * kEcranL + x] = c;
#endif
  }
}

// Assombrit tout l'ecran, pour qu'une fenetre posee dessus se detache et que
// l'on comprenne que la page est en attente. On divise chaque composante par
// deux, une seule fois a l'ouverture — le refaire a chaque image finirait par
// tout noircir.
static void assombrit() {
  for (int i = 0; i < kEcranL * kEcranH; i++) {
#ifdef FM1
    const u16 c = fm1_px_lit(i);
#else
    const u16 c = g_cible[i];
#endif
    const unsigned r = (c & 31) >> 1;
    const unsigned v = ((c >> 5) & 31) >> 1;
    const unsigned b = ((c >> 10) & 31) >> 1;
#ifdef FM1
    fm1_px(i, (u16)(0x8000 | (b << 10) | (v << 5) | r));
#else
    g_cible[i] = (u16)(0x8000 | (b << 10) | (v << 5) | r);
#endif
  }
}

// Une FENETRE : un panneau plein, borde, pose par-dessus la page. Le contenu
// s'ecrit ensuite dedans. Sans le fond plein, le texte se melait a ce qui
// restait affiche derriere et rien n'etait lisible.
static void fenetre(int col, int lig, int larg, int haut) {
  const int x0 = col * MD_CELL_W - 2, y0 = lig * MD_CELL_H - 2;
  const int x1 = x0 + larg * MD_CELL_W + 4, y1 = y0 + haut * MD_CELL_H + 4;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) {
      const bool bord = (y == y0 || y == y1 - 1 || x == x0 || x == x1 - 1);
      pixel(x, y, bord ? kEntete : kFond);
    }
}

// Le repere de lecture. Ce n'etait qu'un caractere « > » de la police, donc
// aussi gros qu'un chiffre et trop voyant. On le dessine ici : un petit
// triangle de trois points de large, centre dans la hauteur de la cellule.
static void chevron(int col, int lig, u16 couleur) {
  col += g_colOrigine;
  static const uint8_t forme[5] = {0x4, 0x6, 0x7, 0x6, 0x4};  // 100 110 111 ...
  int x0 = col * MD_CELL_W + 1, y0 = lig * MD_CELL_H + 2;
#ifdef FM1
  fm1_dans_chevron = true;
#endif
  for (int r = 0; r < 5; r++)
    for (int k = 0; k < 3; k++)
      if (forme[r] & (1 << (2 - k))) pixel(x0 + k, y0 + r, couleur);
#ifdef FM1
  fm1_dans_chevron = false;
#endif
}

// Dessine un caractere a la position (colonne, ligne) en cellules.
static void car(int col, int lig, char c, u16 couleur, bool gros) {
  col += g_colOrigine;
  int code = (unsigned char)c;
  // Deux caracteres d'effet d'AT2 que la police n'a pas : ` et ~.
  static const uint8_t kAccentGrave[MD_FONT_HT] = {0x08, 0x04, 0x02, 0, 0, 0, 0};
  static const uint8_t kTilde[MD_FONT_HT]       = {0, 0, 0x08, 0x15, 0x02, 0, 0};
  const uint8_t *gl;
  if (code == '`') gl = kAccentGrave;
  else if (code == '~') gl = kTilde;
  else {
    if (code < 32 || code > 126) code = 32;   // (FM-1 : les minuscules aussi, md_font.h)
    gl = md_font[code - 32];
  }
  int x0 = col * MD_CELL_W, y0 = lig * MD_CELL_H + g_decalYpx;
  // DEUX tailles cohabitent, et c'est voulu.
  //
  // Les intitules fixes — titres, noms de colonnes, libelles de reglages — se
  // dessinent AGRANDIS : ils gagnent en presence et l'agrandissement, qui
  // double certains traits, ne gene pas sur un mot qu'on ne fait que lire.
  //
  // Tout ce qui porte une VALEUR — les cases, les numeros de ligne, les
  // chiffres qu'on modifie — se dessine a la taille d'origine, 5x7 exacts.
  // C'est la ou la nettete compte : un chiffre agrandi devient trapu et deux
  // valeurs voisines se confondent.
  const int lg = gros ? MD_GLYPHE_W : MD_FONT_W;
  const int ht = gros ? MD_GLYPHE_H : MD_FONT_HT;
  // Centre verticalement dans la cellule quand le glyphe est plus petit.
  const int decY = (MD_GLYPHE_H - ht) / 2;
  for (int r = 0; r < ht; r++) {
    const int sr = (r * MD_FONT_HT) / ht;
    for (int k = 0; k < lg; k++) {
      const int sk = (k * MD_FONT_W) / lg;
      if (gl[sr] & (1 << (MD_FONT_W - 1 - sk)))
        pixel(x0 + k, y0 + decY + r, couleur);
    }
  }
}

// Le texte des VALEURS : police d'origine, nette.
// Les noms de fichiers viennent du Mac en UTF-8 : un « E » accentue y tient
// sur deux octets (precompose), voire trois (E + accent combinant). La police
// n'a que l'ASCII : on ecrit la lettre de base et on saute les accents.
static const char *sansAccent(const char *s, char *c) {
  const unsigned char u = (unsigned char)s[0];
  if (u == 0xCC || u == 0xCD) { *c = 0; return s[1] ? s + 2 : s + 1; }  // accent combinant
  if (u == 0xC3 && s[1]) {
    static const char kLatin[65] =
        "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
    const unsigned char v = (unsigned char)s[1];
    *c = (v >= 0x80 && v <= 0xBF) ? kLatin[v - 0x80] : '?';
    if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
    return s + 2;
  }
  if (u >= 0x80) {                       // autre caractere UTF-8 : un « ? »
    int n = (u >= 0xF0) ? 4 : (u >= 0xE0) ? 3 : 2;
    while (n-- > 0 && *s) s++;
    *c = '?';
    return s;
  }
  *c = (char)u;
  return s + 1;
}

// Le texte des VALEURS : police d'origine, nette.
static void texte(int col, int lig, const char *s, u16 couleur) {
#ifdef FM1
  fm1_debut_mot(col + g_colOrigine, lig);
#endif
  for (int i = 0; *s; ) { char c; s = sansAccent(s, &c); if (c) car(col + i++, lig, c, couleur, false); }
}
// Le texte des INTITULES fixes : police agrandie.
static void titre(int col, int lig, const char *s, u16 couleur) {
#ifdef FM1
  fm1_debut_mot(col + g_colOrigine, lig);
#endif
  for (int i = 0; *s; ) { char c; s = sansAccent(s, &c); if (c) car(col + i++, lig, c, couleur, true); }
}

// Dessine un numero de ligne sur deux chiffres, avec le FOND de son groupe.
// LSDJ colore les numeros par groupes de quatre lignes, en alternant deux
// teintes : c'est ce qui donne le rythme a l'oeil sur deux cent cinquante-six
// lignes. Le fond se peint avant le texte, sur la hauteur du glyphe.
static void numeroLigne(int col, int lig, int idx, bool curseur) {
  col += g_colOrigine;
  const bool fort = ((idx / 4) % 2) == 0;
  const u16 fond  = fort ? kNumFondA  : kNumFondB;
  const u16 encre = curseur ? kTexte : (fort ? kNumTexteA : kNumTexteB);
  const int x0 = col * MD_CELL_W - 1, y0 = lig * MD_CELL_H;
  for (int y = y0; y < y0 + MD_GLYPHE_H + 1; y++)
    for (int x = x0; x < x0 + 2 * MD_CELL_W + 1; x++)
      pixel(x, y, fond);
  char d[3] = { kHex[(idx >> 4) & 15], kHex[idx & 15], 0 };
  const int sauve = g_colOrigine; g_colOrigine = 0;
  texte(col, lig, d, encre);
  g_colOrigine = sauve;
}

// Efface une zone de texte avant de la reecrire.
static void efface(int col, int lig, int n) {
  col += g_colOrigine;
  // On efface UNE COLONNE DE PIXELS DE PLUS a gauche : le pave du curseur
  // deborde d'un pixel de ce cote, et sans ca il laissait une trainee derriere
  // lui a chaque deplacement.
  int x0 = col * MD_CELL_W - 1, y0 = lig * MD_CELL_H;
  for (int y = y0; y < y0 + MD_CELL_H; y++)
    for (int x = x0; x < x0 + n * MD_CELL_W + 1; x++)
      pixel(x, y, (y & 1) ? rvb(0, 1, 1) : kFond);
}

// ── Trame du CRT ────────────────────────────────────────────────────────────
// Une ligne sur deux legerement assombrie : c'est ce qui donne l'oeil du tube.
// Elle est appliquee au fond avant d'ecrire le texte, pas apres, pour que les
// caracteres restent nets.
// ⚠️ ELLE PEINT LA CIBLE COURANTE, PAS g_fond. Elle ecrivait dans g_fond en
// dur : « ecran(bas); trame(); » repeignait donc l'ecran du HAUT, et celui du
// bas n'a jamais recu de fond de sa vie. Tout ce qu'on ecrivait dans sa moitie
// basse tombait sur de la memoire que personne n'avait preparee.
static void trame() {
#ifdef FM1
  fm1_vu_page();   // une page repeinte : la fenetre repart a gauche
#endif
  for (int y = 0; y < kEcranH; y++) {
    u16 c = (y & 1) ? rvb(0, 1, 1) : kFond;
#ifdef FM1
    for (int x = 0; x < kEcranL; x++) fm1_px(y * kEcranL + x, c);
#else
    for (int x = 0; x < kEcranL; x++) g_cible[y * kEcranL + x] = c;
#endif
  }
}

// ── Edition, calquee sur LSDJModel de l'iPad ────────────────────────────────
// A pose une valeur ; un DOUBLE appui cree un element neuf ; A + croix modifie,
// pas de 1 a gauche/droite, grand pas en haut/bas (16, ou 12 demi-tons sur une
// note). C'est la convention de LSDJ, et celle du tracker iPad.

// Premier chain / phrase / instrument non utilise, pour le double appui.
// ⚠️ VIDE **ET** NON REFERENCE D'ABORD. Ne regarder que les references
// rendait une chaine qui porte deja des phrases mais que le SONG ne designe
// pas — on croyait en creer une neuve et on retombait sur un brouillon.
// C'est la meme regle que la version du clonage profond, plus bas.
// ── CE QUE FAIT CHAQUE COMMANDE, EN TOUTES LETTRES ───────────────────────
// Une lettre seule ne se retient pas : « U » ne dit pas « fine tune ». Les
// libelles viennent de md_table_cmds (md_replayer.c), qui associe chaque
// lettre a son effet — ce n'est donc pas une interpretation. L'ORDRE EST
// CELUI DE CETTE TABLE, et il ne doit pas en diverger.
static const char *kNomCmd[] = {
  "TABLE", "ARPEGGIO", "NOTE DELAY", "HOP", "NOTE CUT", "TONE PORTAMENTO",
  "GLOBAL VOLUME", "PANNING", "PITCH BEND", "RETRIG NOTE", "TEMPO",
  "VIBRATO", "TREMOLO", "VOLUME SLIDE", "PORTA + VOL SLIDE",
  "VIBRATO + VOL SLIDE", "SPEED", "POSITION JUMP", "PATTERN BREAK",
  "VIBRATO DEPTH", "FINE TUNE"
};


// ── LES EFFETS D'ADLIB TRACKER 2 (colonnes FX1-FX4 de la phrase) ─────────
// Le caractere de chaque effet est celui qu'AT2 affiche (fx_digits), dans
// l'ordre de ses numeros, qui sont aussi ceux du moteur (MD_EFF_*).
static const char kFxAT2[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ&%!@=#$~^`><";
static const int kNbFxAT2 = 48;
static const char *kNomFxAT2[kNbFxAT2] = {
  "ARPEGGIO", "FREQ SLIDE UP", "FREQ SLIDE DOWN", "TONE PORTAMENTO",
  "VIBRATO", "TONE PORTA + VOL SLIDE", "VIBRATO + VOL SLIDE",
  "FINE FREQ SLIDE UP", "FINE FREQ SLIDE DOWN", "SET MODULATOR VOLUME",
  "VOLUME SLIDE", "POSITION JUMP", "SET INSTRUMENT VOLUME", "PATTERN BREAK",
  "SET TEMPO", "SET SPEED", "TONE PORTA + FINE VSLIDE",
  "VIBRATO + FINE VSLIDE", "SET CARRIER VOLUME", "SET WAVEFORM",
  "FINE VOLUME SLIDE", "RETRIG NOTE", "TREMOLO", "TREMOR",
  "ARPEGGIO + VOL SLIDE", "ARPEGGIO + FINE VSLIDE", "MULTI RETRIG NOTE",
  "FSLIDE UP + VSLIDE", "FSLIDE DOWN + VSLIDE", "FINE FSLIDE UP + VSLIDE",
  "FINE FSLIDE DN + VSLIDE", "FSLIDE UP + FINE VSLIDE",
  "FSLIDE DN + FINE VSLIDE", "FINE FSLIDE UP+FINE VSL",
  "FINE FSLIDE DN+FINE VSL", "EXTENDED COMMANDS", "EXTENDED COMMANDS 2",
  "SET GLOBAL VOLUME", "SWAP ARPEGGIO TABLE", "SWAP VIBRATO TABLE",
  "FORCE INSTRUMENT VOLUME", "EXTENDED COMMANDS 3", "EXTRA FINE ARPEGGIO",
  "EXTRA FINE VIBRATO", "EXTRA FINE TREMOLO", "SET CUSTOM SPEED TABLE",
  "GLOBAL FREQ SLIDE UP", "GLOBAL FREQ SLIDE DOWN" };

// Les pistes 01 a 18 sont les voies de l'OPL3 ; au-dela, des samples.
static const int OPL_NB_PISTES = 18;

static inline int borne(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// La SORTIE est une position stereo COMPLETE : 00 tout a gauche, 80 au centre,
// FF tout a droite, comme dans un module XM. Les trois positions L C R venaient
// du tracker Mega Drive et n'employaient qu'un tiers de ce que la DS sait faire.

#ifdef FM1
#include "fm1_presets.h"   // les presets (page PRESET) : Amber, puis AdLib Tracker II
#include "fm1_opaque.h"    // une valeur que le compilateur ne voit pas (l'emulateur en ligne)
// ── REC : ce qui tient le temps, dans l'interruption audio ─────────────────
// Le decompte, le depart du morceau et le metronome se comptent sur l'horloge
// du son (fm1_echantillons), celle-la meme qui fait avancer le morceau : le
// premier temps du morceau tombe pile un temps apres le « 4 ». (Comptes
// dans la boucle de l'ecran, ils ne s'accordaient pas : un trou apres le 4.)
extern "C" uint32_t fm1_echantillons(void);
extern "C" uint32_t fm1_echantillons_par_seconde(void);
extern "C" void fm1_rappel_audio(void (*f)(void));
static const int kRecInsMetro = MD_MAX_INSTRUMENTS;   // l'instrument du metronome : FF
static volatile int g_recEtat = 0;     // 0 rien, 1 le decompte, 2 l'enregistrement
static volatile int g_recTemps = -1;   // le temps du decompte joue (0 .. 3), 4 : c'est parti
static int g_recVoie = 0, g_recLigne = 0;
static int g_recMetro[3], g_recNbMetro = 0;   // les pistes libres ou sonne le metronome
static uint32_t g_recProchain = 0, g_recPeriode = 1, g_recAvance = 0;
// Le moteur montre la ligne qu'il jouera ENSUITE (play_phrase_row) : celle
// qui vient de jouer est celle qu'il montrait juste avant. g_recPtr* : ce
// qu'il montrait ; g_recJoue* : la ligne qui joue (la ou s'ecrit le clavier).
static int g_recPtrPh = -1, g_recPtrRow = -1;
static volatile int g_recJouePh = -1, g_recJoueRow = -1;
// Pour placer une frappe sur la ligne la PLUS PROCHE : la ligne jouee avant
// celle-ci, l'echantillon ou celle-ci a joue, et le compte des lignes jouees.
static volatile int g_recPrecPh = -1, g_recPrecRow = -1;
static volatile uint32_t g_recJoueEch = 0, g_recNbLignes = 0;
static volatile uint32_t g_recTic = 0;   // l'echantillon du dernier coup de metronome (GLO clignote avec)
// Le son du metronome : [0] les temps, un charleston ferme ; [1] le temps 1, plus long (presque ouvert) et plus
// aigu, qu'on ne le confonde pas avec les autres (recPrepareTemps les prepare, recMetro pose le bon).
static uint8_t g_recMetroSon[2][MD_OPL_OCTETS];
static void recMetro(bool fort) {
  g_recTic = fm1_echantillons();
  for (int o = 0; o < MD_OPL_OCTETS; o++) md_replayer_set_instr_opl(kRecInsMetro, o, g_recMetroSon[fort][o]);
  // Le charleston sur plusieurs pistes a la fois : il s'entend par-dessus le morceau.
  for (int k = 0; k < g_recNbMetro; k++)
    md_replayer_play_test_note(fort ? 84 : 73, kRecInsMetro, g_recMetro[k]);
}
// ── LA DRUM MACHINE (SEQ, troisieme mode) ───────────────────────────────────
// Onze sons OPL3 (les touches noires du clavier), seize pas (les blanches),
// un pas par ligne du tracker : calee sur la phrase qui joue, sinon au tempo.
// Les sons sont les instruments F0..FA du morceau (un preset au depart, que
// les molettes reglent), joues sur les six dernieres voies OPL3. Les pas sont
// dans le morceau (drum_pas), et donc dans le .OBT.
static const char *kDrumNom[11] = { "KICK", "SNARE", "CLAP", "HIHAT C", "HIHAT O", "TOM LO", "TOM HI",
                                    "RIM", "SHAKER", "TAMB", "CYMBAL" };
static const char *kDrumCourt[11] = { "BD", "SD", "CP", "CH", "OH", "LT", "HT", "RM", "SH", "TB", "CY" };
// Les sons, faits ici (pas des presets) : les onze octets OPL3 de chacun,
// dans l'ordre du moteur : 20 (AM VIB EGT KSR MULT) modulateur, porteuse ;
// 40 (KSL TL) ; 60 (ATTACK DECAY) ; 80 (SUSTAIN RELEASE) ; E0 (forme
// d'onde) ; C0 (FEEDBACK, connexion). EGT eteint : le son meurt seul. La
// porteuse a fond (TL 0) : ils doivent s'entendre ; les sons de bruit en
// connexion additive (le modulateur, qui fait le bruit, sort aussi : deux fois plus fort).
static const uint8_t kDrumFM[11][11] = {
  { 0x01, 0x00, 0x03, 0x00, 0xF7, 0xF5, 0xFF, 0xF5, 0x00, 0x00, 0x0C },   // KICK : ~130 Hz (plus bas, le petit haut-parleur ne le rend pas), modulateur fort et feedback 6 : il claque
  { 0x01, 0x02, 0x00, 0x00, 0xF8, 0xF7, 0xF8, 0xF7, 0x00, 0x00, 0x0F },   // SNARE : feedback 7 (le bruit), les deux sortent
  { 0x01, 0x03, 0x02, 0x00, 0xF7, 0xF6, 0xF7, 0xF6, 0x00, 0x00, 0x0F },   // CLAP
  { 0x0F, 0x0F, 0x00, 0x00, 0xF9, 0xF9, 0xF9, 0xF9, 0x00, 0x00, 0x0F },   // HIHAT FERME : court
  { 0x0F, 0x0F, 0x00, 0x00, 0xF7, 0xF5, 0xF7, 0xF5, 0x00, 0x00, 0x0F },   // HIHAT OUVERT
  { 0x00, 0x00, 0x18, 0x00, 0xF8, 0xF5, 0xFF, 0xF5, 0x00, 0x00, 0x04 },   // TOM BAS
  { 0x00, 0x00, 0x18, 0x00, 0xF8, 0xF5, 0xFF, 0xF5, 0x00, 0x00, 0x04 },   // TOM HAUT
  { 0x05, 0x02, 0x06, 0x00, 0xF9, 0xF8, 0xF9, 0xF8, 0x00, 0x04, 0x07 },   // RIM : un clic boise, les deux sortent
  { 0x0F, 0x0F, 0x00, 0x00, 0xC7, 0xC7, 0xF8, 0xF8, 0x00, 0x00, 0x0F },   // SHAKER : l'attaque adoucie
  { 0x0E, 0x0A, 0x00, 0x00, 0xF7, 0xF6, 0xF7, 0xF6, 0x00, 0x00, 0x0D },   // TAMB : bruit et metal
  { 0x0F, 0x0F, 0x00, 0x00, 0xF4, 0xF3, 0xF4, 0xF3, 0x00, 0x00, 0x0F },   // CYMBAL : longue
};
static const uint8_t kDrumNote[11] = { 37, 49, 61, 85, 85, 37, 49, 61, 85, 73, 85 };
static const uint8_t kDrumVoie[11] = { 17, 16, 16, 15, 15, 14, 14, 13, 13, 13, 12 };   // (les charlestons : la meme)
static const int kDrumIns0 = 0xF0;
static int drumNote(const md_module_t *m, int k) {   // sa note, montee ou descendue par OCT
  const int n = kDrumNote[k] + (m ? m->drum_trans[k] : 0);
  return n < 1 ? 1 : n > MD_MAX_NOTE ? MD_MAX_NOTE : n;
}
static const uint8_t kDrumNoire[11] = { 1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25 };       // les touches (0 : F-3)
static const uint8_t kDrumBlanche[16] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };
static volatile int g_drumJoue = 0, g_drumPas = 15;
static volatile uint32_t g_drumPeriode = 5294, g_drumProchain = 0;
static int g_drumVu = -1;
static uint32_t g_drumSonne = 0;                 // les voies qui sonnent (coupees au pas suivant)
// Pour REC (plus bas, dans la boucle) : chaque pas joue, et ses sons.
static volatile uint32_t g_drumCoups[16], g_drumCoupsPas[16];
static volatile int g_drumCoupsSr[16], g_drumCoupsCr[16], g_drumCoupsL[16], g_drumCoupsRef[16];   // (ou en etait le tracker)
static volatile uint32_t g_drumNbCoups = 0;
static void drumAudio(int l, int ref) {
  if (!g_drumJoue) { g_drumVu = -1; return; }
  int pas = -1;
  const uint32_t maintenant = fm1_echantillons();
  if (l >= 0) {                                    // le tracker joue : sur ses lignes
    if (l != g_drumVu) { pas = (l + 15) & 15; g_drumVu = l; g_drumProchain = maintenant + g_drumPeriode; }
  } else {
    g_drumVu = -1;
    if ((int32_t)(maintenant - g_drumProchain) >= 0) {
      pas = (g_drumPas + 1) & 15;
      g_drumProchain += g_drumPeriode;
      if ((int32_t)(maintenant - g_drumProchain) >= 0) g_drumProchain = maintenant + g_drumPeriode;
    }
  }
  if (pas < 0) return;
  md_module_t *m = md_replayer_get_module();
  if (!m) return;
  for (int v = 12; v < 18; v++)
    if ((g_drumSonne >> v) & 1u) md_replayer_stop_test_note(v);
  g_drumSonne = 0;
  uint32_t coups = 0;
  for (int k = 0; k < 11; k++)
    if ((m->drum_pas[k] >> pas) & 1u) {
      md_replayer_play_test_note(drumNote(m, k), kDrumIns0 + k, kDrumVoie[k]);
      g_drumSonne |= 1u << kDrumVoie[k];
      coups |= 1u << k;
    }
  g_drumPas = pas;
  const uint32_t n = g_drumNbCoups;
  g_drumCoups[n & 15u] = coups;
  g_drumCoupsPas[n & 15u] = (uint32_t)pas;
  g_drumCoupsSr[n & 15u] = l >= 0 ? md_replayer_play_song_row(ref) : -1;
  g_drumCoupsCr[n & 15u] = l >= 0 ? md_replayer_play_chain_row(ref) : -1;
  g_drumCoupsL[n & 15u] = l;
  g_drumCoupsRef[n & 15u] = ref;
  g_drumNbCoups = n + 1u;
}
// ── LE LOOPER (SEQ, troisieme mode) ─────────────────────────────────────────
// Comme une pedale de boucle, mais de notes : REC enregistre ce qu'on joue au clavier, REC a nouveau ferme la
// boucle (sa duree : le temps entre les deux, sans rapport avec le tempo du morceau) et elle tourne ; REC
// ensuite ajoute une couche par-dessus (OVERDUB), REC encore la ferme. PLAY / STOP l'arrete et la relance ;
// arretee, REC en recommence une neuve. Chaque couche est jouee au clavier, donc une note a la fois : elle a sa
// voie OPL3 (6 a 11 ; les six dernieres sont a la DRUM MACHINE). Les notes sont datees sur l'horloge du son et
// rejouees ici, dans l'interruption.
struct LoopNote { uint32_t t; uint8_t note, ins, on, couche; };   // (on : 1 la note, 0 sa fin ; 2 la note effacee,
                                                                   //  3 sa fin effacee : gardees, A les remet telles quelles)
static const int kLoopMax = 512, kLoopVoie0 = 6, kLoopVoies = 6;   // (512 : la memoire vive est comptee)
static LoopNote g_loop[kLoopMax];
static volatile int g_loopN = 0, g_loopEtat = 0;   // 0 vide, 1 premiere prise, 2 joue, 3 OVERDUB, 4 arretee,
                                                  // 5 le decompte (1, 2, 3, 4 a l'ecran, sans son)
static volatile uint32_t g_loopT0 = 0, g_loopLg = 1;
// STEP : les pas de la boucle (16 : toute la duree enregistree) ; STP/CUR : ceux de chaque rangee, qui boucle
// alors sur sa propre longueur (de la polymetrie) ; g_loopVuL : ou en est chaque rangee
// Le PAS de la boucle (g_loopUnite, en echantillons) : celui qu'on a joue, trouve a la fermeture (loopDetecte) ;
// la boucle en a g_loopPasG, chaque rangee g_loopPasL (64 au plus). La grille en montre 16 cases : plus de 16
// pas, une case en regroupe plusieurs (loopParCase).
static volatile uint8_t g_loopPasL[6] = { 16, 16, 16, 16, 16, 16 };
static int g_loopPasG = 16;
static volatile uint32_t g_loopUnite = 1;
static inline int loopParCase(void) {             // les pas d'une case de la grille
  int m = g_loopPasG;
  for (int r = 0; r < 6; r++) if (g_loopPasL[r] > m) m = g_loopPasL[r];
  return (m + 15) / 16;
}
// le pas d'une note (le plus proche de son moment : on joue un peu avant, un peu apres), dans sa rangee
static inline int loopPasDe(uint32_t t, int rangee) {
  const uint32_t u = g_loopUnite, n = g_loopPasL[rangee];
  const uint32_t j = (t + u / 2u) / u;
  return (int)(j >= n ? j - n : j);               // (juste avant la fin : le premier pas, la boucle repart)
}
static int g_loopBtnC = 0;                        // (l'ecran : le bouton de droite ou de gauche sous le curseur)
// les touches OP4, OP5, OP6, PIT, GLO, MONO (8, 10, 13, 15, 17, 20) : la rangee 1..6 (STP/CUR) ; -1 : aucune
static inline int kRangeeTouche(int k) {
  return k == 8 ? 0 : k == 10 ? 1 : k == 13 ? 2 : k == 15 ? 3 : k == 17 ? 4 : k == 20 ? 5 : -1;
}
static volatile uint32_t g_loopVuL[6];
static volatile uint8_t g_loopRecale = 0;        // STEP, STP/CUR vient de changer : chaque rangee repart d'ou elle est
static inline uint32_t loopLgLigne(int r) {       // la duree de la rangee r, en echantillons
  const uint32_t p = g_loopPasL[r];
  return g_loopUnite * (p ? p : 1u);
}
static void loopRepart(void) {                    // (chaque rangee : au debut)
  for (int r = 0; r < 6; r++) g_loopVuL[r] = loopLgLigne(r) - 1u;
}
static int g_loopCouche = 0;
static volatile uint8_t g_loopMuet = 0;
static int g_loopOct = 0;                         // OCT sur toute la boucle, depuis son enregistrement           // les couches enlevees (A+B), gardees : A les remet
static void loopSilence(void) {
  for (int v = 0; v < kLoopVoies; v++) md_replayer_stop_test_note(kLoopVoie0 + v);
}
extern "C" void fm1_voie_forte(int voie);         // la prochaine note de cette voie, x2 (une batterie de DRUM KEYBOARD)
static const int kDkIns0 = 0xD4;                  // DRUM KEYBOARD : ses 27 sons, les instruments D4..EE (plus bas)
static bool g_dkPret = false;                     // ... prepares (une fois : KNOB GENERAL les regle ensuite aussi)
// Un morceau charge remplace tous les instruments : la boucle du LOOPER garderait des numeros qui ne sont plus
// ses sons. Ses sons du clavier sont donc mis de cote au chargement (g_loopGarde) puis remis sur des instruments
// a elle (CE..D3), et ses notes renvoyees vers eux ; ceux de DRUM KEYBOARD sont refaits.
static const int kLoopIns0 = 0xCE;
static uint8_t g_loopGardeIns[6], g_loopGardeOpl[6][MD_OPL_OCTETS], g_loopNbGarde = 0;
static bool g_loopARemettre = false;
static void loopAudio(void) {
  if (g_loopEtat != 2 && g_loopEtat != 3) return;
  uint32_t pos[6], avant[6];
  const uint32_t d = fm1_echantillons() - g_loopT0;
  const bool recale = g_loopRecale;
  g_loopRecale = 0;
  for (int r = 0; r < 6; r++) {                   // chaque rangee, sur sa longueur
    const uint32_t lgR = loopLgLigne(r);
    pos[r] = d % lgR;
    avant[r] = g_loopVuL[r];
    // (sa longueur vient de changer, ou sa derniere position n'y tient plus : elle repart de la ou elle est,
    // sans rejouer d'un coup toutes ses notes, ce qui faisait un son tres fort)
    if (recale || avant[r] >= lgR) avant[r] = pos[r];
    if (pos[r] < avant[r] && (int)g_loopPasL[r] < g_loopPasG)   // (une rangee raccourcie repart : sa note se tait)
      md_replayer_stop_test_note(kLoopVoie0 + r);
  }
  const int n = g_loopN;
  for (int i = 0; i < n; i++) {
    const uint32_t t = g_loop[i].t;
    const int rg = g_loop[i].couche % kLoopVoies;
    const uint32_t p = pos[rg], a = avant[rg];
    if (p == a) continue;
    const bool dedans = a < p ? (t > a && t <= p) : (t > a || t <= p);   // (la boucle repart)
    if (!dedans || g_loop[i].on >= 2 || ((g_loopMuet >> rg) & 1u)) continue;
    if (g_loop[i].ins >= kDkIns0 && g_loop[i].ins < kDkIns0 + 27) {
      // une batterie de DRUM KEYBOARD : un coup, sur les quatre voies de DRUM KEYBOARD tour a tour (plusieurs
      // batteries d'une meme couche sonnent ensemble, aucune ne coupe l'autre ; sa fin ne sert pas)
      static int dk = 0;
      if (g_loop[i].on) { fm1_voie_forte(12 + dk); md_replayer_play_test_note(g_loop[i].note, g_loop[i].ins, 12 + dk); dk = (dk + 1) & 3; }
      continue;
    }
    const int voie = kLoopVoie0 + rg;
    if (g_loop[i].on) md_replayer_play_test_note(g_loop[i].note, g_loop[i].ins, voie);
    else md_replayer_stop_test_note(voie);
  }
  for (int r = 0; r < 6; r++) g_loopVuL[r] = pos[r];
}
// LA BOUCLE FERMEE (REC) : on la cale sur ce qu'on a joue. Le pas : la duree sur laquelle les notes de la
// premiere prise tombent le mieux (la plus longue qui leur va, a un dixieme de pas pres). La longueur : REC est
// souvent presse un peu tard ; si la boucle depasse de peu (un pas et demi au plus) un compte regulier (binaire,
// un multiple de 4 ; ternaire, de 3) sans note dans ce qui depasse, elle s'y arrete ; sinon on garde le compte
// joue (un rythme asymetrique). Trop peu de notes : seize pas sur la duree, comme avant.
static void loopDetecte(uint32_t lg, uint32_t *unite, int *nb, uint32_t *decale) {
  static uint32_t on[kLoopMax];
  const uint32_t fs = fm1_echantillons_par_seconde(), mini = fs * 7u / 100u, maxi = fs * 3u / 2u;
  int n = 0;
  for (int i = 0; i < g_loopN; i++)
    if (g_loop[i].on == 1 && g_loop[i].t < lg) on[n++] = g_loop[i].t;
  for (int a = 1; a < n; a++)                    // (dans l'ordre)
    for (int b = a; b > 0 && on[b - 1] > on[b]; b--) { const uint32_t x = on[b]; on[b] = on[b - 1]; on[b - 1] = x; }
  *unite = lg / 16u; *nb = 16; *decale = 0;
  if (n < 3) return;
  double meilleur = 0.0, meilleurErr = 1e9, plusPetitErr = 1e9, plusPetit = 0.0;
  for (int a = 0; a < n; a++) {
    const double ioi = (double)on[a] - (a ? (double)on[a - 1] : 0.0);
    for (int m = 1; m <= 4; m++) {
      const double c = ioi / m;
      if (c < mini || c > maxi) continue;
      double err = 0.0;                           // (la grille calee sur la premiere note)
      for (int b = 0; b < n; b++) {
        const double q = ((double)on[b] - on[0]) / c, f = q - (double)(int)(q + 0.5);
        err += f < 0 ? -f : f;
      }
      err /= n;
      if (err < 0.1 && c > meilleur) { meilleur = c; meilleurErr = err; }
      if (err < plusPetitErr) { plusPetitErr = err; plusPetit = c; }
    }
  }
  double c = meilleur > 0.0 ? meilleur : plusPetit;
  (void)meilleurErr;
  if (c <= 0.0) return;
  {                                               // (affine : les moindres carres sur les pas trouves)
    double sk = 0.0, skt = 0.0;
    for (int b = 0; b < n; b++) {
      const int k = (int)(((double)on[b] - on[0]) / c + 0.5);
      if (k > 0) { sk += (double)k * k; skt += (double)k * ((double)on[b] - on[0]); }
    }
    if (sk > 0.0) c = skt / sk;
  }
  // le decalage : la premiere note, pile sur un pas (tout recule de moins d'un pas ; ca ne s'entend pas)
  const double dec = (double)on[0] - c * (double)(int)((double)on[0] / c);
  *decale = (uint32_t)(dec + 0.5);
  const double nf = ((double)lg - dec) / c;
  const int nMin = (int)(((double)on[n - 1] - dec) / c + 0.5) + 1;   // (la derniere note y tient)
  // (parmi les comptes reguliers entre la derniere note et la fin, le plus regulier : un multiple de 8, puis de
  // 4, de 6, de 3 ; a egalite, le plus long)
  int nReg = -1, scoreReg = 0;
  for (int k = (int)(nf + 0.5); k >= nMin && k >= 2 && nf - k <= 1.6; k--) {
    const int sc = k % 8 == 0 ? 4 : k % 4 == 0 ? 3 : k % 6 == 0 ? 2 : k % 3 == 0 ? 1 : 0;
    if (sc > scoreReg) { scoreReg = sc; nReg = k; }
  }
  int nbPas = nReg > 0 && nf - nReg <= 1.6 ? nReg : ((int)(nf + 0.5) > nMin ? (int)(nf + 0.5) : nMin);
  while (nbPas > 64) { c *= 2.0; nbPas = (nbPas + 1) / 2; }
  if (nbPas < 1) nbPas = 1;
  *unite = (uint32_t)(c + 0.5);
  *nb = nbPas;
}
// UNE LEGERE QUANTIFICATION : les notes de la prise (a partir de l'evenement de), chacune la moitie du chemin
// vers son pas ; sa fin bouge d'autant (sa duree reste). RETIREE pour l'instant (elle deformait les rythmes
// joues, quand le pas trouve n'etait pas le leur) : a affiner, elle n'est plus appelee.
static int g_loopDebutPrise = 0;                  // (le premier evenement de la prise en cours)
static void __attribute__((unused)) loopQuantifie(int de) {
  const int32_t u = (int32_t)g_loopUnite;
  if (u <= 1) return;
  for (int i = de; i < g_loopN; i++) {
    LoopNote &e = g_loop[(int)fm1_opaque((uint32_t)i)];
    if (fm1_opaque(e.on) != 1u) continue;
    const int rg = e.couche % kLoopVoies;
    const int32_t lgR = (int32_t)loopLgLigne(rg), t = (int32_t)e.t;
    const int32_t pas = ((t + u / 2) / u) * u, d = (pas - t) / 2;   // (la moitie du chemin)
    int32_t t2 = t + d;
    if (t2 < 0) t2 = 0;
    if (t2 >= lgR) t2 = lgR - 1;
    e.t = (uint32_t)t2;
    for (int j = i + 1; j < g_loopN; j++) {      // sa fin : le prochain evenement de sa couche, s'il en est la fin
      LoopNote &f = g_loop[(int)fm1_opaque((uint32_t)j)];
      if (f.couche != e.couche) continue;
      if (fm1_opaque(f.on) == 0u) {
        int32_t tf = (int32_t)f.t + d;
        if (tf <= t2) tf = t2 + 1;
        if (tf >= lgR) tf = lgR - 1;
        f.t = (uint32_t)tf;
      }
      break;
    }
  }
}
// La fin de la note i (le prochain evenement de sa couche) : effacee avec elle (efface), ou remise avec elle.
// (fm1_opaque : sans lui, des lectures et ecritures « b[++r=-1] », « b[r+-1] » que l'emulateur en ligne ne
// connait pas)
static void loopFin(int i, bool efface) {
  const uint32_t c = g_loop[i].couche;
  for (int j = i + 1; j < g_loopN; j++) {
    LoopNote &e = g_loop[(int)fm1_opaque((uint32_t)j)];
    const uint32_t o = fm1_opaque(e.on);
    if (e.couche != c) continue;
    if (efface) { if (o < 2u) { if (o == 0u) e.on = (uint8_t)fm1_opaque(3u); break; } }
    else if (o == 3u || o == 0u) { if (o == 3u) e.on = (uint8_t)fm1_opaque(0u); break; }
  }
}
// REMOVE LAST : la derniere note enregistree (et sa fin) s'en va ; ERASE ALL : toute la boucle.
static bool loopEnleveDerniere(void) {
  for (int i = g_loopN - 1; i >= 0; i--)
    if (g_loop[i].on == 1) {
      g_loop[i].on = 2;
      loopFin(i, true);
      md_replayer_stop_test_note(kLoopVoie0 + g_loop[i].couche % kLoopVoies);
      return true;
    }
  return false;
}
static void loopEfface(void) {
  g_loopEtat = 0;
  loopSilence();
  g_loopN = 0; g_loopCouche = 0; g_loopMuet = 0; g_loopOct = 0;
}
// (le clavier : une note jouee, ou lachee, entre dans la boucle si elle s'enregistre)
static void loopNote(int note, int ins, bool on) {
  if ((g_loopEtat != 1 && g_loopEtat != 3) || g_loopN >= kLoopMax) return;
  const uint32_t d = fm1_echantillons() - g_loopT0;
  LoopNote &e = g_loop[g_loopN];
  e.t = g_loopEtat == 1 ? d : d % loopLgLigne(g_loopCouche % kLoopVoies);   // (la longueur de sa rangee)
  e.note = (uint8_t)note; e.ins = (uint8_t)ins; e.on = on; e.couche = (uint8_t)g_loopCouche;
  g_loopN = g_loopN + 1;
}
// 2 a la puissance x (CIRCUIT BENDING 2 : les centiemes en rapport de frequence), sans la libm : e^(x ln 2)
static double deuxPuiss(double x) {
  double t = x * 0.69314718055994531, terme = 1.0, somme = 1.0;
  for (int k = 1; k < 18; k++) { terme *= t / k; somme += terme; }
  return somme;
}
static void recAudio(void) {
  { // GLITCH : la ligne qui joue (la premiere piste qui joue), -1 a l'arret
    static int v = 0;
    int l = -1;
    if (md_replayer_is_playing()) {
      l = md_replayer_play_phrase_row(v);
      for (int t = 0; l < 0 && t < MD_PISTES_OPL; t++)
        if ((l = md_replayer_play_phrase_row(t)) >= 0) v = t;
    }
    drumAudio(l, v);
    loopAudio();
    // (le GLITCH : sur les lignes du tracker ; sinon sur les pas de la DRUM
    // MACHINE, si elle joue ; sinon, a l'arret)
    fm1_glitch_ligne(l >= 0 ? l : g_drumJoue ? ((g_drumPas + 1) & 15) : -1);
  }
  if (g_recEtat == 1) {
    const int t = g_recTemps + 1;
    // (le morceau part un tick du moteur avant le temps : sa premiere ligne
    // joue au tick suivant, pile sur le temps)
    if ((int32_t)(fm1_echantillons() - (g_recProchain - (t == 4 ? g_recAvance : 0))) < 0) return;
    if (t < 4) { recMetro(t == 0); g_recTemps = t; g_recProchain += g_recPeriode; return; }
    // Un temps apres le 4 : le morceau, depuis la ligne de SONG du data.
    md_replayer_set_play_scope(MD_SCOPE_SONG, 0, 0);
    md_replayer_play_from(g_recLigne);
    md_replayer_play();
    g_recPtrPh = md_replayer_play_phrase(g_recVoie);
    g_recPtrRow = md_replayer_play_phrase_row(g_recVoie);
    g_recJouePh = -1; g_recJoueRow = -1; g_recPrecPh = -1; g_recPrecRow = -1; g_recNbLignes = 0;
    g_recTemps = 4;
    g_recEtat = 2;
    return;
  }
  if (g_recEtat == 2) {
    const int ph = md_replayer_play_phrase(g_recVoie), pr = md_replayer_play_phrase_row(g_recVoie);
    if (ph < 0 || pr < 0 || (ph == g_recPtrPh && pr == g_recPtrRow)) return;
    // Une ligne vient de jouer : celle que le moteur montrait.
    if (g_recPtrPh >= 0 && g_recPtrRow >= 0) {
      g_recPrecPh = g_recJouePh; g_recPrecRow = g_recJoueRow;
      g_recJouePh = g_recPtrPh; g_recJoueRow = g_recPtrRow;
      g_recJoueEch = fm1_echantillons();
      g_recNbLignes++;
      const int rpb = md_replayer_get_rows_per_beat() > 0 ? md_replayer_get_rows_per_beat() : 4;
      if (g_recJoueRow % rpb == 0) recMetro(g_recJoueRow == 0);   // sur les temps
    }
    g_recPtrPh = ph; g_recPtrRow = pr;
  }
}

// FM-1 : le projet de depart (a l'allumage et NEW) n'est pas vide : piste 01,
// ligne 00, le chain 00 ; sa ligne 0, la phrase 00 ; et dans celle-ci,
// l'instrument 01 sans note. Le morceau reste muet, mais le clavier (mode
// KEYS, le curseur sur cette case) joue tout de suite un son.
static void fm1ProjetDepart() {
  md_replayer_set_song(0, 0, 0);
  md_replayer_set_chain(0, 0, 0, 0);
  md_replayer_set_phrase(0, 0, 0, 1, MD_EMPTY, MD_EMPTY, 0, MD_EMPTY, 0);
}
#endif

int main(void) {
  // ── Horloge : passer la DSi a 134 MHz ─────────────────────────────────
  // La DSi peut faire tourner son ARM9 deux fois plus vite qu'une DS, mais
  // PERSONNE ne l'enclenche : ni calico, ni libnds. Le registre existe, il
  // faut l'ecrire soi-meme. Sans ca, une cartouche marquee DSi tourne quand
  // meme a 67 MHz — c'est a cette vitesse-la qu'ont ete faites toutes les
  // mesures precedentes, et c'est ce qui les rendait trop pessimistes.
  //
  // En mode DS pur le registre n'existe pas : on ne l'ecrit que si la console
  // est bien en mode TWL, sinon on planterait.
  const bool modeDSi = isDSiMode();
  if (modeDSi) setCpuClock(true);

  // ── Video ─────────────────────────────────────────────────────────────
  // Ecran du haut en bitmap 16 bits : on maitrise chaque pixel, ce qu'il faut
  // pour un rendu de tube. VRAM A lui suffit (256 x 192 x 2 = 96 Ko sur 128).
  powerOn(POWER_ALL_2D);
  videoSetMode(MODE_5_2D);
  vramSetBankA(VRAM_A_MAIN_BG);
  int bg = bgInit(2, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
  g_fond = bgGetGfxPtr(bg);
  ecran(g_fond);

  // Ecran du bas : vide, comme demande.
  videoSetModeSub(MODE_5_2D);
  vramSetBankC(VRAM_C_SUB_BG);
  int bgSub = bgInitSub(2, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
  u16 *bas = bgGetGfxPtr(bgSub);

  // La police doublee ne laisse que 32 colonnes. L'ecran du bas n'est plus un
  // fond noir : il porte les voies qui ne tiennent plus en haut, et la mesure.
  ecran(bas);  trame();
  ecran(g_fond); trame();

  // ── Le moteur ─────────────────────────────────────────────────────────
  // Il pilote directement les seize voies de la DS et l'USB : il n'y a plus
  // d'anneau a remplir. Son sequenceur tourne sous l'interruption du timer 0,
  // ce qui garde le MIDI a l'heure meme quand l'ecran prend du temps a se
  // redessiner.
  md_replayer_init(0);
  md_replayer_new_empty();
#ifdef FM1
  fm1ProjetDepart();
#endif
  // 125 BPM : sans cet appel, un projet neuf partait a 312 BPM (voir
  // GeneTrackerDS, md_replayer_new_empty pose une cadence AT2).
  md_replayer_set_bpm(125.0);
  md_replayer_ds_demarre();

  // ── La carte SD, puis l'USB MIDI ──────────────────────────────────────
  // La carte d'abord : sur DSi, le reveil de la DSpico y lit sa cle.
  const bool carteOK = fatInitDefault();
  // DS Linker Writer (Slot-2) sur DS Lite, DSpico (Slot-1) sur DSi. Sans
  // elles le tracker fonctionne quand meme : seuls les instruments MIDI
  // restent muets.
  const bool usbMidi = xt_midi_init();

  // Dix-huit pistes (les voies de l'OPL3), numerotees 01 a 18 comme dans AT2.
  static char nomsPistes[MD_MAX_CHANNELS][3];
  static const char *noms[MD_MAX_CHANNELS];
  for (int c = 0; c < MD_MAX_CHANNELS; c++) {
    nomsPistes[c][0] = (char)('0' + (c + 1) / 10);
    nomsPistes[c][1] = (char)('0' + (c + 1) % 10);
    nomsPistes[c][2] = 0;
    noms[c] = nomsPistes[c];
  }

  // ── Horloge de l'interface ────────────────────────────────────────────
  // Le timer 2, a 32,7 kHz : il compte les secondes du journal et sert a
  // detecter le double appui. Le timer 0 appartient au sequenceur.
  timerStart(2, ClockDivider_1024, 0, NULL);
  const unsigned kTicksParSeconde = 32727;   // 33,51 MHz / 1024
  unsigned tPrec = timerTick(2), cumul = 0;
  unsigned secondes = 0;   // horloge du journal, en secondes depuis l'allumage
  // Nombre de secondes pendant lesquelles on consigne l'etat de chaque voie
  // apres un depart : c'est la seule facon de voir si une colonne demarre.
  int journalVoies = 0;

  // Maintenir une direction doit faire defiler vite, comme sur LSDJ. Sans
  // cet appel, libnds garde sa cadence par defaut et le curseur avance au
  // compte-gouttes. Les deux valeurs comptent en appels a scanKeys, c'est-a-
  // dire en images : environ un tiers de seconde avant que ca parte, puis
  // trente pas par seconde.
  keysSetRepeat(20, 2);

  int curCanal = 0, curLigne = 0, haut = 0;
  // Carte des pages, comme sur l'iPad :
  //            PROJECT
  //   SONG   CHAIN   PHRASE   INSTR
  // SELECT + haut monte a PROJECT, SELECT + bas redescend.
  enum { PAGE_SONG = 0, PAGE_CHAIN = 1, PAGE_PHRASE = 2,
         PAGE_INSTR = 3, PAGE_PROJECT = 4, PAGE_TABLE = 5,
         PAGE_APROPOS = 6, PAGE_MIDICIBLES = 7, PAGE_PRESET = 8,   // (PRESET : FM-1, bouton SEL)
         PAGE_BEND = 9, PAGE_GLITCH = 10, PAGE_DRUM = 11, PAGE_MASTER = 12, PAGE_GLOBAL = 13,
         PAGE_AIDE = 14, PAGE_LOOPER = 15, PAGE_BEND2 = 16 };   // (HELP : FM-1, bouton ENV ; LOOPER : SEQ)                                           // (CIRCUIT BENDING : FM-1, bouton FX)
  // Curseur de la page TABLE : seize lignes, HUIT arrets.
  //   0 VOL   1 TSP
  //   2 lettre CMD 1   3 sa valeur
  //   4 lettre CMD 2   5 sa valeur
  //   6 code MD CMD    7 sa valeur
  // La lettre et sa valeur se reglent SEPAREMENT — c'est la meme regle que
  // dans une phrase : le curseur passe d'abord sur le premier caractere, puis
  // sur les deux suivants, et A + direction ne modifie que ce qui est sous lui.
  int tabLigne = 0, tabCol = 0;
  int repereTab[5] = { -2, -2, -2, -2, -2 };   // ou sont les chevrons
  int tableId = 0;                 // la table en cours d'edition
  int page = PAGE_SONG, pageVue = -1;
  // FM-1 : ZOOM (menu PROJECT), et pendant la lecture, la vue suit le repere
  // tant qu'on n'a touche ni aux fleches ni a A.
  int zoomNiv = 1;   // FM-1 : ZOOM, 0 OFF, 1 4 COL (par defaut : la premiere fois, sans reglage garde)
  int pianoOct = 0;  // FM-1 : le clavier, decale par OCT- / OCT+ (en demi-tons, d'octave en octave)
  uint32_t collageEch = 0;   // FM-1 : le dernier collage / clonage, sur l'horloge du son (0 : aucun)
  // FM-1 : la page PRESET (SEL) : d'ou l'on vient, l'instrument qu'elle regle,
  // sa voie (pour l'entendre au clavier), et le preset choisi.
  int presetAvant = PAGE_SONG, presetIns = 1, presetVoie = 0, presetSel = 0;
  // FM-1 : la page CIRCUIT BENDING (FX) : d'ou l'on vient, et les sept
  // switches (bits 0..6), qui restent la quand on la quitte. Chacun agit sur
  // ses pistes (bendPistes, bit n : la piste n + 1 ; au depart, toutes). La
  // page montre les pistes qui ont de la musique (bendPiste[]), relues en y
  // entrant ; bendCol : 0 la colonne 0 / 1, puis une piste, bendGauche la
  // premiere piste montree.
  int bendAvant = PAGE_SONG, bendBits = 0, bendSel = 0, bendCol = 0, bendGauche = 0;
  int bendPiste[18], bendNbPistes = 0, bendVoieClavier = 0; bool bendRelis = true;
  // FM-1 : la page GLITCH : l'effet de chaque pas, le pas du curseur, le
  // tampon du son recent (pris sur le tas seulement tant qu'un pas a un effet).
  int glitchPas[16] = { 0 }, glitchSel = 0; void *glitchTampon = 0; bool glitchVu = false, glitchOn = false;   // (eteint au depart : KNOB 1 l'allume)
  // FM-1 : la page MASTER FX : ses sept effets (bits), le curseur, la ligne
  // du DELAY (prise sur le tas seulement tant qu'il est mis).
  int mfxDose[7] = { 0 }, mfxSel = 0; void *mfxTampon = 0;
  // FM-1 : la page KNOB GENERAL : ce que chaque molette a deja pousse, en
  // tout (l'ecran le montre ; remis a zero au chargement d'un morceau).
  int globalPousse[6] = { 0 };
  // ... et SELECT : la moitie des sons en AM (un sur deux), ou leur connexion
  // d'avant (amAvant : la connexion + 1 ; 0 : pas touche).
  bool demiAM = false; static uint8_t amAvant[MD_MAX_INSTRUMENTS + 1];
  // SELECT a quatre positions : 0 FM (la connexion de chacun), 1 AM (la moitie des sons), 2 CURRENT (les sons
  // tels qu'on les a laisses), 3 DEFAULT (tels qu'avant le premier reglage de KNOB GENERAL : sonDefaut, pris a
  // ce moment-la). De DEFAULT, revenir sur CURRENT rend les sons d'avant (sonCourant). Une autre molette
  // apres DEFAULT : SELECT repart de FM, AM.
  int selPos = 0; bool defautPris = false;
  // EDIT : le son d'avant les molettes (SONG, CHAIN, PHRASE, INSTR, LOOPER ; KNOB GENERAL : sonDefaut, plus bas).
  // Chaque instrument est retenu juste avant que les molettes le touchent la premiere fois ; EDIT les remet tous
  // et clignote tant qu'il y a quelque chose a remettre. Huit au plus (la memoire du chargement des .A2M).
  static int editIns[8];
  static uint8_t editSon[8][MD_OPL_OCTETS];
  static int editN = 0;
  // SAVE : un morceau ouvert d'un .A2M s'y reenregistre tant qu'il tient tout entier dans un .A2M (decide a
  // l'ouverture de la fenetre du nom : saveEnA2m) ; sinon, et pour tout autre morceau, en .OBT.
  static bool ouvertA2m = false, saveEnA2m = false;
  // ... et le .A2M ouvert : efface quand le morceau passe en .OBT (on le retrouve par EXPORT A2M)
  static char origineA2m[128] = "";
  // (sur le tas, seulement quand on s'en sert, et rendus au chargement d'un morceau : l'import d'un gros .A2M
  // a besoin de toute la memoire vive)
  static uint8_t (*sonDefaut)[MD_OPL_OCTETS] = nullptr, (*sonCourant)[MD_OPL_OCTETS] = nullptr;
  auto sonsAlloues = [&]() {
    if (!sonDefaut) sonDefaut = (uint8_t (*)[MD_OPL_OCTETS])malloc((MD_MAX_INSTRUMENTS + 1) * MD_OPL_OCTETS);
    if (!sonCourant) sonCourant = (uint8_t (*)[MD_OPL_OCTETS])malloc((MD_MAX_INSTRUMENTS + 1) * MD_OPL_OCTETS);
    return sonDefaut && sonCourant;
  };
  static uint8_t amCourant[MD_MAX_INSTRUMENTS + 1];
  int pousseCourant[6] = { 0 }; bool demiCourant = false;
  int drumOctTout = 0;                             // FM-1 : OCT sur toute la DRUM MACHINE (pages FX), en octaves
  auto mfxMaj = [&]() {
    if (mfxDose[4] && !mfxTampon) {
      mfxTampon = malloc(16384 * 2);
      if (!mfxTampon) { mfxDose[4] = 0; fm1_annonce2("DELAY", "MEMORY FULL"); }
      else fm1_mfx_tampon(mfxTampon);
    } else if (!mfxDose[4] && mfxTampon) {
      fm1_mfx_dose(4, 0);
      fm1_mfx_tampon(0);                         // (l'interruption ne la lit plus : on peut la rendre)
      free(mfxTampon);
      mfxTampon = 0;
    }
    for (int k = 0; k < 7; k++) fm1_mfx_dose(k, mfxDose[k]);
  };
  // Les motifs d'avant (KNOB 1 en arriere y revient, en avant y retourne),
  // KNOB 2 en tire un au hasard.
  static uint8_t glitchHist[16][16]; int glitchHistN = 0, glitchHistPos = 0;
  uint32_t glitchHasard = 0x2545F491u;
  // FM-1 : la DRUM MACHINE : la page d'ou l'on vient, le son choisi.
  int aideAvant = PAGE_SONG, aidePage = 0; bool aideFin = false;
  // CIRCUIT BENDING 2 : le pitch (K1, par 20 centiemes de demi-ton) et l'accordage fin (K2, par centieme),
  // l'horloge de la puce comme un LTC1799 la ferait varier (fm1_horloge : elle glisse)
  // ... et CLOCK PITCH (K3, par 20 centiemes) : le pitch ET la vitesse, comme une cassette lue plus lentement
  int bendPitch = 0, bendFin = 0, bendHorloge = 0;
  auto horlogeMaj = [&]() {
    fm1_horloge((uint32_t)(65536.0 * deuxPuiss((bendPitch + bendFin) / 1200.0) + 0.5));
    fm1_horloge_vitesse((uint32_t)(65536.0 * deuxPuiss(bendHorloge / 1200.0) + 0.5));
  };
  int loopCurL = 0, loopCurC = 1, loopDerNote = 49;
  int loopRecAvant = 0;
  int loopLigneVue = 0;                               // LOOPER : la derniere rangee du curseur (STP/CUR la regle)
  int loopCibleL = 0;                                 // ... celle que les molettes reglent : la derniere enregistree,
                                                      //     ou celle ou l'on a mis le curseur depuis                               // LOOPER : l'etat d'avant le dernier appui de REC (tenu : il efface)   // LOOPER, en D-PAD : le curseur (couche, 0 = son numero,
                                                      // 1..16 = un pas), la derniere note posee   // HELP (ENV) : d'ou l'on vient, la page montree, la derniere ?
  int drumAvant = PAGE_SONG, drumSel = 0, drumCurPas = 0; bool drumMode = false;   // (drumCurPas : le pas du curseur, en D-PAD)
  auto drumArrete = [&]() {
    g_drumJoue = 0;
    for (int v = 12; v < 18; v++) md_replayer_stop_test_note(v);
  };
  auto glitchMaj = [&]() {
    bool un = false;
    // (eteint par KNOB 1 : le motif reste, mais n'agit plus)
    for (int k = 0; k < 16; k++) { fm1_glitch_pas(k, glitchOn ? glitchPas[k] : 0); if (glitchOn && glitchPas[k]) un = true; }
    if (un && !glitchTampon) {
      glitchTampon = malloc(16384 * 2);
      if (!glitchTampon) { fm1_annonce2("GLITCH", "MEMORY FULL"); return; }
      fm1_glitch_tampon(glitchTampon);
    } else if (!un && glitchTampon) {
      fm1_glitch_tampon(0);                     // (l'interruption ne le lit plus : on peut le rendre)
      free(glitchTampon);
      glitchTampon = 0;
    }
  };
  uint32_t bendPistes[7] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                             0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
  // FM-1 : le bouton SAVE ouvre la fenetre de sauvegarde de PROJECT ; on
  // revient ensuite d'ou l'on venait.
  int saveAvant = PAGE_SONG; bool saveRetour = false;
  // FM-1 : REC. 0 rien, 1 le decompte (1, 2, 3, 4), 2 l'enregistrement ; la
  // piste enregistree, l'instrument du clavier (le temps : g_rec*, plus haut).
  // 3 : arme, la lecture arretee (sur PHRASE : le clavier pose les notes une a
  // une ; PLAY lance l'enregistrement). recLibre : enregistrement sur la
  // boucle d'un chain ou d'une phrase (lecture lancee depuis CHAIN, PHRASE) :
  // rien n'est cree ni retire. lectureScope : d'ou PLAY a lance la lecture
  // (0 SONG, 1 CHAIN, 2 PHRASE). recPh0 : la premiere phrase du decompte.
  int recEtat = 0, recVoie = 0, recIns = 1, lectureScope = 0, recPh0 = -1;
  bool recLibre = false;
  int recDerPos = -1, recPrepare = -1;
  // Une frappe pour la ligne SUIVANTE, ecrite une fois cette ligne jouee.
  bool recAttente = false; int recAttPh = -1, recAttRow = 0, recAttNo = 0; uint32_t recAttN = 0;
  // Ce qui a ete prepare d'avance (la phrase, le chain suivants) : retire a
  // l'arret si la lecture n'y est pas arrivee.
  struct { bool la, chainNeuf, phraseNeuve; int sr, cr, ch, ph; } recAvance = { false, false, false, 0, 0, 0, 0 };
  // Les phrases jouees pendant REC, dans l'ordre : ou (SONG, chain), laquelle,
  // creees par REC ou non, et combien de notes elles ont recues. recDerU : la
  // derniere qui en a recu. recBoucle : 0 on avance ; 1 deux phrases vides de
  // suite, le moteur va repartir au debut ; 2 on boucle sur ce qui a ete joue.
  struct UniteRec { int sr, cr, ph, ch; bool phN, chN; int notes; };
  static UniteRec recU[256];
  int recNbU = 0, recDerU = -1, recBoucle = 0, recVuPh = -1;
  bool recDebutPhN = false, recDebutChN = false;
  auto recCompteNote = [&](int ph) {              // une note vient de s'ecrire dans la phrase ph
    if (recBoucle == 2) return;
    for (int k = recNbU - 1; k >= 0; k--)
      if (recU[k].ph == ph) { recU[k].notes++; if (k > recDerU) recDerU = k; return; }
  };
  // Retire ce que REC a cree a partir de la phrase de rang `de` (le silence
  // de la fin) : la phrase de sa ligne de chain, et le chain s'il est vide.
  auto recRetire = [&](int de) {
    if (de < 0) de = 0;
    for (int k = recNbU - 1; k >= de; k--) {
      const UniteRec &u = recU[k];
      if (u.phN) md_replayer_set_chain((uint8_t)u.ch, u.cr, MD_EMPTY, 0);
      if (u.chN) {
        bool vide = true;
        for (int r = 0; r < MD_ROWS_PER_CHAIN && vide; r++) {
          uint8_t ph; int8_t tsp;
          md_replayer_get_chain((uint8_t)u.ch, r, &ph, &tsp);
          if (ph != MD_EMPTY) vide = false;
        }
        if (vide) md_replayer_set_song(recVoie, u.sr, MD_EMPTY);
      }
    }
    if (recNbU > de) recNbU = de;
  };
  bool suiviLecture = false;
  bool sortieMono = false;  // FM-1 : OUT (menu PROJECT), STEREO par defaut, MONO pour le haut-parleur
  int lumiere = 15;         // FM-1 : LIGHT (menu PROJECT), 0 le plus sombre, F le plus lumineux (l'image, pas le retroeclairage)
  // PHRASE en ZOOM : comme les pistes de SONG, les groupes de colonnes (NOTE,
  // IN, CMD, FX1, FX2) apparaissent d'un coup quand le curseur passe le bord ;
  // les numeros de ligne restent a gauche. phGauche : le premier groupe montre.
  int phGauche = 0;
  static const int kPhGrpDeb[5] = { 3, 7, 10, 14, 18 }, kPhGrpLarg[5] = { 3, 2, 3, 3, 3 };
  static const int kPhGrpTitre[5] = { 2, 7, 10, 14, 18 };
  static const char *kPhGrpNom[5] = { "NOTE", "IN", "CMD", "FX1", "FX2" };
  auto phGroupe = [](int k) { static const int g[13] = { 0, 1, -1, 2, 2, 3, 3, 4, 4, -1, -1, -1, -1 };
                              return (k >= 0 && k < 13) ? g[k] : -1; };
  auto phDecal = [&](int gauche) { return gauche ? kPhGrpDeb[gauche] - kPhGrpDeb[0] : 0; };
  // Le groupe tient-il a l'ecran ? (en ZOOM, quinze colonnes, ou douze en 3 COL : la derniere est la 15, la 12)
  auto phDerniere = [&]() { return zoomNiv == 1 ? 15 : 12; };
  auto phVisible = [&](int g) { return !zoomNiv || (g >= phGauche &&
                                  kPhGrpDeb[g] - phDecal(phGauche) + kPhGrpLarg[g] - 1 <= phDerniere()); };
  // Chaque page a son propre curseur, comme sur l'iPad. Le chain montre est
  // celui pointe dans SONG ; la phrase montree est celle pointee dans CHAIN.
  int dernierChain = 0, dernierePhrase = 0;
  // La derniere note posee, RETENUE PAR CANAL. Elle etait unique pour tout le
  // morceau : en arrivant sur une nouvelle colonne, la premiere note heritait
  // de celle qu'on venait d'ecrire ailleurs — sur une basse apres un lead, on
  // se retrouvait trois octaves trop haut. Chaque voie part donc de C-4.
  //   la note 49 est le C de la quatrieme octave : (49-1) % 12 = 0, (49-1)/12 = 4
  int derniereNote[MD_MAX_CHANNELS];
  for (int i2 = 0; i2 < MD_MAX_CHANNELS; i2++) derniereNote[i2] = 49;   // C-4 partout
  // Vrai des qu'une note a ete posee ou touchee sur la voie. Avant ca — sur un
  // morceau qu'on vient d'ouvrir — la note de depart se reprend de la phrase
  // plutot que de tomber sur C-4.
  bool noteRetenue[MD_MAX_CHANNELS];
  for (int i2 = 0; i2 < MD_MAX_CHANNELS; i2++) noteRetenue[i2] = false;
  // La case ou le A en cours vient de poser une note sur une case VIDE. Un
  // accord A+B commence souvent par A : pour B, cette case etait vide.
  int notePoseeParA_ph = -1, notePoseeParA_l = -1;
  // Un KICK dont le coup reste a refabriquer : ses deux zones sonnaient encore
  // au moment du reglage. On redemande a chaque image, jusqu'a ce que ca passe.
  int kickARefaire = 0;
  // ── L'audition part a l'IMAGE SUIVANTE, et c'est voulu ──────────────────
  // En changeant une note avec A + croix, on relachait et on redeclenchait la
  // note dans le MEME instant : un synthe monophonique comme Model D y voit du
  // legato, ne redemarre pas son enveloppe, et on n'entend donc RIEN changer.
  // On relache donc tout de suite, et on redemarre a l'image suivante : seize
  // millisecondes d'ecart, invisibles a l'oeil, mais qui suffisent au synthe.
  int auditionNote = 0, auditionIns = 0, auditionVoie = -1;
  int auditionVel = 127;          // la force de la ligne, -1 = celle en place
  // L'instrument retenu pour CHAQUE voie. Sans lui, poser une note sur une
  // nouvelle colonne reprenait l'instrument 1, et deux voies se retrouvaient a
  // jouer exactement la meme chose — indiscernable d'une voie muette.
  int instrCanal[MD_MAX_CHANNELS];
  for (int i2 = 0; i2 < MD_MAX_CHANNELS; i2++) instrCanal[i2] = 0;
  int instrVoie = 0;    // voie d'ou l'on est descendu dans la page INSTR
  int voieCourante = 0; // voie d'ou l'on est descendu dans CHAIN puis PHRASE
  int voieAudition = -1; // voie sur laquelle sonne la note d'audition
  // Le sort du dernier chargement d'echantillon, affiche sur la page PCM : il
  // n'allait que dans le journal, invisible pendant qu'on travaille. Une
  // banque pleine passait donc pour un chargement qui « ne fait rien ».
  static char msgPCM[16] = "";
  // Un message passager en haut de l'ecran — confirmation d'enregistrement,
  // par exemple — avec la seconde a laquelle il doit disparaitre.
  static char msgProjet[40] = "";
  unsigned msgProjetJusqu = 0;

  // ── Le journal ──────────────────────────────────────────────────────────
  // Ecrit a cote du .nds, pour pouvoir etre releve apres coup au lieu de
  // guetter des chiffres a l'ecran. On accumule en memoire et on n'ecrit sur
  // la carte que par a-coups : ecrire pendant la lecture vide l'anneau audio,
  // ce qui creerait le defaut qu'on essaie de mesurer.
#ifdef FM1
  static char journalTampon[1024];   // (FM-1 : petit ; la memoire vive manquait a l'import des gros .A2M)
#else
  static char journalTampon[8192];
#endif
  int journalLong = 0;
  unsigned journalPerdus = 0;   // lignes jetees, tampon plein pendant la lecture
  bool journalSale = false;
  static char journalChemin[192];
  {
    strcpy(journalChemin, "/XMTRACKER.LOG");
    // nds-bootstrap transmet le chemin de la cartouche ; on ecrit a cote.
    if (__system_argv->argvMagic == ARGV_MAGIC &&
        __system_argv->argc > 0 && __system_argv->argv[0]) {
      char tmp[192];
      strncpy(tmp, __system_argv->argv[0], sizeof(tmp) - 20);
      tmp[sizeof(tmp) - 20] = 0;
      char *barre = strrchr(tmp, '/');
      if (barre) { barre[1] = 0; strcat(tmp, "XMTRACKER.LOG");
                   strcpy(journalChemin, tmp); }
    }
  }

  // Le journal part AUSSI sur la sortie de debogage no$gba, que melonDS
  // recopie sur sa sortie standard. Sans carte SD — le cas dans un emulateur —
  // c'est le seul moyen d'obtenir un texte qu'on puisse copier.
  auto journal = [&](const char *ligne) {
    debogLigne(ligne);
    const int n = (int)strlen(ligne);
    // Tampon plein : on jette, on compte, et on le dira au prochain vidage.
    // Bloquer ici pour ecrire sur la carte tuerait le son (voir plus bas).
    if (journalLong + n + 2 >= (int)sizeof(journalTampon)) {
      journalPerdus++; return;
    }
    memcpy(journalTampon + journalLong, ligne, n);
    journalLong += n;
    journalTampon[journalLong++] = '\n';
    journalTampon[journalLong] = 0;
    journalSale = true;
  };

  // Sur DSi, le lecteur par defaut n'est pas forcement celui qu'on croit :
  // libfat monte la carte sous « sd: » et la cartouche sous « fat: », et le
  // chemin nu peut ne designer ni l'un ni l'autre. On essaie donc plusieurs
  // candidats une seule fois, et on retient celui qui repond.
  bool journalChoisi = false;
  auto videJournal = [&]() {
    if (!journalSale || journalLong == 0) return;
    if (!journalChoisi) {
      char aCote[192] = "";
      if (__system_argv->argvMagic == ARGV_MAGIC &&
          __system_argv->argc > 0 && __system_argv->argv[0]) {
        strncpy(aCote, __system_argv->argv[0], sizeof(aCote) - 20);
        aCote[sizeof(aCote) - 20] = 0;
        char *barre = strrchr(aCote, '/');
        if (barre) { barre[1] = 0; strcat(aCote, "XMTRACKER.LOG"); }
        else aCote[0] = 0;
      }
      const char *candidats[4] = { aCote, "sd:/XMTRACKER.LOG",
                                   "fat:/XMTRACKER.LOG", "/XMTRACKER.LOG" };
      for (int i = 0; i < 4 && !journalChoisi; i++) {
        if (!candidats[i][0]) continue;
        FILE *t = fopen(candidats[i], "a");
        if (!t) continue;
        fclose(t);
        strcpy(journalChemin, candidats[i]);
        journalChoisi = true;
      }
      if (!journalChoisi) return;
    }
    FILE *f = fopen(journalChemin, "a");
    if (!f) return;
    fwrite(journalTampon, 1, (size_t)journalLong, f);
    if (journalPerdus) {
      char pj[80];
      siprintf(pj, "  (%u LIGNES PERDUES : TAMPON PLEIN PENDANT LA LECTURE)\n",
               journalPerdus);
      fwrite(pj, 1, strlen(pj), f);
      journalPerdus = 0;
    }
    fclose(f);
    journalLong = 0; journalTampon[0] = 0; journalSale = false;
  };
  bool navigateur = false;
  static char fichiers[80][40];
  static uint8_t typeFic[80];        // 0 autre, 1 ouvrable, 2 dossier
  int nbFichiers = 0, selFichier = 0, hautFichier = 0;
  // Les morceaux vivent dans « Songs », a cote de la cartouche. On cree le
  // dossier s'il manque : c'est la que l'enregistrement ecrit ET ou le
  // navigateur s'ouvre, pour que les deux ne se cherchent jamais.
  static char dossier[128] = "/";
  static char dossierSongs[128] = "/", dossierSamples[128] = "/";
  // Deux dossiers distincts, a cote de la cartouche : les morceaux et les
  // echantillons ne se cherchent pas au meme endroit, et le navigateur doit
  // s'ouvrir sur le bon selon ce qu'on est venu chercher.
  // Un dossier n'est retenu que s'il repond VRAIMENT. Le chemin transmis par
  // nds-bootstrap peut porter un prefixe de lecteur — « sd: », « fat: » — ou
  // n'en porter aucun, et tous ne designent pas forcement la meme chose. On
  // essaie donc les variantes et on garde celle qui s'ouvre, au lieu de
  // supposer : un dossier qui n'existe pas faisait retomber l'enregistrement
  // a la racine, sans rien dire.
  // Le chemin transmis par nds-bootstrap est utilise TEL QUEL. Le journal l'a
  // confirme : « sd:/roms/nds/UNDOSSIER/... » fonctionne. J'avais ajoute une
  // seconde variante, sans le prefixe de lecteur, en supposant qu'il pourrait
  // ne pas convenir — cette exploration a l'aveugle enchainait des opendir et
  // des mkdir sur un chemin qui ne designe rien, et bloquait le demarrage sur
  // la vraie console. On ne devine plus : une seule tentative, celle qui marche.
  if (__system_argv->argvMagic == ARGV_MAGIC &&
      __system_argv->argc > 0 && __system_argv->argv[0]) {
    char base[128];
    strncpy(base, __system_argv->argv[0], sizeof(base) - 12);
    base[sizeof(base) - 12] = 0;
    char *barre = strrchr(base, '/');
    if (barre) {
      barre[1] = 0;
      siprintf(dossierSongs,   "%sSongs", base);
      siprintf(dossierSamples, "%sSamples", base);
      // ── AUCUNE operation sur la carte pendant le demarrage ──────────────
      // Mesure sur la console : la cartouche se figeait ici, sept reperes
      // affiches sur dix. D'abord avec mkdir() sur un dossier deja present,
      // puis avec opendir() a sa place — donc ce n'est pas l'une ou l'autre
      // fonction, c'est le fait de toucher au systeme de fichiers a cet
      // instant du demarrage. L'emulateur, sans vraie carte, ne le montrait
      // jamais.
      //
      // On se contente donc de CALCULER les chemins. Les dossiers seront
      // verifies et crees au moment ou l'on s'en sert vraiment — a
      // l'enregistrement, ou a l'ouverture du navigateur — quand la carte a
      // eu tout le temps de se mettre en route.
      strcpy(dossier, dossierSongs);
    }
  } else {
    // Sans chemin de cartouche — c'est le cas dans un emulateur, qui lance la
    // ROM directement — les morceaux vont dans /Songs. Les echantillons, eux,
    // partent de la racine : leur dossier est celui que l'utilisateur choisit
    // en chargeant un premier son, retenu ensuite dans XMTRACKER.CFG.
    strcpy(dossierSongs, "/Songs");
    strcpy(dossierSamples, "/");
    strcpy(dossier, dossierSongs);
  }

  // Cree un dossier s'il manque. Appele au PREMIER USAGE, jamais au
  // demarrage : voir plus haut pourquoi.
  auto creeSiAbsent = [](const char *d) {
    DIR *x = opendir(d);
    if (x) { closedir(x); return true; }
    return mkdir(d, 0777) == 0;
  };

  // ── Parcourir le DOSSIER des echantillons a la croix ────────────────────
  // Le moteur ne joue que ce qui est dans la BANQUE du morceau — c'est ce qui
  // rend un .mdm autonome et lisible sur l'iPad. On concilie les deux : la
  // croix parcourt les fichiers du dossier, et celui qu'on pointe entre en
  // banque au passage. S'il y est deja, on s'y rebranche au lieu d'en ajouter
  // une copie, sinon un aller-retour dans la liste la remplirait.
  //
  // Le dossier n'est lu qu'a la PREMIERE utilisation, jamais au demarrage :
  // c'est un acces au systeme de fichiers pendant le demarrage qui figeait la
  // cartouche, ecran noir, et il a fallu quatre essais pour l'etablir.
  static char echFic[64][40];
  int echNb = 0, echSel = -1;
  bool echScanne = false;
  // Vrai pendant l'image ou le navigateur vient de charger un son : le MEME
  // appui sur A, une fois revenu sur la page instrument, tombait sur la ligne
  // LOAD SAMPLE et rouvrait aussitot le navigateur.
  bool navFerme = false;
  auto scanneEch = [&]() {
    echScanne = true;
    echNb = 0;
    DIR *d = opendir(dossierSamples);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && echNb < 64) {
      const char *n = e->d_name;
      if (n[0] == '.') continue;              // metadonnees du systeme
      const int L = (int)strlen(n);
      if (L < 5 || strcasecmp(n + L - 4, ".wav")) continue;
      strncpy(echFic[echNb], n, 39);
      echFic[echNb][39] = 0;
      echNb++;
    }
    closedir(d);
    // Par ordre alphabetique : c'est l'ordre ou l'on s'attend a les feuilleter,
    // alors que la carte les rend dans l'ordre ou ils ont ete copies.
    qsort(echFic, (size_t)echNb, sizeof(echFic[0]),
          [](const void *a, const void *b) {
            return strcasecmp((const char *)a, (const char *)b);
          });
  };
  // Le rang, dans le dossier, de l'echantillon d'un instrument ; -1 s'il n'y
  // est pas. La banque ne garde que 23 caracteres du nom.
  auto rangEch = [&](int ins) {
    const int ech = md_replayer_get_instr_sample(ins);
    if (ech < 0 || ech >= MD_MAX_SAMPLES) return -1;
    char nm[24]; uint32_t lg; int bcl, bn;
    if (!md_replayer_get_sample(ech, nm, 24, &lg, &bcl, &bn)) return -1;
    for (int i = 0; i < echNb; i++)
      if (!strncasecmp(echFic[i], nm, 23)) return i;
    return -1;
  };

  // ── Le dossier d'echantillons, CHOISI par l'utilisateur ─────────────────
  // Aucun dossier n'est impose : c'est celui d'ou l'on a charge le dernier
  // son. Il est note dans XMTRACKER.CFG, a cote du journal, pour rouvrir au
  // meme endroit au lancement suivant. Lu au premier usage seulement : toucher
  // a la carte pendant le demarrage figeait la cartouche.
  bool cfgLu = false;
  auto cheminCfg = [&](char *d) {
    strcpy(d, journalChemin);
    char *pt = strrchr(d, '.');
    if (pt) strcpy(pt, ".CFG"); else strcat(d, ".CFG");
  };
  auto lisCfg = [&]() {
    if (cfgLu) return;
    cfgLu = true;
    char c[200]; cheminCfg(c);
    FILE *f = fopen(c, "r");
    if (!f) return;
    char l[160];
    while (fgets(l, sizeof l, f)) {
      if (!strncmp(l, "SYNC=", 5)) {
        md_replayer_set_sync_ms(atoi(l + 5));
        continue;
      }
#ifdef FM1
      if (!strncmp(l, "ZOOM=", 5)) {           // FM-1 : le ZOOM reste tel qu'on l'a laisse
        zoomNiv = borne(atoi(l + 5), 0, 1);   // (un ancien 3 COL devient 4 COL)
        continue;
      }
      if (!strncmp(l, "MONO=", 5)) {           // ... et OUT aussi
        sortieMono = atoi(l + 5) != 0;
        fm1_sortie_mono(sortieMono);
        continue;
      }
      if (!strncmp(l, "LIGHT=", 6)) {          // ... et LIGHT
        lumiere = borne(atoi(l + 6), 0, 15);
        fm1_luminosite(lumiere);
        continue;
      }
#endif
      if (strncmp(l, "SAMPLES=", 8) != 0) continue;
      char *v = l + 8;
      v[strcspn(v, "\r\n")] = 0;
      DIR *x = opendir(v);
      if (x) { closedir(x);
               strncpy(dossierSamples, v, sizeof(dossierSamples) - 1);
               dossierSamples[sizeof(dossierSamples) - 1] = 0;
               echScanne = false; }
    }
    fclose(f);
  };
  auto ecrisCfg = [&]() {
    char c[200]; cheminCfg(c);
    FILE *f = fopen(c, "w");
    if (!f) return;
    fprintf(f, "SAMPLES=%s\n", dossierSamples);
    fprintf(f, "SYNC=%d\n", md_replayer_get_sync_ms());
#ifdef FM1
    fprintf(f, "ZOOM=%d\n", zoomNiv);
    fprintf(f, "MONO=%d\n", sortieMono ? 1 : 0);
    fprintf(f, "LIGHT=%d\n", lumiere);
#endif
    fclose(f);
  };
  // Le reglage SYNC (et le dossier des sons) se relisent des le demarrage :
  // la synchro OPL3 / samples vaut pour tous les morceaux.
  if (carteOK) lisCfg();

  // La page a laquelle revenir en quittant le navigateur : on repart d'ou l'on
  // venait, et non sur PROJECT — charger un echantillon depuis l'ecran
  // instrument y ramenait, ce qui n'a aucun sens.
  int pageRetour = PAGE_PROJECT;

  // ── Le parcours du dossier d'echantillons a ete RETIRE ─────────────────
  // Il faisait planter la cartouche AU DEMARRAGE sur la vraie console : le
  // journal ne portait plus aucune ligne des versions concernees, preuve que
  // le plantage precedait l'ecriture de son en-tete. Je n'ai pas trouve la
  // cause exacte ; l'emulateur, lui, demarrait normalement, faute de vraie
  // carte SD, et je n'avais donc aucun moyen de la reproduire ici.
  //
  // On revient au systeme qui fonctionnait : « LOAD SAMPLE » charge un fichier
  // dans la banque du morceau, et A + gauche/droite parcourt cette banque.
  // A reprendre plus tard, en verifiant a CHAQUE etape sur la console au lieu
  // d'ajouter plusieurs mecanismes d'un coup.

  // Les trois visages de la page instrument : un echantillon, du MIDI, ou le
  // KICK de synthese. L'ordre est celui que la croix parcourt sur la ligne TYPE.
  enum { GENRE_SAMPLE, GENRE_MIDI, GENRE_KICK, GENRE_OPL };

  // ── Page INSTR d'un instrument OPL ──────────────────────────────────────
  // Rangees 0-2 : reglages de la voie, deux par rangee ; rangees 3-14 : les
  // reglages des operateurs, colonne MOD (modulateur) et colonne CAR
  // (porteuse). Chaque reglage est un groupe de bits d'un des onze octets de
  // l'instrument, au format d'AdLib Tracker 2 (MD_OPL_*).
  // Les rangees de la page, dans l'ordre d'AdLib Tracker 2. Une rangee « par
  // operateur » a deux valeurs, MOD puis CAR (octet + 1 pour la porteuse).
  enum { AFF_HEX, AFF_SORTIE, AFF_KSL, AFF_ONOFF, AFF_ONDE, AFF_MULT,
         AFF_CONN, AFF_PAN, AFF_FTUNE, AFF_TABLE, AFF_TYPE, AFF_INV };
#ifdef FM1
  // FM-1 : F = le MAXIMUM. La puce compte a l'envers pour ces quatre-la (un
  // « rate » F est le plus rapide, donc la duree la plus courte ; un niveau de
  // sustain 0 est le plus fort) : on affiche et on regle masque - valeur, donc
  // F = attaque, decroissance, relache les plus longues, sustain le plus fort.
  #define AFF_ENV AFF_INV
#else
  #define AFF_ENV AFF_HEX
#endif
  struct ChampOpl { const char *nom; bool parOp; int octet; int decalage; int masque; int aff; };
  static const ChampOpl kChampsOp[] = {
    { "TYPE",            false, 0,              0, 0,  AFF_TYPE },
    { "ATTACK RATE",     true,  MD_OPL_AD_M,    4, 15, AFF_ENV },
    { "DECAY RATE",      true,  MD_OPL_AD_M,    0, 15, AFF_ENV },
    { "SUSTAIN LVL",     true,  MD_OPL_SR_M,    4, 15, AFF_ENV },
    { "RELEASE RATE",    true,  MD_OPL_SR_M,    0, 15, AFF_ENV },
    { "OUTPUT LVL",      true,  MD_OPL_VOL_M,   0, 63, AFF_SORTIE },
    { "KEY SCALING LVL", true,  MD_OPL_VOL_M,   6, 3,  AFF_KSL },
    { "CONNECTION",      false, MD_OPL_FB_CONN, 0, 1,  AFF_CONN },
    { "PANNING",         false, 0,              0, 0,  AFF_PAN },
    { "TREMOLO",         true,  MD_OPL_FLAGS_M, 7, 1,  AFF_ONOFF },
    { "VIBRATO",         true,  MD_OPL_FLAGS_M, 6, 1,  AFF_ONOFF },
    { "KEY SCL RATE",    true,  MD_OPL_FLAGS_M, 4, 1,  AFF_ONOFF },
    { "SUSTAIN",         true,  MD_OPL_FLAGS_M, 5, 1,  AFF_ONOFF },
    { "WAVEFORM",        true,  MD_OPL_WAVE_M,  0, 7,  AFF_ONDE },
    { "FEEDBACK",        false, MD_OPL_FB_CONN, 1, 7,  AFF_HEX },
    { "F-TUNE",          false, 0,              0, 0,  AFF_FTUNE },
    { "FREQ MULTIPLIER", true,  MD_OPL_FLAGS_M, 0, 15, AFF_MULT },
    { "TABLE",           false, 0,              0, 0,  AFF_TABLE },
  };
  static const int kOplRangees = (int)(sizeof(kChampsOp) / sizeof(kChampsOp[0]));
  static const int kOplRangeeTable = kOplRangees - 1;
  // KEY SCALING : AT2 les propose dans l'ordre Default, 1.5, 3.0, 6.0 dB ;
  // la puce les code 0, 2, 1, 3.
  static const int kKslOrdre[4] = { 0, 2, 1, 3 };
  static const char *kKslNom[4] = { "DEFAULT", "3.0DB/OCT", "1.5DB/OCT", "6.0DB/OCT" };
  static const char *kOndeNom[8] = { "SINE", "HALF-SINE", "ABS-SINE", "PULSE-SIN",
                                     "SINE EPO", "ABS EPO", "SQUARE", "DER.SQUAR" };
#ifdef FM1
  // FM-1 : des intitules de 4 lettres au plus et des valeurs de 3 caracteres
  // au plus, pour que MOD et CAR tiennent tous les deux dans l'ecran.
  static const char *kNomCourt[] = { "TYPE", "ATK", "DEC", "SUS", "REL", "OUT", "KSL",
                                     "CONN", "PAN", "TREM", "VIB", "KSR", "EGT", "WAVE",
                                     "FB", "FINE", "MULT", "TBL" };
  static_assert(sizeof(kNomCourt) / sizeof(kNomCourt[0]) == sizeof(kChampsOp) / sizeof(kChampsOp[0]),
                "un intitule court par rangee");
  static const char *kKslCourt[4]  = { "DEF", "3.0", "1.5", "6.0" };
  static const char *kOndeCourt[8] = { "SIN", "HLF", "ABS", "PLS", "EPO", "AEP", "SQR", "DSQ" };
#endif
  static const char *kMultNom[16] = { "X.5", "X1", "X2", "X3", "X4", "X5", "X6", "X7",
                                      "X8", "X9", "X10", "X10", "X12", "X12", "X15", "X15" };
  auto oplLit = [](int ins, int octet, int dec, int masque) {
    return (md_replayer_get_instr_opl(ins, octet) >> dec) & masque;
  };
  auto oplEcrit = [](int ins, int octet, int dec, int masque, int v) {
    const uint8_t o = md_replayer_get_instr_opl(ins, octet);
    md_replayer_set_instr_opl(ins, octet,
        (uint8_t)((o & ~(masque << dec)) | ((v & masque) << dec)));
  };
#ifdef FM1
  // ── INSTR OPL3 sur le FM-1 : les cinq pages ─────────────────────────────
  // Celles du tracker DOS, decoupees pour le petit ecran :
  //   1 ENV  l'enveloppe de l'operateur (barres et courbe ADSR)
  //   2 WAVE sa forme d'onde, et le feedback
  //   3 MULT son multiplicateur, et son KEY SCALING
  //   4 CONN la connexion, le panoramique, l'ENVELOPE TYPE, l'accord fin, la TABLE
  // (les presets : la page PRESET, bouton SEL)
  // Chaque page est une liste d'ELEMENTS : un reglage (sa valeur se change
  // avec A + gauche / droite, comme ailleurs), un choix d'une liste ou une
  // case a cocher (A le prend), la ligne OP. Un
  // element designe un champ de kChampsOp : l'edition reste celle d'avant.
  enum { EL_OP, EL_VAL, EL_OPT, EL_CHK };
  struct ElInstr { int genre, champ, opt, lig, col; };   // (des int : un octet signe lu ainsi, l'emulateur en ligne ne le connait pas)
  static const int kInPages = 4;
  static const char *kInPageNom[kInPages] = { "ENV", "WAVE", "MULT", "CONN" };
  auto elementsInstr = [&](int pg, ElInstr *e) {
    int n = 0;
    auto ajoute = [&](int genre, int champ, int opt, int lig, int col) {
      // (volatile : le compilateur ne regroupe pas deux champs en une ecriture
      // double a pre-increment, d[++rA=N], que l'emulateur en ligne ne connait pas)
      volatile ElInstr *v = &e[n];
      v->genre = genre; v->champ = champ; v->opt = opt;
      v->lig = lig; v->col = col; n++;
    };
    if (zoomNiv) {
      // En ZOOM, chaque page tient dans l'ecran, sans defiler (sinon la vue
      // sautait et la ligne OP partait en haut) : ni titre ni espace ; en 3 COL,
      // les listes deviennent une valeur (A + gauche / droite).
      const bool c4 = zoomNiv == 1;
      int lig = 1;
      ajoute(EL_OP, -1, 0, lig++, 0);
      switch (pg) {
        case 0: for (int r = 0; r < 5; r++) ajoute(EL_VAL, 1 + r, 0, lig++, 0); break;   // ATK .. OUT
        case 1:
          if (c4) for (int k = 0; k < 8; k++) ajoute(EL_OPT, 13, k, lig++, 0);
          else ajoute(EL_VAL, 13, 0, lig++, 0);                                         // WAVE
          ajoute(EL_VAL, 14, 0, lig++, 0);                                              // FB
          if (!c4) ajoute(EL_VAL, 15, 0, lig++, 0);                                     // FINE (3 COL : ici)
          break;
        case 2:
          if (c4) for (int k = 0; k < 8; k++) {
            ajoute(EL_OPT, 16, k, lig, 0);
            ajoute(EL_OPT, 16, k + 8, lig++, 8);
          } else ajoute(EL_VAL, 16, 0, lig++, 0);                                       // MULT
          ajoute(EL_VAL, 6, 0, lig++, 0);                                               // KSL
          break;
        default:
          ajoute(EL_VAL, 7, 0, lig++, 0);                                               // CONN
          ajoute(EL_VAL, 8, 0, lig++, 0);                                               // PAN
          for (int k = 0; k < 4; k++) ajoute(EL_CHK, 9 + k, 0, lig++, 0);               // TREM VIB KSR SUST
          if (c4) ajoute(EL_VAL, 15, 0, lig++, 0);                                      // FINE
          ajoute(EL_VAL, kOplRangeeTable, 0, lig++, 0);                                 // TBL
      }
      return n;
    }
    ajoute(EL_OP, -1, 0, 1, 0);
    switch (pg) {
      case 0: for (int r = 0; r < 5; r++) ajoute(EL_VAL, 1 + r, 0, 3 + r, 0); break;   // ATK .. OUT
      case 1:
        for (int k = 0; k < 8; k++) ajoute(EL_OPT, 13, k, 4 + k, 0);                  // WAVEFORM
        ajoute(EL_VAL, 14, 0, 13, 0);                                                  // FEEDBACK
        break;
      case 2:
        if (zoomNiv == 2) for (int k = 0; k < 16; k++) ajoute(EL_OPT, 16, k, 4 + k, 0);   // (3 COL : une colonne)
        else for (int k = 0; k < 8; k++) {
          ajoute(EL_OPT, 16, k, 4 + k, 0);
          ajoute(EL_OPT, 16, k + 8, 4 + k, zoomNiv ? 8 : 10);
        }
        { const int l0 = zoomNiv == 2 ? 21 : 13;
          for (int k = 0; k < 4; k++) ajoute(EL_OPT, 6, k, l0 + 1 + k, 0); }            // KEY SCALING
        break;
      case 3:
        ajoute(EL_VAL, 7, 0, 3, 0);                                                    // CONNECTION
        ajoute(EL_VAL, 8, 0, 4, 0);                                                    // PANNING
        for (int k = 0; k < 4; k++) ajoute(EL_CHK, 9 + k, 0, 7 + k, 0);                // TREM VIB KSR SUSTAIN
        ajoute(EL_VAL, 15, 0, 12, 0);                                                  // F-TUNE
        ajoute(EL_VAL, kOplRangeeTable, 0, 14, 0);                                     // TABLE
        break;
    }
    return n;
  };
  // Les PRESETS (page PRESET, bouton SEL) : ceux d'Amber OPL3 Tracker d'abord
  // (KICK SNARE BASS HH-C HH-O), puis tous les instruments d'AdLib Tracker II
  // (build/gen/fm1_presets.h, fait par tools/presets_at2.py). Appliquer un
  // preset pose ses onze octets et son nom.
  auto appliquePreset = [&](int ins, int k) {
    if (k < 0 || k >= FM1_NB_PRESETS) return;
    for (int o = 0; o < MD_OPL_OCTETS; o++) md_replayer_set_instr_opl(ins, o, FM1_PRESET_FM[k][o]);
    md_replayer_set_instr_gen_val(ins, MD_GEN_PROP_PANNING, 128);
    md_replayer_set_instr_name(ins, FM1_PRESET_NOMS[k]);
  };
  // DRUM KEYBOARD (GLO, troisieme mode) : chaque touche du clavier, un son de batterie a elle, pris dans les
  // presets (AdLib Tracker II : de vraies batteries) : en bas les grosses caisses, puis les caisses claires, les
  // claps, les charlestons, les toms, les percussions, en haut la cymbale. Chacune son instrument (D4..EE :
  // le LOOPER les rejoue tels quels), joues sur quatre voies tour a tour (12..15).
  static const struct { int16_t preset; uint8_t note; } kDk[27] = {
    { 0, 37 },    { 59, 37 },   { 763, 37 },  { 701, 37 },  { 1, 49 },    { 327, 49 },  { 356, 49 },
    { 408, 49 },  { 104, 49 },  { 848, 49 },  { 1528, 49 }, { 1226, 61 }, { 3, 61 },    { 4, 61 },
    { 280, 61 },  { 1918, 40 }, { 1916, 44 }, { 431, 47 },  { 1921, 52 }, { 603, 44 },  { 534, 37 },
    { 109, 61 },  { 479, 61 },  { 600, 61 },  { 436, 73 },  { 341, 61 },  { 914, 61 } };
  // Des batteries, donc des sons qui meurent seuls : certains presets ont l'enveloppe TENUE (EGT : la note
  // reste tant qu'on ne la coupe pas, et une touche de batterie ne la coupe pas) ; elle est mise en percussion
  // (EGT eteint) sur les deux operateurs, avec une chute et un relachement assez vifs.
  auto dkPrepare = [&]() {
    if (g_dkPret) return;                         // (une fois : ensuite, ce que KNOB GENERAL en a fait reste)
    g_dkPret = true;
    for (int k = 0; k < 27; k++) {
      const int ins = kDkIns0 + k;
      appliquePreset(ins, kDk[k].preset);
      // (et plus forts : la porteuse, et en connexion additive le modulateur aussi, 10 crans d'attenuation en
      // moins, ~7,5 dB ; sinon la melodie les ecrasait)
      for (int op = oplLit(ins, MD_OPL_FB_CONN, 0, 1) ? 0 : 1; op < 2; op++)
        oplEcrit(ins, MD_OPL_VOL_M + op, 0, 63, borne(oplLit(ins, MD_OPL_VOL_M + op, 0, 63) - 10, 0, 63));
      for (int op = 0; op < 2; op++) {
        oplEcrit(ins, MD_OPL_FLAGS_M + op, 5, 1, 0);
        if (oplLit(ins, MD_OPL_AD_M + op, 0, 15) < 4) oplEcrit(ins, MD_OPL_AD_M + op, 0, 15, 4);
        if (oplLit(ins, MD_OPL_SR_M + op, 0, 15) < 5) oplEcrit(ins, MD_OPL_SR_M + op, 0, 15, 5);
      }
    }
  };
  // Les sons : les instruments F0..FA. Un instrument qui n'est pas encore
  // un son de la boite (son nom : « DN BD »...) recoit le sien (kDrumFM).
  auto drumPrepare = [&]() {
    for (int k = 0; k < 11; k++) {
      const int ins = kDrumIns0 + k;
      const char *nm = md_replayer_get_instr_name(ins);
      if (nm && !strncmp(nm, "DN ", 3)) continue;   // (« DN » : la version des sons ; les plus vieux sont refaits)
      md_replayer_init_default_instrument(ins - 1);
      for (int o = 0; o < MD_OPL_OCTETS; o++) md_replayer_set_instr_opl(ins, o, kDrumFM[k][o]);
      md_replayer_set_instr_gen_val(ins, MD_GEN_PROP_PANNING, 128);
      char n2[12];
      siprintf(n2, "DN %s", kDrumCourt[k]);
      md_replayer_set_instr_name(ins, n2);
    }
  };
#endif
  // La table attachee a un instrument : OFF, puis 00, 01, 02... Un seul
  // endroit la change, pour les quatre genres d'instrument.
  // Ouvrir la table d'un instrument. S'il n'en a pas, on lui attribue la
  // premiere LIBRE plutot que de montrer un ecran vide — c'est ce que fait
  // l'iPad.
  // La premiere table VIDE. C'est ce que double-A pose sur le champ TABLE,
  // exactement comme double-A pose un chain ou une phrase neufs ailleurs.
  auto tableLibre = []() {
    for (int i = 0; i < MD_MAX_TABLES; i++)
      if (md_replayer_table_is_empty(i)) return i;
    return 0;
  };
  int derniereTable = 0;        // la derniere table employee, rappelee par A
  // La derniere commande posee dans une table, rappelee par A sur une case
  // vide. Sans ce rappel il faut traverser toute la liste a chaque fois.
  uint8_t derCmd = 0, derCmdVal = 0, derMdVal = 0;
  uint8_t derMdCmd = 1;    // le premier CC propose : on part du debut, CC 01
  auto tableDeLInstrument = [](int ins) {
    int t = md_replayer_get_instr_table(ins);
    if (t < 0 || t == MD_EMPTY) {
      t = 0;
      for (int i = 0; i < MD_MAX_TABLES; i++)
        if (md_replayer_table_is_empty(i)) { t = i; break; }
      md_replayer_set_instr_table(ins, t);
    }
    return t;
  };
  auto changeTable = [](int ins, int sens) {
    const int t = md_replayer_get_instr_table(ins);
    const int actuel = (t < 0 || t == MD_EMPTY) ? -1 : t;
    const int n = actuel + sens;
    md_replayer_set_instr_table(ins,
        n < 0 ? -1 : (n >= MD_MAX_TABLES ? MD_MAX_TABLES - 1 : n));
  };
  // Son affichage : « OFF » quand il n'y en a pas.
  auto texteTable = [](int ins, char *d) {
    const int t = md_replayer_get_instr_table(ins);
    if (t < 0 || t == MD_EMPTY) { d[0]='O'; d[1]='F'; d[2]='F'; d[3]=0; }
    else { d[0]=kHex[(t>>4)&15]; d[1]=kHex[t&15]; d[2]=0; }
  };
  auto genreInstr = [&](int ins) {
    const int k = md_replayer_get_instr_kind(ins);
    if (k == MD_INSTR_KIND_OPL) return (int)GENRE_OPL;
    if (k == MD_INSTR_KIND_MIDI) return (int)GENRE_MIDI;
    if (k == MD_INSTR_KIND_KICK) return (int)GENRE_KICK;
    return (int)GENRE_SAMPLE;
  };
  // Le champ TABLE.
  auto champTableDe = [&](int ins) {
    const int g = genreInstr(ins);
    if (g == GENRE_OPL) return kOplRangeeTable;
    return (g == GENRE_MIDI) ? 5 : 8;
  };
  // Le champ SYNC, tout en bas : le retard des echantillons, en millisecondes,
  // qui cale la DS sur le MIDI de l'iPad. C'est un reglage GLOBAL — la meme
  // valeur sur toutes les pages, quel que soit l'instrument ouvert. Il manque
  // sur la page KICK, faute de rangee libre ; on le regle depuis SAMPLE ou
  // MIDI, et il s'applique au kick comme au reste.
  auto champSyncDe = [&](int ins) {
    const int g = genreInstr(ins);
    (void)g;
    return -1;   // le reglage SYNC est sur la page PROJECT
  };
  auto nbChampsDe = [&](int ins) {
    if (genreInstr(ins) == GENRE_OPL) return kOplRangees;
    const int s = champSyncDe(ins);
    return (s < 0) ? champTableDe(ins) + 1 : s + 1;
  };
  int derA = -1; unsigned derAt = 0;   // pour detecter le double appui
  // Numeros RETENUS au moment ou l'on descend, comme chainId / phraseId sur
  // l'iPad. On ne les recalcule pas depuis le curseur : sinon la page CHAIN
  // afficherait toujours le chain de la case actuellement pointee dans SONG.
  int chainId = 0, phraseId = 0, instrId = 1;
  int chLigne = 0, chCol = 0;      // CHAIN : 16 lignes, 2 colonnes (phrase, tsp)
  int phLigne = 0, phCol = 0;      // PHRASE : 16 lignes, 6 colonnes
  int inLigne = 0, inCol = 0;      // INSTR : parametres en lignes, 4 operateurs
#ifdef FM1
  // FM-1, INSTR d'un instrument OPL3 : cinq pages a la facon du tracker DOS
  // (SELECT + droite / gauche), le curseur sur un element de la page, et
  // l'operateur montre (0 MODULATOR, 1 CARRIER), choisi sur la ligne OP.
  int inPage = 0, inItem = 0, inOp = 0;
#endif
  bool enLecture = false;
  int repereVu[MD_MAX_CHANNELS];
  for (int i2 = 0; i2 < MD_MAX_CHANNELS; i2++) repereVu[i2] = -1;
  int repereLigne = -1;   // CHAIN / PHRASE : une seule tete de lecture visible
  int bpmVu = -1;         // dernier BPM affiche, pour ne redessiner qu'au besoin
  // Seize lignes a l'ecran avec la police doublee ; l'en-tete en prend quatre.
  // Le nombre de lignes visibles se DEDUIT de la taille de l'ecran et de la
  // police, au lieu d'etre fixe : en changeant MD_ZOOM on changeait le nombre
  // de lignes tenant a l'ecran, et la derniere se retrouvait coupee — sur
  // PHRASE, la ligne 0F disparaissait purement et simplement.
  //   ligne 0 : nom du tracker et titre de page
  //   ligne 1 : noms des colonnes
  //   ligne 2 : respiration
  //   a partir de 3 : les donnees
  // Seize lignes de SONG, comme LSDJ : l'en-tete en prend trois, puis vient
  // une ligne vide et la ligne d'etat (le titre du morceau).
  const int kLignesVues = 16;
  // SONG : deux colonnes pour le numero de ligne, puis quatre par piste — une
  // pour le chevron, trois pour la valeur. Les seize pistes ne tiennent pas en
  // largeur : la grille DEFILE quand le curseur passe le bord droit, et
  // revient quand il repasse le bord gauche.
#ifdef FM1
  // FM-1 : en ZOOM, trois pistes a l'ecran et une seule case entre elles
  // (comme entre les numeros de ligne et la premiere) ; voir le ZOOM, plus bas.
  const int kColVoie = 3;
  int kPasVoie = 4;
#else
  const int kColVoie = 3, kPasVoie = 4;
#endif
#ifdef FM1
  int kVoiesEcran = 5;   // FM-1 : toujours cinq pistes a l'ecran (elles tiennent pile), la page les fait defiler ; trois en ZOOM
#else
  const int kVoiesEcran = (kCols - kColVoie + 2) / kPasVoie;
#endif
  int gaucheVoie = 0;   // la premiere piste affichee

  // curCanal designe une colonne ; visibles[] donne la piste reelle. Toutes
  // les pistes sont affichees, mais on garde l'indirection de GeneTrackerDS.
  int visibles[MD_MAX_CHANNELS], nbVisibles = 0;
  auto majVisibles = [&]() {
    nbVisibles = 0;
    for (int c = 0; c < md_replayer_get_num_channels(); c++)
      if (!md_replayer_is_channel_disabled(c)) visibles[nbVisibles++] = c;
    if (nbVisibles == 0) { visibles[0] = 0; nbVisibles = 1; }
    if (curCanal > nbVisibles) curCanal = nbVisibles;   // nbVisibles = le « + »
  };
  // La colonne « + » de SONG, apres la derniere piste, tant qu'il en reste.
#ifdef FM1
  // FM-1 : pas de colonne « + », le morceau a toujours ses 18 pistes (MD_PISTES_OPL).
  auto avecPlus = [&]() { return false; };
#else
  auto avecPlus = [&]() { return md_replayer_get_num_channels() < MD_MAX_CHANNELS; };
#endif
  auto nbColonnes = [&]() { return nbVisibles + (avecPlus() ? 1 : 0); };
  auto reel = [&](int i) { return visibles[borne(i, 0, nbVisibles - 1)]; };
  // Les seize pistes jouent toujours : un morceau ouvert ou neuf les rallume.
  auto toutesPistes = [&]() {
    for (int c = 0; c < MD_MAX_CHANNELS; c++) md_replayer_disable_channel(c, false);
    // Un autre morceau : ce que chaque voie avait retenu — derniere note,
    // dernier instrument — appartenait au precedent.
    for (int c = 0; c < MD_MAX_CHANNELS; c++) {
      instrCanal[c] = 0; derniereNote[c] = 49; noteRetenue[c] = false;
    }
    majVisibles();
    gaucheVoie = 0;
    pageVue = -1;
  };
  toutesPistes();

  // Creer un morceau neuf efface celui qui est en memoire, et rien n'est
  // encore enregistre sur la carte : on demande confirmation.
  bool demandeNouveau = false;
  // FM-1 : DELETE (PROJECT) : le navigateur choisit un projet a effacer
  // (navSuppr), puis la question, NO / YES a la croix, A pour valider, B pour
  // annuler (supprOui : YES pointe ; NO au depart).
  bool navSuppr = false, demandeSuppr = false, supprOui = false;
  int menuProjet = 0;
  // Les lignes du menu PROJECT. FM-1 : ZOOM juste avant ABOUT.
#ifdef FM1
  enum { kMenuZoom = 7, kMenuApropos = 8, kMenuSortie = 9, kMenuExport = 10, kMenuSuppr = 11, kMenuLum = 12, kMenuDernier = 12 };
  // Les lignes MONTREES, dans l'ordre. menuProjet reste le numero de la ligne.
  // Ni PATTERN LEN (6 : pas sur un tracker facon LSDJ), ni SYNC / SYNC TEST
  // (4, 5 : le decalage entre les samples et la RetroWave de la DS ; le FM-1
  // n'a ni l'un ni l'autre).
  // EXPORT A2M juste sous SAVE.
  static const int kMenuVu[] = { 0, 1, kMenuExport, 2, kMenuSuppr, 3, kMenuZoom, kMenuSortie, kMenuLum, kMenuApropos };
  static const int kMenuNbVu = (int)(sizeof kMenuVu / sizeof kMenuVu[0]);
  auto rangMenu = [](int id) { for (int i = 0; i < kMenuNbVu; i++) if (kMenuVu[i] == id) return i; return 0; };
  const int kColValMenu = 7;    // les valeurs, juste apres les noms courts (TEMPO)
#else
  enum { kMenuApropos = 7, kMenuDernier = 7 };
  auto rangMenu = [](int id) { return id; };
  const int kColValMenu = 14;
#endif
  bool syncTest = false;   // SYNC TEST : clics OPL3 + sample   // ligne pointee dans le menu de la page PROJECT

  // ── Ouvrir un projet depuis la carte SD ─────────────────────────────────
  // Le format est le .mdm du moteur, exactement celui qu'ecrit l'iPad : c'est
  // le contrat entre les deux projets, un morceau fait la-bas doit s'ouvrir
  // ici. Les .dmf de DefleMask sont acceptes aussi, par le meme importeur que
  // la demo.
  // (carteOK : la carte a ete montee au demarrage, avant l'USB.)



  // On montre TOUT ce que contient le dossier, pas seulement les morceaux :
  // sans les autres fichiers on ne sait pas ou l'on se trouve sur la carte.
  // Les morceaux ouvrables sont en clair, le reste en attenue.
  // Le navigateur sert a deux choses : ouvrir un MORCEAU, ou charger un
  // ECHANTILLON. Ce qu'il met en avant depend de ce qu'on est venu chercher.
  int navGenre = 0;   // 0 = morceaux, 1 = echantillons
  auto estOuvrable = [&](const char *n) {
    const int L = (int)strlen(n);
    if (navGenre == 1) return L >= 5 && !strcasecmp(n + L - 4, ".wav");
    // ── NOS PROJETS SONT DES « .XT », ET RIEN D'AUTRE ─────────────────────
    // Ce n'est pas un module XM : 16 pistes, quatre colonnes CC, des
    // instruments MIDI, un kick de synthese, une banque de samples. Lui donner
    // « .XM » ferait passer notre fichier pour un module, que tous les
    // lecteurs refuseraient. « .XM » reste libre pour l'export, a venir.
    // Les anciens « .XMM » s'ouvrent toujours, sans que rien ne l'annonce : le
    // contenu n'a pas change, et il n'y a aucune raison d'obliger a renommer
    // ce qui existe deja. Seul l'enregistrement, lui, ecrit desormais en .XT.
    // Les morceaux sont des modules AdLib Tracker 2 (.A2M) ; leur .OPS, s'il
    // existe, est relu avec eux.
#ifdef FM1
    // FM-1 : les projets s'y enregistrent en .OBT (OPLBend Tracker : le
    // fichier du moteur, compresse) ; les anciens .XT s'ouvrent toujours.
    if (L >= 4 && !strcasecmp(n + L - 3, ".xt")) return true;
    if (L >= 5 && !strcasecmp(n + L - 4, ".obt")) return true;
#endif
    return L >= 5 && !strcasecmp(n + L - 4, ".a2m");
  };

  auto scanne = [&]() {
    nbFichiers = 0; selFichier = 0; hautFichier = 0;
    DIR *d = opendir(dossier);
    if (!d) { dossier[0] = '/'; dossier[1] = 0; d = opendir(dossier); }
    if (!d) return;
    // Remonter d'un cran, sauf a la racine.
    if (strcmp(dossier, "/") != 0) {
      strcpy(fichiers[0], ".."); typeFic[0] = 2; nbFichiers = 1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && nbFichiers < 80) {
      const char *n = e->d_name;
      if (!strcmp(n, ".") || !strcmp(n, "..")) continue;
      if (n[0] == '.') continue;   // fichiers caches du systeme : du bruit
      char chemin[192];
      const int L = (int)strlen(dossier);
      siprintf(chemin, "%s%s%s", dossier, (L && dossier[L-1] == '/') ? "" : "/", n);
      struct stat st;
      const bool dir = (stat(chemin, &st) == 0) && S_ISDIR(st.st_mode);
      strncpy(fichiers[nbFichiers], n, 39);
      fichiers[nbFichiers][39] = 0;
      typeFic[nbFichiers] = dir ? 2 : (estOuvrable(n) ? 1 : 0);
      nbFichiers++;
    }
    closedir(d);
    // Par ordre alphabetique, sans tenir compte des majuscules : les dossiers
    // d'abord, puis les fichiers. « .. » reste en tete.
    {
      const int deb = (nbFichiers > 0 && !strcmp(fichiers[0], "..")) ? 1 : 0;
      for (int i = deb + 1; i < nbFichiers; i++)
        for (int j = i; j > deb; j--) {
          const auto *tj = WEB_PTR(&typeFic[j - 1]);   // (WEB_PTR : md_replayer.h)
          const bool dirA = tj[0] == 2, dirB = tj[1] == 2;
          const bool avant = (dirB && !dirA) ||
              (dirA == dirB && strcasecmp(fichiers[j], fichiers[j - 1]) < 0);
          if (!avant) break;
          char t[40]; strcpy(t, fichiers[j]); strcpy(fichiers[j], fichiers[j - 1]);
          strcpy(fichiers[j - 1], t);
          auto *tf = WEB_PTR(&typeFic[j - 1]);   // (WEB_PTR : md_replayer.h)
          const auto ty = tf[1]; tf[1] = tf[0]; tf[0] = ty;
        }
    }
    // Le curseur part sur la premiere VRAIE entree, pas sur « .. » : un A de
    // trop remontait d'un dossier sans qu'on comprenne ou l'on etait passe.
    if (nbFichiers > 1 && !strcmp(fichiers[0], "..")) selFichier = 1;
  };

  // Entre dans un sous-dossier, ou remonte d'un cran sur « .. ».
  auto entre = [&](const char *nom) {
    char quitte[40] = "";   // en remontant, le dossier d'ou l'on sort
    if (!strcmp(nom, "..")) {
      char *barre = strrchr(dossier, '/');
      if (barre) { strncpy(quitte, barre + 1, 39); quitte[39] = 0; }
      if (barre == dossier) dossier[1] = 0;      // on revient a la racine
      else if (barre) *barre = 0;
    } else {
      const int L = (int)strlen(dossier);
      if (L + (int)strlen(nom) + 2 >= (int)sizeof(dossier)) return;
      if (!(L && dossier[L-1] == '/')) strcat(dossier, "/");
      strcat(dossier, nom);
    }
    scanne();
    // En remontant, le curseur se pose sur le dossier qu'on vient de quitter.
    if (quitte[0])
      for (int i = 0; i < nbFichiers; i++)
        if (!strcmp(fichiers[i], quitte)) { selFichier = i; break; }
  };

  // Enregistre le morceau au format .mdm — celui du moteur, donc celui de
  // l'iPad. On ne demande pas de nom : il n'y a pas de clavier, et surtout on
  // n'ECRASE JAMAIS un fichier existant. Le premier numero libre est pris.
  static char dernierNom[64] = "";
  // Le nom du morceau courant, sans chemin ni extension. Il suit le fichier
  // ouvert : enregistrer proposera ce nom-la, et non celui d'un projet
  // precedent qu'on ecraserait sans le vouloir.
  static char nomProjet[16] = "";
  // Le nom se saisit dans une petite fenetre : pas de clavier sur une DS, on
  // fait donc defiler les lettres une par une.
  bool dialogueNom = false, assombrirDemande = false;
  // Le nom du fichier, et la position dans la grille de caracteres. Declares
  // ICI, avant les aides du MIDI : elles posent le curseur en ouvrant leur
  // fenetre, et partagent la meme grille.
  static char sauveNom[9] = "";
  int nomLig = 0, nomCol = 0;
  // ── A quoi le MIDI de ce morceau est branche ─────────────────────────────
  // Le tracker ne peut PAS le deviner : cote USB, la DS est le peripherique,
  // et l'hote ne dit ni quelle application l'a ouvert, ni quel projet y est
  // charge. On le DEMANDE donc a l'enregistrement, des qu'un instrument MIDI
  // existe, et on le garde dans le morceau : en le reouvrant, on sait quel
  // logiciel lancer et quelle table de CC y charger.
  bool dialogueMidi = false;
  int midiChamp = 0;              // 0 le logiciel, 1 son projet, 2 le MIDI map
  static char midiSoft[16] = "", midiProj[16] = "", midiMap[16] = "";
  int midiCanal = 0;              // le canal dont on remplit la fiche
  bool midiApresSave = false;     // la fenetre fait partie d'un enregistrement
  int midiLigne = 0;              // curseur de la page MIDI TARGETS
  int pageRetourMidi = PAGE_PROJECT;   // ou revenir en fermant la fenetre
  // Ce qu'on a DEJA propose pendant CET enregistrement. Sans cette trace, un
  // canal laisse vide revenait sans fin : canalAdemander() cherche les fiches
  // vides, et une fiche qu'on vient de valider vide en est une — la fenetre se
  // rouvrait sur le meme canal et le morceau ne s'ecrivait jamais. Un canal
  // n'est donc propose qu'une seule fois par enregistrement.
  u16 midiDemandes = 0;
  int midiRang = 0, midiTotal = 0;   // le « 2/3 » du titre de la fenetre
  // Le premier canal qu'un instrument MIDI emploie et dont la fiche est vide.
  // -1 quand il n'y a plus rien a demander : l'enregistrement part alors tout
  // droit, en deux touches.
  auto canalAdemander = [&]() {
    for (int c = 0; c < MD_MIDI_CANAUX; c++) {
      if (!md_replayer_canal_midi_utilise(c)) continue;
      if (midiDemandes & (1 << c)) continue;   // deja propose, on n'insiste pas
      char s[16], p[16], m[16];
      md_replayer_get_midi_cible(c, s, p, m, 16);
      if (!s[0]) return c;
    }
    return -1;
  };
  auto ouvreDialogueMidi = [&](int canal, bool apresSave) {
    md_replayer_get_midi_cible(canal, midiSoft, midiProj, midiMap, 16);
    midiCanal = canal;
    midiApresSave = apresSave;
    if (apresSave) { midiDemandes |= (u16)(1 << canal); midiRang++; }
    midiChamp = 0;
    // Deja renseigne ? Le curseur part sur OK : trois A, et c'est valide.
    if (midiSoft[0]) { nomLig = 3; nomCol = 9; }
    else             { nomLig = 1; nomCol = 0; }
    // La fenetre se dessine sur la page PROJECT ; on retient d'ou l'on vient
    // pour y revenir en fermant.
    pageRetourMidi = page;
    page = PAGE_PROJECT;
    dialogueMidi = true;
    assombrirDemande = true;
    pageVue = -1;
  };


  auto enregistre = [&](const char *nom) {
    // Ou ecrire : a cote de la cartouche si on connait son chemin, sinon la
    // carte, sinon la racine. Les memes candidats que pour le journal.
    char base[192] = "";
    if (__system_argv->argvMagic == ARGV_MAGIC &&
        __system_argv->argc > 0 && __system_argv->argv[0]) {
      strncpy(base, __system_argv->argv[0], sizeof(base) - 24);
      base[sizeof(base) - 24] = 0;
      char *barre = strrchr(base, '/');
      if (barre) barre[1] = 0; else base[0] = 0;
    }
    // On ecrit dans le dossier des MORCEAUX, jamais dans celui ou le
    // navigateur s'est arrete.
    //
    // Les deux partageaient la meme variable : apres avoir charge un
    // echantillon, elle pointait sur « Samples », et le morceau suivant y
    // atterrissait. Le journal l'a montre noir sur blanc —
    // « ENREGISTRE : .../Samples/MONMORCEAU.MDM ».
    creeSiAbsent(dossierSongs);        // au premier enregistrement
    char courant[136];
    const int Ld = (int)strlen(dossierSongs);
    siprintf(courant, "%s%s", dossierSongs,
             (Ld && dossierSongs[Ld-1] == '/') ? "" : "/");
    const char *racines[4] = { courant, base, "sd:/", "/" };

    // Le nom vient de l'utilisateur : les espaces de fin sont de la place
    // laissee vide dans la fenetre, pas des caracteres voulus.
    char propre[10]; int np = 0;
    for (int i2 = 0; i2 < 8 && nom[i2]; i2++)
      if (nom[i2] != ' ') propre[np++] = nom[i2];
    propre[np] = 0;
    if (np == 0) { strcpy(propre, "SONG"); }

    char chemin[224];
    bool ok = false;
    for (int r = 0; r < 4 && !ok; r++) {
      if (!racines[r][0]) continue;
#ifdef FM1
      // FM-1 : en .OBT (OPLBend Tracker) : le format du moteur, compresse
      // d'un tenant comme un .A2M (compat/fm1_obt.c). Un .XT ouvert se
      // reenregistre en .OBT, a cote.
      // Un morceau ouvert d'un .A2M, qui y tient encore tout entier : en .A2M (saveEnA2m).
      siprintf(chemin, "%s%s.%s", racines[r], propre, saveEnA2m ? "A2M" : "OBT");
      char compteSauve[64] = "";
      char msgA2m[48];
      fm1_fs_raz_erreur();
      ok = saveEnA2m ? fm1_exporte_a2m(chemin, msgA2m, sizeof msgA2m) : md_replayer_save_file(chemin);
      // Pas la place pour la nouvelle copie a cote de l'ancienne : on efface
      // l'ancienne et on recommence (le morceau est toujours en memoire).
      struct stat st;
      if (!ok && fm1_fs_plein() && stat(chemin, &st) == 0) {
        remove(chemin);
        fm1_fs_raz_erreur();
        ok = saveEnA2m ? fm1_exporte_a2m(chemin, msgA2m, sizeof msgA2m) : md_replayer_save_file(chemin);
      }
      strncpy(g_raisonSave, ok ? "" : fm1_fs_erreur()[0] ? fm1_fs_erreur() : "WRITE ERROR", sizeof g_raisonSave - 1);
      siprintf(compteSauve, ok ? "XT : %s" : "XT : ECHEC %s (%s)", propre, g_raisonSave);
#else
      siprintf(chemin, "%s%s.A2M", racines[r], propre);
      // Un .A2M pour AT2, et un .OPS a cote si le morceau contient ce qu'AT2
      // ne connait pas. Une racine non inscriptible echoue simplement et on
      // passe a la suivante.
      char compteSauve[64] = "";
      ok = md_enregistre_a2m(chemin, compteSauve, sizeof compteSauve);
#endif
      journal(compteSauve);
#ifdef FM1
      // Passe en .OBT : le .A2M d'ou il venait s'efface (pas deux versions du meme morceau ; EXPORT A2M le refait)
      if (ok && !saveEnA2m && ouvertA2m && origineA2m[0]) {
        remove(origineA2m);
        journal(origineA2m);
        ouvertA2m = false;
        origineA2m[0] = 0;
      }
#endif
      if (ok) { strncpy(dernierNom, chemin, 63); dernierNom[63] = 0;
                strncpy(nomProjet, propre, 15); nomProjet[15] = 0; }
    }
    return ok;
  };

  // Enregistre et le DIT : sans retour visible, on ne sait pas si ça a eu lieu.
  // Deux chemins y mènent — avec ou sans la fenêtre du MIDI — et ils doivent
  // se comporter pareil.
  auto enregistreEtDis = [&](const char *nom) {
    char e[90];
    if (enregistre(nom)) {
      siprintf(e, "ENREGISTRE : %s", dernierNom);
      siprintf(msgProjet, "SAVED %s", nomProjet);
    } else {
      siprintf(e, "ECHEC DE L'ENREGISTREMENT");
      strcpy(msgProjet, "SAVE FAILED");
    }
    journal(e);
    msgProjetJusqu = secondes + 3;           // trois secondes a l'ecran
#ifdef FM1
    if (strncmp(msgProjet, "SAVED", 5)) fm1_annonce2("SAVE FAILED", g_raisonSave);   // (en gros, sur toute page)
    else fm1_annonce2("SAVED", nomProjet);
#endif
  };

  auto ouvre = [&](const char *nom) {
    char chemin[128];
    const int L = (int)strlen(dossier);
    siprintf(chemin, "%s%s%s", dossier, (L && dossier[L-1] == '/') ? "" : "/", nom);
#ifdef FM1
    fm1_ecran_chargement();   // « LOADING... » pendant l'import ou la lecture du fichier
    // Un morceau charge : le CIRCUIT BENDING revient a zero (aucun effet, toutes les pistes).
    bendBits = 0; bendSel = 0; bendCol = 0; bendGauche = 0;
    for (int k = 0; k < 7; k++) { bendPistes[k] = 0xFFFFFFFFu; fm1_bend_pistes(k, bendPistes[k]); }
    fm1_bend(0);
    for (int k = 0; k < 16; k++) glitchPas[k] = 0;   // (et le GLITCH aussi : le tampon rendu avant l'import)
    drumArrete();                                   // (la DRUM MACHINE s'arrete ; ses pas viennent du fichier)
    glitchSel = 0; glitchVu = false; glitchOn = false;
    glitchMaj();
    for (int k = 0; k < 7; k++) mfxDose[k] = 0;
    mfxSel = 0; mfxMaj();                           // (et le MASTER FX)
    for (int k = 0; k < 6; k++) globalPousse[k] = 0;
    drumOctTout = 0;
    demiAM = false; memset(amAvant, 0, sizeof amAvant);
    fm1_mode_tracker();                            // (un morceau charge : SEQ revient en TRACKER, plus de LOOPER)
    g_loopNbGarde = 0;                             // (les sons de la boucle, mis de cote : remis apres le chargement)
    for (int i = 0; i < g_loopN; i++) {
      const int ii = (int)fm1_opaque(g_loop[i].ins);
      if (ii >= kDkIns0 && ii < kDkIns0 + 27) continue;
      int k = 0;
      while (k < g_loopNbGarde && g_loopGardeIns[k] != ii) k++;
      if (k == g_loopNbGarde && k < 6) {
        g_loopGardeIns[k] = (uint8_t)ii;
        for (int o = 0; o < MD_OPL_OCTETS; o++) g_loopGardeOpl[k][o] = md_replayer_get_instr_opl(ii, o);
        g_loopNbGarde++;
      }
    }
    g_loopARemettre = g_loopN > 0;
    g_dkPret = false;                              // (DRUM KEYBOARD : ses sons, a refaire dans le nouveau morceau)
    if (g_loopEtat == 2 || g_loopEtat == 3) g_loopEtat = 4;   // (et sa boucle s'arrete : ses sons sont ceux d'avant)
    else if (g_loopEtat == 1 || g_loopEtat == 5) g_loopEtat = 0;
    loopSilence();
    selPos = 0; defautPris = false;                // (les sons par defaut : ceux du morceau charge)
    editN = 0;                                     // (EDIT : plus rien a remettre, ce sont d'autres sons)
    free(sonDefaut); free(sonCourant); sonDefaut = sonCourant = nullptr;
    bendPitch = bendFin = bendHorloge = 0; horlogeMaj();   // (et CIRCUIT BENDING 2)
#endif
    // Un module AdLib Tracker 2 est IMPORTE : converti en SONG/CHAIN/PHRASE.
    const int Ln = (int)strlen(nom);
    ouvertA2m = false;
    if (Ln >= 5 && !strcasecmp(nom + Ln - 4, ".a2m")) {
      char compte[64];
      bool ok = md_importe_a2m(chemin, compte, sizeof compte);
      ouvertA2m = ok;
      snprintf(origineA2m, sizeof origineA2m, "%s", ok ? chemin : "");
      if (ok) {
        char compteOps[64] = "";
        if (md_charge_ops(chemin, compteOps, sizeof compteOps) || compteOps[0])
          strcpy(compte, compteOps);
      }
      journal(compte);
      snprintf(msgProjet, sizeof msgProjet, "%.39s", compte);
      msgProjetJusqu = secondes + 6;
      return ok;
    }
    // En flux : la banque va directement du fichier a sa place en memoire.
    return md_replayer_load_file(chemin);
  };
  // Declare AVANT la boucle : les fonctions de copie et de collage, definies
  // ici, doivent pouvoir signaler qu'elles ont modifie le morceau.
  bool modifie = false;

  // ── Selection et presse-papier, regles reprises de l'iPad ───────────────
  // SELECT+B ARME une selection : il ne la bascule pas. C'est ce que fait la
  // reference, et c'est ce qui rend le copier-coller utilisable — sinon, en
  // gardant SELECT enfonce, chaque B annulait la selection qu'on venait de
  // faire. Une selection se termine en copiant, en collant, ou en changeant
  // d'ecran.
  bool selActive = false;
  int selAncreLigne = 0, selAncreCol = 0;

  static uint8_t clipSong[MD_MAX_CHANNELS][MD_SONG_ROWS];
  static uint8_t clipChPh[MD_ROWS_PER_CHAIN];
  static int8_t  clipChTr[MD_ROWS_PER_CHAIN];
  static md_phrase_row_t clipPhr[MD_ROWS_PER_PHRASE];   // lignes entieres
  int clipPage = -1, clipLignes = 0, clipCol0 = 0, clipCols = 0;

  // Ligne et colonne du curseur sur la page courante.
  auto curL = [&]() { return page == PAGE_SONG ? curLigne
                           : page == PAGE_CHAIN ? chLigne : phLigne; };
  auto curC = [&]() { return page == PAGE_SONG ? curCanal
                           : page == PAGE_CHAIN ? chCol : phCol; };
  auto poseCurseur = [&](int l, int c) {
    if (page == PAGE_SONG)       { curLigne = l; curCanal = c; }
    else if (page == PAGE_CHAIN) { chLigne = l;  chCol = c; }
    else                         { phLigne = l;  phCol = c; }
  };

  // La case (ligne, colonne) fait-elle partie de la selection en cours ?
  auto estSelectionne = [&](int l, int c) {
    if (!selActive) return false;
    const int l0 = selAncreLigne < curL() ? selAncreLigne : curL();
    const int l1 = selAncreLigne < curL() ? curL() : selAncreLigne;
    const int c0 = selAncreCol   < curC() ? selAncreCol   : curC();
    const int c1 = selAncreCol   < curC() ? curC()        : selAncreCol;
    return l >= l0 && l <= l1 && c >= c0 && c <= c1;
  };
#ifdef FM1
  // FM-1, pendant une selection (SELECT + B) : le curseur est un pave JAUNE,
  // par-dessus tout (la vue le suit), et un cadre jaune entoure la selection.
  auto paveJaune = [&](int col, int lig, int n) {
    const int x0 = (col + g_colOrigine) * MD_CELL_W - 1, y0 = lig * MD_CELL_H;
    for (int y = y0; y < y0 + MD_GLYPHE_H; y++)
      for (int x = x0; x < x0 + n * MD_CELL_W; x++) pixel(x, y, kSelection);
    fm1_vu_accent(y0 * kEcranL + x0 + 1);
  };
  // le cadre : des cases col0 (incluse) a col1 (exclue), des rangees lig0 a lig1
  auto cadreJaune = [&](int col0, int col1, int lig0, int lig1) {
    const int x0 = (col0 + g_colOrigine) * MD_CELL_W - 2, x1 = (col1 + g_colOrigine) * MD_CELL_W;
    const int y0 = lig0 * MD_CELL_H, y1 = (lig1 + 1) * MD_CELL_H - 1;
    for (int x = x0; x <= x1; x++) { pixel(x, y0, kSelection); pixel(x, y1, kSelection); }
    for (int y = y0; y <= y1; y++) { pixel(x0, y, kSelection); pixel(x1, y, kSelection); }
  };
#endif

  auto copier = [&]() {
    if (!selActive) return;
    const int l0 = selAncreLigne < curL() ? selAncreLigne : curL();
    const int l1 = selAncreLigne < curL() ? curL() : selAncreLigne;
    const int c0 = selAncreCol   < curC() ? selAncreCol   : curC();
    const int c1 = selAncreCol   < curC() ? curC()        : selAncreCol;
    clipPage = page; clipLignes = l1 - l0 + 1;
    clipCol0 = c0;   clipCols = c1 - c0 + 1;
    for (int l = 0; l < clipLignes; l++) {
      if (page == PAGE_SONG) {
        for (int c = 0; c < clipCols; c++)
          clipSong[c][l] = md_replayer_get_song(reel(c0 + c), l0 + l);
      } else if (page == PAGE_CHAIN) {
        md_replayer_get_chain((uint8_t)chainId, l0 + l, &clipChPh[l], &clipChTr[l]);
      } else {
        md_replayer_get_phrase_row((uint8_t)phraseId, l0 + l, &clipPhr[l]);
      }
    }
    // Le curseur revient a l'ancre, comme dans la reference.
    poseCurseur(selAncreLigne, selAncreCol);
    selActive = false;
    modifie = true;
  };

  // ── Le CLONE PROFOND, regles reprises telles quelles de l'iPad ──────────
  // Coller duplique des NUMEROS : les copies pointent sur les memes chains,
  // les memes phrases, les memes instruments — retoucher la copie retouche
  // l'original. Le clone profond recopie ce qui est dessous dans des
  // emplacements libres, pour obtenir une variation modifiable sans rien
  // casser.
  //
  // Ce qu'il duplique depend de l'ecran :
  //   SONG   la chain visee, ET toutes les phrases qu'elle emploie
  //   CHAIN  la phrase visee
  //   PHRASE l'instrument, et seulement sur sa colonne
  //
  // Une source qui revient plusieurs fois dans la selection n'est clonee
  // QU'UNE FOIS : les lignes qui la partageaient partagent aussi la copie.
  auto chainLibre = [&]() {
    static bool pris[MD_MAX_CHAINS];
    memset(pris, 0, sizeof(pris));
    for (int c = 0; c < MD_MAX_CHANNELS; c++)
      for (int r = 0; r < MD_SONG_ROWS; r++) {
        uint8_t v = md_replayer_get_song(c, r);
        if (v != MD_EMPTY && v < MD_MAX_CHAINS) pris[v] = true;
      }
    for (int id = 0; id < MD_MAX_CHAINS; id++) {
      if (pris[id]) continue;
      bool vide = true;
      for (int r = 0; r < MD_ROWS_PER_CHAIN && vide; r++) {
        uint8_t ph; int8_t tr;
        md_replayer_get_chain((uint8_t)id, r, &ph, &tr);
        if (ph != MD_EMPTY) vide = false;
      }
      if (vide) return id;
    }
    // Aucun vide : on prend le premier inutilise et on le nettoie.
    for (int id = 0; id < MD_MAX_CHAINS; id++)
      if (!pris[id]) {
        for (int r = 0; r < MD_ROWS_PER_CHAIN; r++)
          md_replayer_set_chain((uint8_t)id, r, MD_EMPTY, 0);
        return id;
      }
    return MD_MAX_CHAINS - 1;
  };

  auto phraseLibre = [&]() {
    static bool pris[MD_MAX_PHRASES];
    memset(pris, 0, sizeof(pris));
    for (int ch = 0; ch < MD_MAX_CHAINS; ch++)
      for (int r = 0; r < MD_ROWS_PER_CHAIN; r++) {
        uint8_t ph; int8_t tr;
        md_replayer_get_chain((uint8_t)ch, r, &ph, &tr);
        if (ph != MD_EMPTY && ph < MD_MAX_PHRASES) pris[ph] = true;
      }
    for (int id = 0; id < MD_MAX_PHRASES; id++) {
      if (pris[id]) continue;
      bool vide = true;
      for (int r = 0; r < MD_ROWS_PER_PHRASE && vide; r++) {
        md_phrase_row_t rw;
        md_replayer_get_phrase_row((uint8_t)id, r, &rw);
        // Attention a la velocite : sa valeur VIDE est MD_EMPTY (0xFF), pas
        // zero — zero est un volume nul, donc le silence. En la lisant comme
        // une donnee presente, toutes les phrases paraissaient occupees ;
        // phraseLibre tombait dans sa branche de secours, effacait la phrase
        // choisie en y ecrivant velocite zero, et la voie restait muette.
        if (rw.note || rw.instr || rw.vel != MD_EMPTY || rw.cmd != MD_EMPTY ||
            rw.mdcmd != MD_EMPTY || rw.cc[0] != MD_EMPTY ||
            rw.cc[1] != MD_EMPTY || rw.cc[2] != MD_EMPTY)
          vide = false;
      }
      if (vide) return id;
    }
    for (int id = 0; id < MD_MAX_PHRASES; id++)
      if (!pris[id]) {
        for (int r = 0; r < MD_ROWS_PER_PHRASE; r++)
          md_replayer_clear_phrase_row((uint8_t)id, r);
        return id;
      }
    return MD_MAX_PHRASES - 1;
  };

  auto instrLibre = [&]() {
    static bool pris[MD_MAX_INSTRUMENTS + 1];
    memset(pris, 0, sizeof(pris));
    for (int ph = 0; ph < MD_MAX_PHRASES; ph++)
      for (int r = 0; r < MD_ROWS_PER_PHRASE; r++) {
        uint8_t no,i2,ve,cm,cv,mc,mv;
        md_replayer_get_phrase((uint8_t)ph, r, &no,&i2,&ve,&cm,&cv,&mc,&mv);
        if (i2 && i2 <= MD_MAX_INSTRUMENTS) pris[i2] = true;
      }
    for (int id = 1; id <= MD_MAX_INSTRUMENTS; id++)
      if (!pris[id]) return id;
    return MD_MAX_INSTRUMENTS;
  };
#ifdef FM1
  // FM-1 : un instrument NEUF (le premier libre, a son reglage par defaut),
  // pose sur la ligne 0 de la phrase ph, sans note : le data a son son.
  auto instrNeufDans = [&](int ph) {
    const int ins = instrLibre();
    md_replayer_init_default_instrument(ins - 1);
    if (ph >= 0 && ph < MD_MAX_PHRASES)
      md_replayer_set_phrase((uint8_t)ph, 0, 0, (uint8_t)ins, MD_EMPTY, MD_EMPTY, 0, MD_EMPTY, 0);
    return ins;
  };
  // Une phrase neuve (double A), avec l'instrument d'avant, pose sur sa
  // ligne 0 : celui de la piste, sinon celui de la derniere phrase ; un
  // instrument neuf seulement s'il n'y en a encore aucun.
  auto phraseNeuve = [&](int voie) {
    const int ph = phraseLibre();
    int ins = instrCanal[voie];
    if (!ins) {
      for (int r = 0; r < MD_ROWS_PER_PHRASE && !ins; r++) {
        uint8_t no,i2,ve,cm,cv,mc,mv;
        md_replayer_get_phrase((uint8_t)dernierePhrase, r, &no,&i2,&ve,&cm,&cv,&mc,&mv);
        ins = i2;
      }
    }
    if (!ins) ins = instrNeufDans(ph);
    else if (ph >= 0 && ph < MD_MAX_PHRASES)
      md_replayer_set_phrase((uint8_t)ph, 0, 0, (uint8_t)ins, MD_EMPTY, MD_EMPTY, 0, MD_EMPTY, 0);
    instrCanal[voie] = ins;
    return ph;
  };
  auto chainSansPhrase = [&](int id) {
    for (int r = 0; r < MD_ROWS_PER_CHAIN; r++) {
      uint8_t ph; int8_t tsp;
      md_replayer_get_chain((uint8_t)id, r, &ph, &tsp);
      if (ph != MD_EMPTY) return false;
    }
    return true;
  };
#endif

  // ── Ce qu'une case de NOTE vide reprend, comme dans LSDJ ─────────────────
  // La derniere note et le dernier instrument poses sur la voie. Tant que la
  // voie n'en a pas retenu, on les reprend de la phrase elle-meme : la ligne
  // pleine la plus proche, au-dessus d'abord. Un instrument neuf n'est cree
  // que si la phrase n'en contient aucun — l'ancien code en creait un des que
  // la voie n'avait rien retenu, si bien qu'une note posee apres des notes de
  // l'instrument 01 partait sur le 02.
  auto repriseDansPhrase = [&](int ph, int ligne, uint8_t *note, uint8_t *instr) {
    *note = 0; *instr = 0;
    for (int pas = 1; pas < MD_ROWS_PER_PHRASE; pas++)
      for (int sens2 = -1; sens2 <= 1; sens2 += 2) {
        const int r = ligne + sens2 * pas;
        if (r < 0 || r >= MD_ROWS_PER_PHRASE) continue;
        uint8_t no,i2,ve,cm,cv,mc,mv;
        md_replayer_get_phrase((uint8_t)ph, r, &no,&i2,&ve,&cm,&cv,&mc,&mv);
        if (!*note && no && no != MD_EMPTY) *note = no;
        if (!*instr && i2) *instr = i2;
        if (*note && *instr) return;
      }
  };
  auto instrPourNote = [&](int voie, int ph, int ligne) {
    if (instrCanal[voie]) return instrCanal[voie];
    uint8_t n, i2;
    repriseDansPhrase(ph, ligne, &n, &i2);
    if (!i2) {
      const int ni = instrLibre();
      md_replayer_init_default_instrument(ni - 1);   // un instrument OPL
      if (voie >= OPL_NB_PISTES) md_replayer_set_instr_kind(ni, MD_INSTR_KIND_PCM);
      i2 = (uint8_t)ni;
    }
    instrCanal[voie] = i2;
    return (int)i2;
  };
  // ── Un CC s'envoie des qu'on le regle, lecture arretee comprise ──────────
  // Sans ca, le MIDI learn d'un synthe (Model D…) ne recevait rien tant qu'on
  // n'avait pas lance la lecture — et il apprenait alors ce que le morceau
  // jouait, pas le CC qu'on etait en train de choisir.
  //
  // Le canal est celui de l'instrument de la ligne ; a defaut, celui de la
  // voie, puis celui que la phrase emploie ailleurs. Comme en lecture, un CC
  // ne part que pour un instrument MIDI.
  auto instrDuCC = [&](int ph, int ligne, uint8_t insLigne) {
    if (insLigne) return (int)insLigne;
    if (instrCanal[voieCourante]) return instrCanal[voieCourante];
    uint8_t n, i2;
    repriseDansPhrase(ph, ligne, &n, &i2);
    return (int)i2;
  };
  auto envoieCC = [&](int ins, uint8_t cc, uint8_t val) {
    if (cc > 127) return;
    // ⚠️ SEULEMENT A L'ARRET. Pendant la lecture, c'est la tete de lecture qui
    // envoie les CC quand elle passe sur la ligne : les envoyer AUSSI en
    // reglant ferait sauter la valeur en avance, et brouillerait ce qu'on
    // entend. A l'arret, au contraire, c'est le seul moyen de regler un CC a
    // l'oreille et de faire un MIDI learn.
    if (enLecture) return;
#ifdef FM1
    // FM-1 : ces colonnes portent les effets d'AT2 (FX1, FX2), et il n'y a pas
    // de MIDI : ni CC a envoyer, ni message.
    (void)ins; (void)val;
    return;
#endif
    if (ins < 1 || md_replayer_get_instr_kind(ins) != MD_INSTR_KIND_MIDI) {
      strcpy(msgProjet, "CC NEEDS A MIDI INSTRUMENT");
      msgProjetJusqu = secondes + 2;
      return;
    }
    xt_midi_send((uint8_t)(0xB0 | (md_replayer_get_instr_midi_channel(ins) & 15)),
                 cc, (uint8_t)(val & 0x7F));
  };
  auto notePourCase = [&](int voie, int ph, int ligne) {
    if (!noteRetenue[voie]) {
      uint8_t n, i2;
      repriseDansPhrase(ph, ligne, &n, &i2);
      if (n) derniereNote[voie] = n;
      noteRetenue[voie] = true;
    }
    return derniereNote[voie];
  };

  auto cloneProfond = [&]() {
    if (!selActive) return;
    const int l0 = selAncreLigne < curL() ? selAncreLigne : curL();
    const int l1 = selAncreLigne < curL() ? curL() : selAncreLigne;
    // Sur PHRASE, il n'y a rien « dessous » ailleurs que sur la colonne
    // INSTRUMENT : le clone n'y a aucun sens.
    if (page == PAGE_PHRASE && phCol != 1) { selActive = false; return; }

    static int mapPh[MD_MAX_PHRASES];
    static int mapIn[MD_MAX_INSTRUMENTS + 1];

    if (page == PAGE_SONG) {
      for (int i = 0; i < MD_MAX_PHRASES; i++) mapPh[i] = -1;
      const int col = reel(curCanal);
      for (int r = l0; r <= l1; r++) {
        uint8_t src = md_replayer_get_song(col, r);
        if (src == MD_EMPTY) continue;
        int nc = chainLibre();
        for (int k = 0; k < MD_ROWS_PER_CHAIN; k++) {
          uint8_t ph; int8_t tr;
          md_replayer_get_chain(src, k, &ph, &tr);
          if (ph != MD_EMPTY) {
            if (mapPh[ph] < 0) {
              int np = phraseLibre();
              for (int pr = 0; pr < MD_ROWS_PER_PHRASE; pr++) {
                md_phrase_row_t rw;       // la ligne entiere, CC compris
                md_replayer_get_phrase_row(ph, pr, &rw);
                md_replayer_set_phrase_row((uint8_t)np, pr, &rw);
              }
              mapPh[ph] = np;
            }
            ph = (uint8_t)mapPh[ph];
          }
          md_replayer_set_chain((uint8_t)nc, k, ph, tr);
        }
        md_replayer_set_song(col, r, (uint8_t)nc);
      }
    } else if (page == PAGE_CHAIN) {
      for (int i = 0; i < MD_MAX_PHRASES; i++) mapPh[i] = -1;
      for (int r = l0; r <= l1; r++) {
        uint8_t ph; int8_t tr;
        md_replayer_get_chain((uint8_t)chainId, r, &ph, &tr);
        if (ph == MD_EMPTY) continue;
        if (mapPh[ph] < 0) {
          int np = phraseLibre();
          for (int pr = 0; pr < MD_ROWS_PER_PHRASE; pr++) {
            md_phrase_row_t rw;           // la ligne entiere, CC compris
            md_replayer_get_phrase_row(ph, pr, &rw);
            md_replayer_set_phrase_row((uint8_t)np, pr, &rw);
          }
          mapPh[ph] = np;
        }
        md_replayer_set_chain((uint8_t)chainId, r, (uint8_t)mapPh[ph], tr);
      }
    } else {
      for (int i = 0; i <= MD_MAX_INSTRUMENTS; i++) mapIn[i] = -1;
      for (int r = l0; r <= l1; r++) {
        uint8_t no,i2,ve,cm,cv,mc,mv;
        md_replayer_get_phrase((uint8_t)phraseId, r, &no,&i2,&ve,&cm,&cv,&mc,&mv);
        if (!i2) continue;
        if (mapIn[i2] < 0) {
          int ni = instrLibre();
          md_replayer_copy_instrument(i2, ni);
          mapIn[i2] = ni;
        }
        md_replayer_set_phrase((uint8_t)phraseId, r, no,(uint8_t)mapIn[i2],ve,cm,cv,mc,mv);
        instrId = mapIn[i2];
      }
    }
    selActive = false;
    modifie = true;
  };

  // ── Recopie UNE colonne d'une ligne de phrase ────────────────────────────
  // Les colonnes : 0 note, 1 instrument, 2 velocite, 3 lettre de commande,
  // 4 valeur de commande, puis 5 a 12 les quatre CC (numero, valeur).
  //
  // ⚠️ UN NUMERO DE CC VOYAGE AVEC SA VALEUR : les deux ne veulent rien dire
  // l'un sans l'autre, et le curseur ne peut etre que sur l'un des deux.
  auto phCopieColonne = [](md_phrase_row_t *dst, const md_phrase_row_t *src,
                           int col) {
    if (col < 0 || col > 12) return;
    if (col >= 5) {
      const int k = (col - 5) / 2;
      if (k == 0) { dst->mdcmd = src->mdcmd; dst->mdval = src->mdval; }
      else { dst->cc[k - 1] = src->cc[k - 1];
             dst->ccval[k - 1] = src->ccval[k - 1]; }
      return;
    }
    switch (col) {
      // La note emmene son instrument : une note sans instrument ne joue rien.
      case 0: dst->note = src->note; dst->instr = src->instr; break;
      case 1: dst->instr = src->instr; break;
      case 2: dst->vel = src->vel; break;
      default: dst->cmd = src->cmd; dst->cmdval = src->cmdval; break;
    }
  };

  auto coller = [&]() {
    if (clipPage != page || clipLignes <= 0) return;
    const int l0 = curL(), c0 = curC();
    const int n = clipLignes;

    // ── SUR SONG, ET SUR SONG SEULEMENT, LE COLLAGE INSERE ────────────────
    // Tout ce qui se trouve sous le curseur descend d'autant de lignes que le
    // presse-papier en contient, et ce qui deborde en bas est perdu : coller
    // une mesure ne doit pas effacer la suivante, elle doit la repousser.
    // TOUTES les colonnes descendent, pas seulement celles qu'on a copiees —
    // decaler une seule voie desynchroniserait le morceau.
    //
    // ⚠️ NULLE PART AILLEURS. Une chain et une phrase ont seize rangees fixes
    // qui font une mesure : y inserer decalerait le rythme au lieu de
    // remplacer ce qu'on vise. Le collage y ecrase.
    if (page == PAGE_SONG) {
      for (int c = 0; c < MD_MAX_CHANNELS; c++)
        for (int r = MD_SONG_ROWS - 1; r >= l0 + n; r--)
          md_replayer_set_song(c, r, md_replayer_get_song(c, r - n));
      for (int r = l0; r < l0 + n && r < MD_SONG_ROWS; r++)
        for (int c = 0; c < MD_MAX_CHANNELS; c++) md_replayer_set_song(c, r, MD_EMPTY);
      for (int l = 0; l < n; l++) {
        if (l0 + l >= MD_SONG_ROWS) break;
        for (int c = 0; c < clipCols; c++)
          if (c0 + c < nbVisibles)
            md_replayer_set_song(reel(c0 + c), l0 + l, clipSong[c][l]);
      }
    } else if (page == PAGE_CHAIN) {
      // ── ON NE COLLE QUE LES COLONNES COPIEES ──────────────────────────────
      // Copier la colonne TSP seule ne doit pas emporter les numeros de
      // phrase avec elle.
      for (int l = 0; l < n; l++) {
        if (l0 + l >= MD_ROWS_PER_CHAIN) break;
        uint8_t ph; int8_t tr;
        md_replayer_get_chain((uint8_t)chainId, l0 + l, &ph, &tr);
        for (int k = 0; k < clipCols; k++) {
          if (clipCol0 + k == 0) ph = clipChPh[l];
          else                   tr = clipChTr[l];
        }
        md_replayer_set_chain((uint8_t)chainId, l0 + l, ph, tr);
      }
    } else {
      // Meme regle sur PHRASE, et c'est la qu'elle compte le plus : une
      // colonne de CC copiee seule ne doit pas ecraser les notes, les
      // instruments ni les commandes de la phrase visee. Chaque colonne
      // revient dans LA SIENNE ; seule la ligne de depart suit le curseur.
      for (int l = 0; l < n; l++) {
        if (l0 + l >= MD_ROWS_PER_PHRASE) break;
        md_phrase_row_t dst;
        md_replayer_get_phrase_row((uint8_t)phraseId, l0 + l, &dst);
        for (int k = 0; k < clipCols; k++)
          phCopieColonne(&dst, &clipPhr[l], clipCol0 + k);
        md_replayer_set_phrase_row((uint8_t)phraseId, l0 + l, &dst);
      }
    }
    modifie = true;
  };

  // Le son n'a plus besoin d'etre entretenu : les voies de la DS jouent
  // seules. Il reste l'USB, que les longs repeints ne doivent pas affamer.
  auto entretienSon = [&]() { xt_midi_service(); };

  {
    char e[120];
    journal("");
    // La DATE DE COMPILATION dans l'en-tete : sans elle, impossible de savoir
    // si un journal vient de la derniere ROM ou d'une ancienne restee sur la
    // carte — ce qui a deja fait chercher un defaut la ou il n'y en avait pas.
    siprintf(e, "=== OPLBEND TRACKER  %s  USB MIDI %s  BUILD %s %s ===",
             modeDSi ? "DSI" : "DS", usbMidi ? "OUI" : "NON",
             __DATE__, __TIME__);
    journal(e);
    // Les dossiers retenus, pour qu'on n'ait plus a deviner ou un morceau est
    // parti — ni pourquoi il n'est pas la ou on l'attendait.
    if (__system_argv->argvMagic == ARGV_MAGIC && __system_argv->argc > 0 &&
        __system_argv->argv[0]) {
      char e2[190];
      siprintf(e2, "  CARTOUCHE %s", __system_argv->argv[0]);
      journal(e2);
    }
    { char e3[190];
      siprintf(e3, "  SONGS %s", dossierSongs);   journal(e3);
      siprintf(e3, "  SAMPLES %s", dossierSamples); journal(e3); }
  }

  // Le grand remplissage initial est derriere nous : on repart d'un compteur
  // propre, sinon il se lisait comme un premier tour de boucle interminable.
  tPrec = timerTick(2);

  while (true) {
    // L'USB d'abord : c'est lui qui dit a l'hote que la cartouche est la.
    entretienSon();
    // Un coup de kick en attente : on redemande tant que ses deux zones
    // sonnent. Ca tombe des que l'une se libere, sans jamais rien couper.
    if (kickARefaire && md_replayer_rend_kick(kickARefaire) != -2)
      kickARefaire = 0;
    // La note d'audition demandee a l'image precedente : la touche a eu le
    // temps de se relever, l'enveloppe du synthe repart donc pour de bon.
    if (auditionVoie >= 0) {
      md_replayer_play_test_note_vel(auditionNote, auditionIns, auditionVoie,
                                     auditionVel);
      auditionVoie = -1;
    }
    const unsigned tB = timerTick(2);
    cumul += (unsigned short)(tB - tPrec);   // soustraction 16 bits : le
    tPrec = tB;                              // bouclage du compteur est gere
    if (cumul >= kTicksParSeconde) {
      cumul -= kTicksParSeconde;
      // Etat vivant de chaque voie, juste apres un depart.
      if (journalVoies > 0) {
        journalVoies--;
        char l[420]; int n = 0;
        n += siprintf(l + n, "T%04u  VOIES:", (unsigned)secondes);
        for (int c = 0; c < MD_MAX_CHANNELS && n < 380; c++) {
          const int ch = md_replayer_play_chain(c);
          const int ph = md_replayer_play_phrase(c);
          const int ro = md_replayer_play_phrase_row(c);
          n += siprintf(l + n, " %s:c%d/p%d/l%d", noms[c], ch, ph, ro);
        }
        journal(l);
      }
      secondes++;
    }

    scanKeys();
    // Toute modification leve ce drapeau. Sans lui, la page SONG — qui ne se
    // redessine que sur deplacement du curseur — n'affichait pas ce qu'on
    // venait de poser tant qu'on n'avait pas bouge.
    modifie = false;
#ifdef FM1
    if (fm1_page_perdue()) pageVue = -1;           // (UBOOT annule : l'image etait au message)
    { static bool premiere = true;                 // on arrive en mode KEYS : on le dit
      if (premiere) { premiere = false; if (fm1_clavier()) fm1_annonce2("KEYBOARD", 0); } }
    // INSTR OPL3 : le champ de l'element sous le curseur devient celui que
    // l'edition d'avant connait (inLigne, et inCol pour l'operateur).
    bool instrSansChamp = false;                   // (la ligne OP, un preset : pas un champ)
    if (page == PAGE_INSTR && genreInstr(instrId) == GENRE_OPL) {
      ElInstr el[40];
      const int n = elementsInstr(inPage, el);
      inItem = borne(inItem, 0, n - 1);
      if (el[inItem].champ >= 0) {
        inLigne = el[inItem].champ;
        inCol = kChampsOp[inLigne].parOp ? inOp : 0;
      } else instrSansChamp = true;
    }
#endif
    int appui = keysDownRepeat();
    int frappe = keysDown();
    // Les fenetres ouvertes au debut de l'image : si l'une se ferme
    // pendant celle-ci, le A (ou le B) qui l'a fermee ne doit pas agir en
    // plus sur la page d'arrivee. Sans ca, ouvrir un morceau dans le
    // navigateur posait aussi un chain sur la ligne 00 de SONG.
    const bool fenetreAuDebut = navigateur || dialogueNom || demandeNouveau || dialogueMidi;
#ifdef FM1
    // ── OCT- / OCT+ ────────────────────────────────────────────────────────
    // En lecture comme a l'arret : l'octave de ce qui est sous le curseur.
    //   PHRASE : la note de la ligne du curseur (sans note : rien) ;
    //   CHAIN  : la transposition de la ligne du curseur, de 12 ;
    //   SONG   : celle de toutes les lignes du chain sous le curseur.
    // (La transposition plutot que les notes : une phrase peut servir dans
    // plusieurs chains.) Si une note sortait de C-0 … B-8, rien ne bouge. Et
    // le clavier suit : il joue une octave plus haut ou plus bas (pianoOct).
#ifdef FM1
    // LOOPER (sur toute page) : OCT+ / OCT- montent ou descendent TOUTES les notes de la boucle d'une octave
    // (rien ne bouge si une note sortait de C-0 .. B-8, comme sur SONG).
    if (fm1_looper()) {
      if (const int oct = fm1_octave_pas()) {
        const int d = oct > 0 ? 12 : -12;
        bool tient = g_loopN > 0;
        for (int i = 0; i < g_loopN && tient; i++)
          if (g_loop[i].on != 2 && (g_loop[i].note + d < 1 || g_loop[i].note + d > MD_MAX_NOTE)) tient = false;
        if (tient) {
          for (int i = 0; i < g_loopN; i++) g_loop[i].note = (uint8_t)(g_loop[i].note + d);
          loopDerNote = borne(loopDerNote + d, 1, MD_MAX_NOTE);
          g_loopOct += oct > 0 ? 1 : -1;
          char l2[12];
          siprintf(l2, "OCT %+d", g_loopOct);         // (ou l'on en est, depuis la boucle enregistree)
          fm1_annonce2("LOOP", l2);
          modifie = true;
        } else fm1_annonce2("LOOP", g_loopN ? "OUT OF RANGE" : "EMPTY");
      }
    } else
    // DRUM MACHINE : OCT+ / OCT- montent ou descendent le son choisi d'une octave.
    if (fm1_drum()) {
      if (const int oct = fm1_octave_pas()) {
        md_module_t *m = md_replayer_get_module();
        if (m) {
          // TOUTE la boite, sur toute page : chaque son d'une octave (chacun garde son ecart avec les autres)
          for (int k = 0; k < 11; k++)
            m->drum_trans[k] = (int8_t)borne(m->drum_trans[k] + (oct > 0 ? 12 : -12), -36, 36);
          drumOctTout = borne(drumOctTout + (oct > 0 ? 1 : -1), -3, 3);
          char l2[12];
          siprintf(l2, "OCT %+d", drumOctTout);     // (ou l'on en est, depuis le depart)
          fm1_annonce2("ALL DRUMS", l2);
        }
      }
    } else
#endif
    if (const int oct = fm1_octave_pas()) {
      const int d = oct > 0 ? 12 : -12;
      static const char *kGam[12] = {"C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"};
      char m[40] = "";
      int octRel = 0; bool aOctRel = false;        // (la transposition du chain, en octaves, depuis 0)
      // Les notes d'une phrase, transposees de tsp, restent-elles dans C-0 … B-8 ?
      auto tient = [&](int ph, int tsp) {
        if (ph == MD_EMPTY) return true;
        if (tsp < -128 || tsp > 127) return false;
        for (int l = 0; l < MD_ROWS_PER_PHRASE; l++) {
          uint8_t no, i2, ve, cm, cv, mc, mv;
          md_replayer_get_phrase((uint8_t)ph, l, &no,&i2,&ve,&cm,&cv,&mc,&mv);
          if (no && no != MD_EMPTY && (no + tsp < 1 || no + tsp > MD_MAX_NOTE)) return false;
        }
        return true;
      };
      if (page == PAGE_PHRASE) {
        uint8_t no, i2, ve, cm, cv, mc, mv;
        md_replayer_get_phrase((uint8_t)phraseId, phLigne, &no,&i2,&ve,&cm,&cv,&mc,&mv);
        if (no && no != MD_EMPTY) {
          if (no + d >= 1 && no + d <= MD_MAX_NOTE) {
            md_replayer_set_phrase((uint8_t)phraseId, phLigne, (uint8_t)(no + d), i2, ve, cm, cv, mc, mv);
            snprintf(m, sizeof m, "NOTE %s%d", kGam[(no + d - 1) % 12], (no + d - 1) / 12);
            modifie = true;
          } else strcpy(m, "OCTAVE LIMIT");
        }
      } else if (page == PAGE_CHAIN) {
        uint8_t ph; int8_t tsp;
        md_replayer_get_chain((uint8_t)chainId, chLigne, &ph, &tsp);
        if (ph != MD_EMPTY) {
          if (tient(ph, tsp + d)) {
            md_replayer_set_chain((uint8_t)chainId, chLigne, ph, (int8_t)(tsp + d));
            snprintf(m, sizeof m, "TSP %02X", (uint8_t)(tsp + d));
            octRel = (tsp + d) / 12; aOctRel = true;
            modifie = true;
          } else strcpy(m, "OCTAVE LIMIT");
        }
      } else if (page == PAGE_SONG && curCanal < nbVisibles) {
        const uint8_t ch = md_replayer_get_song(reel(curCanal), curLigne);
        bool ok = true, un = false;
        for (int r = 0; ch != MD_EMPTY && r < MD_ROWS_PER_CHAIN; r++) {
          uint8_t ph; int8_t tsp;
          md_replayer_get_chain(ch, r, &ph, &tsp);
          if (ph != MD_EMPTY) { un = true; if (!tient(ph, tsp + d)) ok = false; }
        }
        if (un && ok) {
          for (int r = 0; r < MD_ROWS_PER_CHAIN; r++) {
            uint8_t ph; int8_t tsp;
            md_replayer_get_chain(ch, r, &ph, &tsp);
            if (ph != MD_EMPTY) md_replayer_set_chain(ch, r, ph, (int8_t)(tsp + d));
          }
          snprintf(m, sizeof m, "CHAIN %02X OCT %c", ch, d > 0 ? '+' : '-');
          { uint8_t ph; int8_t tsp; md_replayer_get_chain(ch, 0, &ph, &tsp); octRel = tsp / 12; aOctRel = true; }
          modifie = true;
        } else if (un) strcpy(m, "OCTAVE LIMIT");
      }
      pianoOct = borne(pianoOct + d, -36, 36);
#ifdef FM1
      { // En gros : ce qui a bouge, et OU L'ON EN EST par rapport au depart
        // (le chain : sa transposition en octaves ; sinon, le clavier).
        char l1[20], l2[16];
        if (!m[0] || !strcmp(m, "OCTAVE LIMIT")) strcpy(l1, m[0] ? m : "KEYBOARD");
        else if (!strncmp(m, "CHAIN", 5)) { strncpy(l1, m, 8); l1[8] = 0; }
        else { strncpy(l1, m, sizeof l1 - 1); l1[sizeof l1 - 1] = 0; }
        siprintf(l2, "OCT %+d", aOctRel ? octRel : pianoOct / 12);
        fm1_annonce2(l1, l2);
      }
#endif
      const size_t lg = strlen(m);
      snprintf(m + lg, sizeof m - lg, "%sKEYBOARD %+d", lg ? " " : "", pianoOct / 12);
      strncpy(msgProjet, m, sizeof msgProjet - 1); msgProjet[sizeof msgProjet - 1] = 0;
      msgProjetJusqu = secondes + 2;
    }

    // ── ZOOM et suivi de la lecture ────────────────────────────────────────
    // (pas sur FX : sa legende ne tiendrait pas)
    if (enLecture && !(keysHeld() & KEY_SELECT) &&
        ((appui & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) || (frappe & KEY_A)))
      suiviLecture = false;                       // on a touche au curseur : la vue le suit, lui
    {
      static int zoomVuPage = 0;
      const int z = zoomNiv;
      if (z != zoomVuPage) { zoomVuPage = z; pageVue = -1; }   // tout se redessine a la nouvelle taille
      fm1_zoom(z);
      kVoiesEcran = zoomNiv == 2 ? 3 : zoomNiv ? 4 : 5;   // SONG
      kPasVoie = zoomNiv ? 3 : 4;
      // PHRASE : le premier groupe montre, pour que celui du curseur tienne.
      const int avant = phGauche;
      if (z && page == PAGE_PHRASE) {
        int g = phGroupe(phCol);
        if (g < 0) g = 0;
        if (g < phGauche) phGauche = g;
        while (phGauche < g && kPhGrpDeb[g] - phDecal(phGauche) + kPhGrpLarg[g] - 1 > phDerniere()) phGauche++;
      } else if (!z) phGauche = 0;
      if (phGauche != avant && page == PAGE_PHRASE) pageVue = -1;   // les colonnes changent : on repeint
    }
    fm1_suivre_lecture(enLecture && suiviLecture);


    // ── L'instrument que designe le curseur (SONG, CHAIN, PHRASE) ──────────
    // Comme le tracker DOS : l'instrument OPL3 le PLUS EMPLOYE de ce que
    // designe le curseur, le chain sur SONG, la phrase sur CHAIN et PHRASE.
    // Pendant la lecture, ce qui JOUE sur la colonne du curseur (la ou sont
    // les chevrons) : le chain de la piste du curseur sur SONG, la phrase de
    // la voie ouverte sur CHAIN et PHRASE. 0 : aucun.
    auto instrCible = [&]() {
      static int n[MD_MAX_INSTRUMENTS + 1];
      memset(n, 0, sizeof n);
      // L'instrument le plus employe d'une phrase (compte ajoute a n[]). Une
      // ligne sans instrument garde celui de la ligne d'avant.
      auto comptePhrase = [&](int ph) {
        if (ph < 0 || ph == MD_EMPTY) return;
        int courant = 0;
        for (int r = 0; r < MD_ROWS_PER_PHRASE; r++) {
          md_phrase_row_t pr;
          md_replayer_get_phrase_row((uint8_t)ph, r, &pr);
          if (pr.instr) courant = pr.instr;
          const bool note = pr.note >= 1 && pr.note <= MD_MAX_NOTE;
          if (courant && courant <= MD_MAX_INSTRUMENTS && (note || pr.instr) && genreInstr(courant) == GENRE_OPL)
            n[courant]++;
        }
      };
      if (page == PAGE_SONG || page == PAGE_LOOPER) {   // (LOOPER : le clavier de SONG)
        if (curCanal >= nbVisibles) return 0;
        const int voie = reel(curCanal), joue = enLecture ? md_replayer_play_chain(voie) : -1;
        const uint8_t ch = (joue >= 0 && joue != MD_EMPTY) ? (uint8_t)joue : md_replayer_get_song(voie, curLigne);
        for (int r = 0; ch != MD_EMPTY && r < MD_ROWS_PER_CHAIN; r++) {
          uint8_t ph; int8_t tsp;
          md_replayer_get_chain(ch, r, &ph, &tsp);
          comptePhrase(ph);
        }
      } else if (page == PAGE_CHAIN || page == PAGE_PHRASE) {
        const int joue = enLecture ? md_replayer_play_phrase(voieCourante) : -1;
        if (joue >= 0 && joue != MD_EMPTY) comptePhrase(joue);
        else if (page == PAGE_CHAIN) {
          uint8_t ph; int8_t tsp;
          md_replayer_get_chain((uint8_t)chainId, chLigne, &ph, &tsp);
          comptePhrase(ph);
        } else comptePhrase(phraseId);
      } else if (page == PAGE_INSTR) {
        return genreInstr(instrId) == GENRE_OPL ? instrId : 0;   // celui de la page
      } else if (page == PAGE_PRESET) {
        return genreInstr(presetIns) == GENRE_OPL ? presetIns : 0;   // celui que la page regle
      } else if (page == PAGE_DRUM) {
        return kDrumIns0 + drumSel;                // le son choisi de la DRUM MACHINE
      } else return 0;
      int ins = 0;
      for (int i = 1; i <= MD_MAX_INSTRUMENTS; i++) if (n[i] > n[ins]) ins = i;
      if (n[ins] > 0) return ins;
      // SONG : le chain sous le curseur ne dit rien (vide, ou ses notes sans instrument : un .A2M importe ne
      // l'ecrit souvent qu'a la premiere note de la piste) : celui que la piste jouait juste avant, sinon apres
      if ((page == PAGE_SONG || page == PAGE_LOOPER) && curCanal < nbVisibles) {
        const int voie = reel(curCanal);
        auto dernierDuChain = [&](uint8_t ch) {
          int dernier = 0;
          for (int r = 0; ch != MD_EMPTY && r < MD_ROWS_PER_CHAIN; r++) {
            uint8_t ph; int8_t tsp;
            md_replayer_get_chain(ch, r, &ph, &tsp);
            for (int l = 0; ph != MD_EMPTY && l < MD_ROWS_PER_PHRASE; l++) {
              md_phrase_row_t pr;
              md_replayer_get_phrase_row(ph, l, &pr);
              if (pr.instr && pr.instr <= MD_MAX_INSTRUMENTS && genreInstr(pr.instr) == GENRE_OPL) dernier = pr.instr;
            }
          }
          return dernier;
        };
        for (int l = curLigne - 1; l >= 0; l--)
          if (const int i2 = dernierDuChain(md_replayer_get_song(voie, l))) return i2;
        for (int l = curLigne + 1; l < MD_SONG_ROWS; l++)
          if (const int i2 = dernierDuChain(md_replayer_get_song(voie, l))) return i2;
      }
      return 0;
    };

    // ── HOME : retour a SONG, depuis n'importe quelle page ─────────────────
    if (fm1_home_appuis() && !navigateur && !dialogueNom && !dialogueMidi && page != PAGE_SONG) {
      demandeNouveau = false;                      // (une question en suspens : « non »)
      page = PAGE_SONG; pageVue = -1;
    }

    // ── SAVE : la sauvegarde, comme SAVE dans PROJECT ──────────────────────
    // La fenetre du nom s'ouvre (le curseur sur OK : deux appuis pour
    // reenregistrer) ; fermee, on revient a la page d'ou l'on venait.
    if (fm1_save_appuis() && !navigateur && !dialogueNom && !dialogueMidi) {
      saveAvant = page; saveRetour = true;
      page = PAGE_PROJECT; menuProjet = 1;
      strncpy(sauveNom, nomProjet, 8); sauveNom[8] = 0;
      saveEnA2m = ouvertA2m && fm1_morceau_tient_en_a2m();
      dialogueNom = true; nomLig = 3; nomCol = 9;
      assombrirDemande = true;
      pageVue = -1;
    }
    if (saveRetour && !dialogueNom) { saveRetour = false; page = saveAvant; pageVue = -1; }
    fm1_clavier_suspendu(dialogueNom || navigateur || dialogueMidi);

    // ── SEQ, troisieme mode : la DRUM MACHINE ──────────────────────────────
    // On y arrive (sa page), on en repart (la page d'avant). Elle continue de
    // jouer quand on la quitte : le tracker peut alors partir avec elle.
    if (fm1_drum() != drumMode) {
      drumMode = fm1_drum();
      if (drumMode) { drumPrepare(); drumAvant = page == PAGE_DRUM ? PAGE_SONG : page; page = PAGE_DRUM; pageVue = -1; }
      else if (page == PAGE_DRUM) { page = drumAvant; pageVue = -1; }
    }
    // GLO vers DRUM KEYBOARD : ses sons
    {
      static bool dk = false;
      if (fm1_drumkeys() != dk) { dk = fm1_drumkeys(); if (dk) dkPrepare(); }
    }
    // Un morceau vient d'etre charge : les sons de la boucle reviennent (plus haut, g_loopGarde)
    if (g_loopARemettre && !navigateur) {
      g_loopARemettre = false;
      bool batteries = fm1_drumkeys();
      for (int k = 0; k < g_loopNbGarde; k++) {
        md_replayer_init_default_instrument(kLoopIns0 + k - 1);
        for (int o = 0; o < MD_OPL_OCTETS; o++) md_replayer_set_instr_opl(kLoopIns0 + k, o, g_loopGardeOpl[k][o]);
        char nm[12]; siprintf(nm, "LOOP %d", k + 1);
        md_replayer_set_instr_name(kLoopIns0 + k, nm);
      }
      for (int i = 0; i < g_loopN; i++) {
        const int ii = (int)fm1_opaque(g_loop[i].ins);
        if (ii >= kDkIns0 && ii < kDkIns0 + 27) { batteries = true; continue; }
        for (int k = 0; k < g_loopNbGarde; k++)
          if (g_loopGardeIns[k] == ii) { g_loop[(int)fm1_opaque((uint32_t)i)].ins = (uint8_t)(kLoopIns0 + k); break; }
      }
      if (batteries) dkPrepare();
    }
    // SEQ vers LOOPER : sa page
    {
      static bool looperMode = false;
      if (fm1_looper() != looperMode) {
        looperMode = fm1_looper();
        if (looperMode) { page = PAGE_LOOPER; pageVue = -1; }
      }
    }
    // SEQ vers TRACKER, depuis une page qui n'est pas le tracker (FX, DRUM, HELP, PRESET, PROJECT) : on revient
    // sur SONG (sur SONG, CHAIN, PHRASE, INSTR, TABLE, on reste ou l'on edite). GLO (KEYBOARD / D-PAD) : on reste.
    {
      static int modeAvant = -1;
      const int mode = fm1_drum() ? 1 : fm1_looper() ? 2 : 0;
      if (modeAvant >= 0 && mode != modeAvant && mode == 0 && page != PAGE_SONG && page != PAGE_CHAIN &&
          page != PAGE_PHRASE && page != PAGE_INSTR && page != PAGE_TABLE && !navigateur && !dialogueNom) {
        page = PAGE_SONG; pageVue = -1;
      }
      modeAvant = mode;
    }
    { const double bpm = md_replayer_get_bpm();
      const int rpb = md_replayer_get_rows_per_beat() > 0 ? md_replayer_get_rows_per_beat() : 4;
      g_drumPeriode = (uint32_t)(fm1_echantillons_par_seconde() * 60.0 / ((bpm > 1.0 ? bpm : 125.0) * rpb)); }

    // ── FX : la page CIRCUIT BENDING ───────────────────────────────────────
    // FX l'ouvre, un second appui ramene d'ou l'on venait. La LED de FX reste
    // allumee tant qu'un switch est mis (l'effet continue hors de la page).
    // ── ENV : l'aide (HELP) ────────────────────────────────────────────────
    // ENV l'ouvre (a la premiere page), puis passe a la suivante ; apres la
    // derniere, il ramene d'ou l'on venait (aideFin : le dessin sait combien il
    // y en a, ca depend du ZOOM). HOME, FX, SEL... en sortent aussi.
    if (fm1_env_appuis() && !navigateur && !dialogueNom) {
      if (page != PAGE_AIDE) { aideAvant = page; page = PAGE_AIDE; aidePage = 0; pageVue = -1; }
      else if (aideFin) { page = aideAvant; pageVue = -1; }
      else aidePage++;
    }
    if (page != PAGE_AIDE) fm1_aide_clavier(0);
    fm1_vue_fixe(page == PAGE_DRUM || page == PAGE_AIDE || page == PAGE_LOOPER);
    fm1_led_clignote(page == PAGE_LOOPER && !fm1_clavier() && loopCurL == kLoopVoies + 1 && g_loopBtnC == 1 &&
                     g_loopEtat >= 2 && g_loopEtat <= 4
                     ? (1u << 8) | (1u << 10) | (1u << 13) | (1u << 15) | (1u << 17) | (1u << 20) : 0u);

    if (fm1_fx_appuis() && !navigateur && !dialogueNom) {
      if (page == PAGE_BEND) { page = PAGE_BEND2; pageVue = -1; }   // FX encore : CIRCUIT BENDING 2
      else if (page == PAGE_BEND2) {               // ... puis la page GLITCH
        page = PAGE_GLITCH; pageVue = -1;
        if (!glitchVu) {                           // (la premiere fois : un motif, pret ; il agit quand KNOB 1 l'allume)
          glitchVu = true;
          for (int k = 0; k < 16; k++) glitchPas[k] = kGlitchDefaut[k];
          glitchMaj();
        }
      }
      else if (page == PAGE_GLITCH) { page = PAGE_GLOBAL; pageVue = -1; }   // FX encore : KNOB GENERAL
      else if (page == PAGE_GLOBAL) { page = PAGE_MASTER; pageVue = -1; }   // ... puis MASTER FX
      // (puis d'ou l'on venait ; mais pas le LOOPER ni la DRUM MACHINE, les pages de SEQ : on reboucle)
      else if (page == PAGE_MASTER) { page = bendAvant == PAGE_LOOPER || bendAvant == PAGE_DRUM ? PAGE_BEND : bendAvant; pageVue = -1; }
      else if (page != PAGE_BEND && page != PAGE_BEND2) {
        // (le clavier, sur la page : la piste du curseur de SONG, ou celle ou l'on est descendu)
        bendVoieClavier = (page == PAGE_SONG || page == PAGE_LOOPER) && curCanal < nbVisibles ? reel(curCanal) : voieCourante;
        bendAvant = page; page = PAGE_BEND; pageVue = -1; bendRelis = true;
      }
    }

    // ── SEL : la page PRESET ───────────────────────────────────────────────
    // SEL ouvre la page PRESET pour l'instrument que designe le curseur (celui
    // de la page sur INSTR), et un second appui ramene d'ou l'on venait.
    if (fm1_sel_appuis() && !navigateur && !dialogueNom) {
      if (page != PAGE_PRESET) {
        int ins = page == PAGE_INSTR ? instrId : instrCible();
        if (ins <= 0) ins = instrId > 0 ? instrId : 1;
        presetIns = ins;
        presetVoie = page == PAGE_SONG ? reel(curCanal) : page == PAGE_INSTR ? instrVoie : voieCourante;
        presetAvant = page;
        page = PAGE_PRESET; pageVue = -1;
      } else {
        page = presetAvant; pageVue = -1;
      }
    }

    // ── Les molettes sur SONG, CHAIN, PHRASE, INSTR ────────────────────────
    // Comme les touches F du tracker DOS, sur l'instrument que designe le
    // curseur (instrCible) : KNOB 1 la sortie du modulateur, 2 celle de la
    // porteuse, 3 le feedback, 4 le multiplicateur du modulateur ; PRESETS la
    // forme d'onde de l'OP1 (en mode tracker : en mode KEYS, il deplace le
    // curseur), ALGORITHM celle de l'OP2. Ailleurs, elles ne font rien.
    {
      const bool ici = (page == PAGE_SONG || page == PAGE_LOOPER || page == PAGE_CHAIN || page == PAGE_PHRASE ||
                        page == PAGE_INSTR || page == PAGE_PRESET || page == PAGE_BEND || page == PAGE_BEND2 || page == PAGE_GLITCH ||
                        page == PAGE_DRUM || page == PAGE_MASTER || page == PAGE_GLOBAL) &&
                       !navigateur && !dialogueNom;
      fm1_molettes_instr(ici);
      fm1_page_presets(page == PAGE_PRESET || page == PAGE_GLOBAL);   // (PRESETS : la forme d'onde, pas la piste)
      fm1_page_bend(page == PAGE_BEND || page == PAGE_BEND2 || page == PAGE_MASTER || page == PAGE_GLOBAL || page == PAGE_LOOPER);   // (SELECT : pas le tempo)
      fm1_select_preset(page == PAGE_PRESET);
      int pas[7], aucun = 1;
      for (int k = 0; k < 7; k++) { pas[k] = ici ? fm1_fx_pas(k) : 0; if (pas[k]) aucun = 0; }
      // La page CIRCUIT BENDING : sept effets. Haut / bas choisissent l'effet,
      // gauche / droite la colonne (le curseur rouge) : sur 0 / 1, A + droite
      // le met, A + gauche l'enleve ; sur une piste, A + droite l'y met (en
      // vert, et l'effet avec : on l'entend tout de suite), A + gauche l'en
      // enleve. KNOB 1..4 font aussi les quatre premiers (horaire : mis,
      // l'autre sens : enleve), SELECT le cinquieme (ici, pas le tempo),
      // PRESETS et ALGORITHM les deux derniers.
      // La page GLITCH : KNOB 1 l'allume (horaire) ou l'eteint ; KNOB 2 en
      // arriere, le motif d'avant, en avant le suivant (ceux qu'on avait
      // quittes, puis un neuf au hasard). La croix choisit un pas, A + droite /
      // gauche change son effet. SELECT reste le tempo.
      if (page == PAGE_GLITCH) {
        int sel = glitchSel, d = 0;
        auto retiens = [&]() {                     // le motif courant, dans l'histoire
          if (glitchHistPos < glitchHistN) glitchHistN = glitchHistPos;   // (ce qu'on avait defait s'oublie)
          if (glitchHistN == 16) { memmove(glitchHist[0], glitchHist[1], 15 * 16); glitchHistN = 15; }
          for (int k = 0; k < 16; k++) glitchHist[glitchHistN][k] = (uint8_t)glitchPas[k];
          glitchHistPos = ++glitchHistN;
        };
        if (pas[0] && glitchOn != (pas[0] > 0)) {   // KNOB 1 : allume ou eteint
          glitchOn = pas[0] > 0;
          glitchMaj();
          fm1_annonce2("GLITCH", glitchOn ? "ON" : "OFF");
          if (glitchOn) fm1_gif(-2);                 // (une au hasard)
        }
        if (pas[1] > 0 && glitchHistPos + 1 < glitchHistN) {   // KNOB 2 en avant : celui qu'on avait quitte
          glitchHistPos++;
          for (int k = 0; k < 16; k++) glitchPas[k] = glitchHist[glitchHistPos][k];
          glitchMaj();
          fm1_annonce2("GLITCH", "NEXT");
        } else if (pas[1] > 0) {                   // ... sinon un neuf, au hasard (un pas sur trois environ)
          retiens();
          glitchHasard ^= fm1_echantillons();
          for (int k = 0; k < 16; k++) {
            glitchHasard = glitchHasard * 1103515245u + 12345u;
            const uint32_t r = glitchHasard >> 16;
            glitchPas[k] = (r % 100u) < 35u ? 1 + (int)((r >> 7) % 8u) : 0;
          }
          glitchMaj();
          fm1_annonce2("GLITCH", "RANDOM");
          { static int n = 0; if (++n % 4 == 0) fm1_gif(-2); }   // (de temps en temps, une au hasard)
        } else if (pas[1] < 0 && glitchHistPos > 0) {   // KNOB 2 en arriere : le motif d'avant
          if (glitchHistPos == glitchHistN) { retiens(); glitchHistPos--; }   // (le courant, pour y revenir)
          glitchHistPos--;
          for (int k = 0; k < 16; k++) glitchPas[k] = glitchHist[glitchHistPos][k];
          glitchMaj();
          fm1_annonce2("GLITCH", "PREVIOUS");
        }
        if (!fm1_clavier() && !(keysHeld() & KEY_SELECT)) {
          const bool aTenu = keysHeld() & KEY_A;
          // (fm1_opaque : sans lui, « gauche : -1 » devient un sextra, que
          // l'emulateur en ligne lit sans signe)
          const int g = (int)fm1_opaque((appui & KEY_LEFT) ? 1u : 0u), dr = (int)fm1_opaque((appui & KEY_RIGHT) ? 1u : 0u),
                    h = (int)fm1_opaque((appui & KEY_UP) ? 1u : 0u), b = (int)fm1_opaque((appui & KEY_DOWN) ? 1u : 0u);
          if (!aTenu) sel += b - h;                // (la liste : haut / bas, un pas)
          else d += dr - g;
        }
        glitchSel = borne(sel, 0, 15);
        if (d) {
          glitchPas[glitchSel] = borne(glitchPas[glitchSel] + (d > 0 ? 1 : -1), 0, 8);
          glitchMaj();
          char l1[12];
          siprintf(l1, "STEP %d", glitchSel + 1);
          fm1_annonce2(l1, kGlitchNom[glitchPas[glitchSel]]);
        }
        for (int k = 0; k < 7; k++) pas[k] = 0;
        aucun = 1;
      }
      // La page MASTER FX : la DOSE de chaque effet, 0 (coupe) a F. KNOB 1..4,
      // SELECT, PRESETS, ALGORITHM, un effet chacun (cran par cran) ; haut /
      // bas choisissent, A + droite / gauche montent ou descendent.
      if (page == PAGE_MASTER) {
        int d[7] = { pas[0], pas[1], pas[2], pas[3], pas[6], pas[4], pas[5] };
        if (!fm1_clavier() && !(keysHeld() & KEY_SELECT)) {
          const bool aTenu = keysHeld() & KEY_A;
          const int h = (int)fm1_opaque((appui & KEY_UP) ? 1u : 0u), b = (int)fm1_opaque((appui & KEY_DOWN) ? 1u : 0u);
          const int g = (int)fm1_opaque((appui & KEY_LEFT) ? 1u : 0u), dr = (int)fm1_opaque((appui & KEY_RIGHT) ? 1u : 0u);
          if (!aTenu) mfxSel = borne(mfxSel + b - h, 0, 6);
          else d[mfxSel] += dr - g;
        }
        for (int k = 0; k < 7; k++) {
          if (!d[k]) continue;
          const int avant = mfxDose[k];
          mfxDose[k] = borne(mfxDose[k] + d[k], 0, 15);
          if (mfxDose[k] != avant) {
            mfxSel = k;
            mfxMaj();
            char l2[8];
            siprintf(l2, mfxDose[k] ? "%X" : "OFF", mfxDose[k]);
            fm1_annonce2(kMfxNom[k], l2);
            if (mfxDose[k] == 15) fm1_gif(-2);       // (pousse a fond : une animation, au hasard)
          }
        }
        for (int k = 0; k < 7; k++) pas[k] = 0;
        aucun = 1;
      }
      // La DRUM MACHINE en D-PAD (SELECT + bas depuis SONG, CHAIN, PHRASE...) : la croix deplace le curseur,
      // haut / bas de son en son, gauche / droite de pas en pas ; A met ou enleve la croix.
      if (page == PAGE_DRUM && !fm1_clavier() && !(keysHeld() & KEY_SELECT)) {   // (D-PAD, en TRACKER comme en DRUM MACHINE)
        md_module_t *m = md_replayer_get_module();
        const int g = (int)fm1_opaque((appui & KEY_LEFT) ? 1u : 0u), dr = (int)fm1_opaque((appui & KEY_RIGHT) ? 1u : 0u),
                  h = (int)fm1_opaque((appui & KEY_UP) ? 1u : 0u), b = (int)fm1_opaque((appui & KEY_DOWN) ? 1u : 0u);
        drumSel = borne(drumSel + b - h, 0, 10);
        drumCurPas = borne(drumCurPas + dr - g, 0, 15);
        if ((frappe & KEY_A) && m) {
          m->drum_pas[drumSel] ^= (uint16_t)(1u << drumCurPas);
          // une croix posee, rien ne joue : on entend son son (comme sa touche noire)
          if (((m->drum_pas[drumSel] >> drumCurPas) & 1u) && !g_drumJoue && !enLecture) {
            for (int v = 12; v < 18; v++)
              if ((g_drumSonne >> v) & 1u) md_replayer_stop_test_note(v);
            g_drumSonne = 1u << kDrumVoie[drumSel];
            md_replayer_play_test_note(drumNote(m, drumSel), kDrumIns0 + drumSel, kDrumVoie[drumSel]);
          }
          modifie = true;
        }
        frappe &= ~(KEY_A | KEY_B);               // (rien d'autre ne doit les recevoir ici)
      }
      // CIRCUIT BENDING 2 : K1 le pitch, K2 l'accordage fin (le reste ne fait rien ici)
      if (page == PAGE_BEND2) {
        if (pas[0] || pas[1] || pas[2]) {
          bendPitch = borne(bendPitch + pas[0] * 20, -2400, 2400);
          bendFin = borne(bendFin + pas[1], -100, 100);
          bendHorloge = borne(bendHorloge + pas[2] * 20, -2400, 2400);
          horlogeMaj();
          char l2[16];
          const int c = pas[0] ? bendPitch : bendHorloge;
          if (pas[0] || pas[2]) siprintf(l2, "%c%d.%02d", c < 0 ? '-' : '+', abs(c) / 100, abs(c) % 100);
          else siprintf(l2, "%+d", bendFin);
          fm1_annonce2(pas[0] ? "PITCH" : pas[2] ? "CLOCK PITCH" : "FINETUNE", l2);
        }
        for (int k = 0; k < 7; k++) pas[k] = 0;
        aucun = 1;
      }
      // LOOPER : SELECT (pas le tempo : la boucle a sa duree) change le preset du son du clavier
      if (page == PAGE_LOOPER && pas[6]) {
        static int loopPreset = 0;
        const int ins = instrCible();
        loopPreset = borne(loopPreset + pas[6], 0, FM1_NB_PRESETS - 1);
        if (ins) {
          appliquePreset(ins, loopPreset);
          char l2[20];
          snprintf(l2, sizeof l2, "%d/%d", loopPreset + 1, FM1_NB_PRESETS);
          fm1_annonce2(FM1_PRESET_NOMS[loopPreset], l2);
        }
        pas[6] = 0;
        aucun = 1;
        for (int k = 0; k < 6; k++) if (pas[k]) aucun = 0;
      }
      // Le LOOPER en D-PAD : la croix deplace le curseur sur la grille (une rangee par couche, seize pas de la
      // boucle) ; comme dans PHRASE, A pose la derniere note, A+B l'enleve, A + gauche / droite la change d'un
      // demi-ton, A + haut / bas d'une octave. Sur le numero d'une couche : A+B l'enleve (ses notes gardees),
      // A la remet.
      if (page == PAGE_LOOPER && !fm1_clavier() && !(keysHeld() & KEY_SELECT)) {
        const bool aTenu = keysHeld() & KEY_A, bTenu = keysHeld() & KEY_B;
        const int g = (int)fm1_opaque((appui & KEY_LEFT) ? 1u : 0u), dr = (int)fm1_opaque((appui & KEY_RIGHT) ? 1u : 0u),
                  h = (int)fm1_opaque((appui & KEY_UP) ? 1u : 0u), b = (int)fm1_opaque((appui & KEY_DOWN) ? 1u : 0u);
        const uint32_t lg = g_loopLg;
        const bool boucle = g_loopEtat >= 2 && g_loopEtat <= 4;   // (une boucle fermee : sa duree est connue)
        const int couche = loopCurL;
        if (loopCurL < kLoopVoies) loopLigneVue = loopCurL;   // (STP/CUR regle celle-ci)
        if ((g | dr | h | b) && loopCurL < kLoopVoies) loopCibleL = loopCurL;   // (le curseur bouge : les molettes le suivent)
        // la note posee dans le pas j de la couche (son debut dans le pas), -1 : aucune
        auto noteDuPas = [&](int c, int j) {
          for (int i = 0; i < g_loopN; i++)
            if (g_loop[i].on == 1 && g_loop[i].couche % kLoopVoies == c && loopPasDe(g_loop[i].t, c) / loopParCase() == j) return i;
          return -1;
        };
        const int ici = boucle && loopCurC > 0 ? noteDuPas(couche, loopCurC - 1) : -1;
        static int loopBtnC = 0;                       // (sur les boutons : 0 a gauche RM / CLR, 1 a droite STEP)
        if (!aTenu) {                                  // (sous les couches : RM LAST, CLR ALL ; STEP, STP/CUR)
          loopCurL = borne(loopCurL + b - h, 0, kLoopVoies + 1);
          if (loopCurL < kLoopVoies) loopCurC = borne(loopCurC + dr - g, 0, 16);
          else loopBtnC = borne(loopBtnC + dr - g, 0, 1);
        }
        g_loopBtnC = loopBtnC;
        // (A sur une croix : elle s'enleve au RELACHEMENT de A, si aucune fleche n'a servi entre-temps ; sinon
        // A + croix la change)
        static int aEnleve = -1;
        if (aTenu && (g | dr | h | b)) aEnleve = -1;
        if ((keysUp() & KEY_A) && aEnleve >= 0 && aEnleve < g_loopN && g_loop[aEnleve].on == 1) {
          g_loop[aEnleve].on = 2;
          loopFin(aEnleve, true);
          md_replayer_stop_test_note(kLoopVoie0 + g_loop[aEnleve].couche % kLoopVoies);
          modifie = true;
        }
        if (!aTenu) aEnleve = -1;
        if (loopCurL >= kLoopVoies) {                  // un bouton : A le presse (STEP : A + gauche / droite)
          if (loopBtnC == 0 && (frappe & KEY_A) && !bTenu) {
            if (loopCurL == kLoopVoies) fm1_annonce2("LOOPER", loopEnleveDerniere() ? "REMOVED" : "EMPTY");
            else { loopEfface(); fm1_annonce2("LOOPER", "ERASED"); }
            modifie = true;
          } else if (loopBtnC == 1 && aTenu && (g | dr) && boucle) {
            char l2[20];
            if (loopCurL == kLoopVoies) {              // STEP : toute la boucle
              g_loopPasG = borne(g_loopPasG + dr - g, 1, 64);
              g_loopLg = g_loopUnite * (uint32_t)g_loopPasG;
              g_loopRecale = 1;                        // (avant et apres : l'interruption peut tomber entre les deux)
              for (int rr = 0; rr < kLoopVoies; rr++) g_loopPasL[rr] = (uint8_t)g_loopPasG;
              g_loopRecale = 1;
              siprintf(l2, "%d STEPS", g_loopPasG);
              fm1_annonce2("LOOP", l2);
            } else {                                   // STP/CUR : la rangee du dernier curseur sur la grille
              const int rr = loopLigneVue;
              g_loopRecale = 1;
              g_loopPasL[rr] = (uint8_t)borne(g_loopPasL[rr] + dr - g, 1, 64);
              g_loopRecale = 1;
              siprintf(l2, "%d STEPS", g_loopPasL[rr]);
              char l1[12]; siprintf(l1, "LAYER %d", rr + 1);
              fm1_annonce2(l1, l2);
            }
            modifie = true;
          }
          frappe &= ~(KEY_A | KEY_B);
        } else if (aTenu && ici >= 0 && (g | dr | h | b)) {     // A + croix : la note
          const int ins = g_loop[ici].ins, dk = ins - kDkIns0;
          if (dk >= 0 && dk < 27) {
            // enregistree en DRUM KEYBOARD : la touche voisine (son son a elle), l'octave d'OCT gardee
            const int ecart = g_loop[ici].note - kDk[dk].note, k2 = borne(dk + dr - g + 12 * (h - b), 0, 26);
            g_loop[ici].ins = (uint8_t)(kDkIns0 + k2);
            g_loop[ici].note = (uint8_t)borne(kDk[k2].note + ecart, 1, MD_MAX_NOTE);
            static const char *gam[12] = {"C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"};
            const int nk = borne(42 + k2 + ecart, 1, MD_MAX_NOTE);
            char l2[8];
            siprintf(l2, "%s%d", gam[(nk - 1) % 12], (nk - 1) / 12);
            fm1_annonce2(FM1_PRESET_NOMS[kDk[k2].preset], l2);   // (le son, et le nom de sa touche)
          } else {                                     // au clavier : la hauteur
            const int n = borne(g_loop[ici].note + dr - g + 12 * (h - b), 1, MD_MAX_NOTE);
            g_loop[ici].note = (uint8_t)n;
            loopDerNote = n;
          }
          modifie = true;
        }
        const bool ab = loopCurL < kLoopVoies && (((frappe & KEY_A) && bTenu) || ((frappe & KEY_B) && aTenu));
        if (ab) {
          if (loopCurC == 0) {                         // la couche, enlevee
            g_loopMuet = (uint8_t)(g_loopMuet | (1u << couche));
            md_replayer_stop_test_note(kLoopVoie0 + couche);
            char l2[12]; siprintf(l2, "%d OFF", couche + 1);
            fm1_annonce2("LAYER", l2);
          } else if (false) {                          // (une croix : A seul l'enleve, plus bas)
            g_loop[ici].on = 2;
            loopFin(ici, true);
            md_replayer_stop_test_note(kLoopVoie0 + couche);
          }
          keysIgnoreUntilReleased(KEY_A | KEY_B);
          modifie = true;
        } else if ((frappe & KEY_A) && loopCurL < kLoopVoies) {
          if (loopCurC == 0) {                         // la couche, remise
            if ((g_loopMuet >> couche) & 1u) {
              g_loopMuet = (uint8_t)(g_loopMuet & ~(1u << couche));
              char l2[12]; siprintf(l2, "%d ON", couche + 1);
              fm1_annonce2("LAYER", l2);
            }
          } else if (!boucle) {
            fm1_annonce2("LOOPER", "REC FIRST");
          } else if (ici >= 0) {                       // une croix : A l'enleve, a son relachement (plus haut)
            aEnleve = ici;
          } else if ((loopCurC - 1) * loopParCase() >= g_loopPasL[couche]) {   // (au-dela des pas de la rangee : rien)
          } else if (const int ef = [&]() {          // une note enlevee de ce pas : elle revient telle quelle
                       for (int i = g_loopN - 1; i >= 0; i--)
                         if (g_loop[i].on == 2 && g_loop[i].couche % kLoopVoies == couche &&
                             loopPasDe(g_loop[i].t, couche) / loopParCase() == loopCurC - 1) return i;
                       return -1; }(); ef >= 0) {
            g_loop[ef].on = 1;                         // (sa note, son son, son moment exact ; et sa fin)
            loopFin(ef, false);
          } else if (g_loopN + 2 <= kLoopMax) {        // un pas vide : la derniere note, le temps d'un pas
            // le dernier son de la couche, a sa hauteur (une batterie de DRUM KEYBOARD reste une batterie) ;
            // une couche vide : le son du clavier, la derniere note posee
            int ins = 0, nt = loopDerNote;
            for (int i = g_loopN - 1; i >= 0 && !ins; i--)
              if (g_loop[i].on == 1 && g_loop[i].couche % kLoopVoies == couche) { ins = g_loop[i].ins; nt = g_loop[i].note; }
            if (!ins) ins = instrCible();
            if (ins) {
              // (pile sur le pas de la case : le premier de ses pas)
              const uint32_t pas = g_loopUnite, t0 = (uint32_t)((loopCurC - 1) * loopParCase()) * pas;
              LoopNote &f = g_loop[g_loopN + 1];       // (sa fin d'abord : la note n'existe qu'une fois complete)
              f.t = t0 + pas - 1u < lg ? t0 + pas - 1u : lg - 1u; f.note = (uint8_t)nt; f.ins = (uint8_t)ins;
              f.on = 0; f.couche = (uint8_t)couche;
              LoopNote &e = g_loop[g_loopN];
              e.t = t0; e.note = (uint8_t)nt; e.ins = (uint8_t)ins; e.on = 1; e.couche = (uint8_t)couche;
              g_loopN = g_loopN + 2;
              if (couche > g_loopCouche) g_loopCouche = couche;
            }
          }
          modifie = true;
        }
        frappe &= ~(KEY_A | KEY_B);
      }
      if (page == PAGE_BEND) {
        if (bendRelis) {                          // les pistes qui ont de la musique, dans l'ordre
          bendRelis = false;
          bendNbPistes = 0;
          // (et celle du clavier, meme vide : c'est sur elle qu'il joue)
          for (int c = 0; c < md_replayer_get_num_channels() && c < 18; c++) {
            bool la = c == bendVoieClavier;
            for (int l = 0; l < MD_SONG_ROWS && !la; l++)
              la = md_replayer_get_song(c, l) != MD_EMPTY;
            if (la) bendPiste[bendNbPistes++] = c;
          }
          bendCol = borne(bendCol, 0, bendNbPistes);
        }
        int d[7] = { pas[0], pas[1], pas[2], pas[3], pas[6], pas[4], pas[5] };
        if (!fm1_clavier() && !(keysHeld() & KEY_SELECT)) {
          const bool aTenu = keysHeld() & KEY_A;
          if (appui & KEY_UP) bendSel = borne(bendSel - 1, 0, 6);
          if (appui & KEY_DOWN) bendSel = borne(bendSel + 1, 0, 6);
          if (!aTenu && (appui & KEY_RIGHT)) bendCol = borne(bendCol + 1, 0, bendNbPistes);
          if (!aTenu && (appui & KEY_LEFT)) bendCol = borne(bendCol - 1, 0, bendNbPistes);
          const bool met = aTenu && (appui & KEY_RIGHT), enleve = aTenu && (appui & KEY_LEFT);
          if (bendCol == 0) {
            if (met) d[bendSel] = 1;
            if (enleve) d[bendSel] = -1;
          } else if (met || enleve) {
            const int c = bendPiste[bendCol - 1];
            const uint32_t avant = bendPistes[bendSel];
            if (met) bendPistes[bendSel] |= 1u << c; else bendPistes[bendSel] &= ~(1u << c);
            if (met && !((bendBits >> bendSel) & 1)) { bendBits |= 1 << bendSel; fm1_bend(bendBits); }
            if (bendPistes[bendSel] != avant) fm1_bend_pistes(bendSel, bendPistes[bendSel]);
            char t2[12];
            siprintf(t2, met ? "T%d ON" : "T%d OFF", c + 1);
            fm1_annonce2(kBendNom[bendSel], t2);
          }
        }
        for (int k = 0; k < 7; k++) {
          if (!d[k]) continue;
          const int avant = bendBits;
          if (d[k] > 0) bendBits |= 1 << k; else bendBits &= ~(1 << k);
          if (bendBits != avant) {
            fm1_bend(bendBits);
            fm1_annonce2(kBendNom[k], (bendBits >> k) & 1 ? "ON" : "OFF");
            // Les animations, reparties sur les molettes : GLITCH (KNOB 2) le chat,
            // DRONE (KNOB 4) la tortue, NOISE (PRESETS) la danse, CHAOS
            // (ALGORITHM) le nunchaku.
            if ((bendBits >> k) & 1) fm1_gif(k == 1 ? 0 : k == 3 ? 1 : k == 5 ? 2 : k == 6 ? 3 : -1);
          }
        }
        for (int k = 0; k < 7; k++) pas[k] = 0;
        aucun = 1;
      }
      // La page PRESET : PRESETS (et haut / bas) fait defiler les presets, et
      // chacun s'applique tout de suite a l'instrument, son nom en gros.
      if (page == PAGE_PRESET) {
        // (fm1_opaque : sans lui, le compilateur fait de « haut : -1 » un sextra,
        // que l'emulateur en ligne lit sans signe)
        int d = pas[4] + (int)fm1_opaque((appui & KEY_DOWN) ? 1u : 0u)
                       - (int)fm1_opaque((appui & KEY_UP) ? 1u : 0u);
        pas[4] = 0;
        aucun = 1;
        for (int k = 0; k < 6; k++) if (pas[k]) aucun = 0;
        if (d) {
          presetSel = borne(presetSel + d, 0, FM1_NB_PRESETS - 1);
          appliquePreset(presetIns, presetSel);
          char l2[20];
          snprintf(l2, sizeof l2, "%d/%d", presetSel + 1, FM1_NB_PRESETS);
          fm1_annonce2(FM1_PRESET_NOMS[presetSel], l2);
        }
      }
      // KNOB GENERAL, SELECT : FM, AM (la moitie des sons, un sur deux) (selPos, plus haut) ; les sons d'avant : EDIT.
      // Les sons qui servent : un seul passage sur les phrases (le faire pour chaque instrument prenait des
      // centaines de milliers de lectures : la molette mettait longtemps a agir).
      if (page == PAGE_GLOBAL && pas[6]) {
        auto photo = [&](uint8_t (*b)[MD_OPL_OCTETS]) {
          for (int i = 1; i <= MD_MAX_INSTRUMENTS; i++)
            for (int o = 0; o < MD_OPL_OCTETS; o++) b[i][o] = md_replayer_get_instr_opl(i, o);
        };
        auto remet = [&](uint8_t (*b)[MD_OPL_OCTETS]) {
          for (int i = 1; i <= MD_MAX_INSTRUMENTS; i++)
            if (genreInstr(i) == GENRE_OPL)
              for (int o = 0; o < MD_OPL_OCTETS; o++) md_replayer_set_instr_opl(i, o, b[i][o]);
        };
        auto moitieAM = [&](bool met) {                  // la moitie des sons en AM, ou leur connexion rendue
          static uint8_t sert[MD_MAX_INSTRUMENTS + 1];
          memset(sert, 0, sizeof sert);
          for (int ph = 0; ph < MD_MAX_PHRASES; ph++)
            for (int r = 0; r < MD_ROWS_PER_PHRASE; r++) {
              uint8_t no, i2, ve, cm, cv, mc, mv;
              md_replayer_get_phrase((uint8_t)ph, r, &no,&i2,&ve,&cm,&cv,&mc,&mv);
              if (i2 && i2 <= MD_MAX_INSTRUMENTS) sert[i2] = 1;
            }
          for (int k = 0; k < 11; k++) {                 // (la DRUM MACHINE, si ses sons existent)
            const char *nm = md_replayer_get_instr_name(kDrumIns0 + k);
            if (nm && !strncmp(nm, "DN ", 3)) sert[kDrumIns0 + k] = 1;
          }
          for (int k = 0; g_dkPret && k < 27; k++) sert[kDkIns0 + k] = 1;   // (et DRUM KEYBOARD)
          int n = 0;
          for (int i = 1; i <= MD_MAX_INSTRUMENTS; i++) {
            if (!sert[i] || genreInstr(i) != GENRE_OPL) continue;
            if (met && (n++ & 1) == 0) {                 // un sur deux
              amAvant[i] = (uint8_t)(1 + oplLit(i, MD_OPL_FB_CONN, 0, 1));
              oplEcrit(i, MD_OPL_FB_CONN, 0, 1, 1);
            } else if (!met && amAvant[i]) {
              oplEcrit(i, MD_OPL_FB_CONN, 0, 1, amAvant[i] - 1);
              amAvant[i] = 0;
            }
          }
          demiAM = met;
        };
        if (!sonsAlloues()) { fm1_annonce2("ALL SOUNDS", "MEMORY FULL"); pas[6] = 0; }
        else if (!defautPris) { photo(sonDefaut); defautPris = true; }
        const int vise = !sonsAlloues() ? selPos : borne(selPos + pas[6], 0, 1);   // (FM ou AM ; le son d'avant : EDIT)
        while (selPos != vise) {
          const int de = selPos;
          selPos += vise > selPos ? 1 : -1;
          if (de == 0 && selPos == 1) moitieAM(true);
          else if (de == 1 && selPos == 0) moitieAM(false);
          else if (de == 2 && selPos == 3) {           // DEFAULT : les sons d'avant KNOB GENERAL (ceux-ci gardes)
            photo(sonCourant);
            memcpy(amCourant, amAvant, sizeof amCourant);
            for (int k = 0; k < 6; k++) pousseCourant[k] = globalPousse[k];
            demiCourant = demiAM;
            remet(sonDefaut);
            memset(amAvant, 0, sizeof amAvant);
            for (int k = 0; k < 6; k++) globalPousse[k] = 0;
            demiAM = false;
          } else if (de == 3 && selPos == 2) {         // CURRENT : les sons d'avant DEFAULT
            remet(sonCourant);
            memcpy(amAvant, amCourant, sizeof amAvant);
            for (int k = 0; k < 6; k++) globalPousse[k] = pousseCourant[k];
            demiAM = demiCourant;
          }
        }
        static const char *kSelNom[4] = { "FM", "AM", "CURRENT", "DEFAULT" };
        fm1_annonce3("ALL SOUNDS", kSelNom[selPos], "EDIT TO DEFAULT");
        modifie = true;
      }
      if (page == PAGE_GLOBAL) {                   // (SELECT n'est pas un reglage d'instrument ici)
        pas[6] = 0;
        aucun = 1;
        for (int k = 0; k < 6; k++) if (pas[k]) aucun = 0;
      }
      if (!aucun) {
        // La page KNOB GENERAL : les memes reglages, mais sur TOUS les sons
        // (les instruments OPL3 du morceau et ceux de la DRUM MACHINE), chacun
        // pousse d'autant depuis SA valeur (rien n'est remis a une meme valeur).
        const bool tous = page == PAGE_GLOBAL;
        if (tous && !defautPris && sonsAlloues()) {      // (les sons par defaut : avant le premier reglage)
          for (int i = 1; i <= MD_MAX_INSTRUMENTS; i++)
            for (int o = 0; o < MD_OPL_OCTETS; o++) sonDefaut[i][o] = md_replayer_get_instr_opl(i, o);
          defautPris = true;
        }
        if (tous) selPos = demiAM ? 1 : 0;               // (une autre molette : SELECT repart de FM, AM)
        static uint8_t sert[MD_MAX_INSTRUMENTS + 1];
        int nbSons = 0;
        if (tous) {
          memset(sert, 0, sizeof sert);
          for (int ph = 0; ph < MD_MAX_PHRASES; ph++)
            for (int r = 0; r < MD_ROWS_PER_PHRASE; r++) {
              uint8_t no, i2, ve, cm, cv, mc, mv;
              md_replayer_get_phrase((uint8_t)ph, r, &no,&i2,&ve,&cm,&cv,&mc,&mv);
              if (i2 && i2 <= MD_MAX_INSTRUMENTS) sert[i2] = 1;
            }
          for (int k = 0; k < 11; k++) {                 // (la DRUM MACHINE, si ses sons existent)
            const char *nm = md_replayer_get_instr_name(kDrumIns0 + k);
            if (nm && !strncmp(nm, "DN ", 3)) sert[kDrumIns0 + k] = 1;
          }
          for (int k = 0; g_dkPret && k < 27; k++) sert[kDkIns0 + k] = 1;   // (et DRUM KEYBOARD, le LOOPER compris)
          for (int i = 1; i <= MD_MAX_INSTRUMENTS; i++)
            if (sert[i] && genreInstr(i) != GENRE_OPL) sert[i] = 0;
          for (int i = 1; i <= MD_MAX_INSTRUMENTS; i++) nbSons += sert[i];
        }
        // LOOPER : tous les sons de la rangee du curseur (la couche 1 tant qu'on ne l'a pas bougee) ; une rangee
        // vide, le son du clavier
        bool plusieurs = tous;
        if (page == PAGE_LOOPER && g_loopEtat >= 2 && g_loopEtat <= 4) {
          const int c = loopCibleL;
          memset(sert, 0, sizeof sert);
          for (int i = 0; i < g_loopN; i++) {
            // (fm1_opaque : sans lui, une lecture « b[++r=-2] » que l'emulateur en ligne ne connait pas)
            const int ii = (int)fm1_opaque(g_loop[i].ins);
            if (g_loop[i].on == 1 && g_loop[i].couche % kLoopVoies == c && genreInstr(ii) == GENRE_OPL) sert[ii] = 1;
          }
          for (int i = 1; i <= MD_MAX_INSTRUMENTS; i++) if (sert[i]) plusieurs = true;
        }
        int ins = plusieurs ? 0 : instrCible();
        if (plusieurs)
          for (int i = 1; i <= MD_MAX_INSTRUMENTS && !ins; i++) if (sert[i]) ins = i;
        // La valeur reglee, en gros au milieu de l'ecran ; sans note jouee :
        // on n'entend que le clavier et le morceau.
        char l1[12] = "NO OPL3", l2[24] = "INSTRUMENT";
        while (ins) {                                    // (une fois ; sur KNOB GENERAL, chaque son)
          if (!tous) {                                   // (EDIT : le son tel qu'avant les molettes)
            bool deja = false;
            for (int e = 0; e < editN; e++) if (editIns[e] == ins) deja = true;
            if (!deja && editN < 8) {
              editIns[editN] = ins;
              for (int o = 0; o < MD_OPL_OCTETS; o++) editSon[editN][o] = md_replayer_get_instr_opl(ins, o);
              editN++;
            }
          }
          // (KNOB 2 : LENGTH plutot que la sortie de la porteuse, qui en FM ne
          // fait que le volume de l'instrument ; il se calcule a part, plus bas)
          static const int octet[6] = { MD_OPL_VOL_M, MD_OPL_AD_C, MD_OPL_FB_CONN, MD_OPL_FLAGS_M,
                                        MD_OPL_WAVE_M, MD_OPL_WAVE_M + 1 };
          static const int dec[6] = { 0, 0, 1, 0, 0, 0 }, masque[6] = { 63, 15, 7, 15, 7, 7 };
          snprintf(l1, sizeof l1, "INS %02X", ins);
          for (int k = 0; k < 6; k++) {
            if (!pas[k]) continue;
            const int v = oplLit(ins, octet[k], dec[k], masque[k]);
            if (k == 0) {                           // OUT : 3F = fort, l'inverse du TL
              const int niv = borne(63 - v + pas[k], 0, 63);
              oplEcrit(ins, octet[k], dec[k], masque[k], 63 - niv);
              snprintf(l2, sizeof l2, "MOD OUT %02X", niv);
            } else if (k == 1) {
              // LENGTH, la duree de la note, 0 (tres courte) a F (tenue), sur
              // les DEUX operateurs : DECAY et RELEASE a la meme vitesse, EGT
              // eteint (la note meurt seule, meme tenue par le tracker) ; a F,
              // EGT allume (elle tient tant qu'on la joue), DECAY et RELEASE
              // les plus lents. ATTACK et SUSTAIN ne bougent pas.
              const int avant = oplLit(ins, MD_OPL_FLAGS_C, 5, 1) ? 15 : 15 - oplLit(ins, MD_OPL_AD_C, 0, 15);
              const int lg = borne(avant + pas[k], 0, 15), vitesse = lg == 15 ? 1 : 15 - lg;
              for (int op = 0; op < 2; op++) {
                oplEcrit(ins, MD_OPL_FLAGS_M + op, 5, 1, lg == 15);
                oplEcrit(ins, MD_OPL_AD_M + op, 0, 15, vitesse);
                oplEcrit(ins, MD_OPL_SR_M + op, 0, 15, vitesse);
              }
              snprintf(l2, sizeof l2, "LENGTH %X", lg);
            } else if (k == 4 && !tous) {
              // PRESETS : les formes d'onde de l'OP1, puis (encore a droite) FM,
              // puis AM : la connexion de l'instrument. A gauche, l'inverse.
              static uint8_t posOnde[MD_MAX_INSTRUMENTS + 1];   // (la position + 1 ; 0 : a deduire)
              const bool am = oplLit(ins, MD_OPL_FB_CONN, 0, 1);
              int p0 = posOnde[ins] ? posOnde[ins] - 1 : am ? 9 : v;
              if (am) p0 = 9;
              else if (p0 == 9) p0 = 8;
              const int p = borne(p0 + pas[k], 0, 9);
              posOnde[ins] = (uint8_t)(p + 1);
              if (p <= 7) {
                oplEcrit(ins, octet[k], dec[k], masque[k], p);
                oplEcrit(ins, MD_OPL_FB_CONN, 0, 1, 0);
                snprintf(l2, sizeof l2, "OP1 %s", kOndeNom[p]);
              } else {
                oplEcrit(ins, MD_OPL_FB_CONN, 0, 1, p == 9);
                strcpy(l2, p == 9 ? "AM" : "FM");
              }
            } else {
              const int w = borne(v + pas[k], 0, masque[k]);
              oplEcrit(ins, octet[k], dec[k], masque[k], w);
              if (k == 2) snprintf(l2, sizeof l2, "FEEDBACK %d", w);
              else if (k == 3) snprintf(l2, sizeof l2, "MOD MULT %s", kMultNom[w]);
              else snprintf(l2, sizeof l2, "OP%d %s", k - 3, kOndeNom[w]);   // la forme d'onde
            }
          }
          // (sur SONG, CHAIN, PHRASE, rien ne change a l'ecran : la grille ne se
          // repeint pas ; sur INSTR, les valeurs, si)
          if (page == PAGE_INSTR) modifie = true;
          int suivant = 0;                               // le son suivant (KNOB GENERAL), sinon fini
          for (int i = ins + 1; plusieurs && i <= MD_MAX_INSTRUMENTS && !suivant; i++) if (sert[i]) suivant = i;
          ins = suivant;
        }
        if (plusieurs && !tous) {                        // (LOOPER : la rangee)
          snprintf(l1, sizeof l1, "LAYER %d", loopCibleL + 1);
        }
        if (tous) {                                      // l'annonce : ce qui a bouge, et de combien en tout
          static const char *kQuoi[6] = { "MOD OUT", "LENGTH", "FEEDBACK", "MOD MULT", "OP1 WAVE", "OP2 WAVE" };
          snprintf(l1, sizeof l1, nbSons == 1 ? "%d SOUND" : "%d SOUNDS", nbSons);
          for (int k = 0; k < 6; k++)
            if (pas[k]) {
              globalPousse[k] += pas[k];
              snprintf(l2, sizeof l2, "%s %+d", kQuoi[k], globalPousse[k]);
              if (globalPousse[k] && globalPousse[k] % 4 == 0) fm1_gif(-2);   // (par-ci, par-la, au hasard)
            }
          if (!nbSons) { strcpy(l1, "NO OPL3"); strcpy(l2, "SOUNDS"); }
          modifie = true;
        }
        if ((!tous && editN) || (tous && defautPris)) fm1_annonce3(l1, l2, "EDIT TO DEFAULT");
        else fm1_annonce2(l1, l2);
      }
      // EDIT : les sons d'avant les molettes reviennent
      // (et KNOB GENERAL : tous les sons d'avant son premier reglage, sonDefaut)
      fm1_edit_clignote(editN > 0 || defautPris);
      if (fm1_edit_appuis() && (editN || defautPris) && !navigateur && !dialogueNom) {
        if (defautPris && sonDefaut) {
          for (int i = 1; i <= MD_MAX_INSTRUMENTS; i++)
            if (genreInstr(i) == GENRE_OPL)
              for (int o = 0; o < MD_OPL_OCTETS; o++) md_replayer_set_instr_opl(i, o, sonDefaut[i][o]);
          memset(amAvant, 0, sizeof amAvant);
          for (int k = 0; k < 6; k++) globalPousse[k] = 0;
          demiAM = false; selPos = 0; defautPris = false;   // (le prochain reglage reprend une photo)
        }
        for (int e = 0; e < editN; e++)                   // (apres : ce que les molettes ont touche depuis)
          for (int o = 0; o < MD_OPL_OCTETS; o++) md_replayer_set_instr_opl(editIns[e], o, editSon[e][o]);
        fm1_annonce2("SOUNDS", "DEFAULT");
        editN = 0;
        modifie = true;
      }
    }

    // ── REC ────────────────────────────────────────────────────────────────
    // Sur SONG, CHAIN et PHRASE, en mode clavier comme en mode tracker. REC :
    // un decompte de quatre temps au tempo (1, 2, 3, 4 en gros, un charleston
    // ferme OPL3 a chaque temps), puis le morceau joue depuis la ligne de SONG
    // du data sous le curseur, le metronome continue, et ce qu'on joue au
    // clavier s'ecrit la ou en est la lecture de cette piste (la ligne qui
    // joue), avec l'instrument du clavier. On avance sans s'arreter : quand
    // une phrase finit, la ligne suivante du chain (une phrase neuve si elle
    // est vide) ; quand le chain est plein, la ligne suivante de SONG (un
    // chain neuf si elle est vide). REC (ou START) arrete. Le temps se tient
    // dans l'interruption audio (recAudio, plus haut) ; ici, l'ecran et les
    // donnees. Le metronome joue sur des pistes OPL3 vides du morceau, avec
    // l'instrument FF, remis a son reglage par defaut a l'arret.
    {
      static int temps = -1;                       // le dernier temps du decompte affiche
      // Le data de la piste voie a la ligne de SONG sr, ligne de chain cr :
      // cree s'il manque (un chain, une phrase). La phrase, ou -1. d'avance :
      // retenu (recAvance), pour le retirer si la lecture n'y arrive pas.
      auto prepare = [&](int voie, int sr, int cr, bool avance) {
        if (sr < 0 || sr >= MD_SONG_ROWS || cr < 0 || cr >= MD_ROWS_PER_CHAIN) return -1;
        bool chainNeuf = false, phraseNeuve = false;
        uint8_t ch = md_replayer_get_song(voie, sr);
        if (ch == MD_EMPTY) {
          const int c = chainLibre();
          if (c < 0 || c >= MD_MAX_CHAINS) return -1;
          ch = (uint8_t)c;
          md_replayer_set_song(voie, sr, ch);
          chainNeuf = true;
        }
        uint8_t ph; int8_t tsp;
        md_replayer_get_chain(ch, cr, &ph, &tsp);
        if (ph == MD_EMPTY) {
          const int f = phraseLibre();
          if (f < 0 || f >= MD_MAX_PHRASES) return -1;
          ph = (uint8_t)f;
          md_replayer_set_chain(ch, cr, ph, 0);
          phraseNeuve = true;
        }
        if (!avance) { recDebutPhN = phraseNeuve; recDebutChN = chainNeuf; }   // (la premiere phrase)
        if (avance && (chainNeuf || phraseNeuve)) {
          recAvance.la = true; recAvance.chainNeuf = chainNeuf; recAvance.phraseNeuve = phraseNeuve;
          recAvance.sr = sr; recAvance.cr = cr; recAvance.ch = ch; recAvance.ph = ph;
        }
        return (int)ph;
      };
      // REC s'arrete : la lecture, elle, CONTINUE (comme dans tout logiciel de
      // musique : on sort de l'enregistrement, PLAY arrete la lecture) ; seul
      // le decompte, qui n'a encore rien lance, ne laisse rien jouer.
      auto arrete = [&](bool annonce) {
        g_recEtat = 0;
        recAttente = false;
        if (!recLibre && recEtat == 2) {
          md_replayer_rec_boucle(recVoie, -1, 0, 0, 0);
          // Le silence de la fin (ce que REC a cree apres la derniere note) disparait,
          // les silences du milieu restent.
          if (recBoucle != 2) recRetire(recDerU + 1);
          // Ce qui a ete prepare d'avance sans que la lecture y arrive disparait.
          if (recAvance.la) {
            if (recAvance.phraseNeuve) md_replayer_set_chain((uint8_t)recAvance.ch, recAvance.cr, MD_EMPTY, 0);
            if (recAvance.chainNeuf) md_replayer_set_song(recVoie, recAvance.sr, MD_EMPTY);
          }
        }
        recBoucle = 0;
        recAvance.la = false;
        for (int k = 0; k < g_recNbMetro; k++) md_replayer_stop_test_note(g_recMetro[k]);
        g_recNbMetro = 0;
        md_replayer_init_default_instrument(kRecInsMetro - 1);
        if (recEtat == 1 && (enLecture || md_replayer_is_playing())) { md_replayer_stop(); enLecture = false; }
        recEtat = 0;
        recLibre = false;
        if (annonce) fm1_annonce2("REC OFF", 0);
        modifie = true;
      };
      // Le metronome (les dernieres pistes OPL3 sans rien dans SONG, trois au
      // plus, le charleston FF) et le tempo, pour l'interruption (recAudio).
      auto recPrepareTemps = [&]() {
        g_recNbMetro = 0;
        for (int t = MD_PISTES_OPL - 1; t >= 0 && g_recNbMetro < 3; t--) {
          if (t == recVoie) continue;
          bool vide = true;
          for (int r = 0; r < MD_SONG_ROWS && vide; r++) vide = md_replayer_get_song(t, r) == MD_EMPTY;
          if (vide) g_recMetro[g_recNbMetro++] = t;
        }
        appliquePreset(kRecInsMetro, 3);         // HH-C,
        oplEcrit(kRecInsMetro, MD_OPL_AD_C, 0, 15, 6);   // un peu plus long : il s'entend mieux
        for (int o = 0; o < MD_OPL_OCTETS; o++) g_recMetroSon[0][o] = md_replayer_get_instr_opl(kRecInsMetro, o);
        oplEcrit(kRecInsMetro, MD_OPL_AD_C, 0, 15, 3);   // le temps 1 : bien plus long (decay et release lents)
        oplEcrit(kRecInsMetro, MD_OPL_SR_C, 0, 15, 4);
        for (int o = 0; o < MD_OPL_OCTETS; o++) g_recMetroSon[1][o] = md_replayer_get_instr_opl(kRecInsMetro, o);
        for (int o = 0; o < MD_OPL_OCTETS; o++) md_replayer_set_instr_opl(kRecInsMetro, o, g_recMetroSon[0][o]);
        g_recVoie = recVoie;
        const double bpm = md_replayer_get_bpm();
        g_recPeriode = (uint32_t)(fm1_echantillons_par_seconde() * 60.0 / (bpm > 1.0 ? bpm : 125.0));
        const int rpb = md_replayer_get_rows_per_beat() > 0 ? md_replayer_get_rows_per_beat() : 4;
        const int vit = md_replayer_get_speed() > 0 ? md_replayer_get_speed() : 6;
        g_recAvance = g_recPeriode / (uint32_t)(rpb * vit);   // un tick du moteur
      };
      // REC PENDANT LA LECTURE (ou PLAY, REC arme) : on enregistre tout de
      // suite, sans decompte ni arret, la ou joue la piste du curseur. Lecture
      // lancee depuis SONG : comme REC depuis l'arret (la suite se cree, la
      // fin se range) ; et si la piste n'avait rien la, un data s'y cree, et
      // elle rejoint la lecture. Depuis CHAIN, PHRASE : sur leur boucle.
      auto recLive = [&]() {
        recVoie = (lectureScope == 0 && page == PAGE_SONG && curCanal < nbVisibles) ? reel(curCanal) : voieCourante;
        recLibre = lectureScope != 0;
        recAvance.la = false; recAttente = false; recDebutPhN = false; recDebutChN = false;
        recNbU = 0; recDerU = -1; recBoucle = 0; recVuPh = -1; recDerPos = -1; recPrepare = -1;
        if (md_replayer_play_phrase(recVoie) < 0) {
          int ref = -1;
          for (int t = 0; t < MD_PISTES_OPL && ref < 0; t++)
            if (t != recVoie && md_replayer_play_phrase(t) >= 0) ref = t;
          const int sr = ref >= 0 ? md_replayer_play_song_row(ref) : -1, cr = ref >= 0 ? md_replayer_play_chain_row(ref) : -1;
          if (recLibre || ref < 0 || prepare(recVoie, sr, cr, false) < 0 || !md_replayer_rejoint(recVoie, ref)) {
            fm1_annonce2("REC", "NOTHING PLAYING");
            recEtat = 0; recLibre = false;
            return;
          }
        }
        recIns = instrCible();
        if (!recIns) recIns = instrCanal[recVoie] ? instrCanal[recVoie] : instrId > 0 ? instrId : 1;
        recPrepareTemps();
        g_recPtrPh = md_replayer_play_phrase(recVoie);
        g_recPtrRow = md_replayer_play_phrase_row(recVoie);
        g_recJouePh = -1; g_recJoueRow = -1; g_recPrecPh = -1; g_recPrecRow = -1; g_recNbLignes = 0;
        g_recTemps = 4;                          // (l'ecran dit REC)
        recEtat = 2;
        g_recEtat = 2;
        suiviLecture = true;
        modifie = true;
      };
      static bool rappelPose = false;
      if (!rappelPose) { rappelPose = true; fm1_rappel_audio(recAudio); }
      // REC, la DRUM MACHINE qui tourne (hors de son mode) : son motif est
      // COPIE tel quel la ou est le curseur. Une phrase ne joue qu'un son par
      // ligne : sur SONG, chaque voie de la boite (BD ; SD CP ; CH OH ; LT HT ;
      // RM SH TB ; CY) va sur sa piste, celle du curseur puis les suivantes, a
      // la ligne du curseur (un chain et sa phrase y sont crees s'il le faut,
      // sinon la premiere phrase du chain recoit les pas) ; sur CHAIN et
      // PHRASE, tout va dans la phrase visee, le premier son de chaque pas.
      auto drumCopie = [&]() {
        md_module_t *m = md_replayer_get_module();
        if (!m) return;
        static const uint8_t kVoies[6] = { 17, 16, 15, 14, 13, 12 };
        auto ecrit = [&](int ph, int voieDrum) {   // voieDrum : un groupe (-1 : tous, le premier son)
          for (int j = 0; j < 16; j++) {
            int son = -1;
            for (int k = 0; k < 11; k++)
              if ((m->drum_pas[k] >> j) & 1u && (voieDrum < 0 || kDrumVoie[k] == voieDrum)) {
                if (voieDrum < 0 && son >= 0) continue;   // (tous : le premier)
                son = k;                                   // (un groupe : le dernier, comme la boite)
              }
            uint8_t no, i2, ve, cm, cv, mc, mv;
            md_replayer_get_phrase((uint8_t)ph, j, &no,&i2,&ve,&cm,&cv,&mc,&mv);
            if (son >= 0) md_replayer_set_phrase((uint8_t)ph, j, (uint8_t)drumNote(m, son), (uint8_t)(kDrumIns0 + son), ve, cm, cv, mc, mv);
            else md_replayer_set_phrase((uint8_t)ph, j, 0, 0, ve, cm, cv, mc, mv);
          }
        };
        int pistes = 0;
        if (page == PAGE_SONG && curCanal < nbVisibles) {
          int t = reel(curCanal);
          for (int g = 0; g < 6 && t < MD_PISTES_OPL; g++) {
            bool la = false;
            for (int k = 0; k < 11; k++) if (kDrumVoie[k] == kVoies[g] && m->drum_pas[k]) la = true;
            if (!la) continue;
            uint8_t ch = md_replayer_get_song(t, curLigne);
            if (ch == MD_EMPTY) {
              const int c = chainLibre();
              if (c < 0 || c >= MD_MAX_CHAINS) break;
              ch = (uint8_t)c;
              md_replayer_set_song(t, curLigne, ch);
            }
            uint8_t ph; int8_t tsp;
            md_replayer_get_chain(ch, 0, &ph, &tsp);
            if (ph == MD_EMPTY) {
              const int f = phraseLibre();
              if (f < 0 || f >= MD_MAX_PHRASES) break;
              ph = (uint8_t)f;
              md_replayer_set_chain(ch, 0, ph, 0);
            }
            ecrit(ph, kVoies[g]);
            t++; pistes++;
          }
        } else if (page == PAGE_CHAIN || page == PAGE_PHRASE) {
          int ph = page == PAGE_PHRASE ? phraseId : -1;
          if (page == PAGE_CHAIN) {
            uint8_t p2; int8_t tsp;
            md_replayer_get_chain((uint8_t)chainId, chLigne, &p2, &tsp);
            if (p2 == MD_EMPTY) {
              const int f = phraseLibre();
              if (f >= 0 && f < MD_MAX_PHRASES) { p2 = (uint8_t)f; md_replayer_set_chain((uint8_t)chainId, chLigne, p2, tsp); }
            }
            ph = p2 == MD_EMPTY ? -1 : p2;
          }
          if (ph >= 0) { ecrit(ph, -1); pistes = 1; }
        }
        char l2[20];
        siprintf(l2, pistes ? "%d TRACK%s" : "SONG CHAIN PHRASE", pistes, pistes > 1 ? "S" : "");
        fm1_annonce2(pistes ? "DRUMS COPIED" : "REC", l2);
        modifie = true;
      };
      // LOOPER : REC tenu (plus d'une demi-seconde) defait ce que son appui avait fait, puis efface les notes
      // une a une, la derniere d'abord, tant qu'on le tient (cinq par seconde)
      {
        static bool efface = false;
        static uint32_t prochain = 0, tenuDepuis = 0;
        const uint32_t maintenant = fm1_echantillons(), sec = fm1_echantillons_par_seconde();
        const bool tenu = fm1_rec_tenu();
        if (!tenu) tenuDepuis = maintenant;            // (le temps tenu : depuis CET appui)
        // LE DECOMPTE d'une boucle neuve : 1, 2, 3, 4 en gros, sans son, un peu moins vite que le tempo par defaut
        // (100 BPM) ; la prise commence a la fin du 4, pile (g_loopT0 : le debut du decompte, puis de la prise)
        if (g_loopEtat == 5) {
          const uint32_t temps = sec * 3u / 5u, fait = (maintenant - g_loopT0) / temps;
          static uint32_t vu = 0;
          if (fait >= 4u) {
            g_loopT0 += 4u * temps; g_loopEtat = 1;
            fm1_annonce2("LOOPER", "REC");
            modifie = true;
          } else if (fait != vu) {
            char t[4]; siprintf(t, "%u", (unsigned)fait + 1u);
            fm1_annonce2(t, 0);
            vu = fait;
          }
        }
        if (fm1_looper() && tenu && maintenant - tenuDepuis > sec / 2u && g_loopEtat != 5 && g_loopEtat != 0) {
          if (!efface) {
            efface = true;
            prochain = maintenant;
            if (loopRecAvant == 0 && g_loopEtat == 1) loopEfface();        // (une prise juste lancee : rien)
            else if (loopRecAvant == 4) { g_loopEtat = 4; loopSilence(); }
            else if (g_loopEtat == 3) g_loopEtat = 2;
          }
          if ((int32_t)(maintenant - prochain) >= 0) {
            prochain += sec / 5u;
            fm1_annonce2("LOOPER", loopEnleveDerniere() ? "REMOVED" : "EMPTY");
            modifie = true;
          }
        } else efface = false;
      }
      const int recAppuis = fm1_rec_appuis();
      if (recAppuis && fm1_looper() && !recEtat) {  // LOOPER : REC, comme la pedale
        const uint32_t maintenant = fm1_echantillons();
        loopRecAvant = g_loopEtat;                 // (REC tenu : il efface, plus haut ; et defait cet appui)
        if (g_loopEtat == 4) {                     // arretee : elle repart, et on ajoute une couche
          g_loopT0 = maintenant; loopRepart(); g_loopCouche++; g_loopEtat = 3;
          g_loopDebutPrise = g_loopN;
          loopCibleL = g_loopCouche % kLoopVoies;      // (les molettes : ce qu'on enregistre)
          fm1_annonce2("LOOPER", "OVERDUB");
        } else if (g_loopEtat == 0) {              // vide : une boucle neuve, apres le decompte (plus bas)
          loopSilence();
          g_loopN = 0; g_loopCouche = 0; g_loopMuet = 0; g_loopOct = 0; g_loopT0 = maintenant; g_loopEtat = 5;
          loopCibleL = 0;
          fm1_annonce2("1", 0);
        } else if (g_loopEtat == 5) {              // (REC pendant le decompte : abandonne)
          g_loopEtat = 0;
          fm1_annonce2("LOOPER", "CANCELLED");
        } else if (g_loopEtat == 1) {              // la boucle se ferme : sa duree
          uint32_t lg = maintenant - g_loopT0;
          if (lg < fm1_echantillons_par_seconde() / 10u) lg = fm1_echantillons_par_seconde() / 10u;
          if (g_loopN > 0 && g_loopN < kLoopMax && g_loop[g_loopN - 1].on) {   // (une note tenue : sa fin, au bout)
            LoopNote &e = g_loop[g_loopN];
            const LoopNote &d = g_loop[g_loopN - 1];   // (champ par champ : la copie d'un bloc, l'emulateur ne la lit pas)
            e.note = d.note; e.ins = d.ins; e.couche = d.couche; e.on = 0; e.t = lg - 1u;
            g_loopN = g_loopN + 1;
          }
          {                                        // cale la boucle sur ce qu'on a joue (loopDetecte)
            uint32_t u, dec; int nb;
            loopDetecte(lg, &u, &nb, &dec);
            g_loopUnite = u; g_loopPasG = nb;
            for (int rr = 0; rr < 6; rr++) g_loopPasL[rr] = (uint8_t)nb;
            lg = u * (uint32_t)nb;
            for (int i = 0; i < g_loopN; i++) {    // (tout recule du decalage ; une fin de note au-dela : au bout)
              LoopNote &e = g_loop[(int)fm1_opaque((uint32_t)i)];
              e.t = e.t > dec ? e.t - dec : 0u;
              if (e.t >= lg) e.t = lg - 1u;
            }
          }
          g_loopLg = lg;
          loopRepart(); g_loopEtat = 2;
          char l2[20];
          const uint32_t cs = lg / (fm1_echantillons_par_seconde() / 100u);
          siprintf(l2, "%u.%02u S", (unsigned)(cs / 100u), (unsigned)(cs % 100u));
          fm1_annonce2("LOOP", l2);
        } else if (g_loopEtat == 2) {              // OVERDUB : une couche de plus
          g_loopCouche++; g_loopEtat = 3;
          g_loopDebutPrise = g_loopN;
          loopCibleL = g_loopCouche % kLoopVoies;
          fm1_annonce2("LOOPER", "OVERDUB");
        } else {                                   // (la fin de l'OVERDUB)
          g_loopEtat = 2;
          fm1_annonce2("LOOPER", "PLAY");
        }
      } else if (recAppuis) {
        if (!recEtat && g_drumJoue && !fm1_drum()) drumCopie();
        else if (recEtat) arrete(true);
        else if (enLecture) recLive();
        else if (page == PAGE_PHRASE) {
          // REC arme, la lecture arretee : le clavier pose les notes une a une
          // (une note, la ligne suivante) ; PLAY lance l'enregistrement.
          recVoie = voieCourante; recLibre = true;
          recIns = instrCible();
          if (!recIns) recIns = instrCanal[recVoie] ? instrCanal[recVoie] : instrId > 0 ? instrId : 1;
          recEtat = 3;
          fm1_annonce2("REC ARMED", "PLAY TO REC");
          modifie = true;
        }
        else if ((page == PAGE_SONG && curCanal < nbVisibles) || page == PAGE_CHAIN) {
          recVoie = page == PAGE_SONG ? reel(curCanal) : voieCourante;
          int sr = -1;
          if (page == PAGE_SONG) sr = curLigne;
          else for (int r = 0; r < MD_SONG_ROWS; r++)
            if (md_replayer_get_song(recVoie, r) == chainId) { sr = r; break; }
          if (sr < 0) fm1_annonce2("CHAIN", "NOT IN SONG");
          else {
            if (enLecture) { md_replayer_stop(); enLecture = false; }
            recIns = instrCible();
            recAvance.la = false; recAttente = false;
            recNbU = 0; recDerU = -1; recBoucle = 0; recVuPh = -1;
            const int ph0 = prepare(recVoie, sr, 0, false);
            // Rien a jouer la (un emplacement vide) : un instrument neuf, pose
            // dans la phrase, et c'est lui que le clavier enregistre.
            if (!recIns && ph0 >= 0) {
              uint8_t no, i2, ve, cm, cv, mc, mv;
              md_replayer_get_phrase((uint8_t)ph0, 0, &no,&i2,&ve,&cm,&cv,&mc,&mv);
              const int ins = instrLibre();
              md_replayer_init_default_instrument(ins - 1);
              md_replayer_set_phrase((uint8_t)ph0, 0, no, (uint8_t)ins, ve, cm, cv, mc, mv);
              recIns = ins;
            }
            if (!recIns) recIns = instrId > 0 ? instrId : 1;
            recLibre = false; recPh0 = ph0;
            recPrepareTemps();
            g_recLigne = sr;
            g_recTemps = -1; temps = -1;
            g_recProchain = fm1_echantillons();      // le premier temps, tout de suite
            recEtat = 1;
            g_recEtat = 1;
            modifie = true;
          }
        } else fm1_annonce2("REC", "SONG CHAIN PHRASE");
      }
      if (recEtat == 3 && enLecture) recLive();    // REC arme, puis PLAY
      if (recEtat) {
        const int t = g_recTemps;
        if (t != temps) {                          // 1, 2, 3, 4, puis REC
          temps = t;
          static const char *kNum[4] = { "1", "2", "3", "4" };
          if (t >= 0 && t < 4) fm1_annonce2(kNum[t], "GLO TO TAP TEMPO");   // (le tap tempo : GLO)
          else if (t == 4) fm1_annonce2("REC", 0);
        }
        if (recEtat == 1 && g_recEtat == 2) {      // le morceau est parti (dans l'interruption)
          recEtat = 2; enLecture = true; suiviLecture = true; recDerPos = -1; recPrepare = -1; lectureScope = 0;
        }
        if (recEtat == 2) {
          if (!md_replayer_is_playing() || !enLecture) arrete(false);   // (START l'a arrete)
          else {
            const int sr = md_replayer_play_song_row(recVoie), cr = md_replayer_play_chain_row(recVoie),
                      pr = md_replayer_play_phrase_row(recVoie);
            // A la moitie de la phrase, la suite est prete : la ligne suivante
            // du chain, ou la ligne suivante de SONG quand le chain est plein.
            const int ici = sr * MD_ROWS_PER_CHAIN + cr;
            // Une phrase commence : on la note (la premiere, ou celle preparee
            // d'avance, que REC a creee ; ou une deja la). Pendant le saut au
            // debut, c'est la boucle qui commence.
            if (!recLibre && g_recJouePh >= 0 && g_recJouePh != recVuPh) {
              recVuPh = g_recJouePh;
              if (recBoucle == 1) {
                // Revenu au debut : le silence de la fin s'en va, et on boucle
                // sur tout ce qui a ete joue (jusqu'a la derniere note).
                const int der = recDerU >= 0 ? recDerU : 0;
                recRetire(der + 1);
                if (recAvance.la) {
                  if (recAvance.phraseNeuve) md_replayer_set_chain((uint8_t)recAvance.ch, recAvance.cr, MD_EMPTY, 0);
                  if (recAvance.chainNeuf) md_replayer_set_song(recVoie, recAvance.sr, MD_EMPTY);
                  recAvance.la = false;
                }
                md_replayer_rec_boucle(recVoie, recU[der].sr, recU[der].cr, recU[0].sr, recU[0].cr);
                recBoucle = 2;
                fm1_annonce2("LOOP", 0);
                modifie = true;
              } else if (recBoucle == 0 && recNbU < 256 && sr >= 0 && cr >= 0) {
                UniteRec u = { sr, cr, g_recJouePh, md_replayer_get_song(recVoie, sr), false, false, 0 };
                if (recNbU == 0) { u.phN = recDebutPhN; u.chN = recDebutChN; }
                else if (recAvance.la && recAvance.ph == u.ph) {
                  u.phN = recAvance.phraseNeuve; u.chN = recAvance.chainNeuf; recAvance.la = false;
                }
                recU[recNbU++] = u;
              }
            }
            // Deux phrases de suite sans aucune note (apres la derniere note), la
            // seconde creee par REC : a sa fin, le moteur repart au debut.
            { const int k = recNbU - 1;
              if (!recLibre && recBoucle == 0 && k >= 1 && g_recJouePh == recU[k].ph &&
                  g_recJoueRow >= MD_ROWS_PER_PHRASE - 2 && recU[k].notes == 0 && recU[k - 1].notes == 0 &&
                  k - 1 > recDerU && recU[k].phN) {
                md_replayer_rec_boucle(recVoie, recU[k].sr, recU[k].cr, recU[0].sr, recU[0].cr);
                recBoucle = 1;
              } }
            if (!recLibre && recBoucle == 0 && sr >= 0 && cr >= 0 && pr >= MD_ROWS_PER_PHRASE / 2 && recPrepare != ici) {
              recPrepare = ici;
              if (cr + 1 < MD_ROWS_PER_CHAIN) prepare(recVoie, sr, cr + 1, true);
              else prepare(recVoie, sr + 1, 0, true);
              modifie = true;
            }
            // La frappe gardee pour la ligne suivante : elle a joue, on l'ecrit.
            if (recAttente && (int32_t)(g_recNbLignes - recAttN) >= 0) {
              recAttente = false;
              if (recAttPh >= 0 && recAttPh < MD_MAX_PHRASES && recAttRow >= 0 && recAttRow < MD_ROWS_PER_PHRASE) {
                uint8_t no, i2, ve, cm, cv, mc, mv;
                md_replayer_get_phrase((uint8_t)recAttPh, recAttRow, &no,&i2,&ve,&cm,&cv,&mc,&mv);
                md_replayer_set_phrase((uint8_t)recAttPh, recAttRow, (uint8_t)recAttNo, (uint8_t)recIns,
                                       ve, cm, cv, mc, mv);
                recCompteNote(recAttPh);
                modifie = true;
              }
            }
          }
        }
      }
      // Pendant le decompte, un tap tempo (GLO) change aussi son rythme.
      if (recEtat == 1) {
        const double bpm = md_replayer_get_bpm();
        g_recPeriode = (uint32_t)(fm1_echantillons_par_seconde() * 60.0 / (bpm > 1.0 ? bpm : 125.0));
      }
      // REC avec la DRUM MACHINE qui tourne (et le tracker qui joue) : chaque
      // coup s'ecrit dans le morceau, sur la piste de sa voie, a la ligne ou
      // il a joue (un chain, une phrase y sont crees s'il le faut) ; rejoue
      // ensuite par le tracker, il sonne pareil.
      { static uint32_t drumLu = 0;
        const uint32_t nb = g_drumNbCoups;
        if (recEtat != 2 || !g_drumJoue) drumLu = nb;
        if (nb - drumLu > 16u) drumLu = nb - 16u;
        while (drumLu != nb) {
          const uint32_t i = drumLu++ & 15u, coups = g_drumCoups[i];
          int sr = g_drumCoupsSr[i], cr = g_drumCoupsCr[i];
          const int pas = (int)g_drumCoupsPas[i];
          if (!coups || sr < 0 || cr < 0) continue;
          if (g_drumCoupsL[i] == 0) {                // (le moteur montrait deja la ligne 0 suivante)
            if (--cr < 0) {                          // ... et meme la ligne de SONG suivante :
              if (--sr < 0) continue;                // la derniere phrase du chain d'avant
              const uint8_t chr = md_replayer_get_song(g_drumCoupsRef[i], sr);
              cr = 0;
              for (int r = 0; chr != MD_EMPTY && r < MD_ROWS_PER_CHAIN; r++) {
                uint8_t p2; int8_t t2;
                md_replayer_get_chain(chr, r, &p2, &t2);
                if (p2 != MD_EMPTY) cr = r;
              }
            }
          }
          for (int k = 0; k < 11; k++) {
            if (!((coups >> k) & 1u)) continue;
            const int v = kDrumVoie[k];
            uint8_t ch = md_replayer_get_song(v, sr);
            if (ch == MD_EMPTY) {
              const int c = chainLibre();
              if (c < 0 || c >= MD_MAX_CHAINS) continue;
              ch = (uint8_t)c;
              md_replayer_set_song(v, sr, ch);
            }
            uint8_t ph; int8_t tsp;
            md_replayer_get_chain(ch, cr, &ph, &tsp);
            if (ph == MD_EMPTY) {
              const int f = phraseLibre();
              if (f < 0 || f >= MD_MAX_PHRASES) continue;
              ph = (uint8_t)f;
              md_replayer_set_chain(ch, cr, ph, 0);
            }
            uint8_t no, i2, ve, cm, cv, mc, mv;
            md_replayer_get_phrase(ph, pas, &no,&i2,&ve,&cm,&cv,&mc,&mv);
            md_replayer_set_phrase(ph, pas, (uint8_t)drumNote(md_replayer_get_module(), k), (uint8_t)(kDrumIns0 + k), ve, cm, cv, mc, mv);
          }
          modifie = true;
        } }
      // Les LED des boutons du tracker : forte quand sa page est ouverte ou
      // qu'il est actif, faible sinon (il sert). GLO, le tap tempo, est fort un
      // dixieme de seconde a chaque coup du metronome pendant REC.
      fm1_led_niveau(LED_REC, recEtat == 3 ? ((fm1_echantillons() / (fm1_echantillons_par_seconde() / 4u)) & 1u ? 2 : 1) : recEtat ? 2 : 1);   // (arme : clignote)
      if (fm1_looper() && (g_loopEtat == 1 || g_loopEtat == 3)) fm1_led_niveau(LED_REC, 2);   // (LOOPER : il enregistre)
      if (fm1_looper() && g_loopEtat == 5)          // (le decompte : il clignote avec)
        fm1_led_niveau(LED_REC, ((fm1_echantillons() - g_loopT0) % (fm1_echantillons_par_seconde() * 3u / 5u))
                                  < fm1_echantillons_par_seconde() / 8u ? 2 : 1);
      // GLO : fort en D-PAD, faible en KEYBOARD ; pendant REC, il clignote avec le metronome
      // (en KEYBOARD, il clignote, faible : on voit ou passer en D-PAD)
      fm1_led_niveau(LED_GLO, recEtat ? (fm1_echantillons() - g_recTic < fm1_echantillons_par_seconde() / 10u ? 2 : 1)
                                      : !fm1_clavier() ? 2
                                      : ((fm1_echantillons() / (fm1_echantillons_par_seconde() * 2u / 5u)) & 1u) ? 1 : 0);
      // SEQ : fort sur les pages de ses modes (le tracker, la DRUM MACHINE, le LOOPER), faible ailleurs
      fm1_led_niveau(11, page == PAGE_SONG || page == PAGE_CHAIN || page == PAGE_PHRASE || page == PAGE_INSTR ||
                         page == PAGE_TABLE || page == PAGE_DRUM || page == PAGE_LOOPER ? 2 : 1);
      fm1_led_niveau(LED_PLAY, 0);                 // (la rouge de la touche : eteinte)
      fm1_led_play_vert(enLecture ? 2 : 1);        // PLAY en vert : forte en lecture, faible a l'arret
      fm1_led_niveau(LED_HOME, page == PAGE_SONG ? 2 : 1);
      fm1_led_niveau(LED_SEL, page == PAGE_PRESET ? 2 : 1);
      fm1_led_niveau(LED_FX, page == PAGE_BEND || page == PAGE_BEND2 || page == PAGE_GLITCH || page == PAGE_MASTER || page == PAGE_GLOBAL ? 2 : 1);
      fm1_led_niveau(4, page == PAGE_AIDE ? 2 : 1);   // ENV (FM1_B_ENV) : l'aide
      { // le tempo du GLITCH : une ligne du tracker, en echantillons
        const double bpm = md_replayer_get_bpm();
        const int rpb = md_replayer_get_rows_per_beat() > 0 ? md_replayer_get_rows_per_beat() : 4;
        fm1_glitch_tempo((uint32_t)(fm1_echantillons_par_seconde() * 60.0 / ((bpm > 1.0 ? bpm : 125.0) * rpb)
                                    / deuxPuiss(bendHorloge / 1200.0))); }   // (CLOCK PITCH : la ligne, plus courte ou longue)
      fm1_led_niveau(LED_SAVE, dialogueNom ? 2 : 1);   // fort : la fenetre de sauvegarde ouverte
      // DRUM MACHINE : les noires, fort pour le son choisi ; les blanches, fort
      // sur ses pas mis, et le pas qui joue a l'inverse (on le voit passer).
      if (fm1_drum() && fm1_clavier()) {             // (en D-PAD, les touches sont des boutons)
        uint8_t niv[27];
        md_module_t *m = md_replayer_get_module();
        const uint16_t motif = m ? m->drum_pas[drumSel] : 0;
        for (int k = 0; k < 27; k++) niv[k] = 0;
        for (int j = 0; j < 11; j++) niv[kDrumNoire[j]] = j == drumSel ? 2 : 1;
        for (int j = 0; j < 16; j++) {
          const bool mis = (motif >> j) & 1u, ici = g_drumJoue && j == g_drumPas;
          niv[kDrumBlanche[j]] = (mis != ici) ? 2 : 0;
        }
        fm1_led_touches(niv);
      } else fm1_led_touches(0);
    }

    // ── Le PIANO : les touches du clavier qui ne sont pas des boutons ──────
    // Touche n = fa 3 + n demi-tons. EDIT allume, sur PHRASE : la note se pose
    // sous le curseur, qui descend d'une ligne. Sinon on JOUE l'instrument :
    //   PHRASE : celui de la ligne du curseur, sinon le plus proche au-dessus ;
    //   SONG   : celui de la premiere note du chain sous le curseur ;
    //   CHAIN  : celui de la premiere note de la phrase sous le curseur ;
    //   INSTR, TABLE : l'instrument de la page ;
    //   toute autre page (PROJECT, et celles a venir) : le dernier instrument choisi.
    {
      static int pianoTouche = -1, pianoVoie = -1, pianoNote = 0, pianoIns = 0;
      const u32 pianoAppuis = fm1_piano_appuis();
      // L'instrument de la premiere note d'une phrase (0 : aucun).
      auto instrDePhrase = [&](int ph, int depuis, int jusqua) {
        for (int l = depuis; l != jusqua; l += (jusqua > depuis ? 1 : -1)) {
          uint8_t no, i2, ve, cm, cv, mc, mv;
          md_replayer_get_phrase((uint8_t)ph, l, &no,&i2,&ve,&cm,&cv,&mc,&mv);
          if (i2) return (int)i2;
        }
        return 0;
      };
      auto instrDeChain = [&](int ch, int depuis) {
        for (int r = depuis; r < MD_ROWS_PER_CHAIN; r++) {
          uint8_t ph; int8_t tsp;
          md_replayer_get_chain((uint8_t)ch, r, &ph, &tsp);
          if (ph == MD_EMPTY) continue;
          if (const int i2 = instrDePhrase(ph, 0, MD_ROWS_PER_PHRASE)) return i2;
        }
        return 0;
      };
      for (int k = 0; k < 27; k++) {
        if (!(pianoAppuis & (1u << k))) continue;
        // LOOPER, le curseur sur STP/CUR : OP4, OP5, OP6, PIT, GLO, MONO (les touches noires apres OP3) choisissent
        // la rangee qu'il regle, de 1 a 6, de gauche a droite (elles clignotent ; aucune note)
        if (kRangeeTouche(k) >= 0 && page == PAGE_LOOPER && !fm1_clavier() && loopCurL == kLoopVoies + 1 && g_loopBtnC == 1) {
          loopLigneVue = kRangeeTouche(k);
          char l1[12]; siprintf(l1, "LAYER %d", loopLigneVue + 1);
          char l2[12]; siprintf(l2, "%d STEPS", g_loopPasL[loopLigneVue]);
          fm1_annonce2(l1, l2);
          modifie = true;
          continue;
        }
        if (fm1_drumkeys()) {                     // DRUM KEYBOARD : le son de la touche (un coup : la touche lachee, il finit seul)
          static int dkVoie = 0;
          const int ins = kDkIns0 + k, note = borne(kDk[k].note + pianoOct, 1, MD_MAX_NOTE), voie = 12 + dkVoie;
          dkVoie = (dkVoie + 1) & 3;
          fm1_voie_forte(voie);                    // (plus fort : la melodie les couvrait)
          md_replayer_play_test_note(note, ins, voie);
          if (fm1_looper()) loopNote(note, ins, true);
          continue;
        }
        if (fm1_drum() && !fm1_clavier()) continue;   // (DRUM MACHINE en D-PAD : pas de piano)
        if (fm1_drum()) {
          // DRUM MACHINE : une noire choisit son son, une blanche met ou enleve
          // ce son sur son pas ; aucune ne joue de note.
          md_module_t *m = md_replayer_get_module();
          for (int j = 0; j < 11 && m; j++)
            if (kDrumNoire[j] == k) {
              drumSel = j;
              fm1_annonce2(kDrumNom[j], 0);
              // rien ne joue (ni la boite, ni le tracker) : on entend le son choisi, sur sa voie (le son
              // d'avant de la boite s'arrete ; le pas suivant de la boite les coupe de meme)
              if (!g_drumJoue && !enLecture) {
                for (int v = 12; v < 18; v++)
                  if ((g_drumSonne >> v) & 1u) md_replayer_stop_test_note(v);
                g_drumSonne = 1u << kDrumVoie[j];
                md_replayer_play_test_note(drumNote(m, j), kDrumIns0 + j, kDrumVoie[j]);
              }
            }
          for (int j = 0; j < 16 && m; j++)
            if (kDrumBlanche[j] == k) {
              m->drum_pas[drumSel] ^= (uint16_t)(1u << j);
              modifie = true;
            }
          continue;
        }
        const int note = borne(42 + k + pianoOct, 1, MD_MAX_NOTE);   // fa 3 (F-3), decale par OCT
        int ins = 0, voie = voieCourante;
        if (recEtat) {
          // REC : l'instrument du clavier sur la piste enregistree ; et
          // pendant l'enregistrement, la note s'ecrit sur la ligne qui joue
          // (elle y remplace celle qui y etait).
          ins = recIns; voie = recVoie;
          const int rpb = md_replayer_get_rows_per_beat() > 0 ? md_replayer_get_rows_per_beat() : 4;
          const int32_t ligne = (int32_t)(g_recPeriode / (uint32_t)rpb);
          if (recEtat == 3) {
            // Arme, a l'arret, sur PHRASE : la note sur la ligne du curseur, et
            // le curseur descend d'une ligne.
            if (page == PAGE_PHRASE) {
              uint8_t no, i2, ve, cm, cv, mc, mv;
              md_replayer_get_phrase((uint8_t)phraseId, phLigne, &no,&i2,&ve,&cm,&cv,&mc,&mv);
              md_replayer_set_phrase((uint8_t)phraseId, phLigne, (uint8_t)note, (uint8_t)recIns, ve, cm, cv, mc, mv);
              if (phLigne < MD_ROWS_PER_PHRASE - 1) phLigne++;
              modifie = true;
            }
          } else if (recEtat == 1 && g_recEtat == 1 && g_recTemps == 3 && recPh0 >= 0) {
            // Juste avant le premier temps (la fin du decompte) : la premiere
            // note, pour la ligne 0 (elle manquait souvent).
            const int32_t reste = (int32_t)(g_recProchain - g_recAvance - fm1_echantillons());
            if (reste < ligne / 2) {
              recAttente = true; recAttPh = recPh0; recAttRow = 0; recAttNo = note; recAttN = 1;
            }
          } else if (g_recEtat == 2 && g_recJouePh < 0 && g_recPtrPh >= 0) {
            // Le morceau vient de partir, aucune ligne n'a encore joue : la premiere.
            recAttente = true; recAttPh = g_recPtrPh; recAttRow = g_recPtrRow; recAttNo = note;
            recAttN = g_recNbLignes + 1;
          }
          // La ligne LA PLUS PROCHE de la frappe : celle qui joue, celle
          // d'avant, ou la suivante. La frappe est datee sur l'horloge du son,
          // moins le retard de lecture des touches (une image d'ecran, ~8 ms en
          // moyenne). La suivante ne s'ecrit qu'une fois jouee (recEcrit,
          // plus bas) : la note ne sonne qu'une fois, au clavier.
          else if (g_recEtat == 2 && g_recJouePh >= 0) {
            const int32_t d = (int32_t)(fm1_echantillons() - fm1_echantillons_par_seconde() / 125u - g_recJoueEch);
            int ph = g_recJouePh, pr = g_recJoueRow;
            if (d < -ligne / 2 && g_recPrecPh >= 0) { ph = g_recPrecPh; pr = g_recPrecRow; }
            if (d >= ligne / 2) {
              recAttente = true; recAttPh = g_recPtrPh; recAttRow = g_recPtrRow;
              recAttNo = note; recAttN = g_recNbLignes + 1;
            } else if (ph >= 0 && ph < MD_MAX_PHRASES && pr >= 0 && pr < MD_ROWS_PER_PHRASE) {
              uint8_t no, i2, ve, cm, cv, mc, mv;
              md_replayer_get_phrase((uint8_t)ph, pr, &no,&i2,&ve,&cm,&cv,&mc,&mv);
              md_replayer_set_phrase((uint8_t)ph, pr, (uint8_t)note, (uint8_t)recIns, ve, cm, cv, mc, mv);
              recCompteNote(ph);
              modifie = true;
            }
          }
        } else if (page == PAGE_PRESET) {
          ins = presetIns; voie = presetVoie;      // l'instrument que la page regle
        } else if (page == PAGE_BEND || page == PAGE_BEND2 || page == PAGE_GLITCH || page == PAGE_MASTER || page == PAGE_GLOBAL) {
          voie = bendVoieClavier;                  // la piste du curseur, montree sur la page
          ins = instrCanal[voie] ? instrCanal[voie] : instrId;
        } else if (page == PAGE_LOOPER) {
          ins = instrCible();                      // le clavier de SONG
          if (curCanal < nbVisibles) voie = reel(curCanal);
        } else if (fm1_clavier() && (page == PAGE_SONG || page == PAGE_CHAIN || page == PAGE_PHRASE)) {
          // Mode KEYS : l'instrument le plus employe de ce que designe le
          // curseur, ou de ce qui joue sur sa colonne (instrCible).
          ins = instrCible();
          if (page == PAGE_SONG) voie = reel(curCanal);
        } else if (page == PAGE_PHRASE) {
          ins = instrDePhrase(phraseId, phLigne, -1);
        } else if (page == PAGE_SONG) {
          voie = reel(curCanal);
          const uint8_t v = md_replayer_get_song(voie, curLigne);
          if (v != MD_EMPTY) ins = instrDeChain(v, 0);
        } else if (page == PAGE_CHAIN) {
          uint8_t ph; int8_t tsp;
          md_replayer_get_chain((uint8_t)chainId, chLigne, &ph, &tsp);
          ins = ph != MD_EMPTY ? instrDePhrase(ph, 0, MD_ROWS_PER_PHRASE) : 0;
          if (!ins) ins = instrDeChain(chainId, 0);
        } else {
          ins = instrId;
        }
        // LOOPER : le clavier joue sur la voie de la couche qu'il enregistre (la suivante, quand la boucle tourne
        // sans enregistrer) : on l'entend exactement comme la boucle la rejouera (une autre voie gardait ce que
        // le morceau y avait laisse : la note sonnait bien plus fort en direct qu'en boucle)
        if (ins && fm1_looper())
          voie = kLoopVoie0 + (g_loopEtat == 2 || g_loopEtat == 4 ? g_loopCouche + 1 : g_loopCouche) % kLoopVoies;
        if (ins) {
          if (pianoVoie >= 0) md_replayer_stop_test_note(pianoVoie);
          md_replayer_play_test_note(note, ins, voie);
          if (fm1_looper()) {                      // (LOOPER, sur toute page : dans la boucle)
            if (pianoTouche >= 0) loopNote(pianoNote, pianoIns, false);
            loopNote(note, ins, true);
          }
          pianoTouche = k; pianoVoie = voie; pianoNote = note; pianoIns = ins;
        }
      }
      // La touche lachee coupe sa note.
      if (pianoTouche >= 0 && !(fm1_piano_tenues() & (1u << pianoTouche))) {
        md_replayer_stop_test_note(pianoVoie);
        if (fm1_looper()) loopNote(pianoNote, pianoIns, false);
        pianoTouche = -1; pianoVoie = -1;
      }
    }
#endif
    // ── La colonne « + » de SONG ──────────────────────────────────────────
    // A ajoute une piste (19, 20... : des samples seulement). A+B retire la
    // derniere, et SEULEMENT si elle est entierement vide. Ces touches ne
    // vont pas plus loin : la piste d'a cote n'a pas a les recevoir.
    if (page == PAGE_SONG && curCanal >= nbVisibles && !navigateur && !dialogueNom) {
      const bool retire = (frappe & (KEY_A | KEY_B)) && (keysHeld() & KEY_A) && (keysHeld() & KEY_B);
      if (retire) {
        const int der = md_replayer_get_num_channels() - 1;
        bool vide = true;
        for (int r = 0; r < MD_SONG_ROWS && vide; r++) vide = md_replayer_get_song(der, r) == MD_EMPTY;
        if (der >= MD_PISTES_OPL && vide) {
          md_replayer_set_num_channels(der);
          snprintf(msgProjet, sizeof msgProjet, "TRACK %02d REMOVED", der + 1);
          majVisibles(); curCanal = nbVisibles; pageVue = -1;
        } else strcpy(msgProjet, der < MD_PISTES_OPL ? "OPL3 TRACKS STAY" : "LAST TRACK IS NOT EMPTY");
        msgProjetJusqu = secondes + 2;
        keysIgnoreUntilReleased(KEY_A | KEY_B);
      } else if (frappe & KEY_A) {
        if (avecPlus()) {
          md_replayer_set_num_channels(md_replayer_get_num_channels() + 1);
          snprintf(msgProjet, sizeof msgProjet, "TRACK %02d ADDED (SAMPLES)", md_replayer_get_num_channels());
          msgProjetJusqu = secondes + 2;
          majVisibles(); pageVue = -1;
        }
        keysIgnoreUntilReleased(KEY_A | KEY_B);
      }
      appui &= ~(KEY_A | KEY_B);   // rien d'autre ne doit les recevoir
      frappe &= ~(KEY_A | KEY_B);
    }
    // L et R valent SELECT : sur une DS, SELECT est mal place pour un accord
    // tenu, alors que les gachettes tombent sous les index.
    const bool selTenu = (keysHeld() & (KEY_SELECT | KEY_L | KEY_R)) != 0;
    const int relache = keysUp();


    // L'accord SELECT+B reste valable tant que SELECT est tenu, meme si B a
    // deja ete relache : sinon il faudrait garder trois doigts poses pour le
    // clone profond. C'est ce que fait la reference.
    static bool accordSelB = false;
    if (!selTenu) accordSelB = false;

    // B sert a trois choses : armer une selection avec SELECT, sauter des
    // lignes ou regler le tempo avec la croix, et copier tout seul. On ne peut
    // donc pas copier des l'APPUI — on copie au RELACHEMENT, et seulement si
    // aucune direction n'a ete pressee entre-temps.
    static bool bUtilise = false;
    if (keysHeld() & KEY_B) {
      if (appui & (KEY_UP|KEY_DOWN|KEY_LEFT|KEY_RIGHT)) bUtilise = true;
    }
    if (frappe & KEY_B) bUtilise = false;

    if (selTenu) {
      // SELECT + croix : on change de PAGE.
      // Rangee du bas : SONG - CHAIN - PHRASE. SELECT + haut monte a PROJECT,
      // SELECT + bas en redescend.
      static int derniereRangee = PAGE_SONG;
#ifdef FM1
      // FM-1, en D-PAD : la DRUM MACHINE est sous la rangee (SELECT + bas), et SELECT + haut ou gauche en
      // remonte (la page d'ou l'on venait). En mode DRUM MACHINE (SEQ), elle s'ouvre seule.
      if (!fm1_clavier()) {
        // (fm1_opaque : sans lui, le test de KEY_DOWN, le bit 7, devient un sextra, que l'emulateur en ligne ne lit pas)
        if ((fm1_opaque((uint32_t)frappe) & KEY_DOWN) && page != PAGE_PROJECT && page != PAGE_DRUM && page <= PAGE_TABLE) {
          drumPrepare(); drumAvant = page; page = PAGE_DRUM; pageVue = -1;
          frappe &= ~KEY_DOWN;
        } else if ((frappe & (KEY_UP | KEY_LEFT)) && page == PAGE_DRUM) {
          page = drumAvant; pageVue = -1;
          frappe &= ~(KEY_UP | KEY_LEFT);
        }
      }
#endif
      if (frappe & KEY_UP)   { if (page != PAGE_PROJECT) { derniereRangee = page; menuProjet = 0; }   // (le curseur en haut, sur TEMPO)
                               page = PAGE_PROJECT; }
      if (frappe & KEY_DOWN) {
        if (page == PAGE_PROJECT) {
          // Quitter la page REPOND a la question restee en suspens, par la
          // reponse prudente : « non » pour un nouveau morceau.
          demandeNouveau = false;
          page = derniereRangee;
        }
      }
#ifdef FM1
      // FM-1 : SELECT + gauche, depuis PRESET ou CIRCUIT BENDING, revient a SONG (comme HOME).
      if ((frappe & KEY_LEFT) && (page == PAGE_PRESET || page == PAGE_BEND || page == PAGE_BEND2 || page == PAGE_GLITCH || page == PAGE_MASTER ||
                                  page == PAGE_GLOBAL)) {
        page = PAGE_SONG; pageVue = -1;
        frappe &= ~KEY_LEFT;
      }
      // FM-1 : SELECT + gauche, depuis PROJECT, revient a SONG.
      if ((frappe & KEY_LEFT) && page == PAGE_PROJECT && !navigateur && !dialogueNom) {
        demandeNouveau = false;
        page = PAGE_SONG;
        frappe &= ~KEY_LEFT;                      // (SONG n'a pas a le recevoir aussi)
      }
#endif
#ifdef FM1
      // FM-1, INSTR OPL3 : SELECT + droite / gauche passent d'une page a
      // l'autre ; depuis la derniere, droite descend dans la TABLE (sur sa
      // ligne, comme avant) ; depuis la premiere, gauche remonte a PHRASE.
      if (page == PAGE_INSTR && genreInstr(instrId) == GENRE_OPL) {
        if ((frappe & KEY_RIGHT) && inPage < kInPages - 1) {
          inPage++; inItem = 0; pageVue = -1; frappe &= ~KEY_RIGHT;
        }
        if ((frappe & KEY_LEFT) && inPage > 0) {
          inPage--; inItem = 0; pageVue = -1; frappe &= ~KEY_LEFT;
        }
      }
#endif
      if (page != PAGE_PROJECT) {
        // SELECT + droite = drillIn : on DESCEND DANS la case pointee, et on
        // refuse si elle est vide. C'est ce que fait LSDJ ; passer betement a
        // l'ecran suivant afficherait toujours le chain 00.
#ifdef FM1
        // FM-1 : pas de descente tant que A ou B est tenu, ni dans la demi-
        // seconde qui suit un collage ou un clonage (SELECT + B + A) : la touche
        // droite (le la) touche presque le SELECT de gauche, et on entrait dans
        // le clone qu'on venait de faire.
        if ((keysHeld() & (KEY_A | KEY_B)) ||
            (collageEch && fm1_echantillons() - collageEch < fm1_echantillons_par_seconde() / 2u))
          frappe &= ~KEY_RIGHT;
#endif
        if (frappe & KEY_RIGHT) {
          if (page == PAGE_SONG) {
            uint8_t v = md_replayer_get_song(reel(curCanal), curLigne);
            // On retient la COLONNE : c'est elle qui sonnera en solo depuis
            // CHAIN et PHRASE. Prendre le curseur SONG du moment faisait jouer
            // une AUTRE voie que celle qu'on edite — sur les colonnes PSG et
            // bruit, on n'entendait donc rien.
            if (v != MD_EMPTY) { chainId = v; chLigne = 0; chCol = 0;
                                 voieCourante = reel(curCanal);
                                 page = PAGE_CHAIN; }
          } else if (page == PAGE_CHAIN) {
            uint8_t ph; int8_t tsp; md_replayer_get_chain(chainId, chLigne, &ph, &tsp);
            if (ph != MD_EMPTY) { phraseId = ph; phLigne = 0; phCol = 0; page = PAGE_PHRASE; }
          } else if (page == PAGE_PHRASE) {
            uint8_t no,i2,vel,cmd,cv,mc,mv;
            md_replayer_get_phrase(phraseId, phLigne, &no,&i2,&vel,&cmd,&cv,&mc,&mv);
            // La page d'instrument depend de la COLONNE d'ou l'on descend,
            // pas de l'instrument : un meme instrument pose sur une colonne FM
            // ou PSG ne pilote pas la meme puce, et on ne montre que les
            // reglages qui agissent reellement. C'est la regle de l'iPad.
            if (i2) { instrId = i2; instrVoie = reel(curCanal);
                      inLigne = 0; inCol = 0; page = PAGE_INSTR; }
          } else if (page == PAGE_INSTR) {
#ifdef FM1
            // FM-1 : seulement depuis la valeur TBL, et seulement si une
            // table y est choisie : on descend dans CETTE table. Ailleurs, ou
            // sur OFF, on ne bouge pas (comme une case vide sur SONG).
            const int t = md_replayer_get_instr_table(instrId);
            if (inLigne == champTableDe(instrId) && t >= 0 && t != MD_EMPTY) {
              tableId = t;
              tabLigne = 0; tabCol = 0;
              page = PAGE_TABLE; pageVue = -1;
            }
#else
            // La page « T » de LSDJ : la table de l'instrument, accessible
            // depuis N'IMPORTE QUELLE page d'instrument — FM, PCM, PSG, bruit.
            tableId = tableDeLInstrument(instrId);
            tabLigne = 0; tabCol = 0;
            page = PAGE_TABLE; pageVue = -1;
#endif
          }
        }
        // SELECT + gauche = drillOut : on remonte d'un cran. Depuis la TABLE
        // on revient a l'instrument dont elle depend, pas a la page d'avant.
        if ((frappe & KEY_LEFT) && page == PAGE_TABLE) { page = PAGE_INSTR;
                                                         pageVue = -1; }
        else if ((frappe & KEY_LEFT) && page > PAGE_SONG && page < PAGE_PROJECT)
          page--;
        if (frappe & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN)) selActive = false;

        // SELECT + B : ARME une selection. Il ne la bascule pas — sinon,
        // SELECT reste tenu et chaque B annulerait celle qu'on vient de faire.
        if ((frappe & KEY_B) && page <= PAGE_PHRASE) {
          if (!selActive) {
            selActive = true;
            selAncreLigne = curL(); selAncreCol = curC();
          }
          accordSelB = true;
          bUtilise = true;    // ce B-la n'est pas une copie
        }
        // SELECT + A : coller. Avec une selection armee et l'accord B en
        // cours, c'est le clone profond.
        if ((frappe & KEY_A) && page <= PAGE_PHRASE) {
          if (selActive && (accordSelB || (keysHeld() & KEY_B))) {
            const bool clone = !(page == PAGE_PHRASE && phCol != 1);   // (sur PHRASE, seulement la colonne INSTRUMENT)
            cloneProfond();
#ifdef FM1
            if (clone) fm1_annonce2("DEEP CLONED", 0);
#endif
          } else {
            const bool colle = clipPage == page && clipLignes > 0;
            coller();
#ifdef FM1
            if (colle) fm1_annonce2("PASTED", 0);
#endif
          }
#ifdef FM1
          collageEch = fm1_echantillons() | 1u;     // (SELECT + droite n'y entre pas tout de suite)
#endif
        }
      }
    } else {
      const bool aTenu = (keysHeld() & KEY_A) != 0;

      // Le relachement de A coupe la note d'audition, sur la voie ou elle a
      // ete jouee.
      if ((relache & KEY_A) && voieAudition >= 0) {
        md_replayer_stop_test_note(voieAudition);
        voieAudition = -1;
      }
      if (relache & KEY_A) {
        notePoseeParA_ph = -1;       // l'accord eventuel est fini
        // Une audition demandee mais pas encore partie n'a plus lieu d'etre :
        // sans ca, elle demarrerait APRES le relachement et ne s'arreterait
        // jamais.
        auditionVoie = -1;
      }

      // B relache sans avoir servi a autre chose : on copie la selection.
      // Sans selection armee, il ne se passe rien — c'est ce que fait
      // copySelection() cote iPad, qui sort sur « NOTHING SELECTED ».
      if ((relache & KEY_B) && !bUtilise && page <= PAGE_PHRASE) {
        const bool copie = selActive;
        copier();
#ifdef FM1
        if (copie) fm1_annonce2("COPIED", 0);
#endif
      }

      // ── B + haut/bas : le grand saut dans SONG ─────────────────────────
      // Et RIEN D'AUTRE. B + croix reglait aussi le tempo depuis n'importe
      // quelle page : on changeait le BPM du morceau par accident en voulant
      // faire tout autre chose. Le tempo se regle dans PROJECT, la ou il est
      // ecrit.
      if ((keysHeld() & KEY_B) &&
          (appui & (KEY_UP|KEY_DOWN|KEY_LEFT|KEY_RIGHT))) {
        if (page == PAGE_SONG && (appui & (KEY_UP|KEY_DOWN))) {
          // Sur SONG, B + haut/bas fait un GRAND saut : seize lignes d'un
          // coup, soit une page hexadecimale. La chanson en compte 256, les
          // parcourir ligne par ligne etait interminable.
          //
          // Le signe est CALCULE ICI, pas repris de `sens`. Celui-la sert au
          // reglage d'une valeur, ou haut veut dire PLUS ; ici on se DEPLACE,
          // et la ligne 00 est en haut de l'ecran : monter, c'est diminuer le
          // numero. Partager le meme signe inversait les deux directions.
          curLigne = borne(curLigne + ((appui & KEY_UP) ? -16 : 16),
                           0, MD_SONG_ROWS - 1);
        }
      } else

      // ── A + croix : modifier la valeur sous le curseur ─────────────────
      // Gauche/droite = pas de 1, haut/bas = grand pas. Comme sur l'iPad.
      // La page PROJECT est EXCLUE d'ici : elle a ses propres reglages a la
      // croix — le tempo, les lettres du nom de fichier — et elle les traite
      // dans sa branche, plus bas. Sans cette exclusion, tenir A et presser une
      // fleche entrait ici, ne trouvait aucune page correspondante, et sautait
      // toute la branche PROJECT : ni le tempo ni le choix des lettres ne
      // repondaient.
      if (page != PAGE_PROJECT &&
          aTenu && (appui & (KEY_UP|KEY_DOWN|KEY_LEFT|KEY_RIGHT))) {
        const int sens = (appui & (KEY_UP|KEY_RIGHT)) ? 1 : -1;
        const bool grand = (appui & (KEY_UP|KEY_DOWN)) != 0;
        // ── TOUTE LA SELECTION, ET NON LA SEULE CASE DU CURSEUR ────────────
        // Avec une selection armee (SELECT+B), A + croix doit modifier CHAQUE
        // case selectionnee, sur toutes les colonnes : notes, instruments,
        // velocites, commandes, CC. On promene donc le curseur sur la plage et
        // on rejoue le meme reglage case par case, puis on le remet ou il
        // etait. Sans cette boucle, une seule case bougeait.
        int selL0 = curL(), selL1 = curL(), selC0 = curC(), selC1 = curC();
        if (selActive && page <= PAGE_PHRASE) {
          selL0 = (selAncreLigne < curL()) ? selAncreLigne : curL();
          selL1 = (selAncreLigne < curL()) ? curL() : selAncreLigne;
          selC0 = (selAncreCol   < curC()) ? selAncreCol   : curC();
          selC1 = (selAncreCol   < curC()) ? curC()        : selAncreCol;
        }
        const bool multi = (selL0 != selL1 || selC0 != selC1);
        const int sauveCurL = curLigne, sauveCurC = curCanal;
        const int sauveChL = chLigne, sauveChC = chCol;
        const int sauvePhL = phLigne, sauvePhC = phCol;
        for (int selL = selL0; selL <= selL1; selL++)
        for (int selC = selC0; selC <= selC1; selC++) {
        if (page <= PAGE_PHRASE) poseCurseur(selL, selC);
        if (page == PAGE_SONG) {
          uint8_t v = md_replayer_get_song(reel(curCanal), curLigne);
          if (v != MD_EMPTY) {
            int n = borne((int)v + sens * (grand ? 16 : 1), 0, MD_MAX_CHAINS - 1);
            md_replayer_set_song(reel(curCanal), curLigne, (uint8_t)n); dernierChain = n;
          } else if (sens > 0 && !multi) md_replayer_set_song(reel(curCanal), curLigne, 0);
        } else if (page == PAGE_CHAIN) {
          {
            const uint8_t noChain = (uint8_t)chainId;
            uint8_t ph; int8_t tsp; md_replayer_get_chain(noChain, chLigne, &ph, &tsp);
            // Sur une plage, une rangee sans phrase reste vide (voir PHRASE).
            if (multi && ph == MD_EMPTY) continue;
            if (chCol == 0) {
              int n = (ph == MD_EMPTY ? -1 : (int)ph) + sens * (grand ? 16 : 1);
              if (n < 0) md_replayer_set_chain(noChain, chLigne, MD_EMPTY, tsp);
              else { n = borne(n, 0, MD_MAX_PHRASES - 1);
                     md_replayer_set_chain(noChain, chLigne, (uint8_t)n, tsp);
                     dernierePhrase = n; }
            } else {
              int t = borne((int)tsp + sens * (grand ? 12 : 1), -128, 127);
              md_replayer_set_chain(noChain, chLigne, ph, (int8_t)t);
            }
          }
#ifdef FM1
        } else if (page == PAGE_INSTR && instrSansChamp) {
          // La ligne OP : A + gauche / droite, MODULATOR ou CARRIER. (Un
          // preset ne se regle pas : A l'applique.)
          ElInstr el[40];
          elementsInstr(inPage, el);
          if (el[inItem].genre == EL_OP) inOp = sens > 0 ? 1 : 0;
#endif
        } else if (page == PAGE_INSTR) {
          const int ins = instrId;
          const int pas = sens * (grand ? 16 : 1);
          if (genreInstr(ins) == GENRE_OPL) {
            const ChampOpl &ch = kChampsOp[inLigne];
            const int octet = ch.octet + (ch.parOp ? inCol : 0);
            switch (ch.aff) {
              case AFF_TYPE:          // OPL3 -> SAMPLE
                if (sens > 0) { md_replayer_set_instr_kind(ins, MD_INSTR_KIND_PCM);
                                inLigne = 0; inCol = 0; }
                break;
              case AFF_PAN: {         // L, C ou R, rien d'autre
                const int p = md_replayer_get_instr_gen_val(ins, MD_GEN_PROP_PANNING);
                const int idx = borne((p < 85 ? 0 : p > 170 ? 2 : 1) + sens, 0, 2);
                static const int kPan[3] = { 0, 128, 255 };
                md_replayer_set_instr_gen_val(ins, MD_GEN_PROP_PANNING, kPan[idx]);
              } break;
              case AFF_FTUNE:
                md_replayer_set_instr_gen_val(ins, MD_GEN_PROP_FINE_TUNE,
                    borne(md_replayer_get_instr_gen_val(ins, MD_GEN_PROP_FINE_TUNE)
                          + pas, -128, 127));
                break;
              case AFF_TABLE: changeTable(ins, sens); break;
              case AFF_KSL: {         // dans l'ordre d'AT2
                const int v = oplLit(ins, octet, ch.decalage, ch.masque);
                int k = 0; while (k < 3 && kKslOrdre[k] != v) k++;
                oplEcrit(ins, octet, ch.decalage, ch.masque, kKslOrdre[borne(k + sens, 0, 3)]);
              } break;
              case AFF_SORTIE: {      // OUTPUT LEVEL : 3F = fort, l'inverse du TL
                const int sortie = 63 - oplLit(ins, octet, ch.decalage, ch.masque);
                oplEcrit(ins, octet, ch.decalage, ch.masque,
                         63 - borne(sortie + sens * (grand ? 8 : 1), 0, 63));
              } break;
              case AFF_INV:           // affiche masque - valeur : on regle dans ce sens-la
                oplEcrit(ins, octet, ch.decalage, ch.masque,
                         ch.masque - borne(ch.masque - oplLit(ins, octet, ch.decalage, ch.masque)
                                           + sens * (grand ? 4 : 1), 0, ch.masque));
                break;
              default:
                oplEcrit(ins, octet, ch.decalage, ch.masque,
                         borne(oplLit(ins, octet, ch.decalage, ch.masque)
                               + sens * (grand ? 4 : 1), 0, ch.masque));
            }
            // On l'ENTEND a chaque cran, a l'arret : un son FM se regle a
            // l'oreille.
#ifndef FM1   // FM-1 : pas de son sans le clavier ou la lecture (un potard regle aussi ces valeurs)
            if (!enLecture && inLigne != kOplRangeeTable && inLigne != 0) {
              voieAudition = instrVoie;
              md_replayer_play_test_note(49, ins, voieAudition);
            }
#endif
          } else if (inLigne == 0) {
            // Le TYPE, en tete des trois pages : SAMPLE, MIDI, KICK. La croix
            // les parcourt dans cet ordre et s'arrete aux deux bouts.
            // Sur la RG350 : OPL3 ou SAMPLE. L'OPL3 n'a que dix-huit voies :
            // sur les pistes 19 et plus, seul un SAMPLE peut jouer.
            if (sens < 0) {
              if (instrVoie >= OPL_NB_PISTES) {
                strcpy(msgProjet, "TRACK 19+ : SAMPLES ONLY");
                msgProjetJusqu = secondes + 3;
              } else {
                md_replayer_set_instr_kind(ins, MD_INSTR_KIND_OPL);
                inLigne = 0; inCol = 0;
              }
            }
            // Un KICK qui vient d'etre choisi est fabrique tout de suite,
            // sinon l'instrument n'aurait aucun son.
            if (genreInstr(ins) == GENRE_KICK &&
                md_replayer_rend_kick(ins) == -2) kickARefaire = ins;
          } else if (genreInstr(ins) == GENRE_MIDI) {
            //   0 TYPE   1 CHANNEL   2 PROGRAM   3 VOLUME   4 LENGTH   5 TABLE
            if (inLigne == 1) {
              md_replayer_set_instr_midi_channel(ins,
                  borne(md_replayer_get_instr_midi_channel(ins)
                        + sens * (grand ? 4 : 1), 0, 15));
            } else if (inLigne == 2) {
              // Sous 00 on revient sur « -- » : l'instrument n'envoie alors
              // aucun changement de programme.
              const int p = md_replayer_get_instr_midi_program(ins);
              const int n = (p == MD_EMPTY ? -1 : p) + pas;
              md_replayer_set_instr_midi_program(ins,
                  n < 0 ? MD_EMPTY : borne(n, 0, 127));
            } else if (inLigne == 3) {
              md_replayer_set_instr_pcm_volume(ins,
                  borne(md_replayer_get_instr_pcm_volume(ins) + pas, 0, 255));
            } else if (inLigne == 4) {
              // Duree des notes en ticks ; sous 01 on revient sur AUTO.
              md_replayer_set_instr_midi_length(ins,
                  borne(md_replayer_get_instr_midi_length(ins) + pas, 0, 255));
            } else if (inLigne == 5) {
              changeTable(ins, sens);           // la table, en bas comme ailleurs
            } else {
              // SYNC : le retard des echantillons, en millisecondes. Reglage
              // GLOBAL, retenu dans XMTRACKER.CFG.
              md_replayer_set_sync_ms(borne(md_replayer_get_sync_ms()
                                            + sens * (grand ? 10 : 1), 0, 100));
              ecrisCfg();
            }
          } else if (genreInstr(ins) == GENRE_KICK) {
            //   0 TYPE  1 DROP  2 TIME  3 DECAY  4 CLICK  5 DRIVE
            //   6 VOLUME  7 OUTPUT  8 TABLE
            if (inLigne >= 1 && inLigne <= 5) {
              // Regler un bouton refabrique le coup dans la banque, dans la
              // zone que personne ne lit. Si les deux sonnent encore, on
              // redemande a chaque image : le son n'est jamais coupe.
              const int quoi = inLigne - 1;
              md_replayer_set_instr_kick_val(ins, quoi,
                  borne(md_replayer_get_instr_kick_val(ins, quoi) + pas, 0, 255));
              if (md_replayer_rend_kick(ins) == -2) kickARefaire = ins;
              else kickARefaire = 0;
            } else if (inLigne == 6) {
              md_replayer_set_instr_pcm_volume(ins,
                  borne(md_replayer_get_instr_pcm_volume(ins) + pas, 0, 255));
            } else if (inLigne == 7) {
              md_replayer_set_instr_gen_val(ins, MD_GEN_PROP_PANNING,
                  borne(md_replayer_get_instr_gen_val(ins, MD_GEN_PROP_PANNING)
                        + pas, 0, 255));
            } else {
              changeTable(ins, sens);           // la table, en bas comme ailleurs
            }
            // On l'ENTEND a chaque cran, a l'arret : un kick se regle a
            // l'oreille, pas au chiffre.
#ifndef FM1   // FM-1 : pas de son sans le clavier ou la lecture
            if (!enLecture && inLigne >= 1 && inLigne <= 6) {
              voieAudition = instrVoie;
              md_replayer_play_test_note(49, ins, voieAudition);
            }
#endif
          } else {
            // Meme ordre que l'affichage. « LOAD SAMPLE » ne reagit pas a la
            // croix : c'est A qui l'actionne, plus bas.
            //   0 TYPE  1 LOAD SAMPLE  2 SAMPLE  3 BASE NOTE  4 LOOP
            //   5 VOLUME  6 OUTPUT  7 TABLE
            const int ech = md_replayer_get_instr_sample(ins);
            if (inLigne == 2) {
              // A + gauche/droite feuillette, par ordre alphabetique, les WAV
              // du dossier d'ou vient le dernier son charge. On s'arrete aux
              // deux bouts : retirer le son se fait avec X, pas en tombant
              // par megarde sur « aucun ».
              lisCfg();
              if (!echScanne) scanneEch();
              const int echOk = (ech >= 0 && ech < MD_MAX_SAMPLES) ? ech : -1;
              const int cour = rangEch(ins);
              if (echNb == 0) {
                strcpy(msgPCM, "NO WAV HERE");
              } else {
                const int p2 = (cour < 0) ? (sens > 0 ? 0 : echNb - 1)
                                          : borne(cour + sens, 0, echNb - 1);
                int idx = -1;
                if (p2 == cour) {
                  idx = echOk;                   // au bout de la liste : on reste
                } else {
                  echSel = p2;
                  // Deja en banque ? On s'y rebranche.
                  for (int i3 = 0; i3 < MD_MAX_SAMPLES && idx < 0; i3++) {
                    char nm[24]; uint32_t lg2; int bcl2, base2;
                    if (md_replayer_get_sample(i3, nm, 24, &lg2, &bcl2, &base2) &&
                        !strncasecmp(nm, echFic[p2], 23)) idx = i3;
                  }
                  if (idx < 0) {
                    // Le son qu'on quitte rend sa place s'il ne sert a rien
                    // d'autre : sans ca, feuilleter un dossier remplirait la
                    // banque en quelques pas.
                    if (echOk >= 0) md_replayer_libere_sample(echOk, ins);
                    char chemin[224];
                    const int Ld2 = (int)strlen(dossierSamples);
                    siprintf(chemin, "%s%s%s", dossierSamples,
                             (Ld2 && dossierSamples[Ld2-1] == '/') ? "" : "/",
                             echFic[p2]);
                    // Decode directement dans la banque : jamais deux copies.
                    uint32_t capW = 0, imW = 0, hzW = 0;
                    int bitsW = 8, noteW = 49; int32_t bclW = -1;
                    uint8_t *zoneW = md_replayer_zone_libre(&capW);
                    const int rW = zoneW ? md_audio_charge_wav(chemin, zoneW, capW,
                                               &imW, &hzW, &bitsW, &bclW, &noteW)
                                         : 2;
                    if (rW == 0) {
                      idx = md_replayer_add_sample(echFic[p2], zoneW, imW, bitsW,
                                                   hzW, bclW, noteW);
                      strcpy(msgPCM, idx >= 0 ? "LOADED" : "BANK FULL");
                    } else strcpy(msgPCM, rW == 2 ? "BANK FULL" : "BAD WAV");
                    // Rien de charge : l'instrument ne doit pas rester branche
                    // sur une place qu'on vient de rendre.
                    if (idx < 0 && echOk >= 0) {
                      char nm[24]; uint32_t l3; int b3, n3;
                      if (!md_replayer_get_sample(echOk, nm, 24, &l3, &b3, &n3))
                        md_replayer_set_instr_sample(ins, MD_EMPTY);
                    }
                  }
                }
                if (idx >= 0) {
                  md_replayer_set_instr_sample(ins, idx);
                  md_replayer_set_instr_kind(ins, MD_INSTR_KIND_PCM);
                  // On l'ENTEND en le choisissant, a sa note de reference, et a
                  // l'arret seulement : une audition par-dessus la lecture
                  // volerait une voie au morceau.
#ifdef FM1
                  if (false) {   // FM-1 : pas de son sans le clavier ou la lecture
#else
                  if (!enLecture) {
#endif
                    char nm[24]; uint32_t l4; int b4, noteRef = 49;
                    md_replayer_get_sample(idx, nm, 24, &l4, &b4, &noteRef);
                    voieAudition = instrVoie;
                    md_replayer_play_test_note(noteRef, ins, voieAudition);
                  }
                }
              }
            } else if (inLigne == 5) {
              md_replayer_set_instr_pcm_volume(ins,
                  borne(md_replayer_get_instr_pcm_volume(ins) + pas, 0, 255));
            } else if (inLigne == 6) {
              // Pas de 1 a gauche/droite, de 16 en haut/bas.
              md_replayer_set_instr_gen_val(ins, MD_GEN_PROP_PANNING,
                  borne(md_replayer_get_instr_gen_val(ins, MD_GEN_PROP_PANNING)
                        + pas, 0, 255));
            } else if (inLigne == 7) {
              md_replayer_set_instr_disto(ins, md_replayer_get_instr_disto(ins) + pas);
            } else if (inLigne == 8) {
              changeTable(ins, sens);           // la table, en bas comme ailleurs
            } else if (ech >= 0 && (inLigne == 3 || inLigne == 4)) {
              // Ces deux-la appartiennent a l'ECHANTILLON, pas a l'instrument :
              // sans echantillon charge, il n'y a rien a regler.
              char nm[24]; uint32_t lg = 0; int bcl = -1, base = 60;
              md_replayer_get_sample(ech, nm, 23, &lg, &bcl, &base);
              if (inLigne == 3)
                md_replayer_set_sample_base(ech,
                    borne(base + sens * (grand ? 12 : 1), 1, MD_MAX_NOTE));
              else
                md_replayer_set_sample_loop(ech,
                    borne(bcl + sens * (grand ? 1024 : 16), -1, (int)lg));
            }
          }
        } else if (page == PAGE_TABLE) {
          uint8_t vol,c1,v1,c2,v2,mc,mv; int8_t tsp;
          md_replayer_get_table_row(tableId, tabLigne, &vol, &tsp,
                                    &c1, &v1, &c2, &v2, &mc, &mv);
          const int nbc = md_table_cmd_count();
          // Chaque arret ne modifie QUE ce qu'il montre. La liste des lettres
          // est bornee par le moteur : sans borne haute on posait un indice
          // inexistant, la case affichait « ? » et la commande ne faisait rien.
          switch (tabCol) {
            case 0: vol = (uint8_t)borne((int)vol + sens * (grand ? 16 : 1), 0, 255); break;
            case 1: tsp = (int8_t)borne((int)tsp + sens * (grand ? 12 : 1), -128, 127); break;
            case 2: { int n = (c1 == MD_EMPTY ? -1 : (int)c1) + sens;
                      if (n >= nbc) n = nbc - 1;
                      c1 = (n < 0) ? MD_EMPTY : (uint8_t)n; } break;
            case 3: v1 = (uint8_t)borne((int)v1 + sens * (grand ? 16 : 1), 0, 255); break;
            case 4: { int n = (c2 == MD_EMPTY ? -1 : (int)c2) + sens;
                      if (n >= nbc) n = nbc - 1;
                      c2 = (n < 0) ? MD_EMPTY : (uint8_t)n; } break;
            case 5: v2 = (uint8_t)borne((int)v2 + sens * (grand ? 16 : 1), 0, 255); break;
            // Le CC MIDI : son numero puis sa valeur, tous deux bornes a 7F.
            // Depuis une case vide, droite donne CC 01 : on part du debut.
            case 6: { int n = (mc == MD_EMPTY ? 0 : (int)mc)
                              + sens * (grand ? 16 : 1);
                      mc = (n < 0) ? MD_EMPTY : (uint8_t)borne(n, 0, 127); } break;
            default: mv = (uint8_t)borne((int)mv + sens * (grand ? 16 : 1), 0, 127); break;
          }
          md_replayer_set_table_row(tableId, tabLigne, vol, tsp,
                                    c1, v1, c2, v2, mc, mv);
          // Le CC part a chaque cran, sur le canal de l'instrument de la table.
          if (tabCol >= 6 && mc != MD_EMPTY) envoieCC(instrId, mc, mv);
        } else if (page == PAGE_PHRASE) {
          {
            const uint8_t ph = (uint8_t)phraseId;
            uint8_t no,ins,vel,cmd,cv,mc,mv;
            md_replayer_get_phrase(ph, phLigne, &no,&ins,&vel,&cmd,&cv,&mc,&mv);
            // Les QUATRE colonnes CC occupent les positions 5 a 12 : pour
            // chacune, un numero puis sa valeur.
            const int kCC = (phCol >= 5) ? (phCol - 5) / 2 : -1;
            const bool surNumero = (phCol >= 5) && ((phCol - 5) % 2 == 0);
            uint8_t ccN = MD_EMPTY, ccV = 0;
            if (kCC >= 0) md_replayer_get_phrase_cc(ph, phLigne, kCC, &ccN, &ccV);
            // ── SUR UNE PLAGE, LES CASES VIDES RESTENT VIDES ─────────────────
            // Transposer une selection ne doit toucher QUE ce qui y est deja.
            // Une case de note vide prenait la « derniere note » (notePourCase),
            // et A + croix remplissait tous les trous de la plage. Sur une case
            // seule, en revanche, A + croix continue de poser une valeur.
            if (multi) {
              bool vide = false;
              switch (phCol) {
                case 0: vide = (!no || no == MD_EMPTY); break;
                case 1: vide = (ins == 0); break;
                case 2: vide = (vel == MD_EMPTY); break;
                case 3: case 4: vide = (cmd == MD_EMPTY); break;
                default: vide = (ccN == MD_EMPTY); break;
              }
              if (vide) continue;
            }
            switch (phCol) {
              case 0: { int n = (no && no != MD_EMPTY ? (int)no
                                 : notePourCase(voieCourante, ph, phLigne))
                                + sens * (grand ? 12 : 1);
                        no = (uint8_t)borne(n, 1, MD_MAX_NOTE);
                        derniereNote[voieCourante] = no;
                        noteRetenue[voieCourante] = true;
                        // L'instrument de la voie, et non le 01 d'office : il
                        // est RETENU, pour que la case suivante le reprenne.
                        if (!ins) ins = (uint8_t)instrPourNote(voieCourante, ph, phLigne);
                        instrCanal[voieCourante] = ins; } break;
              case 1: ins = (uint8_t)borne((int)ins + sens * (grand ? 16 : 1), 1, 255);
                      instrCanal[voieCourante] = ins; break;
              case 2: vel = (uint8_t)borne((int)(vel ? vel : 127) + sens * (grand ? 16 : 1), 0, 127); break;
              // 3 = la LETTRE de commande, 4 = sa valeur ; 5 = le CODE
              // machine, 6 = sa valeur. Chacun sous son propre curseur.
              // La liste des commandes est BORNEE par le moteur. Sans borne
              // haute on depassait la derniere lettre et on posait un indice
              // qui n'existe pas : la case affichait « ? » et la commande ne
              // faisait rien. C'est ce qui donnait « aucune commande ne
              // marche » — le moteur, lui, les execute toutes.
              case 3: { const int nb = md_table_cmd_count();
                        int c2 = (cmd == MD_EMPTY ? -1 : (int)cmd) + sens;
                        if (c2 >= nb) c2 = nb - 1;
                        cmd = (c2 < 0) ? MD_EMPTY : (uint8_t)c2;
                        if (cmd != MD_EMPTY) { derCmd = cmd; derCmdVal = cv; } } break;
              case 4: cv = (uint8_t)borne((int)cv + sens * (grand ? 16 : 1), 0, 255);
                      if (cmd != MD_EMPTY) { derCmd = cmd; derCmdVal = cv; } break;
              // Un CC : son NUMERO, puis sa valeur, chacun sous son curseur.
              // Les deux s'arretent a 7F, la limite d'un octet de donnees MIDI.
              // Depuis une case vide, droite donne CC 01 : on part du debut.
              default:
                // Un effet AT2 : son CARACTERE (0-9, A-Z, puis & % ! @ = # $ ~ ^ ` > <),
                // puis sa valeur sur un octet. Sous le premier, la case se vide.
                if (surNumero) {
                  const int m2 = (ccN == MD_EMPTY ? -1 : (int)ccN)
                                 + sens * (grand ? 10 : 1);
                  ccN = (m2 < 0) ? MD_EMPTY : (uint8_t)borne(m2, 0, kNbFxAT2 - 1);
                } else {
                  if (ccN == MD_EMPTY) ccN = 0;
                  ccV = (uint8_t)borne((int)ccV + sens * (grand ? 16 : 1), 0, 255);
                }
                if (ccN != MD_EMPTY) { derMdCmd = ccN; derMdVal = ccV; }
                break;
            }
            md_replayer_set_phrase(ph, phLigne, no,ins,vel,cmd,cv,mc,mv);

            // Le CC part a chaque cran, numero ou valeur : c'est ce qui permet
            // le MIDI learn sans lancer la lecture. Sur une PLAGE, en revanche,
            // on n'envoie rien : seize CC d'affilee noieraient le synthe et ne
            // voudraient rien dire.
            if (kCC >= 0) {
              md_replayer_set_phrase_cc(ph, phLigne, kCC, ccN, ccV);
              // (Un effet ne s'envoie pas : il agit a la lecture.)
            }

            // ⚠️ ON ENTEND CHAQUE CHANGEMENT, PAS SEULEMENT LA POSE.
            // Poser une note avec A la faisait sonner, mais la deplacer avec
            // A + une fleche ne faisait rien entendre : il fallait relacher A
            // et le represser pour savoir ou l'on etait arrive. Regler une
            // hauteur en silence, c'est deviner — et c'est justement en
            // deplacant la note qu'on cherche la bonne.
            //
            // A l'arret seulement, et sur LA VOIE DE LA COLONNE, pour les
            // memes raisons qu'a la pose : une audition par-dessus la lecture
            // volerait une voie au morceau, et la voie d'essai est cablee sur
            // FM6 — une note posee sur une colonne PSG s'y entendrait en FM.
            //
            // On RELACHE maintenant et on rejoue a l'image suivante : sans cet
            // ecart, le synthe MIDI enchaine en legato et n'entend pas la
            // difference (voir auditionVoie, en tete de la boucle).
            // Une seule case s'auditionne : transposer une plage entiere ne
            // doit pas jouer seize notes a la file.
            // La colonne VEL s'ecoute au meme titre que la note : c'est la
            // SEULE facon de regler une force sans lancer la lecture, et en
            // MIDI c'est elle qui devient la velocite de la note-on.
            if ((phCol == 0 || phCol == 2) && no && no != MD_EMPTY &&
                !enLecture && !multi) {
              if (voieAudition >= 0) md_replayer_stop_test_note(voieAudition);
              voieAudition = voieCourante;
              auditionNote = no; auditionIns = ins ? ins : 1;
              auditionVel = (vel == MD_EMPTY) ? 127 : (int)vel;
              auditionVoie = voieCourante;
            }
          }
        }
        }   // fin de la boucle sur les cases selectionnees
        // Le curseur revient ou il etait : modifier une plage ne le deplace pas.
        curLigne = sauveCurL; curCanal = sauveCurC;
        chLigne = sauveChL;   chCol = sauveChC;
        phLigne = sauvePhL;   phCol = sauvePhC;
        // Toute la grille est a repeindre, et pas seulement la case du curseur.
        if (multi) modifie = true;
      // Le curseur BUTE aux extremites, il n'enroule pas.
      //
      // J'avais mis un modulo partout : arrive en ligne 00, une pression vers
      // le haut renvoyait en FF, a l'autre bout de la chanson. Le modele iPad
      // borne partout (max(0, min(n-1, x))) et ne fait jamais ca.
      } else if (page == PAGE_SONG) {
        if (appui & KEY_LEFT)  curCanal = borne(curCanal - 1, 0, nbColonnes() - 1);
        if (appui & KEY_RIGHT) curCanal = borne(curCanal + 1, 0, nbColonnes() - 1);
        if (appui & KEY_UP)    curLigne = borne(curLigne - 1, 0, MD_SONG_ROWS - 1);
        if (appui & KEY_DOWN)  curLigne = borne(curLigne + 1, 0, MD_SONG_ROWS - 1);
      } else if (page == PAGE_CHAIN) {
        if (appui & KEY_UP)    chLigne = borne(chLigne - 1, 0, MD_ROWS_PER_CHAIN - 1);
        if (appui & KEY_DOWN)  chLigne = borne(chLigne + 1, 0, MD_ROWS_PER_CHAIN - 1);
        if (appui & KEY_LEFT)  chCol = 0;   // deux colonnes : phrase, transpose
        if (appui & KEY_RIGHT) chCol = 1;
      } else if (page == PAGE_PHRASE) {
        if (appui & KEY_UP)    phLigne = borne(phLigne - 1, 0, MD_ROWS_PER_PHRASE - 1);
        if (appui & KEY_DOWN)  phLigne = borne(phLigne + 1, 0, MD_ROWS_PER_PHRASE - 1);
        // TREIZE positions : note, instrument, velocite, lettre de commande,
        // valeur de commande, puis numero et valeur de chacun des quatre CC.
        if (appui & KEY_LEFT)  phCol = borne(phCol - 1, 0, 12);
        if (appui & KEY_RIGHT) phCol = borne(phCol + 1, 0, 12);
#ifdef FM1
        // FM-1 : pas de colonne VL a l'ecran, le curseur saute par-dessus.
        if (phCol == 2) phCol = (appui & KEY_LEFT) ? 1 : 3;
        if (phCol > 8) phCol = 8;   // ni FX3 ni FX4 non plus
#endif
      } else if (page == PAGE_TABLE) {
        if (appui & KEY_UP)    tabLigne = borne(tabLigne - 1, 0, MD_TABLE_ROWS - 1);
        if (appui & KEY_DOWN)  tabLigne = borne(tabLigne + 1, 0, MD_TABLE_ROWS - 1);
        if (appui & KEY_LEFT)  tabCol = borne(tabCol - 1, 0, 7);
        if (appui & KEY_RIGHT) tabCol = borne(tabCol + 1, 0, 7);
#ifdef FM1
      } else if (page == PAGE_INSTR && genreInstr(instrId) == GENRE_OPL) {
        // FM-1 : d'element en element de la page. Haut / bas : la ligne
        // d'avant ou d'apres, dans la meme colonne si elle y est ; gauche /
        // droite : l'element voisin sur la meme ligne (MULT, deux colonnes).
        ElInstr el[40];
        const int n = elementsInstr(inPage, el);
        inItem = borne(inItem, 0, n - 1);
        const ElInstr ici = el[inItem];
        if (appui & (KEY_UP | KEY_DOWN)) {
          const int d = (appui & KEY_UP) ? -1 : 1;
          int meilleur = -1;
          for (int k = 0; k < n; k++) {
            if ((el[k].lig - ici.lig) * d <= 0) continue;
            if (meilleur < 0) { meilleur = k; continue; }
            const int dl = (el[k].lig - ici.lig) * d, dm = (el[meilleur].lig - ici.lig) * d;
            const int ec = abs(el[k].col - ici.col), em = abs(el[meilleur].col - ici.col);
            if (dl < dm || (dl == dm && ec < em)) meilleur = k;
          }
          if (meilleur >= 0) inItem = meilleur;
        }
        if (appui & (KEY_LEFT | KEY_RIGHT)) {
          const int d = (appui & KEY_LEFT) ? -1 : 1;
          for (int k = 0; k < n; k++)
            if (el[k].lig == ici.lig && (el[k].col - ici.col) * d > 0) { inItem = k; break; }
        }
#endif
      } else if (page == PAGE_INSTR) {
        // SAMPLE : huit champs ; MIDI : cinq. Le TYPE en tete, la TABLE en bas.
        const int nbChamps = nbChampsDe(instrId);
#ifdef FM1
        // FM-1 : pas de ligne TYPE sur un instrument OPL (pas d'echantillons).
        const int premier = genreInstr(instrId) == GENRE_OPL ? 1 : 0;
#else
        const int premier = 0;
#endif
        inLigne = borne(inLigne, premier, nbChamps - 1);
        if (appui & KEY_UP)    inLigne = borne(inLigne - 1, premier, nbChamps - 1);
        if (appui & KEY_DOWN)  inLigne = borne(inLigne + 1, premier, nbChamps - 1);
        if (genreInstr(instrId) == GENRE_OPL) {
          // Deux colonnes : deux reglages par rangee en haut, MOD et CAR en bas.
          if (appui & KEY_LEFT)  inCol = 0;
          if (appui & KEY_RIGHT) inCol = 1;
          if (!kChampsOp[inLigne].parOp) inCol = 0;   // une seule valeur
        } else inCol = 0;
      } else if (page == PAGE_PROJECT) {
        // La fenetre de nom prend la main sur toute la page.
        // ── La fenetre du MIDI : a quoi sont branches les instruments ──────
        // Meme grille de lettres que le nom de fichier, mais DEUX champs : le
        // logiciel, et son projet ou sa table de CC. X passe de l'un a l'autre
        // — ni L ni R ne sont libres, ils servent de SELECT.
        if (dialogueMidi) {
          static const char *grilleM[4] = { "0123456789", "ABCDEFGHIJ",
                                            "KLMNOPQRST", "UVWXYZ-_<*" };
          if (appui & KEY_LEFT)  nomCol = borne(nomCol - 1, 0, 9);
          if (appui & KEY_RIGHT) nomCol = borne(nomCol + 1, 0, 9);
          if (appui & KEY_UP)    nomLig = borne(nomLig - 1, 0, 3);
          if (appui & KEY_DOWN)  nomLig = borne(nomLig + 1, 0, 3);
          if (frappe & KEY_A) {
            char *champ = (midiChamp == 0) ? midiSoft
                        : (midiChamp == 1) ? midiProj : midiMap;
            const char c = grilleM[nomLig][nomCol];
            const int n = (int)strlen(champ);
            if (c == '<') { if (n > 0) *WEB_PTR(champ + n - 1) = 0; }
            else if (c == '*') {
              if (midiChamp < 2) {
                // OK valide la ligne et passe a la suivante : le logiciel, son
                // projet, puis le MIDI map. Sur un champ deja rempli, le
                // curseur se pose d'emblee sur OK — trois A, et c'est ecrit.
                midiChamp++;
                const char *suiv = (midiChamp == 1) ? midiProj : midiMap;
                if (suiv[0]) { nomLig = 3; nomCol = 9; }
                else         { nomLig = 1; nomCol = 0; }
                pageVue = -1;
              } else {
                // La fiche de CE canal est notee dans le morceau.
                md_replayer_set_midi_cible(midiCanal, midiSoft, midiProj,
                                           midiMap);
                dialogueMidi = false; pageVue = -1;
                page = pageRetourMidi;
                // Si l'on enregistrait, on enchaine sur le canal suivant qui
                // manque, et on n'ecrit le fichier qu'une fois tout renseigne.
                if (midiApresSave) {
                  const int suivant = canalAdemander();
                  if (suivant >= 0) ouvreDialogueMidi(suivant, true);
                  else              enregistreEtDis(sauveNom);
                }
              }
            } else if (n < 15) { champ[n] = c; champ[n + 1] = 0; }
          }
          // B renonce : rien n'est ecrit, ni le fichier ni les noms.
          if (frappe & KEY_B) {
            dialogueMidi = false; pageVue = -1;
            page = pageRetourMidi;
          }
        }
        else if (dialogueNom) {
          static const char *grilleT[4] = { "0123456789", "ABCDEFGHIJ",
                                            "KLMNOPQRST", "UVWXYZ-_<*" };
          if (appui & KEY_LEFT)  nomCol = borne(nomCol - 1, 0, 9);
          if (appui & KEY_RIGHT) nomCol = borne(nomCol + 1, 0, 9);
          if (appui & KEY_UP)    nomLig = borne(nomLig - 1, 0, 3);
          if (appui & KEY_DOWN)  nomLig = borne(nomLig + 1, 0, 3);
          if (frappe & KEY_A) {
            const char c = grilleT[nomLig][nomCol];
            const int n = (int)strlen(sauveNom);
            if (c == '<') { if (n > 0) *WEB_PTR(sauveNom + n - 1) = 0; }
            else if (c == '*') {
              dialogueNom = false; pageVue = -1;
              // ── ENREGISTRER NE DEMANDE QUE LE NOM ─────────────────────────
              // Le logiciel, son projet et le MIDI map de chaque canal se
              // renseignent sur la page MIDI TARGETS, et nulle part ailleurs.
              // Les demander ici, canal par canal, transformait chaque
              // sauvegarde en questionnaire.
              enregistreEtDis(sauveNom);
            } else if (n < 8) { sauveNom[n] = c; sauveNom[n + 1] = 0; }
          }
          if (frappe & KEY_B) { dialogueNom = false; pageVue = -1; }
        }
        // DELETE : la question. Gauche / droite choisissent NO ou YES, A
        // valide ; B annule toujours, et ramene a la liste.
        else if (demandeSuppr) {
          if (appui & (KEY_LEFT | KEY_RIGHT)) { supprOui = !supprOui; }
          if (frappe & KEY_B) { demandeSuppr = false; pageVue = -1; }
          else if (frappe & KEY_A) {
            demandeSuppr = false;
            if (supprOui) {
              char chemin[224];
              const int Ld = (int)strlen(dossier);
              siprintf(chemin, "%s%s%s", dossier, (Ld && dossier[Ld-1] == '/') ? "" : "/", fichiers[selFichier]);
              char nom[16]; strncpy(nom, fichiers[selFichier], 15); nom[15] = 0;
              if (remove(chemin) == 0) {
                fm1_annonce2("DELETED", nom);
                char e[240]; siprintf(e, "EFFACE : %s", chemin); journal(e);
                scanne();
                if (selFichier >= nbFichiers) selFichier = nbFichiers ? nbFichiers - 1 : 0;
              } else fm1_annonce2("CANNOT DELETE", "BUILT-IN SONG");   // (livre avec le firmware)
            }
            pageVue = -1;
          }
        }
        // Le navigateur de fichiers prend la main sur toute la page.
        else if (navigateur) {
          if (appui & KEY_UP)   selFichier = borne(selFichier - 1, 0, nbFichiers ? nbFichiers - 1 : 0);
          if (appui & KEY_DOWN) selFichier = borne(selFichier + 1, 0, nbFichiers ? nbFichiers - 1 : 0);
          if (frappe & KEY_B) {
            // Ressortir de la liste d'echantillons SANS rien charger retient
            // quand meme le dossier, s'il contient des WAV : c'est la qu'on
            // est alle chercher, et n'importe quel instrument — un neuf
            // compris — doit pouvoir y feuilleter aussitot avec A + croix.
            if (navGenre == 1 && strcmp(dossierSamples, dossier) != 0) {
              bool aDesWav = false;
              for (int i = 0; i < nbFichiers && !aDesWav; i++)
                aDesWav = (typeFic[i] == 1);
              if (aDesWav) {
                strncpy(dossierSamples, dossier, sizeof(dossierSamples) - 1);
                dossierSamples[sizeof(dossierSamples) - 1] = 0;
                ecrisCfg();
                scanneEch();
              }
            }
            navigateur = false; page = pageRetour; pageVue = -1;
          }
          if (navSuppr) {                          // DELETE : A sur un projet pose la question
            if (frappe & KEY_B) navSuppr = false;
            else if ((frappe & KEY_A) && nbFichiers > 0 && typeFic[selFichier] == 1) {
              demandeSuppr = true; supprOui = false; pageVue = -1;
            }
          }
          else if ((frappe & KEY_A) && nbFichiers > 0 && typeFic[selFichier] == 2) {
            entre(fichiers[selFichier]);
          }
          else if ((frappe & KEY_A) && nbFichiers > 0
                   && typeFic[selFichier] == 1 && navGenre == 1) {
            // Un echantillon : on le convertit, on le range dans la banque du
            // morceau, et on l'attache a l'instrument en cours d'edition.
            char chemin[224];
            const int Ld = (int)strlen(dossier);
            siprintf(chemin, "%s%s%s", dossier,
                     (Ld && dossier[Ld-1] == '/') ? "" : "/",
                     fichiers[selFichier]);
            // Le son que l'instrument avait rend sa place s'il ne sert plus a
            // rien : recharger plusieurs fois ne remplit pas la banque.
            { const int avant = md_replayer_get_instr_sample(instrId);
              if (avant >= 0 && avant < MD_MAX_SAMPLES)
                md_replayer_libere_sample(avant, instrId); }
            // Decode directement dans la banque : jamais deux copies du son.
            uint32_t cap = 0, im = 0, hz = 0;
            int bits = 8, noteRef = 49; int32_t bcl = -1;
            uint8_t *zone = md_replayer_zone_libre(&cap);
            const int r = zone ? md_audio_charge_wav(chemin, zone, cap, &im, &hz,
                                                     &bits, &bcl, &noteRef)
                               : 2;
            char e[140];
            if (r == 0) {
              // C-4 par defaut (note 49) : la note a laquelle l'echantillon
              // joue a sa cadence. Un WAV qui porte sa note de reference et sa
              // boucle dans un bloc « smpl » les apporte avec lui.
              const int idx = md_replayer_add_sample(fichiers[selFichier],
                                                     zone, im, bits, hz, bcl,
                                                     noteRef);
              if (idx >= 0) {
                md_replayer_set_instr_sample(instrId, idx);
                md_replayer_set_instr_kind(instrId, MD_INSTR_KIND_PCM);
                siprintf(e, "ECHANTILLON %s CHARGE : %lu IMAGES, %d BITS, %lu HZ",
                         fichiers[selFichier], (unsigned long)im, bits,
                         (unsigned long)hz);
                strcpy(msgPCM, "LOADED");
                // Ce dossier devient LE dossier d'echantillons : A + croix y
                // feuillettera les voisins, et LOAD SAMPLE y reviendra, meme
                // apres avoir eteint la console.
                strncpy(dossierSamples, dossier, sizeof(dossierSamples) - 1);
                dossierSamples[sizeof(dossierSamples) - 1] = 0;
                ecrisCfg();
                scanneEch();
                echSel = rangEch(instrId);
              } else { siprintf(e, "BANQUE PLEINE : %s", fichiers[selFichier]);
                       strcpy(msgPCM, "BANK FULL"); }
            } else if (r == 2) {
              siprintf(e, "BANQUE PLEINE : %s", fichiers[selFichier]);
              strcpy(msgPCM, "BANK FULL");
            } else { siprintf(e, "WAV ILLISIBLE : %s", fichiers[selFichier]);
                     strcpy(msgPCM, "BAD WAV"); }
            journal(e);
            // Rien de charge : l'instrument ne reste pas branche sur une place
            // rendue plus haut.
            { const int maintenant = md_replayer_get_instr_sample(instrId);
              char nm[24]; uint32_t l3; int b3, n3;
              if (maintenant >= 0 && maintenant < MD_MAX_SAMPLES &&
                  !md_replayer_get_sample(maintenant, nm, 24, &l3, &b3, &n3))
                md_replayer_set_instr_sample(instrId, MD_EMPTY); }
            // On revient a la page d'ou l'on venait — la page instrument —
            // et non sur la liste des fichiers : le choix est fait. Le meme
            // appui sur A ne doit pas, une fois revenu, rouvrir le navigateur.
            navigateur = false; page = pageRetour; pageVue = -1;
            navFerme = true;
          }
          else if ((frappe & KEY_A) && nbFichiers > 0
                   && typeFic[selFichier] == 1) {
            md_replayer_stop(); enLecture = false;
            { char e[80]; siprintf(e, "OUVERTURE %s", fichiers[selFichier]);
              journal(e); }
            const bool ouvert = ouvre(fichiers[selFichier]);
            if (ouvert) {
              // Le morceau ouvert donne son nom au projet : c'est sous ce
              // nom-la qu'on voudra le reenregistrer.
              { const char *n = fichiers[selFichier]; int j = 0;
                for (; j < 15 && n[j] && n[j] != '.'; j++) {
                  char c2 = n[j];
                  if (c2 >= 'a' && c2 <= 'z') c2 = (char)(c2 - 'a' + 'A');
                  nomProjet[j] = c2;
                }
                nomProjet[j] = 0;
                strncpy(sauveNom, nomProjet, 8); sauveNom[8] = 0; }
              curLigne = 0; haut = 0; curCanal = 0;
              chainId = 0; phraseId = 0; instrId = 1;
              chLigne = chCol = phLigne = phCol = inLigne = inCol = 0;
              selActive = false; clipPage = -1;
              toutesPistes();
              // A quoi son MIDI etait branche : on le dit tout de suite, c'est
              // ce qu'il faut lancer sur l'iPad avant de jouer.
              // Les applications a lancer sur l'iPad, dans l'ordre des canaux.
              // Le detail est sur la page MIDI TARGETS.
              { char liste[40] = "", s[16], p[16], m[16];
                int n = 0;
                for (int c = 0; c < MD_MIDI_CANAUX; c++) {
                  if (!md_replayer_canal_midi_utilise(c)) continue;
                  md_replayer_get_midi_cible(c, s, p, m, 16);
                  if (!s[0]) continue;
                  if (n + (int)strlen(s) + 3 >= (int)sizeof(liste)) break;
                  if (n) { liste[n++] = ' '; liste[n++] = '+'; liste[n++] = ' '; }
                  strcpy(liste + n, s);
                  n += (int)strlen(s);
                }
                if (n) {
                  siprintf(msgProjet, "MIDI %.30s", liste);
                  msgProjetJusqu = secondes + 6;
                } }
            }
            // Un morceau ouvert, on arrive sur SONG pour le jouer tout de
            // suite ; un echec ramene d'ou l'on venait, ou le message l'explique.
            navigateur = false; page = ouvert ? PAGE_SONG : pageRetour; pageVue = -1;
          }
        }
        // Tant qu'une question est posee, A et B lui repondent.
        else if (demandeNouveau) {
          if (frappe & KEY_A) {
            md_replayer_stop(); enLecture = false;
            md_replayer_new_empty();
            ouvertA2m = false;
#ifdef FM1
            fm1ProjetDepart();
#endif
            a2m_oublie_origine();   // un morceau neuf ne doit rien a l'ancien A2M
            md_replayer_set_bpm(125.0);
            toutesPistes();
            curLigne = 0; haut = 0; curCanal = 0;
            chainId = 0; phraseId = 0; instrId = 1;
            chLigne = chCol = phLigne = phCol = inLigne = inCol = 0;
            selActive = false; clipPage = -1;
            demandeNouveau = false; pageVue = -1;
            journal("NOUVEAU MORCEAU");
          }
          if (frappe & KEY_B) { demandeNouveau = false; pageVue = -1; }
        }
        // ── Le menu ────────────────────────────────────────────────────
        // Haut/bas pour choisir la ligne, A pour l'actionner. Sur la ligne du
        // tempo, A ne fait rien tout seul : c'est A + gauche/droite qui change
        // la valeur, et A seul ne doit donc pas declencher d'action.
        else {
#ifdef FM1
          if (appui & KEY_UP)   menuProjet = kMenuVu[borne(rangMenu(menuProjet) - 1, 0, kMenuNbVu - 1)];
          if (appui & KEY_DOWN) menuProjet = kMenuVu[borne(rangMenu(menuProjet) + 1, 0, kMenuNbVu - 1)];
#else
          if (appui & KEY_UP)   menuProjet = borne(menuProjet - 1, 0, kMenuDernier);
          if (appui & KEY_DOWN) menuProjet = borne(menuProjet + 1, 0, kMenuDernier);
#endif
#ifdef FM1
          // ZOOM : A + droite ou gauche, OFF ou 4 COL.
          if (menuProjet == kMenuZoom && (keysHeld() & KEY_A) && (appui & (KEY_LEFT | KEY_RIGHT))) {
            zoomNiv = !zoomNiv;                   // (OFF ou 4 COL : le 3 COL est retire, trop gros pour tout montrer)
            ecrisCfg();                           // (garde au redemarrage)
          }
          // OUT : A + droite (ou gauche), MONO ou STEREO.
          if (menuProjet == kMenuSortie && (keysHeld() & KEY_A) && (appui & (KEY_LEFT | KEY_RIGHT))) {
            sortieMono = !sortieMono;
            fm1_sortie_mono(sortieMono);
            ecrisCfg();
          }
          // LIGHT : A + droite / gauche, plus ou moins lumineux (0 a F)
          if (menuProjet == kMenuLum && (keysHeld() & KEY_A) && (appui & (KEY_LEFT | KEY_RIGHT))) {
            lumiere = borne(lumiere + ((appui & KEY_RIGHT) ? 1 : -1), 0, 15);
            fm1_luminosite(lumiere);
            ecrisCfg();
            pageVue = -1;                         // (la valeur, repeinte)
          }
#endif

          // PATTERN LEN : A + gauche/droite, une ligne ; A + haut/bas, 16.
          if (menuProjet == 6 && (keysHeld() & KEY_A) &&
              (appui & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN))) {
            const int pas2 = (appui & KEY_RIGHT) ? 1 : (appui & KEY_LEFT) ? -1
                           : (appui & KEY_UP) ? 16 : -16;
            md_replayer_set_patt_len(md_replayer_get_patt_len() + pas2);
            menuProjet = 6;   // haut/bas tenus avec A reglent, ils ne deplacent pas
          }

          // SYNC : A + gauche/droite, 1 ms ; A + haut/bas, 10 ms.
          if (menuProjet == 4 && (keysHeld() & KEY_A) &&
              (appui & (KEY_LEFT | KEY_RIGHT))) {
            md_replayer_set_sync_ms(md_replayer_get_sync_ms() +
                                    ((appui & KEY_RIGHT) ? 1 : -1));
            ecrisCfg();
          }

          if (menuProjet == 0) {
            if ((keysHeld() & KEY_A) && (appui & (KEY_LEFT | KEY_RIGHT))) {
              // ── On avance jusqu'au PROCHAIN tempo REELLEMENT atteignable ──
              // Le moteur regle le BPM par le finetune, qui est un entier :
              // pres de 125 BPM une unite vaut deux BPM et demi. Demander
              // « un de plus » retombait donc dans le meme cran et ne changeait
              // rien — le tempo semblait bloque entre 123 et 127.
              //
              // On demande donc des valeurs croissantes jusqu'a ce que le
              // moteur en rende une differente : chaque pression deplace d'un
              // cran, quel qu'en soit l'ecart.
              const int d = (appui & KEY_RIGHT) ? 1 : -1;
              const double avant = md_replayer_get_bpm();
              for (int essai = 1; essai <= 40; essai++) {
                const int vise = borne((int)(avant + 0.5) + d * essai, 32, 255);
                md_replayer_set_bpm((double)vise);
                const double apres = md_replayer_get_bpm();
                const double ecart = apres > avant ? apres - avant : avant - apres;
                if (ecart > 0.4) break;
                // Bute sur une borne : inutile d'insister.
                if (vise <= 32 || vise >= 255) break;
              }
            }
          } else if (frappe & KEY_A) {
            if (menuProjet == 1) {
              // La fenetre s'ouvre sur le nom du projet courant.
              strncpy(sauveNom, nomProjet, 8); sauveNom[8] = 0;
              // Le curseur se pose d'emblee sur OK : reenregistrer un projet
              // qui a deja son nom se fait alors en deux touches.
              saveEnA2m = ouvertA2m && fm1_morceau_tient_en_a2m();
              dialogueNom = true; nomLig = 3; nomCol = 9;
              assombrirDemande = true;
              // La page est VIDEE avant d'ouvrir la fenetre : le menu assombri
              // restait derriere, et ses lignes (la liste des canaux MIDI, les
              // intitules du menu) se melangeaient avec celles de la fenetre.
              pageVue = -1;
            } else if (menuProjet == 2) {
              navSuppr = false;
              if (carteOK) { creeSiAbsent(dossierSongs);
                             strcpy(dossier, dossierSongs);
                             navGenre = 0; scanne(); navigateur = true;
                             pageRetour = PAGE_PROJECT; pageVue = -1; }
            } else if (menuProjet == 3) {
              demandeNouveau = true; pageVue = -1;
            } else if (menuProjet == 5) {
              // SYNC TEST : un clic OPL3 et un clic sample ensemble, deux fois
              // par seconde, pour regler SYNC a l'oreille.
              syncTest = !syncTest;
            } else if (menuProjet == kMenuApropos) {
              page = PAGE_APROPOS; pageVue = -1;
            }
#ifdef FM1
            // EXPORT A2M : le morceau en .A2M pour AdLib Tracker II et Amber
            // OPL3 Tracker, a cote des .XT (seulement ce qu'AT2 connait). Sous
            // le nom du projet ; il s'ouvre aussi par LOAD.
            // DELETE : la meme liste que LOAD, mais A demande d'effacer.
            else if (menuProjet == kMenuSuppr && carteOK) {
              creeSiAbsent(dossierSongs);
              strcpy(dossier, dossierSongs);
              navGenre = 0; navSuppr = true; scanne(); navigateur = true;
              pageRetour = PAGE_PROJECT; pageVue = -1;
            }
            else if (menuProjet == kMenuExport && carteOK) {
              char propre[10], chemin[160], msg[48] = "", l2[24];
              int np = 0;
              for (int i2 = 0; i2 < 8 && nomProjet[i2]; i2++)
                if (nomProjet[i2] != ' ') propre[np++] = nomProjet[i2];
              propre[np] = 0;
              if (!np) strcpy(propre, "SONG");
              creeSiAbsent(dossierSongs);
              siprintf(chemin, "%s/%s.A2M", dossierSongs, propre);
              fm1_ecran_attente("EXPORTING...");
              const bool ok = fm1_exporte_a2m(chemin, msg, sizeof msg);
              pageVue = -1;
              siprintf(l2, "%s.A2M", propre);
              fm1_annonce2(ok ? "EXPORTED" : "EXPORT FAILED", ok ? l2 : msg);
              char e[200];
              siprintf(e, ok ? "EXPORT A2M : %s (%s)" : "EXPORT A2M : ECHEC %s (%s)", chemin, msg);
              journal(e);
            }
#endif
          }
        }
      }
      // ── A seul : poser une valeur ─────────────────────────────────────
      // A avec B tenu ne pose RIEN : cet accord-la efface, plus bas. Sans ce
      // garde, A posait une valeur que l'effacement retirait aussitot — et sur
      // la colonne des notes il la faisait entendre au passage.
      const bool fenetreFermee = fenetreAuDebut && !(navigateur || dialogueNom || demandeNouveau || dialogueMidi);
      if ((frappe & KEY_A) && page != PAGE_PROJECT && !(keysHeld() & KEY_B) &&
          !fenetreFermee) {
        // inLigne entre dans la cle : sans lui, tous les champs d'une page
        // d'instrument partageaient le meme repere et deux A sur deux reglages
        // differents passaient pour un double-A.
        const int cle = page * 100000 + curCanal * 10000 + curLigne * 20
                        + chLigne + phLigne * 3 + phCol + inLigne * 7;
        const unsigned t = timerTick(2);
        const bool doubleA = (cle == derA) && ((unsigned short)(t - derAt) < 16000);
        derA = cle; derAt = t;

        // ── Le champ TABLE d'un instrument ──────────────────────────────
        // Reprise mot pour mot des regles de l'iPad :
        //   double-A          une table VIDE, la premiere libre
        //   A, champ sur OFF  la derniere table employee revient
        //   A, table posee    on la retient, pour ce rappel-la
        if (page == PAGE_INSTR) {
          const int champTable = champTableDe(instrId);
          if (inLigne == champTable) {
            const int t3 = md_replayer_get_instr_table(instrId);
            const bool aucune = (t3 < 0 || t3 == MD_EMPTY);
            if (doubleA) {
              const int neuf = tableLibre();
              md_replayer_set_instr_table(instrId, neuf);
              derniereTable = neuf;
            } else if (aucune) {
              md_replayer_set_instr_table(instrId, derniereTable);
            } else {
              derniereTable = t3;
            }
          }
        }

        if (page == PAGE_SONG) {
          uint8_t v = md_replayer_get_song(reel(curCanal), curLigne);
          if (doubleA || v == MD_EMPTY) {
#ifdef FM1
            // Comme LSDJ et l'iPad : A simple, le DERNIER chain ; double A, un
            // chain NEUF. FM-1 : le chain neuf a tout de suite une phrase, avec
            // l'instrument d'avant (phraseNeuve) ; et tant qu'aucun chain n'a
            // encore servi (le dernier est vide), A simple en cree un aussi.
            int id = dernierChain;
            if (doubleA || chainSansPhrase(id)) {
              id = chainLibre();
              dernierePhrase = phraseNeuve(reel(curCanal));
              md_replayer_set_chain((uint8_t)id, 0, (uint8_t)dernierePhrase, 0);
            }
            md_replayer_set_song(reel(curCanal), curLigne, (uint8_t)id); dernierChain = id;
#else
            int id = doubleA ? chainLibre() : dernierChain;
            md_replayer_set_song(reel(curCanal), curLigne, (uint8_t)id); dernierChain = id;
#endif
          } else dernierChain = v;
        } else if (page == PAGE_CHAIN && chCol == 0) {
          {
            const uint8_t noChain = (uint8_t)chainId;
            uint8_t ph; int8_t tsp; md_replayer_get_chain(noChain, chLigne, &ph, &tsp);
            if (doubleA || ph == MD_EMPTY) {
#ifdef FM1
              // A simple : la DERNIERE phrase ; double A : une phrase NEUVE,
              // avec l'instrument d'avant.
              const int id = doubleA ? phraseNeuve(voieCourante) : dernierePhrase;
#else
              int id = doubleA ? phraseLibre() : dernierePhrase;
#endif
              md_replayer_set_chain(noChain, chLigne, (uint8_t)id, tsp);
              dernierePhrase = id;
            } else dernierePhrase = ph;
          }
        } else if (page == PAGE_PHRASE) {
          {
            const uint8_t ph = (uint8_t)phraseId;
            uint8_t no,ins,vel,cmd,cv,mc,mv;
            md_replayer_get_phrase(ph, phLigne, &no,&ins,&vel,&cmd,&cv,&mc,&mv);
            // Positions 5 a 12 : les quatre CC, numero puis valeur.
            const int kCC = (phCol >= 5) ? (phCol - 5) / 2 : -1;
            uint8_t ccN = MD_EMPTY, ccV = 0;
            if (kCC >= 0) md_replayer_get_phrase_cc(ph, phLigne, kCC, &ccN, &ccV);
            switch (phCol) {
              case 0: if (no && no != MD_EMPTY) {
                        // A sur une note deja posee : elle devient la derniere
                        // note, et son instrument le dernier instrument.
                        derniereNote[voieCourante] = no;
                        noteRetenue[voieCourante] = true;
                        if (ins) instrCanal[voieCourante] = ins;
                        notePoseeParA_ph = -1;
                      } else {
                        // Case vide : la derniere note et le dernier
                        // instrument de la voie, comme dans LSDJ.
                        no = (uint8_t)notePourCase(voieCourante, ph, phLigne);
                        if (!ins) ins = (uint8_t)instrPourNote(voieCourante, ph, phLigne);
                        notePoseeParA_ph = ph; notePoseeParA_l = phLigne;
                      } break;
              case 1: {
                // Colonne INSTRUMENT, regles de l'iPad :
                //   double A  -> un instrument NEUF, initialise, retenu ;
                //   A simple  -> l'instrument de la case, ou celui de la voie.
                const int voie = voieCourante;
                if (doubleA) {
                  const int ni = instrLibre();
                  // Un instrument neuf est un instrument FM de l'OPL3 — un
                  // SAMPLE sur les pistes 19 et plus.
                  md_replayer_init_default_instrument(ni - 1);
                  if (voie >= OPL_NB_PISTES) md_replayer_set_instr_kind(ni, MD_INSTR_KIND_PCM);
                  ins = (uint8_t)ni;
                  instrCanal[voie] = ni;
                  instrId = ni;
                } else if (ins) {
                  instrCanal[voie] = ins;
                  instrId = ins;
                } else {
                  ins = (uint8_t)instrPourNote(voie, ph, phLigne);
                  instrId = ins;
                }
                break;
              }
              case 2: if (vel == MD_EMPTY) vel = 127; break;
              // ⚠️ UNE CASE VIDE RAPPELLE LA DERNIERE COMMANDE POSEE, valeur
              // comprise — comme sur la page TABLE, qui le faisait deja. Ici
              // on repartait de la premiere lettre de la liste et d'une valeur
              // nulle : apres avoir efface une commande avec A+B, il fallait
              // retraverser toute la liste pour retrouver la meme, en
              // declenchant au passage un changement de table ou de tempo.
              case 3: case 4:
                if (cmd == MD_EMPTY) { cmd = derCmd; cv = derCmdVal; }
                else { derCmd = cmd; derCmdVal = cv; }
                break;
              // Un CC vide rappelle le dernier CC pose, valeur comprise.
              default:
                if (ccN == MD_EMPTY) { ccN = derMdCmd; ccV = derMdVal; }
                else { derMdCmd = ccN; derMdVal = ccV; }
                break;
            }
            md_replayer_set_phrase(ph, phLigne, no,ins,vel,cmd,cv,mc,mv);

            // A seul sur un CC le renvoie : pratique pour refaire un MIDI learn.
            if (kCC >= 0) {
              md_replayer_set_phrase_cc(ph, phLigne, kCC, ccN, ccV);
              if (ccN != MD_EMPTY) envoieCC(instrDuCC(ph, phLigne, ins), ccN, ccV);
            }

            // On ENTEND la note qu'on vient de poser, comme sur l'iPad.
            // Uniquement a l'arret : une note d'audition par-dessus la lecture
            // volerait une voie et brouillerait ce qu'on ecoute.
            //
            // Sur LA VOIE DE LA COLONNE, et non sur la voie d'essai : celle-ci
            // est cablee sur FM6, si bien qu'une note posee sur une colonne
            // PSG ou sur le bruit s'auditionnait en synthese FM — on entendait
            // un son qui n'avait rien a voir avec ce qu'on ecrivait. La voie
            // reelle achemine vers la bonne puce. C'est sans risque puisqu'on
            // est a l'arret : elle ne joue rien d'autre a cet instant.
            //
            // Meme ecart d'une image qu'avec A + croix : deux A de suite sur la
            // meme note doivent s'entendre deux fois.
            if (phCol == 0 && no && no != MD_EMPTY && !enLecture) {
              if (voieAudition >= 0) md_replayer_stop_test_note(voieAudition);
              voieAudition = voieCourante;
              auditionNote = no; auditionIns = ins ? ins : 1;
              auditionVel = (vel == MD_EMPTY) ? 127 : (int)vel;
              auditionVoie = voieCourante;
            }
          }
        }
      }

      // ── X : effacer ───────────────────────────────────────────────────
      // Comportement de LSDJ, repris de deleteCell sur l'iPad : sur une case
      // PLEINE on l'efface en laissant le trou ; sur une case VIDE on remonte
      // toute la colonne d'un cran, si bien qu'appuyer plusieurs fois au meme
      // endroit « aspire » la colonne vers le haut.
      // A+B efface, dans les deux ordres, exactement comme X — c'est ce que
      // fait l'iPad : B presse pendant qu'on tient A, ou A presse pendant
      // qu'on tient B. Le B de l'accord ne doit pas declencher de copie au
      // relachement, d'ou le drapeau.
      // ⚠️ FAIRE REMONTER LES DONNEES N'EXISTE QUE DANS SONG.
      // Ailleurs — CHAIN, PHRASE, colonnes CC comprises — effacer efface la
      // case, un point c'est tout. Vider une velocite ne doit pas aspirer la
      // colonne entiere vers le haut sous le curseur.
      const bool decaleOK = (page == PAGE_SONG);
      bool effacer = (frappe & KEY_X) != 0;
      bool accordAB = false;
      if (((frappe & KEY_A) && (keysHeld() & KEY_B)) ||
          ((frappe & KEY_B) && (keysHeld() & KEY_A))) {
        effacer = true;
        accordAB = true;
        bUtilise = true;
      }
      // ── A+B sur la colonne des NOTES d'une phrase ────────────────────────
      //   une note (ou un OFF)  -> on l'efface, et rien d'autre : les lignes
      //                            ne remontent pas ;
      //   une case vide         -> un note off, « OFF », qui relache la note
      //                            MIDI — ou coupe le sample — sur cette ligne.
      // Faire remonter la colonne reste le role de X.
      if (accordAB && page == PAGE_PHRASE && phCol == 0) {
        effacer = false;
        uint8_t no,ins,vel,cmd,cv,mc,mv;
        md_replayer_get_phrase((uint8_t)phraseId, phLigne,
                               &no,&ins,&vel,&cmd,&cv,&mc,&mv);
        // Si l'accord a commence par A sur une case vide, ce A y a deja pose
        // une note : la case comptait comme vide.
        const bool poseeParLeA = (notePoseeParA_ph == phraseId &&
                                  notePoseeParA_l == phLigne);
        const bool pleine = (no != 0) && !poseeParLeA;
        // L'instrument part avec la note : seul, il ne jouerait rien.
        md_replayer_set_phrase((uint8_t)phraseId, phLigne,
                               pleine ? 0 : MD_EMPTY, 0, vel,cmd,cv,mc,mv);
        notePoseeParA_ph = -1;
        // La note d'audition posee par le A de l'accord se tait aussi.
        if (voieAudition >= 0) {
          md_replayer_stop_test_note(voieAudition);
          voieAudition = -1;
        }
        modifie = true;
      }
      if (effacer && page != PAGE_PROJECT) {
        if (page == PAGE_INSTR) {
          // Effacer le champ TABLE, c'est le remettre sur OFF.
          const int champTable = champTableDe(instrId);
          if (inLigne == champTable) md_replayer_set_instr_table(instrId, -1);
          // Effacer le champ SAMPLE retire le son de l'instrument — et de la
          // banque, s'il n'y sert plus.
          else if (genreInstr(instrId) == GENRE_SAMPLE && inLigne == 2) {
            const int ech = md_replayer_get_instr_sample(instrId);
            md_replayer_set_instr_sample(instrId, MD_EMPTY);
            if (ech >= 0 && ech < MD_MAX_SAMPLES) md_replayer_libere_sample(ech, -1);
          }
        } else if (page == PAGE_TABLE) {
          uint8_t vol,c1,v1,c2,v2,mc,mv; int8_t tsp;
          md_replayer_get_table_row(tableId, tabLigne, &vol, &tsp,
                                    &c1, &v1, &c2, &v2, &mc, &mv);
          // Effacer une lettre emmene sa valeur : les deux ne veulent rien
          // dire l'une sans l'autre.
          switch (tabCol) {
            case 0: vol = 0; break;              // 0 = « ne touche pas au volume »
            case 1: tsp = 0; break;              // 0 = « ne transpose pas »
            case 2: case 3: c1 = MD_EMPTY; v1 = 0;
                    md_replayer_clear_active_effects(voieCourante); break;
            case 4: case 5: c2 = MD_EMPTY; v2 = 0;
                    md_replayer_clear_active_effects(voieCourante); break;
            default: mc = MD_EMPTY; mv = 0; break;
          }
          md_replayer_set_table_row(tableId, tabLigne, vol, tsp,
                                    c1, v1, c2, v2, mc, mv);
        } else if (page == PAGE_SONG) {
          if (md_replayer_get_song(reel(curCanal), curLigne) == MD_EMPTY) {
            for (int r = curLigne; r < MD_SONG_ROWS - 1; r++)
              md_replayer_set_song(reel(curCanal), r, md_replayer_get_song(reel(curCanal), r + 1));
            md_replayer_set_song(reel(curCanal), MD_SONG_ROWS - 1, MD_EMPTY);
          } else md_replayer_set_song(reel(curCanal), curLigne, MD_EMPTY);
        } else if (page == PAGE_CHAIN) {
          uint8_t noChain = md_replayer_get_song(reel(curCanal), curLigne);
          if (noChain != MD_EMPTY) {
            uint8_t ph; int8_t tsp; md_replayer_get_chain(noChain, chLigne, &ph, &tsp);
            const bool vide = decaleOK &&
                              ((chCol == 0) ? (ph == MD_EMPTY) : (tsp == 0));
            const int dern = MD_ROWS_PER_CHAIN - 1;
            if (vide) {
              for (int r = chLigne; r < dern; r++) {
                uint8_t p1,p2; int8_t t1,t2;
                md_replayer_get_chain(noChain, r, &p1, &t1);
                md_replayer_get_chain(noChain, r + 1, &p2, &t2);
                if (chCol == 0) md_replayer_set_chain(noChain, r, p2, t1);
                else            md_replayer_set_chain(noChain, r, p1, t2);
              }
              uint8_t pl; int8_t tl; md_replayer_get_chain(noChain, dern, &pl, &tl);
              if (chCol == 0) md_replayer_set_chain(noChain, dern, MD_EMPTY, tl);
              else            md_replayer_set_chain(noChain, dern, pl, 0);
            } else if (chCol == 0) md_replayer_set_chain(noChain, chLigne, MD_EMPTY, tsp);
            else                   md_replayer_set_chain(noChain, chLigne, ph, 0);
          }
        } else if (page == PAGE_PHRASE) {
          {
            const uint8_t ph = (uint8_t)phraseId;
            if (phCol >= 5) {
              // Une colonne CC : pleine, on la vide ; vide, on remonte CETTE
              // colonne d'un cran — les autres ne bougent pas.
              const int kCC = (phCol - 5) / 2;
              uint8_t ccN, ccV;
              md_replayer_get_phrase_cc(ph, phLigne, kCC, &ccN, &ccV);
              if (decaleOK && ccN == MD_EMPTY) {
                for (int r = phLigne; r < MD_ROWS_PER_PHRASE - 1; r++) {
                  uint8_t n2, v2;
                  md_replayer_get_phrase_cc(ph, r + 1, kCC, &n2, &v2);
                  md_replayer_set_phrase_cc(ph, r, kCC, n2, v2);
                }
                md_replayer_set_phrase_cc(ph, MD_ROWS_PER_PHRASE - 1, kCC, MD_EMPTY, 0);
              } else {
                md_replayer_set_phrase_cc(ph, phLigne, kCC, MD_EMPTY, 0);
              }
            } else {
            uint8_t no,ins,vel,cmd,cv,mc,mv;
            md_replayer_get_phrase(ph, phLigne, &no,&ins,&vel,&cmd,&cv,&mc,&mv);
            bool vide;
            switch (phCol) {
              case 0: vide = (no == 0); break;
              case 1: vide = (ins == 0); break;
              case 2: vide = (vel == MD_EMPTY); break;
              case 3: case 4: vide = (cmd == MD_EMPTY); break;
              default: vide = (mc == MD_EMPTY); break;
            }
            vide = vide && decaleOK;
            const int dern = MD_ROWS_PER_PHRASE - 1;
            if (vide) {
              for (int r = phLigne; r < dern; r++) {
                uint8_t a1,b1,c1,d1,e1,f1,g1, a2,b2,c2,d2,e2,f2,g2;
                md_replayer_get_phrase(ph, r,   &a1,&b1,&c1,&d1,&e1,&f1,&g1);
                md_replayer_get_phrase(ph, r+1, &a2,&b2,&c2,&d2,&e2,&f2,&g2);
                switch (phCol) {
                  case 0: a1 = a2; b1 = b2; break;   // la note emmene son instrument
                  case 1: b1 = b2; break;
                  case 2: c1 = c2; break;
                  case 3: case 4: d1 = d2; e1 = e2; break;
                  default: f1 = f2; g1 = g2; break;
                }
                md_replayer_set_phrase(ph, r, a1,b1,c1,d1,e1,f1,g1);
              }
              uint8_t a,b,c,d,e,f,g;
              md_replayer_get_phrase(ph, dern, &a,&b,&c,&d,&e,&f,&g);
              switch (phCol) {
                case 0: a = 0; b = 0; break;
                case 1: b = 0; break;
                case 2: c = 0; break;
                case 3: case 4: d = MD_EMPTY; e = 0; break;
                default: f = MD_EMPTY; g = 0; break;
              }
              md_replayer_set_phrase(ph, dern, a,b,c,d,e,f,g);
            } else {
              switch (phCol) {
                // Effacer la NOTE emporte l'instrument : les deux ne veulent
                // rien dire l'un sans l'autre. Une case laissee avec un
                // instrument mais sans note ne joue rien, et la ligne semblait
                // pourtant occupee.
                case 0: no = 0; ins = 0; break;
                case 1: ins = 0; break;
                case 2: vel = MD_EMPTY; break;   // vide, pas volume nul
                // Effacer une commande doit S'ENTENDRE tout de suite. Un
                // effet continu tourne jusqu'a ce qu'on le reecrive a 00 ;
                // l'effacement laisse 0xFF, que le moteur ignore, si bien que
                // l'effet survivait jusqu'a l'arret de la lecture.
                case 3: case 4: cmd = MD_EMPTY; cv = 0;
                                md_replayer_clear_active_effects(voieCourante);
                                break;
                default: mc = MD_EMPTY; mv = 0;
                         md_replayer_clear_active_effects(voieCourante);
                         break;
              }
              md_replayer_set_phrase(ph, phLigne, no,ins,vel,cmd,cv,mc,mv);
            }
            }   // fin des colonnes autres que CC
          }
        }
      }

      // A seul sur la page TABLE : la case vide prend une valeur de depart,
      // comme dans une phrase. Sur une lettre de commande, la premiere de la
      // liste ; ailleurs, zero suffit puisque zero y est deja le neutre.
      if ((frappe & KEY_A) && page == PAGE_TABLE && !(keysHeld() & KEY_B) &&
          !selTenu) {
        uint8_t vol,c1,v1,c2,v2,mc,mv; int8_t tsp;
        md_replayer_get_table_row(tableId, tabLigne, &vol, &tsp,
                                  &c1, &v1, &c2, &v2, &mc, &mv);
        // Une case de commande VIDE rappelle la DERNIERE posee, avec sa
        // valeur — comme sur l'iPad. Repartir de la premiere lettre obligeait a
        // traverser toute la liste, et on declenchait au passage un changement
        // de table ou de tempo.
        if (tabCol == 2 || tabCol == 3) {
          if (c1 == MD_EMPTY) { c1 = derCmd; v1 = derCmdVal; }
          else { derCmd = c1; derCmdVal = v1; }
        } else if (tabCol == 4 || tabCol == 5) {
          if (c2 == MD_EMPTY) { c2 = derCmd; v2 = derCmdVal; }
          else { derCmd = c2; derCmdVal = v2; }
        } else if (tabCol >= 6) {
          if (mc == MD_EMPTY) { mc = derMdCmd; mv = derMdVal; }
          else { derMdCmd = mc; derMdVal = mv; }
        }
        md_replayer_set_table_row(tableId, tabLigne, vol, tsp,
                                  c1, v1, c2, v2, mc, mv);
        // A seul sur un CC le renvoie : pratique pour refaire un MIDI learn.
        if (tabCol >= 6 && mc != MD_EMPTY) envoieCC(instrId, mc, mv);
      }

#ifdef FM1
      // INSTR OPL3 : A prend le choix d'une liste, coche ou decoche une case.
      if ((frappe & KEY_A) && page == PAGE_INSTR && !(keysHeld() & KEY_B) &&
          genreInstr(instrId) == GENRE_OPL && !navigateur && !dialogueNom) {
        ElInstr el[40];
        elementsInstr(inPage, el);
        const ElInstr &e = el[inItem];
        if (e.genre == EL_OPT || e.genre == EL_CHK) {
          const ChampOpl &ch = kChampsOp[e.champ];
          const int octet = ch.octet + (ch.parOp ? inOp : 0);
          if (e.genre == EL_CHK)
            oplEcrit(instrId, octet, ch.decalage, ch.masque, !oplLit(instrId, octet, ch.decalage, ch.masque));
          else
            oplEcrit(instrId, octet, ch.decalage, ch.masque, ch.aff == AFF_KSL ? kKslOrdre[e.opt & 3] : e.opt);
          modifie = true;
        }
      }
#endif
      // A sur « LOAD SAMPLE » ouvre le navigateur, regle sur les WAV.
      if ((frappe & KEY_A) && page == PAGE_INSTR && !(keysHeld() & KEY_B) &&
          !navFerme && genreInstr(instrId) == GENRE_SAMPLE && inLigne == 1 &&
          carteOK) {
        // Le navigateur s'ouvre dans le dossier du DERNIER son charge — celui
        // que l'utilisateur a choisi — ou a la racine de la carte s'il n'y en
        // a pas encore. On ne cree aucun dossier a sa place.
        lisCfg();
        { DIR *x = opendir(dossierSamples);
          if (x) closedir(x); else strcpy(dossierSamples, "/"); }
        strcpy(dossier, dossierSamples);
        navGenre = 1; scanne(); navigateur = true;
        // Le curseur se pose sur le son deja charge dans l'instrument.
        { const int ech = md_replayer_get_instr_sample(instrId);
          char nm[24]; uint32_t lg; int bcl, bn;
          if (ech >= 0 && ech < MD_MAX_SAMPLES &&
              md_replayer_get_sample(ech, nm, 24, &lg, &bcl, &bn))
            for (int i = 0; i < nbFichiers; i++)
              if (!strncasecmp(fichiers[i], nm, 23)) { selFichier = i; break; } }
        pageRetour = PAGE_INSTR;
        page = PAGE_PROJECT; pageVue = -1;
      }
      navFerme = false;

      // Une touche d'edition a-t-elle ete frappee ? Si oui, la page SONG doit
      // se redessiner meme si le curseur n'a pas bouge.
      if (frappe & (KEY_A | KEY_X)) modifie = true;
      if (aTenu && (appui & (KEY_UP|KEY_DOWN|KEY_LEFT|KEY_RIGHT))) modifie = true;

      // START lance et arrete la lecture, depuis n'importe quelle page.
#ifdef FM1
      // FM-1 : PLAY / STOP, une seule regle. Quelque chose joue (le tracker, la DRUM MACHINE, la boucle du
      // LOOPER) : il l'arrete, tout ce qui joue. Rien ne joue : il lance ce que la page montre (le tracker sur
      // ses pages, la boite sur DRUM, la boucle sur LOOPER) ; ailleurs (FX, PRESET, HELP...), le mode de SEQ.
      // (Pendant REC, le tracker garde sa regle, plus bas.)
      if ((frappe & KEY_START) && !recEtat) {
        const bool boucleJoue = g_loopEtat == 1 || g_loopEtat == 2 || g_loopEtat == 3 || g_loopEtat == 5;
        if (enLecture || g_drumJoue || boucleJoue) {
          if (g_drumJoue) drumArrete();
          if (boucleJoue) {
            g_loopEtat = g_loopEtat == 1 || g_loopEtat == 5 ? 0 : 4;   // (une prise pas fermee : abandonnee)
            loopSilence();
          }
          if (!enLecture) frappe &= ~KEY_START;        // (le tracker qui joue : il s'arrete plus bas)
        } else {
          const bool pageTracker = page == PAGE_SONG || page == PAGE_CHAIN || page == PAGE_PHRASE ||
                                   page == PAGE_INSTR || page == PAGE_TABLE || page == PAGE_PROJECT;
          const int quoi = page == PAGE_DRUM ? 1 : page == PAGE_LOOPER ? 2 : pageTracker ? 0
                         : fm1_drum() ? 1 : fm1_looper() ? 2 : 0;
          if (quoi == 1) {                             // la boite
            g_drumPas = 15; g_drumProchain = fm1_echantillons(); g_drumJoue = 1;
            frappe &= ~KEY_START;
          } else if (quoi == 2) {                      // la boucle
            if (g_loopEtat == 4) {
              g_loopT0 = fm1_echantillons(); loopRepart(); g_loopEtat = 2;
              fm1_annonce2("LOOPER", "PLAY");
            } else fm1_annonce2("LOOPER", "EMPTY");
            frappe &= ~KEY_START;
          }                                            // (sinon le tracker part, plus bas)
        }
      }
#endif
      if (frappe & KEY_START) {
        { char e[110];
          siprintf(e, "T%04u  %s  PAGE %c  LIGNE %02X", (unsigned)secondes,
                   enLecture ? "STOP" : "PLAY", "SCPIJTPMRBGD"[page < 12 ? page : 0], curLigne);
          journal(e);
          if (!enLecture) {
            // Ce que chaque piste va REELLEMENT jouer : le chain, la phrase,
            // sa premiere ligne, et le type de l'instrument.
            for (int c = 0; c < MD_MAX_CHANNELS; c++) {
              const uint8_t vc = md_replayer_get_song(c, curLigne);
              if (vc == MD_EMPTY) continue;
              uint8_t ph = MD_EMPTY; int8_t tr = 0;
              md_replayer_get_chain(vc, 0, &ph, &tr);
              uint8_t no=0,i2=0,ve=0,cm=MD_EMPTY,cv=0,mc=MD_EMPTY,mv=0;
              if (ph != MD_EMPTY)
                md_replayer_get_phrase(ph, 0, &no,&i2,&ve,&cm,&cv,&mc,&mv);
              char e2[150];
              const int n = siprintf(e2, "  %s CHAIN %02X PHRASE %02X L0 NOTE %02X"
                                         " INS %02X VEL %02X",
                                     noms[c], vc, ph, no, i2, ve);
              if (i2)
                siprintf(e2 + n, "  %s CANAL %d",
                         genreInstr(i2) == GENRE_MIDI ? "MIDI" : "SAMPLE",
                         md_replayer_get_instr_midi_channel(i2) + 1);
              journal(e2);
            }
            journalVoies = 2;
          }
        }
        if (enLecture) { md_replayer_stop(); enLecture = false; }
        else {
          // ── La lecture est CONTEXTUELLE, comme sur LSDJ et sur l'iPad ─────
          // Depuis SONG on joue toute la chanson ; depuis CHAIN, seulement le
          // chain pointe ; depuis PHRASE, seulement la phrase pointee. Dans
          // ces deux cas une seule voie sonne — celle de la colonne ou etait
          // le curseur — et la lecture boucle sur elle-meme.
          //
          // Le moteur savait deja le faire : je n'avais jamais branche
          // md_replayer_set_play_scope, et START jouait donc toujours la
          // chanson entiere, quelle que soit la page.
          if (page == PAGE_CHAIN) {
            md_replayer_set_play_scope(MD_SCOPE_CHAIN, voieCourante, chainId);
            md_replayer_set_scope_row(chLigne);
            md_replayer_play_from(0);
#ifdef FM1
            lectureScope = 1;
#endif
          } else if (page == PAGE_PHRASE || page == PAGE_INSTR ||
                     page == PAGE_TABLE) {
#ifdef FM1
            lectureScope = 2;
#endif
            // INSTR joue comme PHRASE : en SOLO, la phrase d'ou l'on est
            // descendu, sur sa seule voie. On regle un instrument pour
            // l'entendre dans son contexte — lancer la chanson entiere n'a
            // aucun sens ici, et c'est pourtant ce que faisait la branche
            // « sinon » ou la page INSTR tombait.
            md_replayer_set_play_scope(MD_SCOPE_PHRASE, voieCourante, phraseId);
            md_replayer_set_scope_chain(chainId, chLigne);   // (son TSP)
            md_replayer_set_scope_row(page == PAGE_PHRASE ? phLigne : 0);

            // Depuis la TABLE aussi : on y regle une table pour l'entendre
            // dans sa phrase, pas pour lancer la chanson entiere.
            md_replayer_play_from(0);
          } else {
            md_replayer_set_play_scope(MD_SCOPE_SONG, 0, 0);
            md_replayer_play_from(page == PAGE_SONG ? curLigne : 0);
#ifdef FM1
            lectureScope = 0;
#endif
          }
          md_replayer_play();
          enLecture = true;
          suiviLecture = true;                    // (FM-1 : la vue suit la lecture)
        }
      }
    }
    // La vue suit le curseur sans jamais le coller au bord.
#ifdef FM1
    // ... ou, pendant la lecture sans qu'on y touche, la ligne qui joue.
    int ligneSuivie = curLigne;
    if (enLecture && suiviLecture) {
      const int rj = md_replayer_play_song_row(reel(curCanal));
      if (rj >= 0) ligneSuivie = rj;
    }
    if (ligneSuivie < haut + 2) haut = ligneSuivie - 2;
    if (ligneSuivie > haut + kLignesVues - 3) haut = ligneSuivie - kLignesVues + 3;
#else
    if (curLigne < haut + 2) haut = curLigne - 2;
    if (curLigne > haut + kLignesVues - 3) haut = curLigne - kLignesVues + 3;
#endif
    if (haut < 0) haut = 0;
    if (haut > MD_SONG_ROWS - kLignesVues) haut = MD_SONG_ROWS - kLignesVues;
    // Et en largeur : les pistes qui manquent a droite apparaissent au fur et
    // a mesure que le curseur avance au-dela du bord de l'ecran.
    if (curCanal < gaucheVoie) gaucheVoie = curCanal;
    if (curCanal > gaucheVoie + kVoiesEcran - 1) gaucheVoie = curCanal - kVoiesEcran + 1;
    if (gaucheVoie > nbColonnes() - kVoiesEcran) gaucheVoie = nbColonnes() - kVoiesEcran;
    if (gaucheVoie < 0) gaucheVoie = 0;
    const int nbVoiesVues = (nbColonnes() - gaucheVoie < kVoiesEcran)
                          ? nbColonnes() - gaucheVoie : kVoiesEcran;

    // ── Le centrage, valable pour TOUTES les pages ──────────────────────
    // Chaque page a sa largeur utile ; on la centre en repartissant l'espace
    // restant de part et d'autre. L'en-tete et le titre de page, eux, se
    // calent sur les bords et remettent le decalage a zero.
    {
      int largeur;
      switch (page) {
#ifdef FM1
      case PAGE_SONG:    largeur = kCols; break;   // FM-1 : calee a gauche, ses 4 pistes tiennent
#else
      case PAGE_SONG:    largeur = kColVoie + nbVoiesVues * kPasVoie; break;
#endif
      // CHAIN et PHRASE restent CALES A GAUCHE : leurs deux ou cinq colonnes
      // etroites, centrees, flottaient au milieu d'un grand vide et l'oeil
      // perdait le bord de reference.
      case PAGE_CHAIN:   largeur = kCols; break;
      case PAGE_PHRASE:  largeur = kCols; break;
      case PAGE_INSTR:   largeur = kCols; break;   // collee a gauche
      case PAGE_TABLE:   largeur = kCols; break;   // collee a gauche
#ifdef FM1
      default:           largeur = kCols; break;   // FM-1 : cale a gauche, l'ecran est etroit
#else
      default:           largeur = 30; break;   // le menu du projet
#endif
      }
      const int c2 = (kCols - largeur) / 2;
      g_colOrigine = c2 > 0 ? c2 : 0;
#ifdef FM1
      // FM-1 : PHRASE recule d'une colonne. Le premier chiffre du numero de
      // ligne sort de l'image (il ne reste que 0..F), et tout se decale a gauche.
      if (page == PAGE_PHRASE) g_colOrigine = -1;
#endif
    }

    // ── Redessin ────────────────────────────────────────────────────────
    static int vuCanal = -1, vuLigne = -1, vuHaut = -1;

    // Vrai quand trame() vient d'effacer l'ecran. Les blocs qui ne repeignent
    // que sur changement DOIVENT le savoir : tester « pageVue != page » ne
    // servait a rien, puisque pageVue est mis a jour dans ce bloc-ci, avant
    // eux. C'est ce qui laissait la page CHAIN vide en y revenant.
    bool ecranEfface = false;
    if (page != pageVue) {
      ecranEfface = true;
      // Changement de page : on repeint tout, en-tete compris.
      const int decalSauve = g_colOrigine;
      g_colOrigine = 0;                  // l'en-tete se cale sur les bords

      // ⚠️ trame() ne repeint que l'ecran DU HAUT. L'ecran du bas garde donc
      // ce que la page precedente y avait laisse — et la page instrument FM y
      // ecrit ses reglages generaux. En changeant de page ils restaient
      // affiches sous une page qui n'a rien a voir.
      //
      // On le vide donc ICI, au moment precis du changement, plutot que de le
      // deduire d'un drapeau garde d'une image sur l'autre. La rangee 2 est
      // epargnee : elle porte le nom du morceau, qui appartient a cet ecran.
      ecran(bas);
      for (int l = 3; l < 19; l++) efface(0, l, 41);
      ecran(g_fond);

      trame();
      entretienSon();   // trame() couvre l'ecran entier : on rend la main au son
#ifndef FM1
      titre(0, 0, "OPL3 TRACKER", kTitre);
#endif
      // Le titre de page se colle au bord DROIT : il ne doit pas subir le
      // decalage de centrage, sinon il flotte au milieu de nulle part.
#ifdef FM1
      // FM-1 : le nom de la page va dans l'en-tete de l'ecran, en haut a droite.
      switch (page) {
      case PAGE_SONG:   fm1_nom_page("SONG");    break;
      case PAGE_CHAIN:  fm1_nom_page("CHAIN");   break;
      case PAGE_PHRASE: fm1_nom_page("PHRASE");  break;
      case PAGE_INSTR:  fm1_nom_page("INSTR");   break;
      case PAGE_TABLE:  fm1_nom_page("");        break;   // (sa ligne dit deja « TABLE 00 »)
      case PAGE_MIDICIBLES: fm1_nom_page("MIDI TARGET"); break;
      case PAGE_PRESET: fm1_nom_page("PRESET");  break;
      case PAGE_BEND:   fm1_nom_page("BENDING"); break;
      case PAGE_GLITCH: fm1_nom_page("GLITCH");  break;
      case PAGE_BEND2:  fm1_nom_page("BENDING 2"); break;
      case PAGE_DRUM:   fm1_nom_page("DRUM");    break;
      case PAGE_MASTER: fm1_nom_page("MASTER");  break;
      case PAGE_GLOBAL: fm1_nom_page("KNOBS");   break;
      case PAGE_AIDE:   fm1_nom_page("HELP");    break;
      case PAGE_LOOPER: fm1_nom_page("LOOPER");  break;
      default:          fm1_nom_page("PROJECT"); break;
      }
#else
      switch (page) {
      case PAGE_SONG:   titre(kCols - 4, 0, "SONG",    kTitre); break;
      case PAGE_CHAIN:  titre(kCols - 5, 0, "CHAIN",   kTitre); break;
      case PAGE_PHRASE: titre(kCols - 6, 0, "PHRASE",  kTitre); break;
      case PAGE_INSTR:  titre(kCols - 5, 0, "INSTR",   kTitre); break;
      case PAGE_TABLE:  titre(kCols - 5, 0, "TABLE",   kTitre); break;
      case PAGE_MIDICIBLES: titre(kCols - 11, 0, "MIDI TARGET", kTitre); break;
      default:          titre(kCols - 7, 0, "PROJECT", kTitre); break;
      }
#endif
      g_colOrigine = decalSauve;         // le contenu de la page, lui, est centre

      // Le decalage vertical des noms de colonnes, a poser JUSTE autour de
      // leur rangee. Laisse actif pour tout le bloc, il decalait aussi les
      // intitules des pages INSTR — qui se retrouvaient une demi-ligne plus
      // bas que leurs valeurs, dessinees ailleurs sans ce decalage.
      const int decalNoms = FM1_DECAL_NOMS((MD_GLYPHE_H + 3 * MD_CELL_H) / 2 - MD_GLYPHE_H / 2
                            - MD_CELL_H);

      if (page == PAGE_SONG) {
        // Les noms de pistes se dessinent avec la grille, plus bas : ils
        // defilent avec elle.
      } else if (page == PAGE_CHAIN) {
        // PHR et TSP, cote a cote : ce sont les intitules de l'iPad, et deux
        // colonnes de deux chiffres n'ont pas a etre eloignees.
        // Colle au bord gauche : numero de ligne, chevron, puis les deux
        // colonnes de deux chiffres separees d'une seule colonne de vide.
        g_decalYpx = decalNoms;
        texte(3, 1, "PHR", kTitre);
        texte(6, 1, "TSP", kTitre);
#ifdef FM1
        fm1_titres_colonnes(1);   // FM-1 : cette rangee s'affiche en petit
#endif
        g_decalYpx = 0;
      } else if (page == PAGE_INSTR) {
        // Tout se dessine avec les valeurs, plus bas : les intitules changent
        // avec le type de l'instrument.
      } else if (page == PAGE_PHRASE) {
        // Les mots entiers ne tiennent plus : la police doublee coute la
        // moitie de la largeur. On revient aux abreviations — c'est le prix
        // de la lisibilite des chiffres, qui etaient illisibles avant.
        g_decalYpx = decalNoms;
        // Au-dessus de colonnes resserrees : des intitules courts, pour qu'ils
        // ne se touchent pas.
#ifndef FM1
        texte(2,  1, "NOTE", kTitre);
        texte(7,  1, "IN", kTitre);
#endif
#ifdef FM1
        // FM-1 : pas de colonne VL ; les suivantes se rapprochent.
        // Ni FX3 ni FX4 : deux effets, une case d'ecart entre chacun.
        // (En ZOOM, seulement les groupes montres, decales : voir phGauche.)
        efface(0, 1, kCols);
        for (int g = 0; g < 5; g++)
          if (phVisible(g))
            texte(kPhGrpTitre[g] - phDecal(phGauche) , 1, kPhGrpNom[g], kTitre);
#else
        texte(10, 1, "VL", kTitre);
        texte(13, 1, "CMD", kTitre);
        texte(17, 1, "FX1", kTitre);
        texte(22, 1, "FX2", kTitre);
        texte(27, 1, "FX3", kTitre);
        texte(32, 1, "FX4", kTitre);
#endif
#ifdef FM1
        fm1_titres_colonnes(1);   // FM-1 : cette rangee s'affiche en petit
#endif
        g_decalYpx = 0;
      } else {
        // Le menu se redessine a chaque image, plus bas : il change avec le
        // curseur. Rien d'autre ici — les touches se trouvent toutes seules.
      }
      g_decalYpx = 0;
      pageVue = page;
      vuCanal = -1; vuLigne = -1; vuHaut = -1;
      for (int c = 0; c < MD_MAX_CHANNELS; c++) repereVu[c] = -1;
      repereLigne = -1;   // trame() vient d'effacer l'ancien chevron
      bpmVu = -1;
    }

    // ── La fenetre de nom ───────────────────────────────────────────────
        if (page == PAGE_PROJECT && navigateur) {
      g_colOrigine = 0;   // une liste de fichiers occupe toute la largeur
      // On ne repeint que si la liste ou le curseur ont bouge. Redessinee a
      // chaque image, elle raye l'ecran de bandes pendant la lecture — les
      // ecritures courent apres le balayage — et vole du temps au son.
      static int vuSel = -1, vuHautF = -1, vuNb = -1, vuGenre = -1;
      const bool refaireListe = ecranEfface || selFichier != vuSel ||
                                hautFichier != vuHautF || nbFichiers != vuNb ||
                                navGenre != vuGenre;
      vuSel = selFichier; vuHautF = hautFichier; vuNb = nbFichiers;
      vuGenre = navGenre;
      if (refaireListe) {
      // La liste tient en douze lignes ; elle defile avec le curseur.
#ifdef FM1
      // FM-1, en ZOOM : six lignes, et tout est coupe a la largeur de l'ecran
      // (douze cases) : la vue reste fixe, c'est la liste qui defile.
      const int kVues = zoomNiv == 2 ? 6 : zoomNiv ? 8 : 12, kLargeZoom = zoomNiv == 1 ? 15 : 12;
#else
      const int kVues = 12;
#endif
      if (selFichier < hautFichier) hautFichier = selFichier;
      if (selFichier > hautFichier + kVues - 1) hautFichier = selFichier - kVues + 1;
      efface(0, 2, 41);
      { // Le chemin, precede de « SD: » pour qu'on sache qu'il s'agit de la
        // carte, en MAJUSCULES — la police n'a pas de minuscules, un
        // « sd:/roms » s'affichait comme des trous — et par la FIN quand il
        // est trop long : c'est le dossier ou l'on est qui compte.
        char plein[200]; int n = 0;
        if (dossier[0] == '/') { plein[n++] = 'S'; plein[n++] = 'D'; plein[n++] = ':'; }
        for (int j = 0; dossier[j] && n < 199; j++) {
          char c2 = dossier[j];
          if (c2 >= 'a' && c2 <= 'z') c2 = (char)(c2 - 'a' + 'A');
          plein[n++] = c2;
        }
        plein[n] = 0;
#ifdef FM1
        const int place = zoomNiv ? kLargeZoom : kCols - 1;
#else
        const int place = kCols - 1;
#endif
        char vu[48];
        if (n > place) { vu[0] = '.'; vu[1] = '.';
                         strcpy(vu + 2, plein + n - (place - 2)); }
        else strcpy(vu, plein);
        texte(0, 2, vu, kEntete); }
      for (int i = 0; i < kVues; i++) {
        const int k = hautFichier + i;
        efface(0, 4 + i, 41);
        if (k >= nbFichiers) continue;
        char l[44];
        l[0] = (k == selFichier) ? '>' : ' ';
        l[1] = (typeFic[k] == 2) ? '/' : ' ';   // les dossiers se voient
        // En MAJUSCULES : la police ne contient que l'espace au souligne, et
        // toute minuscule s'affichait comme un vide. « Boot.nds » devenait
        // « B » suivi de trous, ce qui rendait la liste incomprehensible.
        int j = 0;
#ifdef FM1
        const int lmax = zoomNiv ? kLargeZoom - 2 : 40;   // (ZOOM : le nom coupe, la vue ne bouge pas)
#else
        const int lmax = 40;
#endif
        for (; j < lmax && fichiers[k][j]; j++) {
          char c2 = fichiers[k][j];
          if (c2 >= 'a' && c2 <= 'z') c2 = (char)(c2 - 'a' + 'A');
          else if ((unsigned char)c2 < 32 || (unsigned char)c2 > 95) c2 = '?';
          l[2 + j] = c2;
        }
        l[2 + j] = 0;
        // Un morceau ouvrable ou un dossier ressort ; le reste est la pour
        // qu'on sache ou l'on est, pas pour etre choisi.
        const u16 couleur = (k == selFichier) ? kAccent
                          : (typeFic[k] != 0) ? kSelection : kAttenue;
        texte(0, 4 + i, l, couleur);
      }
      if (nbFichiers == 0) titre(0, 4, "EMPTY", kAttenue);
      // Ce qu'on est venu chercher, et ce que font les touches.
      efface(0, 17, 41);
#ifdef FM1
      // (21 cases sans ZOOM, 15 en 4 COL : les phrases plus longues etaient coupees a droite)
      if (zoomNiv) titre(0, 17, navSuppr ? "DELETE: A  B" : "A OK  B BACK", kAttenue);
      else titre(0, 17, navSuppr ? "DELETE: A  B BACK" : "A OPEN  B BACK", kAttenue);
#else
      titre(0, 17, navGenre == 1 ? "LOAD SAMPLE : A LOAD   B BACK"
                                 : "LOAD SONG : A OPEN   B BACK", kAttenue);
#endif
      }
#ifdef FM1
      // DELETE : la question, a la place de la liste. YES ou NO, le choisi en
      // rouge ; A le valide, B annule.
      if (demandeSuppr) {
        static bool vuOui = false;
        if (refaireListe || vuOui != supprOui) {
          vuOui = supprOui;
          for (int l = 2; l < kLignes; l++) efface(0, l, 41);
          const int lg = zoomNiv == 2 ? 12 : 15;
          char nom[16]; strncpy(nom, fichiers[selFichier], lg); nom[lg] = 0;
          titre(0, 2, "DELETE", kTitre);
          texte(0, 3, nom, kSelection);
          titre(0, 5, zoomNiv == 2 ? "SURE?" : "ARE YOU SURE?", kData);
          texte(0, 6, "NO", supprOui ? kVide : kAccent);
          texte(5, 6, "YES", supprOui ? kAccent : kVide);
          titre(0, 8, zoomNiv ? "A OK B BACK" : "A CONFIRM   B CANCEL", kAttenue);
        }
      }
#endif
    }
    // Les messages prennent la place du menu, pas le bas de l'ecran, et ont
    // leur propre effacement. Ils ne se repeignent QUE s'ils changent : une
    // question figee redessinee a chaque image donnait les memes bandes de
    // vieux televiseur que le menu, si la lecture tournait derriere.
    else if (page == PAGE_PROJECT && demandeNouveau) {
      static bool vuQuestion = false;
      const bool refaireQ = ecranEfface || !vuQuestion;
      vuQuestion = true;
      if (refaireQ) {
      g_colOrigine = 0;
      static const char *msgNouveau[6] = {
        "START A NEW SONG ?", "",
        "THE SONG IN MEMORY WILL BE",
        "ERASED AND CANNOT BE GOT BACK.", "",
        "A YES          B NO" };
      for (int i = 0; i < 6; i++) {
        efface(0, 4 + i, 41);
        titre(0, 4 + i, msgNouveau[i], (i == 0 || i == 5) ? kEntete : kAttenue);
      }
      }
    }
    // ── LA PAGE ABOUT ─────────────────────────────────────────────────
    // L'auteur, et la mention des bibliotheques tierces : leurs licences (MIT,
    // zlib, BSD) demandent que leur avis accompagne le programme. Le detail
    // est dans TIERS.md.
    else if (page == PAGE_APROPOS) {
      static bool vuApropos = false;
      if (ecranEfface || !vuApropos) {
        vuApropos = true;
        static const char *lignes[] = {
#ifdef FM1
          // FM-1 : le firmware est sous GPL-3.0 (il lie le HAL de Felucca).
          "OPLBEND TRACKER",
          "FOR M-VAVE FM-1",
          "",
          "(C) 2026 AUDREN THIBAULT",
          "GPL-3.0 LICENCE",
          "",
          "BASED ON FELUCCA (GPL-3.0)",
          "NOTICES : LICENSES",
          "",
          "B  BACK"
#else
          "OPL3 TRACKER",
          "RETROWAVE OPL3, ANBERNIC RG350",
          "",
          "COPYRIGHT (C) 2026 AUDREN THIBAULT",
          "ALL RIGHTS RESERVED.",
          "",
          "TINYUSB : MIT LICENCE",
          "(C) HA THACH, TINYUSB.ORG",
          "NRIO USB : MIT LICENCE",
          "(C) ADRIAN SIEKIERKA",
          "LIBNDS : ZLIB LICENCE",
          "LIBFAT : BSD LICENCE",
          "FULL NOTICES : TIERS.MD",
          "",
          "B  BACK"
#endif
        };
        ecran(g_fond);
        const int sauve = g_colOrigine; g_colOrigine = 0;
        titre(2, 1, "ABOUT", kEntete);
        for (int i = 0; i < (int)(sizeof(lignes) / sizeof(lignes[0])); i++)
          texte(2, 3 + i, lignes[i],
                (i == 0) ? kAccent : (i == 3) ? kTitre : kData);
        g_colOrigine = sauve;
      }
      if (frappe & KEY_B) { page = PAGE_PROJECT; pageVue = -1; vuApropos = false; }
    }
#ifdef FM1
    // ── LA PAGE PRESET (bouton SEL) ────────────────────────────────────────
    // La liste des presets (Amber, puis AdLib Tracker II), le choisi en rouge :
    // la vue le suit. PRESETS (ou haut / bas) en change, plus haut.
    else if (page == PAGE_PRESET) {
      static int vuSel = -1, vuIns = -1, vuZm = -1;
      if (ecranEfface || presetSel != vuSel || presetIns != vuIns || zoomNiv != vuZm) {
        vuSel = presetSel; vuIns = presetIns; vuZm = zoomNiv;
        const int sauve = g_colOrigine; g_colOrigine = 0;
        { char rp[12];
          siprintf(rp, "INS %02X", presetIns & 0xFF);
          efface(fm1_entete_cellules() - 8, 0, 8);
          texte(COL_REPERE(rp), 0, rp, kEntete); }
        // Juste les lignes qui tiennent a l'ecran (15 sans ZOOM, 9 en 4 COL, 7 en
        // 3 COL), le choisi en troisieme ligne (en ZOOM, la premiere) : la vue n'a pas a defiler, il ne
        // sort jamais de l'ecran, et le nom en gros (au milieu) ne le cache pas.
        const int kVues = zoomNiv == 2 ? 7 : zoomNiv ? 9 : 15;
        for (int l = 2; l < kLignes; l++) efface(0, l, 41);
        // le curseur descend la liste ; elle ne defile que quand il en depasse le bas (ou le haut)
        static int haut0 = 0;
        if (presetSel < haut0) haut0 = presetSel;
        if (presetSel > haut0 + kVues - 1) haut0 = presetSel - kVues + 1;
        if (haut0 > FM1_NB_PRESETS - kVues) haut0 = FM1_NB_PRESETS - kVues;
        if (haut0 < 0) haut0 = 0;
        const int larg = zoomNiv == 2 ? 12 : zoomNiv ? 15 : 21;
        fm1_rangee_espace(1);
        for (int i = 0; i < kVues; i++) {
          const int k = haut0 + i, lig = 2 + i;
          efface(0, lig, 41);
          if (k >= FM1_NB_PRESETS) continue;
          char t[28];
          if (zoomNiv) siprintf(t, "%s", FM1_PRESET_NOMS[k]);
          else siprintf(t, "%04d %s", k + 1, FM1_PRESET_NOMS[k]);
          t[larg] = 0;
          texte(0, lig, t, k == presetSel ? kAccent : kData);
        }
        g_colOrigine = sauve;
      }
    }
#endif
#ifdef FM1
    // ── LA PAGE CIRCUIT BENDING (bouton FX) ────────────────────────────────
    // Les quatre switches, KNOB 1 a 4 : comme des fils soudes sur le bus de la
    // puce FM d'un PSS (src/opl3.c fm1_bend). Mis, en rouge.
    else if (page == PAGE_BEND) {
      // Les sept effets, celui du curseur en rouge (mode tracker). En mode
      // KEYS, la molette de chacun devant lui : K1..K4, SEL, PRE, ALG.
      // A droite de chaque effet, ses pistes (celles qui ont de la musique) :
      // vertes quand il y agit, effacees sinon ; gauche / droite les font
      // defiler, comme les pistes de SONG.
      static int vuBits = -1, vuZm = -1, vuSel = -1, vuCl = -1, vuCol = -1, vuNb = -1;
      static uint32_t vuPistes[7];
      const int cl = fm1_clavier();
      if (ecranEfface || bendBits != vuBits || zoomNiv != vuZm || bendSel != vuSel || cl != vuCl ||
          bendCol != vuCol || bendNbPistes != vuNb || memcmp(vuPistes, bendPistes, sizeof vuPistes)) {
        vuBits = bendBits; vuZm = zoomNiv; vuSel = bendSel; vuCl = cl; vuCol = bendCol; vuNb = bendNbPistes;
        memcpy(vuPistes, bendPistes, sizeof vuPistes);
        static const char *kMol[7] = { "K1", "K2", "K3", "K4", "SEL", "PRE", "ALG" };
        const int sauve = g_colOrigine; g_colOrigine = 0;
        for (int l = 1; l < kLignes; l++) efface(0, l, 41);
        // Le titre, en ZOOM aussi (gros comme le reste) : en 3 COL, douze cases.
        const int l0 = zoomNiv ? 2 : 3;
        titre(0, 1, zoomNiv == 2 ? "CIR BENDING" : "CIRCUIT BENDING", kTitre);
        if (!zoomNiv) fm1_rangee_espace(2);
        const int large = zoomNiv == 2 ? 12 : zoomNiv ? 15 : 21;   // (les cases visibles)
        const int p0 = cl ? 10 : 6;                                // « K1  1 DET », « 1 DET »
        int lc = 3;                                                // « T1 », ou « T10 » : une case de plus
        for (int i = 0; i < bendNbPistes; i++) if (bendPiste[i] >= 9) lc = 4;
        int nv = (large - p0 + 1) / lc;
        if (nv < 1) nv = 1;
        if (bendCol > 0 && bendCol - 1 < bendGauche) bendGauche = bendCol - 1;
        if (bendCol > 0 && bendCol - 1 >= bendGauche + nv) bendGauche = bendCol - nv;
        if (bendGauche > bendNbPistes - nv) bendGauche = bendNbPistes - nv;
        if (bendGauche < 0) bendGauche = 0;
        for (int k = 0; k < 7; k++) {
          const bool mis = (bendBits >> k) & 1;
          char t[16];
          if (cl) siprintf(t, "%-3s %c %s", kMol[k], mis ? '1' : '0', kBendCourt[k]);
          else siprintf(t, "%c %s", mis ? '1' : '0', kBendCourt[k]);
          texte(0, l0 + k, t, (!cl && k == bendSel && bendCol == 0) ? kAccent : mis ? kSelection : kData);
          if (cl || k != bendSel || bendCol != 0)          // le nom de l'effet, a la couleur de sa molette
            texte(cl ? 6 : 2, l0 + k, kBendCourt[k], kEffet[k]);
          for (int i = bendGauche; i < bendNbPistes && i < bendGauche + nv; i++) {
            const int c = bendPiste[i], col = p0 + (i - bendGauche) * lc;
            char p[4];
            siprintf(p, "T%d", c + 1);
            texte(col, l0 + k, p, ((bendPistes[k] >> c) & 1u) ? kVert : kVide);
            if (!cl && k == bendSel && bendCol - 1 == i) {   // le curseur : un cadre rouge, ouvert en haut
              // (au-dessus de la lettre, c'est l'interligne de la rangee d'avant : l'ecran l'agrandit)
              const int x0 = col * MD_CELL_W - 2, x1 = (col + (c >= 9 ? 3 : 2)) * MD_CELL_W;
              const int y0 = (l0 + k) * MD_CELL_H, y1 = (l0 + k) * MD_CELL_H + MD_GLYPHE_H;
              for (int x = x0; x <= x1; x++) pixel(x, y1, kAccent);
              for (int y = y0; y <= y1; y++) { pixel(x0, y, kAccent); pixel(x1, y, kAccent); }
            }
          }
        }
        g_colOrigine = sauve;
      }
    }
    // ── LA PAGE GLITCH (FX, second appui) ──────────────────────────────────
    // Une LISTE, pas une grille (on la confondait avec la DRUM MACHINE) : les
    // 16 pas, leur numero et leur effet en toutes lettres, chaque effet sa
    // couleur ; le curseur en rouge. (Pas de fleche sur le pas qui joue : elle
    // restait parfois affichee, et la redessiner a chaque pas coutait.)
    else if (page == PAGE_GLITCH) {
      static int vuSel = -1, vuZm = -1, vuSom = -1, vuOn = -1;
      // (theme aubergine : RTG peche, STU jaune, REV turquoise, TAP bleu, OCT rose, GAT vert, CRU corail
      // — pas le rouge du curseur —, FLT violet)
      static const u16 kCoul[9] = { kVide, rvb(31, 20, 14), rvb(31, 27, 13), rvb(13, 28, 28), rvb(9, 19, 31),
                                    rvb(31, 13, 23), rvb(6, 28, 15), rvb(31, 15, 12), rvb(22, 17, 31) };
      int som = 0;
      for (int k = 0; k < 16; k++) som = som * 7 + glitchPas[k];
      if (ecranEfface || glitchSel != vuSel || zoomNiv != vuZm || som != vuSom || (int)glitchOn != vuOn) {
        vuSel = glitchSel; vuZm = zoomNiv; vuSom = som; vuOn = glitchOn;
        const int sauve = g_colOrigine; g_colOrigine = 0;
        for (int l = 1; l < kLignes; l++) efface(0, l, 41);
        titre(0, 1, "GLITCH", kTitre);                  // et s'il agit : ON en vert, OFF efface (KNOB 1)
        texte(zoomNiv == 2 ? 7 : 8, 1, glitchOn ? "ON" : "OFF", glitchOn ? kVert : kVide);
        if (!zoomNiv) fm1_rangee_espace(2);
        for (int k = 0; k < 16; k++) {
          char t[20];
          siprintf(t, "%02d", k + 1);
          texte(0, 3 + k, t, k == glitchSel ? kAccent : kData);   // (colle a gauche : plus de fleche devant)
          texte(3, 3 + k, zoomNiv == 2 ? kGlitchCourt[glitchPas[k]] : kGlitchNom[glitchPas[k]],
                k == glitchSel ? kAccent : kCoul[glitchPas[k]]);
        }
        g_colOrigine = sauve;
      }
    }
    // ── LA PAGE KNOB GENERAL (FX : apres GLITCH, avant MASTER FX) ──────────
    // Les molettes de SONG, mais pour tous les sons : ce que chacune regle, et
    // de combien elle a deja tout pousse.
    else if (page == PAGE_GLOBAL) {
      static int vuSom = -1, vuZm = -1;
      int som = selPos;
      for (int k = 0; k < 6; k++) som = som * 61 + globalPousse[k];
      if (ecranEfface || som != vuSom || zoomNiv != vuZm) {
        vuSom = som; vuZm = zoomNiv;
        static const char *kMol[6] = { "K1", "K2", "K3", "K4", "PRE", "ALG" };
        static const char *kQuoi[6] = { "MOD OUT", "LENGTH", "FEEDBACK", "MOD MULT", "OP1 WAVE", "OP2 WAVE" };
        const int sauve = g_colOrigine; g_colOrigine = 0;
        for (int l = 1; l < kLignes; l++) efface(0, l, 41);
        const int l0 = zoomNiv ? 2 : 3;
        titre(0, 1, zoomNiv == 2 ? "ALL SOUNDS" : "KNOB GENERAL", kTitre);
        if (!zoomNiv) fm1_rangee_espace(2);
        for (int k = 0; k < 6; k++) {
          char t[28];
          if (zoomNiv == 2) siprintf(t, "%-3s %+d", kMol[k], globalPousse[k]);
          else siprintf(t, "%-3s %-8s %+d", kMol[k], kQuoi[k], globalPousse[k]);
          texte(0, l0 + k, t, globalPousse[k] ? kSelection : kData);
          const u16 ce = kEffet[k < 4 ? k : k + 1];     // la molette et ce qu'elle regle : sa couleur (PRE, ALG :
          texte(0, l0 + k, kMol[k], ce);                  // les memes que sur les autres pages, SEL entre les deux)
          if (zoomNiv != 2) texte(4, l0 + k, kQuoi[k], ce);
        }
        // « SEL FM » ou « SEL AM » ; dessous, comment revenir aux sons d'avant : EDIT
        texte(0, l0 + 6, "SEL", kEffet[4]);
        texte(4, l0 + 6, selPos ? "AM" : "FM", kSelection);
        titre(0, l0 + 7, "EDIT TO DEFAULT", kAttenue);
        if (!zoomNiv) titre(0, l0 + 8, "ALL SOUNDS AT ONCE", kAttenue);
        g_colOrigine = sauve;
      }
    }
    // ── LA PAGE MASTER FX (FX, troisieme appui) ────────────────────────────
    // Comme CIRCUIT BENDING : 0 / 1 et le nom de chaque effet, mis en jaune, le
    // curseur en rouge (mode tracker) ; en mode KEYS, sa molette devant lui.
    else if (page == PAGE_MASTER) {
      static int vuSom = -1, vuZm = -1, vuSel = -1, vuCl = -1;
      const int cl = fm1_clavier();
      int som = 0;
      for (int k = 0; k < 7; k++) som = som * 16 + mfxDose[k];
      if (ecranEfface || som != vuSom || zoomNiv != vuZm || mfxSel != vuSel || cl != vuCl) {
        vuSom = som; vuZm = zoomNiv; vuSel = mfxSel; vuCl = cl;
        static const char *kMol[7] = { "K1", "K2", "K3", "K4", "SEL", "PRE", "ALG" };
        const int sauve = g_colOrigine; g_colOrigine = 0;
        for (int l = 1; l < kLignes; l++) efface(0, l, 41);
        const int l0 = zoomNiv ? 2 : 3;
        titre(0, 1, "MASTER FX", kTitre);
        if (!zoomNiv) fm1_rangee_espace(2);
        // « K1  DISTORTION F ######## » : la dose en chiffre, et en barre sans zoom
        for (int k = 0; k < 7; k++) {
          const int v = mfxDose[k];
          char t[40], barre[9];
          const char *nom = zoomNiv ? kMfxCourt[k] : kMfxNom[k];
          int j = 0;
          for (; j < (v + 1) / 2; j++) barre[j] = '#';
          barre[j] = 0;
          if (cl) siprintf(t, zoomNiv ? "%-3s %-4s %X" : "%-3s %-10s %X %s", kMol[k], nom, v, barre);
          else siprintf(t, zoomNiv ? "%-4s %X" : "%-10s %X %s", nom, v, barre);
          t[zoomNiv == 2 ? 12 : zoomNiv ? 15 : 21] = 0;     // (la largeur de l'ecran)
          texte(0, l0 + k, t, (!cl && k == mfxSel) ? kAccent : v ? kSelection : kData);
          if (cl || k != mfxSel)                         // le nom de l'effet, a la couleur de sa molette
            texte(cl ? 4 : 0, l0 + k, nom, kEffet[k]);
        }
        g_colOrigine = sauve;
      }
    }
    // ── LA PAGE CIRCUIT BENDING 2 (FX, apres CIRCUIT BENDING) ──────────────
    // L'horloge de la puce, comme un LTC1799 soude a sa place : K1 le pitch (en demi-tons), K2 l'accordage
    // fin (en centiemes) ; l'un sous l'autre, comme sur CIRCUIT BENDING.
    else if (page == PAGE_BEND2) {
      static int vuP = -99999, vuF = -999, vuH = -99999, vuZm = -1;
      if (ecranEfface || bendPitch != vuP || bendFin != vuF || bendHorloge != vuH || zoomNiv != vuZm) {
        vuP = bendPitch; vuF = bendFin; vuH = bendHorloge; vuZm = zoomNiv;
        const int sauve = g_colOrigine; g_colOrigine = 0;
        for (int l = 1; l < kLignes; l++) efface(0, l, 41);
        titre(0, 1, zoomNiv ? "CIR BENDING 2" : "CIRCUIT BENDING 2", kTitre);
        if (!zoomNiv) fm1_rangee_espace(2);
        const int l0 = zoomNiv ? 2 : 3;
        char v[12];
        siprintf(v, "%c%d.%02d", bendPitch < 0 ? '-' : '+', abs(bendPitch) / 100, abs(bendPitch) % 100);
        texte(0, l0, "K1", kEffet[0]); texte(3, l0, "PITCH", kEffet[0]);
        texte(zoomNiv ? 9 : 15, l0, v, bendPitch ? kSelection : kData);
        siprintf(v, "%+d", bendFin);
        texte(0, l0 + 1, "K2", kEffet[1]); texte(3, l0 + 1, zoomNiv ? "FINE" : "FINETUNE", kEffet[1]);
        texte(zoomNiv ? 9 : 15, l0 + 1, v, bendFin ? kSelection : kData);
        siprintf(v, "%c%d.%02d", bendHorloge < 0 ? '-' : '+', abs(bendHorloge) / 100, abs(bendHorloge) % 100);
        texte(0, l0 + 2, "K3", kEffet[2]); texte(3, l0 + 2, zoomNiv ? "CLOCK" : "CLOCK PITCH", kEffet[2]);
        texte(zoomNiv ? 9 : 15, l0 + 2, v, bendHorloge ? kSelection : kData);
        g_colOrigine = sauve;
      }
    }
    // ── LA PAGE LOOPER (SEQ, troisieme mode) ────────────────────────────────
    // Comme la DRUM MACHINE : une rangee par couche (son numero, efface si elle est enlevee), seize pas sur
    // la duree de la boucle, une croix ou une note commence, le pas qui joue en rouge ; en D-PAD, le curseur,
    // et en haut a droite la note sous lui. (La boucle elle-meme : loopAudio, et REC / PLAY plus haut.)
    else if (page == PAGE_LOOPER) {
      const uint32_t pasLg = g_loopUnite * (uint32_t)loopParCase();   // (une case de la grille, en echantillons)
      const bool boucle = g_loopEtat >= 2 && g_loopEtat <= 4;
      // le pas qui joue, rangee par rangee (chacune sur sa longueur : STP/CUR)
      int pjL[kLoopVoies];
      for (int c = 0; c < kLoopVoies; c++)
        pjL[c] = (g_loopEtat == 2 || g_loopEtat == 3) ? (int)(((fm1_echantillons() - g_loopT0) % loopLgLigne(c)) / pasLg) : -1;
      const int pj = pjL[0] * 17 + pjL[1] * 289 + pjL[2] * 4913 + pjL[3] * 83521 + pjL[4] * 1419857 + pjL[5] * 7;
      const int cur = !fm1_clavier() ? loopCurC : -1;
      static uint8_t grille[kLoopVoies][16], note[kLoopVoies][16];
      uint32_t som = (uint32_t)g_loopMuet * 977u + (uint32_t)g_loopEtat * 7u + (uint32_t)g_loopPasG * 131u + (uint32_t)g_loopBtnC * 3u;
      for (int c = 0; c < kLoopVoies; c++) som = som * 17u + g_loopPasL[c];
      memset(grille, 0, sizeof grille);
      for (int i = 0; boucle && i < g_loopN; i++)
        if (g_loop[i].on == 1) {
          const int c = g_loop[i].couche % kLoopVoies, j = loopPasDe(g_loop[i].t, c) / loopParCase();
          if (j >= 0 && j < 16 && !grille[c][j]) {
            // (une batterie de DRUM KEYBOARD : le nom de sa touche, comme une note du clavier)
            const int dk = g_loop[i].ins - kDkIns0;
            grille[c][j] = 1;
            note[c][j] = (uint8_t)(dk >= 0 && dk < 27 ? borne(42 + dk + g_loop[i].note - kDk[dk].note, 1, MD_MAX_NOTE) : g_loop[i].note);
          }
        }
      for (int c = 0; c < kLoopVoies; c++)
        for (int j = 0; j < 16; j++) som = som * 31u + (grille[c][j] ? note[c][j] : 0u);
      static int vuPj = -2, vuCur = -2, vuL = -1, vuZm = -1; static uint32_t vuSom = 1;
      som = som * 7u + (uint32_t)loopLigneVue;
      if (ecranEfface || pj != vuPj || cur != vuCur || loopCurL != vuL || som != vuSom || zoomNiv != vuZm) {
        vuPj = pj; vuCur = cur; vuL = loopCurL; vuSom = som; vuZm = zoomNiv;
        const int sauve = g_colOrigine; g_colOrigine = 0;
        for (int l = 1; l < kLignes; l++) efface(0, l, 41);
        { char rp[12] = "";                        // la note sous le curseur
          if (cur > 0 && loopCurL < kLoopVoies && grille[loopCurL][cur - 1]) {
            static const char *gam[12] = {"C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"};
            const int n = note[loopCurL][cur - 1];
            siprintf(rp, "%s%d", gam[(n - 1) % 12], (n - 1) / 12);
          }
          efface(fm1_entete_cellules() - 8, 0, 8);
          if (rp[0]) texte(COL_REPERE(rp), 0, rp, kEntete); }
        titre(0, 1, "LOOPER", kTitre);
        fm1_rangee_espace(2);
        if (g_loopEtat == 0 || g_loopEtat == 1 || g_loopEtat == 5) {   // pas encore de boucle : ce qu'il faut faire
          titre(0, 5, g_loopEtat == 1 ? "RECORDING" : g_loopEtat == 5 ? "GET READY" : "PRESS REC",
                g_loopEtat == 1 ? kAccent : kData);
        } else
        for (int c = 0; c < kLoopVoies; c++) {
          const bool muet = (g_loopMuet >> c) & 1u;
          char nm[4]; siprintf(nm, "%d", c + 1);
          fm1_rangee_loop(3 + c);
          texte(0, 3 + c, nm, cur == 0 && c == loopCurL ? kAccent
                              : loopCurL == kLoopVoies + 1 && g_loopBtnC == 1 && c == loopLigneVue ? kSelection   // (celle que STP/CUR regle)
                              : muet ? kVide : kData);
          for (int j = 0; j < 16; j++) {
            const bool mis = grille[c][j];
            if (j * loopParCase() < g_loopPasL[c])     // (au-dela des pas de la rangee : rien)
              texte(3 + j, 3 + c, mis ? "X" : "-", j == pjL[c] ? kAccent : muet ? kVide : mis ? kVert : (j % 4 == 0) ? kData : kVide);
            if (c == loopCurL && cur == j + 1) {      // le curseur : un cadre rouge, ouvert en haut
              const int col = 3 + j, lig = 3 + c;
              const int x0 = col * MD_CELL_W - 1, x1 = (col + 1) * MD_CELL_W - 1;
              const int y0 = lig * MD_CELL_H, y1 = lig * MD_CELL_H + MD_GLYPHE_H;
              for (int x = x0; x <= x1; x++) pixel(x, y1, kAccent);
              for (int y = y0; y <= y1; y++) { pixel(x0, y, kAccent); pixel(x1, y, kAccent); }
            }
          }
        }
        // les boutons, sous les couches (en D-PAD, le curseur y descend) ; sans boucle, rien. A gauche RM LAST,
        // CLR ALL (A les presse) ; a droite STEP (les pas de toute la boucle) et STP/CUR (ceux de la rangee du
        // curseur), que A + gauche / droite reglent
        if (boucle) {
          char t[20];
          const int c2 = zoomNiv ? 4 : 8, rv = loopLigneVue;
          const bool ici = cur >= 0 && loopCurL >= kLoopVoies;
          fm1_rangee_espace(3 + kLoopVoies);
          fm1_rangee_normale(4 + kLoopVoies);
          fm1_rangee_normale(5 + kLoopVoies);
          texte(0, 4 + kLoopVoies, zoomNiv ? "RM" : "RM LAST", ici && loopCurL == kLoopVoies && !g_loopBtnC ? kAccent : kData);
          texte(0, 5 + kLoopVoies, zoomNiv ? "CLR" : "CLR ALL", ici && loopCurL == kLoopVoies + 1 && !g_loopBtnC ? kAccent : kData);
          siprintf(t, zoomNiv ? "STEP:%d" : "STEP: %d", g_loopPasG);
          texte(c2, 4 + kLoopVoies, t, ici && loopCurL == kLoopVoies && g_loopBtnC ? kAccent : kData);
          siprintf(t, zoomNiv ? "STP/CUR:%d" : "STP/CUR: %d", g_loopPasL[rv]);
          texte(c2, 5 + kLoopVoies, t, ici && loopCurL == kLoopVoies + 1 && g_loopBtnC ? kAccent : kData);
        }
        g_colOrigine = sauve;
      }
    }
    // ── LA PAGE HELP (ENV) ─────────────────────────────────────────────────
    // Tout le fonctionnement, une partie par page : le texte se coupe a la
    // largeur de l'ecran (21 cases, 15 en 4 COL) et une partie trop
    // longue continue sur la page suivante. « CE QUI : CE QUE CA FAIT » : le
    // debut en jaune. En bas, ENV TO NEXT PAGE (sur la derniere : ENV TO CLOSE) ;
    // en haut a droite, la page.
    else if (page == PAGE_AIDE) {
      // (le titre : ou l'on est, et comment on y arrive ; en jaune, seulement les commandes, en majuscules ;
      // une ligne vide entre deux groupes ; rien d'evident)
      // (les pages du D-PAD d'abord, en petit, le clavier au-dessus : on y lit ARROWS, A, B, SEL sur les touches)
      static const char *const kAide[][2] = {
        { "GLO > D-PAD", "ARROWS: move the cursor\nA: insert the last value\nA, A: insert a new one\nA+LEFT/RIGHT: change value\nA+UP/DOWN: change value by 16\nA+B: delete\n\nSEL+B: start a selection\nB: copy the selection\nSEL+A: paste\nSEL+B+A: clone\n\nSEL+ARROWS: change page\nB+UP/DOWN: jump 16 rows (song)\nPLAY: play/stop" },
        { "D-PAD > DRUM MACHINE", "SEL+DOWN: open the drum machine\nARROWS: move\nA: set/unset a step\nPLAY: start/stop the drums\nSEL+UP: back" },
        { "GLO > KEYBOARD", "normal keyboard\n\nPRESETS: change track" },
        { "GLO > DRUM KEYBOARD", "a drum on each key\n\nOCT-/OCT+: octave" },
        { "SEQ > DRUM MACHINE", "BLACK KEYS: sound\nWHITE KEYS: steps\nPLAY: start/stop\nOCT-/OCT+: octave\n\nin the tracker:\nREC: copy pattern" },
        { "SEQ > LOOPER", "REC: count-in, record\nREC: close the loop\nREC: overdub on/off\nPLAY: stop/start\nREC held: remove notes\n\nSELECT: preset\nOCT-/OCT+: loop octave" },
        { "D-PAD > LOOPER", "ARROWS: move\nA: set/unset a note\nA+LEFT/RIGHT: note -1/+1\nA+UP/DOWN: octave -/+\n\non a layer number:\nA+B: remove the layer\nA: put it back\n\nRM LAST, CLR ALL: A\nSTEP: A+LEFT/RIGHT\n(all the loop)\nSTP/CUR: A+LEFT/RIGHT\n(the cursor's layer ; there,\nOP4..MONO keys pick it)" },
        { "HOME > SONG", "one chain per row\n\nSEL+RIGHT: chain\nSEL+UP: project\nSEL+DOWN: drums" },
        { "SONG > CHAIN", "phrases + transpose\n\nSEL+RIGHT: phrase\nSEL+LEFT: song" },
        { "CHAIN > PHRASE", "notes, instruments,\nvolumes, commands\n\nSEL+RIGHT: instrument\nSEL+LEFT: chain" },
        { "PHRASE > INSTR", "the OPL3 sound\n\nSEL+LEFT/RIGHT: page\nA+LEFT/RIGHT: value\n\non TABLE (last page):\nSEL+RIGHT: table" },
        { "INSTR > TABLE", "steps on each note\n\nSEL+LEFT: instrument" },
        { "SEL+UP > PROJECT", "tempo, save, export,\nload, delete, new,\nzoom, out, light, about\n\nA: open\nA+LEFT/RIGHT: value" },
        { "BUTTONS", "HOME: song\nSEL: presets\nFX: effects\nSEQ: tracker, drums, looper\nREC: record\nOCT-/OCT+: octave\nSAVE: save\nEDIT: undo the knobs\nGLO: keyboard, d-pad,\ndrum keyboard\nENV: help" },
        { "KNOBS", "SELECT: tempo\n\nin the tracker:\nK1: modulator level\nK2: note length\nK3: feedback\nK4: modulator mult\nPRESETS: op1 wave\n(then FM, AM)\nALGORITHM: op2 wave" },
        { "SEL > PRESETS", "2103 OPL3 sounds\n\nSELECT: next sound\nSEL: back" },
        { "REC", "playing: records now\n\nstopped, on phrase:\nnote by note, then\nPLAY records live\n\nstopped, song/chain:\n4 beat count-in\n\nREC: stop" },
        { "FX", "FX: next effect page\nbending, bending 2,\nglitch, knob general,\nmaster fx\n\nSEL+LEFT: song" },
        { "FX > CIRCUIT BENDING", "7 switches on the\nFM chip\n\nKNOBS: on/off\nLEFT/RIGHT: track\nA+RIGHT: bend track\nA+LEFT: unbend track" },
        { "FX > CIRCUIT BENDING 2", "K1: pitch\nK2: fine tune\nK3: clock pitch\n(pitch and speed)" },
        { "FX > GLITCH", "16 steps on the mix\n\nK1: on/off\nK2: other pattern\nUP/DOWN: step\nA+LEFT/RIGHT: effect" },
        { "FX > KNOB GENERAL", "song knobs, on all\nsounds at once\n\nSEL: FM, half AM\nEDIT: default" },
        { "FX > MASTER FX", "7 effects on the mix\n\nKNOBS: amount\nUP/DOWN: effect\nA+LEFT/RIGHT: amount" },
      };
      const int nbParties = (int)(sizeof kAide / sizeof kAide[0]);
      // la largeur et la hauteur d'une partie : en cases visibles et en lignes de texte (sans ZOOM, aerees) ;
      // le D-PAD en petit (la taille de l'en-tete), sous le clavier
      int W = 21, H = 10;
      auto taille = [&](int k) {
        const bool dp = strstr(kAide[k][0], "D-PAD") != nullptr;
        W = dp ? fm1_entete_cellules() - 1 : zoomNiv ? 15 : 21;
        H = dp ? (zoomNiv ? 6 : 11) : (zoomNiv ? 6 : 10);
        return dp;
      };
      static char lignes[48][36];
      static bool tete[48];                       // (la ligne commence une ligne du texte : son « QUI : » en jaune)
      // le texte d'une partie, coupe a la largeur (aux espaces ; \n : a la ligne)
      auto coupe = [&](const char *t) {
        int n = 0;
        bool nouvelle = true;
        while (*t && n < 48) {
          while (*t == ' ') t++;
          tete[n] = nouvelle;
          int lg = 0, esp = -1;
          while (t[lg] && t[lg] != '\n' && lg < W) { if (t[lg] == ' ') esp = lg; lg++; }
          if (t[lg] && t[lg] != '\n' && t[lg] != ' ' && esp > 0) lg = esp;
          memcpy(lignes[n], t, (size_t)lg);
          lignes[n][lg] = 0;
          n++;
          t += lg;
          nouvelle = *t == '\n';
          if (*t == '\n') t++;
        }
        return n;
      };
      // les pages d'une partie : ses groupes de lignes (separes par une ligne vide) tant qu'ils tiennent, un
      // groupe jamais coupe entre deux pages (sauf s'il est plus long qu'une page)
      int pDeb[12], pNb[12];
      auto pagine = [&](int n) {
        int np = 0, d = -1, fin = 0, i = 0;
        while (i < n && np < 11) {
          if (!lignes[i][0]) { i++; continue; }
          int j = i;
          while (j < n && lignes[j][0]) j++;
          if (d >= 0 && j - d > H) { pDeb[np] = d; pNb[np++] = fin - d; d = -1; }   // (la page est pleine)
          while (d < 0 && j - i > H && np < 11) { pDeb[np] = i; pNb[np++] = H; i += H; }
          if (d < 0) d = i;
          fin = j;
          i = j;
        }
        if (d >= 0) { pDeb[np] = d; pNb[np++] = fin - d; }
        return np;
      };
      int total = 0;
      for (int k = 0; k < nbParties; k++) { taille(k); total += pagine(coupe(kAide[k][1])); }
      if (aidePage >= total) aidePage = total - 1;
      aideFin = aidePage == total - 1;
      static int vuP = -1, vuZm = -1;
      if (ecranEfface || aidePage != vuP || zoomNiv != vuZm) {
        vuP = aidePage; vuZm = zoomNiv;
        int partie = 0, debut = 0, nb = 0, p = 0;
        bool dpad = false;
        for (partie = 0; partie < nbParties; partie++) {
          dpad = taille(partie);
          const int np = pagine(coupe(kAide[partie][1]));
          if (aidePage < p + np) { debut = pDeb[aidePage - p]; nb = pNb[aidePage - p]; break; }
          p += np;
        }
        const int sauve = g_colOrigine; g_colOrigine = 0;
        for (int l = 1; l < kLignes; l++) { efface(0, l, 41); fm1_rangee_normale(l); }   // (les espaces de la page d'avant, oublies)
        { char rp[12];
          siprintf(rp, "%d/%d", aidePage + 1, total);
          efface(fm1_entete_cellules() - 8, 0, 8);
          texte(COL_REPERE(rp), 0, rp, kEntete); }
        const char *nomPartie = kAide[partie][0];   // (trop long pour l'ecran : ce qui suit « > » seulement)
        if ((int)strlen(nomPartie) > (zoomNiv ? 15 : 21) && strstr(nomPartie, "> ")) nomPartie = strstr(nomPartie, "> ") + 2;   // (le titre : en gros)
        // le D-PAD : la place du clavier (des rangees vides), puis le titre, un espace, le texte en petit
        const int r0 = dpad ? (zoomNiv ? 3 : 4) : 0;
        for (int l = 1; l <= r0; l++) fm1_rangee_vide(l);
        fm1_aide_clavier(dpad);
        titre(0, 1 + r0, nomPartie, kTitre);
        if (dpad) fm1_rangee_espace(2 + r0);
        else fm1_rangee_vide(2);                  // (une ligne sautee sous le titre, ZOOM compris)
        const int l0 = 3 + r0;
        int i = 0;
        for (; i < nb; i++) {
          const char *t = lignes[debut + i];
          const char *dp = strchr(t, ':');
          if (!t[0]) { fm1_rangee_espace(l0 + i); continue; }   // (une ligne vide : un espace, gardee)
          if (dpad) fm1_rangee_petite(l0 + i);
          else if (!zoomNiv) fm1_rangee_aeree(l0 + i);   // (sans ZOOM, les lignes serrees : un filet entre elles)
          bool commande = dp && dp - t < 34 && dp[1] != 0;   // (« COMMANDE: ce qu'elle fait » : la commande en jaune ;
          for (const char *c = t; commande && c < dp; c++)    //  une explication en minuscules ne l'est pas)
            if (*c >= 'a' && *c <= 'z') commande = false;
          if (tete[debut + i] && commande) {
            char a[36];
            const int k = (int)(dp - t) + 1;
            memcpy(a, t, (size_t)k); a[k] = 0;
            texte(0, l0 + i, a, kSelection);
            texte(k, l0 + i, dp + 1, kData);
          } else texte(0, l0 + i, t, kData);
        }
        fm1_rangee_espace(l0 + i);                // (un espace avant le bas de la page)
        texte(0, l0 + H + 1, aideFin ? "ENV to close" : zoomNiv ? "ENV: next page" : "ENV to next page", kVert);
        g_colOrigine = sauve;
      }
    }
    // ── LA PAGE DRUM MACHINE (SEQ, troisieme mode) ─────────────────────────
    // Un son par rangee, ses seize pas : mis en vert, le pas qui joue en
    // rouge, le son choisi (touche noire) en rouge. En ZOOM, la meme liste.
    else if (page == PAGE_DRUM) {
      static int vuSel = -1, vuZm = -1, vuPas = -2; static uint32_t vuSom = 0;
      md_module_t *m = md_replayer_get_module();
      const int pj = g_drumJoue ? g_drumPas : -1;
      uint32_t som = 0;
      for (int k = 0; m && k < 11; k++) som = som * 31u + m->drum_pas[k];
      static int vuCur = -1;
      const int cur = !fm1_clavier() ? drumCurPas : -1;   // (le curseur de pas : en D-PAD)
      if (m && (ecranEfface || drumSel != vuSel || zoomNiv != vuZm || pj != vuPas || som != vuSom || cur != vuCur)) {
        vuSel = drumSel; vuZm = zoomNiv; vuPas = pj; vuSom = som; vuCur = cur;
        const int sauve = g_colOrigine; g_colOrigine = 0;
        for (int l = 1; l < kLignes; l++) efface(0, l, 41);
        auto pasCase = [&](int col, int lig, int k, int j) {
          const bool mis = (m->drum_pas[k] >> j) & 1u;
          texte(col, lig, mis ? "X" : "-", j == pj ? kAccent : mis ? kVert : (j % 4 == 0) ? kData : kVide);
          if (k == drumSel && j == cur) {         // le curseur : un cadre rouge, ouvert en haut (comme CIRCUIT BENDING)
            const int x0 = col * MD_CELL_W - 1, x1 = (col + 1) * MD_CELL_W - 1;
            const int y0 = lig * MD_CELL_H, y1 = lig * MD_CELL_H + MD_GLYPHE_H;
            for (int x = x0; x <= x1; x++) pixel(x, y1, kAccent);
            for (int y = y0; y <= y1; y++) { pixel(x0, y, kAccent); pixel(x1, y, kAccent); }
          }
        };
        // tous les sons et tous leurs pas, en grand (une grille a sa mesure : l'ecran presque entier), la
        // meme taille avec ou sans ZOOM
        titre(0, 1, "DRUM MACHINE", kTitre);
        fm1_rangee_espace(2);
        for (int k = 0; k < 11; k++) {
          fm1_rangee_drum(3 + k);
          texte(0, 3 + k, kDrumCourt[k], k == drumSel ? kAccent : kData);
          for (int j = 0; j < 16; j++) pasCase(3 + j, 3 + k, k, j);
        }
        g_colOrigine = sauve;
      }
    }
#endif
    // ── LA PAGE MIDI TARGETS ──────────────────────────────────────────────
    // Une ligne par canal : l'application visee, son projet, sa table de CC.
    // Les canaux qu'un instrument MIDI emploie ressortent ; les autres sont
    // attenues, mais se remplissent quand meme — on prepare parfois avant.
    // A ouvre la fiche du canal pointe, B revient au menu.
    else if (page == PAGE_MIDICIBLES) {
      // ⚠️ PAS DE TOUCHES SUR L'IMAGE D'ARRIVEE. Le A qui a valide « MIDI
      // TARGETS » dans le menu est encore enfonce quand cette page se dessine,
      // dans la MEME image : sans ce garde-fou, elle ouvrait aussitot la fiche
      // du canal 01 et l'on ne voyait jamais la liste. Meme chose en revenant
      // de la fenetre, dont on sort avec A.
      if (!ecranEfface) {
        if (appui & KEY_UP)   midiLigne = borne(midiLigne - 1, 0, MD_MIDI_CANAUX - 1);
        if (appui & KEY_DOWN) midiLigne = borne(midiLigne + 1, 0, MD_MIDI_CANAUX - 1);
        if (frappe & KEY_A) ouvreDialogueMidi(midiLigne, false);
        if (frappe & KEY_B) { page = PAGE_PROJECT; pageVue = -1; }
      }

      // On ne repeint que si la table ou le curseur ont bouge : redessinee a
      // chaque image, la page raye l'ecran pendant la lecture.
      int emp = 0;
      for (int c = 0; c < MD_MIDI_CANAUX; c++) {
        char s[16], p[16], m[16];
        md_replayer_get_midi_cible(c, s, p, m, 16);
        emp = emp * 31 + (int)strlen(s) * 7 + (int)strlen(p) * 3 +
              (int)strlen(m) + (s[0] ? s[0] : 1) +
              (md_replayer_canal_midi_utilise(c) ? 101 : 0);
      }
      static int vuMidiLigne = -1, vuMidiEmp = -1;
      if (ecranEfface || midiLigne != vuMidiLigne || emp != vuMidiEmp) {
        vuMidiLigne = midiLigne; vuMidiEmp = emp;
        const int sauve = g_colOrigine; g_colOrigine = 0;
        // Les intitules des trois colonnes, puis les seize canaux, puis le
        // rappel des touches : seize lignes ne laissent pas une rangee de plus.
        efface(0, 1, kCols);
        texte(0, 1, "CHAN SOFT      PROJECT   MAP", kTitre);
        for (int c = 0; c < MD_MIDI_CANAUX; c++) {
          char s[16], p[16], m[16];
          md_replayer_get_midi_cible(c, s, p, m, 16);
          char l2[48];
          siprintf(l2, "CH%02d %.9s %.9s %.8s", c + 1, s[0] ? s : "-",
                   p[0] ? p : "-", m[0] ? m : "-");
          efface(0, 2 + c, kCols);
          texte(0, 2 + c, l2,
                (c == midiLigne) ? kAccent
                : md_replayer_canal_midi_utilise(c) ? kData : kAttenue);
        }
        efface(0, 18, kCols);
        texte(0, 18, "A  SET     B  BACK", kAttenue);
        g_colOrigine = sauve;
      }
    }
    else if (page == PAGE_PROJECT && !dialogueNom) {
      // ── Un vrai menu : on s'y deplace, et A actionne la ligne pointee ────
      // MIDI PANIC coupe les notes restees accrochees sur l'hote ; sa ligne
      // dit aussi ou en est la cartouche USB.
#ifdef FM1
      const char *entrees[kMenuDernier + 1] = { "TEMPO", "SAVE", "LOAD",
                                 "NEW", "SYNC", "SYNC TEST",
                                 "", "ZOOM", "ABOUT", "OUT", "EXPORT A2M", "DELETE", "LIGHT" };
#else
      const char *entrees[8] = { "TEMPO", "SAVE SONG", "LOAD SONG",
                                 "NEW SONG", "SYNC", "SYNC TEST",
                                 "PATTERN LEN", "ABOUT" };
#endif
      // L'USB : pas de cartouche, en attente de l'hote, ou reconnue.
      const int etatUsb = !xt_midi_present() ? 0 : xt_midi_connecte() ? 2 : 1;
      // ── On ne repeint QUE si quelque chose a change ────────────────────
      // Redessine a chaque image, le menu raye l'ecran de bandes pendant la
      // lecture.
      const int bpmVu2 = (int)(md_replayer_get_bpm() + 0.5);
      static int vuMenu = -1, vuBpm = -1, vuCarte = -1, vuUsb = -1;
      static char vuNom[16] = "\x01";
      static int syncVu = -9999; static bool syncTestVu = false;
      static int lenVu = -1;
      static int zoomVu = 0;
      static bool monoVu = true;
      const bool refaireMenu = ecranEfface || menuProjet != vuMenu || zoomVu != zoomNiv || monoVu != sortieMono ||
                               bpmVu2 != vuBpm || (int)carteOK != vuCarte ||
                               etatUsb != vuUsb || syncVu != md_replayer_get_sync_ms() ||
                               syncTestVu != syncTest || lenVu != md_replayer_get_patt_len() ||
                               strncmp(vuNom, nomProjet, sizeof(vuNom) - 1) != 0;
      if (refaireMenu) {
      vuMenu = menuProjet; vuBpm = bpmVu2; vuCarte = carteOK; vuUsb = etatUsb;
      syncVu = md_replayer_get_sync_ms(); syncTestVu = syncTest;
      lenVu = md_replayer_get_patt_len();
      zoomVu = zoomNiv; monoVu = sortieMono;
      strncpy(vuNom, nomProjet, sizeof(vuNom) - 1); vuNom[sizeof(vuNom) - 1] = 0;
#ifdef FM1
      // FM-1 : le nom colle au « > », pour qu'on le voie aussi en ZOOM. Un
      // espace entre l'en-tete (PROJECT) et la premiere ligne.
      for (int k = 0; k <= kMenuDernier; k++) efface(0, 4 + k, 34);
      fm1_rangee_espace(3);
      for (int k = 0; k < kMenuNbVu; k++) {
        const int i = kMenuVu[k], lig = 4 + k;
        texte(0, lig, (i == menuProjet) ? ">" : " ",
              (i == menuProjet) ? kEntete : kData);
        titre(1, lig, entrees[i], (i == menuProjet) ? kAccent : kData);
      }
#else
      for (int i = 0; i <= kMenuDernier; i++) {
        const int lig = 4 + i;
        efface(0, lig, 34);
        texte(0, lig, (i == menuProjet) ? ">" : " ",
              (i == menuProjet) ? kEntete : kData);
        titre(2, lig, entrees[i], (i == menuProjet) ? kAccent : kData);
      }
#endif
      // La valeur du tempo, en face de sa ligne.
      { int b = bpmVu2;
        char d[10];
        d[0] = (char)('0' + (b / 100) % 10);
        d[1] = (char)('0' + (b / 10) % 10);
        d[2] = (char)('0' + b % 10);
        d[3] = ' '; d[4] = 'B'; d[5] = 'P'; d[6] = 'M'; d[7] = 0;
#ifdef FM1
        if (zoomNiv) d[3] = 0;   // en ZOOM : « 125 », le BPM se comprend sur la ligne TEMPO
        efface(kColValMenu, 4 + rangMenu(0), kCols - kColValMenu);
#endif
        texte(kColValMenu, 4 + rangMenu(0), d, (menuProjet == 0) ? kEntete : kData); }
#ifdef FM1
      // FM-1 : plus de nom du morceau sous le menu : il tombait sous le bas de l'ecran, coupe (des traits
      // bizarres apres SAVE). Le nom se lit dans la fenetre de SAVE.
      efface(0, 4 + kMenuNbVu, kCols);
      efface(0, 4 + kMenuNbVu + 1, kCols);
#else
      texte(kColValMenu, 4 + rangMenu(1), nomProjet[0] ? nomProjet : "(UNNAMED)", kData);
#endif
      texte(kColValMenu, 4 + rangMenu(2), carteOK ? "" : "NO SD CARD", kData);
      // SYNC : > 0 les samples attendent, < 0 c'est l'OPL3.
      { const int sy = md_replayer_get_sync_ms();
        char d[24];
        snprintf(d, sizeof d, "%+d MS %s", sy, sy > 0 ? "SAMPLES LATER" : sy < 0 ? "OPL3 LATER" : "");
#ifndef FM1
        efface(kColValMenu, 4 + rangMenu(4), kCols - kColValMenu);
        texte(kColValMenu, 4 + rangMenu(4), d, (menuProjet == 4) ? kEntete : kData);
#else
        (void)d;
#endif
      }
#ifndef FM1
      efface(kColValMenu, 4 + rangMenu(5), kCols - kColValMenu);
      texte(kColValMenu, 4 + rangMenu(5), syncTest ? "ON" : "OFF", (menuProjet == 5) ? kEntete : kData);
#endif
      // PATTERN LEN : la longueur des patterns AT2, ecrite dans le .A2M a
      // l'enregistrement. La lecture, elle, suit les H00 des phrases.
#ifdef FM1
      efface(kColValMenu, 4 + rangMenu(kMenuZoom), kCols - kColValMenu);
      texte(kColValMenu, 4 + rangMenu(kMenuZoom), zoomNiv ? "4 COL" : "OFF", (menuProjet == kMenuZoom) ? kEntete : kData);
      // (OUT a deux lettres de moins que TEMPO : sa valeur commence plus tot, STEREO tient en ZOOM)
      efface(5, 4 + rangMenu(kMenuSortie), kCols - 5);
      texte(5, 4 + rangMenu(kMenuSortie), sortieMono ? "MONO" : "STEREO", (menuProjet == kMenuSortie) ? kEntete : kData);
      { const char lv[2] = { "0123456789ABCDEF"[lumiere & 15], 0 };
        efface(kColValMenu, 4 + rangMenu(kMenuLum), kCols - kColValMenu);
        texte(kColValMenu, 4 + rangMenu(kMenuLum), lv, (menuProjet == kMenuLum) ? kEntete : kData); }
#else
      { const int lp = md_replayer_get_patt_len();
        char d[20];
        if (lp > 0) snprintf(d, sizeof d, "%02X (%d ROWS)", lp, lp);
        else strcpy(d, "OFF");
        efface(14, 10, kCols - 14);
        texte(14, 10, d, (menuProjet == 6) ? kEntete : kData); }
#endif
#ifndef FM1   // (la puce OPL3 en USB de la DS : pas sur le FM-1, ou cette rangee est celle d'ABOUT)
      efface(0, 13, kCols);
      texte(0, 13, xt_midi_present() ? "RETROWAVE OK" : "NO RETROWAVE",
            xt_midi_present() ? kTitre : kAccent);
#endif
      // A quoi le MIDI de ce morceau est branche : de quoi retrouver le bon
      // logiciel et la bonne table de CC en le reouvrant.
      // Les canaux MIDI employes par le morceau, et ce qu'il y a dessus. Le
      // detail des seize est sur la page MIDI TARGETS.
      if (false) { int lig = 11;
        for (int c = 0; c < MD_MIDI_CANAUX && lig <= 13; c++) {
          if (!md_replayer_canal_midi_utilise(c)) continue;
          char s[16], p[16], m[16];
          md_replayer_get_midi_cible(c, s, p, m, 16);
          char l2[44];
          siprintf(l2, "CH%02d %.10s %.10s", c + 1, s[0] ? s : "-",
                   p[0] ? p : "-");
          efface(0, lig, 34);
          texte(0, lig, l2, kData);
          lig++;
        }
        for (; lig <= 13; lig++) efface(0, lig, 34); }
      }
    }

    if (page == PAGE_INSTR) {
      // L'instrument montre est celui RETENU quand on est descendu depuis la
      // phrase, comme instrId sur l'iPad.
      const int ins = instrId;
      const int genre = genreInstr(ins);

      // On ne redessine que ce qui a CHANGE : une page repeinte a chaque
      // image raye l'ecran de bandes pendant la lecture.
      const int echBrut = md_replayer_get_instr_sample(ins);
      const int ech = (echBrut < 0 || echBrut >= MD_MAX_SAMPLES) ? -1 : echBrut;
      // Le dossier se lit a la premiere visite de la page, pour numeroter le
      // son dans sa liste — jamais au demarrage de la console.
      if (genre == GENRE_SAMPLE && !echScanne && carteOK) { lisCfg(); scanneEch(); }
      uint32_t lg = 0; int bcl = -1, base = 60; char nomEch[24] = "";
      if (ech >= 0) md_replayer_get_sample(ech, nomEch, 23, &lg, &bcl, &base);
      const int vol    = md_replayer_get_instr_pcm_volume(ins);
      const int tab    = md_replayer_get_instr_table(ins);
      const int pan    = md_replayer_get_instr_gen_val(ins, MD_GEN_PROP_PANNING);
      const int canalM = md_replayer_get_instr_midi_channel(ins);
      const int prog   = md_replayer_get_instr_midi_program(ins);
      const int lenM   = md_replayer_get_instr_midi_length(ins);
      const int sync   = md_replayer_get_sync_ms();   // reglage global
      // Les cinq reglages du kick, resumes en un seul nombre : il suffit a
      // savoir s'il faut redessiner.
      int kickEmpreinte = 0;
      for (int k = 0; k < 5; k++)
        kickEmpreinte = kickEmpreinte * 257 +
                        md_replayer_get_instr_kick_val(ins, k);
      kickEmpreinte = kickEmpreinte * 257 + md_replayer_get_instr_disto(ins);
      static int vuIns = -1, vuLig = -1, vuGenre = -1, vuEch = -2, vuVol = -1;
      static int vuTab = -2, vuPan = -2, vuCanalM = -1, vuProg = -1, vuLenM = -1;
      static int vuKick = -1, vuSync = -1;
      static int vuBase = -1, vuBoucle = -2, vuEchSel = -2, vuEchNb = -1;
#ifdef FM1
      if (genre == GENRE_OPL && inLigne == 0) inLigne = 1;   // (pas de ligne TYPE)
#endif
      const bool toutRefaire = ecranEfface || ins != vuIns || genre != vuGenre;
      const bool refaire = toutRefaire || inLigne != vuLig || ech != vuEch ||
                           vol != vuVol || tab != vuTab || pan != vuPan ||
                           canalM != vuCanalM || prog != vuProg || lenM != vuLenM ||
                           kickEmpreinte != vuKick || sync != vuSync ||
                           base != vuBase || bcl != vuBoucle ||
                           echSel != vuEchSel || echNb != vuEchNb;
      vuIns = ins; vuLig = inLigne; vuGenre = genre; vuEch = ech; vuVol = vol;
      vuTab = tab; vuPan = pan; vuCanalM = canalM; vuProg = prog; vuLenM = lenM;
      vuKick = kickEmpreinte; vuSync = sync;
      vuBase = base; vuBoucle = bcl; vuEchSel = echSel; vuEchNb = echNb;

      // L'empreinte des onze octets OPL : un reglage change -> on redessine.
      static int vuOpl = -1, vuCol = -1;
      int oplEmpreinte = 0;
      for (int k = 0; k < MD_OPL_OCTETS; k++)
        oplEmpreinte = oplEmpreinte * 31 + md_replayer_get_instr_opl(ins, k);
#ifdef FM1
      // ── FM-1 : les cinq pages d'un instrument OPL3 ───────────────────────
      static int vuPg = -1, vuIt = -1, vuOp = -1, vuFt = -999, vuZm = -1;
      const int fineT = md_replayer_get_instr_gen_val(ins, MD_GEN_PROP_FINE_TUNE);
      // (inCol aussi : il suit l'element une image apres le curseur ; sans lui,
      // c'est l'affichage d'avant, plus bas, qui se redessinait par-dessus)
      if (genre == GENRE_OPL && (refaire || oplEmpreinte != vuOpl || inPage != vuPg || inItem != vuIt ||
                                 inOp != vuOp || fineT != vuFt || zoomNiv != vuZm || inCol != vuCol)) {
        vuOpl = oplEmpreinte; vuPg = inPage; vuIt = inItem; vuOp = inOp; vuFt = fineT; vuZm = zoomNiv;
        vuCol = inCol;                               // (l'affichage d'avant, plus bas, n'a rien a refaire)
        const bool z = zoomNiv != 0;
        const int sauveO = g_colOrigine; g_colOrigine = 0;
        fm1_decales_efface();
        fm1_mots_entete_efface();
        fm1_espace_sous_entete(0);
        for (int l = 1; l < kLignes; l++) efface(0, l, 41);
        static char nomIns[9];
        siprintf(nomIns, "INSTR %02X", ins & 0xFF);
        fm1_nom_page(nomIns);
        { char rp[16];                               // la page, en haut a droite
          siprintf(rp, "%s %d/%d", kInPageNom[inPage], inPage + 1, kInPages);
          efface(fm1_entete_cellules() - 11, 0, 11);
          texte(COL_REPERE(rp), 0, rp, kEntete); }
        // Une barre de `n` cases, remplie a v / vmax (sans la couleur du
        // curseur : la vue le suit).
        auto barre = [&](int col, int lig, int n, int v, int vmax) {
          const int x0 = col * MD_CELL_W, w = n * MD_CELL_W - 1, y0 = lig * MD_CELL_H;
          const int plein = vmax > 0 ? (w * v + vmax / 2) / vmax : 0;
          for (int x = 0; x < w; x += 2)
            for (int y = y0; y < y0 + MD_GLYPHE_H; y++)
              pixel(x0 + x, y, x < plein ? kData : kVide);
        };
        // Les intitules entiers sans ZOOM, courts en ZOOM.
        auto nomChamp = [&](int champ) -> const char * {
          if (z) return kNomCourt[champ];
          switch (champ) {
            case 1: return "ATTACK";   case 2: return "DECAY";      case 3: return "SUSTAIN";
            case 4: return "RELEASE";  case 5: return "OUTPUT";     case 7: return "CONNECTION";
            case 8: return "PANNING";  case 14: return "FEEDBACK";  case 15: return "F-TUNE";
            case kOplRangeeTable: return "TABLE";
            default: return kNomCourt[champ];
          }
        };
        // Les titres et les espaces de chaque page (sans ZOOM : en ZOOM, la page
        // tient dans l'ecran sans eux).
        if (!z) switch (inPage) {
          case 0: fm1_rangee_espace(2); if (!z) fm1_rangee_espace(8); break;
          case 1: fm1_rangee_espace(2); titre(0, 3, "WAVEFORM", kTitre); fm1_rangee_espace(12); break;
          case 2: fm1_rangee_espace(2); titre(0, 3, z ? "MULT" : "FREQ MULTIPLIER", kTitre);
                  fm1_rangee_espace(zoomNiv == 2 ? 20 : 12);
                  titre(0, zoomNiv == 2 ? 21 : 13, z ? "KSL" : "KEY SCALING LVL", kTitre); break;
          default: fm1_rangee_espace(2); fm1_rangee_espace(5);
                  titre(0, 6, z ? "ENV TYPE" : "ENVELOPE TYPE", kTitre); fm1_rangee_espace(11);
                  fm1_rangee_espace(13);
        }
        ElInstr el[40];
        const int n = elementsInstr(inPage, el);
        for (int k = 0; k < n; k++) {
          const ElInstr &e = el[k];
          const bool sel = k == inItem;
          const u16 coul = sel ? kAccent : kData;
          if (e.genre == EL_OP) {
            titre(0, e.lig, "OP", kTitre);
            texte(3, e.lig, inOp ? (z ? "CAR" : "CARRIER") : (z ? "MOD" : "MODULATOR"), coul);
            continue;
          }
          const ChampOpl &ch = kChampsOp[e.champ];
          const int octet = ch.octet + (ch.parOp ? inOp : 0);
          const int v = (ch.parOp || ch.aff == AFF_CONN || ch.aff == AFF_HEX)
                            ? oplLit(ins, octet, ch.decalage, ch.masque) : 0;
          if (e.genre == EL_OPT) {
            char t[20];
            bool coche; const char *nm;
            if (ch.aff == AFF_ONDE)      { coche = v == e.opt; nm = zoomNiv == 2 ? kOndeCourt[e.opt & 7] : kOndeNom[e.opt & 7]; }
            else if (ch.aff == AFF_KSL)  { coche = v == kKslOrdre[e.opt & 3];
                                           nm = zoomNiv == 2 ? kKslCourt[kKslOrdre[e.opt & 3]] : kKslNom[kKslOrdre[e.opt & 3]]; }
            else                         { coche = v == e.opt; nm = kMultNom[e.opt & 15]; }
            siprintf(t, "(%c) %s", coche ? '*' : ' ', nm);
            texte(e.col, e.lig, t, coul);
            continue;
          }
          if (e.genre == EL_CHK) {
            static const char *kLong[4] = { "TREMOLO", "VIBRATO", "KEY SCL RATE", "SUSTAIN" };
            static const char *kCourt[4] = { "TREM", "VIB", "KSR", "SUST" };
            char t[20];
            siprintf(t, "[%c] %s", v ? 'X' : ' ', z ? kCourt[e.champ - 9] : kLong[e.champ - 9]);
            texte(0, e.lig, t, coul);
            continue;
          }
          // Un reglage : son nom, sa valeur, et sa barre sans ZOOM.
          char d[12] = "";
          int bv = -1, bmax = 0;
          switch (ch.aff) {
            case AFF_HEX:    d[0] = kHex[v & 15]; d[1] = 0; bv = v; bmax = ch.masque; break;
            case AFF_INV:    d[0] = kHex[(ch.masque - v) & 15]; d[1] = 0; bv = ch.masque - v; bmax = ch.masque; break;
            case AFF_SORTIE: d[0] = kHex[(63 - v) >> 4]; d[1] = kHex[(63 - v) & 15]; d[2] = 0; bv = 63 - v; bmax = 63; break;
            case AFF_CONN:   strcpy(d, v ? "AM" : "FM"); break;
            case AFF_ONDE:   strcpy(d, zoomNiv == 2 ? kOndeCourt[v & 7] : kOndeNom[v & 7]); break;
            case AFF_MULT:   strcpy(d, kMultNom[v & 15]); break;
            case AFF_KSL:    strcpy(d, zoomNiv == 2 ? kKslCourt[v & 3] : kKslNom[v & 3]); break;
            case AFF_PAN:    strcpy(d, pan < 85 ? "L" : pan > 170 ? "R" : "C"); break;
            case AFF_FTUNE: { const int ft = (fineT + 128) & 255;
                              d[0] = kHex[ft >> 4]; d[1] = kHex[ft & 15]; d[2] = 0; bv = ft; bmax = 255; } break;
            case AFF_TABLE:  texteTable(ins, d); break;
          }
          titre(0, e.lig, nomChamp(e.champ), kTitre);
          int cv = 0, cb = -1, nb = 0;               // la colonne de la valeur ; la barre
          if (z && zoomNiv == 1 && inPage == 0) { cb = 4; nb = 8; cv = 13; }   // (4 COL : les barres tiennent)
          else if (z) cv = 5;
          else if (inPage == 0) { cb = 8; nb = 10; cv = 19; }
          else if (inPage == 1) { cv = 9; cb = 11; nb = 9; }
          else if (inPage == 3) { cv = 11; if (ch.aff == AFF_FTUNE) { cb = 14; nb = 7; } }
          else cv = 6;
          texte(cv, e.lig, d, coul);
          if (cb >= 0 && bv >= 0) barre(cb, e.lig, nb, bv, bmax);
          if (ch.aff == AFF_CONN)                    // le schema : 1 module 2, ou les deux s'ajoutent
            texte(z ? 8 : 14, e.lig, v ? "1+2>" : "1>2>", kTitre);
        }
        // La courbe ADSR de l'operateur, sous l'enveloppe (sans ZOOM) : la
        // montee (ATTACK), la descente (DECAY) vers le SUSTAIN, qui tient si
        // l'EGT (SUSTAIN de l'ENVELOPE TYPE) est mis, puis RELEASE.
        if (inPage == 0 && !z) {
          const int a = 15 - oplLit(ins, MD_OPL_AD_M + inOp, 4, 15), dd = 15 - oplLit(ins, MD_OPL_AD_M + inOp, 0, 15);
          const int sl = 15 - oplLit(ins, MD_OPL_SR_M + inOp, 4, 15), rr = 15 - oplLit(ins, MD_OPL_SR_M + inOp, 0, 15);
          const bool tient = oplLit(ins, MD_OPL_FLAGS_M + inOp, 5, 1) != 0;
          const int x0 = 0, y0 = 9 * MD_CELL_H, w = 20 * MD_CELL_W, h = 6 * MD_CELL_H - 1;
          const int la = 1 + a * 3, ld = 1 + dd * 3, lr = 1 + rr * 3;
          const int lsus = w - la - ld - lr > 6 ? w - la - ld - lr : 6;
          for (int x = 0; x < w; x++) {
            int niv;                                  // 0 .. h
            if (x < la) niv = h * x / la;
            else if (x < la + ld) niv = h - (h - h * sl / 15) * (x - la) / ld;
            else if (x < la + ld + lsus) niv = tient ? h * sl / 15
                                                     : h * sl / 15 - (h * sl / 15) * (x - la - ld) / (lsus + lr);
            else { const int depart = tient ? h * sl / 15 : (h * sl / 15) * lr / (lsus + lr);
                   niv = depart - depart * (x - la - ld - lsus) / lr; }
            if (niv < 0) niv = 0;
            if (x == 0) for (int y = 0; y <= h; y++) pixel(x0, y0 + y, kTitre);   // l'axe : la courbe garde ses rangees
            else if (!(x & 1)) for (int y = h - niv; y <= h; y++) pixel(x0 + x, y0 + y, kData);
          }
        }
        g_colOrigine = sauveO;
      } else
#endif
      if (genre == GENRE_OPL && (refaire || oplEmpreinte != vuOpl || inCol != vuCol)) {
        vuOpl = oplEmpreinte; vuCol = inCol;
#ifdef FM1
        // FM-1 : « INSTR 01 » en haut a gauche, et MOD / CAR sur la meme ligne
        // (plus bas) : il n'y a plus de repere a droite, CAR le chevaucherait.
        fm1_decales_efface();   // (chaque valeur repose le sien, plus bas)
        if (toutRefaire) {
          for (int l = 1; l < kLignes; l++) efface(0, l, 41);
          static char nomIns[9];
          siprintf(nomIns, "INSTR %02X", ins & 0xFF);
          fm1_nom_page(nomIns);
          fm1_espace_sous_entete(7);   // un espace entre l'en-tete (MOD, CAR) et ATK
        }
#else
        if (toutRefaire) {
          for (int l = 1; l < kLignes; l++) efface(0, l, 41);
          char t[8];
          efface(kColRepere, 0, 8);
          t[0]='I'; t[1]='N'; t[2]='S'; t[3]=' ';
          t[4]=kHex[(ins>>4)&15]; t[5]=kHex[ins&15]; t[6]=0;
          texte(COL_REPERE(t), 0, t, kEntete);
        }
#endif
        // MOD et CAR bien separes : la grille tient dans les 40 colonnes.
#ifdef FM1
        // FM-1 : reparties sur la largeur, le meme ecart (5 cases) apres les
        // intitules et entre MOD et CAR. En ZOOM, douze colonnes a l'ecran :
        // une case d'ecart seulement, et tout tient.
        const int kColVal[2] = { zoomNiv ? 5 : 9, zoomNiv ? 9 : 17 };
#else
        const int kColVal[2] = { 19, 30 };   // MOD et CAR, decales de SAMPLE
#endif
#ifdef FM1
        // MOD et CAR dans l'en-tete, chacun centre sur sa colonne de valeurs :
        // l'en-tete a ses propres cellules (7 points, 10 en ZOOM), celles des
        // valeurs en font 11 (20 en ZOOM).
        {
          const int dc = fm1_largeur_cellule(), hc = fm1_largeur_cellule_entete();
          static const char *kNomOp[2] = { "MOD", "CAR" };
          for (int k = 0; k < 2; k++) {
            // Cale sur le debut des valeurs, qui partent a gauche de leur
            // colonne et n'ont souvent qu'un caractere : centre sur trois
            // cases, le titre partait a droite.
            // (la cellule la plus proche ; fm1_mot_entete le place ensuite au
            // point pres, centre sur les trois cases de valeurs)
            const int hcol = ((kColVal[k] + g_colOrigine) * dc + hc / 2) / hc;
            const int sauve = g_colOrigine; g_colOrigine = 0;
            efface(hcol - 1, 0, 5);
            texte(hcol, 0, kNomOp[k], kEntete);
            g_colOrigine = sauve;
            fm1_mot_entete(hcol, 3, kColVal[k] + g_colOrigine, 3);
          }
        }
#else
        titre(kColVal[0], 2, "MOD", kTitre);
        titre(kColVal[1], 2, "CAR", kTitre);
#endif
        for (int r = 0; r < kOplRangees; r++) {
          const ChampOpl &ch = kChampsOp[r];
          // Une ligne vide avant ENVELOPE TYPE et avant WAVEFORM.
          // TYPE sur la rangee 1, a gauche de MOD/CAR ; puis une ligne vide
          // avant ENVELOPE TYPE et avant WAVEFORM.
#ifdef FM1
          // FM-1 : pas de ligne TYPE (pas d'echantillons sur le FM-1), et pas
          // de ligne vide : tout tient dans l'image (TABLE tombait sur la
          // rangee 21, hors de l'image), la vue suit le curseur vers le bas.
          if (ch.aff == AFF_TYPE) continue;
          // Des espaces entre les parties : l'enveloppe, les niveaux et la
          // voie, les drapeaux, puis l'onde, l'accord et la table.
          const int lig = r + (r >= 5) + (r >= 9) + (r >= 13);
          if (r == 5 || r == 9 || r == 13) fm1_rangee_espace(lig - 1);
          efface(0, lig, kCols);
          titre(0, lig, kNomCourt[r], kTitre);
#else
          const int lig = (r == 0) ? 2 : 2 + r + (r >= 9 ? 1 : 0) + (r >= 13 ? 1 : 0);
          if (r == 0) efface(0, lig, kColVal[0] - 1); else efface(0, lig, kCols);
          titre(0, lig, ch.nom, kTitre);
          if (ch.aff == AFF_TYPE) {
            texte(6, lig, "OPL3", inLigne == 0 ? kAccent : kTitre);
            texte(11, lig, "SAMPLE", kData);
            continue;
          }
#endif
          for (int col = 0; col < (ch.parOp ? 2 : 1); col++) {
            const bool sel = inLigne == r && inCol == col;
            const int v = ch.parOp || ch.aff == AFF_CONN || ch.aff == AFF_HEX
                              ? oplLit(ins, ch.octet + (ch.parOp ? col : 0), ch.decalage, ch.masque) : 0;
            char d[12] = "";
            switch (ch.aff) {
              case AFF_HEX:    d[0] = kHex[v & 15]; d[1] = 0; break;
              case AFF_INV:    d[0] = kHex[(ch.masque - v) & 15]; d[1] = 0; break;
              case AFF_SORTIE: d[0] = kHex[(63 - v) >> 4]; d[1] = kHex[(63 - v) & 15]; d[2] = 0; break;
#ifdef FM1
              case AFF_KSL:    strcpy(d, kKslCourt[v & 3]); break;
              case AFF_ONDE:   strcpy(d, kOndeCourt[v & 7]); break;
#else
              case AFF_KSL:    strcpy(d, kKslNom[v & 3]); break;
              case AFF_ONDE:   strcpy(d, kOndeNom[v & 7]); break;
#endif
              case AFF_ONOFF:  strcpy(d, v ? "ON" : "OFF"); break;
              case AFF_MULT:   strcpy(d, kMultNom[v & 15]); break;
              case AFF_CONN:   strcpy(d, v ? "AM" : "FM"); break;
              case AFF_PAN:    strcpy(d, pan < 85 ? "L" : pan > 170 ? "R" : "C"); break;
#ifdef FM1
              // FM-1 : 00 = le plus grave, 80 = juste, FF = le plus aigu (et
              // non FF pour -1, comme le ferait l'octet signe tel quel).
              case AFF_FTUNE: { const int ft = (md_replayer_get_instr_gen_val(ins, MD_GEN_PROP_FINE_TUNE) + 128) & 255;
#else
              case AFF_FTUNE: { const int ft = (uint8_t)md_replayer_get_instr_gen_val(ins, MD_GEN_PROP_FINE_TUNE);
#endif
                                d[0] = kHex[ft >> 4]; d[1] = kHex[ft & 15]; d[2] = 0; } break;
              case AFF_TABLE:  texteTable(ins, d); break;
            }
#ifdef FM1
            // Un seul caractere : au milieu de la colonne, sous le O de MOD et le
            // A de CAR (les valeurs plus longues partent du debut de la colonne).
            // Deux caracteres : une demi-case plus a droite (fm1_decale_demi), centres
            // eux aussi. Trois : la colonne entiere.
            texte(kColVal[col] + (d[0] && !d[1] ? 1 : 0), lig, d, sel ? kAccent : kData);
            if (d[0] && d[1] && !d[2]) fm1_decale_demi(lig, kColVal[col] + g_colOrigine, 2);
#else
            texte(kColVal[col], lig, d, sel ? kAccent : kData);
#endif
          }
        }
      } else if (refaire) {
        if (toutRefaire) {
          // Depuis la rangee 2 : la page KICK, qui a neuf champs, commence une
          // rangee plus haut que les autres.
          for (int l = 1; l < kLignes; l++) efface(0, l, 41);
          char t[8];
          efface(kColRepere, 0, 8);
          t[0]='I'; t[1]='N'; t[2]='S'; t[3]=' ';
          t[4]=kHex[(ins>>4)&15]; t[5]=kHex[ins&15]; t[6]=0;
          texte(COL_REPERE(t), 0, t, kEntete);
        }

        // Le curseur se pose sur la VALEUR, jamais sur l'intitule : c'est la
        // valeur qu'on modifie. « LOAD SAMPLE » fait exception, c'est une
        // action et non un reglage, donc c'est le mot lui-meme qui s'allume.
        static const char *nomS[9] = { "TYPE", "LOAD SAMPLE", "SAMPLE",
                                       "BASE NOTE", "LOOP", "VOLUME",
                                       "PAN", "DISTO", "TABLE" };
        static const char *nomM[7] = { "TYPE", "CHANNEL", "PROGRAM",
                                       "VOLUME", "LENGTH", "TABLE",
                                       "MIDI SYNC" };
        static const char *nomK[9] = { "TYPE", "DROP", "TIME", "DECAY",
                                       "CLICK", "DRIVE", "VOLUME", "OUTPUT",
                                       "TABLE" };
        const int nb = champTableDe(ins) + 1;
        // Neuf champs espaces de deux rangees ne tiennent qu'en partant de la
        // rangee 2 ; les pages plus courtes gardent leur rangee 3.
        const int hautDep = (nb > 8) ? 2 : 3;
        auto ligneDe = [&](int k) { return hautDep + k * 2; };
        for (int k = 0; k < nb; k++) {
          efface(0, ligneDe(k), 13);
          titre(0, ligneDe(k), (genre == GENRE_MIDI) ? nomM[k]
                             : (genre == GENRE_KICK) ? nomK[k] : nomS[k],
                (genre == GENRE_SAMPLE && k == 1 && inLigne == 1) ? kAccent : kTitre);
        }
        auto hex2 = [&](int col, int lig, int v, bool ici) {
          char d[3] = { kHex[(v >> 4) & 15], kHex[v & 15], 0 };
          efface(col, lig, 2);
          texte(col, lig, d, ici ? kAccent : kData);
        };

        // Le TYPE : les deux mots restent lisibles, seul le choisi est en
        // couleur — c'est ainsi que la page montre deja la sortie L C R.
        { const int lig = ligneDe(0);
          const bool ici = (inLigne == 0);
          efface(14, lig, 17);
          efface(6, lig, 12);
          texte(6, lig, "OPL3", kData);
          texte(11, lig, "SAMPLE", ici ? kAccent : kTitre); }

        // La table attachee : « OFF » quand il n'y en a pas.
        { const int ct = champTableDe(ins);
          char d4[4]; texteTable(ins, d4);
          efface(14, ligneDe(ct), 3);
          texte(14, ligneDe(ct), d4, (inLigne == ct) ? kAccent : kData); }

        // MIDI SYNC, tout en bas : le retard des echantillons. Meme valeur sur
        // toutes les pages qui la montrent.
        { const int cs = champSyncDe(ins);
          if (cs >= 0) {
            char d6[10]; siprintf(d6, "%3d MS", sync);
            efface(14, ligneDe(cs), 7);
            texte(14, ligneDe(cs), d6, (inLigne == cs) ? kAccent : kData);
          } }

        if (genre == GENRE_MIDI) {
          // Le canal s'affiche de 01 a 16, comme sur tous les appareils MIDI.
          { char d[4]; siprintf(d, "%02d", canalM + 1);
            efface(14, ligneDe(1), 2);
            texte(14, ligneDe(1), d, (inLigne == 1) ? kAccent : kData); }
          if (prog == MD_EMPTY) {
            efface(14, ligneDe(2), 2);
            texte(14, ligneDe(2), "--", (inLigne == 2) ? kAccent : kData);
          } else hex2(14, ligneDe(2), prog, inLigne == 2);
          hex2(14, ligneDe(3), vol, inLigne == 3);
          // LENGTH : AUTO relache la note juste avant la suivante ; une valeur
          // la relache au bout de ce nombre de ticks.
          if (lenM == 0) {
            efface(14, ligneDe(4), 4);
            texte(14, ligneDe(4), "AUTO", (inLigne == 4) ? kAccent : kData);
          } else {
            efface(14, ligneDe(4), 4);
            hex2(14, ligneDe(4), lenM, inLigne == 4);
          }
        } else if (genre == GENRE_KICK) {
          // Les cinq reglages du coup, puis le volume et la stereo.
          for (int k = 0; k < 5; k++)
            hex2(14, ligneDe(1 + k), md_replayer_get_instr_kick_val(ins, k),
                 inLigne == 1 + k);
          hex2(14, ligneDe(6), vol, inLigne == 6);
          { hex2(14, ligneDe(7), pan, inLigne == 7);
            const char *cote = (pan < 112) ? "L" : (pan > 144) ? "R" : "C";
            efface(17, ligneDe(7), 4);
            texte(17, ligneDe(7), cote, kTitre); }
        } else {
          // Le rang dans le DOSSIER, puis le nom : c'est ce que la croix
          // parcourt. La police n'a pas de minuscules, on les remonte.
          { char d5[40];
            const int rang = rangEch(ins);
            if (ech < 0) siprintf(d5, "--/%02d (NONE)", echNb);
            else if (rang < 0) siprintf(d5, "--/%02d %s", echNb, nomEch);
            else siprintf(d5, "%02d/%02d %s", rang + 1, echNb, nomEch);
            for (int j = 0; d5[j]; j++)
              if (d5[j] >= 'a' && d5[j] <= 'z') d5[j] = (char)(d5[j] - 'a' + 'A');
            efface(14, ligneDe(2), 26);
            texte(14, ligneDe(2), d5, (inLigne == 2) ? kAccent : kData); }

          // Sans echantillon, la note de base et la boucle n'ont AUCUNE valeur
          // a montrer.
          char d2[12];
          static const char *gam2[12] = {"C-","C#","D-","D#","E-","F-",
                                         "F#","G-","G#","A-","A#","B-"};
          if (ech >= 0 && base > 0 && base <= MD_MAX_NOTE) {
            const char *g2 = gam2[(base - 1) % 12];
            d2[0]=g2[0]; d2[1]=g2[1]; d2[2]=(char)('0'+((base-1)/12)); d2[3]=0;
          } else { d2[0]='-'; d2[1]='-'; d2[2]='-'; d2[3]=0; }
          efface(14, ligneDe(3), 4);
          texte(14, ligneDe(3), d2, (inLigne == 3) ? kAccent : kData);

          if (ech < 0 || bcl < 0) { d2[0]='-'; d2[1]='-'; d2[2]=0; }
          else siprintf(d2, "%05d", bcl);
          efface(14, ligneDe(4), 8);
          texte(14, ligneDe(4), d2, (inLigne == 4) ? kAccent : kData);

          hex2(14, ligneDe(5), vol, inLigne == 5);

          // La position stereo, sur 00-FF, suivie du cote vers lequel elle
          // penche : 80 est le centre exact.
          // PAN : une jauge progressive, 00 gauche, 80 centre, FF droite.
          hex2(14, ligneDe(6), pan, inLigne == 6);
          hex2(14, ligneDe(7), md_replayer_get_instr_disto(ins), inLigne == 7);
        }
      }
    }

    // ── La page TABLE ───────────────────────────────────────────────────
    // La page « T » de LSDJ : seize lignes, cinq colonnes, HUIT arrets de
    // curseur — la lettre d'une commande et sa valeur se reglent separement,
    // comme dans une phrase.
    //
    // RÈGLE VALABLE POUR TOUTE PAGE : on ne repeint QUE ce qui a change. Une
    // page redessinee a chaque image fait courir les ecritures apres le
    // balayage de l'ecran — les bandes de vieux televiseur — et vole au son le
    // temps qu'il lui faut. Les reperes de lecture, qui bougent en permanence,
    // sont donc traites A PART de la grille : eux seuls sont repeints quand ils
    // se deplacent.
    if (page == PAGE_TABLE) {
      // Les cinq colonnes, leur largeur, et le chevron qui les precede.
      static const int col[8]   = { 4, 8, 12, 13, 17, 18, 22, 24 };
      static const int larg[8]  = { 2, 2,  1,  2,  1,  2,  2,  2 };
      // Les cinq groupes affichés : ou poser leur chevron, et quel FLUX de
      // lecture ils suivent. Une table ne se deroule pas d'un bloc : la
      // commande H fait boucler un flux sans toucher aux autres, d'ou cinq
      // reperes qui se promenent chacun de leur cote.
      static const int chevCol[5]  = { 3, 7, 11, 16, 21 };
      static const int chevFlux[5] = { 0, 1,  1,  2,  2 };

      static int vuTabId = -1, vuTabL = -1, vuTabC = -1;
      const bool refaireTable = ecranEfface || modifie ||
                                tableId != vuTabId || tabLigne != vuTabL ||
                                tabCol != vuTabC;
      if (refaireTable) {
        vuTabId = tableId; vuTabL = tabLigne; vuTabC = tabCol;

        char t9[28];
        siprintf(t9, "TABLE %02X  INSTR %02X", tableId & 0xFF, instrId & 0xFF);
        efface(0, 0, 22); texte(0, 0, t9, kEntete);

        const int decalT = FM1_DECAL_NOMS((MD_GLYPHE_H + 3 * MD_CELL_H) / 2 - MD_GLYPHE_H / 2
                           - MD_CELL_H);
        g_decalYpx = decalT;
        texte(4, 1, "VOL", kTitre);
        texte(8, 1, "TSP", kTitre);
        texte(12, 1, "CMD", kTitre);
        texte(17, 1, "CMD", kTitre);
        texte(22, 1, "CC", kTitre);
#ifdef FM1
        fm1_titres_colonnes(1);   // FM-1 : cette rangee s'affiche en petit
#endif
        g_decalYpx = 0;

        for (int l = 0; l < MD_TABLE_ROWS; l++) {
          if ((l & 7) == 0) entretienSon();
          const int lig = 3 + l;
          numeroLigne(0, lig, l, l == tabLigne);

          uint8_t vol,c1,v1,c2,v2,mc,mv; int8_t tsp;
          md_replayer_get_table_row(tableId, l, &vol, &tsp,
                                    &c1, &v1, &c2, &v2, &mc, &mv);
          char sv[3], st[3], sl1[2], sa1[3], sl2[2], sa2[3], sm[3], smv[3];
          sv[0]=kHex[(vol>>4)&15]; sv[1]=kHex[vol&15]; sv[2]=0;
          { const uint8_t u = (uint8_t)tsp;
            st[0]=kHex[(u>>4)&15]; st[1]=kHex[u&15]; st[2]=0; }
          // Une commande vide s'ecrit « - -- », comme dans une phrase.
          if (c1 == MD_EMPTY) { sl1[0]='-'; sa1[0]='-'; sa1[1]='-'; }
          else { const char x = md_table_cmd_letter(c1); sl1[0] = x ? x : '?';
                 sa1[0]=kHex[(v1>>4)&15]; sa1[1]=kHex[v1&15]; }
          sl1[1]=0; sa1[2]=0;
          if (c2 == MD_EMPTY) { sl2[0]='-'; sa2[0]='-'; sa2[1]='-'; }
          else { const char x = md_table_cmd_letter(c2); sl2[0] = x ? x : '?';
                 sa2[0]=kHex[(v2>>4)&15]; sa2[1]=kHex[v2&15]; }
          sl2[1]=0; sa2[2]=0;
          if (mc == MD_EMPTY) { sm[0]='-'; sm[1]='-'; smv[0]='-'; smv[1]='-'; }
          else { sm[0]=kHex[(mc>>4)&15]; sm[1]=kHex[mc&15];
                 smv[0]=kHex[(mv>>4)&15]; smv[1]=kHex[mv&15]; }
          sm[2]=0; smv[2]=0;
          const char *txt[8] = { sv, st, sl1, sa1, sl2, sa2, sm, smv };

          for (int k = 0; k < 8; k++) {
            efface(col[k], lig, larg[k]);
            const bool ici = (l == tabLigne && k == tabCol);
            // Le curseur est un PAVE ROUGE, comme sur toutes les autres pages.
            if (ici) {
              const int x0 = (col[k] + g_colOrigine) * MD_CELL_W;
              const int y0 = lig * MD_CELL_H;
              for (int y = y0; y < y0 + MD_GLYPHE_H; y++)
                for (int x = x0 - 1; x < x0 + larg[k] * MD_CELL_W; x++)
                  pixel(x, y, kAccent);
            }
            texte(col[k], lig, txt[k], ici ? kFond : kData);
          }
        }
        for (int g = 0; g < 5; g++) repereTab[g] = -2;   // a redessiner
      }

      // ── Les cinq reperes de lecture ────────────────────────────────────
      // Un chevron devant chaque colonne, a la ligne qu'elle est en train de
      // lire. On ne touche QUE ceux qui ont bouge : les repeindre tous a chaque
      // image, c'etait la page entiere qui scintillait.
      for (int g = 0; g < 5; g++) {
        const int r = enLecture
            ? md_replayer_play_table_pos(voieCourante, chevFlux[g]) : -1;
        if (r == repereTab[g]) continue;
        if (repereTab[g] >= 0 && repereTab[g] < MD_TABLE_ROWS)
          efface(chevCol[g], 3 + repereTab[g], 1);
        if (r >= 0 && r < MD_TABLE_ROWS)
          chevron(chevCol[g], 3 + r, kAccent);
        repereTab[g] = r;
      }
    }

    if (page == PAGE_CHAIN || page == PAGE_PHRASE) {
      // ── Quel chain, quelle phrase ? ──────────────────────────────────
      // Le chain montre est celui pointe dans SONG ; la phrase montree est
      // celle pointee dans CHAIN. C'est la chaine de navigation de LSDJ.
      const uint8_t noChain = (uint8_t)chainId;
      const uint8_t noPhrase = (uint8_t)phraseId;

      // Cet en-tete se reecrivait a CHAQUE image, comme les lignes le
      // faisaient avant : d'ou les memes bandes de vieux televiseur dessus, et
      // le meme temps vole au son. On ne le redessine que s'il change.
      static int vuMontre = -1, vuPageT = -1;
      char t[8];
      const uint8_t montre = (page == PAGE_CHAIN) ? noChain : noPhrase;
      if (ecranEfface || montre != vuMontre || page != vuPageT) {
      vuMontre = montre; vuPageT = page;
      efface(kColRepere, 0, 8);
      t[0] = (page == PAGE_CHAIN) ? 'C' : 'P';
      t[1] = (page == PAGE_CHAIN) ? 'H' : 'H';
      t[2] = ' ';
      t[3] = kHex[(montre >> 4) & 15]; t[4] = kHex[montre & 15]; t[5] = 0;
      texte(COL_REPERE(t), 0, t, kEntete);
      }

      const int nl = (page == PAGE_CHAIN) ? MD_ROWS_PER_CHAIN : MD_ROWS_PER_PHRASE;

      // ── On ne repeint QUE si quelque chose a change ────────────────────
      // Ces seize lignes etaient redessinees a CHAQUE image, curseur immobile
      // et donnees inchangees comprises. Deux consequences : les ecritures
      // couraient apres le balayage de l'ecran — d'ou les bandes noires qui
      // traversent l'image, comme sur un vieux televiseur — et tout ce temps
      // etait vole a la production du son. C'est pour ca que le son se degrade
      // surtout sur ces pages.
      const int idCourant = (page == PAGE_CHAIN) ? chainId : phraseId;
      const int ligCour   = (page == PAGE_CHAIN) ? chLigne : phLigne;
      const int colCour   = (page == PAGE_CHAIN) ? chCol   : phCol;
      static int vuPage2 = -1, vuId = -1, vuLig2 = -1, vuCol2 = -1;
      static bool selVue2 = false;
      const bool selFinie = selVue2 && !selActive;
      selVue2 = selActive;
      const bool refaireGrille = ecranEfface || modifie || selActive ||
                                 selFinie || (page != vuPage2) ||
                                 (idCourant != vuId) || (ligCour != vuLig2) ||
                                 (colCour != vuCol2);
      vuPage2 = page; vuId = idCourant; vuLig2 = ligCour; vuCol2 = colCour;

      for (int l = 0; refaireGrille && l < nl; l++) {
        if (l == 0) entretienSon();
        int lig = 3 + l;
#ifdef FM1
        if (selActive || selFinie) efface(0, lig, 41);   // (le cadre d'avant s'en va)
#endif
        numeroLigne(0, lig, l,
                    (page == PAGE_CHAIN ? l == chLigne : l == phLigne));

        if (page == PAGE_CHAIN) {
          uint8_t ph = MD_EMPTY; int8_t tsp = 0;
          if (noChain != MD_EMPTY) md_replayer_get_chain(noChain, l, &ph, &tsp);
          char a[3], b[3];
          if (ph == MD_EMPTY) { a[0]='-'; a[1]='-'; } else { a[0]=kHex[(ph>>4)&15]; a[1]=kHex[ph&15]; }
          a[2]=0;
          uint8_t u = (uint8_t)tsp;
          b[0]=kHex[(u>>4)&15]; b[1]=kHex[u&15]; b[2]=0;
          // Meme regle que sur SONG : pendant une selection, le rouge tient
          // l'ancre et le jaune s'etend.
#ifdef FM1
          // FM-1 : pendant une selection, le curseur en pave jaune (plus de rouge).
          const bool r0 = !selActive && l == chLigne && chCol == 0;
          const bool r1 = !selActive && l == chLigne && chCol == 1;
          const bool j0 = selActive && l == chLigne && chCol == 0;
          const bool j1 = selActive && l == chLigne && chCol == 1;
#else
          const bool r0 = selActive ? (l == selAncreLigne && selAncreCol == 0)
                                    : (l == chLigne && chCol == 0);
          const bool r1 = selActive ? (l == selAncreLigne && selAncreCol == 1)
                                    : (l == chLigne && chCol == 1);
          const bool j0 = false, j1 = false;
#endif
          const u16 kNorm = kData;
          efface(3, lig, 2);
#ifdef FM1
          if (j0) paveJaune(3, lig, 2);
#endif
          texte(3, lig, a, j0 ? kFond : r0 ? kAccent
                          : estSelectionne(l, 0) ? kSelection : kNorm);
          efface(6, lig, 2);
#ifdef FM1
          if (j1) paveJaune(6, lig, 2);
#endif
          texte(6, lig, b, j1 ? kFond : r1 ? kAccent
                          : estSelectionne(l, 1) ? kSelection : kNorm);
        } else {
          uint8_t no=0,ins=0,vel=0,cmd=MD_EMPTY,cv=0,mc=MD_EMPTY,mv=0;
          if (noPhrase != MD_EMPTY)
            md_replayer_get_phrase(noPhrase, l, &no,&ins,&vel,&cmd,&cv,&mc,&mv);
          static const char *gam[12] = {"C-","C#","D-","D#","E-","F-",
                                        "F#","G-","G#","A-","A#","B-"};
          char nt[4];
          if (no == 0) { nt[0]='-'; nt[1]='-'; nt[2]='-'; }
          else if (no == MD_EMPTY) { nt[0]='O'; nt[1]='F'; nt[2]='F'; }
          else { const char *g = gam[(no-1)%12]; nt[0]=g[0]; nt[1]=g[1];
                 nt[2]=(char)('0'+((no-1)/12)); }
          nt[3]=0;
          char si[3]={'-','-',0}, sv2[3]={'-','-',0};
          if (ins) { si[0]=kHex[(ins>>4)&15]; si[1]=kHex[ins&15]; }
          if (vel != MD_EMPTY) { sv2[0]=kHex[(vel>>4)&15]; sv2[1]=kHex[vel&15]; }
          // La colonne CMD porte un INDICE dans la table des commandes, pas
          // une lettre : celle-ci se lit par md_table_cmd_letter(). En
          // affichant l'indice tel quel, l'indice 0 donnait le caractere nul,
          // qui termine la chaine — la case disparaissait entierement.
          char sl[2]={'-',0}, sc[3]={'-','-',0};
          if (cmd != MD_EMPTY) {
            const char l2 = md_table_cmd_letter(cmd);
            sl[0] = l2 ? l2 : '?';
            sc[0]=kHex[(cv>>4)&15]; sc[1]=kHex[cv&15];
          }
          // Les QUATRE colonnes CC : un numero puis sa valeur, en hexadecimal.
          char sccN[4][3], sccV[4][3];
          for (int k2 = 0; k2 < 4; k2++) {
            uint8_t n2 = MD_EMPTY, v2 = 0;
            if (noPhrase != MD_EMPTY)
              md_replayer_get_phrase_cc(noPhrase, l, k2, &n2, &v2);
            // Comme AT2 : le caractere de l'effet, puis sa valeur.
            if (n2 == MD_EMPTY || n2 >= kNbFxAT2) {
              strcpy(sccN[k2], "-"); strcpy(sccV[k2], "--");
            } else {
              sccN[k2][0] = kFxAT2[n2]; sccN[k2][1] = 0;
              sccV[k2][0] = kHex[(v2 >> 4) & 15]; sccV[k2][1] = kHex[v2 & 15];
              sccV[k2][2] = 0;
            }
          }
          (void)mc; (void)mv;
          // TREIZE positions de curseur, chacune sur ce qu'elle modifie. Tout
          // tient dans les 36 colonnes de l'ecran avec un espace entre chaque
          // groupe : NOTE, INS, VEL, CMD, puis les quatre CC — le premier
          // colle a la commande.
          // Chaque FX : son caractere colle a sa valeur, comme dans AT2.
#ifdef FM1
          // FM-1 : ni VL, ni FX3, ni FX4 (-1) ; une case d'ecart entre les colonnes.
          const int cols[13] = {3, 7, -1, 10, 11, 14, 15, 18, 19, -1, -1, -1, -1};
#else
          const int cols[13] = {3, 7, 10, 13, 14, 17, 18, 22, 23, 27, 28, 32, 33};
#endif
          const int larg[13] = {3, 2,  2,  1,  2,  1,  2,  1,  2,  1,  2,  1,  2};
          const char *txt[13] = {nt, si, sv2, sl, sc,
                                 sccN[0], sccV[0], sccN[1], sccV[1],
                                 sccN[2], sccV[2], sccN[3], sccV[3]};
          const u16 kNorm = kData;
          for (int k = 0; k < 13; k++) {
            if (cols[k] < 0) continue;
#ifdef FM1
            if (!phVisible(phGroupe(k))) continue;   // (ZOOM : hors de l'ecran)
            const int ck = cols[k] - phDecal(phGauche);
#else
            const int ck = cols[k];
#endif
            efface(ck, lig, larg[k]);
#ifdef FM1
            const bool rk = !selActive && l == phLigne && phCol == k;
            const bool jk = selActive && l == phLigne && phCol == k;
            if (jk) paveJaune(ck, lig, larg[k]);
#else
            const bool rk = selActive ? (l == selAncreLigne && selAncreCol == k)
                                      : (l == phLigne && phCol == k);
            const bool jk = false;
#endif
            texte(ck, lig, txt[k],
                  jk ? kFond : rk ? kAccent
                  : estSelectionne(l, k) ? kSelection : kNorm);
          }
        }
      }
#ifdef FM1
      // FM-1 : le cadre jaune autour de la selection.
      if (refaireGrille && selActive) {
        const int l0 = selAncreLigne < curL() ? selAncreLigne : curL();
        const int l1 = selAncreLigne < curL() ? curL() : selAncreLigne;
        const int c0 = selAncreCol < curC() ? selAncreCol : curC();
        const int c1 = selAncreCol < curC() ? curC() : selAncreCol;
        if (page == PAGE_CHAIN) {
          cadreJaune(c0 ? 6 : 3, c1 ? 8 : 5, 3 + l0, 3 + l1);
        } else {
          static const int colsC[13] = {3, 7, -1, 10, 11, 14, 15, 18, 19, -1, -1, -1, -1};
          static const int largC[13] = {3, 2,  2,  1,  2,  1,  2,  1,  2,  1,  2,  1,  2};
          int ka = -1, kb = -1;
          for (int k = c0; k <= c1 && k < 13; k++)
            if (colsC[k] >= 0 && phVisible(phGroupe(k))) { if (ka < 0) ka = k; kb = k; }
          if (ka >= 0)
            cadreJaune(colsC[ka] - phDecal(phGauche), colsC[kb] - phDecal(phGauche) + largC[kb], 3 + l0, 3 + l1);
        }
      }
#endif
    }

    if (page == PAGE_SONG) {

    // La colonne d'ecran d'une piste, ou -1 si elle est hors de la vue.
    auto colDe = [&](int c) {
      const int k = c - gaucheVoie;
      return (k < 0 || k >= nbVoiesVues) ? -1 : kColVoie + k * kPasVoie;
    };
    // Dessine une case : le fond, le curseur eventuel, puis la valeur.
    auto dessineCase = [&](int c, int ligne, bool ici) {
      int l = ligne - haut;
      if (l < 0 || l >= kLignesVues) return;
      const int col = colDe(c);
      if (col < 0) return;
      if (c >= nbVisibles) {
        // La colonne « + » : rien dedans, son en-tete s'allume sous le curseur.
        efface(col, 3 + l, 2);
        if (ici) texte(col, 3 + l, "+", kAccent);
        return;
      }
      uint8_t v = md_replayer_get_song(reel(c), ligne);
      // Le chain est-il vide ? Toutes ses phrases sans note ni commande.
      bool chainVide = (v != MD_EMPTY);
      for (int r2 = 0; chainVide && r2 < MD_ROWS_PER_CHAIN; r2++) {
        uint8_t ph; int8_t tsp;
        md_replayer_get_chain(v, r2, &ph, &tsp);
        if (ph == MD_EMPTY) continue;
        for (int l2 = 0; chainVide && l2 < MD_ROWS_PER_PHRASE; l2++) {
          md_phrase_row_t pr;
          md_replayer_get_phrase_row(ph, l2, &pr);
          // Un H seul (un saut, comme le H00 de fin de pattern) ne joue rien.
          const bool saut = pr.cmd != MD_EMPTY && md_table_cmd_kind(pr.cmd) == MD_CMD_HOP;
          if ((pr.note && pr.note != MD_EMPTY) || pr.note == MD_EMPTY || pr.instr ||
              pr.vel != MD_EMPTY || (pr.cmd != MD_EMPTY && !saut) || pr.mdcmd != MD_EMPTY ||
              pr.cc[0] != MD_EMPTY || pr.cc[1] != MD_EMPTY || pr.cc[2] != MD_EMPTY)
            chainVide = false;
        }
      }
      char cel[3];
      if (v == MD_EMPTY) { cel[0]='-'; cel[1]='-'; }
      else { cel[0]=kHex[(v>>4)&15]; cel[1]=kHex[v&15]; }
      cel[2] = 0;
#ifdef FM1
      efface(col, 3 + l, 3);   // (et la case libre : le point de plus du curseur ; le spaghetti y revient, plus bas)
#else
      efface(col, 3 + l, 2);
#endif
      const bool sel = estSelectionne(ligne, c);
      // Pendant une selection, le rouge marque l'ANCRE — le point fixe d'ou
      // part la plage — et c'est le jaune qui s'etend.
#ifdef FM1
      // FM-1 : non, le curseur devient un pave JAUNE, par-dessus tout, et un
      // cadre jaune entoure la selection (plus bas).
      if (selActive && ici) paveJaune(col, 3 + l, 2);
      else
#else
      if (selActive)
        ici = (ligne == selAncreLigne && c == selAncreCol);
#endif
      if (ici) {
        int x0 = (col + g_colOrigine) * MD_CELL_W, y0 = (3 + l) * MD_CELL_H;
#ifdef FM1
        // FM-1 : un point de plus a droite (l'ecran mange l'espace apres le
        // second chiffre : le pave y etait colle), comme celui de gauche.
        for (int y = y0; y < y0 + MD_GLYPHE_H; y++)
          for (int x = x0 - 1; x < x0 + 2 * MD_CELL_W + 1; x++) pixel(x, y, kAccent);
#else
        for (int y = y0; y < y0 + MD_GLYPHE_H; y++)
          for (int x = x0 - 1; x < x0 + 2 * MD_CELL_W; x++) pixel(x, y, kAccent);
#endif
      }
      texte(col, 3 + l, cel, ici ? kFond : (sel ? kSelection : (chainVide ? kVide : kData)));
    };
    auto dessineNumero = [&](int ligne) {
      int l = ligne - haut;
      if (l < 0 || l >= kLignesVues) return;
      numeroLigne(0, 3 + l, ligne, ligne == curLigne);
    };

    // Pendant une selection, la plage change des que le curseur bouge : on
    // repeint tout, et UNE IMAGE DE PLUS apres qu'elle se termine.
    static bool selVue = false;
    static int vuGauche = -1;
    if (selActive || selVue) vuHaut = -1;
    selVue = selActive;
    // Une modification, un defilement vertical OU horizontal repeint toute la
    // grille.
    if (haut != vuHaut || modifie || gaucheVoie != vuGauche) {
      // Les noms de pistes defilent avec la grille.
      if (gaucheVoie != vuGauche || ecranEfface) {
        const int decalNoms = FM1_DECAL_NOMS((MD_GLYPHE_H + 3 * MD_CELL_H) / 2 - MD_GLYPHE_H / 2
                              - MD_CELL_H);
        efface(0, 1, kCols);
        efface(0, 2, kCols);
#ifdef FM1
        if (zoomNiv) {
          // FM-1, en ZOOM : les noms de pistes vont dans l'en-tete, sur la
          // ligne de « SONG », a sa taille, chacun centre sur sa colonne. Ils
          // s'ecrivent hors de la vue de la rangee 0 (cellules 30 a 39 : l'en-tete
          // en montre 30 en 4 COL, 24 en 3 COL),
          // l'en-tete les place au point pres (fm1_mot_entete).
          fm1_mots_entete_efface();
          const int origine = g_colOrigine;
          g_colOrigine = 0;
          for (int c = gaucheVoie, k = 0; c < gaucheVoie + nbVoiesVues; c++, k++) {
            const char *nm = c >= nbVisibles ? "+" : noms[reel(c)];
            const int lg = (int)strlen(nm), cm = 30 + 2 * k;
            efface(cm, 0, 2);
            texte(cm, 0, nm, kTitre);
            fm1_mot_entete(cm, lg, colDe(c) + origine, c >= nbVisibles ? 1 : 2);
          }
          g_colOrigine = origine;
        } else {
#endif
        g_decalYpx = decalNoms;
        for (int c = gaucheVoie; c < gaucheVoie + nbVoiesVues; c++)
          texte(colDe(c), 1, c >= nbVisibles ? "+" : noms[reel(c)], kTitre);
#ifdef FM1
        fm1_titres_colonnes(1);   // FM-1 : cette rangee s'affiche en petit
        }
#endif
        g_decalYpx = 0;
      }
      for (int l = 0; l < kLignesVues; l++) {
        if ((l & 7) == 0) entretienSon();
#ifdef FM1
        efface(0, 3 + l, 41);                      // (un cadre de selection d'avant s'en va)
#endif
        dessineNumero(haut + l);
        // La colonne du chevron AUSSI : sinon les anciens chevrons rouges
        // restent a l'ecran en defilant. Toutes AVANT les cases : la case du
        // chevron d'une voie est juste a droite de la voie d'avant, et l'effacer
        // apres coup mangeait le point de droite du curseur.
        for (int c = gaucheVoie; c < gaucheVoie + nbVoiesVues; c++) efface(colDe(c) - 1, 3 + l, 1);
        for (int c = gaucheVoie; c < gaucheVoie + nbVoiesVues; c++)
          dessineCase(c, haut + l, c == curCanal && haut + l == curLigne);
      }
#ifdef FM1
      if (selActive) {                             // le cadre jaune, sur ce qui est a l'ecran
        int l0 = selAncreLigne < curLigne ? selAncreLigne : curLigne;
        int l1 = selAncreLigne < curLigne ? curLigne : selAncreLigne;
        int c0 = selAncreCol < curCanal ? selAncreCol : curCanal;
        int c1 = selAncreCol < curCanal ? curCanal : selAncreCol;
        if (l0 < haut) l0 = haut;
        if (l1 > haut + kLignesVues - 1) l1 = haut + kLignesVues - 1;
        if (c0 < gaucheVoie) c0 = gaucheVoie;
        if (c1 > gaucheVoie + nbVoiesVues - 1) c1 = gaucheVoie + nbVoiesVues - 1;
        if (l0 <= l1 && c0 <= c1 && colDe(c0) >= 0 && colDe(c1) >= 0)
          cadreJaune(colDe(c0), colDe(c1) + 2, 3 + l0 - haut, 3 + l1 - haut);
      }
#endif
      for (int c = 0; c < MD_MAX_CHANNELS; c++) repereVu[c] = -1;
    } else if (curCanal != vuCanal || curLigne != vuLigne) {
      if (vuCanal >= 0) { dessineCase(vuCanal, vuLigne, false); dessineNumero(vuLigne); }
      dessineCase(curCanal, curLigne, true);
      dessineNumero(curLigne);
    }
    vuCanal = curCanal; vuLigne = curLigne; vuHaut = haut; vuGauche = gaucheVoie;

    // ── Les reperes de lecture ────────────────────────────────────────
    // Un chevron ROUGE devant la case que chaque piste est en train de jouer.
    // Les voies de la DS jouent sans avance : la ligne du moteur est celle
    // qu'on entend. On ne redessine que ceux qui ont bouge.
    bool chevronsBouges = false;
    for (int c = gaucheVoie; c < gaucheVoie + nbVoiesVues && c < nbVisibles; c++) {
      const int colC = colDe(c) - 1;
      int r = enLecture ? md_replayer_play_song_row(reel(c)) : -1;
      if (r == repereVu[c]) continue;
      if (repereVu[c] >= 0) {
        int l = repereVu[c] - haut;
        if (l >= 0 && l < kLignesVues) efface(colC, 3 + l, 1);
      }
      if (r >= 0) {
        int l = r - haut;
        if (l >= 0 && l < kLignesVues) {
          efface(colC, 3 + l, 1);
          chevron(colC, 3 + l, kAccent);
        }
      }
      repereVu[c] = r;
      chevronsBouges = true;
    }
    // (effacer un chevron mange le point de droite du curseur, s'il est sur la voie d'avant : on le remet)
    if (chevronsBouges) dessineCase(curCanal, curLigne, true);

    }  // fin de la page SONG

    // ── Le repere de lecture des pages CHAIN et PHRASE ────────────────
    // Il n'existait que sur SONG : on voyait donc la lecture avancer dans la
    // chanson, mais jamais dans le chain ni dans la phrase qu'on editait.
    // Le chevron ne s'allume que si le chain / la phrase AFFICHE est bien
    // celui que le canal joue — sinon il montrerait la ligne d'un autre.
    if (page == PAGE_CHAIN || page == PAGE_PHRASE) {
      int r = -1;
      if (enLecture) {
        // voieCourante, et non curCanal : ce dernier est un indice de COLONNE
        // AFFICHEE sur la page SONG, qui ne coincide pas avec le numero de
        // voie des que des colonnes sont masquees. Sur PCM, PSG et le bruit,
        // le repere ne trouvait donc jamais rien et restait invisible.
        if (page == PAGE_CHAIN) {
          if (md_replayer_play_chain(voieCourante) == chainId)
            r = md_replayer_play_chain_row(voieCourante);
        } else {
          if (md_replayer_play_phrase(voieCourante) == phraseId)
            r = md_replayer_play_phrase_row(voieCourante);
        }
      }
      const int colRep = 2;   // le chevron, juste apres le numero de ligne
      if (r != repereLigne) {
        if (repereLigne >= 0) efface(colRep, 3 + repereLigne, 1);
        if (r >= 0) { efface(colRep, 3 + r, 1);
                      chevron(colRep, 3 + r, kAccent); }
        repereLigne = r;
      }
    }


    // ── L'etat du morceau, sur l'ecran du bas ───────────────────────────
    // La mesure qui vivait ici n'a plus lieu d'etre : tout ce qu'elle disait
    // part maintenant dans le journal de la carte, ou on peut le relire. On
    // rend la place a ce qui sert pendant qu'on travaille, et on la prend en
    // BAS pour degager le haut de l'ecran.
    {
      // ⚠️ « DSI 134MHZ  BPM xxx » est PARTI : c'etait une mesure de mise au
      // point posee sous les yeux en permanence. Le BPM se regle et se lit
      // dans PROJECT, la frequence n'interesse que le journal. Il ne reste
      // que le nom du morceau.
      // (Le titre du morceau passe sur la ligne d'etat, plus bas.)

    }

    // ── LE NOM DE LA COMMANDE, SUR L'ECRAN DU BAS ──────────────────────
    // ⚠️ IL ETAIT SUR L'ECRAN DU HAUT, sous la grille — la ou personne ne le
    // cherchait. Une lettre seule ne se retient pas : « U » ne dit pas « fine
    // tune ». Le nom s'affiche donc en clair pendant qu'on parcourt les
    // commandes, et il appartient a l'ecran du bas, qui est fait pour ca.
    //
    // ⚠️ CALCULE A UN SEUL ENDROIT, ET A CHAQUE IMAGE. Les deux versions
    // precedentes vivaient dans les blocs de dessin des pages PHRASE et
    // TABLE : le nom restait donc affiche en quittant la page, puisque plus
    // personne ne passait par la pour l'effacer. Ici, quitter les colonnes de
    // commande — ou la page entiere — le fait disparaitre tout seul.
    {
      const char *nomCmd = 0;
      const uint8_t nNom  = (uint8_t)(sizeof(kNomCmd) / sizeof(kNomCmd[0]));
      // Un CC se lit par son numero en hexadecimal ET en decimal : c'est en
      // decimal que les synthetiseurs les annoncent.
      static char texteCC[40];
      // Le numero seul, sans nom : les noms de la norme (BREATH, FOOT PEDAL…)
      // ne valent pas pour les synthes qui assignent leurs CC eux-memes — sur
      // Model D, le CC 02 est la coupure du filtre.
      auto nomDuCC = [&](uint8_t cc) {
        // En DECIMAL, et rien d'autre : c'est ainsi que les synthes numerotent
        // leurs CC. La colonne est en hexadecimal (0C) ; afficher « CC 0C (12) »
        // faisait lire 0C comme le numero a chercher dans le synthe.
        siprintf(texteCC, "CC %d", cc);
        return (const char *)texteCC;
      };
      if (page == PAGE_PHRASE && !navigateur && !dialogueNom) {
        uint8_t no,i3,ve,cm,cv,mc,mv;
        md_replayer_get_phrase((uint8_t)phraseId, phLigne,
                               &no,&i3,&ve,&cm,&cv,&mc,&mv);
        if ((phCol == 3 || phCol == 4) && cm != MD_EMPTY && cm < nNom)
          nomCmd = kNomCmd[cm];
        else if (phCol >= 5) {
          uint8_t ccN, ccV;
          md_replayer_get_phrase_cc((uint8_t)phraseId, phLigne, (phCol - 5) / 2,
                                    &ccN, &ccV);
          if (ccN != MD_EMPTY && ccN < kNbFxAT2) nomCmd = kNomFxAT2[ccN];
        }
      } else if (page == PAGE_TABLE && !navigateur && !dialogueNom) {
        uint8_t vo, c1, v1, c2, v2, mc2, mv2; int8_t tr;
        md_replayer_get_table_row(tableId, tabLigne, &vo, &tr,
                                  &c1, &v1, &c2, &v2, &mc2, &mv2);
        if ((tabCol == 2 || tabCol == 3) && c1 != MD_EMPTY && c1 < nNom)
          nomCmd = kNomCmd[c1];
        else if ((tabCol == 4 || tabCol == 5) && c2 != MD_EMPTY && c2 < nNom)
          nomCmd = kNomCmd[c2];
        else if ((tabCol == 6 || tabCol == 7) && mc2 != MD_EMPTY && mc2 < 128)
          nomCmd = nomDuCC(mc2);
      } else if (page == PAGE_INSTR && !navigateur && !dialogueNom &&
                 !dialogueMidi) {
        // Sur la ligne CHANNEL d'un instrument MIDI : a quoi ce canal est
        // branche. L'ecran du bas est fait pour ca, et la page est pleine.
        if (genreInstr(instrId) == GENRE_MIDI && inLigne == 1) {
          static char cibleTxt[48];
          const int cn = md_replayer_get_instr_midi_channel(instrId);
          char s[16], p[16], m[16];
          md_replayer_get_midi_cible(cn, s, p, m, 16);
          if (s[0] || p[0] || m[0])
            siprintf(cibleTxt, "CH%02d %.9s %.9s %.8s", cn + 1,
                     s[0] ? s : "-", p[0] ? p : "-", m[0] ? m : "-");
          else
            siprintf(cibleTxt, "CH%02d  NOT SET YET", cn + 1);
          nomCmd = cibleTxt;
        }
      }
#ifdef FM1
      // FM-1 : une commande (CMD, FX, CC) qu'on vient de CHANGER, sur la case du
      // curseur : son nom en gros au milieu de l'ecran (coupe en deux lignes,
      // a l'espace le plus proche du milieu, s'il est long).
      {
        static char vuNomPos[48] = "";
        static int vuPos = -1;
        const int pos = page == PAGE_PHRASE ? (phraseId * 16 + phLigne) * 16 + phCol
                      : page == PAGE_TABLE ? 0x100000 + (tableId * 16 + tabLigne) * 16 + tabCol : -1;
        const char *nm = (page == PAGE_PHRASE || page == PAGE_TABLE) ? nomCmd : 0;
        if (pos == vuPos && nm && strcmp(nm, vuNomPos)) {
          char l1[32], l2[32];
          const int lg = (int)strlen(nm);
          int coupe = -1;
          if (lg > 11)
            for (int d = 0; d < lg && coupe < 0; d++) {
              if (lg / 2 - d > 0 && nm[lg / 2 - d] == ' ') coupe = lg / 2 - d;
              else if (lg / 2 + d < lg && nm[lg / 2 + d] == ' ') coupe = lg / 2 + d;
            }
          // (au-dessus ou en dessous du curseur, jamais par-dessus)
          const int rangeeCur = 3 + (page == PAGE_PHRASE ? phLigne : tabLigne);
          if (coupe > 0) {
            snprintf(l1, sizeof l1, "%.*s", coupe, nm);
            snprintf(l2, sizeof l2, "%s", nm + coupe + 1);
            fm1_annonce2_evite(l1, l2, rangeeCur);
          } else fm1_annonce2_evite(nm, 0, rangeeCur);
        }
        vuPos = pos;
        snprintf(vuNomPos, sizeof vuNomPos, "%s", nm ? nm : "");
      }
#endif
      // On ne repeint QUE si le texte change : redessine a chaque image, il
      // raye l'ecran de bandes pendant la lecture, comme le menu le faisait.
      // ── LA LIGNE D'ETAT, la derniere de l'ecran ──────────────────────────
      // Le nom de la commande ou de l'effet sous le curseur s'il y en a un,
      // sinon le titre du morceau. Rien sur la page INSTRUMENT, qui se sert
      // de toutes les lignes.
      static char vuNomCmd[48] = "\x01";
      static int vuPageNom = -1;
      static char etat[48];
      if (page == PAGE_INSTR) etat[0] = 0;
      else if (nomCmd) snprintf(etat, sizeof etat, "%s", nomCmd);
#ifdef FM1
      else etat[0] = 0;   // FM-1 : jamais le nom du morceau, rien que l'aide des commandes
#else
      else snprintf(etat, sizeof etat, "%s", nomProjet[0] ? nomProjet : "(UNNAMED)");
#endif
      const char *n2 = etat;
      const bool centre = nomCmd != 0;
      // ⚠️ LA PAGE FAIT PARTIE DE LA CLE. Les nettoyages de l'ecran du bas
      // effacent sans prevenir ce cache ; sans la page, un texte inchange
      // n'etait jamais repose et la ligne restait vide pour de bon.
      if (ecranEfface || strncmp(vuNomCmd, n2, sizeof vuNomCmd - 1) != 0 || page != vuPageNom) {
        strncpy(vuNomCmd, n2, sizeof vuNomCmd - 1);
        vuNomCmd[sizeof vuNomCmd - 1] = 0;
        vuPageNom = page;
        const int garde = g_colOrigine;
        g_colOrigine = 0;
        // ⚠️ RANGEE 4, PAS 20. Les rangees au-dela de la 18 ne sont peintes
        // par personne : trame() ecrit dans g_fond EN DUR, sans passer par la
        // cible courante, si bien que « ecran(bas); trame(); » au demarrage a
        // repeint l'ecran du HAUT. Le bas n'a donc jamais recu de fond, et ce
        // qu'on y ecrivait tout en bas ne se voyait pas. La bande 3-18, elle,
        // est entretenue par la page FM et par le nettoyage de changement de
        // page : on sait qu'elle s'affiche.
        if (page != PAGE_INSTR) {
          efface(0, kLignes - 1, kCols);
          if (n2[0]) {
            int lg = 0; while (n2[lg]) lg++;
            if (centre) titre((kCols - lg) / 2, kLignes - 1, n2, kTitre);
            else texte(0, kLignes - 1, n2, kEntete);
          }
        }
        g_colOrigine = garde;
      }
    }

    // ── QUELLE VERSION TOURNE ? ────────────────────────────────────────
    // ⚠️ On a perdu des heures, des deux cotes, a chercher un defaut dans du
    // code deja repare parce qu'un binaire perime etait a l'essai — et rien a
    // l'ecran ne permettait de s'en apercevoir. L'heure de compilation est
    // gravee dans le binaire : elle suffit a repondre en une seconde.
    // (L'heure de compilation ne s'affiche plus : elle est dans le journal.)

    // On n'ecrit sur la carte que lecture ARRETEE.
    //
    // Il y avait ici une exception « sauf si le tampon est presque plein :
    // mieux vaut un a-coup qu'une perte ». C'etait un mauvais marche : mesure
    // sur la console, l'a-coup fait jusqu'a 1,9 SECONDE — l'anneau se vide, la
    // lecture saute, et c'est exactement le defaut qu'on cherchait a consigner.
    // Perdre quelques lignes de journal ne coute rien en comparaison ; elles
    // sont comptees et le total est ecrit des qu'on s'arrete.
    if (journalSale && !enLecture) videJournal();

    // ── La fenetre de nom, dessinee EN DERNIER ──────────────────────────
    // Elle etait au milieu de la chaine des pages, et une partie de son
    // contenu se faisait repeindre par-dessus par un autre bloc : le cadre et
    // la grille survivaient, le titre et le champ du nom disparaissaient.
    // Une fenetre se pose au-dessus de tout, donc elle se dessine en dernier.
    // ── La fenetre du MIDI, dessinee comme celle du nom ─────────────────────
    if (page == PAGE_PROJECT && dialogueMidi) {
      const int decalFenetre = g_colOrigine;
      g_colOrigine = 0;
      static const char *grille[4] = { "0123456789",
                                       "ABCDEFGHIJ",
                                       "KLMNOPQRST",
                                       "UVWXYZ-_<*" };
      if (assombrirDemande) { assombrit(); assombrirDemande = false; }

      // Une rangee de plus que la fenetre du nom, et donc remontee d'autant :
      // il y a TROIS champs a montrer.
      const int fh = 13;
      const int fl = (kLignes - fh - 1) > 1 ? (kLignes - fh - 1) : 1;
      const int fc = 4, fw = 32;
      fenetre(fc, fl, fw, fh);
      for (int r = 0; r < 4; r++)
        for (int c2 = 0; c2 < 10; c2++) {
          const int cx = fc + 2 + c2 * 3, cy = fl + 6 + r * 2;
          const bool ici = (nomLig == r && nomCol == c2);
          const bool estOK = (grille[r][c2] == '*');
          if (ici) {
            const int x0 = cx * MD_CELL_W - 1, y0 = cy * MD_CELL_H - 1;
            const int lg = (estOK ? 2 : 1) * MD_CELL_W + 2;
            for (int y = y0; y < y0 + MD_GLYPHE_H + 3; y++)
              for (int x = x0; x < x0 + lg; x++) pixel(x, y, kAccent);
          }
          char c1[2] = { grille[r][c2], 0 };
          texte(cx, cy, estOK ? "OK" : c1, ici ? kFond : kData);
        }

      // Les deux champs, et lequel on est en train d'ecrire.
      // Le titre reste COURT : en police agrandie, vingt caracteres mordaient
      // sur le rappel de touches pose a droite, et les deux se melangeaient.
      // Pendant un enregistrement il dit AUSSI ou l'on en est — sinon la
      // fenetre qui revient pour le canal suivant passe pour un blocage.
      { char t[24];
        if (midiApresSave && midiTotal > 1)
          siprintf(t, "MIDI CH%02d  %d/%d", midiCanal + 1, midiRang, midiTotal);
        else
          siprintf(t, "MIDI CH%02d", midiCanal + 1);
        titre(fc + 2, fl + 1, t, kTitre); }
      // Ce que fait OK a cette etape, sur sa PROPRE ligne juste au-dessus de
      // la grille : plus rien ne se chevauche.
      efface(fc + 2, fl + 5, fw - 4);
      texte(fc + 2, fl + 5,
            (midiChamp == 0) ? "OK GOES TO PROJ"
          : (midiChamp == 1) ? "OK GOES TO MAP"
          : midiApresSave    ? "OK SAVES SONG" : "OK STORES", kAttenue);
      texte(fc + 21, fl + 5, "B CANCELS", kAttenue);
      const char *val[3] = { midiSoft, midiProj, midiMap };
      static const char *nomChamp[3] = { "SOFT", "PROJ", "MAP" };
      for (int i2 = 0; i2 < 3; i2++) {
        const bool ici = (midiChamp == i2);
        const int lig = fl + 2 + i2;
        efface(fc + 2, lig, fw - 4);
        titre(fc + 2, lig, nomChamp[i2], ici ? kAccent : kTitre);
        texte(fc + 8, lig, val[i2][0] ? val[i2] : "-", ici ? kEntete : kData);
        // Le pave d'ecriture, la ou la prochaine lettre ira.
        if (ici) {
          const int j = (int)strlen(val[i2]);
          const int cx = (fc + 8 + j) * MD_CELL_W;
          const int cy = lig * MD_CELL_H;
          for (int y = cy; y < cy + MD_GLYPHE_H; y++)
            for (int x = cx; x < cx + MD_GLYPHE_W; x++) pixel(x, y, kAttenue);
        }
      }
      g_colOrigine = decalFenetre;
    }

    if (page == PAGE_PROJECT && dialogueNom) {
      // Une fenetre pose ses PROPRES coordonnees : elle ne doit pas heriter du
      // centrage de la page. Le texte passe par texte(), qui applique ce
      // decalage ; les rectangles sont traces en points bruts, qui ne
      // l'appliquent pas. Les deux se retrouvaient decales de trois colonnes,
      // et le curseur d'ecriture tombait au milieu du mot.
      const int decalFenetre = g_colOrigine;
      g_colOrigine = 0;

      // La grille de caracteres, comme dans LSDJ : on s'y promene a la croix
      // et A pose la lettre. C'est bien plus lisible que de faire defiler un
      // alphabet sous un curseur, ou l'on ne voit jamais ce qui vient.
      // Les deux dernieres cases sont le retour arriere et la validation.
      static const char *grille[4] = { "0123456789",
                                       "ABCDEFGHIJ",
                                       "KLMNOPQRST",
                                       "UVWXYZ-_<*" };
#ifdef FM1
      // FM-1 : pas de fenetre a cadre (elle debordait de l'ecran, et ses
      // rangees de cadre s'agrandissaient avec la police) : la page entiere,
      // sur la grille de l'ecran. SAVE AS, le nom, puis les quatre rangees de
      // lettres, une case d'ecart (collees en ZOOM, pour tenir), le choix en
      // rouge.
      assombrirDemande = false;
      {
        for (int l = 1; l < kLignes; l++) efface(0, l, 41);
        titre(0, 1, "SAVE AS:", kTitre);
        fm1_rangee_espace(2);
        char n2[10]; int j = 0;
        for (; j < 8 && sauveNom[j]; j++) n2[j] = sauveNom[j];
        n2[j] = 0;
        char champ[16];
        siprintf(champ, "%-8s", n2);
        for (int k = 0; k < 8; k++) if (champ[k] == ' ') champ[k] = '_';
        texte(0, 3, champ, kData);
        titre(8, 3, saveEnA2m ? ".A2M" : ".OBT", kAttenue);
        fm1_rangee_espace(4);
        // L'ecart des lettres : deux cases sans zoom, une case et demie en
        // 4 COL (une sur deux decalee d'une demi-case : toute la largeur), une
        // en 3 COL. (OK, deux lettres, se colle au < pour tenir.)
        fm1_decales_efface();
        for (int r = 0; r < 4; r++)
          for (int c2 = 0; c2 < 10; c2++) {
            const bool ici = (nomLig == r && nomCol == c2);
            const bool estOK = (grille[r][c2] == '*');
            char c1[2] = { grille[r][c2], 0 };
            int col = zoomNiv == 2 ? c2 : zoomNiv ? (3 * c2) / 2 : 2 * c2;
            if (zoomNiv == 1 && (c2 & 1) && !estOK) fm1_decale_demi(5 + r * 2, col + g_colOrigine, 1);
            texte(col, 5 + r * 2, estOK ? "OK" : c1, ici ? kAccent : kData);
          }
        for (int r = 0; r < 3; r++) fm1_rangee_espace(6 + r * 2);   // (le meme petit espace entre les rangees)
      }
#else
      if (assombrirDemande) { assombrit(); assombrirDemande = false; }

      // La position se DEDUIT de la hauteur de l'ecran : elle etait ecrite en
      // dur pour 21 lignes, et l'agrandissement de la police l'a ramenee a 19
      // — la rangee du OK tombait alors hors du cadre. On cale la fenetre sur
      // le bas, une ligne de marge, quelle que soit la taille de police.
      const int fh = 12;
      const int fl = (kLignes - fh - 1) > 1 ? (kLignes - fh - 1) : 1;
      const int fc = 4, fw = 32;
      fenetre(fc, fl, fw, fh);
      // La grille.
      for (int r = 0; r < 4; r++)
        for (int c2 = 0; c2 < 10; c2++) {
          const int cx = fc + 2 + c2 * 3, cy = fl + 5 + r * 2;
          const bool ici = (nomLig == r && nomCol == c2);
          // La derniere case porte OK sur deux caracteres, comme dans LSDJ.
          // Elle est un peu plus large, d'ou le pave elargi quand elle est
          // pointee.
          const bool estOK = (grille[r][c2] == '*');
          if (ici) {
            const int x0 = cx * MD_CELL_W - 1, y0 = cy * MD_CELL_H - 1;
            const int lg = (estOK ? 2 : 1) * MD_CELL_W + 2;
            for (int y = y0; y < y0 + MD_GLYPHE_H + 3; y++)
              for (int x = x0; x < x0 + lg; x++) pixel(x, y, kAccent);
          }
          char c1[2] = { grille[r][c2], 0 };
          texte(cx, cy, estOK ? "OK" : c1, ici ? kFond : kData);
        }

      titre(fc + 2, fl + 1, "SAVE AS:", kTitre);

      // Le champ du nom, sur fond plein, avec le curseur d'ecriture.
      { const int x0 = (fc + 2) * MD_CELL_W - 1, y0 = (fl + 3) * MD_CELL_H - 1;
        const int x1 = x0 + 14 * MD_CELL_W, y1 = y0 + MD_GLYPHE_H + 3;
        for (int y = y0; y < y1; y++)
          for (int x = x0; x < x1; x++) pixel(x, y, kAttenue); }
      { char n2[10]; int j = 0;
        for (; j < 8 && sauveNom[j]; j++) n2[j] = sauveNom[j];
        n2[j] = 0;
        texte(fc + 2, fl + 3, n2, kFond);
        // Le pave d'ecriture, la ou la prochaine lettre ira.
        const int cx = (fc + 2 + j) * MD_CELL_W;
        const int cy = (fl + 3) * MD_CELL_H;
        for (int y = cy; y < cy + MD_GLYPHE_H; y++)
          for (int x = cx; x < cx + MD_GLYPHE_W; x++) pixel(x, y, kFond);
        titre(fc + 17, fl + 3, ".A2M", kAttenue); }
#endif
      g_colOrigine = decalFenetre;
    }

    // ── Le message passager ─────────────────────────────────────────────
    // Il etait pose sur la LIGNE 0, a partir de la colonne 12 : le nom du
    // fichier exporte passait donc par-dessus le titre de la page, ecrit a
    // droite de cette meme ligne. Il vit maintenant une ligne plus bas, et
    // centre : il ne recouvre plus rien et se lit d'un coup d'oeil.
    {
      static char vuMsg[40] = "";
      char aVoir[40] = "";
      if (msgProjet[0] && secondes < msgProjetJusqu) strcpy(aVoir, msgProjet);
      else msgProjet[0] = 0;
      if (strcmp(aVoir, vuMsg)) {
        strcpy(vuMsg, aVoir);
        const int sauve = g_colOrigine, sauveY = g_decalYpx;
        g_colOrigine = 0; g_decalYpx = 0;
#ifdef FM1
        // FM-1 : hors de PROJECT, la rangee 1 est celle des titres de colonnes
        // (le message s'y eparpillait, mot par mot) : il prend la place de
        // l'en-tete, le nom de la page et les titres qui y sont, le temps de
        // se lire ; a la largeur de l'en-tete.
        // (PROJECT aussi : sa rangee 1 s'affiche tout en bas, coupee : des traits bizarres apres SAVE)
        if (aVoir[0]) {
          fm1_nom_page("");
          fm1_mots_entete_efface();
          efface(0, 0, kCols);
          aVoir[fm1_entete_cellules() < 39 ? fm1_entete_cellules() : 39] = 0;
          texte(0, 0, aVoir, kEntete);
        } else
#endif
        efface(0, 1, kCols);
#ifdef FM1
        if (aVoir[0]) {
          // (deja dans l'en-tete)
        } else
#endif
        if (aVoir[0]) {
          // Centre : la police des titres est plus large, on compte en cellules.
#ifdef FM1
          // FM-1 : cale a gauche et coupe a la largeur de l'ecran (12 cases en
          // ZOOM, 21 sinon), PAS en rouge : la vue suit le rouge (le curseur),
          // et partait vers la droite derriere le message.
          aVoir[zoomNiv == 2 ? 12 : zoomNiv ? 15 : 21] = 0;
          titre(0, 1, aVoir, kEntete);
#else
          const int lg = (int)strlen(aVoir);
          const int c0 = (kCols - lg) / 2;
          titre(c0 > 0 ? c0 : 0, 1, aVoir, kAccent);
#endif
        } else {
          // Les noms de colonnes sont a cheval sur les rangees 1 et 2 :
          // effacer la rangee du message en emportait la moitie haute. Le
          // message parti, on redessine la page entiere.
          pageVue = -1;
        }
        g_colOrigine = sauve; g_decalYpx = sauveY;
      }
    }

    // ── Une fenetre vient de se fermer ? ─────────────────────────────────
    // Le A (ou le B) qui l'a fermee est sans doute encore enfonce : sans ca,
    // sa repetition automatique arrivait sur la page d'en dessous et y posait
    // une valeur — un chain neuf en ligne 00 de SONG apres OPEN.
    // SYNC TEST : deux fois par seconde, un clic OPL3 et un clic sample.
    if (syncTest && !enLecture) {
      static unsigned short dernierClic = 0;
      const unsigned short t = (unsigned short)timerTick(2);
      if ((unsigned short)(t - dernierClic) > 16000) {
        dernierClic = t;
        rg_test_synchro(md_replayer_get_sync_ms());
      }
    }
#ifdef FM1
    // ── Les TRAITS entre les colonnes (SONG, CHAIN, PHRASE, TABLE) ─────────
    // Droits, chaque espace sa couleur, traces a l'afficheur au milieu exact
    // de l'espace noir entre deux colonnes (fm1_traits), et seulement sur le
    // fond : les chevrons de lecture et le curseur passent par-dessus. Ici,
    // les bords de chaque espace dans l'image : le point apres le dernier
    // chiffre d'une colonne, le point avant le premier de la suivante.
    {
      // (theme aubergine : six couleurs, jamais deux fois la meme cote a cote — violet, rose, vert, bleu,
      // peche, turquoise)
      static const u16 kTrait[6] = { rvb(22, 17, 31), rvb(31, 13, 23), rvb(6, 28, 15), rvb(9, 19, 31),
                                     rvb(31, 20, 14), rvb(13, 28, 28) };
      int16_t tg[8], td[8];
      u16 tc[8];
      int n = 0;
      const int W = MD_CELL_W, g = g_colOrigine;
      // l'espace entre la colonne (debut, largeur) et la colonne qui commence a suivant
      auto espace = [&](int debut, int larg, int suivant) {
        if (n >= 8) return;
        tg[n] = (int16_t)((debut + larg - 1 + g) * W + MD_GLYPHE_W);
        td[n] = (int16_t)((suivant + g) * W - 1);
        tc[n] = kTrait[n % 6];
        n++;
      };
      if (!navigateur && !dialogueNom && !dialogueMidi && !demandeNouveau) {
        if (page == PAGE_SONG) {
          espace(0, 2, kColVoie);                          // les numeros de ligne, puis la premiere piste
          for (int k = 0; k + 1 < nbVoiesVues; k++)
            espace(kColVoie + k * kPasVoie, 2, kColVoie + (k + 1) * kPasVoie);
        } else if (page == PAGE_CHAIN) {
          espace(0, 2, 3);                                 // numeros, PHR, TSP
          espace(3, 2, 6);
        } else if (page == PAGE_PHRASE) {
          int avant = -1;
          for (int k = 0; k < 5; k++) {
            if (!phVisible(k)) continue;
            const int deb = kPhGrpDeb[k] - phDecal(phGauche);
            if (avant < 0) espace(0, 2, deb);
            else espace(kPhGrpDeb[avant] - phDecal(phGauche), kPhGrpLarg[avant], deb);
            avant = k;
          }
        } else if (page == PAGE_TABLE) {
          static const int kDeb[6] = { 0, 4, 8, 12, 17, 22 }, kLarg[6] = { 2, 2, 2, 3, 3, 4 };
          for (int k = 0; k + 1 < 6; k++) espace(kDeb[k], kLarg[k], kDeb[k + 1]);
        }
      }
      fm1_traits(n, tg, td, tc, 3 * MD_CELL_H, (3 + kLignesVues) * MD_CELL_H - 1);
    }
#endif
    { static bool fenetreAvant = false;
      const bool fenetre = navigateur || dialogueNom || dialogueMidi || demandeNouveau;
      if (fenetreAvant && !fenetre) keysIgnoreUntilReleased(KEY_A | KEY_B);
      fenetreAvant = fenetre; }
    swiWaitForVBlank();
  }
  return 0;
}
