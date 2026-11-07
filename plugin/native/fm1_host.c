/* SPDX-License-Identifier: GPL-3.0-only */
/* The plugin's side of the firmware (firmware/src/tracker.c includes it with FM1_HOST, in place of main.c, usb.c
 * and ota.c), and the API the plugin calls (fm1_native.h). Each plugin instance loads its own copy of this library,
 * so the firmware's globals are one FM-1 per instance.
 *
 * Two threads share the machine as the chip's main loop and its audio interrupt do:
 *   the tracker's thread runs boot and main() (firmware/amber/main.cpp), holding the CPU lock;
 *   the plugin's render thread takes the lock to run the audio interrupt (audio.c fm1_alnk0_irq), half buffer
 *   by half buffer. The tracker's thread lets it in whenever it reads the time (fm1_ms, fm1_ticks), which its
 *   loops do all the time, unless the IRQs are off (irq_save): then the audio waits, as on the chip.
 * Guest time is the audio rendered: the tracker runs exactly as fast as the sound is pulled, offline too. They take
 * turns: before each half buffer the render waits for the tracker to be idle, waiting for the time to move on (the
 * same time read HOST_SPINS times in a row), so its frames keep up however fast the sound is rendered: offline, as
 * long as it takes (the same keys give the same sound, every time); in real time HOST_TURN at most, the sound
 * goes on (a song loading takes time, as on the FM-1). The plugin's keys and knobs are queued and reach the
 * firmware between two half buffers, as the FM-1's key scan does from its timer interrupt. */
#include <pthread.h>
#include <pthread/qos.h>
#include <sched.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <time.h>
#include "fm1_native.h"

extern int main(void);                            /* the tracker (firmware/amber/main.cpp) */

#define FM1N_API __attribute__((visibility("default")))
#define HOST_KEY_QUEUE 8                          /* a key's presses and releases waiting for the tracker's frames */
#define HOST_SPINS 256u
#define HOST_TURN_NS 2000000                      /* (2 ms of the Mac's time per 2.9 ms of sound) */

static pthread_mutex_t host_cpu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t host_turn = PTHREAD_MUTEX_INITIALIZER;   /* the turns: host_idle, and the two signals */
static pthread_cond_t host_went_idle = PTHREAD_COND_INITIALIZER, host_ticked = PTHREAD_COND_INITIALIZER;
static int host_idle;
static atomic_int host_offline;
static struct { uint8_t key; int8_t a; int16_t b; } host_input[256];   /* (host_turn) */
static uint32_t host_input_n;
static pthread_mutex_t host_screen_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int host_wants, host_stopping, host_alive;
static pthread_t host_thread, host_join;          /* the tracker's thread: as it stores itself, as created */
static int host_started;
static jmp_buf host_exit;
static uint64_t host_frames;                      /* guest time: the frames rendered */
static uint64_t host_seen_frames;
static uint32_t host_creep, host_spins;           /* time read with the IRQs off moves on; reads that see no change */
static char host_error[64];
/* Each key (the panel's, then the Mac's D-PAD keys: bits of KEY_A...) changes once a frame of the tracker at most,
   so that it sees every press and every release, however quick: taps faster than its frames are not merged into
   one long press (which its key repeat would then turn into a jump) */
struct host_keys { uint8_t n, last, state[HOST_KEY_QUEUE]; };
static struct host_keys host_keys[FM1_NKEY], host_dpad_keys[12];
static uint16_t host_screen[240 * 240];
static uint32_t host_screen_version, host_screen_frame;

/* host_thread is stored by the tracker's thread itself as it starts (pthread_create's own store may come after the
   thread has run); no thread-local variable: macOS never unloads a library that has one */
static int host_on_tracker(void) { return host_started && pthread_equal(pthread_self(), host_thread); }

/* the tracker's thread: let the audio in (it asks), sleep while time stands still (its turn is over), stop when
   asked */
