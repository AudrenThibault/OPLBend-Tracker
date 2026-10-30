// ============================================================================
//  Les voies de son du FM-1, vues depuis le moteur du tracker.
//  (Calque sur compat/rg_voix_opl.cpp de la version RG350.)
//
//    - opl_voix_*  : l'OPL3 ; les registres sont ecrits exactement comme sur la
//      RetroWave (et comme dans Amber OPL3 Tracker), mais vers l'OPL3 logiciel
//      du FM-1 (src/opl3.c) au lieu de l'USB ;
//    - ds_voix_*   : les echantillons de la DS. Sur le FM-1 ils sont MUETS pour
//      l'instant (pas de carte SD d'ou les charger) : seuls ds_voix_stop (la
//      relache de la voie OPL3) et ds_voix_envoie ont un effet ;
//    - xt_midi_*   : rien pour l'instant (le FM-1 a pourtant l'USB-MIDI : a
//      brancher plus tard, comme le MIDI de la DS) ;
//    - rg_son_*    : la sortie audio, toujours ouverte.
//  Le moteur appelle tout cela depuis le tick, dans l'interruption audio, ou
//  depuis la boucle principale sous md_lock : l'OPL3 ne voit donc jamais deux
//  ecritures a la fois.
// ============================================================================

static uint8_t voix_b0[OPL_NB_VOIES];             /* A0/B0 : bloc, F-Num haut, key on */
static uint8_t voix_r40[2][0x16];                 /* KSL / TL de chaque operateur, tels qu'ecrits */

static void ecrit(int port, uint8_t reg, uint8_t val) { opl3_ecrit((uint32_t)port, reg, val); }

static void voix_reinitialise(void)
{
    int p, r;
    opl3_reset();
    memset(voix_b0, 0, sizeof voix_b0);
    ecrit(1, 0x05, 0x01);                         /* 0x105 : mode OPL3, dix-huit voies */
    ecrit(0, 0x01, 0x20);                         /* WSE : formes d'onde */
    ecrit(1, 0x04, 0x00);                         /* pas de voies 4 operateurs */
    for (p = 0; p < 2; p++) {
        for (r = 0x40; r <= 0x55; r++) {
            ecrit(p, (uint8_t)r, 0x3F);
            voix_r40[p][r - 0x40] = 0x3F;
        }
        for (r = 0xB0; r <= 0xB8; r++)
            ecrit(p, (uint8_t)r, 0x00);
    }
}

// ── opl_voix ────────────────────────────────────────────────────────────────
void opl_voix_init(void) { voix_reinitialise(); }

void opl_voix_instrument(int voie, const uint8_t fm[11], int pan)
{
    int port, ch, op;
    uint8_t cote;
    if (voie < 0 || voie >= OPL_NB_VOIES)
        return;
    port = voie / 9;
    ch = voie % 9;
    op = OP_OFF[ch];
    ecrit(port, (uint8_t)(0x20 + op), fm[0]);
    ecrit(port, (uint8_t)(0x60 + op), fm[4]);
    ecrit(port, (uint8_t)(0x80 + op), fm[6]);
    ecrit(port, (uint8_t)(0xE0 + op), fm[8]);
    ecrit(port, (uint8_t)(0x20 + op + 3), fm[1]);
    ecrit(port, (uint8_t)(0x60 + op + 3), fm[5]);
    ecrit(port, (uint8_t)(0x80 + op + 3), fm[7]);
    ecrit(port, (uint8_t)(0xE0 + op + 3), fm[9]);
    // KSL des deux operateurs : le TL viendra avec opl_voix_volume.
    voix_r40[port][op] = (uint8_t)((voix_r40[port][op] & 0x3F) | (fm[2] & 0xC0));
    voix_r40[port][op + 3] = (uint8_t)((voix_r40[port][op + 3] & 0x3F) | (fm[3] & 0xC0));
    ecrit(port, (uint8_t)(0x40 + op), voix_r40[port][op]);
    ecrit(port, (uint8_t)(0x40 + op + 3), voix_r40[port][op + 3]);
    // Stereo : l'OPL3 n'a que gauche, droite ou les deux.
    cote = (uint8_t)(pan < 85 ? 0x10 : (pan > 170 ? 0x20 : 0x30));
    ecrit(port, (uint8_t)(0xC0 + ch), (uint8_t)(cote | (fm[10] & 0x0F)));
}

void opl_voix_volume(int voie, int tlPorteuse, int tlModulateur)
{
    int port, op;
    if (voie < 0 || voie >= OPL_NB_VOIES)
        return;
    port = voie / 9;
    op = OP_OFF[voie % 9];
    if (tlPorteuse > 63) tlPorteuse = 63;
    if (tlModulateur > 63) tlModulateur = 63;
    if (tlPorteuse < 0) tlPorteuse = 0;
    if (tlModulateur < 0) tlModulateur = 0;
    voix_r40[port][op + 3] = (uint8_t)((voix_r40[port][op + 3] & 0xC0) | tlPorteuse);
    voix_r40[port][op] = (uint8_t)((voix_r40[port][op] & 0xC0) | tlModulateur);
    ecrit(port, (uint8_t)(0x40 + op + 3), voix_r40[port][op + 3]);
    ecrit(port, (uint8_t)(0x40 + op), voix_r40[port][op]);
}

