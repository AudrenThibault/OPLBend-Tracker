// ============================================================================
//  Un petit systeme de fichiers dans la flash du FM-1, pour les fopen /
//  opendir / stat du tracker (qui, sur la DS, parlait a la carte SD).
//
//  Zone : FL_DATA (0x97000 .. 0xE0000), soit 73 secteurs de 4 Ko.
//    - deux secteurs de REPERTOIRE (A et B) : une sauvegarde va dans celui qui
//      ne porte pas la copie la plus recente, son en-tete (avec un numero de
//      sequence et une CRC) en DERNIER ; une coupure de courant laisse donc
//      toujours l'ancien repertoire en place ;
//    - les autres secteurs portent les fichiers, chacun d'un seul tenant.
//  Un fichier est COMPRESSE par blocs de 4 Ko, suivis d'une table des blocs.
//  On lit donc n'importe ou dans un fichier en ne decompressant qu'un bloc.
//  Les blocs sont en LZ (des copies de ce qui precede dans le bloc) : un .XT
//  de 214 Ko y tient en 8 Ko environ. Les fichiers ecrits avant (et les
//  morceaux livres avec le firmware) sont en PackBits, qui n'en tirait que
//  88 Ko : le bit FS_LZ de nb_sect dit lequel.
//  Un dossier n'est qu'un prefixe de nom (« Songs/xxx.XT ») ; mkdir en note
//  un vide. Les noms se comparent sans tenir compte de la casse, comme FAT.
//  Les fichiers .LOG (le journal du tracker) ne vont pas en flash : ils
//  finissent dans l'anneau du journal en RAM, pour ne pas user la flash.
// ============================================================================
#define FS_BASE     0x00097000u
#define FS_FIN      0x000E0000u
#define FS_SECT     4096u
#define FS_DONNEES  (FS_BASE + 2u * FS_SECT)
#define FS_NB_SECT  ((FS_FIN - FS_DONNEES) / FS_SECT)
#define FS_MAGIE    0x53464D46u                   /* "FMFS" */
#define FS_NOM      52u
#define FS_NB_ENT   63u
#define FS_DOSSIER  0xFFFFFFFFu                   /* taille d'une entree qui est un dossier */
#define FS_BLOC     4096u                         /* bloc compresse, en octets de fichier */
#define FS_COMP_MAX (FS_BLOC + FS_BLOC / 8u + 16u)   /* (LZ, au pire : un bit de plus par octet) */
#define FS_LZ       0x8000u                       /* nb_sect : le fichier est en LZ (sinon PackBits) */
#define FS_NSECT(e) ((uint32_t)(e)->nb_sect & 0x7FFFu)

typedef struct {
    char nom[FS_NOM];
    uint16_t premier, nb_sect;                    /* secteurs, depuis FS_DONNEES */
    uint32_t taille;                              /* octets du fichier, FS_DOSSIER = dossier */
    uint32_t index;                               /* ou commence la table des blocs, dans l'etendue */
} fs_ent_t;
typedef struct { uint32_t magie, seq, rsv, crc; fs_ent_t ent[FS_NB_ENT]; } fs_rep_t;
_Static_assert(sizeof(fs_rep_t) <= FS_SECT, "le repertoire tient dans un secteur");
_Static_assert(FS_FIN <= FL_DATA_HI && FS_BASE >= FL_DATA_LO, "dans FL_DATA");

static fs_rep_t fs_rep;

/* Pourquoi la derniere ecriture a echoue (SAVE FAILED le dit en gros) */
enum { FS_E_OK, FS_E_FLASH, FS_E_FICHIERS, FS_E_MEMOIRE, FS_E_PLACE, FS_E_ECRITURE, FS_E_REPERTOIRE };
static uint8_t fs_err;
const char *fm1_fs_erreur(void)
{
    static const char *const t[] = {"", "NO FLASH", "TOO MANY FILES OPEN", "MEMORY FULL", "FLASH FULL",
                                     "FLASH WRITE ERROR", "TOO MANY FILES"};
    return t[fs_err < 7u ? fs_err : 0];
}
int fm1_fs_plein(void) { return fs_err == FS_E_PLACE; }
void fm1_fs_raz_erreur(void) { fs_err = FS_E_OK; }

/* Les morceaux livres avec le firmware (tools/rom_songs.py) : en lecture seule, dans OPLTRACKER/Songs ; un
   fichier du meme nom enregistre en flash passe devant. */
#include "fm1_rom.h"
static int rom_cherche(const char *nom)
{
    uint32_t i;
    for (i = 0; i < FM1_ROM_NB; i++)
        if (!strcasecmp(fm1_rom[i].nom, nom))
            return (int)i;
    return -1;
}
static int8_t fs_cote = -1;                       /* le secteur (0 A, 1 B) de la copie en place */
static uint8_t fs_comp[FS_COMP_MAX] __attribute__((aligned(4)));              /* un bloc compresse, en transit */

