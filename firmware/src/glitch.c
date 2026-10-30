/* SPDX-License-Identifier: GPL-3.0-only */
/* GLITCH (FX, second press): an effect on the whole mix in the spirit of dBlue Glitch, a pattern of 16
 * steps, one per tracker row: synced on the playing phrase, or on the drum machine (fm1_glitch_ligne,
 * from the tracker, each block); when nothing plays (-1), it lets the sound through and stands still. Each step has
 * its effect (0 = none). The effects that replay sound read a ring of the recent mix (mono, 16-bit, on
 * the heap only while a step is set: fm1_glitch_tampon). Included by audio.c, run in the audio ISR
 * between the OPL3 and the output stage. */
#define GL_N 16384u                               /* frames in the ring (a power of two) */
#define GL_NB 9                                   /* effects, 0 = none */

static int16_t *volatile gl_buf;
static uint8_t gl_pas[16];
static volatile uint32_t gl_lg = 5294u;           /* samples per step (a row at 125 BPM, 4 rows a beat) */
static volatile int gl_ligne = -1;                /* the playing row (-1: stopped, free-running) */
static volatile int gl_etape;                     /* the step that plays */
static uint32_t gl_ecrit, gl_debut, gl_phase, gl_pos16;
static int gl_vu = -2;
static int32_t gl_lp[2], gl_tenu[2];

void fm1_glitch_pas(int k, int e)
{
    if (k >= 0 && k < 16)
        gl_pas[k] = (uint8_t)(e < 0 ? 0 : e >= GL_NB ? GL_NB - 1 : e);
}
int fm1_glitch_etape(void) { return gl_ligne < 0 ? -1 : gl_etape; }   /* (-1 : a l'arret) */
void fm1_glitch_tempo(uint32_t ech) { gl_lg = ech < 256u ? 256u : ech > GL_N / 2u ? GL_N / 2u : ech; }
void fm1_glitch_tampon(void *p)
{
    if (p)
        memset(p, 0, GL_N * sizeof(int16_t));
    gl_buf = (int16_t *)p;
}
void fm1_glitch_ligne(int l) { gl_ligne = l; }

/* (aucune division par echantillon : tout ce qui depend de la longueur du pas se calcule a son debut, puis
   des compteurs avancent ; les divisions, 64 bits surtout, ralentissaient toute l'interface) */
static uint32_t gl_s2, gl_s8, gl_q, gl_c2, gl_c8, gl_co, gl_cq, gl_porte;
static uint32_t gl_v24, gl_dv24;                  /* TAP : la vitesse (Q24), sa baisse par echantillon */
static int32_t gl_k16, gl_dk16;                   /* FLT : le coefficient (Q15 << 16), sa baisse */

static void glitch_pas_debut(uint32_t L)
{
    gl_s2 = L / 2u ? L / 2u : 1u;
    gl_s8 = L / 8u ? L / 8u : 1u;
    gl_q = L / 4u + 1u;
    gl_c2 = gl_c8 = gl_co = gl_cq = gl_porte = 0;
    gl_v24 = 1u << 24;
    gl_dv24 = (1u << 24) / L;
    gl_k16 = 30600 << 16;
    gl_dk16 = (int32_t)((30000u << 16) / L);
}

static void glitch_rend(int32_t *o, uint32_t n)
{
    int16_t *b = gl_buf;
    uint32_t i, c;
    const uint32_t L = gl_lg;
    const int l = gl_ligne;
    if (!b)
        return;
    if (l < 0) {                                  /* rien ne joue : le son passe tel quel */
        gl_vu = l;
        return;
    }
    if (l != gl_vu) {                   /* a new row: its step starts (the engine shows the next row) */
        gl_etape = (l + 15) & 15;
        gl_phase = 0;
        gl_debut = gl_ecrit;
        gl_pos16 = 0;
        glitch_pas_debut(L);
    }
    gl_vu = l;
    for (i = 0; i < n; i++) {
        int32_t m = (o[2u * i] + o[2u * i + 1u]) >> 1, y;
        const uint32_t ph = gl_phase;
        if (m > 32767) m = 32767;
        if (m < -32767) m = -32767;
        b[gl_ecrit & (GL_N - 1u)] = (int16_t)m;
        gl_ecrit++;
        switch (gl_pas[gl_etape]) {
        case 1:                                   /* RTG: the step's first half, over and over */
            y = b[(gl_debut + gl_c2) & (GL_N - 1u)];
            if (++gl_c2 >= gl_s2) gl_c2 = 0;
            o[2u * i] = o[2u * i + 1u] = y;
            break;
        case 2:                                   /* STU: a stutter, an eighth of the step */
            y = b[(gl_debut + gl_c8) & (GL_N - 1u)];
            if (++gl_c8 >= gl_s8) gl_c8 = 0;
            o[2u * i] = o[2u * i + 1u] = y;
            break;
        case 3:                                   /* REV: the step before, backwards */
            y = b[(gl_debut - 1u - (ph & (GL_N / 2u - 1u))) & (GL_N - 1u)];
            o[2u * i] = o[2u * i + 1u] = y;
            break;
        case 4:                                   /* TAP: a tape stop, down to nothing over the step */
            gl_pos16 += gl_v24 >> 8;
            gl_v24 = gl_v24 > gl_dv24 ? gl_v24 - gl_dv24 : 0u;
            y = b[(gl_debut + (gl_pos16 >> 16)) & (GL_N - 1u)];
            o[2u * i] = o[2u * i + 1u] = (int32_t)(((int64_t)y * (int32_t)(gl_v24 >> 12)) >> 12);
            break;
        case 5:                                   /* OCT: the first half, an octave up */
            y = b[(gl_debut + gl_co) & (GL_N - 1u)];
            gl_co += 2u;
            if (gl_co >= gl_s2) gl_co -= gl_s2;
            o[2u * i] = o[2u * i + 1u] = y;
            break;
        case 6:                                   /* GAT: chopped in four */
            if (++gl_cq >= gl_q) { gl_cq = 0; gl_porte ^= 1u; }
            if (gl_porte)
                o[2u * i] = o[2u * i + 1u] = 0;
            break;
        case 7:                                   /* CRU: held every 8 samples, 6 bits */
            for (c = 0; c < 2u; c++) {
                if ((ph & 7u) == 0u)
                    gl_tenu[c] = o[2u * i + c] & ~0x3FF;
                o[2u * i + c] = gl_tenu[c];
            }
            break;
        case 8: {                                 /* FLT: a low-pass closing over the step */
            const int32_t k = gl_k16 >> 16;
            gl_k16 = gl_k16 - gl_dk16 > (600 << 16) ? gl_k16 - gl_dk16 : 600 << 16;
            for (c = 0; c < 2u; c++) {
                gl_lp[c] += (int32_t)(((int64_t)(o[2u * i + c] - gl_lp[c]) * k) >> 15);
                o[2u * i + c] = gl_lp[c];
            }
            break;
        }
        default:
            gl_lp[0] = o[2u * i];
            gl_lp[1] = o[2u * i + 1u];
            break;
        }
        gl_phase++;
    }
}
