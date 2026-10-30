// ============================================================================
//  Les dix-huit voies 2 operateurs de l'OPL3, vues depuis le moteur.
//
//  Le pilotage est celui d'Amber OPL3 Tracker (a2m_replayer.c) : un
//  instrument, ce sont onze octets ecrits dans les registres 20/40/60/80/E0
//  de ses deux operateurs et dans C0 ; une note, c'est un F-Num et un bloc
//  dans A0/B0, avec la table de frequences d'AdLib Tracker 2.
//
//  Les ecritures s'accumulent puis partent vers la RetroWave par
//  ds_voix_envoie(), a la fin de chaque tick, comme les ordres des voies DS.
// ============================================================================
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPL_NB_VOIES 18
// Le mode percussion de la puce : cinq voies de plus, ses sons de batterie (BD, SD, TT, CY, HH : OPL_VOIX_PERC + 0..4),
// sur les voies 7, 8 et 9 de la puce. Charger un instrument sur l'une d'elles met la puce en mode percussion ;
// opl_voix_init l'en sort.
#define OPL_VOIX_PERC 64   // (au-dela des voies des samples, 18 et plus : les arreter ne touche pas la batterie)

void opl_voix_init(void);
// Charge un instrument (onze octets au format A2M) ; `pan` 0-255, 128 centre.
void opl_voix_instrument(int voie, const uint8_t fm[11], int pan);
// Hauteur telle que la puce la veut : (bloc << 10) | F-Num, comme AT2.
// `attaque` relance l'enveloppe (note neuve).
void opl_voix_hauteur(int voie, int hauteur, bool attaque);
// Total Level 0-63 (0 = fort) de la porteuse et du modulateur.
void opl_voix_volume(int voie, int tlPorteuse, int tlModulateur);
void opl_voix_relache(int voie);   // key off : l'enveloppe part en release
// Profondeur du tremolo et du vibrato de toute la puce (registre BD) :
// 0 ou 1, -1 = inchange.
void opl_voix_profondeurs(int tremolo, int vibrato);

#ifdef __cplusplus
}
#endif
