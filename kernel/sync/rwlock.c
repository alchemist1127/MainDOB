#include "sync/rwlock.h"
#include "proc/scheduler.h"

void rwlock_init(rwlock_t *rw)
{
    spinlock_init(&rw->guard);
    rw->readers         = 0;
    rw->writers_waiting = 0;
    wait_queue_init(&rw->read_waiters);
    wait_queue_init(&rw->write_waiters);
}

void rwlock_read_lock(rwlock_t *rw)
{
    for (;;)
    {
        uint32_t fl = spinlock_acquire_irqsave(&rw->guard);
        if (rw->readers >= 0 && rw->writers_waiting == 0)
        {
            rw->readers++;
            spinlock_release_irqrestore(&rw->guard, fl);
            return;
        }
        wait_prepare(&rw->read_waiters, 0);
        spinlock_release_irqrestore(&rw->guard, fl);
        scheduler_yield();
        wait_finish();
    }
}

void rwlock_read_unlock(rwlock_t *rw)
{
    uint32_t fl = spinlock_acquire_irqsave(&rw->guard);
    rw->readers--;
    bool wake_writer = (rw->readers == 0 && rw->writers_waiting > 0);
    spinlock_release_irqrestore(&rw->guard, fl);

    if (wake_writer)
    {
        wait_queue_wake_one(&rw->write_waiters);
    }
}

void rwlock_write_lock(rwlock_t *rw)
{
    for (;;)
    {
        uint32_t fl = spinlock_acquire_irqsave(&rw->guard);
        if (rw->readers == 0)
        {
            rw->readers = -1;
            spinlock_release_irqrestore(&rw->guard, fl);
            return;
        }
        rw->writers_waiting++;
        wait_prepare(&rw->write_waiters, 0);
        spinlock_release_irqrestore(&rw->guard, fl);
        scheduler_yield();
        wait_finish();

        fl = spinlock_acquire_irqsave(&rw->guard);
        rw->writers_waiting--;          /* conteggio esatto, sempre       */
        spinlock_release_irqrestore(&rw->guard, fl);
    }
}

void rwlock_write_unlock(rwlock_t *rw)
{
    uint32_t fl = spinlock_acquire_irqsave(&rw->guard);
    rw->readers = 0;
    bool writer_next = (rw->writers_waiting > 0);
    spinlock_release_irqrestore(&rw->guard, fl);

    if (writer_next)
    {
        wait_queue_wake_one(&rw->write_waiters);
    }
    else
    {
        wait_queue_wake_all(&rw->read_waiters);
    }
}
