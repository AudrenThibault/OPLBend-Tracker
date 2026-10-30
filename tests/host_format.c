/* Host check of the FM-1 changes to md_replayer.c: the streamed save writes the same bytes as
 * md_replayer_save_mem, and the streamed load reads them back.  tests/host_format.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
int enterCriticalSection(void) { return 0; }
void leaveCriticalSection(int a) { (void)a; }
void irqSet(int i, void (*f)(void)) { (void)i; (void)f; }
void irqEnable(int i) { (void)i; }
volatile unsigned short rg_timer_cr[4], rg_timer_data[4];
void rg_regle_periode_tick(double s) { (void)s; }
void opl_voix_init(void) {}
void opl_voix_instrument(int v, const uint8_t f[11], int p) { (void)v; (void)f; (void)p; }
void opl_voix_hauteur(int v, int h, bool a) { (void)v; (void)h; (void)a; }
void opl_voix_volume(int v, int a, int b) { (void)v; (void)a; (void)b; }
void opl_voix_relache(int v) { (void)v; }
void opl_voix_profondeurs(int a, int b) { (void)a; (void)b; }
void ds_voix_init(void) {}
void ds_voix_joue(int v, const uint8_t *d, uint32_t i, int b, int32_t l, uint32_t h, int vo, int p) {}
void ds_voix_freq(int v, uint32_t h) {}
void ds_voix_disto(int v, int d) {}
void ds_voix_vol(int v, int a, int p) {}
void ds_voix_stop(int v) {}
bool ds_voix_joue_encore(int v) { return false; }
void ds_voix_envoie(void) {}
bool ds_voix_lit_zone(const void *d, uint32_t o) { return false; }
void ds_voix_regle_retard(int ms) {}
void ds_voix_retard_actif(bool a) {}
void ds_voix_purge(void) {}
bool xt_midi_init(void) { return false; }
bool xt_midi_present(void) { return false; }
bool xt_midi_connecte(void) { return false; }
uint32_t xt_midi_perdus(void) { return 0; }
void xt_midi_send(uint8_t a, uint8_t b, uint8_t c) {}
void xt_midi_service(void) {}
void xt_midi_irq(void) {}
#include "CustomReplayer/md_replayer.h"
int main(void)
{
    md_replayer_init(0);
    md_replayer_new_empty();
    /* some content, so the comparison means something */
    md_module_t *m = md_replayer_get_module();
    if (!m) { puts("no module accessor"); return 2; }
    for (int i = 0; i < 40; i++) {
        m->song[i % 18][i] = (uint8_t)(i * 3);
        m->chains[i].rows[i % 16].phrase = (uint8_t)i;
        m->chains[i].rows[(i + 3) % 16].transpose = (int8_t)(i - 20);
        m->phrases[i].rows[i % 16].note = (uint8_t)(1 + i);
        m->phrases[i].rows[i % 16].instr = (uint8_t)i;
        m->phrases[i].rows[i % 16].cmd = 'C';
        m->instruments[i].kind = (uint8_t)(i % 3); m->instruments[i].pcm_volume = (uint8_t)(0x55 + i); m->instruments[i].kick_drop = (uint8_t)i;
        snprintf(m->instr_names[i], 43, "INSTR %d", i);
        m->tables[i % 32].rows[i % 16].vol = (uint8_t)i;
    }
    strcpy(m->songname, "TEST SONG");
    uint32_t n = 0;
    uint8_t *a = md_replayer_save_mem(&n);
    if (!md_replayer_save_file("build/host_format.xt")) { puts("save failed"); return 1; }
    FILE *f = fopen("build/host_format.xt", "rb");
    uint8_t *b = malloc(n + 65536);
    size_t lb = fread(b, 1, n + 65536, f);
    fclose(f);
    printf("save_mem %u bytes, streamed file %zu bytes, first %u bytes %s\n", n, lb, n,
           lb >= n && memcmp(a, b, n) == 0 ? "IDENTICAL" : "DIFFERENT");
    for (uint32_t i = 0; i < n && i < lb; i++) if (a[i] != b[i]) { printf("first difference at %u\n", i); break; }
    md_replayer_new_empty();
    if (!md_replayer_load_file("build/host_format.xt")) { puts("load failed"); return 1; }
    uint32_t n2 = 0;
    uint8_t *c = md_replayer_save_mem(&n2);
    printf("reload: %s\n", n2 == n && memcmp(a, c, n) == 0 ? "SAME SONG" : "CHANGED");
    { FILE *o = fopen(getenv("DUMP") ? getenv("DUMP") : "/dev/null", "wb"); fwrite(c, 1, n2, o); fclose(o); }
    return 0;
}
