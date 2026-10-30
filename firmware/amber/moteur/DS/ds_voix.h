// ============================================================================
//  Les seize voies de son de la DS, vues depuis l'ARM9.
//
//  Remplace la couche « puce » de GeneTrackerDS : il n'y a plus de YM2612 ni
//  de SN76489 à émuler, les échantillons sont joués par le matériel lui-même.
//  C'est ce qui rend le son gratuit en calcul, y compris sur une DS Lite.
//
//  Les ordres sont accumulés puis partent vers l'ARM7 par ds_voix_envoie(),
//  à la fin de chaque tick. Une valeur identique à la précédente n'est pas
//  renvoyée : le moteur réécrit hauteur et volume à chaque ligne.
//
//  Copyright (C) 2026 Audren Thibault. Tous droits réservés.
// ============================================================================
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS_NB_VOIES 16

void ds_voix_init(void);

// Démarre un échantillon SIGNÉ de `bits` 8 ou 16, long de `images`. `boucle`
// est le début de boucle en images (-1 = joué une fois). `hz` est la cadence
// de lecture voulue. `vol` 0-127, `pan` 0 gauche / 64 centre / 127 droite.
// Les données doivent commencer sur une frontière de 4 octets.
void ds_voix_joue(int voie, const uint8_t *donnees, uint32_t images, int bits,
                  int32_t boucle, uint32_t hz, int vol, int pan);
void ds_voix_freq(int voie, uint32_t hz);
// RG350 : la disto de la voie, 0-255 (0 = aucune), comme le tracker GameCube.
void ds_voix_disto(int voie, int disto);
void ds_voix_vol(int voie, int vol, int pan);
void ds_voix_stop(int voie);
bool ds_voix_joue_encore(int voie);   // démarrée et pas arrêtée par nous

void ds_voix_envoie(void);

// ── Le retard des échantillons ──────────────────────────────────────────────
// Le MIDI arrive en retard : il traverse l'USB, puis le synthétiseur de l'iPad
// et son tampon audio. Les voies de la DS, elles, partent tout de suite. Pour
// que les deux tombent ensemble, ce sont les ÉCHANTILLONS qui attendent — on
// ne peut pas envoyer le MIDI avant que le séquenceur ait lu la ligne.
// Une voie lit-elle encore quelque part dans cette zone de mémoire ? La puce
// ne le dit pas à l'ARM9 : la réponse se CALCULE, avec la durée de
// l'échantillon et l'horloge à la milliseconde. C'est ce qui permet de
// refabriquer un son sans couper celui qui sonne.
bool ds_voix_lit_zone(const void *debut, uint32_t octets);

void ds_voix_regle_retard(int ms);     // 0 = immédiat
void ds_voix_retard_actif(bool actif); // hors lecture, une audition reste immédiate
void ds_voix_purge(void);              // tout part maintenant (arrêt de la lecture)

#ifdef __cplusplus
}
#endif
