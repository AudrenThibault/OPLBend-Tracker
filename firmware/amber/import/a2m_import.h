// Import d'un module AdLib Tracker 2 (.A2M v14) : voir a2m_import.cpp.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Remplace le morceau en cours par le module. `msg` recoit un compte rendu
// court, affichable (ou la raison de l'echec).
bool md_importe_a2m(const char *chemin, char *msg, int cap);

// Le bloc « songdata » du dernier A2M importe (NULL s'il n'y en a pas) : il
// sert de base a l'enregistrement, pour ne rien perdre de ce que le tracker ne
// represente pas. A oublier pour un morceau neuf.
const uint8_t *a2m_songdata_origine(void);
void a2m_oublie_origine(void);

// Enregistrement (a2m_export.cpp) : NOM.A2M, et NOM.OPS si le morceau contient
// ce qu'AT2 ne connait pas. A l'ouverture, md_charge_ops relit le .OPS s'il
// correspond toujours au .A2M.
bool md_enregistre_a2m(const char *cheminA2M, char *msg, int cap);
bool md_charge_ops(const char *cheminA2M, char *msg, int cap);

#ifdef __cplusplus
}
#endif
