// ============================================================================
//  Le langage entre les deux processeurs pour piloter les voies de son.
//
//  Les registres des seize voies matérielles n'appartiennent qu'à l'ARM7.
//  L'ARM9, qui fait tourner le séquenceur, lui envoie donc ses ordres par le
//  FIFO, en paquets de commandes de seize octets. Ce fichier est inclus des
//  deux côtés : c'est le seul contrat entre eux.
//
//  Copyright (C) 2026 Audren Thibault. Tous droits réservés.
// ============================================================================
#pragma once
#include <nds.h>
#include <stdint.h>

#define XT_FIFO_VOIX         FIFO_USER_01
// Le port cartouche de la DSi (voir arm7/main.c, portSlot1).
#define XT_FIFO_SLOT1        FIFO_USER_02
#define XT_SLOT1_ETAT        1u   // lire seulement
#define XT_SLOT1_RECYCLE     2u   // couper puis rendre le courant
#define XT_VOIX_PAR_MESSAGE  8          // 8 x 16 = 128 octets par message
#define FIFO_MAX_DATA_BYTES_XT (XT_VOIX_PAR_MESSAGE * 16)

enum {
  XT_VOIX_JOUE = 1,   // démarre un échantillon
  XT_VOIX_FREQ = 2,   // change la hauteur
  XT_VOIX_VOL  = 3,   // change le volume et la position stéréo
  XT_VOIX_STOP = 4,   // coupe la voie
};

// Longueur, en MOTS de 32 bits comme le veut la puce, et deux drapeaux.
#define XT_VOIX_16BITS  0x80000000u
#define XT_VOIX_BOUCLE  0x40000000u
#define XT_VOIX_MOTS    0x003FFFFFu

typedef struct {
  uint8_t  op;
  uint8_t  voie;       // 0-15
  uint8_t  vol;        // 0-127
  uint8_t  pan;        // 0 gauche, 64 centre, 127 droite
  uint16_t timer;      // valeur du registre SOUNDxTMR
  uint16_t pnt;        // point de bouclage, en mots
  uint32_t adresse;    // début des données, en mémoire principale
  uint32_t longueur;   // mots après le point de bouclage, + drapeaux
} xt_voix_cmd_t;

#ifdef __cplusplus
static_assert(sizeof(xt_voix_cmd_t) == 16, "xt_voix_cmd_t doit faire 16 octets");
#else
_Static_assert(sizeof(xt_voix_cmd_t) == 16, "xt_voix_cmd_t doit faire 16 octets");
#endif
