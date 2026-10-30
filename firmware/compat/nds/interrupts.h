#pragma once
// « Couper les interruptions » = prendre le verrou du sequenceur (recursif).
#ifdef __cplusplus
extern "C" {
#endif
enum { IRQ_TIMER0 = 1 << 3 };
int enterCriticalSection(void);
void leaveCriticalSection(int ancien);
void irqSet(int irq, void (*rappel)(void));
void irqEnable(int irq);
#ifdef __cplusplus
}
#endif
