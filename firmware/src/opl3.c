/* SPDX-License-Identifier: GPL-3.0-only */
/* An OPL3 (YMF262) for the tracker, driven by register writes exactly as the RetroWave board was
 * (opl3_ecrit(port, reg, val)): 18 two-operator channels, the eight waveforms, feedback, FM / additive
 * connection, the chip's envelope rates (KSR, the attack curve), sustain / percussive envelopes (EGT), key
 * scale level (the chip's KSL table), tremolo and vibrato with the BD depths, left / right outputs.
 * Each operator works as the chip does: a 10-bit phase into the log-sine ROM, the attenuation added in the
 * log domain, the exponent ROM to a 13-bit output (TK_LOGSIN / TK_EXP: tools/gen_tables.py).
 * Not modelled: 4-operator channels (0x104), the rhythm mode, the chip's 49.7 kHz rate (FS here).
 * The envelopes and the LFOs step every ENV_DIV samples; the phases every sample. */

#define OPL3_NV 18u
#define ENV_DIV 4u
#define ATT_MAX (511 << 16)
enum { EG_ATT, EG_DEC, EG_SUS, EG_REL, EG_OFF };

static const uint8_t MULTX2[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
static const uint8_t OP_OFF[9] = {0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12};
static const uint8_t KSL_ROM[16] = {0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64};
static const uint8_t KSL_SHIFT[4] = {8, 1, 2, 0};

typedef struct {
    uint32_t phase;
    int32_t att;                  /* envelope, Q16 units of 0.1875 dB (0 loudest, 511 off) */
    uint32_t tot;                 /* this step's total attenuation << 3 (log-sine units) */
    uint32_t inc;
    uint8_t stage;
} op3_t;

typedef struct {
    op3_t op[2];
    int32_t fb1, fb2;
    uint8_t on;                   /* key on (B0 bit 5) as last seen */
    uint32_t bcnt;                /* CIRCUIT BENDING: the glitch's sample count */
    uint32_t rph;                 /* ... the ring modulator's phase */
    int32_t bgain;                /* ... the level kept as the unbent note's (Q8, 0 = 256) */
    uint32_t t_on;                /* ... opl3_t at the last key on (a glide starts there) */
    uint32_t bruit;               /* ... the noise's shift register */
    uint32_t mes_b, mes_v, mes_n; /* ... the level measure, over 64 samples (32 bits) */
    uint8_t fort;                 /* x2 (+6 dB): a drum of DRUM KEYBOARD on this voice (fm1_voie_forte), this note */
    uint32_t bk_cle;              /* ... the wiring drawn for (fnum, block, switches), kept: drawn again only when */
    uint32_t bk_ratio, bk_ring, bk_arplen, bk_stut;   /*     the note or the switches change, not every 4 samples */
    int32_t bk_fold;
    uint8_t bk_fuzz, bk_xor, bk_crush, bk_arp, bk_glisse, bk_wobble, bk_bruit, bk_blk;
} ch3_t;

static uint8_t opl3_reg[2][256];
static uint8_t opl3_brut[2][256];                 /* the values as written (CIRCUIT BENDING: opl3_plie) */
static uint8_t opl3_bend_bits;
static uint32_t opl3_bend_pistes[7] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                       0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};   /* ... each switch's tracks */
static uint8_t opl3_bend_voie[18];                 /* ... the switches each voice (= track) gets */
/* CIRCUIT BENDING 2: PITCH (K1, K2) every voice's pitch only (x Q16, fm1_horloge); CLOCK PITCH (K3) the chip's
   clock, as if an LTC1799 drove it: the pitch AND the speed, like a tape played slower (fm1_horloge_vitesse :
   the envelopes, the LFOs, and outside the chip the sequencer, fm1_sequenceur). Each slides to where the knobs
   set it, a little every 4 samples: it glides, never steps. */
