/* SPDX-License-Identifier: GPL-3.0-only */
/* The tracker's OPL3 played by Nuked OPL3 (firmware/nuked, LGPL-2.1-or-later, Nuke.YKT), a cycle-accurate YMF262,
 * with the circuit bending of opl3.c on top. Built after opl3.c (tracker.c), whose own functions are renamed *_fm1
 * there: the chip's writes go to both OPL3s, the render to the one chosen (opl3_nuked_on), so switching is instant.
 *   the chip runs at its own rate (49,716 Hz), resampled to FS, linear as Nuked's OPL3_GenerateResampled;
 *   CLOCK PITCH (opl3_vitesse) runs the chip faster or slower: the pitch, the envelopes, the LFOs, as a real clock;
 *   PITCH (opl3_horloge) and the bending's wrong notes, arpeggios, glides and octaves change each channel's F-number
 *   and block for the phase generator only (the envelopes keep the written note's key scaling);
 *   the switches' sample effects (fuzz, noise, ring, crosstalk, crush, fold, restarts) act on each channel's output,
 *   its level kept as the unbent note's, as opl3.c does; a DRUM KEYBOARD drum is x2 (opl3.c opl3_touche).
 * The writes reach the chip apart in time, as on a real one (Nuked's buffered writes, OPL_WRITEBUF_DELAY cycles
 * apart): the tracker's « key off, key on » that starts each note again needs that (opl3.c takes it at the write).
 * Each note's wiring is drawn as in opl3.c (the same note, always the same way). The chip always runs in OPL3 mode
 * (0x105 NEW = 1); four-operator channels (0x104) play as the chip plays them. Not done: the chip's one-sample delay
 * between some channels' outputs (OPL_QUIRK_CHANNELSAMPLEDELAY): all 36 operators, then the mix. */
#include "../nuked/opl3.c"

#define NK_RATE 49716u

typedef struct {
    uint16_t fnum;                /* this block's F-number and block for the phase (bent) */
    uint8_t blk, bent;
    uint8_t fuzz, xor_, crush, bruit;
    uint32_t ring, stut;
    int32_t fold;
    uint32_t bcnt, rph, sbruit;   /* the restarts' count, the ring's phase, the noise's shift register */
    int32_t bgain;                /* the level kept as the unbent note's (Q8, 0 = 256) */
    uint32_t mes_b, mes_v, mes_n;
    uint32_t bk_cle, bk_ratio, bk_ring, bk_arplen, bk_stut;   /* the wiring drawn for (fnum, block, switches) */
    int32_t bk_fold;
    uint8_t bk_fuzz, bk_xor, bk_crush, bk_arp, bk_glisse, bk_wobble, bk_bruit, bk_blk;
} nk_voie_t;

static opl3_chip nk_chip;
static nk_voie_t nk_voie[OPL3_NV];
static int32_t nk_dernier[OPL3_NV];               /* each channel's last unbent sample (the crosstalk) */
static int32_t nk_avant[2], nk_apres[2];          /* the two chip samples the output falls between */
static uint32_t nk_pas;                           /* where it falls, Q16 */
static volatile int opl3_nuked_on;                /* the render: 0 opl3.c, 1 Nuked (the plugin, fm1n_engine) */

static void nk_reset(void)
{
    OPL3_Reset(&nk_chip, FS);
    OPL3_WriteReg(&nk_chip, 0x105, 1);
    memset(nk_voie, 0, sizeof nk_voie);
    memset(nk_dernier, 0, sizeof nk_dernier);
    nk_avant[0] = nk_avant[1] = nk_apres[0] = nk_apres[1] = 0;
    nk_pas = 0;
}

static void nk_ecrit(uint32_t port, uint32_t reg, uint32_t val)
{
    port &= 1u;
    reg &= 0xFFu;
    (void)val;                                    /* (the value as opl3.c bent it: opl3_plie) */
    OPL3_WriteRegBuffered(&nk_chip, (uint16_t)(port << 8 | reg),
                          (uint8_t)(port == 1u && reg == 5u ? opl3_reg[1][5] | 1u : opl3_reg[port][reg]));
}

static void nk_replie(void)                       /* the switches changed: the bent registers again (DRONE's release) */
{
    uint32_t p, r;
    for (p = 0; p < 2u; p++)
        for (r = 0x80u; r < 0x96u; r++)
            OPL3_WriteRegBuffered(&nk_chip, (uint16_t)(p << 8 | r), opl3_reg[p][r]);
}

