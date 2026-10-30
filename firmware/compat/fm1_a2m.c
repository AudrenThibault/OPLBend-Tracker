// ============================================================================
//  L'import d'un module AdLib Tracker 2 (.A2M, version 14) SUR LE FM-1.
//
//  La conversion est celle de la RG350 (amber/import/a2m_import.cpp, copie) :
//  chaque position de l'ordre devient une ligne du SONG ; pour chaque piste,
//  le pattern devient un CHAIN de phrases de 16 lignes, phrases et chains
//  identiques partages ; les deux effets d'une case vont dans FX1 et FX2 tels
//  quels ; les macros FM des instruments sont reprises.
//
//  Ce qui change : la RG350 decompresse le fichier entier en memoire (le bloc
//  « songdata » fait 1,1 Mo, les patterns 245 Ko par paquet de huit), ce que
//  les 430 Ko du FM-1 ne permettent pas. Ici tout passe AU FIL DE L'EAU : le
//  LZH d'AT2 ne regarde que les 16 derniers Ko qu'il a sortis, on le
//  decompresse donc par paquets de 16 Ko et on ne garde de chaque paquet que
//  ce qui sert (les noms, les instruments, les macros, l'ordre...). Les
//  patterns se convertissent piste par piste, dans l'ordre ou ils arrivent.
//  Les numeros de phrases et de chains peuvent donc differer de la RG350 ; le
//  morceau, lui, est le meme.
//
//  L'export A2M et les .OPS restent a venir.
// ============================================================================
#include "CustomReplayer/md_replayer.h"

// ── Le LZH d'AT2, au fil de l'eau ──────────────────────────────────────────
// Le decodeur de amber/import/unlzh.c (ar002 de Haruhiko Okumura, domaine
// public), recopie : les tables sont allouees le temps de l'import, et le
// fichier se lit par 512 octets au lieu d'etre en memoire. lz_lit() sort le
// paquet suivant (16 Ko au plus) dans la fenetre, qui sert aussi de
// dictionnaire : c'est toujours la meme qu'il faut lui redonner.
#define LZ_DICBIT 14
#define LZ_DIC (1u << LZ_DICBIT)
#define LZ_MAXMATCH 256
#define LZ_THRESHOLD 2
#define LZ_NC (255 + LZ_MAXMATCH + 2 - LZ_THRESHOLD)
#define LZ_CBIT 16
#define LZ_CODE_BIT 16
#define LZ_NP (LZ_DICBIT + 1)
#define LZ_NT (LZ_CODE_BIT + 3)
#define LZ_PBIT 14
#define LZ_TBIT 15
#define LZ_NPT 64                                 /* (unlzh.c : 1 << TBIT ; il n'en sert que NT = 19) */
#define LZ_BITBUF 16

typedef struct {
    uint16_t left[2 * LZ_NC - 1], right[2 * LZ_NC - 1];
    uint8_t c_len[LZ_NC], pt_len[LZ_NPT];
    uint16_t pt_table[256], c_table[4096];
    uint8_t entree[512];
} lz_tables_t;
static lz_tables_t *lz;
static FILE *lz_f;
static uint32_t lz_reste, lz_e_pos, lz_e_n;      /* octets compresses a lire ; le tampon d'entree */
static uint16_t lz_bitbuf;
static uint32_t lz_subbitbuf, lz_blocksize, lz_i;
static int lz_bitcount, lz_j, lz_mauvais;

static uint8_t lz_octet(void)
{
    if (lz_e_pos >= lz_e_n) {
        uint32_t n = lz_reste < sizeof lz->entree ? lz_reste : (uint32_t)sizeof lz->entree;
        lz_e_n = n ? (uint32_t)fread(lz->entree, 1, n, lz_f) : 0;
        lz_reste -= n;
        lz_e_pos = 0;
        if (!lz_e_n)
            return 0;
    }
    return lz->entree[lz_e_pos++];
}

