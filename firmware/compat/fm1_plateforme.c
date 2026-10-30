// ============================================================================
//  Voir nds.h : l'ecran, les touches et les timers de la DS, sur le FM-1.
//  (Calque sur compat/rg_plateforme.cpp de la version RG350.)
// ============================================================================

// ── L'ecran ─────────────────────────────────────────────────────────────────
// Le tracker dessine dans une image de 320 x 240 en couleurs DS (15 bits),
// gardee ici en 8 bits : un indice dans une palette qui se remplit au fil des
// couleurs rencontrees (le tracker en emploie quelques dizaines). L'afficheur
// montre une fenetre de 240 points de large, decalee de s_vue_x : elle suit
// le curseur (les points dessines dans la couleur d'accent du tracker, sauf
// le chevron de lecture) et repart a gauche quand une page est repeinte.
// L'« ecran du bas » partage cette image.
static uint8_t s_ecran[RG_ECRAN_L * RG_ECRAN_H] __attribute__((aligned(4)));
static u16 s_palette[256];
static uint32_t s_pal_n = 1;                      /* 0 : le noir */
static int32_t s_vue_x, s_vue_x_vu = -1;          /* le decalage montre, et celui de l'image envoyee */
static int32_t s_vue_y, s_vue_y_vu = -1;
static int32_t s_accy_min = 0x7FFFFFFF, s_accy_max = -1;
static int32_t s_acc_min = 0x7FFFFFFF, s_acc_max = -1;   /* l'accent dessine depuis la derniere image */
static int32_t s_chevy_min = 0x7FFFFFFF, s_chevy_max = -1;   /* le repere de lecture redessine depuis */
/* ZOOM (menu PROJECT) : tout en plus gros, 1 = 4 COL (quatre pistes sur SONG), 2 = 3 COL (trois, les
   lignes 0 a 7 a l'ecran) ; la vue avance d'une colonne ou d'une ligne a la fois. Pendant la lecture,
   tant qu'on ne touche pas au curseur, elle suit le repere de lecture (s_suivre, que main.cpp tient a jour). */
static uint8_t s_zoom, s_suivre, s_retour_curseur;
static int32_t s_curseur_y = -1;                  /* la derniere rangee ou le curseur a ete dessine */
static void zoom_cartes(void);
void fm1_zoom(int niveau)
{
    const uint8_t z = (uint8_t)(niveau < 0 ? 0 : niveau > 2 ? 2 : niveau);
    if (s_zoom != z) {
        s_zoom = z;
        zoom_cartes();
        s_vue_x = s_vue_y = 0;
        s_vue_x_vu = -1;                          /* (tout repart a l'ecran) */
    }
}
void fm1_suivre_lecture(int on)
{
    if (s_suivre && !on)
        s_retour_curseur = 1;                     /* on reprend la main : la vue revient au curseur */
    s_suivre = (uint8_t)(on != 0);
}
void fm1_vu_chevron(int i)                        /* i : un point du repere de lecture */
{
    const int y = i / RG_ECRAN_L;
    if (y < s_chevy_min) s_chevy_min = y;
    if (y > s_chevy_max) s_chevy_max = y;
}

int bgInit(int c, BgType t, BgSize s2, int a, int b) { (void)c; (void)t; (void)s2; (void)a; (void)b; return 0; }
int bgInitSub(int c, BgType t, BgSize s2, int a, int b) { (void)c; (void)t; (void)s2; (void)a; (void)b; return 1; }
u16 *bgGetGfxPtr(int id) { (void)id; return (u16 *)(void *)s_ecran; }   /* (le tracker n'y ecrit pas sur le FM-1) */

static uint8_t palette_indice(u16 c)
{
    static u16 dernier[2];
    static uint8_t dernier_i[2] = {0, 0};
    uint32_t i;
    c &= 0x7FFFu;
    if (c == dernier[0]) return dernier_i[0];
    if (c == dernier[1]) return dernier_i[1];
    for (i = 0; i < s_pal_n; i++)
        if (s_palette[i] == c)
            break;
    if (i == s_pal_n) {
        if (s_pal_n < 256u)
            s_palette[s_pal_n++] = c;
        else
            i = 0;                                /* (plus de place : noir) */
    }
    dernier[1] = dernier[0];
    dernier_i[1] = dernier_i[0];
    dernier[0] = c;
    dernier_i[0] = (uint8_t)i;
    return (uint8_t)i;
}

void fm1_px(int i, u16 c) { s_ecran[i] = palette_indice(c); }
u16 fm1_px_lit(int i) { return (u16)(s_palette[s_ecran[i]] | 0x8000u); }
void fm1_vu_accent(int i)                         /* i : l'indice du point dans l'image */
{
    const int x = i % RG_ECRAN_L, y = i / RG_ECRAN_L;
    if (x < s_acc_min) s_acc_min = x;
    if (x > s_acc_max) s_acc_max = x;
    if (y < s_accy_min) s_accy_min = y;
    if (y > s_accy_max) s_accy_max = y;
    s_curseur_y = y;
}
static int32_t s_rangee_titres = -1;              /* la rangee des titres de colonnes (main.cpp), -1 : aucune */
static uint8_t s_titres_gros;                     /* les titres un peu plus gros : comme l'en-tete (6 x 8) */
void fm1_titres_colonnes(int rangee) { s_rangee_titres = rangee; s_titres_gros = 0; }   /* (en police d'origine, 5 x 7) */
void fm1_titres_colonnes_gros(int rangee) { s_rangee_titres = rangee; s_titres_gros = 1; }

/* Le style d'une rangee, choisi par la page (remis a 0 a chaque page) :
   ESPACE : une rangee vide qu'on GARDE (les vides sont sinon retirees), de FM1_ESPACE_H points ;
   MOYEN  : une police un peu plus grande que celle des pages (cellules de 12 x 16, lettres de 10 x 15). */
enum { STYLE_PAGE, STYLE_ESPACE, STYLE_MOYEN, STYLE_LIGNE_VIDE, STYLE_AERE, STYLE_PETIT, STYLE_DRUM, STYLE_LOOP };   /* LIGNE_VIDE : gardee, de la hauteur
   d'une ligne ; AERE : une ligne, et un filet de fond dessous (l'aide) ; PETIT : a la taille de l'en-tete, et un filet
   (l'aide du D-PAD : des lignes plus longues) */
#define FM1_FILET_H 3u
#define FM1_ESPACE_H 7u
#define FM1_MOYEN_W 12
#define FM1_MOYEN_H 16u
static uint8_t s_style[RG_ECRAN_H / FM1_CELL_H];
void fm1_rangee_espace(int rangee) { if (rangee > 0 && rangee < RG_ECRAN_H / FM1_CELL_H) s_style[rangee] = STYLE_ESPACE; }
void fm1_rangee_vide(int rangee) { if (rangee > 0 && rangee < RG_ECRAN_H / FM1_CELL_H) s_style[rangee] = STYLE_LIGNE_VIDE; }
void fm1_rangee_normale(int rangee) { if (rangee > 0 && rangee < RG_ECRAN_H / FM1_CELL_H) s_style[rangee] = 0; }   /* (son style d'avant, oublie) */
/* DRUM : la grille de la DRUM MACHINE, a sa mesure (le nom, un espace etroit, seize pas de 13 points ; des
   rangees de 18) : l'ecran presque entier */
#define FM1_DRUM_H 18u
void fm1_rangee_drum(int rangee) { if (rangee > 0 && rangee < RG_ECRAN_H / FM1_CELL_H) s_style[rangee] = STYLE_DRUM; }
/* LOOP : la grille du LOOPER, comme la DRUM MACHINE, avec de l'air sous chaque rangee (ses couches, moins serrees) */
#define FM1_LOOP_AIR 6u
void fm1_rangee_loop(int rangee) { if (rangee > 0 && rangee < RG_ECRAN_H / FM1_CELL_H) s_style[rangee] = STYLE_LOOP; }
void fm1_rangee_petite(int rangee) { if (rangee > 0 && rangee < RG_ECRAN_H / FM1_CELL_H) s_style[rangee] = STYLE_PETIT; }
void fm1_rangee_aeree(int rangee) { if (rangee > 0 && rangee < RG_ECRAN_H / FM1_CELL_H) s_style[rangee] = STYLE_AERE; }
void fm1_rangee_moyenne(int rangee) { if (rangee > 0 && rangee < RG_ECRAN_H / FM1_CELL_H) s_style[rangee] = STYLE_MOYEN; }

/* Sans en-tete (page FX : son bouton allume dit deja ou l'on est) */
static uint8_t s_sans_entete;
void fm1_sans_entete(int oui) { s_sans_entete = (uint8_t)(oui != 0); }
/* ou commencent les textes, par rangee : deux titres dans des cellules voisines (PHR, TSP) restent separes */
/* (deux mots de 32 bits par rangee, ecrits et lus de la meme facon : un |= sur 64 bits devenait d[++r1=r2],
   que l'emulateur en ligne ne lit pas ; et un uint64_t vu en deux moities ne range pas forcement le bas d'abord) */
static uint32_t s_mots[RG_ECRAN_H / FM1_CELL_H][2];
void fm1_debut_mot(int col, int lig)
{
    if (col >= 0 && col < 64 && lig >= 0 && lig < RG_ECRAN_H / FM1_CELL_H)
        s_mots[lig][(uint32_t)col >> 5] |= 1u << ((uint32_t)col & 31u);
}
static int mot_debute(int lig, int col)
{
    return (s_mots[lig][(uint32_t)col >> 5] >> ((uint32_t)col & 31u)) & 1u;
}

// L'EN-TETE : la rangee 0 de l'image (le repere « PH 00 », « INS 01 »...) en
// haut de l'ecran, a sa taille d'origine (5 x 7 : tout petit), qui ne defile
// jamais ; le nom de la page s'y ecrit a droite.
#define FM1_ENTETE_H FM1_CELL_H                   /* dans l'image : la rangee 0 */
#define FM1_ENTETE_ECRAN_H 9                      /* a l'ecran */
/* Chaque ZOOM : la cellule des valeurs (w x h) et sa lettre (l, sans l'espace), celle de l'en-tete (ew x eh) */
static const struct { uint8_t w, h, l, ew, eh; } ZOOMS[3] = {
    {0, 0, 0, 0, 0},                              /* (sans zoom : les tailles des pages) */
    {16, 22, 14, 8, 11},                          /* 4 COL : lettres de 14 x 20, en-tete de 7 x 10 */
    {20, 27, 17, 10, 13},                         /* 3 COL : lettres de 17 x 25, en-tete de 9 x 12 */
};
#define FM1_ENTETE_ZOOM_MAX 13u
#define ENTETE_LETTRES_H() (s_zoom ? (uint32_t)ZOOMS[s_zoom].eh : FM1_ENTETE_ECRAN_H)
#define ENTETE_H() (s_sans_entete ? 0u : ENTETE_LETTRES_H() + s_espace_entete)
/* un espace sous l'en-tete (INSTR : ses titres MOD / CAR y sont), remis a 0 a chaque page */
static uint32_t s_espace_entete;
void fm1_espace_sous_entete(int points) { s_espace_entete = points > 0 ? (uint32_t)points : 0u; }
/* Des titres DANS l'en-tete (INSTR : MOD, CAR), centres au point pres sur une colonne de valeurs :
   le mot est ecrit dans la rangee 0 a la cellule `col_mot` ; il s'affiche centre sur les `n_cases`
   cellules de valeurs qui commencent a `col_valeurs`. Remis a zero a chaque page. */
/* Des valeurs decalees d'une demi-cellule (INSTR : deux caractères centres dans une colonne de trois).
   fm1_decale_demi(rangee, col, n) : les n cellules a partir de col, sur cette rangee de l'image. */
