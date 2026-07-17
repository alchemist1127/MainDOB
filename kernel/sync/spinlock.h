#ifndef MAINDOB_SYNC_SPINLOCK_H
#define MAINDOB_SYNC_SPINLOCK_H

#include "lib/types.h"
#include "sync/atomic.h"
#include "arch/x86/cpu.h"

/* Spinlock 1.1. In build UP (default fino al milestone 1.1.3) la mutua
 * esclusione e' IF=0: il campo lock resta scritto per ispezione e per
 * il rilevamento lockup, senza LOCK-prefix. La build SMP (MAINDOB_SMP)
 * aggiunge l'xchg atomico. La API e' identica: il codice cliente non
 * cambia quando l'SMP si accende. */

typedef struct
{
    volatile uint32_t lock;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

#define SPINLOCK_LOCKUP_SPINS (1u << 28)
void spinlock_lockup_report(volatile void *lock, void *ret_addr)
    __attribute__((noreturn));

static inline void spinlock_init(spinlock_t *sl)
{
    sl->lock = 0;
}

static inline void spinlock_acquire(spinlock_t *sl)
{
#ifdef MAINDOB_SMP
    uint32_t spins = 0;
    while (atomic_xchg(&sl->lock, 1) != 0)
    {
        __asm__ volatile ("pause");
        if (unlikely(++spins > SPINLOCK_LOCKUP_SPINS))
        {
            spinlock_lockup_report(&sl->lock, __builtin_return_address(0));
        }
    }
#else
    sl->lock = 1;
#endif
}

static inline void spinlock_release(spinlock_t *sl)
{
    compiler_barrier();
    sl->lock = 0;
}

/* Tentativo non bloccante: true se il lock e' stato preso. Per i
 * cancelli opportunistici (es. auto-pulizia dall'idle) dove "qualcun
 * altro ci sta gia' pensando" e' un esito buono quanto il successo:
 * mai girare a vuoto per un lavoro che non richiede il chiamante. */
static inline bool spinlock_try_acquire(spinlock_t *sl)
{
#ifdef MAINDOB_SMP
    return atomic_xchg(&sl->lock, 1) == 0;
#else
    if (sl->lock != 0)
    {
        return false;
    }
    sl->lock = 1;
    return true;
#endif
}

static inline uint32_t spinlock_acquire_irqsave(spinlock_t *sl)
{
    uint32_t flags = irq_save();
    spinlock_acquire(sl);
    return flags;
}

static inline void spinlock_release_irqrestore(spinlock_t *sl, uint32_t flags)
{
    spinlock_release(sl);
    irq_restore(flags);
}

#endif
