// ============================================================================
//  Le transfert de morceaux par USB : l'application du Mac (tracker/mac) envoie
//  des fichiers (.A2M...) que le FM-1 enregistre dans le dossier des morceaux,
//  ou LOAD les trouve.
//
//  Par SysEx, sur le meme chemin que la mise a jour (ota.c) : une trame est
//  F0 pack7(message) F7 (8 bits -> 7, poids faibles d'abord). Nos messages
//  commencent par 00 'T' 'K' ; ceux de M-UPGRADE par 00 59 : chacun reconnait
//  les siens. Le Mac attend la reponse a chaque message avant d'envoyer le
//  suivant : le FM-1 n'est jamais deborde.
//
//  Mac -> FM-1 : 00 'T' 'K' cmd ...
//    00 BONJOUR                        -> l'identite du tracker
//    01 DEBUT   taille(4) nom...0      -> un fichier commence
//    02 DONNEES position(4) octets...  -> 512 octets au plus
//    03 FIN     crc32(4)               -> le fichier est complet : enregistre
//    04 ANNULE                         -> on oublie le fichier en cours
//  Et dans l'autre sens (RECUPERER, l'export A2M par exemple) :
//    05 LISTE   numero(1)              -> le n-ieme morceau : taille(4) nom (statut 1 : il n'y en a plus)
//    06 OUVRE   nom...0                -> taille(4)
//    07 LIT     position(4)            -> position(4) puis 256 octets au plus
//    08 FERME
//  Diagnostic : 09 INFO (la flash, la memoire), 0A ENTREE n (le repertoire)
//  FM-1 -> Mac : 00 'T' 'K' (cmd | 0x40) statut [...]
//    statut 0 : bien recu ; 1 : impossible (place, nom) ; 2 : position
//    inattendue (suivie de la position attendue, 4 octets) ; 3 : erreur de
//    contenu (crc, taille).
//  Les nombres sont petit-boutistes.
// ============================================================================
#define TK_DONNEES_MAX 512u
#define TK_DECODE_MAX (8u + TK_DONNEES_MAX + 16u)
#define TK_SILENCE_MS 15000u                      /* un transfert abandonne par le Mac : on le referme */
#define TK_LIT_MAX 256u                           /* octets de fichier par reponse LIT */
#define TK_DOSSIER "/OPLTRACKER/Songs"

static uint8_t tk_dec[TK_DECODE_MAX];
static FILE *tk_f, *tk_lf;                       /* le fichier recu ; celui qu'on envoie au Mac */
static char tk_chemin[80];
static uint32_t tk_taille, tk_recu, tk_crc, tk_vu_ms;

static uint32_t tk_lit32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t tk_crc32(uint32_t c, const uint8_t *p, uint32_t n)   /* le CRC-32 de zlib (c : 0 au depart) */
{
    uint32_t k;
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void tk_repond(uint8_t cmd, uint8_t statut, const uint8_t *p, uint32_t n)
{
    static uint8_t msg[5 + 4 + TK_LIT_MAX], fil[2 + ((5u + 4u + TK_LIT_MAX) * 8u + 6u) / 7u];
    uint32_t w;
    if (n > 4u + TK_LIT_MAX)
        n = 4u + TK_LIT_MAX;
    msg[0] = 0;
    msg[1] = 'T';
    msg[2] = 'K';
    msg[3] = (uint8_t)(cmd | 0x40u);
    msg[4] = statut;
    memcpy(msg + 5, p, n);
    fil[0] = 0xF0;
    w = 1u + ota_pack7(msg, 5u + n, fil + 1);
    fil[w++] = 0xF7;
    ota_wire_send(fil, w);
}

static void tk_abandonne(void)                    /* le fichier en cours n'est pas complet : il disparait */
{
    if (!tk_f)
        return;
    fclose(tk_f);
    tk_f = 0;
    remove(tk_chemin);
}

/* Un nom de fichier sur : lettres, chiffres, espace, - _ . ; pas de chemin. */
static int tk_nom_ok(const char *n)
{
    uint32_t i;
    if (!n[0] || n[0] == '.')
        return 0;
    for (i = 0; n[i]; i++) {
        const char c = n[i];
        if (i >= 40u || !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                          c == ' ' || c == '-' || c == '_' || c == '.'))
            return 0;
    }
    return 1;
}

