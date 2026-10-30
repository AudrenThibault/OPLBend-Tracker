/* SPDX-License-Identifier: GPL-3.0-only */
/* The output stage (audio.c; the host test builds it too): TONE, DRIVE, MASTER, then Q15 -> 24 bit. */
static int32_t lp1[2], lp2[2];                    /* TONE filter state, Q15 << 4 */
/* OUT (menu PROJECT): MONO, the same mix on both channels, for the built-in speaker (it plays the left
   channel only: a voice panned right was silent there); STEREO, each voice where its panning puts it,
   for headphones (the speaker mutes itself when they are plugged in; the firmware cannot tell). */
static uint8_t fm1_mono = 0;                      /* STEREO by default (the tracker sets it from its cfg) */
void fm1_sortie_mono(int on) { fm1_mono = (uint8_t)(on != 0); }

/* one-pole coefficient (Q15) for 0 OFF, SOFT 12 kHz, WARM 6 kHz, DARK 3 kHz: 1 - exp(-2 pi fc / FS) */
static const int32_t TONE_K[4] = {32768, 26984, 18264, 11121};

static inline int32_t soft_clip(int32_t x)       /* Q15 in, Q15 out: cubic, flat past +-1.0 */
{
    if (x > 32767)
        return 21845;
    if (x < -32767)
        return -21845;
    return x - (int32_t)(((int64_t)x * x * x) >> 30) / 3;
}

static void out_stage(int32_t *o, uint32_t n)
{
    uint32_t i, c, tone = fm1_tone & 3u, drive = fm1_drive & 3u;
    int32_t k = TONE_K[tone], m = master_vol;
    for (i = 0; i < n; i++) {
        if (fm1_mono) {                           /* (a voice panned to one side: 6 dB under a centred one) */
            const int32_t mono = (o[2u * i] + o[2u * i + 1u]) / 2;
            o[2u * i] = o[2u * i + 1u] = mono;
        }
        for (c = 0; c < 2u; c++) {
            int32_t x = o[2u * i + c] * 2;            /* +6 dB: one full channel at -18 dBFS, as on an OPL3 */
            if (tone) {                               /* two one-poles: 12 dB / octave */
                lp1[c] += (int32_t)(((int64_t)((x << 4) - lp1[c]) * k) >> 15);
                lp2[c] += (int32_t)(((int64_t)(lp1[c] - lp2[c]) * k) >> 15);
                x = lp2[c] >> 4;
            }
            if (drive) {
                x = soft_clip(x * (int32_t)(drive + 1u)) * 3 / 2;
            } else if (x > 32767)
                x = 32767;
            else if (x < -32767)
                x = -32767;
            x = (x * m) >> 8;
            o[2u * i + c] = x * (1 << OUT_SHIFT);
        }
    }
}

