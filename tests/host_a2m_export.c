// Test, sur le Mac, de l'EXPORT A2M du FM-1 (firmware/compat/fm1_a2m_export.c) :
//  1. le LZH en flux : des donnees de toutes sortes, compressees puis rendues par le decodeur
//     d'AT2 (amber/import/unlzh.c, le meme qu'Amber), doivent revenir a l'identique ;
//  2. un morceau exporte : l'en-tete (tailles, CRC d'AT2), le songdata et les patterns relus.
//   cc -O1 -Ifirmware/amber/moteur/CustomReplayer -o build/host_a2m tests/host_a2m_export.c \
//      firmware/amber/import/unlzh.c && ./build/host_a2m
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "md_replayer.h"

int LZH_decompress(char *source, char *dest, int source_size, int dest_size);

static md_module_t *module;
md_module_t *md_replayer_get_module(void) { return module; }
bool md_replayer_get_instr_macro(int i, md_macro_fm_t *m, uint32_t *off) { (void)i; (void)m; (void)off; return false; }
static void fm1_wdt_feed(void) {}
static void a2m_message(char *msg, int cap, const char *t) { snprintf(msg, (size_t)cap, "%s", t); }

#include "../firmware/compat/fm1_a2m_export.c"

static int echecs;
#define VERIFIE(c, ...) do { if (!(c)) { printf("ECHEC : " __VA_ARGS__); printf("\n"); echecs++; } } while (0)

static uint8_t *lit_fichier(const char *chemin, long *n)
{
    FILE *f = fopen(chemin, "rb");
    uint8_t *d;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = malloc((size_t)*n);
    fread(d, 1, (size_t)*n, f);
    fclose(f);
    return d;
}

static void aller_retour(const char *nom, const uint8_t *src, uint32_t n)
{
    long lg;
    uint8_t *c, *d = calloc(1, n + 16);
    memset(&ex, 0, sizeof ex);
    ex.f = fopen("build/host_lzh.bin", "wb");
    ex_debut(n);
    ex_entre(src, n);
    ex_fin();
    ex_vide();
    fclose(ex.f);
    c = lit_fichier("build/host_lzh.bin", &lg);
    VERIFIE(lg == (long)ex.n, "%s : taille %ld / %u", nom, lg, ex.n);
    LZH_decompress((char *)c, (char *)d, (int)lg, (int)n);
    VERIFIE(!memcmp(src, d, n), "%s : les donnees ne reviennent pas", nom);
    printf("  %-22s %8u -> %7ld octets\n", nom, n, lg);
    free(c);
    free(d);
}

static uint32_t crc_at2(const uint8_t *p, uint32_t n, uint32_t c) { return ex_crc(c, p, n); }

