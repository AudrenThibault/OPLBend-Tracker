/* SPDX-License-Identifier: GPL-3.0-only */
/* I2S output (ALNK0 -> the external codec, hal/fm1_audio.h) and the audio ISR. Per half buffer, blocks
 * of CTL frames: the tracker's sequencer ticks that fall in the block (fm1_sequenceur: the DS timer 0),
 * the OPL3 (opl3_rend), the GLITCH (glitch.c), then the output stage (outstage.c) and 24-bit stereo. */
#define HALF_WORDS (HALF_FRAMES * 2u)
#define OUT_SHIFT 7                               /* Q15 -> 24-bit, -6 dBFS ceiling */

static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));
static int32_t mixbuf[2u * CTL];
static volatile uint32_t audio_us, audio_us_max;  /* the last and the worst render of a half */
#include "outstage.c"
#include "glitch.c"                         /* GLITCH (FX, second page), on the mix */
#include "masterfx.c"                       /* MASTER FX (FX, third page) */

static void fm1_sequenceur(uint32_t n);           /* compat/fm1_plateforme.c */
#if FELUCCA_UAC
/* L'ENTREE AUDIO USB (usb.c, plus loin) : ce qu'on entend, au niveau de MASTER, envoye a l'ordinateur (Ableton
   l'enregistre comme une carte son) ; le volume a l'ecoute est celui du FM-1, on ne s'y fait pas mal */
static void uac_render_start(void);
static void uac_tap(const int32_t *out, uint32_t n);
static int32_t usbbuf[2u * CTL];
#endif

void fm1_alnk0_irq(void)                         /* via isr_alnk0 (hal/fm1_isr.S) */
{
    uint8_t p = fm1_audio_pending();
    uint32_t t0 = fm1_ticks();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half(), b, i, us;
        int32_t *o = &abuf[half * HALF_WORDS];
        fm1_crit_irq = 1;                         /* the engine's locks must not turn the IRQs back on here */
#if FELUCCA_UAC
        uac_render_start();
#endif
        for (b = 0; b < HALF_FRAMES; b += CTL) {
            fm1_sequenceur(CTL);
            for (i = 0; i < 2u * CTL; i++)
                mixbuf[i] = 0;
            opl3_rend(mixbuf, CTL);
            glitch_rend(mixbuf, CTL);
            mfx_rend(mixbuf, CTL);
            out_stage(mixbuf, CTL);
#if FELUCCA_UAC
            for (i = 0; i < 2u * CTL; i++)       /* (Q15, celui de la sortie avant le 24 bits) */
                usbbuf[i] = mixbuf[i] >> OUT_SHIFT;
            uac_tap(usbbuf, CTL);
#endif
            for (i = 0; i < 2u * CTL; i++)
                o[2u * b + i] = mixbuf[i];
        }
        fm1_crit_irq = 0;
        fm1_audio_ack_half();
        us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
        audio_us = us;
        if (us > audio_us_max)
            audio_us_max = us;
    }
}
extern void isr_alnk0(void);

static void audio_init(void)
{
    uint32_t i;
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
}