static void lz_fillbuf(int n)
{
    lz_bitbuf = (uint16_t)(lz_bitbuf << n);
    while (n > lz_bitcount) {
        lz_bitbuf |= (uint16_t)(lz_subbitbuf << (n -= lz_bitcount));
        lz_subbitbuf = lz_octet();
        lz_bitcount = 8;
    }
    lz_bitbuf |= (uint16_t)(lz_subbitbuf >> (lz_bitcount -= n));
}

static unsigned lz_getbits(int n)
{
    unsigned x = (unsigned)lz_bitbuf >> (LZ_BITBUF - n);
    lz_fillbuf(n);
    return x;
}

static void lz_make_table(int nchar, const uint8_t *bitlen, int tablebits, uint16_t *table)
{
    uint16_t count[17], weight[17], start[18], *p;
    unsigned i, k, len, ch, jutbits, avail, nextcode, mask;
    for (i = 1; i <= 16; i++)
        count[i] = 0;
    for (i = 0; i < (unsigned)nchar; i++)
        count[bitlen[i] & 31u] += bitlen[i] <= 16 ? 1 : 0;
    start[1] = 0;
    for (i = 1; i <= 16; i++)
        start[i + 1] = (uint16_t)(start[i] + (count[i] << (16 - i)));
    if (start[17] & 0xffffu)
        lz_mauvais = 1;
    jutbits = 16 - (unsigned)tablebits;
    for (i = 1; i <= (unsigned)tablebits; i++) {
        start[i] = (uint16_t)(start[i] >> jutbits);
        weight[i] = (uint16_t)(1u << (tablebits - i));
    }
    while (i <= 16) {
        weight[i] = (uint16_t)(1u << (16 - i));
        i++;
    }
    i = start[tablebits + 1] >> jutbits;
    if (i != 0) {
        k = 1u << tablebits;
        while (i != k)
            table[i++] = 0;
    }
    avail = (unsigned)nchar;
    mask = 1u << (15 - tablebits);
    for (ch = 0; ch < (unsigned)nchar; ch++) {
        if ((len = bitlen[ch]) == 0 || len > 16)
            continue;
        nextcode = start[len] + weight[len];
        if (len <= (unsigned)tablebits) {
            if (nextcode > (1u << tablebits)) {
                lz_mauvais = 1;
                return;
            }
            for (i = start[len]; i < nextcode; i++)
                table[i] = (uint16_t)ch;
        } else {
            k = start[len];
            p = &table[k >> jutbits];
            i = len - (unsigned)tablebits;
            while (i != 0) {
                if (*p == 0) {
                    if (avail >= 2 * LZ_NC - 1) {
                        lz_mauvais = 1;
                        return;
                    }
                    lz->right[avail] = lz->left[avail] = 0;
                    *p = (uint16_t)avail++;
                }
                p = (k & mask) ? &lz->right[*p] : &lz->left[*p];
                k <<= 1;
                i--;
            }
            *p = (uint16_t)ch;
        }
        start[len] = (uint16_t)nextcode;
    }
}

static void lz_read_pt_len(int nn, int nbit, int i_special)
{
    int i, c, n = (int)lz_getbits(nbit);
    unsigned mask;
    if (n == 0) {
        c = (int)lz_getbits(nbit);
        for (i = 0; i < nn; i++)
            lz->pt_len[i] = 0;
        for (i = 0; i < 256; i++)
            lz->pt_table[i] = (uint16_t)c;
        return;
    }
    if (n > LZ_NPT) {                             /* (un fichier abime : unlzh.c debordait) */
        lz_mauvais = 1;
        return;
    }
    i = 0;
    while (i < n) {
        c = lz_bitbuf >> (LZ_BITBUF - 3);
        if (c == 7) {
            mask = 1u << (LZ_BITBUF - 1 - 3);
            while (mask & lz_bitbuf) {
                mask >>= 1;
                c++;
            }
        }
        lz_fillbuf((c < 7) ? 3 : c - 3);
        lz->pt_len[i++] = (uint8_t)c;
        if (i == i_special) {
            c = (int)lz_getbits(2);
            while (--c >= 0 && i < LZ_NPT)
                lz->pt_len[i++] = 0;
        }
    }
    while (i < nn)
        lz->pt_len[i++] = 0;
    lz_make_table(nn, lz->pt_len, 8, lz->pt_table);
}