static volatile uint32_t opl3_horloge_cible = 65536u, opl3_vitesse_cible = 65536u;
static uint32_t opl3_horloge = 65536u, opl3_vitesse = 65536u, opl3_env_reste;
static inline uint32_t borne_horloge(uint32_t q16) { return q16 < 8192u ? 8192u : q16 > 524288u ? 524288u : q16; }
void fm1_horloge(uint32_t q16) { opl3_horloge_cible = borne_horloge(q16); }
void fm1_horloge_vitesse(uint32_t q16) { opl3_vitesse_cible = borne_horloge(q16); }
uint32_t fm1_vitesse(void) { return opl3_vitesse; }
static inline uint32_t glisse(uint32_t v, uint32_t cible)   /* (~25 ms) */
{
    const int32_t ecart = (int32_t)(cible - v);
    return (uint32_t)((int32_t)v + (ecart > 255 || ecart < -255 ? ecart / 256 : ecart > 0 ? 1 : ecart < 0 ? -1 : 0));
}
static ch3_t opl3_ch[OPL3_NV];
static int32_t opl3_dernier[OPL3_NV];             /* CIRCUIT BENDING: each voice's last sample (the crosstalk) */
static uint32_t opl3_t;                           /* ... the samples rendered (the arpeggios' clock) */
/* CIRCUIT BENDING: each switch wires each note to one behaviour, drawn from its own palette by the note's
   pitch (the same note, always the same; two neighbours, often not). Several switches add up. */
enum { B_RIEN, B_FAUX, B_FUZZ, B_RING, B_ARP, B_OCT, B_SOUS, B_STUT, B_CRUSH, B_XOR, B_FOLD,
       B_GLISSE, B_WOBBLE, B_BRUIT };
#define NB_BEND 7
static const uint8_t B_PALETTE[NB_BEND][8] = {
    {B_RIEN, B_FAUX, B_FAUX, B_FUZZ, B_ARP, B_FAUX, B_RING, B_RIEN},    /* 1 DETUNE: wrong notes, some fuzzed */
    {B_RIEN, B_STUT, B_OCT, B_ARP, B_STUT, B_SOUS, B_ARP, B_OCT},       /* 2 GLITCH: octaves, restarts, arpeggios */
    {B_RIEN, B_RING, B_XOR, B_CRUSH, B_RING, B_CRUSH, B_FUZZ, B_XOR},   /* 3 CROSS: ring, crosstalk, crushed bits */
    {B_FOLD, B_FOLD, B_FUZZ, B_SOUS, B_FOLD, B_RING, B_FOLD, B_ARP},    /* 4 DRONE: folded, fuzzed, sub */
    {B_RIEN, B_GLISSE, B_GLISSE, B_WOBBLE, B_GLISSE, B_OCT, B_WOBBLE, B_RIEN},   /* 5 GLIDE: notes sliding in, wobbling */
    {B_RIEN, B_BRUIT, B_CRUSH, B_BRUIT, B_RING, B_BRUIT, B_FUZZ, B_RIEN},       /* 6 NOISE: notes turned to noise */
    {B_ARP, B_STUT, B_BRUIT, B_GLISSE, B_RING, B_FOLD, B_FAUX, B_XOR},          /* 7 CHAOS: a bit of everything */
};
/* semitones -5 .. +5 as frequency ratios, Q12 (a wrong note) */
static const uint16_t B_DEMITON[11] = {3069, 3251, 3444, 3649, 3866, 4096, 4340, 4598, 4871, 5161, 5468};
/* the arpeggios: three steps, Q12 ratios (root, third or fourth or fifth, octave...) */
static const uint16_t B_ARPEGE[4][3] = {{4096, 5161, 6137}, {4096, 6137, 8192}, {4096, 8192, 16384}, {4096, 5468, 7298}};
static uint32_t opl3_am_ph, opl3_vib_ph;
#define LFO_AM_INC ((uint32_t)(3.7 * 4294967296.0 * ENV_DIV / FS))
#define LFO_VIB_INC ((uint32_t)(6.1 * 4294967296.0 * ENV_DIV / FS))