/* Une fois par image (fm1_entretien), avant la mise a jour : nos messages, s'il y en a. */
static void tk_service(void)
{
    const uint8_t *p;
    uint32_t n, d;
    if (tk_f && fm1_ms - tk_vu_ms > TK_SILENCE_MS)
        tk_abandonne();
    if (tk_lf && fm1_ms - tk_vu_ms > TK_SILENCE_MS) {
        fclose(tk_lf);
        tk_lf = 0;
    }
    if (!ota_frame_get(&p, &n))
        return;
    d = ota_unpack7(p, n, tk_dec, sizeof tk_dec);
    if (d < 4u || d > sizeof tk_dec || tk_dec[0] != 0 || tk_dec[1] != 'T' || tk_dec[2] != 'K')
        return;                                   /* pas a nous : pour la mise a jour (ota_service) */
    ota_frame_done();
    tk_vu_ms = fm1_ms;
    switch (tk_dec[3]) {
    case 0x00: {                                  /* BONJOUR */
        static const char id[] = "OPLBEND TRACKER " TK_VERSION;
        tk_repond(0x00, 0, (const uint8_t *)id, sizeof id - 1u);
        break;
    }
    case 0x01: {                                  /* DEBUT */
        char nom[48];
        uint32_t k = 0;
        tk_abandonne();
        if (d < 9u) {
            tk_repond(0x01, 1, 0, 0);
            break;
        }
        while (8u + k < d && k + 1u < sizeof nom && tk_dec[8 + k])
            nom[k] = (char)tk_dec[8 + k], k++;
        nom[k] = 0;
        tk_taille = tk_lit32(tk_dec + 4);
        tk_recu = 0;
        tk_crc = 0;
        if (!tk_nom_ok(nom) || tk_taille == 0 || tk_taille > 512u * 1024u) {
            tk_repond(0x01, 1, 0, 0);
            break;
        }
        snprintf(tk_chemin, sizeof tk_chemin, "/OPLTRACKER/Songs/%s", nom);
        tk_f = fopen(tk_chemin, "wb");
        tk_repond(0x01, tk_f ? 0 : 1, 0, 0);
        break;
    }
    case 0x02: {                                  /* DONNEES */
        const uint32_t pos = d >= 8u ? tk_lit32(tk_dec + 4) : 0xFFFFFFFFu, m = d >= 8u ? d - 8u : 0;
        uint8_t attendu[4];
        if (!tk_f) {
            tk_repond(0x02, 1, 0, 0);
            break;
        }
        if (pos != tk_recu) {                     /* (un message perdu : le Mac reprend ici) */
            attendu[0] = (uint8_t)tk_recu;
            attendu[1] = (uint8_t)(tk_recu >> 8);
            attendu[2] = (uint8_t)(tk_recu >> 16);
            attendu[3] = (uint8_t)(tk_recu >> 24);
            tk_repond(0x02, 2, attendu, 4);
            break;
        }
        if (tk_recu + m > tk_taille || fwrite(tk_dec + 8, 1, m, tk_f) != m) {
            tk_abandonne();
            tk_repond(0x02, 1, 0, 0);             /* (la flash est pleine) */
            break;
        }
        tk_crc = tk_crc32(tk_crc, tk_dec + 8, m);
        tk_recu += m;
        tk_repond(0x02, 0, tk_dec + 4, 4);
        break;
    }
    case 0x03: {                                  /* FIN */
        int ok;
        if (!tk_f) {
            tk_repond(0x03, 1, 0, 0);
            break;
        }
        ok = d >= 8u && tk_recu == tk_taille && tk_lit32(tk_dec + 4) == tk_crc;
        if (!ok) {
            tk_abandonne();
            tk_repond(0x03, 3, 0, 0);
            break;
        }
        ok = fclose(tk_f) == 0;
        tk_f = 0;
        if (!ok)
            remove(tk_chemin);
        tk_repond(0x03, ok ? 0 : 1, 0, 0);
        break;
    }
    case 0x10: {                                  /* SONDE port : l'etat des broches (diagnostic, tools/sonde_broches.py) */
        /* Pour chercher la detection du casque : le port tk_dec[4] (0..7), ses reglages, et ses entrees lues
           tel quel, puis entree numerique activee sur ses broches en entree, sans tirage, tirees en haut,
           tirees en bas. Tout est remis comme avant, interruptions coupees le temps de la mesure. */
        const uint32_t p = d >= 5u ? (tk_dec[4] & 7u) : 0u;
        uint32_t v[8], k, f;
        volatile uint32_t *base = (volatile uint32_t *)(0x50000u + p * 0x40u);
        const uint32_t dir = base[2], die = base[3], pu = base[4], pd = base[5], in0 = base[1];
        uint8_t o[32];
        f = irq_save();
        base[3] = die | dir;                      /* entree numerique sur les broches en entree */
        base[4] = pu & ~dir; base[5] = pd & ~dir; /* sans tirage */
        for (k = 0; k < 200u; k++) __asm__ volatile("");
        v[5] = base[1];
        base[4] = pu | dir; base[5] = pd & ~dir;  /* tirees vers le haut */
        for (k = 0; k < 200u; k++) __asm__ volatile("");
        v[6] = base[1];
        base[4] = pu & ~dir; base[5] = pd | dir;  /* tirees vers le bas */
        for (k = 0; k < 200u; k++) __asm__ volatile("");
        v[7] = base[1];
        base[3] = die; base[4] = pu; base[5] = pd;
        irq_restore(f);
        v[0] = dir; v[1] = die; v[2] = pu; v[3] = pd; v[4] = in0;
        for (k = 0; k < 8u; k++) {
            o[4 * k] = (uint8_t)v[k]; o[4 * k + 1] = (uint8_t)(v[k] >> 8);
            o[4 * k + 2] = (uint8_t)(v[k] >> 16); o[4 * k + 3] = (uint8_t)(v[k] >> 24);
        }
        tk_repond(0x10, 0, o, sizeof o);
        break;
    }
    case 0x05: {                                  /* LISTE numero : le n-ieme morceau du dossier */
        DIR *dd = opendir(TK_DOSSIER);
        struct dirent *e;
        uint32_t k = 0;
        const uint32_t voulu = d >= 5u ? tk_dec[4] : 0u;
        uint8_t o[4 + 40];
        int trouve = 0;
        if (dd) {
            while ((e = readdir(dd)) != 0) {
                char chemin[80];
                struct stat st;
                uint32_t lg;
                if (e->d_type == DT_DIR)
                    continue;
                if (k++ != voulu)
                    continue;
                snprintf(chemin, sizeof chemin, "%s/%s", TK_DOSSIER, e->d_name);
                lg = stat(chemin, &st) == 0 ? (uint32_t)st.st_size : 0u;
                o[0] = (uint8_t)lg; o[1] = (uint8_t)(lg >> 8); o[2] = (uint8_t)(lg >> 16); o[3] = (uint8_t)(lg >> 24);
                lg = (uint32_t)strlen(e->d_name);
                if (lg > 40u)
                    lg = 40u;
                memcpy(o + 4, e->d_name, lg);
                tk_repond(0x05, 0, o, 4u + lg);
                trouve = 1;
                break;
            }
            closedir(dd);
        }
        if (!trouve)
            tk_repond(0x05, 1, 0, 0);
        break;
    }
    case 0x06: {                                  /* OUVRE nom : le fichier a envoyer au Mac */
        char nom[48], chemin[80];
        uint32_t k = 0, lg;
        uint8_t o[4];
        if (tk_lf)
            fclose(tk_lf);
        tk_lf = 0;
        while (4u + k < d && k + 1u < sizeof nom && tk_dec[4 + k])
            nom[k] = (char)tk_dec[4 + k], k++;
        nom[k] = 0;
        if (!tk_nom_ok(nom)) {
            tk_repond(0x06, 1, 0, 0);
            break;
        }
        snprintf(chemin, sizeof chemin, "%s/%s", TK_DOSSIER, nom);
        tk_lf = fopen(chemin, "rb");
        if (!tk_lf || fseek(tk_lf, 0, SEEK_END)) {
            if (tk_lf)
                fclose(tk_lf);
            tk_lf = 0;
            tk_repond(0x06, 1, 0, 0);
            break;
        }
        lg = (uint32_t)ftell(tk_lf);
        o[0] = (uint8_t)lg; o[1] = (uint8_t)(lg >> 8); o[2] = (uint8_t)(lg >> 16); o[3] = (uint8_t)(lg >> 24);
        tk_repond(0x06, 0, o, 4);
        break;
    }
    case 0x07: {                                  /* LIT position : 256 octets au plus, a partir de la */
        static uint8_t o[4 + TK_LIT_MAX];
        const uint32_t pos = d >= 8u ? tk_lit32(tk_dec + 4) : 0u;
        size_t n;
        if (!tk_lf || fseek(tk_lf, (long)pos, SEEK_SET)) {
            tk_repond(0x07, 1, 0, 0);
            break;
        }
        memcpy(o, tk_dec + 4, 4);
        n = fread(o + 4, 1, TK_LIT_MAX, tk_lf);
        tk_repond(0x07, 0, o, 4u + (uint32_t)n);
        break;
    }
    case 0x08:                                    /* FERME */
        if (tk_lf)
            fclose(tk_lf);
        tk_lf = 0;
        tk_repond(0x08, 0, 0, 0);
        break;
    case 0x09: {                                  /* INFO : la flash et la memoire (diagnostic) */
        uint8_t o[14];
        fs_etat(o);
        tk_repond(0x09, 0, o, sizeof o);
        break;
    }
    case 0x0A: {                                  /* ENTREE n : la n-ieme entree du repertoire (diagnostic) */
        uint8_t o[48];
        const int n = fs_entree(d >= 5u ? tk_dec[4] : 0u, o);
        if (n < 0)
            tk_repond(0x0A, 1, 0, 0);
        else
            tk_repond(0x0A, 0, o, (uint32_t)n);
        break;
    }
    case 0x04:                                    /* ANNULE */
        tk_abandonne();
        tk_repond(0x04, 0, 0, 0);
        break;
    default:
        tk_repond(tk_dec[3] & 0x3Fu, 1, 0, 0);
        break;
    }
}
