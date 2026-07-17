#ifndef MAINDOB_TIME_EVENT_H
#define MAINDOB_TIME_EVENT_H

#include "lib/types.h"

/* Nucleo eventi per-CPU (tickless).
 *
 * Nessuna frequenza di polling: ogni CPU arma il proprio timer LAPIC
 * one-shot (o TSC-deadline) sulla PROSSIMA scadenza che la riguarda —
 * il minimo tra la testa dell'heap timer globale e la fine del quanto
 * del thread in esecuzione. Con niente in agenda la CPU dorme in hlt
 * finche' un evento (IRQ, IPI, scadenza) non la sveglia.
 *
 * Attivazione: time_event_try_enable() (dopo lapic_init) accende il
 * modo eventi se LAPIC timer e TSC sono usabili e maschera il tick
 * periodico del PIT. Senza i prerequisiti (hardware d'epoca degradato)
 * il PIT a 1000 Hz resta il fallback, dietro le stesse API.
 *
 * Rinfresco: chiunque muove la prima scadenza (armo/cancel di timer,
 * dispatch di un thread) chiama time_event_refresh() sulla PROPRIA
 * CPU. La CPU che inserisce la scadenza piu' vicina e' quella che si
 * arma per essa: un fire che al drain non trova nulla di maturo e' un
 * no-op economico che si riarma. */

/* true se il modo eventi e' attivo (deciso una volta al boot). */
bool time_event_mode(void);

/* Prova ad attivare il modo eventi sulla BSP. Chiamare dopo
 * lapic_init() e tsc_init(). Logga l'esito; senza prerequisiti resta
 * il PIT periodico. */
void time_event_try_enable(void);

/* Riarma il timer locale sulla prossima scadenza rilevante per questa
 * CPU. No-op se il modo eventi e' spento. Chiamabile con IF=0. */
void time_event_refresh(void);

#endif /* MAINDOB_TIME_EVENT_H */
