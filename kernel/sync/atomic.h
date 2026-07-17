#ifndef MAINDOB_SYNC_ATOMIC_H
#define MAINDOB_SYNC_ATOMIC_H

#include "lib/types.h"

/* Primitive atomiche su volatile uint32_t (i686, ordinamento x86 TSO).
 * Componente standard: spinlock, refcount, TLB shootdown e contatori
 * per-CPU usano SOLO queste — niente `lock` inline sparsi nei moduli.
 *
 * Su x86 il prefisso LOCK e' barriera piena; per letture/scritture
 * semplici basta volatile + barriera del compilatore, il TSO fa il
 * resto. */

/* Impedisce al compilatore di riordinare attraverso questo punto.
 * (Nessuna istruzione emessa: barriera di sola compilazione.) */
static inline void compiler_barrier(void)
{
    __asm__ volatile ("" ::: "memory");
}

/* Barriera hardware piena (store visibili prima di proseguire). Usata
 * dove il TSO da solo non basta: pubblicazione cross-CPU prima di un
 * IPI (es. TLB shootdown). `lock addl $0,(esp)` e' la barriera piena
 * canonica sui P6 senza MFENCE (Armada E500 non ha SSE2). */
static inline void memory_barrier(void)
{
    __asm__ volatile ("lock addl $0, (%%esp)" ::: "memory", "cc");
}

static inline uint32_t atomic_read(volatile uint32_t *p)
{
    return *p;
}

static inline void atomic_set(volatile uint32_t *p, uint32_t val)
{
    *p = val;
}

static inline void atomic_inc(volatile uint32_t *p)
{
    __asm__ volatile ("lock incl %0" : "+m"(*p) :: "memory");
}

static inline void atomic_dec(volatile uint32_t *p)
{
    __asm__ volatile ("lock decl %0" : "+m"(*p) :: "memory");
}

/* Decrementa; true se il risultato e' zero (per i refcount: l'ultimo
 * che molla libera). Usa il flag ZF di `lock decl` direttamente. */
static inline bool atomic_dec_and_test(volatile uint32_t *p)
{
    uint8_t zero;
    __asm__ volatile ("lock decl %0; setz %1"
                      : "+m"(*p), "=q"(zero)
                      :
                      : "memory", "cc");
    return zero != 0;
}

/* Somma `val` e ritorna il NUOVO valore (lock xadd + somma locale). */
static inline uint32_t atomic_add_return(volatile uint32_t *p, uint32_t val)
{
    uint32_t old = val;
    __asm__ volatile ("lock xaddl %0, %1"
                      : "+r"(old), "+m"(*p)
                      :
                      : "memory", "cc");
    return old + val;
}

/* Scambio atomico: scrive `val`, ritorna il valore precedente.
 * (xchg su memoria e' implicitamente LOCK su x86.) */
static inline uint32_t atomic_xchg(volatile uint32_t *p, uint32_t val)
{
    __asm__ volatile ("xchgl %0, %1"
                      : "+r"(val), "+m"(*p)
                      :
                      : "memory");
    return val;
}

/* Compare-and-swap: se *p == expected scrive desired e ritorna true. */
static inline bool atomic_cas(volatile uint32_t *p, uint32_t expected,
                              uint32_t desired)
{
    uint8_t ok;
    __asm__ volatile ("lock cmpxchgl %3, %1; setz %2"
                      : "+a"(expected), "+m"(*p), "=q"(ok)
                      : "r"(desired)
                      : "memory", "cc");
    return ok != 0;
}

#endif /* MAINDOB_SYNC_ATOMIC_H */
