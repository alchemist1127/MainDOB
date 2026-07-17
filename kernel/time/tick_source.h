#ifndef MAINDOB_TIME_TICK_SOURCE_H
#define MAINDOB_TIME_TICK_SOURCE_H

/* Tick source — astrae la sorgente hardware delle scadenze del modo
 * eventi (time/event.c). Due backend:
 *
 *   1. LAPIC one-shot / TSC-deadline (preferito): timer per-CPU, grana
 *      ~10 ns, nessun giro di controller sull'hot path. Scelto quando
 *      il LAPIC c'e' ed il suo timer e' utilizzabile.
 *
 *   2. PIT one-shot, 8254 canale 0 in mode 0 (fallback): grana ~838 ns
 *      (cristallo 1.193182 MHz), orizzonte massimo di UN armamento
 *      ~54.9 ms (contatore a 16 bit). Scelto quando il LAPIC manca —
 *      Mobile Celeron con APIC fuso (Armada E500), classe 486,
 *      emulatori senza APIC. Il tetto dei 54.9 ms e' gestito con il
 *      RIARMO IMPILATO dentro l'ISR: il chiamante arma scadenze
 *      arbitrarie e non vede alcuna differenza di comportamento.
 *
 * La selezione avviene UNA volta, in time_event_try_enable; da li' in
 * poi il motore eventi dispatcha attraverso il puntatore alla vtable
 * senza mai piu' ramificare sull'hardware. Entrambi i backend esigono
 * il TSC come monotono (gate a monte, in event.c): senza TSC il kernel
 * resta sul PIT periodico e questo modulo non entra in gioco.
 *
 * I backend vivono nei rispettivi file hardware (lapic.c, pit.c) ed
 * esportano una struct const; qui c'e' solo la superficie di dispatch.
 */

#include "lib/types.h"

typedef void (*tick_source_cb_t)(void);

struct tick_source
{
    /* Prepara il backend a fare da sorgente eventi. Per il LAPIC e' un
     * no-op (lapic_init e' gia' corso in stage_smp_online); per il PIT
     * sostituisce l'ISR periodico con quello one-shot e parcheggia il
     * contatore. Da chiamare UNA volta, prima di register_callback. */
    void (*install)(void);

    /* Programma un IRQ alla scadenza assoluta (ns sul monotono TSC).
     * Scadenze gia' passate sparano il prima possibile. */
    void (*arm_deadline_ns)(uint64_t deadline_ns);

    /* Nessun fuoco fino al prossimo arm. Idempotente. */
    void (*disarm)(void);

    /* Installa la callback che l'ISR invoca a scadenza reale raggiunta
     * (per il PIT: i fuochi intermedi del riarmo impilato NON la
     * invocano). */
    void (*register_callback)(tick_source_cb_t cb);

    /* Etichetta per i log di boot. */
    const char *name;

    /* Oltre questa distanza un singolo armamento viene troncato dal
     * backend (il riarmo impilato copre il resto). Informativo. */
    uint64_t max_arm_delta_ns;
};

/* Backend esportati dai file hardware. */
extern const struct tick_source tick_source_lapic;
extern const struct tick_source tick_source_pit_oneshot;

/* Sceglie il backend in base al SOLO hardware (LAPIC usabile o meno).
 * Non ha effetti collaterali: l'attivazione (install + callback) resta
 * al chiamante. Non restituisce mai NULL: il PIT c'e' sempre. */
const struct tick_source *tick_source_select(void);

#endif /* MAINDOB_TIME_TICK_SOURCE_H */
