// ============================================================================
//  Ce que la libc de JieLi n'a pas et dont le tracker se sert : malloc et ses
//  freres, la famille printf, exit / abort. (Les chaines, les maths et qsort
//  viennent de la libc de JieLi.)
//
//  Le tas occupe la RAM laissee libre par .data / .bss, de _heap_start a
//  _heap_end (firmware/app.ld). Allocateur a liste libre, premier bloc qui
//  convient, blocs voisins recolles a la liberation : le tracker n'alloue
//  presque rien (le module, les macros FM, les tampons de fichiers).
// ============================================================================
#include <stdarg.h>
#include <stddef.h>

extern uint8_t _heap_start[], _heap_end[];

typedef struct bloc { size_t taille; struct bloc *suivant; } bloc_t;   // taille : en-tete compris
#define ALIGNE(n) (((n) + 7u) & ~(size_t)7u)
#define TETE ALIGNE(sizeof(bloc_t))
static bloc_t *tas_libre;
static size_t tas_utilise, tas_pic;
static uint8_t tas_pret;

static void tas_init(void)
{
    uintptr_t d = ALIGNE((uintptr_t)_heap_start), f = (uintptr_t)_heap_end & ~(uintptr_t)7u;
    tas_libre = (bloc_t *)d;
    tas_libre->taille = f - d;
    tas_libre->suivant = 0;
    tas_pret = 1;
}

static size_t tas_libre_total(void)
{
    size_t n = 0;
    bloc_t *b;
    for (b = tas_libre; b; b = b->suivant)
        n += b->taille;
    return n;
}

void *malloc(size_t n)
{
    bloc_t **pp, *b;
    size_t t;
    uint32_t f;
    if (!n)
        n = 1;
    t = ALIGNE(n) + TETE;
    f = irq_save();
    if (!tas_pret)
        tas_init();
    for (pp = &tas_libre; (b = *pp) != 0; pp = &b->suivant)
        if (b->taille >= t) {
            if (b->taille - t >= TETE + 16u) {        // on coupe : la fin reste libre
                bloc_t *r = (bloc_t *)((uint8_t *)b + t);
                r->taille = b->taille - t;
                r->suivant = b->suivant;
                *pp = r;
                b->taille = t;
            } else
                *pp = b->suivant;
            tas_utilise += b->taille;
            if (tas_utilise > tas_pic)
                tas_pic = tas_utilise;
            irq_restore(f);
            return (uint8_t *)b + TETE;
        }
    irq_restore(f);
    return 0;
}

void free(void *p)
{
    bloc_t *b, **pp, *c;
    uint32_t f;
    if (!p)
        return;
    b = (bloc_t *)((uint8_t *)p - TETE);
    f = irq_save();
    tas_utilise -= b->taille;
    // rangee par adresse, pour recoller avec les voisines
    for (pp = &tas_libre; (c = *pp) != 0 && c < b; pp = &c->suivant)
        ;
    b->suivant = c;
    *pp = b;
    if (c && (uint8_t *)b + b->taille == (uint8_t *)c) {
        b->taille += c->taille;
        b->suivant = c->suivant;
    }
    if (pp != &tas_libre) {
        bloc_t *a = (bloc_t *)((uint8_t *)pp - offsetof(bloc_t, suivant));
        if ((uint8_t *)a + a->taille == (uint8_t *)b) {
            a->taille += b->taille;
            a->suivant = b->suivant;
        }
    }
    irq_restore(f);
}

