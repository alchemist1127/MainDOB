#ifndef MAINDOB_MM_DMA_POOL_H
#define MAINDOB_MM_DMA_POOL_H

#include "lib/types.h"

/* Riserva fisica CONTIGUA per i buffer DMA, presa UNA volta a boot —
 * quando il bitmap del PMM e' vergine e la contiguita' e' garantita.
 *
 * Il problema che risolve: pmm_alloc_contiguous su un sistema acceso
 * da settimane puo' fallire per frammentazione fisica anche con molta
 * RAM libera — e un driver che si riavvia (o un hotplug USB) non
 * troverebbe piu' i frame contigui per i propri ring/descrittori.
 * Con la riserva, la contiguita' e' una proprieta' del boot, non una
 * speranza sul churn.
 *
 * Contratto:
 *   - dma_pool_alloc: prova nel pool; 0 se il pool manca o e' pieno —
 *     il chiamante ripiega su pmm_alloc_contiguous (routing nel livello
 *     logico, non qui).
 *   - dma_pool_free: true se il range era del pool (e lo restituisce);
 *     false se estraneo — il chiamante lo rende al PMM. Cosi' i due
 *     percorsi di allocazione convivono senza che il free debba sapere
 *     da dove veniva il buffer.
 *   - Se la riserva a boot fallisce (macchine minuscole) il pool resta
 *     spento e ogni verbo degrada al comportamento pre-pool. */

void     dma_pool_init(void);
uint32_t dma_pool_alloc(uint32_t pages);            /* -> phys o 0      */
bool     dma_pool_free(uint32_t phys, uint32_t pages);

/* Diagnostica (task manager / log): pagine libere nel pool, 0 se off. */
uint32_t dma_pool_free_pages(void);

#endif /* MAINDOB_MM_DMA_POOL_H */