/* Each channel for the next ENV_DIV samples: its wiring (opl3.c's palette and draw), its pitch, its effects. */
static void nk_bloc(void)
{
    const uint8_t bd = opl3_reg[0][0xBD];
    uint32_t c;
    opl3_t += ENV_DIV;
    opl3_horloge = glisse(opl3_horloge, opl3_horloge_cible);
    opl3_vitesse = glisse(opl3_vitesse, opl3_vitesse_cible);
    for (c = 0; c < OPL3_NV; c++) {
        nk_voie_t *h = &nk_voie[c];
        const uint8_t *r = opl3_reg[c / 9u];
        const uint32_t cc = c % 9u, rythme = (bd & 0x20u) && c >= 6u && c <= 8u;
        const uint32_t bnd = rythme ? 0u : opl3_bend_voie[c];
        uint32_t fnum = r[0xA0u + cc] | ((uint32_t)(r[0xB0u + cc] & 3u) << 8), blk = (r[0xB0u + cc] >> 2) & 7u;
        uint32_t ratio = 4096, arp = 0, arplen = 1, glisse_ = 0, wobble = 0;
        h->fuzz = h->xor_ = h->crush = h->bruit = 0;
        h->ring = h->stut = 0;
        h->fold = 0;
        if (bnd && h->bk_cle == (fnum | blk << 10 | bnd << 13 | 1u << 31)) {
            ratio = h->bk_ratio; h->ring = h->bk_ring; arplen = h->bk_arplen; h->stut = h->bk_stut; h->fold = h->bk_fold;
            h->fuzz = h->bk_fuzz; h->xor_ = h->bk_xor; h->crush = h->bk_crush; arp = h->bk_arp; glisse_ = h->bk_glisse;
            wobble = h->bk_wobble; h->bruit = h->bk_bruit; blk = h->bk_blk;
        } else if (bnd) {
            const uint32_t hn = (fnum * 2654435761u) ^ (blk * 40503u);
            uint32_t sw;
            for (sw = 0; sw < NB_BEND; sw++) {
                uint32_t e, g;
                if (!((bnd >> sw) & 1u))
                    continue;
                g = ((hn >> (sw * 4u + 3u)) + sw * 0x9E37u) * 2246822519u >> 24;
                e = B_PALETTE[sw][g & 7u];
                switch (e) {
                case B_FAUX:  ratio = (ratio * B_DEMITON[g % 11u == 5u ? 9u : g % 11u]) >> 12; break;
                case B_FUZZ:  h->fuzz = 1; break;
                case B_RING:  h->ring = (150u + (g >> 3) * 23u) * (uint32_t)(4294967296.0 / NK_RATE); break;
                case B_ARP:   arp = 1u + (g & 3u); arplen = (2600u + (g >> 2 & 3u) * 900u); break;
                case B_OCT:   blk = blk + 1u + (g & 1u) > 7u ? 7u : blk + 1u + (g & 1u); break;
                case B_SOUS:  blk = blk ? blk - 1u : 0u; break;
                case B_STUT:  h->stut = (300u + (g & 7u) * 170u) * NK_RATE / FS; break;
                case B_CRUSH: h->crush = 1; break;
                case B_XOR:   h->xor_ = 1; break;
                case B_FOLD:  h->fold = 0x400 + (int32_t)(g & 3u) * 0x180; break;
                case B_GLISSE: glisse_ = 1u + (g & 1u); break;
                case B_WOBBLE: wobble = 1; break;
                case B_BRUIT: h->bruit = 1; break;
                default: break;
                }
            }
            h->bk_cle = fnum | blk << 10 | bnd << 13 | 1u << 31;
            h->bk_ratio = ratio; h->bk_ring = h->ring; h->bk_arplen = arplen; h->bk_stut = h->stut; h->bk_fold = h->fold;
            h->bk_fuzz = h->fuzz; h->bk_xor = h->xor_; h->bk_crush = h->crush; h->bk_arp = (uint8_t)arp;
            h->bk_glisse = (uint8_t)glisse_; h->bk_wobble = (uint8_t)wobble; h->bk_bruit = h->bruit; h->bk_blk = (uint8_t)blk;
        }
        if (bnd) {
            if (arp)
                ratio = (ratio * B_ARPEGE[arp - 1u][(opl3_t / arplen) % 3u]) >> 12;
            if (glisse_) {
                const uint32_t d = opl3_t - opl3_ch[c].t_on, lg = 8000u;
                if (d < lg)
                    ratio = (ratio * (glisse_ == 1u ? 2048u + 2048u * d / lg : 8192u - 4096u * d / lg)) >> 12;
            }
            if (wobble) {
                const uint32_t ph = (opl3_t * 47u) & 0xFFFFu, tr = ph < 0x8000u ? ph : 0xFFFFu - ph;
                ratio = (ratio * (3650u + tr / 37u)) >> 12;
            }
        }
        h->bent = (uint8_t)(ratio != 4096u || opl3_horloge != 65536u || blk != ((r[0xB0u + cc] >> 2) & 7u));
        if (h->bent) {                            /* the bent frequency, as an F-number and a block again */
            uint64_t f = ((uint64_t)fnum << blk) * ratio >> 12;
            uint32_t b = 0;
            f = f * opl3_horloge >> 16;
            while (b < 7u && (f >> b) > 1023u)
                b++;
            f >>= b;
            h->fnum = (uint16_t)(f > 1023u ? 1023u : f);
            h->blk = (uint8_t)b;
        }
    }
}

