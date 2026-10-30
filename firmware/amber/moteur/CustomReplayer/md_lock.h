#ifndef MD_LOCK_H
#define MD_LOCK_H

// ── Le verrou du moteur, selon la machine ───────────────────────────────────
// Sur iOS l'interface et le fil audio touchent le même état en même temps, et
// il faut un vrai verrou.
//
// ⚠️ SUR DS, LE SÉQUENCEUR TOURNE MAINTENANT SOUS INTERRUPTION. Dans
// GeneTrackerDS il tournait dans la boucle principale et le verrou ne faisait
// rien. Ici c'est le timer du séquenceur qui l'appelle : tout ce que la boucle
// principale modifie en même temps doit donc couper les interruptions.
//
// Le verrou COMPTE ses prises. Certaines fonctions verrouillées en appellent
// d'autres qui verrouillent aussi ; sans le compte, la prise intérieure
// écraserait l'état sauvegardé par l'extérieure, et les interruptions
// resteraient coupées pour de bon — le tracker se figerait.
//
// Sous l'interruption elle-même, elles sont déjà coupées : md_trylock répond
// donc toujours oui.

#if defined(__APPLE__) && !defined(ARM9)
  #include <os/lock.h>
  typedef os_unfair_lock md_lock_t;
  #define MD_LOCK_INIT      OS_UNFAIR_LOCK_INIT
  #define md_lock(l)        os_unfair_lock_lock(l)
  #define md_unlock(l)      os_unfair_lock_unlock(l)
  #define md_trylock(l)     os_unfair_lock_trylock(l)
#else
  #include <stdbool.h>
  #include <nds/ndstypes.h>
  #include <nds/interrupts.h>
  typedef struct { int profondeur; int ime; } md_lock_t;
  #define MD_LOCK_INIT      { 0, 0 }
  static inline void md_lock(md_lock_t *l) {
    const int ime = enterCriticalSection();
    if (l->profondeur++ == 0) l->ime = ime;
  }
  static inline void md_unlock(md_lock_t *l) {
    if (l->profondeur > 0 && --l->profondeur == 0) leaveCriticalSection(l->ime);
  }
  #define md_trylock(l)     (true)
#endif

#endif