/* ── PackBits ─────────────────────────────────────────────────────────────── */
static uint32_t pb_comprime(const uint8_t *s, uint32_t n, uint8_t *d)
{
    uint32_t i = 0, o = 0;
    while (i < n) {
        uint32_t r = 1;
        while (i + r < n && r < 129u && s[i + r] == s[i])
            r++;
        if (r >= 2u) {                            /* une repetition : 128 + (r - 2), l'octet */
            d[o++] = (uint8_t)(126u + r);
            d[o++] = s[i];
            i += r;
        } else {                                  /* des litteraux, jusqu'a la prochaine repetition */
            uint32_t l = 0, t = o++;
            while (i < n && l < 128u && !(i + 1u < n && s[i + 1u] == s[i])) {
                d[o++] = s[i++];
                l++;
            }
            if (!l) {                             /* (une paire isolee en fin de bloc) */
                d[o++] = s[i++];
                l = 1;
            }
            d[t] = (uint8_t)(l - 1u);
        }
    }
    return o;
}

/* ── LZ ─────────────────────────────────────────────────────────────────────
   Par groupes : un octet de drapeaux (bit 0 d'abord), puis huit elements : 0 un octet tel quel, 1 une
   copie : o0, (o1 << 4) | l, avec la distance (o1 o0) + 1 (jusqu'a 4096) et la longueur l + 3 (3..17) ;
   l = 15 : un octet de plus, la longueur 18 + cet octet (jusqu'a 273). */
#define LZ_TETES 1024u
#define LZ_CHAINE 32u
static uint32_t lz_hache(const uint8_t *p) { return ((uint32_t)p[0] << 6 ^ (uint32_t)p[1] << 3 ^ p[2]) & (LZ_TETES - 1u); }

static uint32_t lz_comprime(const uint8_t *s, uint32_t n, uint8_t *d, int16_t *tables)
{
    int16_t *tete = tables, *prec = tables + LZ_TETES;    /* (prec : FS_BLOC entrees) */
    uint32_t i = 0, o = 0, k, drap = 0, nb = 8;
    for (k = 0; k < LZ_TETES; k++)
        tete[k] = -1;
    while (i < n) {
        uint32_t meilleur = 0, dist = 0;
        if (nb == 8) {                            /* un nouveau groupe : son octet de drapeaux */
            drap = o++;
            d[drap] = 0;
            nb = 0;
        }
        if (i + 3u <= n) {
            const uint32_t h = lz_hache(s + i);
            int32_t j = tete[h];
            uint32_t tours = 0;
            while (j >= 0 && tours++ < LZ_CHAINE) {
                uint32_t l = 0;
                const uint32_t max = n - i < 273u ? n - i : 273u;
                while (l < max && s[(uint32_t)j + l] == s[i + l])
                    l++;
                if (l > meilleur) {
                    meilleur = l;
                    dist = i - (uint32_t)j;
                    if (l == max)
                        break;
                }
                j = prec[j];
            }
        }
        if (meilleur >= 3u) {
            const uint32_t c = dist - 1u;
            d[drap] |= (uint8_t)(1u << nb);
            d[o++] = (uint8_t)c;
            if (meilleur >= 18u) {
                d[o++] = (uint8_t)((c >> 8) << 4 | 15u);
                d[o++] = (uint8_t)(meilleur - 18u);
            } else
                d[o++] = (uint8_t)((c >> 8) << 4 | (meilleur - 3u));
        } else {
            meilleur = 1;
            d[o++] = s[i];
        }
        nb++;
        for (k = 0; k < meilleur; k++, i++)       /* chaque position passee entre dans les chaines */
            if (i + 3u <= n) {
                const uint32_t h = lz_hache(s + i);
                prec[i] = tete[h];
                tete[h] = (int16_t)i;
            }
    }
    return o;
}

static uint32_t lz_decomprime(const uint8_t *s, uint32_t n, uint8_t *d, uint32_t max)
{
    uint32_t i = 0, o = 0, nb = 8, drap = 0;
    while (i < n && o < max) {
        if (nb == 8) {
            drap = s[i++];
            nb = 0;
            continue;
        }
        if ((drap >> nb++) & 1u) {
            uint32_t c, l, dist;
            if (i + 2u > n)
                break;
            c = s[i] | (uint32_t)(s[i + 1] >> 4) << 8;
            l = s[i + 1] & 15u;
            i += 2;
            if (l == 15u) {
                if (i >= n)
                    break;
                l = 18u + s[i++];
            } else
                l += 3u;
            dist = c + 1u;
            if (dist > o)
                break;                            /* (abime) */
            while (l-- && o < max) {
                d[o] = d[o - dist];
                o++;
            }
        } else
            d[o++] = s[i++];
    }
    return o;
}

