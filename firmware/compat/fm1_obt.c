// ============================================================================
//  Les projets .OBT (OPLBend Tracker) : le fichier du moteur (celui des .XT,
//  214 Ko a taille fixe), compresse d'un seul tenant dans le LZH d'AdLib
//  Tracker II (le compresseur en flux de fm1_a2m_export.c, le decodeur en flux
//  de fm1_a2m.c) : un projet tient en quelques Ko, comme un .A2M.
//  C'est fm1_fs.c qui s'en charge : fopen d'un nom en .OBT ouvre le fichier
//  brut dessous, et fread / fwrite / fseek passent par ici ; le moteur lit et
//  ecrit comme avant.
//    « OBT1 », le flux LZH (5 octets, puis les blocs, puis la fin), et la
//    taille decompressee (4 octets, a la fin : elle n'est connue qu'apres).
//  En lecture, la fenetre de 16 Ko du decodeur sert aussi de tampon : on lit
//  en avancant ; un retour en arriere recommence au debut.
// ============================================================================

static int fs_est_obt(const char *chemin)
{
    const uint32_t n = (uint32_t)strlen(chemin);
    return n >= 4u && !strcasecmp(chemin + n - 4u, ".OBT");
}

static struct fm1_fichier *obt_place(void)
{
    uint32_t i;
    for (i = 0; i < sizeof fs_fichiers / sizeof fs_fichiers[0]; i++)
        if (fs_fichiers[i].mode == F_LIBRE)
            return &fs_fichiers[i];
    fs_err = FS_E_FICHIERS;
    return 0;
}

static int obt_recommence(FILE *f)                /* le flux, depuis le debut */
{
    struct fm1_fichier *b = f->brut;
    if (fseek(b, 4, SEEK_SET))
        return -1;
    lz_debut(b, b->taille - 8u);
    f->fen_debut = 0;
    f->fen_n = 0;
    return 0;
}

static FILE *obt_ouvre(const char *chemin, const char *mode)
{
    struct fm1_fichier *f = obt_place(), *b;
    uint8_t t[4];
    if (!f)
        return 0;
    memset(f, 0, sizeof *f);
    f->mode = mode[0] == 'r' ? F_OBT_L : F_OBT_E;   /* (la place est prise) */
    fs_brut = 1;
    b = fopen(chemin, mode[0] == 'r' ? "rb" : "wb");
    fs_brut = 0;
    if (!b) {
        f->mode = F_LIBRE;
        return 0;
    }
    f->brut = b;
    if (f->mode == F_OBT_L) {
        f->fen = (uint8_t *)malloc(LZ_DIC);
        lz = (lz_tables_t *)malloc(sizeof *lz);
        if (!f->fen || !lz || b->taille < 8u || fread(t, 1, 4, b) != 4 || memcmp(t, "OBT1", 4) ||
            fseek(b, -4, SEEK_END) || fread(t, 1, 4, b) != 4) {
            obt_ferme(f);
            return 0;
        }
        f->taille = (uint32_t)t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 | (uint32_t)t[3] << 24;
        if (obt_recommence(f)) {
            obt_ferme(f);
            return 0;
        }
        return f;
    }
    ew = (ex_travail_t *)malloc(EX_TRAVAIL_OBT);
    if (!ew) {
        fs_err = FS_E_MEMOIRE;
        fclose(b);
        remove(chemin);
        f->mode = F_LIBRE;
        return 0;
    }
    memset(&ex, 0, sizeof ex);
    ex.f = b;
    if (fwrite("OBT1", 1, 4, b) != 4)
        ex.erreur = 1;
    ex_debut(0xFFFFFFFFu);                        /* (la taille : a la fin) */
    return f;
}

static size_t obt_ecrit(const uint8_t *s, size_t total, FILE *f)
{
    if (!ew)
        return 0;
    ex_entre(s, (uint32_t)total);
    f->taille += (uint32_t)total;
    f->pos = f->taille;
    fm1_wdt_feed();
    return ex.erreur ? 0 : total;
}

static size_t obt_lit(uint8_t *d, size_t total, FILE *f)
{
    size_t fait = 0;
    while (fait < total) {
        uint32_t m;
        if (f->pos >= f->taille) {
            f->fin = 1;
            break;
        }
        while (f->pos >= f->fen_debut + f->fen_n) {   /* le paquet suivant du flux */
            const uint32_t debut = f->fen_debut + f->fen_n, reste = f->taille - debut;
            const uint32_t n = lz_lit(reste < LZ_DIC ? reste : LZ_DIC, f->fen);
            if (!n || lz_mauvais) {
                f->erreur = 1;
                return fait;
            }
            f->fen_debut = debut;
            f->fen_n = n;
            fm1_wdt_feed();
        }
        m = f->fen_debut + f->fen_n - f->pos;
        if (m > total - fait)
            m = (uint32_t)(total - fait);
        memcpy(d + fait, f->fen + (f->pos - f->fen_debut), m);
        fait += m;
        f->pos += m;
    }
    return fait;
}

static int obt_cherche(FILE *f, long position)
{
    if (position < 0 || position > (long)f->taille)
        return -1;
    if ((uint32_t)position < f->fen_debut && obt_recommence(f))   /* en arriere : depuis le debut */
        return -1;
    f->pos = (uint32_t)position;
    f->fin = 0;
    return 0;
}

static int obt_ferme(FILE *f)
{
    int rc = 0;
    if (f->mode == F_OBT_E) {
        uint8_t t[4];
        if (ew) {
            ex_fin();
            ex_vide();
            free(ew);
            ew = 0;
        }
        t[0] = (uint8_t)f->taille; t[1] = (uint8_t)(f->taille >> 8);
        t[2] = (uint8_t)(f->taille >> 16); t[3] = (uint8_t)(f->taille >> 24);
        if (ex.erreur || fwrite(t, 1, 4, f->brut) != 4)
            rc = EOF;
        if (fclose(f->brut))
            rc = EOF;
    } else {
        if (f->brut)
            fclose(f->brut);
        free(f->fen);
        free(lz);
        lz = 0;
    }
    f->mode = F_LIBRE;
    return rc;
}
