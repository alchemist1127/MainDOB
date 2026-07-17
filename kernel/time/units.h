#ifndef MAINDOB_TIME_UNITS_H
#define MAINDOB_TIME_UNITS_H

/* Conversioni di unita' tempo. Il kernel lavora in ns uint64; queste
 * convertono ai confini delle API (userspace in ms/us). MS_TO_NS regge
 * 584 anni di uptime prima dell'overflow. */

#include "lib/types.h"

#define NS_PER_US           1000ULL
#define NS_PER_MS           1000000ULL
#define NS_PER_S            1000000000ULL

/* uint64 -> uint64, esatte */
#define US_TO_NS(x)         ((uint64_t)(x) * NS_PER_US)
#define MS_TO_NS(x)         ((uint64_t)(x) * NS_PER_MS)
#define S_TO_NS(x)          ((uint64_t)(x) * NS_PER_S)

/* ns -> ms / us: divisione vera via udiv64_u32 (due divl, esatta su
 * TUTTO il dominio uint64).
 *
 * PERCHE' non la moltiplicazione reciproca (ns * M) >> k: il limite
 * dichiarato di quello schema riguarda la precisione del reciproco e
 * IGNORA l'overflow del PRODOTTO — con M = ceil(2^50/10^6), ns * M
 * eccede 2^64 gia' per ns >= 16'383'999'997: clock_now_ms() (e ogni
 * API in ms, syscall comprese) vivrebbe MODULO ~16384 ms, tornando
 * indietro di 16.384 s ogni 16.4 s di uptime e avvelenando ogni logica
 * temporale (pacing GUI, dwell driver, timeout). La divisione vera
 * costa ~50 cicli contro ~10: e' un confine di API, non un hot loop —
 * la correttezza vince senza discussione. */

static inline uint64_t ns_to_ms(uint64_t ns)
{
    return udiv64_u32(ns, 1000000u);
}

static inline uint64_t ns_to_us(uint64_t ns)
{
    return udiv64_u32(ns, 1000u);
}

#define NS_TO_MS(x)         ns_to_ms(x)
#define NS_TO_US(x)         ns_to_us(x)

#endif /* MAINDOB_TIME_UNITS_H */