#define NB_DECALES 48u
static struct { uint8_t rangee, col, n; } s_decales[NB_DECALES];
static uint32_t s_nb_decales;
void fm1_decales_efface(void) { s_nb_decales = 0; }
void fm1_decale_demi(int rangee, int col, int n)
{
    if (s_nb_decales < NB_DECALES && rangee > 0 && col >= 0 && n > 0) {
        s_decales[s_nb_decales].rangee = (uint8_t)rangee;
        s_decales[s_nb_decales].col = (uint8_t)col;
        s_decales[s_nb_decales++].n = (uint8_t)n;
    }
}
#define NB_MOTS_ENTETE 8u
static struct { uint8_t col_mot, n, col_val, n_cases; } s_mots_entete[NB_MOTS_ENTETE];
static uint32_t s_nb_mots_entete;
void fm1_mots_entete_efface(void) { s_nb_mots_entete = 0; }   /* (SONG en ZOOM : les pistes ont defile) */
void fm1_mot_entete(int col_mot, int n_lettres, int col_valeurs, int n_cases)
{
    if (s_nb_mots_entete < NB_MOTS_ENTETE && col_mot >= 0 && col_valeurs >= 0) {
        s_mots_entete[s_nb_mots_entete].col_mot = (uint8_t)col_mot;
        s_mots_entete[s_nb_mots_entete].n = (uint8_t)n_lettres;
        s_mots_entete[s_nb_mots_entete].col_val = (uint8_t)col_valeurs;
        s_mots_entete[s_nb_mots_entete++].n_cases = (uint8_t)n_cases;
    }
}
/* en ZOOM, les titres de colonnes grossissent aussi, moins que le reste (lettres de 6 x 8) */
#define TITRES_GROS (s_titres_gros || s_zoom)
/* combien de cellules de la rangee 0 l'en-tete montre (main.cpp y cale le repere « PH 00 » a droite) */
int fm1_entete_cellules(void) { return s_zoom ? 240 / ZOOMS[s_zoom].ew : 34; }
int fm1_largeur_cellule_entete(void) { return s_zoom ? ZOOMS[s_zoom].ew : 7; }
static char s_nom_page[16];
static uint8_t s_clavier = 1;                     /* le mode KEYS (SEQ clignote) : les 27 touches sont un clavier */
static uint8_t s_drumkeys;                        /* GLO, troisieme mode : DRUM KEYBOARD (une batterie par touche) */
static uint8_t s_drum;                            /* le mode DRUM MACHINE (SEQ une troisieme fois) : les touches aussi */
static uint8_t s_led_drum[27], s_led_drum_la;     /* ... la lumiere de chaque touche, posee par le tracker */
static uint8_t s_compacte = 1;                    /* la taille de PHRASE (cellules de 11 x 14), sur TOUTES les pages */
void fm1_nom_page(const char *nom)
{
    str_cpy(s_nom_page, nom, sizeof s_nom_page);
}

/* la largeur de la fenetre, en points de l'image : PHRASE a des cellules plus etroites */
#define FM1_ZOOM_W ((int32_t)ZOOMS[s_zoom].w)
#define FM1_ZOOM_H ((uint32_t)ZOOMS[s_zoom].h)
#define FM1_ZOOM_H_MAX 27u
#define FM1_VUE_L_ZOOM ((uint32_t)(240 * FM1_CELL_W + FM1_ZOOM_W - 1) / (uint32_t)FM1_ZOOM_W)
int fm1_largeur_cellule(void) { return s_zoom ? FM1_ZOOM_W : s_compacte ? 11 : FM1_CELL_ECRAN_W; }
static uint32_t vue_l(void) { return s_zoom ? FM1_VUE_L_ZOOM : s_compacte ? FM1_VUE_L_COMPACTE : FM1_VUE_L; }

static void entete_nom_page(void)                 /* le nom, a gauche de la rangee 0, a chaque image */
{
    const uint32_t lg = (uint32_t)strlen(s_nom_page);
    const uint8_t encre = palette_indice(RGB15(29, 27, 31));
    uint32_t y, x, k;
    if (!lg)
        return;
    for (y = 0; y < FM1_CELL_H; y++) {            /* sa place : les 8 premieres colonnes */
        const uint8_t fond = palette_indice((y & 1u) ? RGB15(0, 1, 1) : 0);   /* (la trame du tracker) */
        for (x = 0; x < 8u * FM1_CELL_W; x++)
            s_ecran[y * RG_ECRAN_L + x] = fond;
    }
    if (!strcmp(s_nom_page, "PHRASE") || !strcmp(s_nom_page, "SONG") || !strcmp(s_nom_page, "CHAIN")) {
        /* a cote du nom : « KEYBOARD » en mode clavier (le nom que SEQ annonce) */
        const char *mot = s_drumkeys ? "DRUM KEYS" : "KEYBOARD";
        const uint8_t couleur = encre;
        for (y = 0; y < FM1_CELL_H; y++) {
            const uint8_t fond = palette_indice((y & 1u) ? RGB15(0, 1, 1) : 0);
            for (x = 9u * FM1_CELL_W; x < 18u * FM1_CELL_W; x++)
                s_ecran[y * RG_ECRAN_L + x] = fond;
        }
        for (k = 0; s_clavier && !s_drum && !s_nb_mots_entete && mot[k]; k++)   /* (SONG en ZOOM : les pistes y sont) */
            for (y = 0; y < MD_FONT_HT; y++)
                for (x = 0; x < MD_FONT_W; x++)
                    if (md_font[mot[k] - 0x20u][y] & (0x10u >> x))
                        s_ecran[y * RG_ECRAN_L + FM1_CELL_W * (9u + k) + x] = couleur;
    }
    for (k = 0; k < lg && k < 8u; k++) {
        uint32_t c = (uint8_t)s_nom_page[k];
        if (c < 0x20u || c >= 0x60u)
            continue;
        for (y = 0; y < MD_FONT_HT; y++)
            for (x = 0; x < MD_FONT_W; x++)
                if (md_font[c - 0x20u][y] & (0x10u >> x))
                    s_ecran[y * RG_ECRAN_L + FM1_CELL_W * k + x] = encre;
    }
}

void fm1_vu_page(void)
{
    s_rangee_titres = -1;
    memset(s_mots, 0, sizeof s_mots);
    memset(s_style, 0, sizeof s_style);
    s_vue_x = s_vue_y = 0;
    s_acc_min = s_accy_min = 0x7FFFFFFF;
    s_acc_max = s_accy_max = -1;
    s_curseur_y = -1;
    s_espace_entete = 0;
    s_nb_mots_entete = 0;
    s_nb_decales = 0;
}

/* une coordonnee de la fenetre : `v` (decalage), `lmin..lmax` (l'accent), sur `taille` montres de `total` */
static int32_t vue_axe(int32_t v, int32_t lmin, int32_t lmax, int32_t taille, int32_t total, int32_t cellule)
{
    /* (zoom : une colonne de marge, et on avance d'une colonne a la fois) */
    const int32_t bord = (s_zoom ? 1 : 2) * cellule, avance = (s_zoom ? 1 : 5) * cellule, maxi = total - taille;
    if (lmax < 0 || maxi <= 0)
        return maxi <= 0 ? 0 : v;
    if (lmax >= v + taille - bord)
        v = lmax - taille + avance + 1;
    if (lmin < v + bord && lmax - lmin < taille - 2 * avance)
        v = lmin - avance;
    v -= v % cellule;
    if (v < 4 * cellule && lmax < taille - bord)  /* pres du bord gauche : tout a gauche (les numeros de ligne) */
        v = 0;
    if (v < 0) v = 0;
    if (v > maxi) v = maxi;
    return v;
}

// Les rangees de cellules MONTREES : celles qui portent quelque chose dans la
// partie visible. Les rangees vides (titres absents, separations) ne prennent
// pas de place a l'ecran ; s_vue_y est un indice dans cette liste.
#define NB_RANGEES (RG_ECRAN_H / FM1_CELL_H)
#define RANGEES_ECRAN (240 / FM1_CELL_ECRAN_H)
static uint8_t s_rangees[NB_RANGEES], s_nb_rangees;

/* Les couleurs du DECOR (les spaghettis entre les colonnes) : pour trouver les cases et les rangees vides
   (la place des titres, les rangees a montrer), elles comptent comme le fond. */
/* (une table, une case par couleur de la palette : ce test se fait pour chaque point analyse, et la liste
   parcourue a chaque fois ralentissait tout l'affichage) */
static uint8_t s_decor[8], s_nb_decor, s_fond_tab[256], s_fond_f0 = 0xFF, s_fond_f1 = 0xFF, s_fond_sale = 1;
static uint32_t s_fond_gen;                       /* (change avec la table : les analyses gardees ne valent plus) */
void fm1_couleurs_decor(const u16 *c, int n)
{
    int i;
    uint8_t d[8];
    const uint8_t nb = (uint8_t)(n < 0 ? 0 : n > 8 ? 8 : n);
    for (i = 0; i < nb; i++)
        d[i] = palette_indice(c[i]);
    if (nb != s_nb_decor || memcmp(d, s_decor, nb)) {
        memcpy(s_decor, d, nb);
        s_nb_decor = nb;
        s_fond_sale = 1;
    }
}
static inline int est_fond(uint8_t v, uint8_t f0, uint8_t f1)
{
    if (s_fond_sale || f0 != s_fond_f0 || f1 != s_fond_f1) {
        uint32_t k;
        memset(s_fond_tab, 0, sizeof s_fond_tab);
        s_fond_tab[f0] = s_fond_tab[f1] = 1;
        for (k = 0; k < s_nb_decor; k++)
            s_fond_tab[s_decor[k]] = 1;
        s_fond_f0 = f0;
        s_fond_f1 = f1;
        s_fond_sale = 0;
        s_fond_gen++;
    }
    return s_fond_tab[v];
}

/* L'EMPREINTE de chaque ligne de l'image, une fois par image, lue par mots (s_ecran est aligne) : les
   analyses (rangees vides, largeur de la page, lignes a renvoyer) ne relisent plus les points d'une ligne
   qui n'a pas change. Sans elles, chaque image relisait tout l'ecran plusieurs fois, octet par octet : avec
   les effets, que le son prend sur le processeur, l'interface ralentissait. */
static uint32_t s_h_ligne[RG_ECRAN_H];
static void empreintes_image(void)
{
    const uint32_t *m = (const uint32_t *)(const void *)s_ecran;
    uint32_t y, i;
    for (y = 0; y < RG_ECRAN_H; y++) {
        uint32_t h = 2166136261u;
        for (i = 0; i < RG_ECRAN_L / 4u; i++)
            h = (h ^ *m++) * 16777619u;
        s_h_ligne[y] = h;
    }
}
static uint32_t cle_analyse(uint32_t y0, uint32_t n)   /* les lignes y0.., la fenetre et la table du fond */
{
    uint32_t h = s_fond_gen * 2654435761u ^ (uint32_t)s_vue_x << 20 ^ (uint32_t)s_zoom << 30 ^ (uint32_t)s_compacte << 31, k;
    for (k = 0; k < n; k++)
        h = (h ^ s_h_ligne[y0 + k]) * 16777619u;
    return h;
}

static int rangee_vide(uint32_t r)
{
    static uint32_t cle[NB_RANGEES];
    static uint8_t vide[NB_RANGEES], connue[NB_RANGEES];
    const uint8_t f0 = palette_indice(0), f1 = palette_indice(RGB15(0, 1, 1));   /* les deux fonds de la trame */
    uint32_t y, x, c;
    if (s_style[r] == STYLE_ESPACE || s_style[r] == STYLE_LIGNE_VIDE)   /* (gardee : un espace voulu) */
        return 0;
    est_fond(0, f0, f1);                          /* (la table a jour : s_fond_gen) */
    c = cle_analyse(r * FM1_CELL_H, FM1_CELL_H);
    if (connue[r] && cle[r] == c)
        return vide[r];
    cle[r] = c;
    connue[r] = 1;
    vide[r] = 0;
    for (y = r * FM1_CELL_H; y < (r + 1u) * FM1_CELL_H; y++) {
        const uint8_t *l = &s_ecran[y * RG_ECRAN_L + (uint32_t)s_vue_x];
        for (x = 0; x < vue_l() && (uint32_t)s_vue_x + x < RG_ECRAN_L; x++)
            if (!est_fond(l[x], f0, f1))
                return 0;
    }
    vide[r] = 1;
    return 1;
}

static uint32_t largeur_contenu(void)              /* jusqu'ou la page dessine quelque chose (sous l'en-tete) */
{
    const uint8_t f0 = palette_indice(0), f1 = palette_indice(RGB15(0, 1, 1));   /* les deux fonds de la trame */
    static uint32_t cle[RG_ECRAN_H];
    static uint8_t bout[RG_ECRAN_H];              /* (par ligne : son dernier point dessine, gardes) */
    uint32_t y, x, fin = 0;
    est_fond(0, f0, f1);
    for (y = FM1_CELL_H; y < RG_ECRAN_H; y++) {
        const uint32_t c = (s_h_ligne[y] ^ s_fond_gen * 2654435761u) | 1u;   /* (0 : jamais vue) */
        if (cle[y] != c) {
            const uint8_t *l = &s_ecran[y * RG_ECRAN_L];
            cle[y] = c;
            bout[y] = 0;
            for (x = RG_ECRAN_L - 1u; x > 0u; x--)
                if (!est_fond(l[x], f0, f1)) {     /* (le decor ne compte pas : il n'elargit pas la page) */
                    bout[y] = (uint8_t)x;
                    break;
                }
        }
        if (bout[y] > fin)
            fin = bout[y];
    }
    return fin + 1u;
}

/* Les titres de colonnes restent sous l'en-tete quand la page defile, s'il n'y a rien au-dessus d'eux
   (sur FX, la legende des molettes y est : ils defilent alors avec le reste). */
static uint8_t s_titres_fixes;
static void titres_fixes_calcule(void)
{
    int32_t r;
    s_titres_fixes = 0;
    if (s_rangee_titres < 1 || s_rangee_titres >= (int32_t)NB_RANGEES || rangee_vide((uint32_t)s_rangee_titres))
        return;
    for (r = 1; r < s_rangee_titres; r++)
        if (!rangee_vide((uint32_t)r))
            return;
    s_titres_fixes = 1;
}
static uint32_t hauteur_titres(void)
{
    return s_titres_fixes ? (TITRES_GROS ? FM1_ENTETE_ECRAN_H : FM1_CELL_TITRE_H) : 0u;
}