static inline int32_t op3_wave(uint32_t ws, uint32_t ph, uint32_t tot)
{
    uint32_t lvl, neg = 0;
    int32_t v;
    ph &= 1023u;
    switch (ws) {
    case 0:
        neg = ph & 0x200u;
        lvl = TK_LOGSIN[ph & 0x100u ? (ph & 0xFFu) ^ 0xFFu : ph & 0xFFu];
        break;
    case 1:
        if (ph & 0x200u)
            return 0;
        lvl = TK_LOGSIN[ph & 0x100u ? (ph & 0xFFu) ^ 0xFFu : ph & 0xFFu];
        break;
    case 2:
        lvl = TK_LOGSIN[ph & 0x100u ? (ph & 0xFFu) ^ 0xFFu : ph & 0xFFu];
        break;
    case 3:
        if (ph & 0x100u)
            return 0;
        lvl = TK_LOGSIN[ph & 0xFFu];
        break;
    case 4:
        if (ph & 0x200u)
            return 0;
        neg = (ph & 0x300u) == 0x100u;
        lvl = TK_LOGSIN[ph & 0x80u ? ((ph ^ 0xFFu) << 1) & 0xFFu : (ph << 1) & 0xFFu];
        break;
    case 5:
        if (ph & 0x200u)
            return 0;
        lvl = TK_LOGSIN[ph & 0x80u ? ((ph ^ 0xFFu) << 1) & 0xFFu : (ph << 1) & 0xFFu];
        break;
    case 6:
        neg = ph & 0x200u;
        lvl = 0;
        break;
    default:
        neg = ph & 0x200u;
        ph = neg ? (ph & 0x1FFu) ^ 0x1FFu : ph & 0x1FFu;
        lvl = ph << 3;
        break;
    }
    lvl += tot;
    if (lvl > 0x1FFFu)
        return 0;
    v = (int32_t)((((uint32_t)TK_EXP[(lvl & 0xFFu) ^ 0xFFu] | 0x400u) << 1) >> (lvl >> 8));
    return neg ? -v : v;
}

static void opl3_reset(void)
{
    uint32_t c, k;
    memset(opl3_reg, 0, sizeof opl3_reg);
    memset(opl3_brut, 0, sizeof opl3_brut);
    for (c = 0; c < OPL3_NV; c++) {
        opl3_ch[c].on = 0;
        for (k = 0; k < 2u; k++) {
            opl3_ch[c].op[k].att = ATT_MAX;
            opl3_ch[c].op[k].stage = EG_OFF;
        }
    }
}

/* Key on / off (B0 bit 5), taken at the write: the tracker's « off then on » for each note starts the
   note again, as the RetroWave and the RG350 play it (their writes are apart in time). */
/* the next key on of these voices is a DRUM KEYBOARD drum: louder (the melody covered them); any other note on
   the voice (the song's) plays at its level */
static volatile uint32_t opl3_fort_arme;
void fm1_voie_forte(int voie) { if (voie >= 0 && voie < 18) opl3_fort_arme |= 1u << voie; }
static void opl3_touche(ch3_t *h, uint32_t b0)
{
    const uint8_t on = (uint8_t)((b0 >> 5) & 1u);
    uint32_t k;
    if (on && !h->on) {
        const uint32_t bit = 1u << (uint32_t)(h - opl3_ch);
        h->fort = (opl3_fort_arme & bit) != 0u;
        opl3_fort_arme &= ~bit;
    }
    if (on && !h->on)
        h->t_on = opl3_t;                          /* (CIRCUIT BENDING: a glide starts here) */
    if (on && !h->on)
        for (k = 0; k < 2u; k++) {                 /* key on: the phase from 0, the attack from where it is */
            h->op[k].phase = 0;
            h->op[k].stage = EG_ATT;
        }
    else if (!on && h->on)
        for (k = 0; k < 2u; k++)
            if (h->op[k].stage != EG_OFF)
                h->op[k].stage = EG_REL;
    h->on = on;
}

/* CIRCUIT BENDING (the tracker's FX page): four switches, like wires soldered across a PSS's FM chip.
   Each note gets its own « wiring » from its pitch (opl3_rend: the same note, always the same way; two
   neighbours, not at all):
     1 DETUNE  two notes in three play wrong (up to ~4 semitones off), about half of them fuzzed (hard clip)
     2 GLITCH  some notes jump one or two octaves up, others restart over and over (a buzz)
     3 CROSS   some voices' bits crossed with the next voice's (bus crosstalk), the others crushed to a few
               bits; and here, the modulators' MULT bits 0-1 crossed, the carriers' tremolo stuck on
     4 DRONE   the sound folded over itself (each note its own fold), and here, feedback stuck at 7, the
               carriers' release almost infinite
   The written values are kept as they came (opl3_brut): a switch acts at once on what is sounding. */
