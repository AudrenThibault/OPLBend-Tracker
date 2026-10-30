/* SPDX-License-Identifier: GPL-3.0-only */
/* The engine (opl.c, player.c, song.c's defaults, audio.c's output stage) built for the Mac: renders
 * the demo song to a WAV, so the sound can be checked without the emulator.
 *   tests/host.sh  ->  build/host_demo.wav */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "tk_tables.h"

/* the firmware's environment, reduced */
static uint32_t fm1_icfg(void) { return 0; }
static void fm1_irq_off(void) {}
static void fm1_wdt_feed(void) {}
static unsigned str_len(const char *s) { return (unsigned)strlen(s); }
static void str_cpy(char *d, const char *s, unsigned max)
{
    unsigned i = 0;
    while (s[i] && i + 1 < max) { d[i] = s[i]; i++; }
    d[i] = 0;
}
#define FL_DATA_HI 0x000E0000u
#define ST_SECTOR 4096u
#define ST_PAYLOAD_OFF 256u
static uint8_t flash_ok;
static int st_read(uint32_t off, void *d, uint32_t n) { (void)off; (void)d; (void)n; return -1; }
static int st_erase(uint32_t off) { (void)off; return -1; }
static int st_prog(uint32_t off, const void *s, uint32_t n) { (void)off; (void)s; (void)n; return -1; }
static uint32_t st_crc32(const void *p, uint32_t n) { (void)p; (void)n; return 0; }
#define __attribute__(x)
#include "../firmware/src/core.h"
#undef __attribute__
#include "../firmware/src/opl.c"
#include "../firmware/src/player.c"
#include "../firmware/src/song.c"

/* audio.c's output stage, as is */
#define OUT_SHIFT 7
#include "../firmware/src/outstage.c"

static void wr16(FILE *f, uint32_t v) { fputc((int)(v & 255u), f); fputc((int)(v >> 8), f); }
static void wr32(FILE *f, uint32_t v) { wr16(f, v & 0xFFFFu); wr16(f, v >> 16); }

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "build/host_demo.wav";
    double seconds = argc > 2 ? atof(argv[2]) : 8.0;
    uint32_t frames = (uint32_t)(seconds * FS), done, i;
    int32_t mix[2 * CTL];
    FILE *f = fopen(out, "wb");
    double sum[2] = {0, 0};
    int32_t peak = 0;
    song_defaults();
    if (argc > 3)
        sd.proj.tone = (uint8_t)atoi(argv[3]);
    opl_init();
    master_vol = 255;
    player_post(EV_PLAY, 0, PM_SONG, 0);
    fwrite("RIFF", 1, 4, f); wr32(f, 36 + frames / CTL * CTL * 4); fwrite("WAVEfmt ", 1, 8, f);
    wr32(f, 16); wr16(f, 1); wr16(f, 2); wr32(f, FS); wr32(f, FS * 4); wr16(f, 4); wr16(f, 16);
    fwrite("data", 1, 4, f); wr32(f, frames / CTL * CTL * 4);
    for (done = 0; done + CTL <= frames; done += CTL) {
        player_block();
        memset(mix, 0, sizeof mix);
        opl_render(mix, CTL);
        out_stage(mix, CTL);
        for (i = 0; i < 2 * CTL; i++) {
            int32_t s = mix[i] >> 8;               /* 24-bit -> 16-bit */
            sum[i & 1] += (double)s * s;
            if (abs(s) > peak) peak = abs(s);
            wr16(f, (uint32_t)(uint16_t)(int16_t)s);
        }
    }
    fclose(f);
    printf("%s: %.1f s, peak %.3f, rms L %.4f R %.4f\n", out, seconds, peak / 32768.0,
           sqrt(sum[0] / (frames / 1.0)) / 32768.0, sqrt(sum[1] / (frames / 1.0)) / 32768.0);
    return 0;
}
