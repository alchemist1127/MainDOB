#ifndef MAINDOB_LIB_BITMAP_H
#define MAINDOB_LIB_BITMAP_H

#include "lib/types.h"

/* Bitmap su buffer fornito dal chiamante (zero allocazioni): componente
 * standard per tabelle di slot (SHM, handle, IRQ claim, DMA). API 1:1
 * con l'1.0; la ricerca dello zero usa __builtin_ctz invece del loop
 * bit-a-bit (una BSF al posto di fino a 32 iterazioni). */

typedef struct
{
    uint32_t *data;         /* array di uint32_t coi bit                */
    uint32_t  size;         /* numero totale di bit                     */
} bitmap_t;

/* Inizializza: tutti i bit a 0. */
void bitmap_init(bitmap_t *bm, uint32_t *buffer, uint32_t num_bits);

/* Operazioni su singolo bit (index fuori range: set/clear no-op,
 * test ritorna true = "occupato", come nell'1.0). */
void bitmap_set(bitmap_t *bm, uint32_t index);
void bitmap_clear(bitmap_t *bm, uint32_t index);
bool bitmap_test(bitmap_t *bm, uint32_t index);

/* Primo bit a zero. Ritorna l'indice, o UINT32_MAX se tutto occupato. */
uint32_t bitmap_find_first_zero(bitmap_t *bm);

/* N bit a zero consecutivi. Indice del primo, o UINT32_MAX. */
uint32_t bitmap_find_contiguous_zeros(bitmap_t *bm, uint32_t count);

/* Operazioni su intervallo. */
void bitmap_set_range(bitmap_t *bm, uint32_t start, uint32_t count);
void bitmap_clear_range(bitmap_t *bm, uint32_t start, uint32_t count);

#endif /* MAINDOB_LIB_BITMAP_H */