void opl_voix_hauteur(int voie, int hauteur, bool attaque)
{
    int port, ch, bloc, fnum;
    uint8_t b0;
    if (voie < 0 || voie >= OPL_NB_VOIES)
        return;
    port = voie / 9;
    ch = voie % 9;
    // La hauteur est deja celle de la puce : (bloc << 10) | F-Num.
    bloc = (hauteur >> 10) & 7;
    fnum = hauteur & 0x3FF;
    b0 = (uint8_t)((bloc << 2) | (fnum >> 8));
    ecrit(port, (uint8_t)(0xA0 + ch), (uint8_t)(fnum & 0xFF));
    if (attaque) {
        // Key off puis key on : l'enveloppe repart a coup sur.
        ecrit(port, (uint8_t)(0xB0 + ch), b0);
        voix_b0[voie] = (uint8_t)(b0 | 0x20);
        ecrit(port, (uint8_t)(0xB0 + ch), voix_b0[voie]);
    } else {
        voix_b0[voie] = (uint8_t)((voix_b0[voie] & 0x20) | b0);
        ecrit(port, (uint8_t)(0xB0 + ch), voix_b0[voie]);
    }
}

void opl_voix_relache(int voie)
{
    if (voie < 0 || voie >= OPL_NB_VOIES)
        return;
    voix_b0[voie] &= (uint8_t)~0x20;
    ecrit(voie / 9, (uint8_t)(0xB0 + voie % 9), voix_b0[voie]);
}

void opl_voix_profondeurs(int tremolo, int vibrato)
{
    uint8_t bd = opl3_reg[0][0xBD];
    if (tremolo >= 0) bd = (uint8_t)((bd & 0x7F) | (tremolo ? 0x80 : 0));
    if (vibrato >= 0) bd = (uint8_t)((bd & 0xBF) | (vibrato ? 0x40 : 0));
    ecrit(0, 0xBD, bd);
}

void opl_voix_purge(void) {}

// ── ds_voix : les echantillons (muets sur le FM-1 pour l'instant) ──────────
void ds_voix_init(void) {}
void ds_voix_joue(int voie, const uint8_t *donnees, uint32_t images, int bits, int32_t boucle, uint32_t hz,
                  int vol, int pan)
{
    (void)voie; (void)donnees; (void)images; (void)bits; (void)boucle; (void)hz; (void)vol; (void)pan;
}
void ds_voix_freq(int voie, uint32_t hz) { (void)voie; (void)hz; }
void ds_voix_disto(int voie, int disto) { (void)voie; (void)disto; }
void ds_voix_vol(int voie, int vol, int pan) { (void)voie; (void)vol; (void)pan; }
void ds_voix_stop(int voie) { opl_voix_relache(voie); }
bool ds_voix_joue_encore(int voie) { (void)voie; return false; }
void ds_voix_envoie(void) {}
bool ds_voix_lit_zone(const void *debut, uint32_t octets) { (void)debut; (void)octets; return false; }
void ds_voix_regle_retard(int ms) { (void)ms; }
void ds_voix_retard_actif(bool actif) { (void)actif; }
void ds_voix_purge(void) {}

// ── xt_midi : pas encore de MIDI ────────────────────────────────────────────
bool xt_midi_init(void) { return false; }
bool xt_midi_present(void) { return true; }       /* (la RG350 disait « la RetroWave repond ») */
bool xt_midi_connecte(void) { return false; }
uint32_t xt_midi_perdus(void) { return 0; }
void xt_midi_send(uint8_t a, uint8_t b, uint8_t c) { (void)a; (void)b; (void)c; }
void xt_midi_service(void) {}
void xt_midi_irq(void) {}

// ── rg_son : la sortie audio du FM-1 ────────────────────────────────────────
double rg_ecart_opl_max;
void rg_son_demarre(void) {}
const char *rg_son_sortie(void) { return "FM-1"; }
int rg_son_retard_opl(void) { return 0; }
bool rg_son_lecture(void) { return false; }       /* (pas de file d'attente : rien a dater) */
void rg_son_stop(int voie) { (void)voie; }
void rg_son_clic(int voie, int retardMs) { (void)voie; (void)retardMs; }

// Le test de synchro : un clic sur la derniere voie de l'OPL3 (il n'y a plus
// de samples ni de MIDI a caler dessus).
void rg_test_synchro(int syncMs)
{
    static const uint8_t kClic[11] = {0x01, 0x01, 0x10, 0x00, 0xF8, 0xF6, 0xFF, 0xFF, 0x00, 0x00, 0x00};
    const int a = enterCriticalSection();
    (void)syncMs;
    opl_voix_instrument(OPL_NB_VOIES - 1, kClic, 128);
    opl_voix_volume(OPL_NB_VOIES - 1, 0, 16);
    opl_voix_hauteur(OPL_NB_VOIES - 1, (5 << 10) | 0x157, true);
    leaveCriticalSection(a);
}