static void lz_read_c_len(void)
{
    int i, c, n = (int)lz_getbits(LZ_CBIT);
    unsigned mask;
    if (n == 0) {
        c = (int)lz_getbits(LZ_CBIT);
        for (i = 0; i < LZ_NC; i++)
            lz->c_len[i] = 0;
        for (i = 0; i < 4096; i++)
            lz->c_table[i] = (uint16_t)c;
        return;
    }
    i = 0;
    while (i < n && i < LZ_NC) {
        c = lz->pt_table[lz_bitbuf >> (LZ_BITBUF - 8)];
        if (c >= LZ_NT) {
            mask = 1u << (LZ_BITBUF - 1 - 8);
            do {
                c = (lz_bitbuf & mask) ? lz->right[c] : lz->left[c];
                mask >>= 1;
            } while (c >= LZ_NT && mask);
        }
        lz_fillbuf((int)lz->pt_len[c < LZ_NPT ? c : 0]);
        if (c <= 2) {
            if (c == 0)
                c = 1;
            else if (c == 1)
                c = (int)lz_getbits(4) + 3;
            else
                c = (int)lz_getbits(LZ_CBIT) + 20;
            while (--c >= 0 && i < LZ_NC)
                lz->c_len[i++] = 0;
        } else
            lz->c_len[i++] = (uint8_t)(c - 2);
    }
    while (i < LZ_NC)
        lz->c_len[i++] = 0;
    lz_make_table(LZ_NC, lz->c_len, 12, lz->c_table);
}

static unsigned lz_decode_c(void)
{
    unsigned j, mask;
    if (lz_blocksize == 0) {
        lz_blocksize = lz_getbits(16);
        if (lz_blocksize == 0)
            return LZ_NC;                         /* la fin */
        lz_read_pt_len(LZ_NT, LZ_TBIT, 3);
        lz_read_c_len();
        lz_read_pt_len(LZ_NP, LZ_PBIT, -1);
        if (lz_mauvais)
            return LZ_NC;
    }
    lz_blocksize--;
    j = lz->c_table[lz_bitbuf >> (LZ_BITBUF - 12)];
    if (j >= LZ_NC) {
        mask = 1u << (LZ_BITBUF - 1 - 12);
        do {
            j = (lz_bitbuf & mask) ? lz->right[j] : lz->left[j];
            mask >>= 1;
        } while (j >= LZ_NC && mask);
    }
    lz_fillbuf((int)lz->c_len[j < LZ_NC ? j : 0]);
    return j;
}

static unsigned lz_decode_p(void)
{
    unsigned j = lz->pt_table[lz_bitbuf >> (LZ_BITBUF - 8)], mask;
    if (j >= LZ_NP) {
        mask = 1u << (LZ_BITBUF - 1 - 8);
        do {
            j = (lz_bitbuf & mask) ? lz->right[j] : lz->left[j];
            mask >>= 1;
        } while (j >= LZ_NP && mask);
    }
    lz_fillbuf((int)lz->pt_len[j < LZ_NPT ? j : 0]);
    if (j != 0)
        j = (1u << (j - 1)) + lz_getbits((int)(j - 1));
    return j;
}

