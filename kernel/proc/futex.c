#include "proc/futex.h"
#include "proc/thread.h"
#include "proc/process.h"
#include "proc/wait.h"
#include "proc/scheduler.h"
#include "krt/uaccess.h"
#include "sync/spinlock.h"
#include "time/clock.h"

/* Futex process-private: chiave = (address space, uaddr). Bucket =
 * wait_queue standard. Invariante anti-lost-wake: leggi *uaddr sotto il
 * lock del bucket PRIMA di accodarti.
 *
 * VINCOLO D5: uaccess_get_u32 non fa mai fault-and-sleep (mapping
 * stabili in 1.1). Introdurre demand paging richiede di ridisegnare
 * QUESTA lettura prima. */

#define FUTEX_BUCKETS 128u

static wait_queue_t s_buckets[FUTEX_BUCKETS];

void futex_init(void)
{
    for (uint32_t i = 0; i < FUTEX_BUCKETS; i++)
    {
        wait_queue_init(&s_buckets[i]);
    }
}

static wait_queue_t *bucket_for(process_t *as, uint32_t uaddr)
{
    uint32_t h = (uint32_t)(uintptr_t)as ^ uaddr ^ (uaddr >> 13);
    h *= 2654435761u;                           /* mix di Knuth           */
    return &s_buckets[h & (FUTEX_BUCKETS - 1u)];
}

int futex_wait(uint32_t uaddr, uint32_t val, uint32_t timeout_ms)
{
    if (uaddr & 3u)
    {
        return FUTEX_EFAULT;
    }

    thread_t     *self = current_thread;
    process_t    *as   = self->owner;
    wait_queue_t *b    = bucket_for(as, uaddr);

    uint32_t fl = spinlock_acquire_irqsave(&b->lock);

    uint32_t cur;
    if (!uaccess_get_u32(uaddr, &cur))
    {
        spinlock_release_irqrestore(&b->lock, fl);
        return FUTEX_EFAULT;
    }
    if (cur != val)
    {
        spinlock_release_irqrestore(&b->lock, fl);
        return FUTEX_EAGAIN;
    }

    /* Accoda a mano dentro il lock del bucket (stessa disciplina delle
     * wait queue), poi rilascia e dormi. */
    self->wq_next     = NULL;
    self->blocked_on  = b;
    if (b->tail != NULL)
    {
        b->tail->wq_next = self;
    }
    else
    {
        b->head = self;
    }
    b->tail = self;

    self->wait_result = WAIT_OK;
    self->state       = THREAD_BLOCKED;
    if (timeout_ms > 0 &&
        !timer_arm_in(&self->sleep_timer, MS_TO_NS(timeout_ms)))
    {
        /* Heap timer saturo: fail-closed. Sgancia self dal bucket (siamo
         * gia' sotto b->lock) e ritorna timeout invece di dormire per
         * sempre. */
        thread_t *prev = NULL;
        for (thread_t *cur = b->head; cur != NULL; cur = cur->wq_next)
        {
            if (cur == self)
            {
                if (prev != NULL) { prev->wq_next = cur->wq_next; }
                else              { b->head       = cur->wq_next; }
                if (b->tail == cur) { b->tail = prev; }
                cur->wq_next    = NULL;
                cur->blocked_on = NULL;
                break;
            }
            prev = cur;
        }
        self->state = THREAD_RUNNING;
        spinlock_release_irqrestore(&b->lock, fl);
        timer_note_arm_saturation();
        return FUTEX_ETIMEDOUT;
    }
    spinlock_release_irqrestore(&b->lock, fl);

    scheduler_yield();
    timer_cancel(&self->sleep_timer);

    return (self->wait_result == WAIT_TIMEOUT) ? FUTEX_ETIMEDOUT : FUTEX_OK;
}

int futex_wake(uint32_t uaddr, uint32_t count)
{
    if (uaddr & 3u)
    {
        return 0;
    }

    process_t    *as = current_thread->owner;
    wait_queue_t *b  = bucket_for(as, uaddr);

    thread_t *woken_head = NULL;
    thread_t *woken_tail = NULL;
    uint32_t  woken = 0;

    uint32_t fl = spinlock_acquire_irqsave(&b->lock);
    thread_t *prev = NULL;
    thread_t *cur  = b->head;
    while (cur != NULL && woken < count)
    {
        thread_t *next = cur->wq_next;
        if (cur->owner == as)
        {
            if (prev != NULL)
            {
                prev->wq_next = next;
            }
            else
            {
                b->head = next;
            }
            if (b->tail == cur)
            {
                b->tail = prev;
            }
            cur->blocked_on = NULL;
            cur->wq_next    = NULL;
            if (woken_tail != NULL)
            {
                woken_tail->wq_next = cur;
            }
            else
            {
                woken_head = cur;
            }
            woken_tail = cur;
            woken++;
        }
        else
        {
            prev = cur;
        }
        cur = next;
    }
    spinlock_release_irqrestore(&b->lock, fl);

    for (thread_t *t = woken_head; t != NULL; )
    {
        thread_t *n = t->wq_next;
        t->wq_next = NULL;
        scheduler_unblock(t);
        t = n;
    }
    return (int)woken;
}
