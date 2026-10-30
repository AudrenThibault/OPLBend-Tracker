// Les samples et la sortie audio : sur le FM-1, voir fm1_voix.c.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void rg_son_demarre(void);
const char *rg_son_sortie(void);   // "OSS", "ALSA" ou "AUCUNE"
// Retard a appliquer a l'OPL3, en ms (le reglage SYNC quand il est negatif,
// pendant la lecture seulement).
int rg_son_retard_opl(void);
// Pendant la lecture, chaque sortie part a l'heure PREVUE du tick plus cette
// latence fixe (secondes) : le rythme ne depend plus des aleas du systeme.
// Le noyau de la RG350 ne reveille un fil que 2 a 8 ms apres l'heure demandee
// (pas de minuteries haute resolution) : la latence couvre ce retard.
static const double RG_LATENCE_TICK = 0.025;
extern double rg_ecart_opl_max;           // mesure : envoi OPL3 - echeance
bool rg_son_lecture(void);
void rg_son_stop(int voie);        // arrete le sample d'une voie
void rg_son_clic(int voie, int retardMs);   // un clic court, retarde de retardMs
// Test de synchro : un clic OPL3 et un clic sample « en meme temps », chacun
// retarde comme le veut le reglage SYNC (ms, signe).
void rg_test_synchro(int syncMs);
void opl_voix_purge(void);         // rg_voix_opl.cpp : vide le retard OPL3
#ifdef __cplusplus
}
#endif