static uint32_t hauteur_rangee(uint32_t i)        /* a l'ecran, la rangee montree i */
{
    if ((int32_t)s_rangees[i] == s_rangee_titres)
        return TITRES_GROS ? FM1_ENTETE_ECRAN_H : FM1_CELL_TITRE_H;
    if (s_style[s_rangees[i]] == STYLE_ESPACE)
        return FM1_ESPACE_H;
    if (s_style[s_rangees[i]] == STYLE_MOYEN)
        return FM1_MOYEN_H;
    if (s_style[s_rangees[i]] == STYLE_PETIT)
        return ENTETE_LETTRES_H() + FM1_FILET_H;
    if (s_style[s_rangees[i]] == STYLE_DRUM)
        return FM1_DRUM_H;
    if (s_style[s_rangees[i]] == STYLE_LOOP)
        return FM1_DRUM_H + FM1_LOOP_AIR;
    return (s_zoom ? FM1_ZOOM_H : s_compacte ? FM1_CELL_COMPACTE_H : FM1_CELL_ECRAN_H)
           + (s_style[s_rangees[i]] == STYLE_AERE ? FM1_FILET_H : 0u);
}

/* une page qui tient toute dans l'ecran a sa maniere (la DRUM MACHINE, l'aide) : la fenetre reste a gauche */
static uint8_t s_vue_fixe;
void fm1_vue_fixe(int on) { s_vue_fixe = (uint8_t)(on != 0); }

static void vue_suit_curseur(void)               /* une fois par image */
{
    uint32_t r;
    if (s_vue_fixe || largeur_contenu() <= vue_l())   /* la page tient en largeur (SONG) : pas de glissement */
        s_vue_x = 0;
    else
        s_vue_x = vue_axe(s_vue_x, s_acc_min, s_acc_max, (int32_t)vue_l(), RG_ECRAN_L, FM1_CELL_W);
    uint32_t place;
    titres_fixes_calcule();
    place = 241u - ENTETE_H() - hauteur_titres(); /* (les titres fixes restent en haut) */
    const int32_t marge = s_zoom ? 1 : 2;
    /* ce qu'on suit en hauteur : le curseur, ou pendant la lecture (sans y toucher) le repere */
    int32_t suivi_y = s_suivre ? s_chevy_max : s_accy_max;
    if (!s_suivre && s_retour_curseur && suivi_y < 0)
        suivi_y = s_curseur_y;
    s_retour_curseur = 0;
    s_nb_rangees = 0;
    for (r = 1; r < NB_RANGEES; r++)              /* (la rangee 0 est l'en-tete, les titres a part) */
        if (!rangee_vide(r) && !(s_titres_fixes && (int32_t)r == s_rangee_titres))
            s_rangees[s_nb_rangees++] = (uint8_t)r;
    if (suivi_y >= 0) {                           /* sa rangee dans la liste, avec une marge */
        int32_t p = 0, fin;
        uint32_t h, i;
        while (p < (int32_t)s_nb_rangees && s_rangees[p] < (uint32_t)suivi_y / FM1_CELL_H)
            p++;
        if (p < s_vue_y + marge)
            s_vue_y = p - marge;
        if (s_vue_y < 0)
            s_vue_y = 0;
        fin = p + marge < (int32_t)s_nb_rangees ? p + marge : (int32_t)s_nb_rangees - 1;
        for (;;) {                                /* jusqu'a ce que la rangee `fin` tienne entiere */
            for (h = 0, i = (uint32_t)s_vue_y; (int32_t)i <= fin; i++)
                h += hauteur_rangee(i);
            if (h <= place || s_vue_y >= p)
                break;
            s_vue_y++;
        }
    }
    {   /* pas de vide en bas : on remonte tant qu'il reste de la place sous la derniere rangee */
        uint32_t h, i;
        for (;;) {
            for (h = 0, i = (uint32_t)s_vue_y; i < s_nb_rangees; i++)
                h += hauteur_rangee(i);
            if (s_vue_y == 0 || h + hauteur_rangee((uint32_t)s_vue_y - 1u) > place)
                break;
            s_vue_y--;
        }
    }
    s_acc_min = s_accy_min = 0x7FFFFFFF;
    s_acc_max = s_accy_max = -1;
    s_chevy_min = 0x7FFFFFFF;
    s_chevy_max = -1;
}

// L'agrandissement, cellule par cellule : la largeur a l'ecran de chacun des
// 6 points d'une cellule (les 5 du glyphe puis l'espace), et la hauteur de
// chacune des 8 lignes (les 7 du glyphe puis l'interligne). Un trait fait 2 ou
// 3 points, toujours au meme endroit de la cellule : toutes les lettres sont
// identiques.
static const uint8_t AGR_X[FM1_CELL_W] = {2, 3, 2, 3, 2, 2};           /* 14 */
static const uint8_t AGR_Y[FM1_CELL_H] = {3, 2, 3, 2, 3, 2, 3, 1};     /* 19 */
/* la rangee des titres de colonnes : la police d'origine (5 x 7), chaque mot cale sur sa colonne (carte_titres) */
static const uint8_t AGR_Y_TITRE[FM1_CELL_H] = {1, 1, 1, 1, 1, 1, 1, 1};   /* 8 */
/* PHRASE : la meme police, REDUITE dans les deux sens (lettres de 9 x 13 au lieu de 12 x 18, memes
   proportions) : en-tete, titres et 16 lignes de 14 remplissent exactement les 240 points (l'interligne
   de la ligne F tombe sous l'ecran) */
static const uint8_t AGR_X_COMPACTE[FM1_CELL_W] = {2, 2, 1, 2, 2, 2};       /* 11 */
static const uint8_t AGR_Y_COMPACTE[FM1_CELL_H] = {2, 2, 2, 1, 2, 2, 2, 1};   /* 14 */
/* l'en-tete (nom de la page, repere) : un peu plus gros que la police d'origine, lettres de 6 x 8 */
static const uint8_t AGR_X_ENTETE[FM1_CELL_W] = {1, 1, 2, 1, 1, 1};    /* 7 */
static const uint8_t AGR_Y_ENTETE[FM1_CELL_H] = {1, 1, 1, 2, 1, 1, 1, 1};   /* 9 */
_Static_assert(FM1_CELL_W == 6 && FM1_CELL_H == 8, "AGR_X / AGR_Y sont faites pour la cellule 6 x 8");
static uint16_t s_src_x[240], s_src_x_compacte[240], s_src_x_id[240], s_src_x_moyen[240];
static const uint8_t AGR_X_MOYEN[FM1_CELL_W] = {2, 2, 2, 2, 2, 2};        /* 12 : lettres de 10 */
static const uint8_t AGR_Y_MOYEN[FM1_CELL_H] = {2, 2, 2, 3, 2, 2, 2, 1};  /* 16 : lettres de 15 */
static const uint8_t AGR_Y_DRUM[FM1_CELL_H] = {2, 3, 2, 3, 2, 2, 2, 2};   /* 18 : lettres de 16 */
static uint16_t s_src_x_drum[240];
static uint8_t s_src_y_drum[FM1_DRUM_H];
static uint8_t s_src_y_moyen[FM1_MOYEN_H];
static uint16_t s_src_x_zoom[240];
static const uint8_t AGR_X_ZOOM[3][FM1_CELL_W] = {
    {0}, {3, 3, 2, 3, 3, 2},                      /* 4 COL : 16, lettres de 14 */
    {4, 3, 3, 3, 4, 3}};                          /* 3 COL : 20, lettres de 17 */
static const uint8_t AGR_Y_ZOOM[3][FM1_CELL_H] = {
    {0}, {3, 3, 3, 3, 3, 3, 2, 2},                /* 22 : lettres de 20 */
    {4, 3, 4, 3, 4, 3, 4, 2}};                    /* 27 : lettres de 25 (17 x 25 : 12 x 18 agrandi) */
static uint8_t s_src_y_zoom[FM1_ZOOM_H_MAX];
#define TITRE_VIDE 0xFFFFu
static uint16_t s_src_x_titre[240];               /* (refaite a chaque image : TITRE_VIDE = le fond) */
static uint8_t s_src_y_entete[FM1_ENTETE_ECRAN_H];
static uint16_t s_src_x_id_z[240];
static uint8_t s_src_y_entete_z[FM1_ENTETE_ZOOM_MAX];
static const uint8_t AGR_X_ENTETE_Z[3][FM1_CELL_W] = {
    {0}, {2, 1, 2, 1, 1, 1},                      /* 4 COL : 8, lettres de 7 */
    {2, 2, 2, 1, 2, 1}};                          /* 3 COL : 10, lettres de 9 */
static const uint8_t AGR_Y_ENTETE_Z[3][FM1_CELL_H] = {
    {0}, {2, 1, 2, 1, 2, 1, 1, 1},                /* 11 : lettres de 10 */
    {2, 2, 1, 2, 2, 1, 2, 1}};                    /* 13 : lettres de 12 */
static uint8_t s_src_y_compacte[FM1_CELL_COMPACTE_H];   /* colonne de l'afficheur -> point de l'image (relatif) */
static uint8_t s_src_y[FM1_CELL_ECRAN_H], s_src_y_titre[FM1_CELL_TITRE_H];   /* ligne dans la cellule */

static void agr_axe(uint16_t *dst, uint32_t n, const uint8_t *agr, uint32_t cellule)
{
    uint32_t c, k, r, i = 0;
    for (c = 0; i < n; c++)
        for (k = 0; k < cellule && i < n; k++)
            for (r = 0; r < agr[k] && i < n; r++)
                dst[i++] = (uint16_t)(c * cellule + k);
}

static void zoom_cartes(void)                    /* les cartes du ZOOM choisi (fm1_zoom) */
{
    uint16_t t[FM1_ZOOM_H_MAX];
    uint32_t i;
    if (!s_zoom)
        return;
    agr_axe(s_src_x_zoom, 240, AGR_X_ZOOM[s_zoom], FM1_CELL_W);
    agr_axe(t, FM1_ZOOM_H, AGR_Y_ZOOM[s_zoom], FM1_CELL_H);
    for (i = 0; i < FM1_ZOOM_H; i++) s_src_y_zoom[i] = (uint8_t)t[i];
    agr_axe(s_src_x_id_z, 240, AGR_X_ENTETE_Z[s_zoom], FM1_CELL_W);
    agr_axe(t, ENTETE_LETTRES_H(), AGR_Y_ENTETE_Z[s_zoom], FM1_CELL_H);
    for (i = 0; i < ENTETE_LETTRES_H(); i++) s_src_y_entete_z[i] = (uint8_t)t[i];
}

static void agr_init(void)
{
    uint16_t t[FM1_CELL_ECRAN_H];
    uint32_t i;
    agr_axe(s_src_x, 240, AGR_X, FM1_CELL_W);
    agr_axe(s_src_x_compacte, 240, AGR_X_COMPACTE, FM1_CELL_W);
    agr_axe(s_src_x_moyen, 240, AGR_X_MOYEN, FM1_CELL_W);
    zoom_cartes();
    agr_axe(t, FM1_MOYEN_H, AGR_Y_MOYEN, FM1_CELL_H);
    for (i = 0; i < FM1_MOYEN_H; i++) s_src_y_moyen[i] = (uint8_t)t[i];
    agr_axe(t, FM1_DRUM_H, AGR_Y_DRUM, FM1_CELL_H);
    for (i = 0; i < FM1_DRUM_H; i++) s_src_y_drum[i] = (uint8_t)t[i];
    {   /* la DRUM MACHINE : le nom (2 cases de 12), l'espace (6), les pas (13 : 3 2 2 2 2 2) */
        uint32_t x = 0, c, k;
        for (c = 0; c < 19u; c++)
            for (k = 0; k < FM1_CELL_W; k++) {
                const uint32_t lg = c < 2u ? 2u : c == 2u ? 1u : k == 0u ? 3u : 2u, j0 = x;
                for (; x < j0 + lg && x < 240u; x++)
                    s_src_x_drum[x] = (uint16_t)(c * FM1_CELL_W + k);
            }
        for (; x < 240u; x++)
            s_src_x_drum[x] = TITRE_VIDE;
    }
    agr_axe(s_src_x_id, 240, AGR_X_ENTETE, FM1_CELL_W);   /* (l'en-tete) */
    agr_axe(t, FM1_CELL_COMPACTE_H, AGR_Y_COMPACTE, FM1_CELL_H);
    for (i = 0; i < FM1_CELL_COMPACTE_H; i++) s_src_y_compacte[i] = (uint8_t)t[i];
    agr_axe(t, FM1_ENTETE_ECRAN_H, AGR_Y_ENTETE, FM1_CELL_H);
    for (i = 0; i < FM1_ENTETE_ECRAN_H; i++) s_src_y_entete[i] = (uint8_t)t[i];
    agr_axe(t, FM1_CELL_ECRAN_H, AGR_Y, FM1_CELL_H);
    for (i = 0; i < FM1_CELL_ECRAN_H; i++) s_src_y[i] = (uint8_t)t[i];
    agr_axe(t, FM1_CELL_TITRE_H, AGR_Y_TITRE, FM1_CELL_H);
    for (i = 0; i < FM1_CELL_TITRE_H; i++) s_src_y_titre[i] = (uint8_t)t[i];
}

