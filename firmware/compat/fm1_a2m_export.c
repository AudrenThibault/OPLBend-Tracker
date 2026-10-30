// ============================================================================
//  EXPORT A2M (menu PROJECT) : le morceau en .A2M d'AdLib Tracker II (version
//  14), pour l'ouvrir dans AT2 et dans Amber OPL3 Tracker. Seulement ce qu'AT2
//  connait : la conversion est celle de amber/import/a2m_export.cpp, sans le
//  .OPS. Chaque ligne du SONG devient un pattern (le chain deroule, son TSP
//  applique aux notes), les patterns identiques une seule fois ; FX1 / FX2
//  partent tels quels. Ne partent PAS : CMD (LSDJ), VEL, FX3 / FX4, les
//  tables, le circuit bending, et ce que joue un instrument SAMPLE.
//
//  Le FM-1 n'a pas la RAM de tout preparer (le bloc « songdata » fait a lui
//  seul 1,1 Mo) : tout est produit au fil de l'eau, et compresse au fil de
//  l'eau dans le LZH d'AT2 (lzh_compress.c, le meme flux de bits), mais avec
//  les seules repetitions d'un octet (distance 1) : il ne faut aucune fenetre,
//  et un .A2M est surtout fait de zeros. L'en-tete porte les tailles et une
//  CRC des donnees compressees, et un fichier ne se reprend pas en arriere
//  (fm1_fs.c) : deux passes, la premiere sans rien ecrire.
// ============================================================================
#define EX_SD 1138338u                            /* le bloc songdata, comme a2m_export.cpp */
#define EX_NOMS 86u
#define EX_INSTRUMENTS 11051u
#define EX_MACROS 14621u
#define EX_COLONNES_OFF 1130042u
#define EX_ORDRE 1124381u
#define EX_TEMPO 1124509u
#define EX_SPEED 1124510u
#define EX_LONGUEUR 1124512u
#define EX_PISTES_SD 1124514u
#define EX_MACRO_SPEEDUP 1124515u
#define EX_LIGNES_PAR_TEMPS 1138335u
#define EX_FINETUNE_TEMPO 1138336u
#define EX_PISTES 20
#define EX_LIGNES 256
#define EX_PATTERN ((uint32_t)EX_PISTES * EX_LIGNES * 6u)

// ── Le LZH, en flux ─────────────────────────────────────────────────────────
#define EX_NC 511                                 /* 255 + MAXMATCH (256) + 2 - THRESHOLD (2) */
#define EX_NP 15
#define EX_NT 19
#define EX_CBIT 16
#define EX_PBIT 14
#define EX_TBIT 15
#define EX_BLOC 1303u                             /* symboles par bloc, comme Amber (non « ultra ») */

static struct {
    FILE *f;                                      /* 0 : la premiere passe, on ne fait que compter */
    uint32_t n, crc;                              /* octets sortis, et leur CRC (celle de l'en-tete) */
    uint32_t acc;
    int bits;
    uint32_t ns;
    uint32_t entre, pos;                          /* les octets entres ; le prochain a coder */
    uint32_t nsym;
    int erreur;
} ex;

/* Le travail, pris sur le tas le temps de l'export seulement (~45 Ko ; 40 Ko pour un .OBT) : en memoire fixe, il manquait a
   l'import d'un .A2M (« MEMOIRE PLEINE »). */
#define EX_NOEUDS 1024                            /* (2 x 511 au plus : les noeuds d'un arbre de Huffman) */
#define EX_FENETRE 4096u                          /* la distance d'une copie, au plus (la RAM : 4 Ko suffisent) */
#define EX_AM 8191u                               /* l'anneau de l'entree : 8 Ko */
#define EX_TETES 2048u
#define EX_CHAINE 48u                             /* candidats essayes par position */
typedef struct {
    uint32_t crc_table[256];
    uint8_t sortie[512];
    uint16_t sym[EX_BLOC], dist[EX_BLOC];
    uint8_t anneau[EX_AM + 1u];
    uint16_t tete[EX_TETES], prec[EX_FENETRE];
    uint16_t hf_f[EX_NOEUDS];                     /* (au plus EX_BLOC : 16 bits suffisent) */
    int16_t hf_g[EX_NOEUDS], hf_d[EX_NOEUDS], hf_s[EX_NOEUDS], hf_pile[EX_NOEUDS], hf_prof[EX_NOEUDS],
            hf_syms[EX_NOEUDS];
    uint32_t cf[EX_NC], pf[EX_NP], tf[EX_NT];
    uint8_t cl[EX_NC], pl[EX_NP], tl[EX_NT];
    uint16_t cc[EX_NC], pc[EX_NP], tc[EX_NT];
    /* (l'export seulement : un .OBT n'en prend pas la place, fm1_obt.c) */
    uint8_t tampon[1024];
    md_macro_fm_t macro;
} ex_travail_t;
#define EX_TRAVAIL_OBT offsetof(ex_travail_t, tampon)
static ex_travail_t *ew;
#define ex_crc_table (ew->crc_table)