static void host_poll(void)
{
    if (!host_on_tracker() || fm1_crit_prof)
        return;
    if (atomic_load(&host_stopping))
        longjmp(host_exit, 1);
    if (host_frames != host_seen_frames) {
        host_seen_frames = host_frames;
        host_spins = 0;
    } else if (++host_spins >= HOST_SPINS) {
        const uint64_t seen = host_frames;
        host_spins = 0;
        pthread_mutex_unlock(&host_cpu);
        pthread_mutex_lock(&host_turn);
        host_idle = 1;
        pthread_cond_broadcast(&host_went_idle);
        while (host_frames == seen && !atomic_load(&host_stopping))
            pthread_cond_wait(&host_ticked, &host_turn);
        host_idle = 0;
        pthread_mutex_unlock(&host_turn);
        pthread_mutex_lock(&host_cpu);
        return;
    }
    if (atomic_load(&host_wants)) {
        pthread_mutex_unlock(&host_cpu);
        while (atomic_load(&host_wants))
            sched_yield();
        pthread_mutex_lock(&host_cpu);
    }
}

static uint32_t host_now_ticks(void)
{
    return (uint32_t)(host_frames * 24000000u / FS) + host_creep;
}

static uint32_t fm1_host_ticks(void)
{
    if (host_on_tracker() && fm1_crit_prof)
        host_creep += FM1_TICKS_PER_US;           /* (a wait with the IRQs off ends: TIMER4 runs on the chip) */
    host_poll();
    return host_now_ticks();
}

static uint32_t fm1_host_ms(void)
{
    host_poll();
    return (uint32_t)(host_frames * 1000u / FS);
}

static void fm1_host_wait_ticks(uint32_t span)
{
    const uint32_t t0 = fm1_host_ticks();
    while ((uint32_t)(fm1_host_ticks() - t0) < span)
        ;
}

static void fm1_host_stop(const char *why)
{
    if (!host_error[0])
        str_cpy(host_error, why, sizeof host_error);
    if (host_on_tracker())
        longjmp(host_exit, 1);
}

static void fm1_fatal(const char *quoi) { fm1_host_stop(quoi); }

static void master_poll(void)                     /* (main.c's, from the plugin's MASTER) */
{
    static int32_t knob = -1;
    const int32_t a = fm1_adc_read(FM1_ADC_MASTER);
    knob = knob < 0 ? a * 16 : knob + (a * 16 - knob) / 8;
    {
        const uint32_t k = (uint32_t)knob / 16u;
        master_vol = (uint8_t)((k * k) >> 12 > 255u ? 255u : (k * k) >> 12);
    }
}

static void tempo_poll(void);
static int keys_pop(struct host_keys *q);
static void host_set_key(uint32_t id, int down);
static void keys_frame(void)                      /* the next press or release of each key, for this frame */
{
    uint32_t i;
    int down;
    for (i = 0; i < FM1_NKEY; i++)
        if ((down = keys_pop(&host_keys[i])) >= 0)
            host_set_key(i, down);
    for (i = 0; i < 12u; i++)
        if ((down = keys_pop(&host_dpad_keys[i])) >= 0)
            fm1_host_dpad = down ? fm1_host_dpad | 1u << i : fm1_host_dpad & ~(1u << i);
}

static void fm1_entretien(void)                    /* once a frame (swiWaitForVBlank) */
{
    keys_frame();
    master_poll();
    tempo_poll();
}

static void *host_main(void *arg)
{
    (void)arg;
    host_thread = pthread_self();
    pthread_mutex_lock(&host_cpu);
    if (!setjmp(host_exit)) {
        flash_ok = 1;
        lcd_init();
        master_poll();
        opl3_reset();
        audio_init();
        s_ecran_neuf = 1;
        main();
        fm1_host_stop("MAIN RETURNED");
    }
    fm1_crit_prof = 0;
    pthread_mutex_unlock(&host_cpu);
    pthread_mutex_lock(&host_turn);
    atomic_store(&host_alive, 0);
    pthread_cond_broadcast(&host_went_idle);
    pthread_mutex_unlock(&host_turn);
    return 0;
}

/* the render thread, before a half buffer: the tracker's turn first, until it waits for the time (or HOST_TURN) */
static void host_wait_turn(void)
{
    struct timespec t;
    pthread_mutex_lock(&host_turn);
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_nsec += HOST_TURN_NS;
    if (t.tv_nsec >= 1000000000) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000;
    }
    while (!host_idle && atomic_load(&host_alive))
        if (atomic_load(&host_offline))
            pthread_cond_wait(&host_went_idle, &host_turn);
        else if (pthread_cond_timedwait(&host_went_idle, &host_turn, &t))
            break;
    host_idle = 0;                                /* (this turn is taken: the next waits for the tracker again) */
    pthread_mutex_unlock(&host_turn);
}

