/* SPDX-License-Identifier: GPL-3.0-only */
/* The FM-1's hardware layer (firmware/hal) for the plugin: the same names, on the Mac. The firmware is
 * compiled as it is (firmware/src/tracker.c with FM1_HOST); everything it asks of the hardware lands here,
 * and native/fm1_host.c (included at the end of the same unit) gives it the plugin's keys, time and flash.
 *   time      guest time = the audio rendered (fm1_host_ticks), as the FM-1's clock follows its codec
 *   IRQs      the audio "interrupt" runs on the plugin's render thread while the tracker's thread waits
 *             (fm1_host.c: the CPU lock); irq_save / irq_restore keep it out, as on the chip
 *   input     the plugin sets the debounced keys and adds encoder steps (no matrix scan)
 *   LCD       the ST7789 commands the firmware sends are drawn into a 240x240 picture
 *   flash     1 MiB in RAM; the plugin saves and restores its storage areas */
#pragma once
#include <stdint.h>
#include <string.h>

#define FM1_INLINE static inline

/* ------------------------------------------------------------ time --- */
#define FM1_TICKS_PER_US 24u
static uint32_t fm1_host_ticks(void);
static void fm1_host_wait_ticks(uint32_t span);
static inline void fm1_time_init(void) {}
static inline uint32_t fm1_ticks(void) { return fm1_host_ticks(); }
static inline uint32_t fm1_micros(void) { return fm1_host_ticks() / FM1_TICKS_PER_US; }
static inline void fm1_delay_us(uint32_t us) { fm1_host_wait_ticks(us * FM1_TICKS_PER_US); }
static inline void fm1_delay_ms(uint32_t ms) { fm1_host_wait_ticks(ms * 1000u * FM1_TICKS_PER_US); }

/* ------------------------------------------------------- system --- */
static void fm1_host_stop(const char *why);
static void host_poll(void);
static inline void fm1_wdt_arm(uint8_t t) { (void)t; }
static inline void fm1_wdt_feed(void) { host_poll(); }   /* (the firmware's long loops feed it: the sound's turn) */
static inline void fm1_wdt_stop(void) {}
static inline void fm1_reboot(void) { fm1_host_stop("REBOOT"); }
static inline void fm1_enter_uboot(void) { fm1_host_stop("UBOOT"); }

/* ------------------------------------------------- IRQs, guards --- */
static volatile uint32_t fm1_crit_prof;   /* the tracker's thread: IRQs off (nesting) */
static volatile uint8_t fm1_crit_irq;     /* inside the audio "interrupt" */
static inline uint32_t irq_save(void)
{
    if (fm1_crit_irq)
        return 1;
    fm1_crit_prof++;
    return 0;
}
static inline void irq_restore(uint32_t f)
{
    if (!f && fm1_crit_prof)
        fm1_crit_prof--;
}
static inline uint32_t fm1_icfg(void) { return 0; }
static inline void fm1_irq_off(void) {}
static inline void fm1_irq_on(void) {}
static inline void fm1_irq_init(void) {}
static inline void fm1_irq_enable_all(void) {}
static inline void fm1_guard_enable(uint32_t which) { (void)which; }
static inline void fm1_guard_lock_top(void) {}
static inline void fm1_guard_unlock_top(void) {}

/* ------------------------------------------------------------ input --- */
#define FM1_NCOL 11u
#define FM1_NKEY 41u              /* ids: 0..13 buttons, 14..40 note keys */
#define FM1_NENC 7u
static const int8_t FM1_KEYMAP[6][FM1_NCOL] = {   /* (as hal/fm1_input.h: the LEDs are addressed by it) */
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    { 5, 11,  4, 10,  3,  9,  2,  8, -1, -1, -1},
    {34, 35, 36, 37, 38, 40, 39, 13,  7,  6, 12},
    {23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},
    { 0,  1, 15, 14, 17, 16, 19, 18, 20, 21, 22},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
};
enum { FM1_BTN_OCT_DOWN = 0, FM1_BTN_OCT_UP = 1 };
static volatile struct {
    uint32_t notes;              /* bit n = note key n (0 = F3 .. 26 = G5) */
    uint32_t buttons;            /* bit i = button i (0..13) */
    uint32_t pressed, released;  /* button edges since the last fm1_input_edges() */
    uint32_t notes_pressed;      /* note-key press edges since the last fm1_input_note_edges() */
    int16_t enc_steps[FM1_NENC]; /* + = clockwise */
} fm1_in;
static uint32_t fm1_host_dpad;           /* the D-PAD functions held on the Mac's keyboard (KEY_A...: compat/nds.h) */
/* the host's playback, for this block (fm1_host.c): the song follows its position (compat/fm1_plateforme.c),
   at its tempo (suit 1) or at the song's own (suit 2: the host follows it through Link); 0 free */