static uint32_t ex_crc(uint32_t c, const uint8_t *p, uint32_t n)   /* (pas de ~ : celle d'AT2, a2m_crc32) */
{
    while (n--)
        c = ex_crc_table[(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return c;
}

static void ex_vide(void)
{
    if (ex.f && ex.ns && fwrite(ew->sortie, 1, ex.ns, ex.f) != ex.ns)
        ex.erreur = 1;
    ex.ns = 0;
}

static void ex_octet_sortie(uint8_t b)
{
    ex.crc = ex_crc(ex.crc, &b, 1);
    ex.n++;
    ew->sortie[ex.ns++] = b;
    if (ex.ns == sizeof ew->sortie)
        ex_vide();
}

static void ex_bits(int n, uint32_t v)            /* les poids forts d'abord */
{
    while (n > 0) {
        const int place = 8 - ex.bits;
        if (n >= place) {
            ex.acc = (ex.acc << place) | (v >> (n - place));
            n -= place;
            v &= (1u << n) - 1u;
            ex_octet_sortie((uint8_t)ex.acc);
            ex.acc = 0;
            ex.bits = 0;
        } else {
            ex.acc = (ex.acc << n) | v;
            ex.bits += n;
            n = 0;
        }
    }
}

/* Les longueurs de Huffman (16 bits au plus) : huf_lengths de lzh_compress.c, ses tableaux hors de la pile. */
#define hf_f (ew->hf_f)
#define hf_g (ew->hf_g)
#define hf_d (ew->hf_d)
#define hf_s (ew->hf_s)
#define hf_pile (ew->hf_pile)
#define hf_prof (ew->hf_prof)
#define hf_syms (ew->hf_syms)

static void ex_longueurs(int n, const uint32_t *freq, uint8_t *bl)
{
    int actifs = 0, nn = 0, sp, i, j, nbs, k;
    int count[32];
    memset(bl, 0, (size_t)n);
    for (i = 0; i < n; i++)
        if (freq[i])
            actifs++;
    if (actifs <= 1) {
        for (i = 0; i < n; i++)
            if (freq[i]) {
                bl[i] = 1;
                break;
            }
        return;
    }
    for (i = 0; i < n; i++)
        if (freq[i]) {
            hf_f[nn] = (uint16_t)freq[i];
            hf_s[nn] = (int16_t)i;
            hf_g[nn] = hf_d[nn] = -1;
            nn++;
        }
    for (;;) {                                    /* les deux plus petits, fondus */
        int m1 = -1, m2 = -1;
        for (i = 0; i < nn; i++)
            if (hf_f[i] && (m1 < 0 || hf_f[i] < hf_f[m1]))
                m1 = i;
        for (i = 0; i < nn; i++)
            if (hf_f[i] && i != m1 && (m2 < 0 || hf_f[i] < hf_f[m2]))
                m2 = i;
        if (m2 < 0)
            break;
        hf_f[nn] = (uint16_t)(hf_f[m1] + hf_f[m2]);
        hf_f[m1] = hf_f[m2] = 0;
        hf_s[nn] = -1;
        hf_g[nn] = (int16_t)m1;
        hf_d[nn] = (int16_t)m2;
        nn++;
    }
    sp = 0;
    hf_pile[sp] = (int16_t)(nn - 1);
    hf_prof[sp++] = 0;
    while (sp > 0) {
        const int nd = hf_pile[--sp], d = hf_prof[sp];
        if (hf_s[nd] >= 0)
            bl[hf_s[nd]] = (uint8_t)(d > 31 ? 31 : d);
        else {
            hf_pile[sp] = hf_g[nd];
            hf_prof[sp++] = (int16_t)(d + 1);
            hf_pile[sp] = hf_d[nd];
            hf_prof[sp++] = (int16_t)(d + 1);
        }
    }
    memset(count, 0, sizeof count);               /* au plus 16 bits (Kraft) */
    for (i = 0; i < n; i++)
        if (bl[i])
            count[bl[i]]++;
    {
        int trop = 0;
        for (i = 31; i > 16; i--) {
            trop += count[i];
            count[i] = 0;
        }
        while (trop > 0) {
            i = 15;
            while (i > 0 && count[i] == 0)
                i--;
            if (i <= 0)
                break;
            count[i]--;
            count[i + 1] += 2;
            count[16]--;
            trop--;
        }
    }
    nbs = 0;                                      /* les plus longs d'abord, a longueur egale les plus rares */
    for (i = 0; i < n; i++)
        if (bl[i])
            hf_syms[nbs++] = (int16_t)i;
    for (i = 0; i < nbs - 1; i++)
        for (j = 0; j < nbs - 1 - i; j++) {
            const int a = hf_syms[j], b = hf_syms[j + 1];
            if (bl[a] < bl[b] || (bl[a] == bl[b] && freq[a] > freq[b])) {
                hf_syms[j] = (int16_t)b;
                hf_syms[j + 1] = (int16_t)a;
            }
        }
    memset(bl, 0, (size_t)n);
    k = 0;
    for (i = 16; i > 0; i--)
        while (count[i] > 0 && k < nbs) {
            bl[hf_syms[k++]] = (uint8_t)i;
            count[i]--;
        }
}

static void ex_codes(int n, const uint8_t *bl, uint16_t *cd)
{
    uint16_t cnt[17], suiv[17], k = 0;
    int i, b;
    memset(cnt, 0, sizeof cnt);
    memset(suiv, 0, sizeof suiv);
    for (i = 0; i < n; i++)
        if (bl[i] && bl[i] <= 16u)
            cnt[bl[i]]++;
    for (b = 1; b <= 16; b++) {
        k = (uint16_t)((k + cnt[b - 1]) << 1);
        suiv[b] = k;
    }
    for (i = 0; i < n; i++)
        cd[i] = bl[i] ? suiv[bl[i]]++ : 0;
}

static void ex_ecrit_pt(int nn, int nbit, int isp, const uint8_t *pl)   /* (write_pt) */
{
    int n = nn, cnt = 0, seul = 0, i;
    while (n > 0 && !pl[n - 1])
        n--;
    if (n == 0) {
        ex_bits(nbit, 0);
        ex_bits(nbit, 0);
        return;
    }
    for (i = 0; i < nn; i++)
        if (pl[i]) {
            cnt++;
            seul = i;
        }
    if (cnt == 1) {
        ex_bits(nbit, 0);
        ex_bits(nbit, (uint32_t)seul);
        return;
    }
    ex_bits(nbit, (uint32_t)n);
    for (i = 0; i < n; i++) {
        const int c = pl[i];
        if (c < 7)
            ex_bits(3, (uint32_t)c);
        else {
            int j;
            for (j = 0; j < c - 4; j++)
                ex_bits(1, 1);
            ex_bits(1, 0);
        }
        if (i == isp - 1) {
            int ecart = 0;
            while (i + 1 + ecart < n && !pl[i + 1 + ecart] && ecart < 3)
                ecart++;
            ex_bits(2, (uint32_t)ecart);
            i += ecart;
        }
    }
}

/* les longueurs du C-arbre, par le T-arbre (write_ct) ; compte_seul : on ne fait que compter les T */
static void ex_ecrit_ct(const uint8_t *cl, const uint8_t *tl, const uint16_t *tc, uint32_t *tf)
{
    int n = EX_NC, i = 0, cnt = 0, seul = 0;
    while (n > 0 && !cl[n - 1])
        n--;
    if (!tf) {
        if (n == 0) {
            ex_bits(EX_CBIT, 0);
            ex_bits(EX_CBIT, 0);
            return;
        }
        for (i = 0; i < EX_NC; i++)
            if (cl[i]) {
                cnt++;
                seul = i;
            }
        if (cnt == 1) {
            ex_bits(EX_CBIT, 0);
            ex_bits(EX_CBIT, (uint32_t)seul);
            return;
        }
        ex_bits(EX_CBIT, (uint32_t)n);
        i = 0;
    }
#define EX_T(s) do { if (tf) tf[s]++; else ex_bits(tl[s], tc[s]); } while (0)
    while (i < n) {
        if (!cl[i]) {
            int r = 1;
            while (i + r < n && !cl[i + r])
                r++;
            while (r > 0) {
                if (r >= 20) {
                    EX_T(2);
                    if (!tf)
                        ex_bits(EX_CBIT, (uint32_t)(r - 20));
                    i += r;
                    r = 0;
                } else if (r >= 3) {
                    const int e = r > 18 ? 18 : r;
                    EX_T(1);
                    if (!tf)
                        ex_bits(4, (uint32_t)(e - 3));
                    r -= e;
                    i += e;
                } else {
                    EX_T(0);
                    r--;
                    i++;
                }
            }
        } else {
            EX_T(cl[i] + 2);
            i++;
        }
    }
#undef EX_T
}

#define ex_cf (ew->cf)
#define ex_pf (ew->pf)
#define ex_tf (ew->tf)
#define ex_cl (ew->cl)
#define ex_pl (ew->pl)
#define ex_tl (ew->tl)
#define ex_cc (ew->cc)
#define ex_pc (ew->pc)
#define ex_tc (ew->tc)

static int ex_code_pos(uint32_t v)               /* le code de position : le nombre de bits de v */
{
    int c = 0;
    while (v) {
        v >>= 1;
        c++;
    }
    return c;
}

static void ex_bloc(void)                         /* les symboles en attente : un bloc (send_block) */
{
    uint32_t j;
    int i, c_actifs = 0, p_actifs = 0, t_actifs = 0;
    if (!ex.nsym)
        return;
    memset(ex_cf, 0, sizeof ex_cf);
    memset(ex_pf, 0, sizeof ex_pf);
    memset(ex_tf, 0, sizeof ex_tf);
    for (j = 0; j < ex.nsym; j++) {
        ex_cf[ew->sym[j]]++;
        if (ew->sym[j] > 255u)
            ex_pf[ex_code_pos(ew->dist[j])]++;
    }
    ex_longueurs(EX_NC, ex_cf, ex_cl);
    ex_codes(EX_NC, ex_cl, ex_cc);
    ex_longueurs(EX_NP, ex_pf, ex_pl);
    ex_codes(EX_NP, ex_pl, ex_pc);
    ex_ecrit_ct(ex_cl, 0, 0, ex_tf);
    ex_longueurs(EX_NT, ex_tf, ex_tl);
    ex_codes(EX_NT, ex_tl, ex_tc);
    for (i = 0; i < EX_NC; i++) c_actifs += ex_cf[i] != 0;
    for (i = 0; i < EX_NP; i++) p_actifs += ex_pf[i] != 0;
    for (i = 0; i < EX_NT; i++) t_actifs += ex_tf[i] != 0;
    ex_bits(16, ex.nsym);
    ex_ecrit_pt(EX_NT, EX_TBIT, 3, ex_tl);
    if (t_actifs <= 1)                            /* un seul T : le decodeur n'en lit aucun bit */
        memset(ex_tl, 0, sizeof ex_tl);
    ex_ecrit_ct(ex_cl, ex_tl, ex_tc, 0);
    ex_ecrit_pt(EX_NP, EX_PBIT, -1, ex_pl);
    for (j = 0; j < ex.nsym; j++) {
        const uint16_t s = ew->sym[j];
        if (c_actifs > 1)                         /* (un seul symbole : le decodeur n'en lit aucun bit) */
            ex_bits(ex_cl[s], ex_cc[s]);
        if (s > 255u) {
            const uint32_t v = ew->dist[j];
            const int pc = ex_code_pos(v);
            if (p_actifs > 1)
                ex_bits(ex_pl[pc], ex_pc[pc]);
            if (pc > 1)                           /* (les bits sous le premier : toujours ecrits) */
                ex_bits(pc - 1, v - (1u << (pc - 1)));
        }
    }
    ex.nsym = 0;
}

static void ex_symbole(uint16_t s, uint16_t dist)
{
    ew->sym[ex.nsym] = s;
    ew->dist[ex.nsym++] = dist;
    if (ex.nsym == EX_BLOC)
        ex_bloc();
}

/* Les copies : ce qui precede, jusqu'a 8 Ko en arriere (comme Amber), trouve par des chaines de
   hachage sur trois octets. L'entree passe par un anneau : 8 Ko d'histoire, et 256 octets d'avance. */
static uint32_t ex_hache(uint32_t q)
{
    return ((uint32_t)ew->anneau[q & EX_AM] << 7 ^ (uint32_t)ew->anneau[(q + 1u) & EX_AM] << 3 ^
            ew->anneau[(q + 2u) & EX_AM]) & (EX_TETES - 1u);
}

static void ex_insere(uint32_t q)                 /* la position q entre dans sa chaine */
{
    const uint32_t h = ex_hache(q);
    ew->prec[q & (EX_FENETRE - 1u)] = ew->tete[h];
    ew->tete[h] = (uint16_t)q;
}

static void ex_avance(uint32_t dispo)            /* un symbole a la position ex.pos (dispo octets devant) */
{
    const uint32_t pos = ex.pos, max = dispo < 256u ? dispo : 256u;
    uint32_t meilleur = 0, dist = 0, k;
    if (max >= 3u && pos >= 1u) {
        uint32_t q = ew->tete[ex_hache(pos)], tours = 0;
        while (tours++ < EX_CHAINE) {
            const uint32_t d = (pos - q) & 0xFFFFu;   /* (la position, sur 16 bits) */
            uint32_t l = 0;
            if (d == 0u || d > EX_FENETRE || d > pos)
                break;
            while (l < max && ew->anneau[(pos - d + l) & EX_AM] == ew->anneau[(pos + l) & EX_AM])
                l++;
            if (l > meilleur) {
                meilleur = l;
                dist = d;
                if (l == max)
                    break;
            }
            q = ew->prec[q & (EX_FENETRE - 1u)];
        }
    }
    if (meilleur >= 3u)
        ex_symbole((uint16_t)(meilleur + 254u), (uint16_t)(dist - 1u));   /* (longueur + 255 + 1 - THRESHOLD) */
    else {
        meilleur = 1;
        ex_symbole(ew->anneau[pos & EX_AM], 0);
    }
    for (k = 0; k < meilleur; k++)
        if (pos + k + 2u < ex.entre)
            ex_insere(pos + k);
    ex.pos = pos + meilleur;
}

static void ex_debut(uint32_t taille)             /* un flux compresse : 5 octets, puis les blocs */
{
    uint32_t k;
    ex_octet_sortie(0);                           /* (pas « ultra ») */
    ex_octet_sortie((uint8_t)taille);
    ex_octet_sortie((uint8_t)(taille >> 8));
    ex_octet_sortie((uint8_t)(taille >> 16));
    ex_octet_sortie((uint8_t)(taille >> 24));
    ex.pos = ex.entre = 0;
    ex.nsym = 0;
    ex.acc = 0;
    ex.bits = 0;
    for (k = 0; k < EX_TETES; k++)
        ew->tete[k] = 0;                          /* (distance nulle : la chaine s'arrete) */
}

static void ex_entre(const uint8_t *p, uint32_t n)
{
    while (n--) {
        ew->anneau[ex.entre & EX_AM] = *p++;
        ex.entre++;
        if (ex.entre - ex.pos >= 258u)
            ex_avance(ex.entre - ex.pos);
    }
}

static void ex_fin(void)
{
    while (ex.pos < ex.entre)
        ex_avance(ex.entre - ex.pos);
    ex_bloc();
    ex_bits(16, 0);                               /* (la fin) */
    if (ex.bits) {
        ex_octet_sortie((uint8_t)(ex.acc << (8 - ex.bits)));
        ex.bits = 0;
        ex.acc = 0;
    }
}

// ── Le morceau, vu comme un .A2M ────────────────────────────────────────────
static md_module_t *ex_m;
static int ex_pistes, ex_lignes, ex_lg, ex_nb_patterns;
static uint8_t ex_ordre[128], ex_rep[128];        /* le pattern de chaque ligne ; la ligne qui porte chaque pattern */
#define ex_macro (ew->macro)

static int ex_opl(int ins)
{
    return ins >= 1 && ins <= MD_MAX_INSTRUMENTS && ex_m->instruments[ins - 1].kind == MD_INSTR_KIND_OPL;
}

/* La case (piste t, ligne r) du pattern de la ligne de SONG k, ses six octets AT2 (vide : des zeros). */
static void ex_case(int t, int k, int r, uint8_t *e)
{
    uint8_t ch;
    memset(e, 0, 6);
    if (t >= ex_pistes || r >= ex_lg || k >= ex_lignes || (ch = ex_m->song[t][k]) == MD_EMPTY)
        return;
    {
        const md_chain_row_t *cr = &ex_m->chains[ch % MD_MAX_CHAINS].rows[r / MD_ROWS_PER_PHRASE];
        const md_phrase_row_t *pr;
        if (cr->phrase == MD_EMPTY)
            return;
        pr = &ex_m->phrases[cr->phrase % MD_MAX_PHRASES].rows[r % MD_ROWS_PER_PHRASE];
        if (pr->instr && !ex_opl(pr->instr))
            return;                               /* un sample : AT2 n'y voit rien */
        if (pr->note == MD_EMPTY)
            e[0] = 0xFF;                          /* key off */
        else if (pr->note) {
            const int n = (int)pr->note + cr->transpose;
            e[0] = (uint8_t)(n < 1 ? 1 : n > 96 ? 96 : n);
        }
        e[1] = pr->instr;
        if (pr->mdcmd != MD_EMPTY) {
            e[2] = pr->mdcmd;
            e[3] = pr->mdval;
        }
        if (pr->cc[0] != MD_EMPTY) {
            e[4] = pr->cc[0];
            e[5] = pr->ccval[0];
        }
    }
}

static uint32_t ex_crc_pattern(int k)
{
    uint8_t e[6];
    uint32_t c = 0xFFFFFFFFu;
    int t, r;
    for (t = 0; t < EX_PISTES; t++)
        for (r = 0; r < EX_LIGNES; r++) {
            ex_case(t, k, r, e);
            c = ex_crc(c, e, 6);
        }
    return c;
}

static int ex_memes(int a, int b)                 /* deux lignes de SONG, le meme pattern ? */
{
    uint8_t e[6], f[6];
    int t, r;
    for (t = 0; t < ex_pistes; t++)
        for (r = 0; r < ex_lg; r++) {
            ex_case(t, a, r, e);
            ex_case(t, b, r, f);
            if (memcmp(e, f, 6))
                return 0;
        }
    return 1;
}

static void ex_pascal(uint8_t *d, const char *s)  /* une chaine Pascal de 43 octets */
{
    uint32_t n = (uint32_t)strlen(s);
    memset(d, 0, 43);
    if (n > 42u)
        n = 42u;
    d[0] = (uint8_t)n;
    memcpy(d + 1, s, n);
}

/* pose src (len octets, a l'offset off du songdata) dans le morceau [base, base + n) */
static void ex_pose(uint8_t *buf, uint32_t base, uint32_t n, uint32_t off, const uint8_t *src, uint32_t len)
{
    uint32_t a = off > base ? off : base, b = off + len < base + n ? off + len : base + n;
    if (a < b)
        memcpy(buf + (a - base), src + (a - off), b - a);
}

/* le morceau [base, base + n) du songdata */
static void ex_songdata(uint8_t *buf, uint32_t base, uint32_t n)
{
    uint8_t t[43];
    int i;
    memset(buf, 0, n);
    if (base < 86u) {
        ex_pascal(t, ex_m->songname);
        ex_pose(buf, base, n, 0, t, 43);
        ex_pascal(t, ex_m->composer);
        ex_pose(buf, base, n, 43, t, 43);
    }
    for (i = 0; i < MD_MAX_INSTRUMENTS; i++) {
        const md_instr_t *ins = &ex_m->instruments[i];
        uint32_t o = EX_NOMS + (uint32_t)i * 43u;
        if (o < base + n && o + 43u > base) {
            char defaut[16];
            snprintf(defaut, sizeof defaut, "Instrument %02d", i + 1);
            if (strcmp(ex_m->instr_names[i], defaut)) {
                ex_pascal(t, ex_m->instr_names[i]);
                ex_pose(buf, base, n, o, t, 43);
            }
        }
        o = EX_INSTRUMENTS + (uint32_t)i * 14u;
        if (o < base + n && o + 14u > base && ins->kind == MD_INSTR_KIND_OPL) {
            memset(t, 0, 14);
            memcpy(t, MD_OPL_FM(ins), 11);
            t[11] = ins->panning < 85 ? 1 : ins->panning > 170 ? 2 : 0;
            t[12] = (uint8_t)ins->fine_tune;
            ex_pose(buf, base, n, o, t, 14);
        }
        o = EX_MACROS + (uint32_t)i * (uint32_t)sizeof(md_macro_fm_t);
        if (ins->kind == MD_INSTR_KIND_OPL &&
            ((o < base + n && o + sizeof(md_macro_fm_t) > base) ||
             (EX_COLONNES_OFF + (uint32_t)i * 28u < base + n && EX_COLONNES_OFF + (uint32_t)i * 28u + 28u > base))) {
            uint32_t off = 0;
            if (md_replayer_get_instr_macro(i + 1, &ex_macro, &off)) {
                int b;
                ex_pose(buf, base, n, o, (const uint8_t *)&ex_macro, (uint32_t)sizeof ex_macro);
                for (b = 0; b < 28; b++)
                    t[b] = (uint8_t)((off >> b) & 1u);
                ex_pose(buf, base, n, EX_COLONNES_OFF + (uint32_t)i * 28u, t, 28);
            }
        }
    }
    ex_pose(buf, base, n, EX_ORDRE, ex_ordre, 128);
    t[0] = ex_m->initial_tempo;
    ex_pose(buf, base, n, EX_TEMPO, t, 1);
    t[0] = ex_m->initial_speed;
    ex_pose(buf, base, n, EX_SPEED, t, 1);
    t[0] = (uint8_t)ex_lg;
    t[1] = (uint8_t)(ex_lg >> 8);
    ex_pose(buf, base, n, EX_LONGUEUR, t, 2);
    t[0] = MD_PISTES_OPL;
    ex_pose(buf, base, n, EX_PISTES_SD, t, 1);
    i = ex_m->macro_speedup ? ex_m->macro_speedup : 1;
    t[0] = (uint8_t)i;
    t[1] = (uint8_t)(i >> 8);
    ex_pose(buf, base, n, EX_MACRO_SPEEDUP, t, 2);
    t[0] = ex_m->bpm_rows_per_beat;
    ex_pose(buf, base, n, EX_LIGNES_PAR_TEMPS, t, 1);
    t[0] = (uint8_t)ex_m->bpm_tempo_finetune;
    t[1] = (uint8_t)((uint16_t)ex_m->bpm_tempo_finetune >> 8);
    ex_pose(buf, base, n, EX_FINETUNE_TEMPO, t, 2);
}

#define ex_tampon (ew->tampon)

static uint32_t ex_flux_songdata(void)            /* le songdata compresse ; sa taille */
{
    uint32_t base, n0 = ex.n;
    ex_debut(EX_SD);
    for (base = 0; base < EX_SD; base += sizeof ex_tampon) {
        const uint32_t n = EX_SD - base < sizeof ex_tampon ? EX_SD - base : (uint32_t)sizeof ex_tampon;
        ex_songdata(ex_tampon, base, n);
        ex_entre(ex_tampon, n);
        if ((base & 0xFFFFu) == 0u)
            fm1_wdt_feed();
    }
    ex_fin();
    return ex.n - n0;
}

static uint32_t ex_flux_patterns(int b)           /* les huit patterns du bloc b, compresses ; leur taille */
{
    uint32_t n0 = ex.n;
    uint8_t e[6];
    int p, t, r;
    ex_debut(8u * EX_PATTERN);
    for (p = 0; p < 8; p++) {
        const int idx = b * 8 + p;
        for (t = 0; t < EX_PISTES; t++)
            for (r = 0; r < EX_LIGNES; r++) {
                if (idx < ex_nb_patterns)
                    ex_case(t, ex_rep[idx], r, e);
                else
                    memset(e, 0, 6);
                ex_entre(e, 6);
            }
        fm1_wdt_feed();
    }
    ex_fin();
    return ex.n - n0;
}

static bool ex_exporte(const char *chemin, char *msg, int cap);

/* Ecrit le .A2M ; faux (et msg) si ce n'est pas possible. */
bool fm1_exporte_a2m(const char *chemin, char *msg, int cap)
{
    bool ok;
    ex_m = md_replayer_get_module();
    if (!ex_m) {
        a2m_message(msg, cap, "NO SONG");
        return false;
    }
    ew = (ex_travail_t *)malloc(sizeof *ew);
    if (!ew) {
        a2m_message(msg, cap, "MEMORY FULL");
        return false;
    }
    ok = ex_exporte(chemin, msg, cap);
    free(ew);
    ew = 0;
    return ok;
}

static bool ex_exporte(const char *chemin, char *msg, int cap)
{
    uint32_t crc_pattern[128], long_sd = 0, long_blocs[16], crc = 0;
    int k, j, passe, nb_blocs;
    for (k = 0; k < 256; k++) {
        uint32_t c = (uint32_t)k;
        for (j = 0; j < 8; j++)
            c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
        ex_crc_table[k] = c;
    }
    // Les lignes de SONG jouees (jusqu'a la premiere vide partout), la longueur des patterns.
    ex_pistes = ex_m->num_channels < MD_PISTES_OPL ? ex_m->num_channels : MD_PISTES_OPL;
    ex_lignes = 0;
    while (ex_lignes < 128) {
        int quelque = 0;
        for (k = 0; k < ex_pistes && !quelque; k++)
            quelque = ex_m->song[k][ex_lignes] != MD_EMPTY;
        if (!quelque)
            break;
        ex_lignes++;
    }
    if (ex_lignes == 0) {
        a2m_message(msg, cap, "NOTHING TO EXPORT");
        return false;
    }
    if (ex_m->patt_len > 0)
        ex_lg = ex_m->patt_len;
    else {
        ex_lg = MD_ROWS_PER_PHRASE;
        for (k = 0; k < ex_lignes; k++)
            for (j = 0; j < ex_pistes; j++) {
                const uint8_t ch = ex_m->song[j][k];
                int r;
                if (ch == MD_EMPTY)
                    continue;
                /* (la derniere ligne du chain qui a une phrase ; en avancant : a reculer, le compilateur fait une
                   lecture « b[r++=-2] » que l'emulateur en ligne ne connait pas) */
                for (r = 0; r < MD_ROWS_PER_CHAIN; r++)
                    if (ex_m->chains[ch % MD_MAX_CHAINS].rows[r].phrase != MD_EMPTY &&
                        (r + 1) * MD_ROWS_PER_PHRASE > ex_lg)
                        ex_lg = (r + 1) * MD_ROWS_PER_PHRASE;
            }
    }
    if (ex_lg > EX_LIGNES)
        ex_lg = EX_LIGNES;
    // Les patterns, chacun une seule fois (meme CRC, puis compares case par case).
    memset(ex_ordre, 0, sizeof ex_ordre);
    ex_nb_patterns = 0;
    for (k = 0; k < ex_lignes; k++) {
        crc_pattern[k] = ex_crc_pattern(k);
        for (j = 0; j < ex_nb_patterns; j++)
            if (crc_pattern[ex_rep[j]] == crc_pattern[k] && ex_memes(ex_rep[j], k))
                break;
        if (j == ex_nb_patterns)
            ex_rep[ex_nb_patterns++] = (uint8_t)k;
        ex_ordre[k] = (uint8_t)j;
        fm1_wdt_feed();
    }
    if (ex_lignes < 128)
        ex_ordre[ex_lignes] = 0x80;               /* reboucle sur la premiere position */
    nb_blocs = (ex_nb_patterns + 7) / 8;
    // Deux passes : la premiere compte (les tailles, la CRC de l'en-tete), la seconde ecrit.
    for (passe = 0; passe < 2; passe++) {
        uint8_t h[84];
        memset(&ex, 0, sizeof ex);
        ex.crc = 0xFFFFFFFFu;
        if (passe) {
            ex.f = fopen(chemin, "wb");
            if (!ex.f) {
                a2m_message(msg, cap, "CANNOT WRITE A2M");
                return false;
            }
            memset(h, 0, sizeof h);
            memcpy(h, "_A2module_", 10);
            h[10] = (uint8_t)crc;
            h[11] = (uint8_t)(crc >> 8);
            h[12] = (uint8_t)(crc >> 16);
            h[13] = (uint8_t)(crc >> 24);
            h[14] = 14;                           /* version 14 */
            h[15] = (uint8_t)ex_nb_patterns;
            for (k = 0; k < 17; k++) {
                const uint32_t v = k ? long_blocs[k - 1] : long_sd;
                h[16 + 4 * k] = (uint8_t)v;
                h[17 + 4 * k] = (uint8_t)(v >> 8);
                h[18 + 4 * k] = (uint8_t)(v >> 16);
                h[19 + 4 * k] = (uint8_t)(v >> 24);
            }
            if (fwrite(h, 1, sizeof h, ex.f) != sizeof h)
                ex.erreur = 1;
        }
        long_sd = ex_flux_songdata();
        memset(long_blocs, 0, sizeof long_blocs);
        for (k = 0; k < nb_blocs; k++)
            long_blocs[k] = ex_flux_patterns(k);
        ex_vide();
        if (!passe) {                             /* la CRC d'AT2 : les donnees, puis les tailles sur 16 bits */
            uint8_t l2[2];
            crc = ex.crc;
            for (k = 0; k < 17; k++) {
                const uint32_t v = k ? long_blocs[k - 1] : long_sd;
                l2[0] = (uint8_t)v;
                l2[1] = (uint8_t)(v >> 8);
                crc = ex_crc(crc, l2, 2);
            }
        } else {
            const int err = ex.erreur;
            if (fclose(ex.f) != 0 || err) {
                remove(chemin);
                a2m_message(msg, cap, "CANNOT WRITE A2M (FULL?)");
                return false;
            }
        }
    }
    snprintf(msg, (size_t)cap, "%d PATTERNS", ex_nb_patterns);
    return true;
}

/* Le morceau tient-il TOUT ENTIER dans un .A2M ? (SAVE : un morceau ouvert d'un .A2M s'y reenregistre tant que
   rien ne se perdrait.) Ce que l'export ne garde pas fait dire non : VEL, CMD, FX3 / FX4, une table, un sample,
   la DRUM MACHINE, une piste au-dela des OPL3, une ligne de SONG apres la premiere vide, une ligne au-dela de la
   longueur des patterns. Seul le H qui traduit cette longueur (pose a l'ouverture du .A2M) ne compte pas. */
bool fm1_morceau_tient_en_a2m(void)
{
    const md_module_t *m = md_replayer_get_module();
    int t, k, r, p, lignes = 0;
    if (!m)
        return false;
    for (k = 0; k < 16; k++)
        if (m->drum_pas[k])
            return false;
    while (lignes < MD_SONG_ROWS) {               /* (comme l'export : jusqu'a la premiere ligne vide partout) */
        int quelque = 0;
        for (t = 0; t < m->num_channels && t < MD_PISTES_OPL && !quelque; t++)
            quelque = m->song[t][lignes] != MD_EMPTY;
        if (!quelque)
            break;
        lignes++;
    }
    for (t = 0; t < m->num_channels && t < MD_MAX_CHANNELS; t++)
        for (k = 0; k < MD_SONG_ROWS; k++) {
            const uint8_t ch = m->song[t][k];
            if (ch == MD_EMPTY)
                continue;
            if (t >= MD_PISTES_OPL || k >= lignes)
                return false;
            for (r = 0; r < MD_ROWS_PER_CHAIN; r++) {
                const md_chain_row_t *cr = &m->chains[ch % MD_MAX_CHAINS].rows[r];
                if (cr->phrase == MD_EMPTY)
                    continue;
                for (p = 0; p < MD_ROWS_PER_PHRASE; p++) {
                    md_phrase_row_t rangee;       /* (copiee : lue en place, une lecture « b[++r=-6] » que
                                                     l'emulateur en ligne ne connait pas) */
                    const md_phrase_row_t *pr = &rangee;
                    memcpy(&rangee, &m->phrases[cr->phrase % MD_MAX_PHRASES].rows[p], sizeof rangee);
                    const int ligne = r * MD_ROWS_PER_PHRASE + p;
                    const int saut = pr->cmd != MD_EMPTY && md_table_cmd_kind(pr->cmd) == MD_CMD_HOP &&
                                     m->patt_len && ligne == (int)m->patt_len;
                    const int vide = !pr->note && !pr->instr && pr->mdcmd == MD_EMPTY && pr->cc[0] == MD_EMPTY;
                    if (pr->vel != MD_EMPTY || (pr->cmd != MD_EMPTY && !saut) || pr->cc[1] != MD_EMPTY ||
                        pr->cc[2] != MD_EMPTY)
                        return false;
                    if (m->patt_len && ligne >= (int)m->patt_len && !vide)
                        return false;             /* (au-dela de la longueur des patterns : perdu) */
                    if (pr->instr) {
                        const md_instr_t *in = &m->instruments[(pr->instr - 1) % MD_MAX_INSTRUMENTS];
                        if (in->kind != MD_INSTR_KIND_OPL || in->table != MD_EMPTY || in->perc)   /* (PERC : AT2 non) */
                            return false;
                    }
                }
            }
        }
    return true;
}
