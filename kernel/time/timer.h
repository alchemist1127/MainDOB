#ifndef MAINDOB_TIME_TIMER_H
#define MAINDOB_TIME_TIMER_H

/* Timer — min-heap di scadenze assolute, drenato dal tick PIT 1000 Hz
 * (timer_on_tick dall'ISR IRQ0). Heap globale unico con lock irqsave;
 * la migrazione per-CPU (D2) non cambiera' l'API. Risoluzione 1 ms;
 * sotto il ms si usa tsc_busy_wait_ns.
 *
 * `gen` invalida i fire drenati di un armo precedente (riuso dello
 * stesso timer per una nuova attesa); `fire_pending` segnala una
 * callback in volo, e chi libera la memoria che contiene il timer deve
 * attendere che torni 0.
 *
 * Callback: contesto IRQ, IF=0, lock timer non tenuto, switch differiti
 * al punto post-EOI. Possono svegliare thread e (ri)armare timer; non
 * devono bloccare ne' prendere lock non-irqsave. */

#include "lib/types.h"
#include "time/units.h"

typedef void (*timer_fn_t)(void *arg);

typedef struct timer
{
    uint64_t   deadline_ns;     /* scadenza assoluta (ns monotoni); 0 = non armato */
    uint64_t   period_ns;       /* != 0: al fire si riarma a deadline += period
                                 * (cadenza originale, drift-free)              */
    timer_fn_t fn;
    void      *arg;             /* di proprieta' del chiamante                  */

    int32_t    heap_idx;        /* slot nell'heap, -1 se fuori (cancel O(log n))*/
    uint32_t   gen;             /* generazione anti-stantio (vedi sopra)        */
    volatile int32_t fire_pending; /* callback in volo (vedi sopra)             */
} timer_t;

/* Init del sottosistema (heap vuoto). Prima di pit_init. Idempotente. */
void timer_subsystem_init(void);

/* Setup una-tantum del timer. Non arma. */
/* Prepara `t` (fn/arg, back-index a riposo). CONTRATTO: mai su un
 * timer potenzialmente ARMATO — per i pool l'idioma e' init UNA volta
 * a vita dello slot e poi solo arm/cancel (il ri-armo di un timer vivo
 * e' gestito da arm: detach verificato + gen++). La ruota si difende
 * comunque dai back-index incoerenti, ma il contratto resta. */
void timer_init(timer_t *t, timer_fn_t fn, void *arg);

/* Arma `t` a `delay_ns` da adesso. Se gia' armato, il vecchio armo e'
 * sostituito. false se l'heap e' pieno. */
bool timer_arm_in(timer_t *t, uint64_t delay_ns);

/* Arma `t` alla scadenza assoluta (ns monotoni). Scadenza nel passato:
 * fuoco al prossimo tick ("il prima possibile"). */
bool timer_arm_at(timer_t *t, uint64_t deadline_ns);

/* Arma `t` periodico ogni `period_ns`, primo fuoco tra `period_ns`. */
bool timer_arm_periodic(timer_t *t, uint64_t period_ns);

/* Cancella. Dopo il ritorno la callback non fara' effetto (gen bumpata:
 * un fire gia' drenato viene scartato). Chi libera la memoria che
 * contiene `t` attende fire_pending == 0 (vedi sopra). No-op su timer
 * mai armato o gia' scattato. */
void timer_cancel(timer_t *t);

/* true finche' una callback drenata per `t` e' in volo. */
static inline bool timer_fire_in_flight(const timer_t *t)
{
    return t->fire_pending > 0;
}

/* Chiamata dall'ISR del PIT a ogni tick (IF=0). Percorso vuoto: un
 * lock + un confronto. Con scadenze mature: drain con lock mollato
 * attorno a ogni callback, incorniciato da
 * scheduler_timer_drain_begin/end (deferral degli switch). */
void timer_on_tick(void);

/* Nota rate-limited: un timer di attesa non e' stato armabile (heap saturo)
 * e l'attesa e' stata degradata a timeout immediato invece che a hang.
 * Chiamata dai percorsi di attesa temporizzata (wait, futex). */
void timer_note_arm_saturation(void);

/* Prima scadenza in agenda (UINT64_MAX se vuota) — per il nucleo
 * eventi (time/event.c). */
uint64_t timer_next_deadline_ns(void);

#endif /* MAINDOB_TIME_TIMER_H */