static struct { int suit, joue; double temps, bpm; uint32_t depuis; } fm1_host_tempo;   /* temps: beats, depuis
                                                                                         frames into the block */
static uint8_t fm1_led[FM1_NCOL];
static uint8_t fm1_led_dim[FM1_NCOL];
static uint8_t fm1_led_breath[FM1_NCOL];
static uint8_t fm1_led_mid[FM1_NCOL];
static inline void fm1_input_init(void) {}
static inline void fm1_led_dim_level(uint32_t lo) { (void)lo; }
static inline int fm1_led_anim_on(void) { return 0; }
static inline void fm1_led_anim_stop(void) {}
/* (the tracker's thread holds the CPU: the plugin's keys come in under it, fm1_host.c) */
static int32_t fm1_enc_take(uint32_t e)
{
    int32_t s = fm1_in.enc_steps[e];
    fm1_in.enc_steps[e] = 0;
    return s;
}
static uint32_t fm1_input_edges(uint32_t *released)
{
    uint32_t p = fm1_in.pressed;
    if (released)
        *released = fm1_in.released;
    fm1_in.pressed = fm1_in.released = 0;
    return p;
}
static uint32_t fm1_input_note_edges(void)
{
    uint32_t p = fm1_in.notes_pressed;
    fm1_in.notes_pressed = 0;
    return p;
}
static void fm1_led_key(uint32_t id, int on)
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id) {
                if (on)
                    fm1_led[p] |= (uint8_t)(1u << r);
                else
                    fm1_led[p] &= (uint8_t)~(1u << r);
            }
}

/* ------------------------------------------------------- MASTER pot --- */
#define FM1_ADC_MASTER 0u
static int32_t fm1_host_master = 512;     /* 0..1023 */
static inline void fm1_adc_init(void) {}
static inline int32_t fm1_adc_read(uint32_t ch) { return ch == FM1_ADC_MASTER ? fm1_host_master : -1; }

/* ---------------------------------------------------- audio, timer --- */
#define FM1_AUDIO_HALF 0x80u
static inline void fm1_audio_init(int32_t *buf, uint32_t half_words, void (*isr)(void), uint32_t prio)
{
    (void)buf; (void)half_words; (void)isr; (void)prio;
}
static inline uint8_t fm1_audio_pending(void) { return FM1_AUDIO_HALF; }
static inline void fm1_audio_ack_aux(uint8_t p) { (void)p; }
static inline uint32_t fm1_audio_free_half(void) { return 0; }
static inline void fm1_audio_ack_half(void) {}
static inline void fm1_audio_stop(void) {}
static inline void fm1_timer5_start(void (*isr)(void), uint32_t prio) { (void)isr; (void)prio; }
static inline void fm1_timer5_ack(void) {}