static uint32_t pb_decomprime(const uint8_t *s, uint32_t n, uint8_t *d, uint32_t max)
{
    uint32_t i = 0, o = 0;
    while (i < n && o < max) {
        uint32_t h = s[i++];
        if (h < 128u) {
            uint32_t l = h + 1u;
            while (l-- && i < n && o < max)
                d[o++] = s[i++];
        } else {
            uint32_t r = h - 126u;
            uint8_t v = i < n ? s[i++] : 0;
            while (r-- && o < max)
                d[o++] = v;
        }
    }
    return o;
}

/* ── Le repertoire ─────────────────────────────────────────────────────────── */
static uint32_t fs_crc(const fs_rep_t *r) { return st_crc32(r->ent, sizeof r->ent); }

static void fs_monte(void)
{
    fs_rep_t *lu_p = (fs_rep_t *)(void *)fs_comp;   /* (le tampon de compression sert de place) */
#define lu (*lu_p)
    uint32_t seq = 0, k;
    fs_cote = -1;
    memset(&fs_rep, 0, sizeof fs_rep);
    if (!flash_ok)
        return;
    for (k = 0; k < 2u; k++) {
        if (st_read(FS_BASE + k * FS_SECT, &lu, sizeof lu))
            continue;
        if (lu.magie != FS_MAGIE || lu.crc != fs_crc(&lu))
            continue;
        if (fs_cote < 0 || (int32_t)(lu.seq - seq) > 0) {
            fs_rep = lu;
            seq = lu.seq;
            fs_cote = (int8_t)k;
        }
    }
#undef lu
}

static int fs_ecrit_rep(void)
{
    uint32_t k = fs_cote == 0 ? 1u : 0u, base = FS_BASE + k * FS_SECT;
    fs_rep_t *r = &fs_rep;
    int rc;
    r->magie = FS_MAGIE;
    r->seq++;
    r->crc = fs_crc(r);
    fm1_wdt_feed();
    rc = st_erase(base);
    if (!rc)
        rc = st_prog(base + 16u, (const uint8_t *)r + 16u, sizeof *r - 16u);   /* les entrees */
    if (!rc)
        rc = st_prog(base, r, 16u);                                           /* l'en-tete : en dernier */
    if (!rc)
        fs_cote = (int8_t)k;
    return rc;
}

/* « fat:/Songs//A.XT » -> « Songs/A.XT » */
static void fs_norme(const char *c, char *d)
{
    uint32_t n = 0;
    const char *p = c;
    while (*p && *p != ':' && *p != '/')
        p++;
    if (*p == ':')                                 /* un lecteur (fat:, sd:) : ignore */
        c = p + 1;
    while (*c && n + 1u < FS_NOM) {
        if (*c == '/' && (n == 0 || d[n - 1] == '/')) { c++; continue; }
        if (*c == '.' && c[1] == '/' && (n == 0 || d[n - 1] == '/')) { c += 2; continue; }
        d[n++] = *c++;
    }
    while (n && d[n - 1] == '/')
        n--;
    d[n] = 0;
}

static int fs_cherche(const char *nom)            /* l'entree de ce nom, -1 si aucune */
{
    uint32_t i;
    for (i = 0; i < FS_NB_ENT; i++)
        if (fs_rep.ent[i].nom[0] && !strcasecmp(fs_rep.ent[i].nom, nom))
            return (int)i;
    return -1;
}

static int fs_libre(void)
{
    uint32_t i;
    for (i = 0; i < FS_NB_ENT; i++)
        if (!fs_rep.ent[i].nom[0])
            return (int)i;
    return -1;
}

/* le plus long passage de secteurs libres ; `sauf` : une entree a compter libre (aucune : -1) */
static uint32_t fs_place(uint32_t *premier)
{
    static uint8_t pris[FS_NB_SECT];
    uint32_t i, s, meilleur = 0, run = 0;
    memset(pris, 0, sizeof pris);
    for (i = 0; i < FS_NB_ENT; i++) {
        const fs_ent_t *e = &fs_rep.ent[i];
        if (!e->nom[0] || e->taille == FS_DOSSIER)
            continue;
        for (s = e->premier; s < (uint32_t)e->premier + FS_NSECT(e) && s < FS_NB_SECT; s++)
            pris[s] = 1;
    }
    *premier = 0;
    for (s = 0; s <= FS_NB_SECT; s++) {
        if (s < FS_NB_SECT && !pris[s]) {
            run++;
            continue;
        }
        if (run > meilleur) {
            meilleur = run;
            *premier = s - run;
        }
        run = 0;
    }
    return meilleur;
}