static void host_tick(void)                       /* after it: time has moved on, the tracker's turn */
{
    pthread_mutex_lock(&host_turn);
    pthread_cond_broadcast(&host_ticked);
    pthread_mutex_unlock(&host_turn);
}

/* the plugin's threads: the CPU, the audio interrupt's way */
static void host_take(void)
{
    atomic_store(&host_wants, 1);
    pthread_mutex_lock(&host_cpu);
    atomic_store(&host_wants, 0);
}
static void host_give(void) { pthread_mutex_unlock(&host_cpu); }

static void host_queue(int key, int a, int b)
{
    pthread_mutex_lock(&host_turn);
    if (host_input_n < sizeof host_input / sizeof host_input[0]) {
        host_input[host_input_n].key = (uint8_t)key;
        host_input[host_input_n].a = (int8_t)a;
        host_input[host_input_n].b = (int16_t)b;
        host_input_n++;
    }
    pthread_mutex_unlock(&host_turn);
}

static void keys_push(struct host_keys *q, int down)    /* (the render, the CPU taken) */
{
    if (q->last == (uint8_t)down)
        return;
    q->last = (uint8_t)down;
    if (q->n < HOST_KEY_QUEUE)
        q->state[q->n++] = (uint8_t)down;
    else
        q->state[q->n - 1] = (uint8_t)down;
}

static int keys_pop(struct host_keys *q)                 /* (the tracker, once a frame) */
{
    int down;
    if (!q->n)
        return -1;
    down = q->state[0];
    memmove(q->state, q->state + 1, --q->n);
    return down;
}

static void host_set_key(uint32_t id, int down)
{
    const uint32_t note = id >= 14u, bit = note ? 1u << (id - 14u) : 1u << id;
    const uint32_t was = ((note ? fm1_in.notes : fm1_in.buttons) & bit) != 0u;
    if ((uint32_t)(down != 0) == was)
        return;
    if (note) {
        fm1_in.notes ^= bit;
        if (down)
            fm1_in.notes_pressed |= bit;
    } else {
        fm1_in.buttons ^= bit;
        if (down)
            fm1_in.pressed |= bit;
        else
            fm1_in.released |= bit;
    }
}

static void host_flash_init(void)                 /* an erased part, the first time it is touched */
{
    static int done;
    if (!done) {
        memset(fm1_host_flash, 0xFF, sizeof fm1_host_flash);
        done = 1;
    }
}

static void host_inputs(void)                     /* (the render, the CPU taken) */
{
    static const uint8_t MATRIX[FM1_NENC] = {0, 6, 1, 2, 3, 4, 5};   /* SELECT PRESETS ALGORITHM KNOB1..4 */
    uint32_t k;
    pthread_mutex_lock(&host_turn);
    for (k = 0; k < host_input_n; k++) {
        const int a = host_input[k].a, b = host_input[k].b;
        if (host_input[k].key == 2)               /* a D-PAD function from the Mac's keyboard */
            keys_push(&host_dpad_keys[a], b != 0);
        else if (host_input[k].key)
            keys_push(&host_keys[a], b != 0);
        else {
            const int32_t s = fm1_in.enc_steps[MATRIX[a]] + b;
            fm1_in.enc_steps[MATRIX[a]] = (int16_t)(s > 32 ? 32 : s < -32 ? -32 : s);
        }
    }
    host_input_n = 0;
    pthread_mutex_unlock(&host_turn);
}

/* ------------------------------------------------------------------ API --- */
FM1N_API int fm1n_version(void) { return FM1N_VERSION; }

FM1N_API void fm1n_flash_write(uint32_t off, const uint8_t *src, uint32_t n)
{
    if (off < FM1_HOST_FLASH && n <= FM1_HOST_FLASH - off) {
        host_take();
        host_flash_init();
        memcpy(fm1_host_flash + off, src, n);
        host_give();
    }
}

FM1N_API void fm1n_flash_read(uint32_t off, uint8_t *dst, uint32_t n)
{
    if (off < FM1_HOST_FLASH && n <= FM1_HOST_FLASH - off) {
        host_take();
        host_flash_init();
        memcpy(dst, fm1_host_flash + off, n);
        host_give();
    }
}

FM1N_API int fm1n_start(void)
{
    if (host_started)
        return 0;
    host_flash_init();
    host_started = 1;
    atomic_store(&host_alive, 1);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);   /* (the sound waits for its turns) */
    if (pthread_create(&host_join, &attr, host_main, 0)) {
        host_started = 0;
        atomic_store(&host_alive, 0);
        return -1;
    }
    return 0;
}

