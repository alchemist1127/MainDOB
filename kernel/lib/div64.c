#include "lib/types.h"

/* Helper di divisione a 64 bit per i386 (li fornirebbe libgcc, ma il
 * kernel linka -nostdlib). Fast path quando il divisore sta in 32 bit
 * (due divl hardware); ripiego bit-a-bit per divisori a 64 bit veri,
 * che nel kernel non compaiono su percorsi caldi. */

static uint64_t udiv64(uint64_t n, uint64_t d, uint64_t *rem)
{
    if (d == 0)
    {
        /* Comportamento definito localmente: evita il #DE, il chiamante
         * kernel non divide mai per zero se non per bug. */
        if (rem != NULL)
        {
            *rem = 0;
        }
        return 0xFFFFFFFFFFFFFFFFull;
    }

    if ((d >> 32) == 0)
    {
        /* Divisore a 32 bit: long division in due passi con divl. */
        uint32_t d32 = (uint32_t)d;
        uint32_t n_hi = (uint32_t)(n >> 32);
        uint32_t n_lo = (uint32_t)n;

        uint32_t q_hi = n_hi / d32;
        uint32_t r_hi = n_hi % d32;

        uint64_t low = ((uint64_t)r_hi << 32) | n_lo;
        uint32_t q_lo, r_lo;
        __asm__ ("divl %4"
                 : "=a"(q_lo), "=d"(r_lo)
                 : "a"((uint32_t)low), "d"((uint32_t)(low >> 32)), "r"(d32));

        if (rem != NULL)
        {
            *rem = r_lo;
        }
        return ((uint64_t)q_hi << 32) | q_lo;
    }

    /* Divisore pieno a 64 bit: shift-and-subtract. */
    uint64_t q = 0;
    uint64_t r = 0;
    for (int i = 63; i >= 0; i--)
    {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d)
        {
            r -= d;
            q |= 1ull << i;
        }
    }
    if (rem != NULL)
    {
        *rem = r;
    }
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d)
{
    return udiv64(n, d, NULL);
}

uint64_t __umoddi3(uint64_t n, uint64_t d)
{
    uint64_t r;
    udiv64(n, d, &r);
    return r;
}

int64_t __divdi3(int64_t n, int64_t d)
{
    bool neg = (n < 0) != (d < 0);
    uint64_t q = udiv64(n < 0 ? (uint64_t)-n : (uint64_t)n,
                        d < 0 ? (uint64_t)-d : (uint64_t)d, NULL);
    return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t n, int64_t d)
{
    uint64_t r;
    udiv64(n < 0 ? (uint64_t)-n : (uint64_t)n,
           d < 0 ? (uint64_t)-d : (uint64_t)d, &r);
    return (n < 0) ? -(int64_t)r : (int64_t)r;
}