/* Un bloc compresse de `n` octets a la position courante de `f` ; rend sa taille decompressee. */
static uint32_t lz_debut(FILE *f, uint32_t n)
{
    uint8_t t[5];
    lz_f = f;
    lz_reste = n;
    lz_e_pos = lz_e_n = 0;
    for (int k = 0; k < 5; k++)
        t[k] = lz_octet();                        /* l'octet « ultra », puis la taille, petit-boutiste */
    lz_bitbuf = 0;
    lz_subbitbuf = 0;
    lz_bitcount = 0;
    lz_fillbuf(LZ_BITBUF);
    lz_blocksize = 0;
    lz_j = 0;
    lz_i = 0;
    lz_mauvais = 0;
    return (uint32_t)t[1] | (uint32_t)t[2] << 8 | (uint32_t)t[3] << 16 | (uint32_t)t[4] << 24;
}

/* le paquet suivant, `count` octets (16 Ko au plus), dans `buf` (la fenetre, toujours la meme) */
static unsigned lz_lit(unsigned count, uint8_t *buf)
{
    unsigned r = 0, c;
    while (--lz_j >= 0) {
        buf[r] = buf[lz_i];
        lz_i = (lz_i + 1) & (LZ_DIC - 1);
        if (++r == count)
            return r;
    }
    for (;;) {
        c = lz_decode_c();
        if (c == LZ_NC)
            return r;
        if (c <= 255) {
            buf[r] = (uint8_t)c;
            if (++r == count)
                return r;
        } else {
            lz_j = (int)(c - (255 + 1 - LZ_THRESHOLD));
            lz_i = (r - lz_decode_p() - 1) & (LZ_DIC - 1);
            while (--lz_j >= 0) {
                buf[r] = buf[lz_i];
                lz_i = (lz_i + 1) & (LZ_DIC - 1);
                if (++r == count)
                    return r;
            }
        }
    }
}

// ── Les champs du « songdata », au fil de l'eau ────────────────────────────
// Emplacements dans le bloc songdata d'un A2M v9-14 (Amber, parse_songdata).
#define A2M_SONGDATA 1138338u
#define A2M_NOMS 86u
#define A2M_INSTRUMENTS 11051u
#define A2M_MACROS 14621u
#define A2M_ORDRE 1124381u                         /* ... tempo, speed, longueur, pistes, macro speedup */
#define A2M_ORDRE_N 136u
#define A2M_COLONNES_OFF 1130042u
#define A2M_FIN 1138335u                           /* lignes par temps, finetune du tempo */
#define A2M_PISTES 20
#define A2M_LIGNES 256

/* Une suite de N fiches de R octets a partir de `base` : chaque fiche complete part a `fini`. */
typedef struct {
    uint32_t base, r, n;
    uint8_t *fiche;
    void (*fini)(uint32_t k, const uint8_t *fiche);
} a2m_suite_t;

static void a2m_recolte(a2m_suite_t *s, const uint8_t *p, uint32_t off, uint32_t n)
{
    const uint32_t fin = s->base + s->r * s->n;
    uint32_t a = off > s->base ? off : s->base, b = off + n < fin ? off + n : fin;
    while (a < b) {
        const uint32_t dans = (a - s->base) % s->r, k = (a - s->base) / s->r;
        uint32_t m = s->r - dans;
        if (m > b - a)
            m = b - a;
        memcpy(s->fiche + dans, p + (a - off), m);
        a += m;
        if (dans + m == s->r)
            s->fini(k, s->fiche);
    }
}

static md_module_t *a2m_m;
static md_macro_fm_t *a2m_macro;                 /* (une fiche de macro ; sert aussi a reposer ses colonnes) */
static uint8_t a2m_ordre[A2M_ORDRE_N], a2m_fin[3];

static void a2m_pascal(char *dest, const uint8_t *src)
{
    const int n = src[0] > 42 ? 42 : src[0];
    memcpy(dest, src + 1, (size_t)n);
    dest[n] = 0;
}

