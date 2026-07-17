#ifndef MAINDOB_MM_PMM_H
#define MAINDOB_MM_PMM_H

#include "lib/types.h"

#define PAGE_SIZE           4096u
#define PAGE_SHIFT          12u
#define ADDR_TO_FRAME(a)    ((uint32_t)(a) >> PAGE_SHIFT)
#define FRAME_TO_ADDR(f)    ((uint32_t)(f) << PAGE_SHIFT)

#define PMM_ZONE_ANY        0u
#define PMM_ZONE_DMA        (1u << 0)   /* RICHIEDE < 16 MB (DMA device)   */
#define PMM_ZONE_PREFER_LOW (1u << 1)   /* preferisce < 16 MB, poi ovunque */
#define PMM_DMA_LIMIT       0x01000000u     /* < 16 MB                    */

typedef struct
{
    uint32_t total_frames;
    uint32_t free_frames;
    uint32_t dma_free;
    uint32_t double_free_caught;    /* D4: contatore diagnostico          */
    uint32_t pressure;              /* percentuale frame usati (0..100)   */
} pmm_stats_t;

void     pmm_init(void);
uint32_t pmm_alloc_frame(uint32_t zone_flags);       /* 0 = OOM            */
void     pmm_free_frame(uint32_t phys_addr);
uint32_t pmm_alloc_contiguous(uint32_t count, uint32_t zone_flags);
void     pmm_free_contiguous(uint32_t phys_addr, uint32_t count);
void     pmm_mark_used(uint32_t phys_addr, uint32_t size);
void     pmm_get_stats(pmm_stats_t *out);
bool     pmm_is_frame_allocated(uint32_t phys_addr);
uint32_t pmm_get_ram_top(void);
bool     pmm_selftest(void);

#endif
