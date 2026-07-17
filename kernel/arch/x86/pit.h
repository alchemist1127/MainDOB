#ifndef MAINDOB_ARCH_PIT_H
#define MAINDOB_ARCH_PIT_H

#include "lib/types.h"

/* PIT 8254, tre ruoli (nessun tick periodico sulle macchine con TSC):
 *   - finestra di CALIBRAZIONE sincrona (mode 0, polling readback,
 *     zero IRQ): pit_calibration_window, usata da tsc_init e
 *     dall'attesa di boot del fallback;
 *   - one-shot (mode 0): backend tick_source del modo eventi sulle
 *     macchine senza LAPIC — tick_source_pit_oneshot, raggiungibile
 *     solo via time/tick_source.h;
 *   - periodico a PIT_HZ (mode 3): ESTREMO fallback quando il TSC non
 *     e' calibrabile (deciso una volta in stage_time_online); solo in
 *     quel caso pit_ticks/pit_uptime_ms avanzano e fanno da monotono
 *     (vedi clock.c). */

#define PIT_HZ        1000u
#define PIT_INPUT_HZ  1193182u   /* cristallo 8254: base di calibrazione */

void     pit_init(void);
uint64_t pit_ticks(void);
uint64_t pit_uptime_ms(void);

/* Finestra sincrona mode 0: attende delay_ticks (max 65535 ~ 54.9 ms)
 * a colpi di readback e ritorna il delta TSC misurato sui fronti. */
uint64_t pit_calibration_window(uint16_t delay_ticks);

#endif