FM1N_API void fm1n_stop(void)
{
    if (!host_started)
        return;
    atomic_store(&host_stopping, 1);
    host_tick();
    pthread_join(host_join, 0);
    host_started = 0;
}

FM1N_API int fm1n_render(int32_t *lr, int frames)
{
    int done = 0;
    uint32_t i;
    for (; done + (int)HALF_FRAMES <= frames; done += (int)HALF_FRAMES) {
        host_wait_turn();
        host_take();
        host_inputs();
        fm1_alnk0_irq();
        memcpy(lr + 2 * done, abuf, sizeof(int32_t) * 2u * HALF_FRAMES);
        host_frames += HALF_FRAMES;
        if ((host_screen_frame += HALF_FRAMES) >= FS / 60u) {   /* the screen, 60 times a second of sound */
            host_screen_frame = 0;
            pthread_mutex_lock(&host_screen_lock);
            memcpy(host_screen, fm1_host_lcd, sizeof host_screen);
            host_screen_version++;
            pthread_mutex_unlock(&host_screen_lock);
        }
        host_give();
        host_tick();
    }
    return done;
}

FM1N_API void fm1n_key(int id, int down)
{
    if (id >= 0 && id < (int)FM1_NKEY)
        host_queue(1, id, down != 0);
}

FM1N_API void fm1n_dpad(int bit, int down)
{
    if (bit >= 0 && bit < 12)
        host_queue(2, bit, down != 0);
}

FM1N_API void fm1n_encoder(int index, int steps)
{
    if (index >= 0 && index < (int)FM1_NENC)
        host_queue(0, index, steps < -32 ? -32 : steps > 32 ? 32 : steps);
}

/* PROJECT > LOAD (amber/main.cpp): the plugin is asked for a song from the Mac; it gives it here (or none),
 * and the tracker opens it at its next frame (or, with none, its list of the songs saved in the project) */
static atomic_int host_mac_wanted;
static int host_mac_ready;                          /* 1 a song, 2 none chosen (the CPU) */

void fm1_host_mac_demande(void) { atomic_store(&host_mac_wanted, 1); }

int fm1_host_mac_arrive(char *nom, int max)
{
    const int r = host_mac_ready;
    host_mac_ready = 0;
    if (r == 1)
        str_cpy(nom, fm1_host_mac_name, (unsigned)max);
    return r;
}

FM1N_API int fm1n_load_wanted(void) { return atomic_exchange(&host_mac_wanted, 0); }

FM1N_API int fm1n_load(const char *name, const uint8_t *data, uint32_t n)
{
    if (name && (!name[0] || strlen(name) >= sizeof fm1_host_mac_name || n > FM1_HOST_MAC_MAX))
        return -1;
    host_take();
    if (name) {
        memcpy(fm1_host_mac_data, data, n);
        fm1_host_mac_size = n;
        str_cpy(fm1_host_mac_name, name, sizeof fm1_host_mac_name);
    }
    host_mac_ready = name ? 1 : 2;
    host_give();
    return 0;
}

/* PROJECT > SAVE (amber/main.cpp): the song just saved in the project, offered to the Mac (the plugin's dialog) */
static uint8_t host_saved_data[FM1_HOST_MAC_MAX];
static char host_saved_name[40];
static uint32_t host_saved_size;                    /* 0: none waiting (the CPU) */
static atomic_int host_saved_waiting;

void fm1_host_mac_sauve(const char *nom, const uint8_t *data, uint32_t n)
{
    if (!n || n > FM1_HOST_MAC_MAX)
        return;
    memcpy(host_saved_data, data, n);
    str_cpy(host_saved_name, nom, sizeof host_saved_name);
    host_saved_size = n;
    atomic_store(&host_saved_waiting, 1);
}

FM1N_API int fm1n_saved(char *name, int max, uint8_t *data, uint32_t n)
{
    uint32_t size;
    if (!atomic_exchange(&host_saved_waiting, 0))
        return 0;
    host_take();
    size = host_saved_size <= n ? host_saved_size : 0;
    if (size) {
        memcpy(data, host_saved_data, size);
        str_cpy(name, host_saved_name, (unsigned)max);
    }
    host_saved_size = 0;
    host_give();
    return (int)size;
}

