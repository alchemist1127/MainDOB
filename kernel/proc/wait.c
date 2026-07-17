#include "proc/wait.h"
#include "proc/thread.h"
#include "proc/scheduler.h"
#include "time/clock.h"

/* Wait queue intrusive (link in thread_t.wq_next): zero allocazioni.
 * Un thread sta in al massimo UNA wait queue. */

/* === Verbi (chiamare con wq->lock preso) ================================= */

static void enqueue_tail(wait_queue_t *wq, thread_t *t)
{
    t->wq_next    = NULL;
    t->blocked_on = wq;

    if (wq->tail != NULL)
    {
        wq->tail->wq_next = t;
    }
    else
    {
        wq->head = t;
    }
    wq->tail = t;
}

static thread_t *dequeue_head(wait_queue_t *wq)
{
    thread_t *t = wq->head;
    if (t == NULL)
    {
        return NULL;
    }

    wq->head = t->wq_next;
    if (wq->head == NULL)
    {
        wq->tail = NULL;
    }
    t->wq_next    = NULL;
    t->blocked_on = NULL;
    return t;
}

/* Sgancia `target` da wq se presente (chiamare con wq->lock preso). */
static void unlink_locked(wait_queue_t *wq, thread_t *target)
{
    thread_t *prev = NULL;
    for (thread_t *cur = wq->head; cur != NULL; cur = cur->wq_next)
    {
        if (cur == target)
        {
            if (prev != NULL)
            {
                prev->wq_next = cur->wq_next;
            }
            else
            {
                wq->head = cur->wq_next;
            }
            if (wq->tail == cur)
            {
                wq->tail = prev;
            }
            cur->wq_next    = NULL;
            cur->blocked_on = NULL;
            return;
        }
        prev = cur;
    }
}

/* === API ================================================================== */

void wait_queue_init(wait_queue_t *wq)
{
    spinlock_init(&wq->lock);
    wq->head = NULL;
    wq->tail = NULL;
}

thread_t *wait_queue_extract_one(wait_queue_t *wq)
{
    uint32_t fl = spinlock_acquire_irqsave(&wq->lock);
    thread_t *t = dequeue_head(wq);
    spinlock_release_irqrestore(&wq->lock, fl);

    if (t != NULL)
    {
        t->wait_result = WAIT_OK;
    }
    return t;
}

bool wait_queue_wake_one(wait_queue_t *wq)
{
    uint32_t fl = spinlock_acquire_irqsave(&wq->lock);
    thread_t *t = dequeue_head(wq);
    spinlock_release_irqrestore(&wq->lock, fl);

    if (t == NULL)
    {
        return false;
    }
    t->wait_result = WAIT_OK;
    scheduler_unblock(t);
    return true;
}

void wait_queue_wake_all(wait_queue_t *wq)
{
    /* Stacca l'intera catena in O(1) sotto lock, svegliala fuori. */
    uint32_t fl = spinlock_acquire_irqsave(&wq->lock);
    thread_t *chain = wq->head;
    wq->head = NULL;
    wq->tail = NULL;
    for (thread_t *t = chain; t != NULL; t = t->wq_next)
    {
        t->blocked_on = NULL;
    }
    spinlock_release_irqrestore(&wq->lock, fl);

    while (chain != NULL)
    {
        thread_t *next = chain->wq_next;
        chain->wq_next     = NULL;
        chain->wait_result = WAIT_OK;
        scheduler_unblock(chain);
        chain = next;
    }
}

void wait_queue_remove_thread(wait_queue_t *wq, thread_t *target)
{
    if (wq == NULL || target == NULL)
    {
        return;
    }

    uint32_t fl = spinlock_acquire_irqsave(&wq->lock);
    unlink_locked(wq, target);
    spinlock_release_irqrestore(&wq->lock, fl);
}

bool wait_queue_empty(wait_queue_t *wq)
{
    return wq->head == NULL;
}

void wait_prepare(wait_queue_t *wq, uint32_t timeout_ms)
{
    thread_t *t = current_thread;

    uint32_t fl = spinlock_acquire_irqsave(&wq->lock);
    enqueue_tail(wq, t);
    t->wait_result = WAIT_OK;
    t->state       = THREAD_BLOCKED;
    if (timeout_ms > 0 &&
        !timer_arm_in(&t->sleep_timer, MS_TO_NS(timeout_ms)))
    {
        /* Heap timer saturo: impossibile GARANTIRE il timeout. Fail-closed:
         * sgancia e NON bloccare all'infinito — il thread torna eseguibile
         * con esito WAIT_TIMEOUT, cosi' il chiamante prende subito il suo
         * ramo di timeout (spesso l'ultima barriera contro un componente
         * guasto) invece di dormire per sempre. scheduler_yield su uno stato
         * RUNNING e' volontario: non blocca. */
        unlink_locked(wq, t);
        t->state       = THREAD_RUNNING;
        t->wait_result = WAIT_TIMEOUT;
        spinlock_release_irqrestore(&wq->lock, fl);
        timer_note_arm_saturation();
        return;
    }
    spinlock_release_irqrestore(&wq->lock, fl);
}

void wait_cancel(void)
{
    thread_t *t = current_thread;

    timer_cancel(&t->sleep_timer);
    if (t->blocked_on != NULL)
    {
        wait_queue_remove_thread(t->blocked_on, t);
    }

    uint32_t fl = irq_save();
    if (t->state == THREAD_BLOCKED)
    {
        t->state = THREAD_RUNNING;      /* mai svegliato: annulla pulito  */
        irq_restore(fl);
        return;
    }
    irq_restore(fl);

    /* Un waker ci ha battuto: siamo gia' READY in coda. Consuma
     * l'enqueue attraverso lo scheduler. */
    scheduler_yield();
}

int wait_finish(void)
{
    thread_t *t = current_thread;
    timer_cancel(&t->sleep_timer);
    return t->wait_result;
}