static uint8_t opl3_plie(uint32_t port, uint32_t reg, uint32_t val)
{
    const uint32_t off = reg & 0x1Fu, mod = (off & 7u) < 3u;   /* (an operator register : modulator or carrier) */
    const uint32_t voie = port * 9u + (off >> 3) * 3u + (off & 7u) % 3u;
    if (reg >= 0x80u && reg <= 0x95u && off < 0x16u && (off & 7u) < 6u && !mod &&
        (opl3_bend_voie[voie] & 8u))              /* DRONE: the carriers' release almost infinite */
        val = (val & 0xF0u) | 0x01u;
    return (uint8_t)val;
}

/* port 0 / 1, register: the chip's write. Key on / off take effect here, the rest at the next render. */
static void opl3_ecrit(uint32_t port, uint32_t reg, uint32_t val)
{
    port &= 1u;
    reg &= 0xFFu;
    opl3_brut[port][reg] = (uint8_t)val;
    opl3_reg[port][reg] = opl3_plie(port, reg, val);
    if (reg >= 0xB0u && reg <= 0xB8u)
        opl3_touche(&opl3_ch[port * 9u + reg - 0xB0u], val);
}

/* Each voice's switches (those on, on its track); the registers already written are bent again (a key on / off stays as it is). */
static void opl3_replie(void)
{
    uint32_t p, r, c, k;
    for (c = 0; c < OPL3_NV; c++) {
        uint32_t b = 0;
        for (k = 0; k < 7u; k++)
            if (((opl3_bend_bits >> k) & 1u) && ((opl3_bend_pistes[k] >> c) & 1u))
                b |= 1u << k;
        opl3_bend_voie[c] = (uint8_t)b;
    }
    for (p = 0; p < 2u; p++)
        for (r = 0x20u; r < 0x100u; r++)
            opl3_reg[p][r] = opl3_plie(p, r, opl3_brut[p][r]);
}

void fm1_bend(int bits)
{
    opl3_bend_bits = (uint8_t)(bits & 127);
    opl3_replie();
}

/* Switch `effet` only on the tracks whose bit is set (track n = voice n). */
void fm1_bend_pistes(int effet, uint32_t pistes)
{
    if (effet < 0 || effet >= 7)
        return;
    opl3_bend_pistes[effet] = pistes;
    opl3_replie();
}

static uint32_t eff_rate(uint32_t r, uint32_t ksr, uint32_t ksv)
{
    uint32_t e;
    if (!r)
        return 0;
    e = 4u * r + (ksr ? ksv : ksv >> 2);
    return e > 63u ? 63u : e;
}

static inline uint32_t tri(uint32_t ph, uint32_t top)
{
    uint32_t x = ph >> 16;
    x = x < 32768u ? x : 65535u - x;
    return (x * top) >> 15;
}