/* One cycle of the chip (opl3.c's OPL3_Generate4Ch, with each channel's bent pitch and its effects). */
static void nk_cycle(int32_t *lr)
{
    opl3_chip *chip = &nk_chip;
    uint16_t fn[OPL3_NV];
    uint8_t bl[OPL3_NV];
    int32_t mix[2] = {0, 0};
    uint8_t shift = 0;
    uint32_t c, ii;
    const uint32_t bd = opl3_reg[0][0xBD];
    for (c = 0; c < OPL3_NV; c++)
        if (nk_voie[c].bent) {
            fn[c] = chip->channel[c].f_num;
            bl[c] = chip->channel[c].block;
            chip->channel[c].f_num = nk_voie[c].fnum;
            chip->channel[c].block = nk_voie[c].blk;
        }
    for (ii = 0; ii < 36u; ii++)
        OPL3_ProcessSlot(&chip->slot[ii]);
    for (c = 0; c < OPL3_NV; c++)
        if (nk_voie[c].bent) {
            chip->channel[c].f_num = fn[c];
            chip->channel[c].block = bl[c];
        }

    for (c = 0; c < OPL3_NV; c++) {
        opl3_channel *ch = &chip->channel[c];
        nk_voie_t *h = &nk_voie[c];
        const uint32_t rythme = (bd & 0x20u) && c >= 6u && c <= 8u;
        int32_t v = (int16_t)(*ch->out[0] + *ch->out[1] + *ch->out[2] + *ch->out[3]);
        if (!rythme && opl3_bend_voie[c]) {       /* CIRCUIT BENDING, on this sample (opl3.c's) */
            const int32_t brut = v;
            if (!h->bgain)
                h->bgain = 256;
            if (h->stut && ++h->bcnt >= h->stut) {
                h->bcnt = 0;
                ch->slotz[0]->pg_phase = ch->slotz[1]->pg_phase = 0;
            }
            if (h->fuzz) {
                v *= 3;
                if (v > 0xC00) v = 0xC00;
                else if (v < -0xC00) v = -0xC00;
            }
            if (h->bruit) {
                int32_t n;
                h->sbruit = h->sbruit ? h->sbruit : 0xACE1u;
                h->sbruit = (h->sbruit >> 1) ^ (0u - (h->sbruit & 1u) & 0xB400u);
                n = (h->sbruit & 1u) ? (v < 0 ? -v : v) : (v < 0 ? v : -v);
                v = (v + n) / 2 + n / 4;
            }
            if (h->ring) {
                h->rph += h->ring;
                if (h->rph & 0x80000000u) v = -v;
            }
            if (h->xor_)
                v ^= nk_dernier[(c + 1u) % OPL3_NV] & 0x7C0;
            if (h->crush)
                v = (int32_t)((uint32_t)v & ~0x1FFu);
            if (h->fold) {
                if (v > h->fold) v = 2 * h->fold - v;
                else if (v < -h->fold) v = -2 * h->fold - v;
            }
            nk_dernier[c] = brut;
            h->mes_b += (uint32_t)(brut * brut) >> 4;
            h->mes_v += (uint32_t)(v * v) >> 4;
            if (++h->mes_n >= 64u) {              /* the level, over 64 samples: the gain of the next */
                uint32_t b = h->mes_b, v2 = h->mes_v;
                h->mes_b = h->mes_v = h->mes_n = 0;
                while ((b | v2) >= (1u << 20)) { b >>= 1; v2 >>= 1; }
                if (v2) {
                    const uint32_t q = (b << 12) / v2;
                    uint32_t rac = 0, bit;
                    int32_t cible;
                    for (bit = 1u << 15; bit; bit >>= 1)
                        if ((rac | bit) * (rac | bit) <= q)
                            rac |= bit;
                    cible = (int32_t)(rac << 2);
                    if (cible < 32) cible = 32;
                    if (cible > 1024) cible = 1024;
                    h->bgain += cible > h->bgain ? (cible - h->bgain) / 8 : (cible - h->bgain) / 2;
                }
            }
            v = (v * h->bgain) >> 8;
        } else
            nk_dernier[c] = v;
        if (opl3_ch[c].fort)
            v *= 2;
        if (ch->cha)
            mix[0] += v;
        if (ch->chb)
            mix[1] += v;
    }
    lr[0] = mix[0];
    lr[1] = mix[1];

    /* the chip's clocks and its buffered writes (OPL3_Generate4Ch, as is) */
    if ((chip->timer & 0x3f) == 0x3f)
        chip->tremolopos = (chip->tremolopos + 1) % 210;
    chip->tremolo = chip->tremolopos < 105 ? chip->tremolopos >> chip->tremoloshift
                                           : (210 - chip->tremolopos) >> chip->tremoloshift;
    if ((chip->timer & 0x3ff) == 0x3ff)
        chip->vibpos = (chip->vibpos + 1) & 7;
    chip->timer++;
    if (chip->eg_state) {
        while (shift < 13 && ((chip->eg_timer >> shift) & 1) == 0)
            shift++;
        chip->eg_add = shift > 12 ? 0 : shift + 1;
        chip->eg_timer_lo = (uint8_t)(chip->eg_timer & 0x3u);
    }
    if (chip->eg_timerrem || chip->eg_state) {
        if (chip->eg_timer == UINT64_C(0xfffffffff)) {
            chip->eg_timer = 0;
            chip->eg_timerrem = 1;
        } else {
            chip->eg_timer++;
            chip->eg_timerrem = 0;
        }
    }
    chip->eg_state ^= 1;
    for (;;) {
        opl3_writebuf *w = &chip->writebuf[chip->writebuf_cur];
        if (w->time > chip->writebuf_samplecnt || !(w->reg & 0x200))
            break;
        w->reg &= 0x1ff;
        OPL3_WriteReg(chip, w->reg, w->data);
        chip->writebuf_cur = (chip->writebuf_cur + 1) % OPL_WRITEBUF_SIZE;
    }
    chip->writebuf_samplecnt++;
}