static void a2m_fini_titre(uint32_t k, const uint8_t *f) { a2m_pascal(k ? a2m_m->composer : a2m_m->songname, f); }
static void a2m_fini_nom(uint32_t k, const uint8_t *f) { a2m_pascal(a2m_m->instr_names[k], f); }
static void a2m_fini_instrument(uint32_t k, const uint8_t *s)
{
    md_instr_t *ins = &a2m_m->instruments[k];
    ins->kind = MD_INSTR_KIND_OPL;
    memcpy(MD_OPL_FM(ins), s, MD_OPL_OCTETS);
    ins->panning = s[11] == 1 ? 0 : (s[11] == 2 ? 255 : 128);
    ins->fine_tune = (int8_t)s[12];               /* le F-TUNE d'AT2, tel quel */
}
static void a2m_fini_macro(uint32_t k, const uint8_t *f)
{
    const md_macro_fm_t *mac = (const md_macro_fm_t *)(const void *)f;
    md_replayer_set_instr_macro((int)k + 1, mac->length ? mac : NULL, 0);   /* (colonnes : plus loin) */
}
static void a2m_fini_colonnes(uint32_t k, const uint8_t *f)
{
    uint32_t off = 0, b;
    for (b = 0; b < 28u; b++)
        if (f[b])
            off |= 1u << b;
    if (off && md_replayer_get_instr_macro((int)k + 1, a2m_macro, NULL))
        md_replayer_set_instr_macro((int)k + 1, a2m_macro, off);
}
static void a2m_fini_copie(uint32_t k, const uint8_t *f) { (void)k; (void)f; }   /* (la fiche est la destination) */

// ── Les patterns : une piste a la fois ──────────────────────────────────────
static uint8_t a2m_utilise[128];                  /* les patterns que l'ordre joue */
static uint8_t a2m_chain_de[128][A2M_PISTES];     /* (pattern, piste) -> chain, MD_EMPTY : aucun */
static int a2m_bloc, a2m_nb_patterns, a2m_longueur, a2m_pistes, a2m_hop, a2m_nb_phrases, a2m_nb_chains, a2m_plein;

static uint8_t a2m_note(uint8_t n)                /* 1-96 comme le tracker ; les notes « fixes » jouees */
{
    if (n == 0) return 0;
    if (n <= 96) return n;
    if (n >= 0x91 && n <= 0x90 + 96) return (uint8_t)(n - 0x90);
    return MD_EMPTY;
}

