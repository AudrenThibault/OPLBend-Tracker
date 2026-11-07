/* SPDX-License-Identifier: GPL-3.0-only */
/* The firmware built for the Mac (fm1_host.c), as the plugin sees it: a library loaded once per instance. */
#pragma once
#include <stdint.h>

#define FM1N_VERSION 5
#define FM1N_RATE 44118           /* the FM-1's sample rate (core.h FS) */
#define FM1N_BLOCK 128            /* fm1n_render's frames come in blocks of this */
#define FM1N_LCD 240
#define FM1N_SONG_MAX (1 << 20)    /* a song file, at most (hal/fm1_host_hal.h FM1_HOST_MAC_MAX) */
#define FM1N_CONTROLS 41          /* 0..13 buttons, 14..40 note keys */
#define FM1N_LED_FAINT 70
#define FM1N_LED_STRONG 255
enum { FM1N_DPAD_A, FM1N_DPAD_B, FM1N_DPAD_SELECT, FM1N_DPAD_START, FM1N_DPAD_RIGHT, FM1N_DPAD_LEFT,
       FM1N_DPAD_UP, FM1N_DPAD_DOWN, FM1N_DPAD_DELETE = 10 };   /* (bits of compat/nds.h KEY_*: DELETE is its X) */

#ifdef __cplusplus
extern "C" {
#endif

int fm1n_version(void);
void fm1n_flash_write(uint32_t off, const uint8_t *src, uint32_t n);   /* the 1 MiB flash (before fm1n_start) */
void fm1n_flash_read(uint32_t off, uint8_t *dst, uint32_t n);
int fm1n_start(void);                                       /* boot, then the tracker, on its own thread */
void fm1n_stop(void);
int fm1n_render(int32_t *lr, int frames);                   /* 24-bit stereo, interleaved; returns the frames done */
void fm1n_offline(int offline);                             /* 1: the tracker gets all the time it needs (exports) */
void fm1n_key(int id, int down);
void fm1n_dpad(int bit, int down);                          /* a D-PAD function, in every GLO mode (FM1N_DPAD_*) */
void fm1n_encoder(int index, int steps);                    /* 0 SELECT, 1 PRESETS, 2 ALGORITHM, 3..6 KNOB 1..4 */
void fm1n_master(int value);                                /* 0..1023 */
void fm1n_engine(int nuked);                                /* the OPL3: 0 the FM-1's, 1 Nuked */
void fm1n_transport(int follow, int playing, double beat, double bpm);   /* before each block: the host's playback;
                                       follow 1: its position and tempo, 2: its position, the song's tempo; 0 free */
void fm1n_link(int peers);                                  /* 1: Link has peers (START holds the song for the host) */
void fm1n_play(int on);                                     /* start (the whole song) or stop the tracker */
int fm1n_tracker(double *bpm, int *start, double *late);    /* 1: the song plays; its own tempo; START since
                                       the last call: 1 started, -1 stopped, 0 none; how late it is on the host (ms) */
int fm1n_load_wanted(void);                                 /* 1 once PROJECT > LOAD asked for a song from the Mac */
int fm1n_load(const char *name, const uint8_t *data, uint32_t n);   /* the song chosen (.OBT .XT .A2M), opened next;
                                                                       name 0: none (the songs saved in the project) */
int fm1n_saved(char *name, int max, uint8_t *data, uint32_t n);   /* PROJECT > SAVE: the song just saved (its file
                                                                     name and contents), once; returns its size, 0: none */
uint32_t fm1n_screen(uint16_t *rgb565);                     /* 240 x 240; returns the frame's number */
void fm1n_leds(uint8_t *levels);                            /* FM1N_CONTROLS levels: 0, FAINT, STRONG */
int fm1n_error(char *out, int max);                         /* why the firmware stopped, if it did */

#ifdef __cplusplus
}
#endif