/* n samples (a multiple of ENV_DIV) added into out (interleaved L R), as opl3.c's opl3_rend */
static void nk_rend(int32_t *out, uint32_t n)
{
    uint32_t s0, i;
    for (s0 = 0; s0 < n; s0 += ENV_DIV) {
        uint32_t step;
        nk_bloc();
        step = (uint32_t)(((uint64_t)NK_RATE << 16) / FS * opl3_vitesse >> 16);   /* chip cycles per sample, Q16 */
        for (i = 0; i < ENV_DIV; i++) {
            int32_t *o = out + 2u * (s0 + i);
            nk_pas += step;
            while (nk_pas >= 65536u) {
                nk_pas -= 65536u;
                nk_avant[0] = nk_apres[0];
                nk_avant[1] = nk_apres[1];
                nk_cycle(nk_apres);
            }
            o[0] += nk_avant[0] + (int32_t)(((int64_t)(nk_apres[0] - nk_avant[0]) * nk_pas) >> 16);
            o[1] += nk_avant[1] + (int32_t)(((int64_t)(nk_apres[1] - nk_avant[1]) * nk_pas) >> 16);
        }
    }
}

/* ------------------------------------------- the two OPL3s, as one (opl3.c's names) --- */
static void opl3_reset(void)
{
    opl3_reset_fm1();
    nk_reset();
}

static void opl3_ecrit(uint32_t port, uint32_t reg, uint32_t val)
{
    opl3_ecrit_fm1(port, reg, val);
    nk_ecrit(port, reg, val);
}

static void opl3_rend(int32_t *out, uint32_t n)
{
    if (opl3_nuked_on)
        nk_rend(out, n);
    else
        opl3_rend_fm1(out, n);
}

void fm1_bend(int bits)
{
    fm1_bend_fm1(bits);
    nk_replie();
}

void fm1_bend_pistes(int effet, uint32_t pistes)
{
    fm1_bend_pistes_fm1(effet, pistes);
    nk_replie();
}
