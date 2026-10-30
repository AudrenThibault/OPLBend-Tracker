// ============================================================================
//  Ce que le tracker DS attend de libnds, refait pour le M-VAVE FM-1.
//  (Calque sur compat/nds.h de la version RG350.)
//
//  Le code du tracker reste celui de la DS / RG350 : il dessine dans un ecran
//  de RG_ECRAN_L x RG_ECRAN_H (couleurs DS sur 15 bits), lit les touches de la
//  DS et fait battre son sequenceur sur le timer 0. Ici :
//    - l'ecran est une image de 320 x 240 (8 bits, palette) dont l'afficheur
//      240 x 240 du FM-1 montre une fenetre qui suit le curseur ; l'« ecran du
//      bas » de la DS partage cette image (le tracker ne fait qu'y effacer puis
//      repeindre tout de suite l'ecran du haut) ;
//    - les touches DS viennent des boutons, des touches et des molettes du
//      FM-1, selon la table de fm1_touches.h ;
//    - le timer 0 bat dans l'interruption audio, a l'echantillon pres, et
//      « couper les interruptions » coupe vraiment celles du FM-1.
// ============================================================================
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "nds/ndstypes.h"
#include "nds/interrupts.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── La surface de dessin ────────────────────────────────────────────────────
// La grille de la RG350 : 40 colonnes, 21 lignes, les pages gardent leur
// disposition, avec la police 5x7 a sa taille d'origine (MD_ZOOM 100 : les
// valeurs ET les intitules en 5x7). L'afficheur du FM-1 montre cette image
// AGRANDIE (FM1_CELL_ECRAN_W / _H ci-dessous), donc une fenetre de l'image qui
// suit le curseur dans les deux sens (fm1_plateforme.c) : environ 17 colonnes
// et 12 lignes a la fois.
#ifndef MD_ZOOM
#define MD_ZOOM 100
#endif
#define FM1_CELL_W (5 * MD_ZOOM / 100 + 1)
#define FM1_CELL_H (7 * MD_ZOOM / 100 + 1)   /* (md_font.h, FM1 : un point d'interligne) */
#define RG_ECRAN_L (40 * FM1_CELL_W)
#define RG_ECRAN_H (21 * FM1_CELL_H)
// A l'ecran, une cellule de l'image (6 x 8 points) devient FM1_CELL_ECRAN_W x
// FM1_CELL_ECRAN_H points ; chaque cellule s'agrandit de la meme facon, donc
// toutes les lettres restent identiques. 14 points de large : 4 pistes de SONG
// tiennent exactement dans les 240 points de l'afficheur.
#define FM1_CELL_ECRAN_W 14
#define FM1_CELL_ECRAN_H 19
#define FM1_CELL_TITRE_H 8    /* la rangee des titres de colonnes : la police d'origine, 5 x 7 */
#define FM1_CELL_COMPACTE_H 14   /* les rangees de PHRASE (cellules de 11 x 14) : ses 16 lignes tiennent */
#define FM1_VUE_L_COMPACTE ((240 * FM1_CELL_W + 10) / 11)
#define FM1_VUE_L  ((240 * FM1_CELL_W + FM1_CELL_ECRAN_W - 1) / FM1_CELL_ECRAN_W)   /* en points de l'image */
#define FM1_VUE_H  ((240 * FM1_CELL_H + FM1_CELL_ECRAN_H - 1) / FM1_CELL_ECRAN_H)

// ── Couleurs DS : 1 bit d'alpha, puis 5 bits de bleu, vert, rouge ───────────
#define ARGB16(a, r, g, b) ((u16)(((a) << 15) | ((b) << 10) | ((g) << 5) | (r)))
#define RGB15(r, g, b)     ((u16)(((b) << 10) | ((g) << 5) | (r)))

