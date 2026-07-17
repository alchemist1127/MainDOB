#ifndef MAINDOB_KRT_ENTROPY_H
#define MAINDOB_KRT_ENTROPY_H

#include "lib/types.h"

/* Pool di entropia standard (SYS_RANDOM + futuri usi kernel: ASLR,
 * cookie). Sorgenti: TSC jitter campionato a ogni tick del PIT +
 * eventi di boot. NON crittograficamente forte in senso stretto:
 * xorshift128 rimescolato di continuo; documentato come tale. */

void     entropy_init(void);
void     entropy_add(uint32_t sample);          /* cheap, IRQ-safe        */
void     entropy_get_bytes(void *buf, uint32_t len);

#endif