int main(void)
{
    static uint8_t buf[300000];
    uint32_t i, s = 12345;
    ew = calloc(1, sizeof *ew);                   /* (l'export prend et rend le sien) */
    for (i = 0; i < 256; i++) {
        uint32_t c = i, j;
        for (j = 0; j < 8; j++)
            c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
        ex_crc_table[i] = c;
    }
    printf("1. le LZH en flux\n");
    memset(buf, 0, sizeof buf);
    aller_retour("zeros", buf, 200000);
    for (i = 0; i < sizeof buf; i++) { s = s * 1103515245u + 12345u; buf[i] = (uint8_t)(s >> 16); }
    aller_retour("hasard", buf, 100000);
    for (i = 0; i < sizeof buf; i++) { s = s * 1103515245u + 12345u; buf[i] = (s >> 20) % 7 == 0 ? (uint8_t)(s >> 8) : 0; }
    aller_retour("clairseme", buf, 245760);
    for (i = 0; i < sizeof buf; i++) { s = s * 1103515245u + 12345u; buf[i] = (uint8_t)((s >> 24) & 3); }
    aller_retour("peu de symboles", buf, 50000);
    for (i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i / 300);
    aller_retour("longues series", buf, 300000);
    buf[0] = 7;
    aller_retour("un octet", buf, 1);
    aller_retour("deux symboles", (const uint8_t *)"\x01\x01\x01\x02\x02", 5);

    printf("2. un morceau\n");
    module = calloc(1, sizeof *module);
    module->num_channels = 18;
    module->initial_tempo = 50;
    module->initial_speed = 6;
    strcpy(module->songname, "ESSAI");
    for (i = 0; i < MD_SONG_ROWS; i++) {
        int t;
        for (t = 0; t < MD_MAX_CHANNELS; t++) module->song[t][i] = MD_EMPTY;
    }
    for (i = 0; i < MD_MAX_CHAINS; i++) {
        int r;
        for (r = 0; r < MD_ROWS_PER_CHAIN; r++) { module->chains[i].rows[r].phrase = MD_EMPTY; module->chains[i].rows[r].transpose = 0; }
    }
    for (i = 0; i < MD_MAX_PHRASES; i++) {
        int r;
        for (r = 0; r < MD_ROWS_PER_PHRASE; r++) {
            md_phrase_row_t *p = &module->phrases[i].rows[r];
            memset(p, 0, sizeof *p);
            p->vel = p->cmd = p->mdcmd = MD_EMPTY;
            p->cc[0] = p->cc[1] = p->cc[2] = MD_EMPTY;
        }
    }
    for (i = 0; i < MD_MAX_INSTRUMENTS; i++) {
        snprintf(module->instr_names[i], 43, "Instrument %02u", i + 1);
        module->instruments[i].kind = MD_INSTR_KIND_OPL;
    }
    strcpy(module->instr_names[0], "BASSE");
    MD_OPL_FM(&module->instruments[0])[0] = 0x21;
    // piste 1 : chain 0 (phrase 0, puis phrase 0 transposee de +12) ; piste 3 : chain 1 ; deux lignes de SONG
    module->song[0][0] = 0; module->song[0][1] = 0; module->song[2][1] = 1;
    module->chains[0].rows[0].phrase = 0;
    module->chains[0].rows[1].phrase = 0; module->chains[0].rows[1].transpose = 12;
    module->chains[1].rows[0].phrase = 1;
    module->phrases[0].rows[0].note = 37; module->phrases[0].rows[0].instr = 1;
    module->phrases[0].rows[4].note = MD_EMPTY;
    module->phrases[0].rows[8].mdcmd = 0x0A; module->phrases[0].rows[8].mdval = 0x40;
    module->phrases[0].rows[9].cmd = 3;     /* (une commande LSDJ : ne part pas) */
    module->phrases[1].rows[2].note = 49; module->phrases[1].rows[2].instr = 1;
    {
        char msg[48];
        long lg;
        uint8_t *f, *sd = malloc(EX_SD), *pat = malloc(8 * EX_PATTERN);
        uint32_t lsd, crc;
        const bool ok = fm1_exporte_a2m("build/host_export.a2m", msg, sizeof msg);
        ew = calloc(1, sizeof *ew);
        for (i = 0; i < 256; i++) {
            uint32_t c = i, j;
            for (j = 0; j < 8; j++)
                c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
            ex_crc_table[i] = c;
        }
        VERIFIE(ok, "export : %s", msg);
        f = lit_fichier("build/host_export.a2m", &lg);
        VERIFIE(!memcmp(f, "_A2module_", 10) && f[14] == 14, "en-tete");
        VERIFIE(f[15] == 2, "2 patterns, et non %d", f[15]);
        lsd = (uint32_t)f[16] | (uint32_t)f[17] << 8 | (uint32_t)f[18] << 16 | (uint32_t)f[19] << 24;
        {
            uint32_t lb = (uint32_t)f[20] | (uint32_t)f[21] << 8 | (uint32_t)f[22] << 16 | (uint32_t)f[23] << 24, k;
            uint8_t l2[2];
            VERIFIE(84 + lsd + lb == (uint32_t)lg, "tailles : 84 + %u + %u != %ld", lsd, lb, lg);
            crc = crc_at2(f + 84, (uint32_t)lg - 84u, 0xFFFFFFFFu);
            for (k = 0; k < 17; k++) {
                const uint32_t v = (uint32_t)f[16 + 4 * k] | (uint32_t)f[17 + 4 * k] << 8;
                l2[0] = (uint8_t)v; l2[1] = (uint8_t)(v >> 8);
                crc = crc_at2(l2, 2, crc);
            }
            VERIFIE(crc == ((uint32_t)f[10] | (uint32_t)f[11] << 8 | (uint32_t)f[12] << 16 | (uint32_t)f[13] << 24), "CRC");
            LZH_decompress((char *)f + 84, (char *)sd, (int)lsd, EX_SD);
            LZH_decompress((char *)f + 84 + lsd, (char *)pat, (int)lb, 8 * EX_PATTERN);
        }
        VERIFIE(sd[0] == 5 && !memcmp(sd + 1, "ESSAI", 5), "titre");
        VERIFIE(sd[EX_NOMS] == 5 && !memcmp(sd + EX_NOMS + 1, "BASSE", 5), "nom de l'instrument 1");
        VERIFIE(sd[EX_NOMS + 43] == 0, "nom par defaut : vide");
        VERIFIE(sd[EX_INSTRUMENTS] == 0x21, "registres de l'instrument 1");
        VERIFIE(sd[EX_ORDRE] == 0 && sd[EX_ORDRE + 1] == 1 && sd[EX_ORDRE + 2] == 0x80, "ordre %d %d %d",
                sd[EX_ORDRE], sd[EX_ORDRE + 1], sd[EX_ORDRE + 2]);
        VERIFIE(sd[EX_TEMPO] == 50 && sd[EX_SPEED] == 6 && sd[EX_PISTES_SD] == 18, "tempo, speed, pistes");
        VERIFIE((sd[EX_LONGUEUR] | sd[EX_LONGUEUR + 1] << 8) == 2 * MD_ROWS_PER_PHRASE, "longueur des patterns %d",
                sd[EX_LONGUEUR] | sd[EX_LONGUEUR + 1] << 8);
        {
#define CASE(p, t, r) (pat + (p) * EX_PATTERN + ((t) * EX_LIGNES + (r)) * 6)
            const uint8_t *c = CASE(0, 0, 0);
            VERIFIE(c[0] == 37 && c[1] == 1, "pattern 0, piste 1, ligne 0 : %d %d", c[0], c[1]);
            VERIFIE(CASE(0, 0, 4)[0] == 0xFF, "key off");
            VERIFIE(CASE(0, 0, 8)[2] == 0x0A && CASE(0, 0, 8)[3] == 0x40, "FX1");
            VERIFIE(!CASE(0, 0, 9)[2] && !CASE(0, 0, 9)[4], "la commande LSDJ ne part pas");
            VERIFIE(CASE(0, 0, MD_ROWS_PER_PHRASE)[0] == 49, "TSP +12 applique : %d", CASE(0, 0, MD_ROWS_PER_PHRASE)[0]);
            VERIFIE(CASE(1, 2, 2)[0] == 49, "pattern 1, piste 3");
        }
        printf("  %ld octets, %s\n", lg, msg);
    }
    printf(echecs ? "%d ECHEC(S)\n" : "TOUT EST BON\n", echecs);
    return echecs != 0;
}
