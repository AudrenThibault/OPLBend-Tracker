/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-1 OPL3 TRACKER: what the firmware side shares. The tracker itself (firmware/amber: the DS / RG350
 * tracker, its interface and its sequencer) runs on top, through the DS-like layer of firmware/compat. */

#define FS 44118u                 /* the I2S frame rate: 24 MHz / 544 = 44,117.6 Hz */
#define HALF_FRAMES 128u          /* I2S half buffer, 2.9 ms */
#define CTL 16u                   /* audio block: the sequencer's ticks fall on block boundaries (0.36 ms) */

static volatile uint32_t fm1_ms;  /* milliseconds since boot (TIMER5 ISR, main.c) */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")   /* a ring slot stored before its index moves */
/* Bit `n` of `v` as 0 or 1, opaque to the compiler: it cannot turn « bit ? mask : 0 » into sextra, a
   signed bit-field extract that the web emulator (the online one) runs as unsigned. Costs nothing. */
#include "fm1_opaque.h"
static inline uint32_t web_bit(uint32_t v, uint32_t n) { return fm1_opaque((v >> n) & 1u); }

#define BOOTGUARD_MAGIC 0x42475244u
struct { uint32_t magic, failed, pending; } bootguard __attribute__((section(".noinit")));

static volatile uint8_t master_vol = 200;   /* the MASTER pot, 0..255 (master_poll) */
static uint8_t fm1_tone = 1;                 /* output filter: 0 OFF, 1 SOFT, 2 WARM, 3 DARK (outstage.c) */
static uint8_t fm1_drive;                    /* 0 OFF, 1..3 soft saturation */

static const char HEX[] = "0123456789ABCDEF";