/* ── FILE ──────────────────────────────────────────────────────────────────── */
enum { F_LIBRE, F_LECTURE, F_ECRITURE, F_PUITS, F_OBT_L, F_OBT_E };   /* (OBT : fm1_obt.c, compresse d'un tenant) */
struct fm1_fichier {
    uint8_t mode, erreur, fin, lz;                /* lz : ses blocs sont en LZ (sinon PackBits) */
    int16_t *lz_tables;                           /* ecriture : les chaines du LZ */
    /* un .OBT (fm1_obt.c) : le fichier brut dessous, et ce qui en est decompresse */
    struct fm1_fichier *brut;
    uint8_t *fen;
    uint32_t fen_debut, fen_n;
    uint32_t debut;                               /* adresse flash de l'etendue */
    uint32_t taille, pos;
    uint32_t index;
    int32_t bloc_en_cache;
    uint8_t *bloc;                                /* lecture : le bloc decompresse ; ecriture : le bloc en cours */
    const uint8_t *rom;                           /* un morceau livre avec le firmware (sinon 0 : la flash) */
    /* ecriture */
    uint32_t premier, sect_max, ecrit, efface;    /* efface : secteurs deja effaces */
    uint32_t *table, nb_blocs, cap_table;
    char nom[FS_NOM];
};
static struct fm1_fichier fs_fichiers[4];
/* (fm1_obt.c, plus loin) */
static uint8_t fs_brut;                           /* on ouvre le fichier brut d'un .OBT : pas de .OBT dessous */
static int fs_est_obt(const char *chemin);
static FILE *obt_ouvre(const char *chemin, const char *mode);
static size_t obt_lit(uint8_t *d, size_t total, FILE *f);
static size_t obt_ecrit(const uint8_t *s, size_t total, FILE *f);
static int obt_cherche(FILE *f, long position);
static int obt_ferme(FILE *f);
static struct fm1_fichier fs_console = {F_PUITS};
FILE *const stdin = &fs_console;
FILE *const stdout = &fs_console;
FILE *const stderr = &fs_console;

static int fs_est_journal(const char *nom)
{
    uint32_t n = (uint32_t)strlen(nom);
    return n >= 4u && !strcasecmp(nom + n - 4u, ".LOG");
}

static void fs_init(void) { fs_monte(); }

FILE *fopen(const char *chemin, const char *mode)
{
    char nom[FS_NOM];
    struct fm1_fichier *f = 0;
    uint32_t i;
    if (!chemin || !mode)
        return 0;
    if (!fs_brut && fs_est_obt(chemin))
        return obt_ouvre(chemin, mode);
    fs_norme(chemin, nom);
    for (i = 0; i < sizeof fs_fichiers / sizeof fs_fichiers[0]; i++)
        if (fs_fichiers[i].mode == F_LIBRE) {
            f = &fs_fichiers[i];
            break;
        }
    if (!f || !nom[0]) {
        if (!f && mode[0] != 'r')
            fs_err = FS_E_FICHIERS;
        return 0;
    }
    memset(f, 0, sizeof *f);
    str_cpy(f->nom, nom, sizeof f->nom);
    f->bloc_en_cache = -1;
    if (fs_est_journal(nom)) {                    /* le journal : en RAM */
        if (mode[0] == 'r')
            return 0;
        f->mode = F_PUITS;
        return f;
    }
    if (mode[0] == 'r' && (!flash_ok || fs_cherche(nom) < 0) && rom_cherche(nom) >= 0) {
        const int k = rom_cherche(nom);             /* un morceau livre avec le firmware */
        f->bloc = (uint8_t *)malloc(FS_BLOC);
        if (!f->bloc)
            return 0;
        f->mode = F_LECTURE;
        f->rom = fm1_rom_donnees + fm1_rom[k].debut;
        f->taille = fm1_rom[k].taille;
        f->index = fm1_rom[k].index;
        return f;
    }
    if (!flash_ok) {
        if (mode[0] != 'r')
            fs_err = FS_E_FLASH;
        return 0;
    }
    if (mode[0] == 'r') {
        int e = fs_cherche(nom);
        if (e < 0 || fs_rep.ent[e].taille == FS_DOSSIER)
            return 0;
        f->bloc = (uint8_t *)malloc(FS_BLOC);
        if (!f->bloc)
            return 0;
        f->mode = F_LECTURE;
        f->lz = (uint8_t)((fs_rep.ent[e].nb_sect & FS_LZ) != 0u);
        f->debut = FS_DONNEES + (uint32_t)fs_rep.ent[e].premier * FS_SECT;
        f->taille = fs_rep.ent[e].taille;
        f->index = fs_rep.ent[e].index;
        return f;
    }
    if (mode[0] == 'w' || mode[0] == 'a') {       /* (« a » sur un vrai fichier : on recommence a zero) */
        uint32_t premier, n = fs_place(&premier);
        if (n == 0) {
            fs_err = FS_E_PLACE;
            return 0;
        }
        f->bloc = (uint8_t *)malloc(FS_BLOC);
        f->cap_table = 64;
        f->table = (uint32_t *)malloc(f->cap_table * sizeof(uint32_t));
        /* (sous un .OBT, deja compresse : PackBits, sans les chaines du LZ) */
        f->lz_tables = fs_brut ? 0 : (int16_t *)malloc((LZ_TETES + FS_BLOC) * sizeof(int16_t));
        if (!f->bloc || !f->table || (!fs_brut && !f->lz_tables)) {
            fs_err = FS_E_MEMOIRE;
            free(f->bloc);
            free(f->table);
            free(f->lz_tables);
            return 0;
        }
        f->lz = (uint8_t)!fs_brut;
        f->mode = F_ECRITURE;
        f->premier = premier;
        f->sect_max = n;
        f->debut = FS_DONNEES + premier * FS_SECT;
        return f;
    }
    return 0;
}