static void a2m_fini_piste(uint32_t k, const uint8_t *s)   /* s : les 256 lignes de 6 octets d'une piste */
{
    const int p = a2m_bloc * 8 + (int)(k / A2M_PISTES), c = (int)(k % A2M_PISTES);
    const int nb_ph = (a2m_longueur + MD_ROWS_PER_PHRASE - 1) / MD_ROWS_PER_PHRASE;
    uint8_t ids[MD_ROWS_PER_CHAIN];
    int ph, id, x;
    if (p >= a2m_nb_patterns || p >= 128 || !a2m_utilise[p] || c >= a2m_pistes || a2m_plein)
        return;
    for (ph = 0; ph < nb_ph && ph < MD_ROWS_PER_CHAIN; ph++) {
        md_phrase_t phrase;
        for (int r = 0; r < MD_ROWS_PER_PHRASE; r++) {
            md_phrase_row_t *pr = &phrase.rows[r];
            const int ligne = ph * MD_ROWS_PER_PHRASE + r;
            memset(pr, 0, sizeof *pr);
            pr->vel = MD_EMPTY;
            pr->cmd = MD_EMPTY;
            for (x = 0; x < MD_PHRASE_FX; x++) {
                *md_fx_type(pr, x) = MD_EMPTY;
                *md_fx_val(pr, x) = 0;
            }
            if (ligne < a2m_longueur) {
                const uint8_t *e = s + ligne * 6;
                pr->note = a2m_note(e[0]);
                pr->instr = e[1];
                for (x = 0; x < 2; x++) {
                    const uint8_t t = e[2 + 2 * x], v = e[3 + 2 * x];
                    if ((t == 0 && v == 0) || t > MD_EFF_GLOBAL_FSLIDE_DOWN)
                        continue;                 /* case vide, ou inconnu du moteur */
                    *md_fx_type(pr, x) = t;
                    *md_fx_val(pr, x) = v;
                }
            } else if (ligne == a2m_longueur && a2m_hop >= 0) {
                pr->cmd = (uint8_t)a2m_hop;       /* un pattern plus court qu'un multiple de 16 : H00 */
                pr->cmdval = 0;
            }
        }
        for (id = 0; id < a2m_nb_phrases; id++)   /* une phrase identique : partagee */
            if (!memcmp(&a2m_m->phrases[id], &phrase, sizeof phrase))
                break;
        if (id == a2m_nb_phrases) {
            if (a2m_nb_phrases >= MD_MAX_PHRASES) {
                a2m_plein = 1;
                return;
            }
            a2m_m->phrases[a2m_nb_phrases++] = phrase;
        }
        ids[ph] = (uint8_t)id;
    }
    for (id = 0; id < a2m_nb_chains; id++) {      /* un chain identique : partage */
        for (ph = 0; ph < nb_ph && ph < MD_ROWS_PER_CHAIN; ph++)
            if (a2m_m->chains[id].rows[ph].phrase != ids[ph])
                break;
        if (ph == nb_ph || ph == MD_ROWS_PER_CHAIN)
            break;
    }
    if (id == a2m_nb_chains) {
        if (a2m_nb_chains >= MD_MAX_CHAINS) {
            a2m_plein = 1;
            return;
        }
        for (ph = 0; ph < nb_ph && ph < MD_ROWS_PER_CHAIN; ph++) {
            a2m_m->chains[id].rows[ph].phrase = ids[ph];
            a2m_m->chains[id].rows[ph].transpose = 0;
        }
        a2m_nb_chains++;
    }
    a2m_chain_de[p][c] = (uint8_t)id;
}

static void a2m_message(char *msg, int cap, const char *texte)
{
    if (msg && cap > 0)
        str_cpy(msg, texte, (unsigned)cap);
}

/* Le fichier, bloc compresse par bloc compresse : chaque paquet de 16 Ko aux suites `s`. */
static int a2m_bloc_lz(FILE *f, uint32_t pos, uint32_t n, uint8_t *fenetre, a2m_suite_t *s, int ns, uint32_t total)
{
    uint32_t off = 0, taille;
    if (fseek(f, (long)pos, SEEK_SET))
        return -1;
    taille = lz_debut(f, n);
    if (taille > total)
        taille = total;
    while (off < taille && !lz_mauvais) {
        const uint32_t m = lz_lit(taille - off > LZ_DIC ? LZ_DIC : taille - off, fenetre);
        if (!m)
            break;
        for (int k = 0; k < ns; k++)
            a2m_recolte(&s[k], fenetre, off, m);
        off += m;
        fm1_wdt_feed();                           /* (un import prend plusieurs secondes) */
    }
    if (off < total) {                            /* ce qui manque : des zeros, comme la RG350 */
        memset(fenetre, 0, LZ_DIC);
        while (off < total) {
            const uint32_t m = total - off > LZ_DIC ? LZ_DIC : total - off;
            for (int k = 0; k < ns; k++)
                a2m_recolte(&s[k], fenetre, off, m);
            off += m;
        }
    }
    return lz_mauvais ? -1 : 0;
}