// ── Touches, valeurs de libnds ──────────────────────────────────────────────
enum {
  KEY_A = 1 << 0, KEY_B = 1 << 1, KEY_SELECT = 1 << 2, KEY_START = 1 << 3,
  KEY_RIGHT = 1 << 4, KEY_LEFT = 1 << 5, KEY_UP = 1 << 6, KEY_DOWN = 1 << 7,
  KEY_R = 1 << 8, KEY_L = 1 << 9, KEY_X = 1 << 10, KEY_Y = 1 << 11,
  KEY_TOUCH = 1 << 12, KEY_LID = 1 << 13
};
void scanKeys(void);
u32 keysHeld(void);
u32 keysDown(void);
u32 keysUp(void);
u32 keysDownRepeat(void);
void keysSetRepeat(u8 delai, u8 cadence);
void keysIgnoreUntilReleased(u32 touches);

// ── Image ───────────────────────────────────────────────────────────────────
// Envoie ce qui a change a l'afficheur, fait l'entretien du FM-1 (chien de
// garde, USB, mise a jour, molette MASTER) et attend l'image suivante (60 Hz).
void swiWaitForVBlank(void);

#define MODE_5_2D 0
#define POWER_ALL_2D 0
#define VRAM_A_MAIN_BG 0
#define VRAM_C_SUB_BG 0
typedef enum { BgType_Bmp16 = 0 } BgType;
typedef enum { BgSize_B16_256x256 = 0 } BgSize;
static inline void powerOn(int x) { (void)x; }
static inline void videoSetMode(int x) { (void)x; }
static inline void videoSetModeSub(int x) { (void)x; }
static inline void vramSetBankA(int x) { (void)x; }
static inline void vramSetBankC(int x) { (void)x; }
int bgInit(int couche, BgType t, BgSize s, int a, int b);      // ecran du haut -> 0
int bgInitSub(int couche, BgType t, BgSize s, int a, int b);   // ecran du bas  -> 1
u16 *bgGetGfxPtr(int id);
static inline void DC_FlushRange(const void *p, u32 n) { (void)p; (void)n; }

// ── Timers ──────────────────────────────────────────────────────────────────
// Le timer 2 sert d'horloge a l'interface ; le timer 0 au sequenceur.
#define BUS_CLOCK 33513982u
enum { ClockDivider_1 = 0, ClockDivider_64 = 1, ClockDivider_256 = 2, ClockDivider_1024 = 3 };
#define TIMER_ENABLE  (1 << 7)
#define TIMER_IRQ_REQ (1 << 6)
extern volatile u16 rg_timer_cr[4];
extern volatile u16 rg_timer_data[4];
#define TIMER_CR(n)   (rg_timer_cr[n])
#define TIMER_DATA(n) (rg_timer_data[n])
void timerStart(int timer, int diviseur, u16 recharge, void (*rappel)(void));
u16 timerTick(int timer);

// ── Machine ─────────────────────────────────────────────────────────────────
static inline bool isDSiMode(void) { return true; }
static inline void setCpuClock(bool v) { (void)v; }
bool fatInitDefault(void);

// La periode exacte du tick (le moteur la donne a chaque changement de tempo),
// et l'heure du tick : sur le FM-1 le tick bat deja dans l'interruption audio,
// ses sorties partent donc a l'heure sans file d'attente.
void rg_regle_periode_tick(double secondes);
bool rg_dans_tick(void);
double rg_heure_tick(void);
void rg_temps_reel(int priorite);

// Le chemin du programme : le tracker range ses morceaux (Songs) a cote.
#define ARGV_MAGIC 0x5f617267
struct __argv { int argvMagic; char *commandLine; int length; int argc; char **argv; };
extern struct __argv *__system_argv;

// newlib de devkitARM : variantes « entieres seulement » de printf.
#define siprintf  sprintf
#define sniprintf snprintf
#define iprintf   printf
#define fiprintf  fprintf

#ifdef __cplusplus
}
#endif