// On n'envoie a l'afficheur que les lignes qui ont change depuis l'image
// precedente : une empreinte par ligne de l'image, des bandes de 4 lignes
// de l'afficheur.
static uint32_t s_empreinte[240];
static uint8_t s_ecran_neuf = 1;                  /* tout renvoyer (demarrage, retour d'UBOOT...) */
#define BANDE 4u
static uint16_t s_bande[2][240u * BANDE];
static uint8_t s_bande_k;

/* LE THEME AUBERGINE, aux couleurs du boitier du FM-1 (rose-mauve, touches violettes ; HOME bleu, PLAY vert,
   REC rouge) : l'image garde les couleurs du tracker (le fond noir, le curseur, que les analyses
   reconnaissent), l'afficheur les traduit ici. entete : la ligne du haut (le nom de la page), en bleu. */
#define THEME_FOND RGB15(1, 0, 2)                 /* #080010, une prune presque noire (plus claire, on croyait l'ecran en panne) */
static u16 theme(u16 c, int entete)
{
    switch (c & 0x7FFFu) {
    case RGB15(0, 0, 0):
    case RGB15(0, 1, 1):   return THEME_FOND;     /* (le fond, et la trame : une seule prune, sans rayures) */
    case RGB15(29, 27, 31): return entete ? RGB15(9, 19, 31) : RGB15(31, 30, 31);   /* les intitules */
    case RGB15(23, 20, 30): return entete ? RGB15(9, 19, 31) : RGB15(30, 25, 28);   /* les valeurs : rose pale */
    case RGB15(28, 9, 10):  return RGB15(31, 6, 9);    /* le curseur : le rouge de REC */
    case RGB15(17, 0, 31):  return RGB15(22, 10, 19);  /* les numeros de ligne : le mauve du boitier */
    case RGB15(14, 23, 26):
    case RGB15(16, 17, 24): return RGB15(25, 20, 31);  /* ... leur texte, lilas */
    case RGB15(11, 10, 16): return RGB15(13, 9, 14);   /* le vide */
    case RGB15(31, 29, 10): return RGB15(31, 27, 13);  /* la selection : jaune */
    case RGB15(8, 28, 10):  return RGB15(6, 28, 15);   /* une piste ou agit un effet : le vert de PLAY */
    default:                return (u16)(c & 0x7FFFu);
    }
}

/* LES FILS : les traits entre les colonnes ondulent, parfois et d'un point seulement, comme les fils d'un
   circuit bending. Chaque trait a ses encoches (environ une toutes les six rangees), decalees de celles du
   trait voisin : jamais deux a la meme hauteur. Fixes dans l'image : elles ne bougent pas quand on navigue. */
#define FIL_PERIODE 48u                           /* (en lignes de l'image) */
static int32_t fil_ecart(uint32_t t, uint32_t yimg, uint32_t y0, uint32_t graine)
{
    uint32_t r, j, p, jit, lg;
    if (yimg < y0)
        return 0;
    r = yimg - y0 + FIL_PERIODE - (t * 23u + graine) % FIL_PERIODE;   /* (23 : le voisin, une demi-periode plus loin) */
    j = r / FIL_PERIODE;
    p = r % FIL_PERIODE;
    jit = (t * 7u + j * 11u) % 9u;                /* (un peu d'irregularite, 0 .. 8 lignes) */
    lg = 5u + (t * 3u + j * 5u) % 4u;             /* (5 .. 8 lignes) */
    if (p < jit || p >= jit + lg)
        return 0;
    return ((t + j) & 1u) ? 1 : -1;
}

/* pour chaque ligne de l'afficheur : la ligne de l'image montree (-1 : le fond) et son genre */
static int16_t s_ly_src[240];
static uint8_t s_ly_titre[240];                   /* 0 rangee normale, 1 titres de colonnes, 2 en-tete, 3 PHRASE */

static uint32_t empreinte_ligne(uint32_t ly)     /* la ligne ly de l'afficheur : celle de sa ligne d'image */
{
    return (s_ly_src[ly] < 0 ? 0x9E3779B9u : s_h_ligne[s_ly_src[ly]]) ^ s_ly_titre[ly];
}

static inline uint16_t ds_vers_panneau(u16 c)    /* BGR555 de la DS -> RGB565, octets permutes */
{
    uint32_t r = c & 31u, g = (c >> 5) & 31u, b = (c >> 10) & 31u;
    uint32_t v = (r << 11) | (((g << 1) | (g >> 4)) << 5) | b;
    return (uint16_t)((v >> 8) | (v << 8));
}


static int cellule_vide(uint32_t r, uint32_t c)  /* (sans l'espace de droite : le pave du curseur y deborde) */
{
    static uint32_t cle[NB_RANGEES], pleines[NB_RANGEES][2];   /* (par rangee : ses cellules, gardees) */
    const uint8_t f0 = palette_indice(0), f1 = palette_indice(RGB15(0, 1, 1));
    uint32_t y, x, k, h;
    est_fond(0, f0, f1);
    h = cle_analyse(r * FM1_CELL_H, FM1_CELL_H) | 1u;
    if (cle[r] != h) {
        cle[r] = h;
        pleines[r][0] = pleines[r][1] = 0;
        for (k = 0; k < RG_ECRAN_L / FM1_CELL_W; k++)
            for (y = r * FM1_CELL_H; y < (r + 1u) * FM1_CELL_H; y++) {
                for (x = k * FM1_CELL_W; x < k * FM1_CELL_W + FM1_CELL_W - 1u; x++)
                    if (!est_fond(s_ecran[y * RG_ECRAN_L + x], f0, f1))
                        break;
                if (x < k * FM1_CELL_W + FM1_CELL_W - 1u) {
                    pleines[r][k >> 5] |= 1u << (k & 31u);
                    break;
                }
            }
    }
    return !((pleines[r][c >> 5] >> (c & 31u)) & 1u);
}

/* Les titres en police d'origine, chacun CENTRE sur sa colonne de valeurs : la suite de cellules pleines
   de la premiere rangee de valeurs sous les titres qui chevauche le mot (sinon, cale sur sa cellule).
   s_src_x_titre : point de l'afficheur -> point de l'image, relatif a s_vue_x. */
#define NB_COLS (RG_ECRAN_L / FM1_CELL_W)
static void carte_titres(void)
{
    int32_t cw = s_zoom ? FM1_ZOOM_W : s_compacte ? 11 : FM1_CELL_ECRAN_W;
    const int32_t c0 = s_vue_x / FM1_CELL_W, lw = TITRES_GROS ? 7 : FM1_CELL_W;   /* (lw : une lettre de titre) */
    uint8_t plein[NB_COLS];
    int32_t c, r, mot_c[NB_COLS], mot_n[NB_COLS], nb = 0, k;
    for (k = 0; k < 240; k++)
        s_src_x_titre[k] = TITRE_VIDE;
    if (s_rangee_titres < 0)
        return;
    for (c = 0; c < NB_COLS; c++) {               /* les mots */
        if (cellule_vide((uint32_t)s_rangee_titres, (uint32_t)c))
            continue;
        if (!nb || mot_c[nb - 1] + mot_n[nb - 1] != c || mot_debute(s_rangee_titres, c)) {
            mot_c[nb] = c;
            mot_n[nb++] = 0;
        }
        mot_n[nb - 1]++;
    }
    for (c = 0; c < NB_COLS; c++)
        plein[c] = 0;
    for (r = s_rangee_titres + 1; r < (int32_t)NB_RANGEES; r++) {   /* la premiere rangee de valeurs */
        int32_t vu = 0;
        for (c = 0; c < NB_COLS; c++)
            vu |= plein[c] = (uint8_t)!cellule_vide((uint32_t)r, (uint32_t)c);
        if (vu) {
            if (s_style[r] == STYLE_MOYEN)        /* les colonnes a la largeur de leurs valeurs */
                cw = FM1_MOYEN_W;
            break;
        }
    }
    for (k = 0; k < nb; k++) {
        int32_t a = mot_c[k], b = mot_c[k] + mot_n[k] - 1, x0, i, j, meilleur = 0;
        for (c = 0; c < NB_COLS; c++) {           /* la suite pleine qui chevauche le plus le mot */
            int32_t d = c, f, rec;
            if (!plein[c] || (c && plein[c - 1]))
                continue;
            for (f = c; f + 1 < NB_COLS && plein[f + 1]; f++)
                ;
            rec = (f < b ? f : b) - (d > a ? d : a) + 1;
            if (rec > meilleur) {
                meilleur = rec;
                x0 = ((d + f + 1) * cw - 2) / 2 - (mot_n[k] * lw - 1) / 2;   /* (2 : l'espace de la derniere cellule) */
            }
        }
        if (!meilleur)
            x0 = a * cw;
        x0 -= c0 * cw;
        for (i = 0; i < mot_n[k]; i++)
            for (j = 0; j < lw; j++) {
                const int32_t sx = x0 + i * lw + j,
                              src = (a + i - c0) * FM1_CELL_W + (TITRES_GROS ? (int32_t)s_src_x_id[j] : j);
                if (sx >= 0 && sx < 240 && src >= 0)
                    s_src_x_titre[sx] = (uint16_t)src;
            }
    }
}

/* La carte d'une rangee qui a des valeurs decalees d'une demi-cellule (fm1_decale_demi). */
static const uint16_t *carte_decalee(uint32_t rangee, const uint16_t *base)
{
    static uint16_t c[240];
    const int32_t demi = (int32_t)fm1_largeur_cellule() / 2;
    uint32_t k, trouve = 0;
    int32_t i;
    for (k = 0; k < s_nb_decales; k++)
        if (s_decales[k].rangee == rangee) {
            if (!trouve) {
                memcpy(c, base, sizeof c);
                trouve = 1;
            }
            {
                const int32_t a = (int32_t)s_decales[k].col * FM1_CELL_W - s_vue_x,
                              b = a + (int32_t)s_decales[k].n * FM1_CELL_W;
                for (i = 0; i < 240; i++)         /* leur place d'origine : le fond */
                    if ((int32_t)base[i] >= a && (int32_t)base[i] < b)
                        c[i] = TITRE_VIDE;
                for (i = 0; i < 240; i++)         /* puis une demi-cellule plus a droite */
                    if ((int32_t)base[i] >= a && (int32_t)base[i] < b && i + demi < 240)
                        c[i + demi] = base[i];
            }
        }
    return trouve ? c : base;
}

/* La carte de l'en-tete : celle de sa police, puis les mots places au point pres (fm1_mot_entete). */
static uint16_t s_src_x_entete[240];
static void carte_entete(void)
{
    const uint16_t *base = s_zoom ? s_src_x_id_z : s_src_x_id;
    const int32_t lw = fm1_largeur_cellule_entete();          /* une lettre de l'en-tete, a l'ecran */
    const int32_t dc = (int32_t)fm1_largeur_cellule();        /* une cellule des valeurs */
    const int32_t glyphe = s_zoom ? ZOOMS[s_zoom].l : s_compacte ? 9 : 12;   /* une lettre des valeurs, sans l'espace */
    const int32_t c0 = s_vue_x / FM1_CELL_W;
    uint32_t k;
    int32_t i, j;
    memcpy(s_src_x_entete, base, sizeof s_src_x_entete);
    for (k = 0; k < s_nb_mots_entete; k++) {
        const int32_t a = s_mots_entete[k].col_mot * FM1_CELL_W, b = a + s_mots_entete[k].n * FM1_CELL_W;
        const int32_t debut = (s_mots_entete[k].col_val - c0) * dc;
        const int32_t large = (s_mots_entete[k].n_cases - 1) * dc + glyphe;
        const int32_t x0 = debut + large / 2 - (s_mots_entete[k].n * lw - 1) / 2;
        for (i = 0; i < 240; i++)                             /* sa place d'origine : le fond */
            if ((int32_t)base[i] >= a && (int32_t)base[i] < b)
                s_src_x_entete[i] = TITRE_VIDE;
        for (i = 0; i < s_mots_entete[k].n; i++)
            for (j = 0; j < lw; j++) {
                const int32_t sx = x0 + i * lw + j;
                if (sx >= 0 && sx < 240)
                    s_src_x_entete[sx] = (uint16_t)(a + i * FM1_CELL_W + base[j]);
            }
    }
}