bool md_importe_a2m(const char *chemin, char *msg, int cap)
{
    uint8_t h[84], *fenetre = 0, *fiche = 0, *colonnes = 0;
    uint32_t long_sd, long_blocs[16], pos;
    int k, ok = 0;
    FILE *f = fopen(chemin, "rb");
    if (!f) {
        a2m_message(msg, cap, "FICHIER ILLISIBLE");
        return false;
    }
    if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "_A2module_", 10)) {
        fclose(f);
        a2m_message(msg, cap, "PAS UN FICHIER A2M");
        return false;
    }
    if (h[14] != 14) {
        fclose(f);
        a2m_message(msg, cap, "A2M: VERSION 14 SEULEMENT");
        return false;
    }
    a2m_nb_patterns = h[15];
    long_sd = (uint32_t)h[16] | (uint32_t)h[17] << 8 | (uint32_t)h[18] << 16 | (uint32_t)h[19] << 24;
    for (k = 0; k < 16; k++)
        long_blocs[k] = (uint32_t)h[20 + 4 * k] | (uint32_t)h[21 + 4 * k] << 8 |
                        (uint32_t)h[22 + 4 * k] << 16 | (uint32_t)h[23 + 4 * k] << 24;

    md_replayer_stop();
    md_replayer_new_empty();
    a2m_m = md_replayer_get_module();
    lz = (lz_tables_t *)malloc(sizeof *lz);
    fenetre = (uint8_t *)malloc(LZ_DIC);
    fiche = (uint8_t *)malloc(A2M_PISTES * A2M_LIGNES * 6 > sizeof(md_macro_fm_t) ? A2M_PISTES * A2M_LIGNES * 6 : sizeof(md_macro_fm_t));
    a2m_macro = (md_macro_fm_t *)malloc(sizeof(md_macro_fm_t));
    colonnes = (uint8_t *)malloc(43);
    if (!a2m_m || !lz || !fenetre || !fiche || !a2m_macro || !colonnes) {
        a2m_message(msg, cap, "MEMOIRE PLEINE");
        goto fin;
    }

    // ── Le songdata : on n'en garde que ce qui sert ────────────────────────
    {
        uint8_t petite[43];                       /* (titres, noms, instruments, colonnes : fiches courtes) */
        (void)petite;
        a2m_suite_t s[] = {
            {0, 43, 2, colonnes, a2m_fini_titre},
            {A2M_NOMS, 43, MD_MAX_INSTRUMENTS, colonnes, a2m_fini_nom},
            {A2M_INSTRUMENTS, 14, MD_MAX_INSTRUMENTS, colonnes, a2m_fini_instrument},
            {A2M_MACROS, sizeof(md_macro_fm_t), MD_MAX_INSTRUMENTS, fiche, a2m_fini_macro},
            {A2M_ORDRE, A2M_ORDRE_N, 1, a2m_ordre, a2m_fini_copie},
            {A2M_COLONNES_OFF, 28, MD_MAX_INSTRUMENTS, colonnes, a2m_fini_colonnes},
            {A2M_FIN, 3, 1, a2m_fin, a2m_fini_copie},
        };
        if (a2m_bloc_lz(f, 84, long_sd, fenetre, s, (int)(sizeof s / sizeof s[0]), A2M_SONGDATA)) {
            a2m_message(msg, cap, "A2M ABIME");
            goto fin;
        }
    }
    {
        const uint8_t *o = a2m_ordre;             /* A2M_ORDRE : l'ordre (128), puis les reglages */
        const int tempo = o[128], speed = o[129], macro_speedup = o[134] | o[135] << 8;
        a2m_longueur = o[131] | o[132] << 8;
        if (a2m_longueur < 1 || a2m_longueur > A2M_LIGNES)
            a2m_longueur = 64;
        a2m_pistes = o[133];
        if (a2m_pistes < 1 || a2m_pistes > A2M_PISTES)
            a2m_pistes = 18;
        if (a2m_pistes > MD_PISTES_OPL)
            a2m_pistes = MD_PISTES_OPL;
        a2m_m->initial_tempo = (uint8_t)(tempo ? tempo : 50);
        a2m_m->initial_speed = (uint8_t)(speed ? speed : 6);
        a2m_m->macro_speedup = (uint16_t)(macro_speedup ? macro_speedup : 1);
        a2m_m->bpm_rows_per_beat = a2m_fin[0] ? a2m_fin[0] : 4;
        a2m_m->bpm_tempo_finetune = (int16_t)(a2m_fin[1] | a2m_fin[2] << 8);
        a2m_m->num_channels = MD_PISTES_OPL;
        /* La longueur des patterns, gardee telle quelle pour le .A2M (un enregistrement doit la
           rendre a AT2) ; le tracker n'y touche pas : a la lecture, ce sont les H00 poses plus bas,
           comme la RG350. */
        a2m_m->patt_len = (uint16_t)a2m_longueur;
    }
    // L'ordre s'arrete au premier saut (0x80 et plus) : le SONG reboucle de lui-meme.
    memset(a2m_utilise, 0, sizeof a2m_utilise);
    memset(a2m_chain_de, MD_EMPTY, sizeof a2m_chain_de);
    {
        int n = 0;
        for (k = 0; k < 128 && n < MD_SONG_ROWS; k++) {
            const uint8_t o = a2m_ordre[k];
            if (o >= 0x80)
                break;
            if (o < a2m_nb_patterns) {
                a2m_utilise[o] = 1;
                n++;
            }
        }
        if (!n) {
            a2m_message(msg, cap, "A2M: ORDRE VIDE");
            goto fin;
        }
    }

    // ── Les patterns : huit par bloc compresse, piste par piste ────────────
    a2m_hop = -1;
    for (k = 0; k < md_table_cmd_count(); k++)
        if (md_table_cmd_letter(k) == 'H') {
            a2m_hop = k;
            break;
        }
    a2m_nb_phrases = a2m_nb_chains = a2m_plein = 0;
    pos = 84 + long_sd;
    for (a2m_bloc = 0; a2m_bloc < (a2m_nb_patterns + 7) / 8 && a2m_bloc < 16; a2m_bloc++) {
        a2m_suite_t s = {0, A2M_LIGNES * 6, 8 * A2M_PISTES, fiche, a2m_fini_piste};
        if (!long_blocs[a2m_bloc])
            continue;
        if (a2m_bloc_lz(f, pos, long_blocs[a2m_bloc], fenetre, &s, 1, 8u * A2M_PISTES * A2M_LIGNES * 6))
            break;
        pos += long_blocs[a2m_bloc];
    }

    // ── Le SONG : une ligne par position de l'ordre ────────────────────────
    {
        int ligne = 0;
        for (k = 0; k < 128 && ligne < MD_SONG_ROWS; k++) {
            const uint8_t o = a2m_ordre[k];
            if (o >= 0x80)
                break;
            if (o >= a2m_nb_patterns)
                continue;
            for (int c = 0; c < a2m_pistes; c++)
                a2m_m->song[c][ligne] = a2m_chain_de[o][c];
            ligne++;
        }
        md_replayer_set_tempo(a2m_m->initial_tempo);
        md_replayer_set_speed(a2m_m->initial_speed);
        if (a2m_plein)
            snprintf(msg, (size_t)cap, "A2M TROP GROS: %d CHAINS %d PHRASES", a2m_nb_chains, a2m_nb_phrases);
        else
            snprintf(msg, (size_t)cap, "A2M: %d PATTERNS, %d CHAINS, %d PHRASES", ligne, a2m_nb_chains, a2m_nb_phrases);
    }
    ok = 1;
fin:
    fclose(f);
    free(lz);
    lz = 0;
    free(fenetre);
    free(fiche);
    free(a2m_macro);
    a2m_macro = 0;
    free(colonnes);
    return ok != 0;
}

// L'export A2M et les .OPS : pas encore sur le FM-1.
const uint8_t *a2m_songdata_origine(void) { return 0; }
void a2m_oublie_origine(void) {}
bool md_enregistre_a2m(const char *c, char *msg, int cap) { (void)c; a2m_message(msg, cap, "A2M SAVE: NOT YET ON FM-1"); return false; }
bool md_charge_ops(const char *c, char *msg, int cap) { (void)c; (void)msg; (void)cap; return false; }