static int fs_ajoute(struct fm1_fichier *f, const void *p, uint32_t n)   /* au bout du flux compresse */
{
    uint32_t fin = f->ecrit + n;
    if (fin > f->sect_max * FS_SECT) {
        fs_err = FS_E_PLACE;
        return -1;
    }
    while (f->efface * FS_SECT < fin) {
        fm1_wdt_feed();
        if (st_erase(f->debut + f->efface * FS_SECT)) {
            fs_err = FS_E_ECRITURE;
            return -1;
        }
        f->efface++;
    }
    if (n && st_prog(f->debut + f->ecrit, p, n)) {
        fs_err = FS_E_ECRITURE;
        return -1;
    }
    f->ecrit = fin;
    return 0;
}

static int fs_vide_bloc(struct fm1_fichier *f, uint32_t n)   /* le bloc en cours (n octets) part en flash */
{
    uint32_t c;
    if (f->nb_blocs + 2u > f->cap_table) {
        uint32_t *t = (uint32_t *)realloc(f->table, f->cap_table * 2u * sizeof(uint32_t));
        if (!t) {
            fs_err = FS_E_MEMOIRE;
            return -1;
        }
        f->table = t;
        f->cap_table *= 2u;
    }
    f->table[f->nb_blocs++] = f->ecrit;
    c = f->lz ? lz_comprime(f->bloc, n, fs_comp, f->lz_tables) : pb_comprime(f->bloc, n, fs_comp);
    return fs_ajoute(f, fs_comp, c);
}

size_t fwrite(const void *p, size_t t, size_t n, FILE *f)
{
    const uint8_t *s = (const uint8_t *)p;
    size_t total = t * n, fait = 0;
    if (!f)
        return 0;
    if (f->mode == F_PUITS) {
        journal_ecrit((const char *)p, total);
        return n;
    }
    if (f->mode == F_OBT_E)
        return t ? obt_ecrit(s, total, f) / t : 0;
    if (f->mode != F_ECRITURE || f->erreur)
        return 0;
    while (fait < total) {
        uint32_t dans = f->taille % FS_BLOC, k = FS_BLOC - dans;
        if (k > total - fait)
            k = (uint32_t)(total - fait);
        memcpy(f->bloc + dans, s + fait, k);
        fait += k;
        f->taille += k;
        f->pos = f->taille;
        if (f->taille % FS_BLOC == 0u && fs_vide_bloc(f, FS_BLOC)) {
            f->erreur = 1;
            break;
        }
    }
    return t ? fait / t : 0;
}

int fclose(FILE *f)
{
    int rc = 0;
    if (!f || f == &fs_console)
        return 0;
    if (f->mode == F_OBT_L || f->mode == F_OBT_E)
        return obt_ferme(f);
    if (f->mode == F_ECRITURE) {
        if (!f->erreur && f->taille % FS_BLOC)
            f->erreur = (uint8_t)(fs_vide_bloc(f, f->taille % FS_BLOC) != 0);
        if (!f->erreur) {                         /* la table : une adresse par bloc, puis la fin */
            uint32_t index = f->ecrit;
            f->table[f->nb_blocs] = f->ecrit;
            f->erreur = (uint8_t)(fs_ajoute(f, f->table, (f->nb_blocs + 1u) * 4u) != 0);
            if (!f->erreur) {
                int e = fs_cherche(f->nom);
                if (e < 0)
                    e = fs_libre();
                if (e < 0) {
                    f->erreur = 1;
                    fs_err = FS_E_REPERTOIRE;
                }
                else {
                    fs_ent_t *x = &fs_rep.ent[e];
                    memset(x, 0, sizeof *x);
                    str_cpy(x->nom, f->nom, sizeof x->nom);
                    x->premier = (uint16_t)f->premier;
                    x->nb_sect = (uint16_t)((f->ecrit + FS_SECT - 1u) / FS_SECT | (f->lz ? FS_LZ : 0u));
                    x->taille = f->taille;
                    x->index = index;
                    f->erreur = (uint8_t)(fs_ecrit_rep() != 0);
                    if (f->erreur)
                        fs_err = FS_E_ECRITURE;
                }
            }
        }
        rc = f->erreur ? EOF : 0;
        free(f->table);
        free(f->lz_tables);
    }
    free(f->bloc);
    f->mode = F_LIBRE;
    return rc;
}