static void lignes_init(void)
{
    uint32_t ly = 0, i = (uint32_t)s_vue_y;
    for (; ly < ENTETE_H(); ly++) {               /* l'en-tete : la rangee 0, un peu agrandie */
        /* (sous les lettres : l'espace eventuel, l'interligne de la rangee 0) */
        s_ly_src[ly] = (int16_t)(ly >= ENTETE_LETTRES_H() ? FM1_CELL_H - 1u
                                 : s_zoom ? s_src_y_entete_z[ly] : s_src_y_entete[ly]);
        s_ly_titre[ly] = 2;
    }
    {                                             /* les titres fixes, juste dessous */
        const uint32_t ht = hauteur_titres();
        uint32_t k;
        for (k = 0; k < ht && ly < 240u; k++, ly++) {
            s_ly_src[ly] = (int16_t)((uint32_t)s_rangee_titres * FM1_CELL_H + (TITRES_GROS ? s_src_y_entete[k] : s_src_y_titre[k]));
            s_ly_titre[ly] = 1;
        }
    }
    while (ly < 240u) {
        if (i >= s_nb_rangees) {
            s_ly_src[ly] = -1;
            s_ly_titre[ly++] = 0;
            continue;
        }
        {
            const int titre = (int32_t)s_rangees[i] == s_rangee_titres;
            const uint8_t style = s_style[s_rangees[i]];
            const uint32_t h = hauteur_rangee(i), h_lettres = style == STYLE_AERE || style == STYLE_PETIT ? h - FM1_FILET_H
                                                            : style == STYLE_LOOP ? FM1_DRUM_H : h;
            const uint8_t *carte_y = titre ? (TITRES_GROS ? s_src_y_entete : s_src_y_titre)
                                   : style == STYLE_PETIT ? (s_zoom ? s_src_y_entete_z : s_src_y_entete)
                                   : style == STYLE_DRUM || style == STYLE_LOOP ? s_src_y_drum
                                   : style == STYLE_MOYEN ? s_src_y_moyen
                                   : s_zoom ? s_src_y_zoom
                                   : s_compacte ? s_src_y_compacte : s_src_y;
            uint32_t k;
            for (k = 0; k < h && ly < 240u; k++, ly++) {
                /* (un espace montre l'interligne de sa rangee : le fond) */
                s_ly_src[ly] = (int16_t)(s_rangees[i] * FM1_CELL_H +
                                         (style == STYLE_ESPACE || style == STYLE_LIGNE_VIDE || k >= h_lettres ? FM1_CELL_H - 1u : carte_y[k]));
                s_ly_titre[ly] = (uint8_t)(titre ? 1 : style == STYLE_MOYEN ? 4 : style == STYLE_PETIT ? 6 : style == STYLE_DRUM || style == STYLE_LOOP ? 7
                                           : s_zoom ? 5 : s_compacte ? 3 : 0);
            }
        }
        i++;
    }
}

static const uint8_t *ligne_image(uint32_t ly)   /* la ligne de l'image montree sur la ligne ly de l'afficheur */
{
    static uint8_t vide[240 + FM1_CELL_W];
    if (s_ly_src[ly] < 0)
        return vide;
    return &s_ecran[(uint32_t)s_ly_src[ly] * RG_ECRAN_L + (s_ly_titre[ly] == 2 || s_ly_titre[ly] == 6 ? 0u : (uint32_t)s_vue_x)];
}

/* L'ANNONCE : une ou deux lignes en gros au milieu de l'ecran (le mode KEYS / TRACKER, la valeur qu'une molette
   regle, le tempo), par-dessus la page, a la meme taille quel que soit le ZOOM, une seconde apres la derniere.
   Chaque ligne est aussi grosse qu'elle peut (lettres de 20 x 28 au plus) dans la largeur de l'ecran. */
static char s_annonce[2][24];
static uint32_t s_annonce_images, s_annonce_fin;   /* (montree tant que fm1_ms n'a pas atteint s_annonce_fin) */
static uint32_t s_ann_ech[2], s_ann_x0[2], s_ann_y0[2], s_ann_bx0, s_ann_bx1, s_ann_by0, s_ann_by1;
static int32_t s_ann_neuf_y0 = -1, s_ann_neuf_y1 = -1;   /* les lignes de l'afficheur a renvoyer cette fois */
#define ANNONCE_MARGE 10u
static void ann_a_renvoyer(uint32_t y0, uint32_t y1)
{
    if (s_ann_neuf_y0 < 0 || (int32_t)y0 < s_ann_neuf_y0) s_ann_neuf_y0 = (int32_t)y0;
    if ((int32_t)y1 > s_ann_neuf_y1) s_ann_neuf_y1 = (int32_t)y1;
}
void fm1_annonce2(const char *l1, const char *l2)
{
    uint32_t k, h = 0, w = 0, n[2];
    if (s_annonce_images)
        ann_a_renvoyer(s_ann_by0, s_ann_by1);    /* (l'ancien cadre, s'il etait plus grand) */
    str_cpy(s_annonce[0], l1 ? l1 : "", sizeof s_annonce[0]);
    str_cpy(s_annonce[1], l2 ? l2 : "", sizeof s_annonce[1]);
    for (k = 0; k < 2u; k++) {
        n[k] = (uint32_t)strlen(s_annonce[k]);
        s_ann_ech[k] = n[k] ? (240u - 2u * ANNONCE_MARGE - 4u) / ((MD_FONT_W + 1u) * n[k]) : 0u;
        if (s_ann_ech[k] > 4u) s_ann_ech[k] = 4u;
        if (n[k] && !s_ann_ech[k]) s_ann_ech[k] = 1u;
        if (n[k]) {
            const uint32_t lw = n[k] * (MD_FONT_W + 1u) * s_ann_ech[k] - s_ann_ech[k];
            if (lw > w) w = lw;
            s_ann_x0[k] = (240u - lw) / 2u;
            h += MD_FONT_HT * s_ann_ech[k] + (h ? 8u : 0u);
        }
    }
    s_ann_by0 = (240u - h) / 2u - ANNONCE_MARGE;
    s_ann_by1 = s_ann_by0 + h + 2u * ANNONCE_MARGE - 1u;
    s_ann_bx0 = (240u - w) / 2u - ANNONCE_MARGE;
    s_ann_bx1 = s_ann_bx0 + w + 2u * ANNONCE_MARGE - 1u;
    s_ann_y0[0] = s_ann_by0 + ANNONCE_MARGE;
    s_ann_y0[1] = s_ann_y0[0] + (n[0] ? MD_FONT_HT * s_ann_ech[0] + 8u : 0u);
    s_annonce_images = 1u;
    s_annonce_fin = fm1_ms + 1000u;               /* une seconde apres la derniere */
    ann_a_renvoyer(s_ann_by0, s_ann_by1);
}
static void fm1_annonce(const char *mot) { fm1_annonce2(mot, 0); }
/* la ligne ly de l'afficheur, par-dessus : le cadre et les lignes */
static void annonce_ligne(uint16_t *l, uint32_t ly, uint16_t fond, uint16_t encre, uint16_t bord)
{
    uint32_t x, k;
    if (ly < s_ann_by0 || ly > s_ann_by1)
        return;
    for (x = s_ann_bx0; x <= s_ann_bx1; x++)
        l[x] = (ly == s_ann_by0 || ly == s_ann_by1 || x == s_ann_bx0 || x == s_ann_bx1) ? bord : fond;
    for (k = 0; k < 2u; k++) {
        const uint32_t e = s_ann_ech[k], n = (uint32_t)strlen(s_annonce[k]), pas = (MD_FONT_W + 1u) * e;
        if (!n || ly < s_ann_y0[k] || ly >= s_ann_y0[k] + MD_FONT_HT * e)
            continue;
        for (x = 0; x < n * pas; x++) {
            const uint32_t c8 = (uint8_t)s_annonce[k][x / pas], cx = (x % pas) / e, cy = (ly - s_ann_y0[k]) / e;
            if (cx < MD_FONT_W && c8 >= 0x20u && c8 < 0x60u && (md_font[c8 - 0x20u][cy] & (0x10u >> cx)))
                l[s_ann_x0[k] + x] = encre;
        }
    }
}

/* Les GIF de la page CIRCUIT BENDING (tools/gifs.py) : un quart de l'ecran (80 x 60 agrandis de moitie),
   a droite, par-dessus la page ; il reste 4 s (l'extrait boucle, a sa vraie vitesse) en montant (ou en
   descendant), puis s'en va. fm1_gif(k) le lance. */
#include "fm1_gifs.h"
static int8_t s_gif = -1;
static uint32_t s_gif_t0;
static int32_t s_gif_y = -1, s_gif_img = -1, s_gif_h = 0;
void fm1_gif(int k)
{
    /* (pas trop souvent : une chance sur deux, et jamais deux en moins de 20 s) */
    /* (k = -2 : n'importe laquelle, tiree au hasard ; jamais deux fois de suite la meme, partout) */
    static uint32_t dernier, hasard = 0x9E3779B9u;
    static int derniere = -1;
    const int nb = (int)(sizeof FM1_GIFS / sizeof FM1_GIFS[0]);
    hasard = hasard * 1103515245u + 12345u + fm1_ms;
    if (k == -2 || (k >= 0 && k == derniere)) {           /* (au hasard, ou deja vue juste avant : une autre) */
        k = (int)((hasard >> 8) % (uint32_t)(nb - 1));
        if (k >= derniere && derniere >= 0) k++;           /* (toutes sauf la derniere) */
    }
    if (k < 0 || k >= nb)
        return;
    if ((dernier && fm1_ms - dernier < 20000u) || (hasard >> 16) % 2u)
        return;
    dernier = fm1_ms ? fm1_ms : 1u;
    derniere = k;
    if (s_gif >= 0 && s_gif_y >= 0)
        ann_a_renvoyer((uint32_t)s_gif_y, (uint32_t)(s_gif_y + s_gif_h));
    s_gif = (int8_t)k;
    s_gif_t0 = fm1_ms;
    s_gif_img = s_gif_y = -1;
}
static void gif_etat(void)                        /* avant l'image : ou il en est, et les lignes a renvoyer */
{
    const fm1_gif_t *g;
    uint32_t t, T;
    int32_t img, y;
    if (s_gif < 0)
        return;
    g = &FM1_GIFS[s_gif];
    t = fm1_ms - s_gif_t0;
    /* (a l'ecran 4 s, l'extrait en boucle ; une histoire de 2 s ou plus — le nunchaku — une seule fois, puis
       sa derniere image un instant) */
    T = g->ms >= 2000u ? g->ms + 700u : 4000u;
    s_gif_h = (int32_t)g->h * 3 / 2;
    if (t >= T) {                                 /* fini : ses lignes, sans lui */
        if (s_gif_y >= 0)
            ann_a_renvoyer((uint32_t)s_gif_y, (uint32_t)(s_gif_y + s_gif_h));
        s_gif = -1;
        s_gif_y = -1;
        return;
    }
    img = g->ms >= 2000u ? (t >= g->ms ? (int32_t)g->n - 1 : (int32_t)(t * g->n / g->ms))
                         : (int32_t)((t % (g->ms ? g->ms : 1u)) * g->n / (g->ms ? g->ms : 1u));
    y = s_gif == 1 ? 60 + (int32_t)(50u * t / T)            /* le saut descend */
                   : 150 - (int32_t)(50u * t / T);          /* les autres montent */
    if (y + s_gif_h > 239)
        y = 239 - s_gif_h;
    if (img != s_gif_img || y != s_gif_y) {
        if (s_gif_y >= 0)
            ann_a_renvoyer((uint32_t)s_gif_y, (uint32_t)(s_gif_y + s_gif_h));
        ann_a_renvoyer((uint32_t)y, (uint32_t)(y + s_gif_h));
        s_gif_img = img;
        s_gif_y = y;
    }
}
static void gif_ligne(uint16_t *l, uint32_t ly)  /* la ligne ly de l'afficheur, le GIF dessus */
{
    const fm1_gif_t *g;
    uint32_t x, w2, x0, sy;
    const uint8_t *rang;
    if (s_gif < 0 || s_gif_y < 0 || (int32_t)ly < s_gif_y || (int32_t)ly >= s_gif_y + s_gif_h)
        return;
    g = &FM1_GIFS[s_gif];
    w2 = (uint32_t)g->l * 3u / 2u;
    x0 = 240u - w2 - 4u;
    sy = (ly - (uint32_t)s_gif_y) * 2u / 3u;
    if (sy >= g->h)
        return;
    {   /* la ligne, ses series decodees (longueur << 4 | couleur), puis agrandie de moitie */
        uint8_t pts[96];
        uint32_t n = 0;
        rang = g->pts + g->lignes[(uint32_t)s_gif_img * g->h + sy];
        while (n < g->l && n < sizeof pts) {
            const uint32_t v = *rang++, k = (v >> 4) + 1u;
            uint32_t j;
            for (j = 0; j < k && n < sizeof pts; j++)
                pts[n++] = (uint8_t)(v & 15u);
        }
        for (x = 0; x < w2; x++) {
            const uint32_t c = g->pal[pts[x * 2u / 3u]];
            l[x0 + x] = (uint16_t)((c >> 8) | (c << 8));   /* (l'afficheur : l'octet fort d'abord) */
        }
    }
}

/* L'ECRAN D-PAD (tools/dpad.py) : en passant en mode D-PAD (SEQ), le clavier du FM-1, ce que fait chaque touche
   et tous les raccourcis, sur tout l'ecran. Il reste tant qu'on ne touche a rien ; le premier bouton, la
   premiere touche ou molette le font partir, et ne font rien d'autre (scanKeys). */
