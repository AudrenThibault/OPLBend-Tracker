// ============================================================================
//  Le MIDI par l'USB : cartouche DS Linker Writer (port GBA) sur DS et
//  DS Lite, DSpico (port DS) sur DSi. Voir xt_dcd.c.
//
//  Les messages sont mis en FILE, depuis n'importe où — y compris sous
//  l'interruption du séquenceur — puis vidés vers l'USB par xt_midi_irq() à
//  chaque tick, ou par xt_midi_service() depuis la boucle principale quand la
//  lecture est arrêtée (l'iPad a besoin qu'on lui réponde pour nous reconnaître).
//
//  Copyright (C) 2026 Audren Thibault. Tous droits réservés.
// ============================================================================
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// À appeler une fois au démarrage, APRÈS fatInitDefault() : sur DSi, le
// réveil de la DSpico lit sa clé sur la carte SD. Faux si la cartouche ne
// répond pas.
bool xt_midi_init(void);

bool xt_midi_present(void);    // la cartouche USB répond
bool xt_midi_connecte(void);   // un hôte (l'iPad) nous a reconnus
uint32_t xt_midi_perdus(void); // messages jetés faute de place

// Un message MIDI. Program change et channel pressure n'ont que deux octets :
// d2 est alors ignoré.
void xt_midi_send(uint8_t status, uint8_t d1, uint8_t d2);

void xt_midi_service(void);    // boucle principale
void xt_midi_irq(void);        // interruption du séquenceur

#ifdef __cplusplus
}
#endif
