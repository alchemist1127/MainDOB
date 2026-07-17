#ifndef MAINDOB_PROC_WAIT_H
#define MAINDOB_PROC_WAIT_H

#include "lib/types.h"
#include "sync/spinlock.h"

#define WAIT_OK       0
#define WAIT_TIMEOUT  1
#define WAIT_CANCELLED 2

struct thread;

typedef struct
{
    spinlock_t     lock;
    struct thread *head;
    struct thread *tail;
} wait_queue_t;

void wait_queue_init(wait_queue_t *wq);
bool wait_queue_wake_one(wait_queue_t *wq);
/* Stacca il primo thread SENZA svegliarlo (resta BLOCKED, fuori da ogni
 * coda): il chiamante decide — direct-switch se e' sulla propria CPU,
 * scheduler_unblock altrimenti. NULL se vuota. */
struct thread *wait_queue_extract_one(wait_queue_t *wq);
void wait_queue_wake_all(wait_queue_t *wq);
void wait_queue_remove_thread(wait_queue_t *wq, struct thread *target);
bool wait_queue_empty(wait_queue_t *wq);

/* Pattern anti-lost-wakeup (ereditato dall'1.0, dov'era corretto):
 *
 *     wait_prepare(wq, tmo);       // enqueue + BLOCKED, sotto wq->lock
 *     if (predicato_vero())        // re-check DOPO essere visibili
 *         wait_cancel();           //   l'evento e' arrivato: non dormire
 *     else { scheduler_yield(); }
 *     risultato = wait_finish();
 */
void wait_prepare(wait_queue_t *wq, uint32_t timeout_ms);
void wait_cancel(void);
int  wait_finish(void);

#endif