static int fs_charge_bloc(struct fm1_fichier *f, uint32_t k)
{
    uint32_t a[2], n;
    if ((int32_t)k == f->bloc_en_cache)
        return 0;
    if (f->rom) {                                 /* (un morceau livre avec le firmware : deja en memoire) */
        memcpy(a, f->rom + f->index + k * 4u, sizeof a);
        if (a[1] < a[0] || a[1] - a[0] > FS_COMP_MAX)
            return -1;
        n = a[1] - a[0];
        memcpy(fs_comp, f->rom + a[0], n);
    } else {
        if (st_read(f->debut + f->index + k * 4u, a, sizeof a) || a[1] < a[0] || a[1] - a[0] > FS_COMP_MAX)
            return -1;
        n = a[1] - a[0];
        if (st_read(f->debut + a[0], fs_comp, n))
            return -1;
    }
    if (f->lz)
        lz_decomprime(fs_comp, n, f->bloc, FS_BLOC);
    else
        pb_decomprime(fs_comp, n, f->bloc, FS_BLOC);
    f->bloc_en_cache = (int32_t)k;
    return 0;
}

size_t fread(void *p, size_t t, size_t n, FILE *f)
{
    uint8_t *d = (uint8_t *)p;
    size_t total = t * n, fait = 0;
    if (f && f->mode == F_OBT_L)
        return t ? obt_lit(d, total, f) / t : 0;
    if (!f || f->mode != F_LECTURE)
        return 0;
    while (fait < total) {
        uint32_t k, dans, m;
        if (f->pos >= f->taille) {
            f->fin = 1;
            break;
        }
        k = f->pos / FS_BLOC;
        dans = f->pos % FS_BLOC;
        if (fs_charge_bloc(f, k)) {
            f->erreur = 1;
            break;
        }
        m = FS_BLOC - dans;
        if (m > f->taille - f->pos)
            m = f->taille - f->pos;
        if (m > total - fait)
            m = (uint32_t)(total - fait);
        memcpy(d + fait, f->bloc + dans, m);
        fait += m;
        f->pos += m;
    }
    return t ? fait / t : 0;
}

int fseek(FILE *f, long position, int origine)
{
    long p;
    if (!f || (f->mode != F_LECTURE && f->mode != F_OBT_L))
        return -1;
    p = origine == SEEK_SET ? position : origine == SEEK_CUR ? (long)f->pos + position : (long)f->taille + position;
    if (f->mode == F_OBT_L)
        return obt_cherche(f, p);
    if (p < 0 || p > (long)f->taille)
        return -1;
    f->pos = (uint32_t)p;
    f->fin = 0;
    return 0;
}

long ftell(FILE *f) { return f ? (long)f->pos : -1; }
void rewind(FILE *f) { fseek(f, 0, SEEK_SET); }
int feof(FILE *f) { return f ? f->fin : 1; }
int ferror(FILE *f) { return f ? f->erreur : 1; }
int fflush(FILE *f) { (void)f; return 0; }

int fgetc(FILE *f)
{
    uint8_t c;
    return fread(&c, 1, 1, f) == 1 ? c : EOF;
}

char *fgets(char *s, int n, FILE *f)
{
    int i = 0, c = 0;
    if (n <= 0)
        return 0;
    while (i < n - 1 && (c = fgetc(f)) != EOF) {
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    s[i] = 0;
    return i ? s : 0;
}

int fputc(int c, FILE *f)
{
    uint8_t x = (uint8_t)c;
    return fwrite(&x, 1, 1, f) == 1 ? c : EOF;
}

int fputs(const char *s, FILE *f) { return fwrite(s, 1, strlen(s), f) == strlen(s) ? 0 : EOF; }

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char b[256];
    int n = vsnprintf(b, sizeof b, fmt, ap);
    fwrite(b, 1, n < (int)sizeof b ? (size_t)n : sizeof b - 1u, f);
    return n;
}

int remove(const char *chemin)
{
    char nom[FS_NOM];
    int e;
    fs_norme(chemin, nom);
    e = fs_cherche(nom);
    if (e < 0)
        return -1;
    memset(&fs_rep.ent[e], 0, sizeof fs_rep.ent[e]);
    return fs_ecrit_rep() ? -1 : 0;
}

