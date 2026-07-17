#include "sync/mutex.h"
#include "proc/thread.h"
#include "proc/scheduler.h"
#include "console/console.h"

/* Mutex con priority inheritance (modello 1.0, che era corretto):
 * la priorita' EFFETTIVA dell'owner e'
 *     min(base_priority, min boost_prio dei mutex posseduti)
 * ricalcolata da stato puro — nessun contatore che puo' derivare.
 * boost_prio e' "sticky" fino all'unlock (mai riabbassato quando un
 * singolo waiter se ne va): al peggio l'owner resta alto un po' piu'
 * a lungo, mai inflazione permanente.
 *
 * D3: superare THREAD_MAX_OWNED_MUTEXES NON e' panic — il mutex extra
 * funziona ma non partecipa a PI/rilascio-orfani; loggato e contato. */

/* === Verbi ============================================================== */

static void recompute_owner_priority(thread_t *owner)
{
    if (owner == NULL)
    {
        return;
    }

    uint32_t eff = owner->base_priority;
    for (uint32_t i = 0; i < owner->owned_mutex_count; i++)
    {
        mutex_t *m = owner->owned_mutexes[i];
        if (m != NULL && m->boost_prio < eff)
        {
            eff = m->boost_prio;
        }
    }
    scheduler_set_priority(owner, eff);
}

static void track_ownership(mutex_t *m, thread_t *t)
{
    m->locked    = 1;
    m->owner_tid = t->tid;
    t->waiting_on_mutex = NULL;

    if (t->owned_mutex_count < THREAD_MAX_OWNED_MUTEXES)
    {
        t->owned_mutexes[t->owned_mutex_count++] = m;
    }
    else
    {
        t->untracked_mutexes++;
        kprintf("[MTX ] TID %d oltre %u mutex tracciati (#%u non in PI)\n",
                t->tid, (uint32_t)THREAD_MAX_OWNED_MUTEXES,
                t->untracked_mutexes);
    }
}

static void untrack_ownership(mutex_t *m, thread_t *t)
{
    for (uint32_t i = 0; i < t->owned_mutex_count; i++)
    {
        if (t->owned_mutexes[i] == m)
        {
            t->owned_mutexes[i] =
                t->owned_mutexes[--t->owned_mutex_count];
            return;
        }
    }
    if (t->untracked_mutexes > 0)
    {
        t->untracked_mutexes--;
    }
}

/* === API ================================================================ */

void mutex_init(mutex_t *m)
{
    spinlock_init(&m->guard);
    m->locked     = 0;
    m->owner_tid  = -1;
    m->boost_prio = UINT32_MAX;
    wait_queue_init(&m->waiters);
}

static int lock_internal(mutex_t *m, uint32_t timeout_ms)
{
    for (;;)
    {
        uint32_t fl = spinlock_acquire_irqsave(&m->guard);

        if (m->locked == 0)
        {
            track_ownership(m, current_thread);
            spinlock_release_irqrestore(&m->guard, fl);
            return WAIT_OK;
        }

        /* Registra il boost sul mutex e FOTOGRAFA l'owner (per TID) sotto
         * il guard. L'applicazione della priorita' avviene FUORI dal guard,
         * risolta per TID: mai un puntatore-thread portato attraverso una
         * finestra di free (niente UAF), e mai una chiamata nello scheduler
         * mentre si tiene m->guard. Il TID dell'owner viene anche
         * REGISTRATO sul waiter (waiting_owner_tid): e' l'anello della
         * catena PI transitiva, che thread_boost_by_tid percorre di solo
         * TID in TID sotto il lifecycle-lock — la catena c'e' di nuovo,
         * ma senza mai dereferenziare mutex o thread altrui. */
        uint32_t req = current_thread->priority;
        if (req < m->boost_prio)
        {
            m->boost_prio = req;
        }
        tid_t owner_tid = m->owner_tid;

        current_thread->waiting_on_mutex  = m;
        current_thread->waiting_owner_tid = owner_tid;
        wait_prepare(&m->waiters, timeout_ms);
        spinlock_release_irqrestore(&m->guard, fl);

        thread_boost_by_tid(owner_tid, req);    /* fuori dal guard, safe */

        scheduler_yield();
        int result = wait_finish();
        current_thread->waiting_on_mutex  = NULL;
        current_thread->waiting_owner_tid = -1;

        if (result == WAIT_TIMEOUT)
        {
            return WAIT_TIMEOUT;
        }
        /* svegliati dall'unlock: rigioca l'acquisizione */
    }
}

void mutex_lock(mutex_t *m)
{
    lock_internal(m, 0);
}

int mutex_lock_timeout(mutex_t *m, uint32_t timeout_ms)
{
    return lock_internal(m, timeout_ms);
}

bool mutex_trylock(mutex_t *m)
{
    uint32_t fl = spinlock_acquire_irqsave(&m->guard);
    bool taken = (m->locked == 0);
    if (taken)
    {
        track_ownership(m, current_thread);
    }
    spinlock_release_irqrestore(&m->guard, fl);
    return taken;
}

void mutex_unlock(mutex_t *m)
{
    uint32_t fl = spinlock_acquire_irqsave(&m->guard);

    thread_t *self = current_thread;
    if (self->tid != m->owner_tid)
    {
        spinlock_release_irqrestore(&m->guard, fl);
        kprintf("[MTX ] unlock da non-owner (TID %d) - ignorato\n",
                self->tid);
        return;
    }

    untrack_ownership(m, self);
    m->locked     = 0;
    m->owner_tid  = -1;
    m->boost_prio = UINT32_MAX;
    spinlock_release_irqrestore(&m->guard, fl);

    /* FUORI dal guard: recompute riguarda self (current_thread), che e'
     * vivo per costruzione -> nessun UAF, e nessuna chiamata nello
     * scheduler mentre si tiene m->guard. */
    recompute_owner_priority(self);
    wait_queue_wake_one(&m->waiters);   /* fuori dal guard: no inversione */
}

void mutex_release_all_owned(struct thread *t)
{
    /* Ordine inverso: il PI si srotola correttamente. */
    for (int i = (int)t->owned_mutex_count - 1; i >= 0; i--)
    {
        mutex_t *m = t->owned_mutexes[i];
        if (m != NULL && m->locked && m->owner_tid == t->tid)
        {
            /* unlock manuale: current_thread NON e' t */
            uint32_t fl = spinlock_acquire_irqsave(&m->guard);
            m->locked     = 0;
            m->owner_tid  = -1;
            m->boost_prio = UINT32_MAX;
            spinlock_release_irqrestore(&m->guard, fl);
            /* wake_ALL, non one: questo e' il rilascio ORFANO del reap —
             * se il mutex vive in un PCB in teardown, un co-waiter non
             * svegliato resterebbe BLOCKED per sempre su una wait queue
             * che sta per essere kfree-ata col processo. Svegliarli
             * tutti: rigiocano l'acquisizione, e chi trova il mutex
             * sparito fallisce pulito. */
            wait_queue_wake_all(&m->waiters);
        }
    }
    t->owned_mutex_count = 0;
}