#include "fm1_dpad.h"
static uint8_t s_dpad_ecran, s_aide_clavier;
/* le clavier, en haut des pages HELP du D-PAD (la page lui garde la place : des rangees vides) */
void fm1_aide_clavier(int on)
{
    if ((uint8_t)(on != 0) != s_aide_clavier) {
        s_aide_clavier = (uint8_t)(on != 0);
        s_ecran_neuf = 1;                         /* (ses lignes sont du fond pour la page : a renvoyer) */
    }
}
static void clavier_ligne(uint16_t *l, uint32_t ly)
{
    const uint32_t y0 = ENTETE_H() + 2u;
    const uint8_t *p;
    uint32_t n = 0;
    if (!s_aide_clavier || ly < y0 || ly >= y0 + FM1_CLAV_H)
        return;
    p = FM1_CLAV + FM1_CLAV_LIGNES[ly - y0];
    while (n < 240u) {
        const uint32_t v = *p++, k = (v >> 4) + 1u, c = FM1_DPAD_PAL[v & 15u];
        uint32_t j;
        for (j = 0; j < k && n < 240u; j++)
            l[n++] = (uint16_t)((c >> 8) | (c << 8));
    }
}
static void dpad_ligne(uint16_t *l, uint32_t ly)   /* (hors de son cadre, transparent : la page se voit) */
{
    const uint8_t *p = FM1_DPAD + FM1_DPAD_LIGNES[ly];
    uint32_t n = 0;
    while (n < 240u) {
        const uint32_t v = *p++, k = (v >> 4) + 1u, c = FM1_DPAD_PAL[v & 15u];
        uint32_t j;
        if ((v & 15u) == FM1_DPAD_TRANSP) {
            n += k;
            continue;
        }
        for (j = 0; j < k && n < 240u; j++)
            l[n++] = (uint16_t)((c >> 8) | (c << 8));   /* (l'afficheur : l'octet fort d'abord) */
    }
}

/* Les TRAITS entre les colonnes (SONG, CHAIN, PHRASE, TABLE) : chaque espace noir est donne par ses bords
   dans l'image (le dernier point de la colonne d'avant + 1, le premier de la suivante - 1) ; le trait se
   trace a l'afficheur, au milieu EXACT de l'espace en points d'ecran (l'agrandissement n'est pas regulier :
   le milieu dans l'image ne l'est pas a l'ecran), sur les rangees y0..y1 de l'image, et seulement sur le
   fond : un chevron, un curseur passent par-dessus. */
#define NB_TRAITS 8
static int16_t s_tr_g[NB_TRAITS], s_tr_d[NB_TRAITS];
static uint16_t s_tr_coul[NB_TRAITS];
static int32_t s_tr_y0, s_tr_y1;
static uint8_t s_nb_traits;
void fm1_traits(int n, const int16_t *g, const int16_t *d, const u16 *coul, int y0, int y1)
{
    int k, change = 0;
    if (n < 0) n = 0;
    if (n > NB_TRAITS) n = NB_TRAITS;
    if (n != s_nb_traits || y0 != s_tr_y0 || y1 != s_tr_y1)
        change = 1;
    for (k = 0; k < n; k++)
        if (s_tr_g[k] != g[k] || s_tr_d[k] != d[k] || s_tr_coul[k] != (coul[k] & 0x7FFFu)) {
            change = 1;
            s_tr_g[k] = g[k];
            s_tr_d[k] = d[k];
            s_tr_coul[k] = (uint16_t)(coul[k] & 0x7FFFu);
        }
    s_nb_traits = (uint8_t)n;
    s_tr_y0 = y0;
    s_tr_y1 = y1;
    if (change)
        s_ecran_neuf = 1;                         /* (tout l'ecran : les traits ont bouge) */
}
/* le point d'ecran du milieu de chaque trait, pour une carte (point d'ecran -> point de l'image) */
static void traits_x(const uint16_t *carte, int32_t vue, int16_t *x)
{
    uint32_t k, i;
    for (k = 0; k < s_nb_traits; k++) {
        int32_t imin = -1, imax = -1;
        for (i = 0; i < 240u; i++) {
            const int32_t sx = (int32_t)carte[i] + vue;
            if (carte[i] == TITRE_VIDE || sx < s_tr_g[k] || sx > s_tr_d[k])
                continue;
            if (imin < 0) imin = (int32_t)i;
            imax = (int32_t)i;
        }
        x[k] = (int16_t)(imin < 0 ? -1 : (imin + imax) / 2);
    }
}

static void montre(void)
{
    static uint16_t pan[256], pan_entete[256];
    uint32_t y = 0, i;
    const uint16_t *tr_carte = 0;                 /* (les traits : la carte de leurs points, et ces points) */
    int16_t tr_x[NB_TRAITS];
    if (!s_src_x[239])
        agr_init();
    empreintes_image();
    gif_etat();
    entete_nom_page();
    vue_suit_curseur();
    lignes_init();
    carte_titres();
    carte_entete();
    if (s_vue_x != s_vue_x_vu || s_vue_y != s_vue_y_vu) {   /* la fenetre a bouge : tout */
        s_vue_x_vu = s_vue_x;
        s_vue_y_vu = s_vue_y;
        s_ecran_neuf = 1;
    }
    for (i = 0; i < s_pal_n; i++)                 /* (la trame du tracker, une ligne sur deux en bleu-noir : */
        pan[i] = ds_vers_panneau(theme(s_palette[i], 0)), pan_entete[i] = ds_vers_panneau(theme(s_palette[i], 1));   /* sans rayures) */
    while (y < 240u) {
        uint32_t y0, n, k;
        uint16_t *d;
        const int ann = (int32_t)y >= s_ann_neuf_y0 && (int32_t)y <= s_ann_neuf_y1;   /* (l'annonce a change la) */
        if (!s_ecran_neuf && !ann && empreinte_ligne(y) == s_empreinte[y]) {
            y++;
            continue;
        }
        y0 = y;
        n = 0;
        while (y < 240u && n < BANDE) {           /* la bande : les lignes changees qui se suivent */
            const uint32_t h = empreinte_ligne(y);
            if (!s_ecran_neuf && h == s_empreinte[y] && !((int32_t)y >= s_ann_neuf_y0 && (int32_t)y <= s_ann_neuf_y1))
                break;
            s_empreinte[y] = h;
            y++;
            n++;
        }
        d = s_bande[s_bande_k];
        s_bande_k ^= 1u;                          /* l'autre bande part peut-etre encore par DMA */
        for (k = 0; k < n; k++) {
            const uint8_t *src = ligne_image(y0 + k);
            const uint8_t genre = s_ly_titre[y0 + k];
            const uint16_t *carte = genre == 2 ? s_src_x_entete : genre == 6 ? (s_zoom ? s_src_x_id_z : s_src_x_id)
                                  : genre == 7 ? s_src_x_drum
                                  : genre == 1 ? s_src_x_titre : genre == 3 ? s_src_x_compacte
                                  : genre == 4 ? s_src_x_moyen : genre == 5 ? s_src_x_zoom : s_src_x;
            if (s_nb_decales && s_ly_src[y0 + k] >= 0 && (genre == 0 || genre == 3 || genre == 5))
                carte = carte_decalee((uint32_t)s_ly_src[y0 + k] / FM1_CELL_H, carte);
            const uint16_t fond = pan[palette_indice((s_ly_src[y0 + k] & 1) ? RGB15(0, 1, 1) : 0)];
            uint16_t *l = d + k * 240u;
            const uint16_t *pl = genre == 2 ? pan_entete : pan;
            for (i = 0; i < 240u; i++) {
                const uint32_t sx = carte[i];
                if (sx == TITRE_VIDE) {
                    l[i] = fond;
                    continue;
                }
                l[i] = (genre == 2 || genre == 6 ? 0u : (uint32_t)s_vue_x) + sx < RG_ECRAN_L ? pl[src[sx]] : pan[0];
            }
            if (s_nb_traits && s_ly_src[y0 + k] >= s_tr_y0 && s_ly_src[y0 + k] <= s_tr_y1 && genre != 2) {
                uint32_t t;
                if (carte != tr_carte) {          /* (une autre carte : les milieux, recalcules) */
                    traits_x(carte, s_vue_x, tr_x);
                    tr_carte = carte;
                }
                for (t = 0; t < s_nb_traits; t++) {
                    const int32_t xs = tr_x[t] < 0 ? -1 : tr_x[t] + fil_ecart(t, (uint32_t)s_ly_src[y0 + k], (uint32_t)s_tr_y0,
                                                                               (uint32_t)s_tr_g[0] * 7u);
                    if (xs < 0 || xs >= 240 || carte[xs] == TITRE_VIDE || (uint32_t)s_vue_x + carte[xs] >= RG_ECRAN_L)
                        continue;
                    if (est_fond(src[carte[xs]], palette_indice(0), palette_indice(RGB15(0, 1, 1))))
                        l[xs] = ds_vers_panneau(s_tr_coul[t]);
                }
            }
            gif_ligne(l, y0 + k);
            clavier_ligne(l, y0 + k);
            if (s_dpad_ecran)
                dpad_ligne(l, y0 + k);
            if (s_annonce_images)                 /* (par-dessus tout, l'ecran D-PAD compris : son nom au milieu) */
                annonce_ligne(l, y0 + k, ds_vers_panneau(theme(0, 0)), ds_vers_panneau(theme(RGB15(29, 27, 31), 0)),
                              ds_vers_panneau(theme(RGB15(28, 9, 10), 0)));
        }
        lcd_blit(0, y0, 240, n, d);
    }
    s_ecran_neuf = 0;
    s_ann_neuf_y0 = s_ann_neuf_y1 = -1;
    if (s_annonce_images && (int32_t)(fm1_ms - s_annonce_fin) >= 0) {
        s_annonce_images = 0;
        ann_a_renvoyer(s_ann_by0, s_ann_by1);     /* l'annonce finie : ses lignes, sans elle */
    }
}

/* « LOADING... » au milieu de l'ecran, envoye tout de suite : pendant l'import d'un morceau, la boucle du
   tracker ne tourne plus et l'ecran garderait sinon la page d'avant, aux numeros de ligne vides.
   fm1_ecran_attente : un autre mot (« EXPORTING... »), meme chose. */
void fm1_ecran_attente(const char *mot);
void fm1_ecran_chargement(void) { fm1_ecran_attente("LOADING..."); }

void fm1_ecran_attente(const char *mot)
{
    const uint8_t fond = palette_indice(0), encre = palette_indice(RGB15(29, 27, 31));
    const uint32_t r = 6, c0 = 1;                 /* rangee 6, a partir de la colonne 1 */
    uint32_t k, y, x;
    memset(s_ecran, fond, sizeof s_ecran);
    fm1_traits(0, 0, 0, 0, 0, 0);                 /* (pas de fils sur ces ecrans-la) */
    fm1_nom_page("");
    fm1_vu_page();
    for (k = 0; mot[k]; k++)
        for (y = 0; y < MD_FONT_HT; y++)
            for (x = 0; x < MD_FONT_W; x++)
                if (md_font[mot[k] - 0x20u][y] & (0x10u >> x))
                    s_ecran[(r * FM1_CELL_H + y) * RG_ECRAN_L + (c0 + k) * FM1_CELL_W + x] = encre;
    s_ecran_neuf = 1;
    montre();
    lcd_sync();
}


// ── Les touches ─────────────────────────────────────────────────────────────
static u32 s_tenues, s_avant, s_repete, s_masque;
static int s_delai = 30, s_cadence = 15, s_compte;
static int32_t s_crans[FM1_NB_MOLETTES];         /* crans des molettes pas encore joues */
static u32 s_accord;                              /* l'accord qu'une molette tient cette image */
static const uint8_t MOLETTE_MATRICE[FM1_NB_MOLETTES] = {0, 1, 6, 2, 3, 4, 5};   /* (panel.c de Felucca) */

static int controle_tenu(unsigned id, u32 boutons, u32 notes)
{
    if (id < 14u)
        return (boutons >> id) & 1u;
    if (id < 41u)
        return (notes >> (id - 14u)) & 1u;
    return 0;
}

// La molette du tempo : comme A + gauche / droite sur TEMPO dans PROJECT
// (main.cpp) — le moteur regle le BPM par crans : on avance jusqu'au prochain
// tempo reellement atteignable.
double md_replayer_get_bpm(void);
void md_replayer_set_bpm(double bpm);
static void tempo_pas(int d)
{
    const double avant = md_replayer_get_bpm();
    int essai;
    char b[12];
    for (essai = 1; essai <= 40; essai++) {
        int vise = (int)(avant + 0.5) + d * essai;
        double apres, ecart;
        if (vise < 32) vise = 32;
        if (vise > 255) vise = 255;
        md_replayer_set_bpm((double)vise);
        apres = md_replayer_get_bpm();
        ecart = apres > avant ? apres - avant : avant - apres;
        if (ecart > 0.4 || vise <= 32 || vise >= 255)
            break;
    }
    {   /* « TEMPO » / « 125 BPM », en gros au milieu de l'ecran */
        int v = (int)(md_replayer_get_bpm() + 0.5), k = 0;
        char t[4];
        do t[k++] = (char)('0' + v % 10); while ((v /= 10) && k < 3);
        for (essai = 0; essai < k; essai++) b[essai] = t[k - 1 - essai];
        str_cpy(b + k, " BPM", sizeof b - (size_t)k);
        fm1_annonce2("TEMPO", b);
    }
}