int rename(const char *ancien, const char *nouveau)
{
    char a[FS_NOM], n[FS_NOM];
    int e, x;
    fs_norme(ancien, a);
    fs_norme(nouveau, n);
    e = fs_cherche(a);
    if (e < 0 || !n[0])
        return -1;
    x = fs_cherche(n);
    if (x >= 0 && x != e)
        memset(&fs_rep.ent[x], 0, sizeof fs_rep.ent[x]);
    str_cpy(fs_rep.ent[e].nom, n, FS_NOM);
    return fs_ecrit_rep() ? -1 : 0;
}

/* ── Dossiers ──────────────────────────────────────────────────────────────── */
/* le reste du nom sous le dossier `d` (longueur ld), 0 si le nom n'y est pas */
static const char *fs_sous(const char *nom, const char *d, uint32_t ld)
{
    if (!ld)
        return nom;
    if (strncasecmp(nom, d, ld) || nom[ld] != '/')
        return 0;
    return nom + ld + 1u;
}

static int fs_est_dossier(const char *nom)
{
    uint32_t i, l = (uint32_t)strlen(nom);
    if (!l)
        return 1;
    for (i = 0; i < FS_NB_ENT; i++) {
        const fs_ent_t *e = &fs_rep.ent[i];
        if (!e->nom[0])
            continue;
        if (e->taille == FS_DOSSIER && !strcasecmp(e->nom, nom))
            return 1;
        if (fs_sous(e->nom, nom, l))
            return 1;
    }
    for (i = 0; i < FM1_ROM_NB; i++)
        if (fs_sous(fm1_rom[i].nom, nom, l))
            return 1;
    return 0;
}

struct fm1_dossier { uint8_t pris; char nom[FS_NOM]; uint32_t i; struct dirent ent; };
static struct fm1_dossier fs_dossiers[2];

DIR *opendir(const char *chemin)
{
    uint32_t i;
    char nom[FS_NOM];
    fs_norme(chemin, nom);
    if (!fs_est_dossier(nom))
        return 0;
    for (i = 0; i < 2u; i++)
        if (!fs_dossiers[i].pris) {
            fs_dossiers[i].pris = 1;
            str_cpy(fs_dossiers[i].nom, nom, FS_NOM);
            fs_dossiers[i].i = 0;
            return &fs_dossiers[i];
        }
    return 0;
}

/* le premier segment de `reste` (jusqu'au '/'), dans `seg` ; 1 si c'est un dossier */
static int fs_segment(const char *reste, char *seg)
{
    uint32_t n = 0;
    while (reste[n] && reste[n] != '/' && n + 1u < 64u) {
        seg[n] = reste[n];
        n++;
    }
    seg[n] = 0;
    return reste[n] == '/';
}

struct dirent *readdir(DIR *d)
{
    uint32_t ld = (uint32_t)strlen(d->nom);
    while (d->i < FS_NB_ENT) {
        uint32_t i = d->i++, j;
        const fs_ent_t *e = &fs_rep.ent[i];
        const char *r;
        char seg[64];
        int dossier, deja = 0;
        if (!e->nom[0] || !(r = fs_sous(e->nom, d->nom, ld)) || !*r)
            continue;
        dossier = fs_segment(r, seg) || e->taille == FS_DOSSIER;
        for (j = 0; j < i && !deja; j++) {        /* un dossier n'apparait qu'une fois */
            const char *r2;
            char seg2[64];
            if (fs_rep.ent[j].nom[0] && (r2 = fs_sous(fs_rep.ent[j].nom, d->nom, ld)) && *r2)
                deja = (fs_segment(r2, seg2) || fs_rep.ent[j].taille == FS_DOSSIER) && dossier &&
                       !strcasecmp(seg, seg2);
        }
        if (deja)
            continue;
        str_cpy(d->ent.d_name, seg, sizeof d->ent.d_name);
        d->ent.d_type = dossier ? DT_DIR : DT_REG;
        return &d->ent;
    }
    while (d->i < FS_NB_ENT + FM1_ROM_NB) {       /* puis les morceaux livres avec le firmware */
        const uint32_t k = d->i++ - FS_NB_ENT;
        const char *r;
        char seg[64], seg2[64];
        uint32_t j;
        int dossier, deja = 0;
        if (!(r = fs_sous(fm1_rom[k].nom, d->nom, ld)) || !*r || fs_cherche(fm1_rom[k].nom) >= 0)
            continue;                             /* (hors du dossier, ou un fichier de la flash passe devant) */
        dossier = fs_segment(r, seg);
        for (j = 0; dossier && j < FS_NB_ENT + k && !deja; j++) {   /* un dossier n'apparait qu'une fois */
            const char *n2 = j < FS_NB_ENT ? fs_rep.ent[j].nom : fm1_rom[j - FS_NB_ENT].nom, *r2;
            if (n2[0] && (r2 = fs_sous(n2, d->nom, ld)) && *r2)
                deja = (fs_segment(r2, seg2) || (j < FS_NB_ENT && fs_rep.ent[j].taille == FS_DOSSIER)) &&
                       !strcasecmp(seg, seg2);
        }
        if (deja)
            continue;
        str_cpy(d->ent.d_name, seg, sizeof d->ent.d_name);
        d->ent.d_type = dossier ? DT_DIR : DT_REG;
        return &d->ent;
    }
    return 0;
}