/* -------------------------------------------------------------- LCD --- */
static uint16_t fm1_host_lcd[240 * 240];  /* RGB565, as the panel holds it */
static struct { uint8_t cmd; uint32_t arg, x0, x1, y0, y1, x, y; uint8_t hi, half; } fm1_host_panel;
static uint32_t fm1_lcd_timeouts;
static inline void fm1_lcd_hw_init(void) {}
static inline void fm1_lcd_backlight(uint32_t on) { (void)on; }
static inline void fm1_lcd_baud(uint32_t b) { (void)b; }
static inline void fm1_lcd_wait(void) {}
static inline void fm1_lcd_deselect(void) {}
static inline void fm1_lcd_send_cmd(uint8_t c)
{
    fm1_host_panel.cmd = c;
    fm1_host_panel.arg = 0;
    fm1_host_panel.half = 0;
    if (c == 0x2Cu) {                       /* RAMWR: from the window's corner */
        fm1_host_panel.x = fm1_host_panel.x0;
        fm1_host_panel.y = fm1_host_panel.y0;
    }
}
static void fm1_lcd_send_data(const void *p, uint32_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t i;
    for (i = 0; i < n; i++) {
        const uint32_t v = b[i];
        switch (fm1_host_panel.cmd) {
        case 0x2Au:                         /* CASET: x0 x1, 16 bits each */
        case 0x2Bu: {
            const uint32_t k = fm1_host_panel.arg++;
            uint32_t *d = fm1_host_panel.cmd == 0x2Au ? (k < 2u ? &fm1_host_panel.x0 : &fm1_host_panel.x1)
                                                      : (k < 2u ? &fm1_host_panel.y0 : &fm1_host_panel.y1);
            *d = (k & 1u) ? (*d & 0xFF00u) | v : v << 8;
            break;
        }
        case 0x2Cu:                         /* RAMWR: RGB565, high byte first */
            if (!fm1_host_panel.half) {
                fm1_host_panel.hi = (uint8_t)v;
                fm1_host_panel.half = 1;
                break;
            }
            fm1_host_panel.half = 0;
            if (fm1_host_panel.x < 240u && fm1_host_panel.y < 240u)
                fm1_host_lcd[fm1_host_panel.y * 240u + fm1_host_panel.x] = (uint16_t)(fm1_host_panel.hi << 8 | v);
            if (++fm1_host_panel.x > fm1_host_panel.x1) {
                fm1_host_panel.x = fm1_host_panel.x0;
                fm1_host_panel.y++;
            }
            break;
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------ flash --- */
#define FL_DATA_LO      0x00097000u
#define FL_DATA_HI      0x000E0000u
#define FL_GLOB_LO      0x000FC000u
#define FL_GLOB_HI      0x000FF000u
#define FL_OTA_LO       0x000E0000u
#define FL_OTA_HI       0x000E5000u
#define FL_AUTO_LO      0x000E5000u
#define FL_AUTO_HI      0x000E7000u
#define FL_VBANK_LO     0x000E7000u
#define FL_VBANK_HI     0x000E8000u
#define FL_IN(off, n, lo, hi) ((uint32_t)(off) >= (lo) && (uint32_t)(off) <= (hi) && \
                               (uint32_t)(n) <= (hi) - (uint32_t)(off))
#define FL_STORE_OK(off, n) (FL_IN(off, n, FL_DATA_LO, FL_DATA_HI) || FL_IN(off, n, FL_GLOB_LO, FL_GLOB_HI) || \
                             FL_IN(off, n, FL_AUTO_LO, FL_VBANK_HI))
#define FM1_HOST_FLASH (1u << 20)
static uint8_t fm1_host_flash[FM1_HOST_FLASH];
#define FL_FAR(fn) (fn)
static inline uint32_t fl_jedec_ram(void) { return 0x856014u; }
static int fl_read_ram(uint32_t off, uint8_t *dst, uint32_t n)
{
    if (off >= FM1_HOST_FLASH || n > FM1_HOST_FLASH - off)
        return -1;
    memcpy(dst, fm1_host_flash + off, n);
    return 0;
}
static int fl_erase4k_ram(uint32_t off, uint32_t *took_us)
{
    if (took_us)
        *took_us = 0;
    if (off & 0xFFFu || off >= FM1_HOST_FLASH)
        return -1;
    memset(fm1_host_flash + off, 0xFF, 0x1000u);
    return 0;
}
static int fl_prog_ram(uint32_t off, const uint8_t *src, uint32_t n, uint32_t *took_us)
{
    uint32_t i;
    if (took_us)
        *took_us = 0;
    if (off >= FM1_HOST_FLASH || n > FM1_HOST_FLASH - off)
        return -1;
    for (i = 0; i < n; i++)                 /* (a NOR write only clears bits) */
        fm1_host_flash[off + i] &= src[i];
    return 0;
}
static int fl_write(uint32_t off, const uint8_t *src_ram, uint32_t n)
{
    return fl_prog_ram(off, src_ram, n, 0);
}

/* --------------------------------------------------- the Mac's files --- */
/* PROJECT > LOAD opens the Mac's files: the song chosen there is read as MAC/<name> (compat/fm1_fs.c),
 * then the tracker opens it as from its list (amber/main.cpp, fm1_host.c) */
#define FM1_HOST_MAC_MAX (1u << 20)
static uint8_t fm1_host_mac_data[FM1_HOST_MAC_MAX];
static char fm1_host_mac_name[40];
static uint32_t fm1_host_mac_size;
static const uint8_t *fm1_host_mac_file(const char *name, uint32_t *size)
{
    if (!fm1_host_mac_name[0] || strcasecmp(name, fm1_host_mac_name))
        return 0;
    *size = fm1_host_mac_size;
    return fm1_host_mac_data;
}