/* GLO : le TAP TEMPO. Chaque appui est date ; des le deuxieme, le tempo devient celui des intervalles
   (la moyenne des quatre derniers), affiche en gros. Plus de 2 s sans appui : on repart de zero. */
static uint32_t s_echantillons;                   /* (l'horloge du son, plus bas : fm1_sequenceur) */
static void __attribute__((unused)) tap_tempo(void)
{
    static uint32_t t[5];
    static uint32_t n;
    const uint32_t m = (uint32_t)((uint64_t)s_echantillons * 1000u / FS);   /* en ms, sur l'horloge du son */
    uint32_t k, somme;
    if (n && m - t[(n - 1u) % 5u] > 2000u)
        n = 0;
    t[n % 5u] = m;
    n++;
    if (n < 2u)
        return;
    k = n - 1u < 4u ? n - 1u : 4u;                /* les intervalles pris */
    somme = t[(n - 1u) % 5u] - t[(n - 1u - k) % 5u];
    if (!somme)
        return;
    {
        int bpm = (int)((60000u * k + somme / 2u) / somme), v, d;
        char b[12], c[4];
        if (bpm < 32) bpm = 32;
        if (bpm > 255) bpm = 255;
        md_replayer_set_bpm((double)bpm);
        v = (int)(md_replayer_get_bpm() + 0.5);
        d = 0;
        do c[d++] = (char)('0' + v % 10); while ((v /= 10) && d < 3);
        for (k = 0; k < (uint32_t)d; k++) b[k] = c[d - 1 - (int)k];
        str_cpy(b + d, " BPM", sizeof b - (size_t)d);
        fm1_annonce2("TEMPO", b);
    }
}
/* LES LED, a trois niveaux : 0 eteinte (un bouton qui ne fait rien), 1 faible (il sert : le niveau moyen du
   FM-1, fm1_led_mid et la lueur), 2 forte (sa page est ouverte, ou il est actif). id : un bouton (0..13) ou
   une touche du clavier (14..40), comme fm1_led_key. Le tracker regle HOME, SEL, FX, PLAY, REC et GLO ;
   ici, EDIT, SEQ, OCT, le clavier, et les boutons qui ne font rien. */
/* La LED VERTE de PLAY : a part, hors de la carte des touches (colonne 8, ligne PA5 : bit 1), comme Felucca
   (ui_input.c LED_PLAY_GREEN). La LED de la touche PLAY elle-meme est rouge. */
void fm1_led_play_vert(int niv)
{
    const uint8_t m = 1u << 1;
    fm1_led[8] = (uint8_t)(niv == 2 ? fm1_led[8] | m : fm1_led[8] & ~m);
    fm1_led_mid[8] = (uint8_t)(niv == 1 ? fm1_led_mid[8] | m : fm1_led_mid[8] & ~m);
    fm1_led_dim[8] = (uint8_t)(niv == 1 ? fm1_led_dim[8] | m : fm1_led_dim[8] & ~m);
}
void fm1_led_niveau(int id, int niv)
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if ((uint8_t)FM1_KEYMAP[r][p] == (uint8_t)id) {   /* (sans signe : pas de sextra, l'emulateur en ligne) */
                const uint8_t m = (uint8_t)(1u << r);
                fm1_led[p] = (uint8_t)(niv == 2 ? fm1_led[p] | m : fm1_led[p] & ~m);
                fm1_led_mid[p] = (uint8_t)(niv == 1 ? fm1_led_mid[p] | m : fm1_led_mid[p] & ~m);
                fm1_led_dim[p] = (uint8_t)(niv == 1 ? fm1_led_dim[p] | m : fm1_led_dim[p] & ~m);
            }
}

/* OCT- / OCT+ : les appuis pas encore lus par le tracker (fm1_octave_pas) */
static int32_t s_octave;
/* REC : ses appuis pas encore lus (fm1_rec_appuis) ; sa LED, allumee pendant l'enregistrement */
static int32_t s_rec;
int fm1_rec_appuis(void) { const int32_t n = s_rec; s_rec = 0; return (int)n; }
int fm1_rec_tenu(void) { return (int)((fm1_in.buttons >> FM1_B_REC) & 1u); }   /* (LOOPER : REC tenu efface) */
/* SEL : la page PRESET (ses appuis pas encore lus) ; sa LED toujours allumee, pour dire qu'elle existe */
static int32_t s_sel;
int fm1_sel_appuis(void) { const int32_t n = s_sel; s_sel = 0; return (int)n; }
/* FX : la page CIRCUIT BENDING (ses appuis pas encore lus) ; sa LED, que le tracker allume */
static int32_t s_fx;
int fm1_fx_appuis(void) { const int32_t n = s_fx; s_fx = 0; return (int)n; }
/* ENV : les pages d'aide (HELP) : il les ouvre, puis passe a la suivante */
static int32_t s_env;
int fm1_env_appuis(void) { const int32_t n = s_env; s_env = 0; return (int)n; }
/* SAVE : la fenetre de sauvegarde (ses appuis pas encore lus) */
static int32_t s_save;
int fm1_save_appuis(void) { const int32_t n = s_save; s_save = 0; return (int)n; }
/* Une fenetre de texte (le nom, le navigateur) : le mode KEYS s'efface le temps qu'elle est ouverte (la croix,
   A et B y redeviennent des touches). */
static uint8_t s_clavier_suspendu;
void fm1_clavier_suspendu(int on) { s_clavier_suspendu = (uint8_t)(on != 0); }
/* HOME : ramene a SONG (ses appuis pas encore lus) */
static int32_t s_home;
int fm1_home_appuis(void) { const int32_t n = s_home; s_home = 0; return (int)n; }
/* Sur la page PRESET, PRESETS fait defiler les presets (fm1_fx_pas 4), en mode KEYS aussi. */
static uint8_t s_page_presets, s_choix_preset;
void fm1_page_presets(int on) { s_page_presets = (uint8_t)(on != 0); }
void fm1_select_preset(int on) { s_choix_preset = (uint8_t)(on != 0); }   /* la page PRESET : SELECT choisit le preset */
/* Sur la page CIRCUIT BENDING en mode KEYS : SELECT, PRESETS et ALGORITHM mettent les effets 5 a 7
   (fm1_fx_pas 6, 4, 5) au lieu du tempo et du curseur. */
static uint8_t s_page_bend;
void fm1_page_bend(int on) { s_page_bend = (uint8_t)(on != 0); }
int fm1_drum(void) { return s_drum; }
static uint8_t s_looper;                          /* SEQ, troisieme mode : le LOOPER */
int fm1_looper(void) { return s_looper; }
void fm1_mode_tracker(void) { s_drum = 0; s_looper = 0; }   /* (un morceau charge : SEQ revient en TRACKER) */
static uint32_t s_led_clignote;                   /* des touches du clavier qui clignotent (LOOPER, STP/CUR : les rangees) */
void fm1_led_clignote(uint32_t touches) { s_led_clignote = touches; }
int fm1_drumkeys(void) { return s_drumkeys; }
void fm1_led_touches(const uint8_t *niv)          /* DRUM MACHINE : la lumiere des 27 touches (0 : la normale) */
{
    if (niv) memcpy(s_led_drum, niv, sizeof s_led_drum);
    s_led_drum_la = (uint8_t)(niv != 0);
}
int fm1_octave_pas(void) { const int32_t d = s_octave; s_octave = 0; return (int)d; }

/* Le PIANO : les touches du clavier qui ne font pas une touche DS (fm1_touches.h).
   Leurs appuis pas encore lus, et celles tenues. EDIT allume / eteint la saisie. */
static u32 s_piano_appuis, s_piano_tenues;
static u32 touches_boutons(void)                  /* les touches du clavier qui font une touche DS */
{
    u32 m = 0, i;
    for (i = 0; i < sizeof FM1_TOUCHES / sizeof FM1_TOUCHES[0]; i++)
        if (FM1_TOUCHES[i].fm1 != FM1_AUCUNE && FM1_TOUCHES[i].fm1 >= FM1_TOUCHE(0))
            m |= 1u << (FM1_TOUCHES[i].fm1 - FM1_TOUCHE(0));
    return m;
}
static u32 piano_masque(void) { return s_clavier && !s_clavier_suspendu ? 0u : touches_boutons(); }   /* (mode KEYS : toutes des notes) */
/* SONG, CHAIN, PHRASE, INSTR : les molettes reglent l'instrument (celui sous le curseur), comme les touches F
   du tracker DOS : KNOB 1..4 (fm1_fx_pas 0..3), PRESETS (4 : en mode tracker seulement, il deplace le curseur
   en mode KEYS) et ALGORITHM (5). Ailleurs, elles ne font rien. */
#define NB_REGLAGES 7                             /* (6 : SELECT, sur la page CIRCUIT BENDING en mode KEYS) */
static int32_t s_fx_pas[NB_REGLAGES];
static uint8_t s_molettes_instr;
void fm1_molettes_instr(int on)
{
    uint32_t k;
    if (s_molettes_instr != (uint8_t)(on != 0))
        for (k = 0; k < NB_REGLAGES; k++) s_fx_pas[k] = 0;
    s_molettes_instr = (uint8_t)(on != 0);
}
int fm1_fx_pas(int k)
{
    int32_t d;
    if (k < 0 || k >= NB_REGLAGES)
        return 0;
    d = s_fx_pas[k];
    s_fx_pas[k] = 0;
    return (int)d;
}
u32 fm1_piano_appuis(void) { const u32 a = s_piano_appuis; s_piano_appuis = 0; return a; }
u32 fm1_piano_tenues(void) { return s_piano_tenues; }
/* Un message du systeme (UBOOT annule...) a pris l'image : la page est a redessiner. */
static uint8_t s_page_perdue;
int fm1_page_perdue(void) { const int p = s_page_perdue; s_page_perdue = 0; return p; }
int fm1_clavier(void) { return s_clavier; }


