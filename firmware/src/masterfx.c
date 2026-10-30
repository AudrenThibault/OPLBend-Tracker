/* SPDX-License-Identifier: GPL-3.0-only */
/* MASTER FX (FX, third press): seven effects on the whole mix (the tracker and the drum machine), each
 * with its amount, 0 (off) .. 15 (all of it, set hard; fm1_mfx_dose): DISTORTION, RING MOD, PHASER, FLANGER, DELAY (on the tempo: ping-pong in stereo,
 * one echo in mono), CRUSH, FILTER (a resonant low-pass sweeping, a wah). Included by audio.c after
 * glitch.c (gl_lg: the samples of a row), run in the audio ISR before the output stage. The delay's
 * line is on the heap only while the delay is on (fm1_mfx_tampon). */
#define MFX_N 16384u                              /* the delay line, mono 16-bit (0.37 s) */
#define MFX_FL 1024u                              /* the flanger's line, per channel */

static volatile uint8_t mfx_bits, mfx_dose[7];  /* (bits : les effets dont la dose n'est pas nulle) */
static int16_t *volatile mfx_buf;
static uint32_t mfx_ecrit;
static int16_t mfx_fl[2][MFX_FL];
static uint32_t mfx_fl_ecrit;
static uint32_t mfx_lfo_ring, mfx_lfo_ph, mfx_lfo_fl, mfx_lfo_flt, mfx_ring_ph;
static int32_t mfx_ap[2][4][2];                   /* the phaser: 4 all-pass stages, (last in, last out) */
static int32_t mfx_ph_fb[2];
static int32_t mfx_tenu[2];
static uint32_t mfx_cpt;
static int32_t mfx_lo[2], mfx_bd[2];              /* the filter (state variable) */

void fm1_mfx_dose(int k, int dose)
{
    uint32_t i, b = 0;
    if (k < 0 || k >= 7)
        return;
    mfx_dose[k] = (uint8_t)(dose < 0 ? 0 : dose > 15 ? 15 : dose);
    for (i = 0; i < 7u; i++)
        if (mfx_dose[i])
            b |= 1u << i;
    mfx_bits = (uint8_t)b;
}

/* (par echantillon, des decalages plutot que des divisions signees : / 3 ~ x 85 >> 8) */
static inline int32_t mfx_dose_mix(int32_t sec, int32_t mouille, uint32_t k)   /* sec -> mouille, dose k */
{
    /* (x 273 / 4096, soit / 15, en 32 bits : la division 64 bits, l'emulateur ne la lit pas) */
    return sec + (((mouille - sec) * (int32_t)mfx_dose[k]) * 273 >> 12);
}
void fm1_mfx_tampon(void *p)
{
    if (p)
        memset(p, 0, MFX_N * sizeof(int16_t));
    mfx_buf = (int16_t *)p;
}

static inline int32_t mfx_tri(uint32_t ph, int32_t haut)   /* a triangle, 0 .. haut */
{
    const uint32_t x = ph >> 16;
    return (int32_t)(((x < 32768u ? x : 65535u - x) * (uint32_t)haut) >> 15);
}

static inline int32_t mfx_borne(int32_t x)
{
    return x > 32767 ? 32767 : x < -32767 ? -32767 : x;
}