void *calloc(size_t n, size_t t)
{
    size_t total = n * t;
    void *p = (t && total / t != n) ? 0 : malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *realloc(void *p, size_t n)
{
    bloc_t *b;
    size_t ancien;
    void *q;
    if (!p)
        return malloc(n);
    if (!n) {
        free(p);
        return 0;
    }
    b = (bloc_t *)((uint8_t *)p - TETE);
    ancien = b->taille - TETE;
    if (ancien >= n)
        return p;
    q = malloc(n);
    if (q) {
        memcpy(q, p, ancien);
        free(p);
    }
    return q;
}

// ── exit / abort : il n'y a nulle part ou revenir ───────────────────────────
static void fm1_fatal(const char *quoi);
void exit(int code) { (void)code; fm1_fatal("EXIT"); for (;;) ; }
void abort(void) { fm1_fatal("ABORT"); for (;;) ; }
void _exit(int code) { exit(code); }
void __cxa_pure_virtual(void) { fm1_fatal("PURE VIRTUAL"); }

// ── printf ──────────────────────────────────────────────────────────────────
typedef struct { char *p; size_t reste; size_t total; } sortie_t;

static void sort_c(sortie_t *s, char c)
{
    if (s->reste > 1) {
        *s->p++ = c;
        s->reste--;
    }
    s->total++;
}

int vsnprintf(char *dest, size_t n, const char *fmt, va_list ap)
{
    sortie_t s = {dest, n, 0};
    char tmp[24];
    for (; *fmt; fmt++) {
        int gauche = 0, plus = 0, espace = 0, zero = 0, largeur = 0, prec = -1, lon = 0;
        const char *txt;
        int lg, i, neg = 0;
        uint32_t v;                               /* (32 bits : rien ne s'affiche en %ll, et la boucle 64 bits
                                                     lisait la retenue par sextra, que l'emulateur web rate) */
        if (*fmt != '%') {
            sort_c(&s, *fmt);
            continue;
        }
        fmt++;
        for (;; fmt++) {
            if (*fmt == '-') gauche = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') espace = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '#') ;
            else break;
        }
        if (*fmt == '*') {
            largeur = va_arg(ap, int);
            if (largeur < 0) { gauche = 1; largeur = -largeur; }
            fmt++;
        } else
            while (*fmt >= '0' && *fmt <= '9')
                largeur = largeur * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 'j' || *fmt == 't') {
            if (*fmt == 'l') lon++;
            fmt++;
        }
        switch (*fmt) {
        case 'd': case 'i':
            {
                long long x = lon >= 2 ? va_arg(ap, long long) : lon ? (long long)va_arg(ap, long) : (long long)va_arg(ap, int);
                neg = x < 0;
                v = (uint32_t)(neg ? (unsigned long long)(-x) : (unsigned long long)x);
            }
            goto nombre;
        case 'u': case 'x': case 'X':
            v = lon >= 2 ? (uint32_t)va_arg(ap, unsigned long long) : lon ? (uint32_t)va_arg(ap, unsigned long)
                                                                 : (uint32_t)va_arg(ap, unsigned);
        nombre: {
            const unsigned base = (*fmt == 'x' || *fmt == 'X') ? 16u : 10u;
            const char *chiffres = *fmt == 'x' ? "0123456789abcdef" : "0123456789ABCDEF";
            char signe = neg ? '-' : plus ? '+' : espace ? ' ' : 0;
            int nc = 0, remplit;
            do {
                tmp[nc++] = chiffres[v % base];
                v /= base;
            } while (v && nc < (int)sizeof tmp);
            while (prec > nc && nc < (int)sizeof tmp)
                tmp[nc++] = '0';
            remplit = largeur - nc - (signe ? 1 : 0);
            if (!gauche && !zero)
                for (i = 0; i < remplit; i++) sort_c(&s, ' ');
            if (signe) sort_c(&s, signe);
            if (!gauche && zero)
                for (i = 0; i < remplit; i++) sort_c(&s, '0');
            while (nc) sort_c(&s, tmp[--nc]);
            if (gauche)
                for (i = 0; i < remplit; i++) sort_c(&s, ' ');
            break;
        }
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            tmp[1] = 0;
            txt = tmp;
            lg = 1;
            goto texte;
        case 's':
            txt = va_arg(ap, const char *);
            if (!txt) txt = "(null)";
            for (lg = 0; txt[lg] && (prec < 0 || lg < prec); lg++)
                ;
        texte:
            if (!gauche)
                for (i = lg; i < largeur; i++) sort_c(&s, ' ');
            for (i = 0; i < lg; i++) sort_c(&s, txt[i]);
            if (gauche)
                for (i = lg; i < largeur; i++) sort_c(&s, ' ');
            break;
        case '%':
            sort_c(&s, '%');
            break;
        case 0:
            fmt--;
            break;
        default:                                  // format inconnu : recopie tel quel
            sort_c(&s, '%');
            sort_c(&s, *fmt);
            break;
        }
    }
    if (n)
        *s.p = 0;
    return (int)s.total;
}

int vsprintf(char *s, const char *fmt, va_list ap) { return vsnprintf(s, (size_t)-1 / 2, fmt, ap); }

int snprintf(char *s, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *s, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(s, (size_t)-1 / 2, fmt, ap);
    va_end(ap);
    return r;
}

// ── Le journal : stdout / stderr finissent dans un anneau en RAM ────────────
// (lisible avec le debogueur de l'emulateur : fm1_journal, fm1_journal_pos)
char fm1_journal[1024];
volatile uint32_t fm1_journal_pos;

static void journal_ecrit(const char *p, size_t n)
{
    while (n--)
        fm1_journal[fm1_journal_pos++ % sizeof fm1_journal] = *p++;
}

int vfprintf(FILE *f, const char *fmt, va_list ap);
int printf(const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int puts(const char *s)
{
    journal_ecrit(s, strlen(s));
    journal_ecrit("\n", 1);
    return 0;
}

int putchar(int c)
{
    char x = (char)c;
    journal_ecrit(&x, 1);
    return c;
}