/* The host's playback (amber/main.cpp, compat/fm1_plateforme.c): its position each block, its start and stop;
 * and back, the song's own tempo and the START presses (the plugin passes them on through Link). */
int md_replayer_get_rows_per_beat(void);
int md_replayer_host_lignes_par_temps(void);
double md_replayer_get_bpm(void);
void md_replayer_set_bpm(double bpm);
static atomic_int host_lecture, host_start_edge;

int fm1_host_lecture(void) { return atomic_exchange(&host_lecture, 0); }
static atomic_int host_link;                        /* Link has peers: START starts the host too */

void fm1_host_start(int joue)
{
    /* (with Link, the song waits for the host from its very first tick: the plugin holds it, then the host's
       start lines it up, compat/fm1_plateforme.c fm1_suit_hote) */
    if (joue && atomic_load(&host_link) && fm1_host_tempo.suit)
        fm1_host_tempo.joue = 2;
    atomic_store(&host_start_edge, joue ? 1 : -1);
}

FM1N_API void fm1n_link(int peers) { atomic_store(&host_link, peers != 0); }

static void tempo_poll(void)                       /* following the host's tempo: the song shows it (PROJECT) */
{
    /* (the host's beat may be several of the song's, md_replayer_host_lignes_par_temps: in the song's own BPM) */
    const double bpm = fm1_host_tempo.bpm * md_replayer_host_lignes_par_temps() / md_replayer_get_rows_per_beat();
    const double d = md_replayer_get_bpm() - bpm;
    if (fm1_host_tempo.suit == 1 && fm1_host_tempo.joue && bpm > 1.0 && (d > 0.005 || d < -0.005))
        md_replayer_set_bpm(bpm);
}

FM1N_API void fm1n_transport(int follow, int playing, double beat, double bpm)
{
    fm1_host_tempo.suit = follow;
    fm1_host_tempo.joue = playing;
    fm1_host_tempo.temps = beat;
    fm1_host_tempo.bpm = bpm;
    fm1_host_tempo.depuis = 0;
}

FM1N_API void fm1n_play(int on) { atomic_store(&host_lecture, on ? 1 : -1); }

FM1N_API int fm1n_tracker(double *bpm, int *start, double *late)
{
    uint32_t p;
    int playing;
    host_take();
    p = s_periode_q16 ? s_periode_q16 : periode_timer0_q16();
    *bpm = p ? 60.0 * FS * 65536.0 / ((double)p * md_replayer_host_vitesse() * md_replayer_host_lignes_par_temps())
               * fm1_vitesse() / 65536.0
             : md_replayer_get_bpm() * md_replayer_get_rows_per_beat() / md_replayer_host_lignes_par_temps();
    playing = md_replayer_host_joue_song();
    *late = s_cale ? s_ecart_ms : 0.0;
    host_give();
    *start = atomic_exchange(&host_start_edge, 0);
    return playing;
}

FM1N_API void fm1n_offline(int offline) { atomic_store(&host_offline, offline != 0); }

FM1N_API void fm1n_master(int value) { fm1_host_master = value < 0 ? 0 : value > 1023 ? 1023 : value; }

FM1N_API void fm1n_engine(int nuked) { opl3_nuked_on = nuked != 0; }   /* (opl3_nuked.c) */

FM1N_API uint32_t fm1n_screen(uint16_t *rgb565)
{
    uint32_t v;
    pthread_mutex_lock(&host_screen_lock);
    memcpy(rgb565, host_screen, sizeof host_screen);
    v = host_screen_version;
    pthread_mutex_unlock(&host_screen_lock);
    return v;
}

FM1N_API void fm1n_leds(uint8_t *levels)
{
    uint32_t p, r;
    memset(levels, 0, FM1_NKEY);
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++) {
            const int id = FM1_KEYMAP[r][p];
            const uint8_t m = (uint8_t)(1u << r);
            if (id < 0)
                continue;
            levels[id] = (fm1_led[p] & m) ? FM1N_LED_STRONG
                       : ((fm1_led_mid[p] | fm1_led_dim[p] | fm1_led_breath[p]) & m) ? FM1N_LED_FAINT : 0;
        }
}

FM1N_API int fm1n_error(char *out, int max)
{
    if (max > 0)
        str_cpy(out, host_error, (unsigned)max);
    return host_error[0] != 0;
}