static void mfx_rend(int32_t *o, uint32_t n)
{
    const uint32_t b = mfx_bits;
    uint32_t i, c;
    if (!b)
        return;
    for (i = 0; i < n; i++) {
        int32_t x[2] = { o[2u * i], o[2u * i + 1u] };
        if (b & 1u)                               /* DISTORTION: x2 .. x9 with the amount, folded back past the ceiling */
            for (c = 0; c < 2u; c++) {
                int32_t y = x[c] * (2 + (int32_t)mfx_dose[0] / 2);
                if (y > 12000) y = 24000 - y;
                if (y < -12000) y = -24000 - y;
                x[c] = mfx_dose_mix(x[c], mfx_borne(y) * 3 >> 3, 0);   /* (le niveau : a peine plus fort que sans) */
            }
        if (b & 2u) {                             /* RING MOD: a triangle carrier, 80 .. 900 Hz, swept */
            const int32_t f = 80 + mfx_tri(mfx_lfo_ring, 820);   /* Hz */
            int32_t car;
            mfx_lfo_ring += 29204u;               /* (0.3 Hz) */
            mfx_ring_ph += (uint32_t)f * 97352u;  /* (Hz x 2^32 / FS) */
            car = mfx_tri(mfx_ring_ph, 65534) - 32767;
            for (c = 0; c < 2u; c++)
                x[c] = mfx_dose_mix(x[c], mfx_borne((int32_t)(((int64_t)x[c] * car) >> 14)), 1);   /* (x2 : la porteuse en triangle) */
        }
        if (b & 4u) {                             /* PHASER: 4 all-passes, the coefficient on a 0.4 Hz LFO */
            const int32_t a = 6000 + mfx_tri(mfx_lfo_ph, 22000);   /* Q15 */
            mfx_lfo_ph += 38938u;
            for (c = 0; c < 2u; c++) {
                int32_t v = x[c] + (mfx_ph_fb[c] * (int32_t)mfx_dose[2] >> 5), k;   /* (le retour, avec la dose) */
                for (k = 0; k < 4; k++) {         /* y = a (v - y1) + v1 */
                    const int32_t y = (int32_t)(((int64_t)a * (v - mfx_ap[c][k][1])) >> 15) + mfx_ap[c][k][0];
                    mfx_ap[c][k][0] = v;
                    mfx_ap[c][k][1] = y;
                    v = y;
                }
                mfx_ph_fb[c] = v;
                x[c] = mfx_dose_mix(x[c], mfx_borne((x[c] + v) * 3 >> 3), 2);
            }
        }
        if (b & 8u) {                             /* FLANGER: 1 .. 250 samples, on a 0.25 Hz LFO, much feedback */
            const uint32_t d = 2u + (uint32_t)mfx_tri(mfx_lfo_fl, 248);
            mfx_lfo_fl += 24336u;
            for (c = 0; c < 2u; c++) {
                const int32_t r = mfx_fl[c][(mfx_fl_ecrit - d) & (MFX_FL - 1u)];
                mfx_fl[c][mfx_fl_ecrit & (MFX_FL - 1u)] = (int16_t)mfx_borne(x[c] + (r * (int32_t)mfx_dose[3] * 273 >> 13));   /* (/ 30) */
                x[c] = mfx_dose_mix(x[c], mfx_borne((x[c] + r) * 9 >> 4), 3);   /* (le niveau garde : ni creux ni trop fort) */
            }
            mfx_fl_ecrit++;
        }
        if ((b & 16u) && mfx_buf) {               /* DELAY: a row (a 16th) ; stereo: ping-pong, the right at two */
            int16_t *buf = mfx_buf;
            uint32_t t = gl_lg;
            int32_t e1, e2;
            if (t > MFX_N / 2u - 1u)
                t = MFX_N / 2u - 1u;
            e1 = buf[(mfx_ecrit - t) & (MFX_N - 1u)];
            e2 = buf[(mfx_ecrit - 2u * t) & (MFX_N - 1u)];
            const int32_t fb = (int32_t)mfx_dose[4] * 3;   /* (le retour, /64 : jusqu'a 0,7 ; l'echo, avec la dose) */
            if (fm1_mono) {                       /* one echo, fed back */
                buf[mfx_ecrit & (MFX_N - 1u)] = (int16_t)mfx_borne(((x[0] + x[1]) >> 1) + (e1 * fb >> 6));
                x[0] = mfx_dose_mix(x[0], mfx_borne((x[0] * 2 + e1) * 85 >> 8), 4);
                x[1] = mfx_dose_mix(x[1], mfx_borne((x[1] * 2 + e1) * 85 >> 8), 4);
            } else {                              /* left at one row, right at two, fed back from the right */
                buf[mfx_ecrit & (MFX_N - 1u)] = (int16_t)mfx_borne(((x[0] + x[1]) >> 1) + (e2 * fb >> 6));
                x[0] = mfx_dose_mix(x[0], mfx_borne((x[0] * 2 + e1) * 85 >> 8), 4);
                x[1] = mfx_dose_mix(x[1], mfx_borne((x[1] * 2 + e2) * 85 >> 8), 4);
            }
            mfx_ecrit++;
        }
        if (b & 32u) {                            /* CRUSH: held 1 .. 8 samples, 11 .. 4 bits, with the amount */
            const uint32_t d = mfx_dose[5], tenue = 1u + d / 2u;
            const int32_t masque = ~((1 << (4 + d * 7u / 15u)) - 1);
            if (++mfx_cpt >= tenue)               /* (un compteur : pas de division par echantillon) */
                mfx_cpt = 0;
            if (mfx_cpt == 0u)
                for (c = 0; c < 2u; c++)
                    mfx_tenu[c] = (x[c] & masque) * 3 >> 2;
            x[0] = mfx_tenu[0];
            x[1] = mfx_tenu[1];
        }
        if (b & 64u) {                            /* FILTER: resonant low-pass, 1 Hz sweep (a wah) */
            const int32_t f = 16000 - mfx_tri(mfx_lfo_flt, 960 * (int32_t)mfx_dose[6]);   /* Q15 : ~2 sin(pi fc / FS) ; plus de dose, plus de balayage */
            mfx_lfo_flt += 97352u;
            for (c = 0; c < 2u; c++) {
                const int32_t hi = x[c] - mfx_lo[c] - (mfx_bd[c] * 9 >> 5);   /* (q: little damping) */
                mfx_bd[c] = mfx_borne(mfx_bd[c] + (int32_t)(((int64_t)f * hi) >> 15));
                mfx_lo[c] = mfx_borne(mfx_lo[c] + (int32_t)(((int64_t)f * mfx_bd[c]) >> 15));
                x[c] = mfx_dose_mix(x[c], mfx_lo[c], 6);
            }
        }
        o[2u * i] = x[0];
        o[2u * i + 1u] = x[1];
    }
}