void scanKeys(void)
{
    u32 enfonces = fm1_input_edges(0), boutons = fm1_in.buttons | enfonces;   /* un appui bref compte aussi */
    u32 notes_appuis = fm1_input_note_edges();
    u32 notes = fm1_in.notes | notes_appuis;
    uint32_t i;
    /* l'ecran D-PAD : le premier appui le fait partir, et rien d'autre (ses touches restent masquees jusqu'au
       relachement, plus bas) */
    const int dpad_part = s_dpad_ecran && (enfonces || notes_appuis);
    if (dpad_part) {
        s_dpad_ecran = 0;
        s_ecran_neuf = 1;
        enfonces &= (1u << FM1_B_ENV) | (1u << FM1_B_SEQ) | (1u << FM1_B_GLO);   /* (sauf ENV, l'aide que l'ecran annonce,
                                                                                   SEQ et GLO, les modes) */
        notes_appuis = 0;
    }
    if (enfonces & (1u << FM1_B_OCTUP)) s_octave++;
    if (enfonces & (1u << FM1_B_OCTDN)) s_octave--;
    if (enfonces & (1u << FM1_B_REC)) s_rec++;
    if (enfonces & (1u << FM1_B_SCL)) s_sel++;
    if (enfonces & (1u << FM1_B_HOME)) s_home++;
    if (enfonces & (1u << FM1_B_FX)) s_fx++;
    if (enfonces & (1u << FM1_B_SAVE)) s_save++;
    if (enfonces & (1u << FM1_B_ENV)) s_env++;
    fm1_led_niveau(FM1_B_EDIT, 0);                /* (EDIT ne fait rien) */
    fm1_led_niveau(FM1_B_OCTDN, 1);
    fm1_led_niveau(FM1_B_OCTUP, 1);
    fm1_led_niveau(FM1_B_LFO, 0);
    fm1_led_niveau(FM1_B_ARP, 0);
    {   /* SEQ : TRACKER ou DRUM MACHINE (en DRUM MACHINE, il clignote vite).
           GLO : KEYBOARD ou D-PAD, sur toutes les pages, la DRUM MACHINE comprise (en KEYBOARD, ses touches sont ses
           sons et ses pas ; en D-PAD, la croix y deplace le curseur). En D-PAD, GLO est allume, en KEYBOARD faible ;
           son ecran (le clavier, ce que font les touches) s'affiche en y passant. On demarre en KEYBOARD, TRACKER. */
        static uint32_t depuis;
        if (enfonces & (1u << FM1_B_SEQ)) {       /* TRACKER -> DRUM MACHINE -> LOOPER -> TRACKER */
            if (s_drum) { s_drum = 0; s_looper = 1; }
            else if (s_looper) s_looper = 0;
            else s_drum = 1;
            depuis = fm1_ms;
            fm1_annonce(s_drum ? "DRUM MACHINE" : s_looper ? "LOOPER" : "TRACKER");
        }
        if (enfonces & (1u << FM1_B_GLO)) {       /* KEYBOARD -> D-PAD -> DRUM KEYBOARD -> KEYBOARD */
            if (s_drumkeys) s_drumkeys = 0;       /* (DRUM KEYBOARD : les touches sont des notes, comme KEYBOARD) */
            else if (s_clavier) s_clavier = 0;
            else { s_clavier = 1; s_drumkeys = 1; }
            if (!s_clavier) {                     /* D-PAD : son ecran */
                s_dpad_ecran = 1;
                s_ecran_neuf = 1;
            }
            fm1_annonce(s_drumkeys ? "DRUM KEYBOARD" : s_clavier ? "KEYBOARD" : "D-PAD");
        }
        (void)depuis;                             /* (les LED de SEQ et de GLO : main.cpp, selon la page) */
    }
    {   /* le clavier, faible : tout en mode KEYS ; sinon, seulement les touches qui sont des boutons */
        const u32 boutons_touches = touches_boutons();
        for (i = 0; i < 27u; i++)
            fm1_led_niveau((int)FM1_TOUCHE(i), ((s_led_clignote >> i) & 1u) ? ((fm1_ms / 250u) & 1u ? 2 : 0)
                                              : s_drum && s_clavier && s_led_drum_la ? s_led_drum[i]
                                              : (s_clavier || ((boutons_touches >> i) & 1u)) ? 1 : 0);
    }
    s_piano_appuis |= notes_appuis & ~piano_masque();
    s_piano_tenues = fm1_in.notes & ~piano_masque();
    s_avant = s_tenues;
    s_tenues = 0;
    for (i = 0; i < sizeof FM1_TOUCHES / sizeof FM1_TOUCHES[0]; i++)
        if (FM1_TOUCHES[i].fm1 != FM1_AUCUNE && !(s_clavier && !s_clavier_suspendu && FM1_TOUCHES[i].fm1 >= FM1_TOUCHE(0)) &&
            controle_tenu(FM1_TOUCHES[i].fm1, boutons, notes))
            s_tenues |= FM1_TOUCHES[i].ds;
    if (dpad_part)
        s_masque |= s_tenues;                     /* (ce qui a ferme l'ecran D-PAD n'agit pas) */
    // Les molettes : chaque cran devient UNE repetition de sa touche DS (ce que
    // la croix tenue donne : keysDownRepeat), un cran par image. La file est
    // plafonnee : une molette tournee vite ne continue pas apres qu'on l'a lachee.
    for (i = 0; i < FM1_NB_MOLETTES; i++) {
        const int32_t pas = fm1_enc_take(MOLETTE_MATRICE[i]);
        if (pas && s_dpad_ecran) {                /* (une molette aussi fait partir l'ecran D-PAD ; elle agit) */
            s_dpad_ecran = 0;
            s_ecran_neuf = 1;
        }
        if (s_choix_preset && i == FM1_M_SELECT) {   /* (PRESET : SELECT change le preset ; pas le tempo) */
            s_fx_pas[4] += pas;
            continue;
        }
        if (s_page_bend && i == FM1_M_SELECT) {   /* (CIRCUIT BENDING : l'effet 5, GLIDE ; pas le tempo) */
            s_fx_pas[6] += pas;
            continue;
        }
        if (s_clavier && !s_drum && i == FM1_M_PRESETS && !s_page_presets && !s_page_bend) {   /* mode KEYS : PRESETS va de piste en piste */
            s_crans[i] += pas;
            if (s_crans[i] > 3) s_crans[i] = 3;
            if (s_crans[i] < -3) s_crans[i] = -3;
            continue;
        }
        if ((i >= FM1_M_K1 && i <= FM1_M_K4) || i == FM1_M_PRESETS || i == FM1_M_ALGORITHM) {
            if (s_molettes_instr)                 /* la molette regle l'instrument ; ailleurs, rien */
                s_fx_pas[i == FM1_M_PRESETS ? 4 : i == FM1_M_ALGORITHM ? 5 : i - FM1_M_K1] += pas;
            continue;
        }
        if (FM1_MOLETTES[i].avec == FM1_TEMPO) {
            if (pas)
                tempo_pas(pas > 0 ? 1 : -1);
            continue;
        }
        if (!(FM1_MOLETTES[i].moins | FM1_MOLETTES[i].plus))
            continue;
        s_crans[i] += pas;
        if (s_crans[i] > 3) s_crans[i] = 3;
        if (s_crans[i] < -3) s_crans[i] = -3;
    }
    // Les accords des molettes (KNOB 1, 2) : enfonces une image, relaches la suivante.
    if (s_accord)
        s_accord = 0;
    else
        for (i = 0; i < FM1_NB_MOLETTES; i++)
            if (s_crans[i] && FM1_MOLETTES[i].avec) {
                const int plus = s_crans[i] > 0;
                s_accord = FM1_MOLETTES[i].avec | (plus ? FM1_MOLETTES[i].plus : FM1_MOLETTES[i].moins);
                s_crans[i] += plus ? -1 : 1;
                break;
            }
    s_tenues |= s_accord;
    // Une touche masquee disparait tant qu'elle reste enfoncee.
    s_masque &= s_tenues;
    s_tenues &= ~s_masque;
    // Repetition, comme libnds : apres `delai` images, puis toutes les `cadence`.
    if (s_delai) {
        if (s_tenues != s_avant) {
            s_compte = s_delai;
            s_repete = s_tenues & ~s_avant;
        }
        if (--s_compte == 0) {
            s_compte = s_cadence;
            s_repete = s_tenues;
        }
    }
    for (i = 0; i < FM1_NB_MOLETTES; i++)          /* un cran de molette par image */
        if (s_crans[i] && !FM1_MOLETTES[i].avec) {
            const int plus = s_crans[i] > 0;
            if (s_clavier && i == FM1_M_PRESETS)  /* mode KEYS : PRESETS va de piste en piste */
                s_repete |= plus ? KEY_RIGHT : KEY_LEFT;
            else
                s_repete |= plus ? FM1_MOLETTES[i].plus : FM1_MOLETTES[i].moins;
            s_crans[i] += plus ? -1 : 1;
        }
}
void keysIgnoreUntilReleased(u32 touches)
{
    s_masque |= touches & s_tenues;
    s_tenues &= ~touches;
    s_repete &= ~touches;
}
u32 keysHeld(void) { return s_tenues; }
u32 keysDown(void) { return s_tenues & ~s_avant; }
u32 keysUp(void) { return s_avant & ~s_tenues; }
u32 keysDownRepeat(void) { const u32 r = s_repete; s_repete = 0; return r; }
void keysSetRepeat(u8 delai, u8 cadence) { s_delai = delai; s_cadence = cadence; }

// ── Les timers ──────────────────────────────────────────────────────────────
volatile u16 rg_timer_cr[4];
volatile u16 rg_timer_data[4];
static const uint32_t kDiviseurs[4] = {1, 64, 256, 1024};
static uint64_t s_timer_depart[4];

static uint64_t maintenant_ticks(void)            /* TIMER4 (24 MHz), etendu sur 64 bits */
{
    static uint64_t cumul;
    static uint32_t dernier;
    const uint32_t f = irq_save(), t = fm1_ticks();
    cumul += (uint32_t)(t - dernier);
    dernier = t;
    irq_restore(f);
    return cumul;
}

void timerStart(int timer, int diviseur, u16 recharge, void (*rappel)(void))
{
    (void)rappel;
    rg_timer_cr[timer & 3] = (u16)(TIMER_ENABLE | diviseur);
    rg_timer_data[timer & 3] = recharge;
    s_timer_depart[timer & 3] = maintenant_ticks();
}

u16 timerTick(int timer)
{
    const uint64_t dt = maintenant_ticks() - s_timer_depart[timer & 3];
    return (u16)(dt * BUS_CLOCK / kDiviseurs[rg_timer_cr[timer & 3] & 3] / 24000000u);
}

// ── Le timer 0 du sequenceur : dans l'interruption audio ───────────────────
static void (*s_rappel)(void);
static uint8_t s_lance;
static volatile uint32_t s_periode_q16;          /* echantillons par tick, Q16 (0 : celle du timer) */
static uint32_t s_cumul_q16;
static uint8_t s_dans_tick;
static uint32_t s_ticks;

void rg_regle_periode_tick(double secondes)
{
    double p = secondes * (double)FS * 65536.0;
    s_periode_q16 = p < 65536.0 ? 65536u : p > 4.0e9 ? 4000000000u : (uint32_t)p;
}
bool rg_dans_tick(void) { return s_dans_tick != 0; }
double rg_heure_tick(void) { return (double)s_ticks; }
void rg_temps_reel(int priorite) { (void)priorite; }

// Comme sur la DS : enterCriticalSection coupe les interruptions et rend
// l'etat d'AVANT (1 : elles l'etaient deja) ; leaveCriticalSection(ancien) ne
// les rend que si elles etaient permises. Le verrou du moteur (md_lock.h)
// l'appelle a chaque prise et ne rend qu'a la derniere : un compteur qui
// monterait a chaque appel ne redescendrait jamais.
int enterCriticalSection(void)
{
    if (fm1_crit_irq || fm1_crit_prof)
        return 1;
    return (int)irq_save();
}
void leaveCriticalSection(int ancien) { irq_restore((uint32_t)ancien); }

void irqSet(int irq, void (*rappel)(void)) { if (irq == IRQ_TIMER0) s_rappel = rappel; }
void irqEnable(int irq) { if (irq == IRQ_TIMER0) s_lance = 1; }

static uint32_t periode_timer0_q16(void)          /* a defaut de periode exacte : celle du timer DS */
{
    const u16 cr = rg_timer_cr[0];
    if (!(cr & TIMER_ENABLE))
        return 0;
    return (uint32_t)((uint64_t)(0x10000u - rg_timer_data[0]) * kDiviseurs[cr & 3] * FS * 65536u / BUS_CLOCK);
}

// Appelee par l'interruption audio, une fois par bloc de n echantillons : les
// ticks qui tombent dans ce bloc passent tout de suite, avant son calcul.
/* L'horloge du son : les echantillons rendus depuis l'allumage, et un rappel a chaque bloc, dans
   l'interruption audio, avant le sequenceur (REC : le decompte, le depart du morceau et le metronome,
   a l'echantillon pres, sur l'horloge meme du morceau). */
static void (*s_rappel_audio)(void);
uint32_t fm1_echantillons(void) { return s_echantillons; }
uint32_t fm1_echantillons_par_seconde(void) { return FS; }
void fm1_rappel_audio(void (*f)(void)) { s_rappel_audio = f; }

uint32_t fm1_vitesse(void);                      /* (src/opl3.c : CLOCK PITCH, la vitesse de l'horloge, Q16) */
static uint32_t s_echantillons_reste;
static void fm1_sequenceur(uint32_t n)
{
    uint32_t p;
    const uint32_t v = fm1_vitesse();
    if (s_rappel_audio)
        s_rappel_audio();
    /* l'horloge de la musique (fm1_echantillons : le tracker, la DRUM MACHINE, le LOOPER, REC) avance a la
       vitesse de CLOCK PITCH, comme une cassette lue plus lentement */
    s_echantillons_reste += n * v;
    s_echantillons += s_echantillons_reste >> 16;
    s_echantillons_reste &= 0xFFFFu;
    if (!s_lance || !s_rappel)
        return;
    p = s_periode_q16 ? s_periode_q16 : periode_timer0_q16();
    if (!p)
        return;
    {   /* (en local : sinon le compilateur fait un « [mem] += 0x100000 » que l'emulateur ne connait pas) */
        uint32_t cumul = s_cumul_q16 + n * v;
        while (cumul >= p) {
            cumul -= p;
            s_dans_tick = 1;
            s_rappel();
            s_dans_tick = 0;
            s_ticks++;
            p = s_periode_q16 ? s_periode_q16 : p;    /* une commande T a pu changer le tempo */
        }
        s_cumul_q16 = cumul;
    }
}

// ── La « carte SD » : la flash ──────────────────────────────────────────────
bool fatInitDefault(void)
{
    fs_init();
    return flash_ok != 0;
}

static char s_arg0[] = "/OPLTRACKER/opltracker";
static char *s_argv[] = { s_arg0, 0 };
static struct __argv s_args = { ARGV_MAGIC, s_arg0, sizeof s_arg0, 1, s_argv };
struct __argv *__system_argv = &s_args;

// ── L'image ─────────────────────────────────────────────────────────────────
static void fm1_entretien(void);                  /* main.c : chien de garde, USB, mise a jour, MASTER */

void swiWaitForVBlank(void)
{
    static uint32_t prochaine;
    montre();
    fm1_entretien();
    if ((int32_t)(fm1_ms - prochaine) > 100)
        prochaine = fm1_ms;
    prochaine += 16u;                             /* (environ 60 images par seconde) */
    while ((int32_t)(fm1_ms - prochaine) < 0)     /* (fm1_ms en RAM, rien d'autre : le chien de garde est nourri */
        ;                                         /*  une fois par image, par fm1_entretien) */
}
