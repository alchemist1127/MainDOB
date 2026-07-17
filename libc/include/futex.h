#ifndef _DOB_FUTEX_H
#define _DOB_FUTEX_H

#include <sys/syscall.h>
#include <stdint.h>

/* Process-private futex. Uncontended lock/unlock never enters the kernel (see
 * the mutex below); only contention calls futex_wait/futex_wake. */

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

/* Block while *uaddr == val, until woken or timeout_ms elapses (0 = forever).
 * Returns 0 woken, -1 *uaddr != val, -2 timeout, -3 bad address. */
static inline int futex_wait(volatile uint32_t *uaddr, uint32_t val, uint32_t timeout_ms)
{
    return syscall4(SYS_FUTEX, FUTEX_WAIT, (int)(uintptr_t)uaddr, (int)val, (int)timeout_ms);
}

/* Wake up to `count` waiters on *uaddr (FIFO order). Returns the number woken. */
static inline int futex_wake(volatile uint32_t *uaddr, uint32_t count)
{
    return syscall3(SYS_FUTEX, FUTEX_WAKE, (int)(uintptr_t)uaddr, (int)count);
}

/* ---- 3-state mutex (Drepper) over the futex ----
 * 0 = unlocked, 1 = locked (no waiters), 2 = locked (possible waiters). */
typedef volatile uint32_t dob_mutex_t;
#define DOB_MUTEX_INIT 0

static inline uint32_t _dm_cas(volatile uint32_t *p, uint32_t expect, uint32_t want)
{
    uint32_t old;
    __asm__ volatile ("lock cmpxchgl %2,%1"
                      : "=a"(old), "+m"(*p) : "r"(want), "0"(expect) : "memory");
    return old;
}
static inline uint32_t _dm_xchg(volatile uint32_t *p, uint32_t want)
{
    uint32_t old;
    __asm__ volatile ("xchgl %0,%1" : "=r"(old), "+m"(*p) : "0"(want) : "memory");
    return old;
}

static inline void dob_mutex_lock(dob_mutex_t *m)
{
    uint32_t c = _dm_cas(m, 0, 1);
    if (c != 0) {
        if (c != 2) c = _dm_xchg(m, 2);
        while (c != 0) {
            futex_wait(m, 2, 0);
            c = _dm_xchg(m, 2);
        }
    }
}

/* Returns 0 if acquired, -1 if already locked. */
static inline int dob_mutex_trylock(dob_mutex_t *m)
{
    return _dm_cas(m, 0, 1) == 0 ? 0 : -1;
}

static inline void dob_mutex_unlock(dob_mutex_t *m)
{
    if (_dm_xchg(m, 0) == 2)     /* had waiters → wake one */
        futex_wake(m, 1);
}

#endif
