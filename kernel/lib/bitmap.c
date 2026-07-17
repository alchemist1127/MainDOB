#include "lib/bitmap.h"
#include "lib/string.h"

#define BITS_PER_ENTRY 32u

void bitmap_init(bitmap_t *bm, uint32_t *buffer, uint32_t num_bits)
{
    bm->data = buffer;
    bm->size = num_bits;
    uint32_t bytes = ((num_bits + BITS_PER_ENTRY - 1) / BITS_PER_ENTRY)
                   * sizeof(uint32_t);
    memset(buffer, 0, bytes);
}

void bitmap_set(bitmap_t *bm, uint32_t index)
{
    if (index < bm->size)
    {
        bm->data[index / BITS_PER_ENTRY] |= (1U << (index % BITS_PER_ENTRY));
    }
}

void bitmap_clear(bitmap_t *bm, uint32_t index)
{
    if (index < bm->size)
    {
        bm->data[index / BITS_PER_ENTRY] &= ~(1U << (index % BITS_PER_ENTRY));
    }
}

bool bitmap_test(bitmap_t *bm, uint32_t index)
{
    if (index >= bm->size)
    {
        return true;    /* fuori range = occupato (contratto 1.0) */
    }
    return (bm->data[index / BITS_PER_ENTRY]
            & (1U << (index % BITS_PER_ENTRY))) != 0;
}

uint32_t bitmap_find_first_zero(bitmap_t *bm)
{
    uint32_t entries = (bm->size + BITS_PER_ENTRY - 1) / BITS_PER_ENTRY;

    for (uint32_t i = 0; i < entries; i++)
    {
        if (bm->data[i] == 0xFFFFFFFFu)
        {
            continue;                       /* parola piena, salto */
        }

        /* ~word ha un 1 dove la bitmap ha uno 0: ctz (una BSF) trova
         * il primo in tempo costante — l'1.0 iterava fino a 32 bit. */
        uint32_t bit = (uint32_t)__builtin_ctz(~bm->data[i]);
        uint32_t index = i * BITS_PER_ENTRY + bit;

        return (index < bm->size) ? index : UINT32_MAX;
    }
    return UINT32_MAX;
}

uint32_t bitmap_find_contiguous_zeros(bitmap_t *bm, uint32_t count)
{
    if (count == 0)
    {
        return UINT32_MAX;
    }
    if (count == 1)
    {
        return bitmap_find_first_zero(bm);
    }

    uint32_t run_start = 0;
    uint32_t run_len = 0;

    for (uint32_t i = 0; i < bm->size; i++)
    {
        if (!bitmap_test(bm, i))
        {
            if (run_len == 0)
            {
                run_start = i;
            }
            run_len++;
            if (run_len == count)
            {
                return run_start;
            }
        }
        else
        {
            run_len = 0;
        }
    }
    return UINT32_MAX;
}

void bitmap_set_range(bitmap_t *bm, uint32_t start, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        bitmap_set(bm, start + i);
    }
}

void bitmap_clear_range(bitmap_t *bm, uint32_t start, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        bitmap_clear(bm, start + i);
    }
}