int closedir(DIR *d)
{
    if (d)
        d->pris = 0;
    return 0;
}

int stat(const char *chemin, struct stat *st)
{
    char nom[FS_NOM];
    int e;
    fs_norme(chemin, nom);
    memset(st, 0, sizeof *st);
    e = fs_cherche(nom);
    if (e >= 0 && fs_rep.ent[e].taille != FS_DOSSIER) {
        st->st_mode = S_IFREG;
        st->st_size = (long)fs_rep.ent[e].taille;
        return 0;
    }
    if ((e = rom_cherche(nom)) >= 0) {
        st->st_mode = S_IFREG;
        st->st_size = (long)fm1_rom[e].taille;
        return 0;
    }
    if (fs_est_dossier(nom)) {
        st->st_mode = S_IFDIR;
        return 0;
    }
    return -1;
}

int mkdir(const char *chemin, unsigned mode)
{
    char nom[FS_NOM];
    int e;
    (void)mode;
    fs_norme(chemin, nom);
    if (!nom[0] || fs_cherche(nom) >= 0 || fs_est_dossier(nom) || !flash_ok)
        return -1;
    e = fs_libre();
    if (e < 0)
        return -1;
    memset(&fs_rep.ent[e], 0, sizeof fs_rep.ent[e]);
    str_cpy(fs_rep.ent[e].nom, nom, FS_NOM);
    fs_rep.ent[e].taille = FS_DOSSIER;
    return fs_ecrit_rep() ? -1 : 0;
}

/* Pour le diagnostic par USB (fm1_transfert.c, INFO / ENTREE) */
static void fs_etat(uint8_t *o)                   /* 14 octets */
{
    static uint8_t pris[FS_NB_SECT];
    uint32_t i, s2, libres = 0, premier, ouverts = 0;
    memset(pris, 0, sizeof pris);
    for (i = 0; i < FS_NB_ENT; i++) {
        const fs_ent_t *e = &fs_rep.ent[i];
        if (!e->nom[0] || e->taille == FS_DOSSIER)
            continue;
        for (s2 = e->premier; s2 < (uint32_t)e->premier + FS_NSECT(e) && s2 < FS_NB_SECT; s2++)
            pris[s2] = 1;
    }
    for (s2 = 0; s2 < FS_NB_SECT; s2++)
        libres += !pris[s2];
    for (i = 0; i < sizeof fs_fichiers / sizeof fs_fichiers[0]; i++)
        ouverts += fs_fichiers[i].mode != F_LIBRE;
    o[0] = (uint8_t)flash_ok;
    o[1] = (uint8_t)libres;
    o[2] = (uint8_t)fs_place(&premier);
    o[3] = (uint8_t)FS_NB_SECT;
    o[4] = (uint8_t)ouverts;
    o[5] = fs_err;
    o[6] = (uint8_t)tas_utilise; o[7] = (uint8_t)(tas_utilise >> 8); o[8] = (uint8_t)(tas_utilise >> 16);
    o[9] = (uint8_t)tas_pic; o[10] = (uint8_t)(tas_pic >> 8); o[11] = (uint8_t)(tas_pic >> 16);
    i = (uint32_t)(_heap_end - _heap_start);
    o[12] = (uint8_t)(i >> 8); o[13] = (uint8_t)(i >> 16);
}

static int fs_entree(uint32_t n, uint8_t *o)      /* la n-ieme entree : taille(4) premier(2) nb_sect(2) nom ; sa longueur */
{
    uint32_t i, k = 0, l;
    for (i = 0; i < FS_NB_ENT; i++) {
        const fs_ent_t *e = &fs_rep.ent[i];
        if (!e->nom[0] || k++ != n)
            continue;
        o[0] = (uint8_t)e->taille; o[1] = (uint8_t)(e->taille >> 8); o[2] = (uint8_t)(e->taille >> 16); o[3] = (uint8_t)(e->taille >> 24);
        o[4] = (uint8_t)e->premier; o[5] = (uint8_t)(e->premier >> 8);
        o[6] = (uint8_t)e->nb_sect; o[7] = (uint8_t)(e->nb_sect >> 8);
        l = (uint32_t)strlen(e->nom);
        if (l > 40u)
            l = 40u;
        memcpy(o + 8, e->nom, l);
        return (int)(8u + l);
    }
    return -1;
}