/* n samples (a multiple of ENV_DIV) of the 18 channels, added into out (interleaved L R) */
static void opl3_rend(int32_t *out, uint32_t n)
{
    uint32_t s0, c, k, i;
    const uint8_t bd = opl3_reg[0][0xBD];
    for (s0 = 0; s0 < n; s0 += ENV_DIV) {
        uint32_t am;
        int32_t vib;
        uint32_t env_pas, hz;                      /* (the envelope steps this block: the clock's speed) */
        opl3_t += ENV_DIV;
        opl3_horloge = glisse(opl3_horloge, opl3_horloge_cible);
        opl3_vitesse = glisse(opl3_vitesse, opl3_vitesse_cible);
        hz = (uint32_t)(((uint64_t)opl3_horloge * opl3_vitesse) >> 16);   /* pitch: both */
        opl3_env_reste += opl3_vitesse;
        env_pas = opl3_env_reste >> 16;
        opl3_env_reste &= 0xFFFFu;
        if (opl3_vitesse == 65536u) {
            opl3_am_ph += LFO_AM_INC;
            opl3_vib_ph += LFO_VIB_INC;
        } else {
            opl3_am_ph += (uint32_t)(((uint64_t)LFO_AM_INC * opl3_vitesse) >> 16);
            opl3_vib_ph += (uint32_t)(((uint64_t)LFO_VIB_INC * opl3_vitesse) >> 16);
        }
        am = tri(opl3_am_ph, bd & 0x80u ? 26u : 6u);
        vib = (int32_t)tri(opl3_vib_ph + 0x40000000u, 65535u) - 32768;   /* -32768 .. 32767 */
        if (!(bd & 0x40u))
            vib >>= 1;
        for (c = 0; c < OPL3_NV; c++) {
            ch3_t *h = &opl3_ch[c];
            const uint8_t *r = opl3_reg[c / 9u];
            const uint32_t bnd = opl3_bend_voie[c];
            uint32_t somme_b, somme_v;
            uint32_t fuzz = 0, stut = 0, xor_ = 0, crush = 0, ring = 0, arp = 0, arplen = 1, ratio = 4096;
            uint32_t glisse = 0, wobble = 0, bruit = 0;
            int32_t fold = 0;
            uint32_t cc = c % 9u, c0 = r[0xC0u + cc], fnum, blk, ksv, fb, add;
            uint32_t ws0, ws1, ml, mr, base;
            op3_t *m = &h->op[0], *cr = &h->op[1];
            int32_t *o = out + 2u * s0;
            if (m->stage == EG_OFF && cr->stage == EG_OFF)
                continue;
            fnum = r[0xA0u + cc] | ((uint32_t)(r[0xB0u + cc] & 3u) << 8);
            blk = (r[0xB0u + cc] >> 2) & 7u;
            if (bnd && h->bk_cle == (fnum | blk << 10 | bnd << 13 | 1u << 31)) {   /* (the same as the last block) */
                ratio = h->bk_ratio; ring = h->bk_ring; arplen = h->bk_arplen; stut = h->bk_stut; fold = h->bk_fold;
                fuzz = h->bk_fuzz; xor_ = h->bk_xor; crush = h->bk_crush; arp = h->bk_arp; glisse = h->bk_glisse;
                wobble = h->bk_wobble; bruit = h->bk_bruit; blk = h->bk_blk;
            } else if (bnd) {                      /* CIRCUIT BENDING: this note's wiring, switch by switch */
                const uint32_t cle = fnum | blk << 10 | bnd << 13 | 1u << 31;
                const uint32_t hn = (fnum * 2654435761u) ^ (blk * 40503u);
                uint32_t sw;
                for (sw = 0; sw < NB_BEND; sw++) {
                    uint32_t e, g;
                    if (!((bnd >> sw) & 1u))
                        continue;
                    g = ((hn >> (sw * 4u + 3u)) + sw * 0x9E37u) * 2246822519u >> 24;   /* this switch's draw for this note */
                    e = B_PALETTE[sw][g & 7u];
                    switch (e) {
                    case B_FAUX:  ratio = (ratio * B_DEMITON[g % 11u == 5u ? 9u : g % 11u]) >> 12; break;
                    case B_FUZZ:  fuzz = 1; break;
                    case B_RING:  ring = (150u + (g >> 3) * 23u) * 97352u; break;   /* (Hz x 2^32 / FS) */
                    case B_ARP:   arp = 1u + (g & 3u); arplen = (2600u + (g >> 2 & 3u) * 900u); break;
                    case B_OCT:   blk = blk + 1u + (g & 1u) > 7u ? 7u : blk + 1u + (g & 1u); break;
                    case B_SOUS:  blk = blk ? blk - 1u : 0u; break;
                    case B_STUT:  stut = 300u + (g & 7u) * 170u; break;
                    case B_CRUSH: crush = 1; break;
                    case B_XOR:   xor_ = 1; break;
                    case B_FOLD:  fold = 0x400 + (int32_t)(g & 3u) * 0x180; break;
                    case B_GLISSE: glisse = 1u + (g & 1u); break;   /* from an octave below (1) or above (2) */
                    case B_WOBBLE: wobble = 1; break;
                    case B_BRUIT: bruit = 1; break;
                    default: break;
                    }
                }
                h->bk_cle = cle;
                h->bk_ratio = ratio; h->bk_ring = ring; h->bk_arplen = arplen; h->bk_stut = stut; h->bk_fold = fold;
                h->bk_fuzz = (uint8_t)fuzz; h->bk_xor = (uint8_t)xor_; h->bk_crush = (uint8_t)crush; h->bk_arp = (uint8_t)arp;
                h->bk_glisse = (uint8_t)glisse; h->bk_wobble = (uint8_t)wobble; h->bk_bruit = (uint8_t)bruit; h->bk_blk = (uint8_t)blk;
            }
            ksv = (blk << 1) | ((fnum >> 9) & 1u);
            fb = (c0 >> 1) & 7u;
            add = c0 & 1u;
            ml = 0u - web_bit(c0, 4);             /* (web_bit: core.h) */
            mr = 0u - web_bit(c0, 5);
            base = (fnum << blk) * 4616u;          /* (fnum << blk) * 49716 / 2^20 Hz -> 32-bit phase at FS */
            if (hz != 65536u)                      /* (CIRCUIT BENDING 2: the pitch, the clock) */
                base = (uint32_t)(((uint64_t)base * hz) >> 16);
            if (bnd) {                             /* (CIRCUIT BENDING: a wrong note, an arpeggio's step) */
                if (arp)
                    ratio = (ratio * B_ARPEGE[arp - 1u][(opl3_t / arplen) % 3u]) >> 12;
                if (glisse) {                      /* the first ~180 ms: from an octave off, to the note */
                    const uint32_t d = opl3_t - h->t_on, lg = 8000u;
                    if (d < lg)
                        ratio = (ratio * (glisse == 1u ? 2048u + 2048u * d / lg : 8192u - 4096u * d / lg)) >> 12;
                }
                if (wobble) {                      /* a deep, fast vibrato (~7 Hz, about a whole tone) */
                    const uint32_t ph = (opl3_t * 47u) & 0xFFFFu;
                    const uint32_t tr = ph < 0x8000u ? ph : 0xFFFFu - ph;   /* 0 .. 0x7FFF */
                    ratio = (ratio * (3650u + tr / 37u)) >> 12;
                }
                if (ratio != 4096u)
                    base = (uint32_t)(((uint64_t)base * ratio) >> 12);
            }
            for (k = 0; k < 2u; k++) {
                op3_t *p = &h->op[k];
                uint32_t off = OP_OFF[cc] + (k ? 3u : 0u);
                uint32_t r20 = r[0x20u + off], r40 = r[0x40u + off], r60 = r[0x60u + off], r80 = r[0x80u + off];
                uint32_t ksr = (r20 >> 4) & 1u, sl = r80 >> 4, ksl, t, inc;
                int32_t ksl_raw;
                for (uint32_t pe = 0; pe < env_pas; pe++)   /* (as many steps as the clock gives: CLOCK PITCH) */
                    switch (p->stage) {                /* the envelope, ENV_DIV samples */
                    case EG_ATT: {
                        uint32_t a = TK_ENV_A[eff_rate(r60 >> 4, ksr, ksv)];
                        if (a == 0xFFFFFFFFu)
                            p->att = 0;
                        else
                            p->att -= (int32_t)(((uint32_t)((p->att >> 16) + 1) * a) >> 4);
                        if (p->att <= 0) {
                            p->att = 0;
                            p->stage = EG_DEC;
                        }
                        break;
                    }
                    case EG_DEC: {
                        uint32_t lim = (sl == 15u ? 31u : sl) << 4;
                        p->att += (int32_t)TK_ENV_D[eff_rate(r60 & 15u, ksr, ksv)];
                        if (p->att >= (int32_t)(lim << 16)) {
                            p->att = (int32_t)(lim << 16);
                            p->stage = (r20 & 0x20u) ? EG_SUS : EG_REL;
                        }
                        break;
                    }
                    case EG_REL:
                        p->att += (int32_t)TK_ENV_D[eff_rate(r80 & 15u, ksr, ksv)];
                        if (p->att >= ATT_MAX) {
                            p->att = ATT_MAX;
                            p->stage = EG_OFF;
                        }
                        break;
                    default:
                        break;
                    }
                ksl_raw = ((int32_t)KSL_ROM[fnum >> 6] << 2) - ((8 - (int32_t)blk) << 5);
                ksl = ksl_raw > 0 ? (uint32_t)ksl_raw >> KSL_SHIFT[r40 >> 6] : 0u;
                t = (uint32_t)(p->att >> 16) + (r40 & 63u) * 4u + ksl + (am & (0u - web_bit(r20, 7)));
                p->tot = (t > 511u ? 511u : t) << 3;
                inc = base;
                if (web_bit(r20, 6))               /* vibrato: +-fnum/128 at the deepest */
                    inc += (uint32_t)(((int32_t)(inc >> 7) * vib) >> 15);
                p->inc = (inc >> 1) * MULTX2[r20 & 15u];
            }
            ws0 = r[0xE0u + OP_OFF[cc]] & 7u;
            ws1 = r[0xE0u + OP_OFF[cc] + 3u] & 7u;
            somme_b = somme_v = 0;
            if (bnd && !h->bgain)
                h->bgain = 256;
            for (i = 0; i < ENV_DIV; i++) {
                int32_t fbm = fb ? (h->fb1 + h->fb2) >> (9u - fb) : 0;
                int32_t a = op3_wave(ws0, (m->phase >> 22) + (uint32_t)fbm, m->tot), b, v;
                m->phase += m->inc;
                h->fb2 = h->fb1;
                h->fb1 = a;
                b = op3_wave(ws1, (cr->phase >> 22) + (add ? 0u : (uint32_t)a), cr->tot);
                cr->phase += cr->inc;
                v = add ? a + b : b;
                if (bnd) {                         /* CIRCUIT BENDING, on this sample */
                    const int32_t brut = v;
                    if (stut && ++h->bcnt >= stut) {
                        h->bcnt = 0;
                        m->phase = cr->phase = 0;
                    }
                    if (fuzz) {                    /* (x3, pas x8 : le son de l'instrument s'entend encore, ses reglages aussi) */
                        v *= 3;
                        if (v > 0xC00) v = 0xC00;
                        else if (v < -0xC00) v = -0xC00;
                    }
                    if (bruit) {                   /* noise, at the note's level, half mixed with the note itself */
                        int32_t n;
                        h->bruit = h->bruit ? h->bruit : 0xACE1u;
                        h->bruit = (h->bruit >> 1) ^ (0u - (h->bruit & 1u) & 0xB400u);
                        n = (h->bruit & 1u) ? (v < 0 ? -v : v) : (v < 0 ? v : -v);
                        v = (v + n) / 2 + n / 4;
                    }
                    if (ring) {                    /* a square ring modulator, its own pitch per note */
                        h->rph += ring;
                        if (h->rph & 0x80000000u) v = -v;
                    }
                    if (xor_)
                        v ^= opl3_dernier[(c + 1u) % OPL3_NV] & 0x7C0;
                    if (crush)
                        v = (int32_t)((uint32_t)v & ~0x1FFu);
                    if (fold) {
                        if (v > fold) v = 2 * fold - v;
                        else if (v < -fold) v = -2 * fold - v;
                    }
                    opl3_dernier[c] = brut;
                    /* the level stays the unbent note's: measured over the block, corrected on the next */
                    /* (la puissance : moyenne des carres, en 32 bits ; un point fait au plus 2^13 : son carre
                       / 16, 2^22 ; 64 de suite, 2^28) */
                    somme_b += (uint32_t)(brut * brut) >> 4;
                    somme_v += (uint32_t)(v * v) >> 4;
                    v = (v * h->bgain) >> 8;               /* (32 bits : 2^13 x 2^10) */
                }
                if (h->fort)
                    v *= 2;
                o[0] += (int32_t)((uint32_t)v & ml);
                o[1] += (int32_t)((uint32_t)v & mr);
                o += 2;
            }
            /* CIRCUIT BENDING : le niveau, mesure sur 64 echantillons (pas a chaque bloc de 4 : une division
               64 bits et une racine par bloc et par voie ralentissaient tout, le son craquait) ; le gain du
               suivant, monte lentement, redescend vite (il ne s'emballe pas quand le son se coupe en boucle) */
            if (bnd) {
                h->mes_b += somme_b;
                h->mes_v += somme_v;
                h->mes_n += ENV_DIV;
                if (h->mes_n >= 64u) {
                    uint32_t b = h->mes_b, v2 = h->mes_v;
                    h->mes_b = h->mes_v = h->mes_n = 0;
                    while ((b | v2) >= (1u << 20)) { b >>= 1; v2 >>= 1; }
                    if (v2) {
                        const uint32_t q = (b << 12) / v2;  /* the power ratio, Q12 : the gain is its root (Q6) */
                        uint32_t rac = 0, bit;
                        int32_t cible;
                        for (bit = 1u << 15; bit; bit >>= 1)
                            if ((rac | bit) * (rac | bit) <= q)
                                rac |= bit;
                        cible = (int32_t)(rac << 2);        /* (Q6 -> Q8) */
                        if (cible < 32) cible = 32;
                        if (cible > 1024) cible = 1024;
                        h->bgain += cible > h->bgain ? (cible - h->bgain) / 8 : (cible - h->bgain) / 2;
                    }
                }
            }
        }
    }
}
