#include "sync/semaphore.h"
#include "proc/scheduler.h"

void semaphore_init(semaphore_t *s, int32_t initial)
{
    spinlock_init(&s->guard);
    s->count = initial;
    wait_queue_init(&s->waiters);
}

static int wait_internal(semaphore_t *s, uint32_t timeout_ms)
{
    for (;;)
    {
        uint32_t fl = spinlock_acquire_irqsave(&s->guard);
        if (s->count > 0)
        {
            s->count--;
            spinlock_release_irqrestore(&s->guard, fl);
            return WAIT_OK;
        }

        wait_prepare(&s->waiters, timeout_ms);
        spinlock_release_irqrestore(&s->guard, fl);

        scheduler_yield();
        if (wait_finish() == WAIT_TIMEOUT)
        {
            return WAIT_TIMEOUT;
        }
    }
}

void semaphore_wait(semaphore_t *s)
{
    wait_internal(s, 0);
}

int semaphore_wait_timeout(semaphore_t *s, uint32_t timeout_ms)
{
    return wait_internal(s, timeout_ms);
}

bool semaphore_trywait(semaphore_t *s)
{
    uint32_t fl = spinlock_acquire_irqsave(&s->guard);
    bool taken = (s->count > 0);
    if (taken)
    {
        s->count--;
    }
    spinlock_release_irqrestore(&s->guard, fl);
    return taken;
}

void semaphore_signal(semaphore_t *s)
{
    uint32_t fl = spinlock_acquire_irqsave(&s->guard);
    s->count++;
    spinlock_release_irqrestore(&s->guard, fl);

    wait_queue_wake_one(&s->waiters);
}
