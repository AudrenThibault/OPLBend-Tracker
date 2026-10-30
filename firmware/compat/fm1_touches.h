// ============================================================================
//  LES TOUCHES DU TRACKER SUR LE FM-1 — c'est ICI, et nulle part ailleurs,
//  qu'on choisit quel bouton du FM-1 fait A, B, SELECT, START, la croix...
//
//  Le tracker est celui de la DS : il ne connait que les touches de la DS
//  (KEY_A, KEY_B, KEY_SELECT, KEY_START, la croix, L, R, X, Y). Chaque ligne
//  de FM1_TOUCHES ci-dessous donne, pour une touche DS, le controle du FM-1
//  qui l'enfonce. Plusieurs controles peuvent faire la meme touche DS.
//
//  Les controles du FM-1 :
//    - les boutons : FM1_B_FX, FM1_B_SCL, FM1_B_ENV, FM1_B_LFO, FM1_B_EDIT,
//      FM1_B_GLO, FM1_B_HOME, FM1_B_SAVE, FM1_B_ARP, FM1_B_SEQ, FM1_B_PLAY,
//      FM1_B_REC, FM1_B_OCTDN (OCT-), FM1_B_OCTUP (OCT+) ;
//    - les 27 touches du clavier : FM1_TOUCHE(n), n = 0 (F3) ... 26 (G5).
//      Do C4 = FM1_TOUCHE(7), do diese = 8, re = 9, re diese = 10, mi = 11...
//
//  Les molettes (encodeurs) donnent des appuis breves sur une touche DS par
//  cran : FM1_MOLETTES, plus bas. FM1_AUCUNE = rien.
//
//  Affectation choisie le 12/10/2026 : la croix et A / B / SELECT sur les
//  touches du clavier, START sur PLAY.
//  OCT- et OCT+ tenus ensemble 5 s entrent toujours en mode UBOOT (secours) :
//  c'est le firmware qui s'en occupe, quelle que soit cette table.
// ============================================================================
#pragma once

// Identifiants des controles (matrice du FM-1 : 0..13 boutons, 14..40 touches)
#define FM1_B_OCTDN 0
#define FM1_B_OCTUP 1
#define FM1_B_FX    2
#define FM1_B_SCL   3
#define FM1_B_ENV   4
#define FM1_B_LFO   5
#define FM1_B_EDIT  6
#define FM1_B_GLO   7
#define FM1_B_HOME  8
#define FM1_B_SAVE  9
#define FM1_B_ARP   10
#define FM1_B_SEQ   11
#define FM1_B_PLAY  12
#define FM1_B_REC   13
#define FM1_TOUCHE(n) (14 + (n))
#define FM1_AUCUNE  255

// Touche DS <- controle FM-1. Ajouter des lignes pour doubler une touche.
// Les touches du clavier, a partir du fa grave (n 0) :
//   0 fa  1 fa# (OP1)  2 sol  3 sol# (OP2)  4 la  5 la# (OP3)  6 si  7 do ...
//   22 re# (POLY)  23 mi  24 fa  25 fa# (sans nom)  26 sol
static const struct { unsigned ds; unsigned char fm1; } FM1_TOUCHES[] = {
  { KEY_UP,     FM1_TOUCHE(1)  },   // OP1
  { KEY_LEFT,   FM1_TOUCHE(0)  },   // le fa, tout a gauche
  { KEY_DOWN,   FM1_TOUCHE(2)  },   // le sol
  { KEY_RIGHT,  FM1_TOUCHE(4)  },   // le la
  { KEY_A,      FM1_TOUCHE(25) },   // le diese tout a droite, apres POLY
  { KEY_B,      FM1_TOUCHE(24) },   // le fa juste en dessous, un peu a gauche
  { KEY_B,      FM1_TOUCHE(23) },   // ... et le mi tout a droite, juste a sa gauche
  { KEY_SELECT, FM1_TOUCHE(6)  },   // SELECT a gauche : le si, juste a droite de la touche Droite
  { KEY_SELECT, FM1_TOUCHE(7)  },   // ... et le do, juste a sa droite
  { KEY_SELECT, FM1_TOUCHE(21) },   // SELECT a droite : le re, juste a gauche des B
  { KEY_START,  FM1_B_PLAY     },   // PLAY
  { KEY_START,  FM1_TOUCHE(22) },   // ... et le re# (POLY), juste en haut a gauche du SELECT de droite
  { KEY_L,      FM1_AUCUNE     },
  { KEY_R,      FM1_AUCUNE     },
  { KEY_X,      FM1_AUCUNE     },
  { KEY_Y,      FM1_AUCUNE     },
};

// Les molettes : un cran dans le sens horaire donne `plus`, dans l'autre sens
// `moins`. Sans `avec`, c'est une repetition de la touche (comme la croix
// tenue) ; avec `avec`, c'est un ACCORD : `avec` et la direction enfonces
// ensemble le temps d'une image, puis relaches (aucune ne s'en sert pour
// l'instant). `avec` = FM1_TEMPO : la molette regle le tempo, un cran = le BPM
// atteignable suivant. 0 partout = rien ici : les molettes qui reglent
// l'instrument passent par fm1_plateforme.c (fm1_fx_pas).
#define FM1_TEMPO 0x80000000u
// Ordre : SELECT, ALGORITHM, PRESETS, KNOB 1, KNOB 2, KNOB 3, KNOB 4.
enum { FM1_M_SELECT, FM1_M_ALGORITHM, FM1_M_PRESETS, FM1_M_K1, FM1_M_K2, FM1_M_K3, FM1_M_K4, FM1_NB_MOLETTES };
static const struct { unsigned moins, plus, avec; } FM1_MOLETTES[FM1_NB_MOLETTES] = {
  { 0,         0,         FM1_TEMPO  },   // SELECT    : le tempo (BPM), sur toutes les pages
  { 0,         0,         0          },   // ALGORITHM : la forme d'onde de l'OP2 (fm1_fx_pas 5, main.cpp)
  { 0,         0,         0          },   // PRESETS   : celle de l'OP1 (fm1_fx_pas 4) ; en mode KEYS, la croix gauche / droite
  { 0,         0,         0          },   // KNOB 1    : OUT du modulateur (fm1_fx_pas 0)
  { 0,         0,         0          },   // KNOB 2    : OUT de la porteuse (1)
  { 0,         0,         0          },   // KNOB 3    : FEEDBACK (2)
  { 0,         0,         0          },   // KNOB 4    : MULT du modulateur (3)
};
