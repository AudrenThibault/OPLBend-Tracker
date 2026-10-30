/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The few libc routines the compiler may call implicitly (struct copies,
 * zeroing). Freestanding: no other libc. */
#include <stddef.h>
#ifndef FM1_LIBC_JIELI          /* the tracker links JieLi's libc, which has these */
void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

#else
/* memmove (and memcpy, in the same object of JieLi's libc): JieLi's copies backwards with
   b[++r=-1] (EE59 / EE5B), which the web emulator does not know (tools/build.py, web_compat).
   The same, by index: it compiles to forms both run. Whole words when both are aligned. */
static void copie_avant(unsigned char *p, const unsigned char *q, size_t n)
{
    size_t i = 0;
    if ((((size_t)p | (size_t)q) & 3u) == 0)
        for (; i + 4u <= n; i += 4u)
            *(unsigned *)(void *)(p + i) = *(const unsigned *)(const void *)(q + i);
    for (; i < n; i++)
        p[i] = q[i];
}

void *memcpy(void *d, const void *s, size_t n)
{
    copie_avant(d, s, n);
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    size_t i;
    if (p <= q || p >= q + n)
        copie_avant(p, q, n);
    else
        for (i = n; i; i--)
            p[i - 1] = q[i - 1];
    return d;
}
#endif

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static unsigned str_len(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

static void str_cpy(char *d, const char *s, unsigned max)
{
    unsigned i = 0;
    while (s[i] && i + 1 < max) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}

/* decimal with an optional fixed point: fmt_fix(buf, -63, 1) -> "-6.3" */
static void fmt_fix(char *b, int32_t v, int decimals)
{
    char t[12];
    int n = 0, neg = v < 0, i;
    uint32_t u = (uint32_t)(neg ? -v : v);
    do {
        t[n++] = (char)('0' + u % 10u);
        u /= 10u;
        if (n == decimals)
            t[n++] = '.';
    } while (u || n <= decimals);
    if (neg)
        t[n++] = '-';
    for (i = 0; i < n; i++)
        b[i] = t[n - 1 - i];
    b[n] = 0;
}

static void fmt_int(char *b, int32_t v) { fmt_fix(b, v, 0); }

static uint32_t rng_state = 0x1234567u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
