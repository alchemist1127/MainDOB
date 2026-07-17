#ifndef MAINDOB_LIB_TYPES_H
#define MAINDOB_LIB_TYPES_H

/* Tipi base del kernel 1.1. Semantica IDENTICA al lib/types.h dell'1.0:
 * ogni struct condivisa con l'userspace (isr_regs_t, buffer syscall)
 * dipende da queste larghezze, quindi qui non si "migliora", si
 * conserva. Le uniche aggiunte sono commentate come tali. */

/* Interi a larghezza fissa (ILP32, i686) */
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;

typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;

/* Dimensioni e indirizzi */
typedef uint32_t    size_t;
typedef int32_t     ssize_t;
typedef uint32_t    uintptr_t;
typedef int32_t     intptr_t;
typedef int32_t     ptrdiff_t;

/* Processi e risorse (ABI: pid/tid/handle sono int32 con l'userspace) */
typedef int32_t     pid_t;
typedef int32_t     tid_t;
typedef int32_t     handle_t;

/* Booleano */
typedef enum { false = 0, true = 1 } bool;

/* Null */
#define NULL ((void *)0)

/* Limiti */
#define UINT8_MAX   0xFF
#define UINT16_MAX  0xFFFF
#define UINT32_MAX  0xFFFFFFFF
#define UINT64_MAX  0xFFFFFFFFFFFFFFFFULL
#define INT32_MIN   (-2147483647 - 1)
#define INT32_MAX   2147483647
#define SIZE_MAX    UINT32_MAX

/* Utilita' strutturali */
#define offsetof(type, member) ((size_t)&((type *)0)->member)
#define container_of(ptr, type, member) \
    ((type *)((uint8_t *)(ptr) - offsetof(type, member)))

/* Allineamento — 'align' DEVE essere potenza di 2 */
#define ALIGN_UP(val, align)   (((val) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(val, align) ((val) & ~((align) - 1))
#define IS_ALIGNED(val, align) (((val) & ((align) - 1)) == 0)

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Attributi compilatore */
#define PACKED        __attribute__((packed))
#define NORETURN      __attribute__((noreturn))
#define UNUSED        __attribute__((unused))
#define ALIGNED(n)    __attribute__((aligned(n)))
#define HOT           __attribute__((hot))
#define COLD          __attribute__((cold))
#define ALWAYS_INLINE __attribute__((always_inline))

/* Hint di branch: alimentano il layout del codice di GCC — il ramo
 * previsto resta in fall-through, il freddo va lontano dalla I-cache.
 * Costo runtime zero: annotazione puramente compile-time.
 *
 *   if (likely(caso_comune))   { ... }
 *   if (unlikely(caso_errore)) { return; }
 */
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* Sezione boot (gira pre-paging) */
#define BOOT_CODE   __attribute__((section(".boot")))
#define BOOT_DATA   __attribute__((section(".boot.data")))

/* === Aritmetica 64-bit ===
 *
 * Il kernel linka con -nostdlib: __udivdi3/__umoddi3 di libgcc non ci
 * sono. Questi wrapper usano `divl` x86 (64/32 -> 32) direttamente.
 *
 *   udiv64_u32 — uint64 / uint32 -> quoziente uint64 (esatto)
 *   umod64_u32 — uint64 % uint32 -> resto uint32
 *
 * Algoritmo: dividendo spezzato in (hi, lo). Primo divl: q_hi, r_hi =
 * hi/d, hi%d. Secondo divl su (r_hi:lo): q_lo, resto. Quoziente =
 * (q_hi << 32) | q_lo. Il chiamante garantisce divisore != 0 — nessun
 * check runtime (invarianti interne: violarle e' un bug, vedi D3). */
static inline uint64_t udiv64_u32(uint64_t dividend, uint32_t divisor)
{
    uint32_t hi = (uint32_t)(dividend >> 32);
    uint32_t lo = (uint32_t)dividend;
    uint32_t q_hi, r_hi;

    /* Passo 1: hi/divisor. `divl` legge il dividendo da EDX:EAX;
     * hi viene esteso a zero fornendo EDX=0 esplicitamente. */
    __asm__ ("divl %2"
             : "=a"(q_hi), "=d"(r_hi)
             : "rm"(divisor), "a"(hi), "d"(0U)
             : "cc");

    /* Passo 2: (r_hi:lo)/divisor. r_hi < divisor per costruzione
     * (resto di una divisione per lo stesso divisore): divl non
     * puo' andare in overflow. */
    uint32_t q_lo, r_lo;
    __asm__ ("divl %2"
             : "=a"(q_lo), "=d"(r_lo)
             : "rm"(divisor), "a"(lo), "d"(r_hi)
             : "cc");
    (void)r_lo;

    return ((uint64_t)q_hi << 32) | q_lo;
}

static inline uint32_t umod64_u32(uint64_t dividend, uint32_t divisor)
{
    uint32_t hi = (uint32_t)(dividend >> 32);
    uint32_t lo = (uint32_t)dividend;
    uint32_t q_hi, r_hi;

    __asm__ ("divl %2"
             : "=a"(q_hi), "=d"(r_hi)
             : "rm"(divisor), "a"(hi), "d"(0U)
             : "cc");

    uint32_t q_lo, r_lo;
    __asm__ ("divl %2"
             : "=a"(q_lo), "=d"(r_lo)
             : "rm"(divisor), "a"(lo), "d"(r_hi)
             : "cc");
    (void)q_hi;
    (void)q_lo;

    return r_lo;
}

#endif /* MAINDOB_LIB_TYPES_H */
