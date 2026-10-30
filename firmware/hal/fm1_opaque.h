/* SPDX-License-Identifier: GPL-3.0-only */
/* A value the compiler cannot see through. It keeps it from emitting the few instruction forms the web
 * emulator (the online one) gets wrong: sextra, a signed bit-field extract it runs as unsigned, for
 * « bit ? mask : 0 » or « 4-bit value >= 8 ». The device runs them fine; this costs nothing there. */
#pragma once
#include <stdint.h>

static inline uint32_t fm1_opaque(uint32_t v)
{
    __asm__("" : "+r"(v));
    return v;
}
