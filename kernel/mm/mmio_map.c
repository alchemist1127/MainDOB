#include "mm/mmio_map.h"
#include "mm/kheap.h"
#include "mm/pmm.h"
#include "mm/paging.h"

void *mmio_map(uint64_t phys, uint32_t len, bool cache_disable,
              uint32_t *out_virt, uint32_t *out_pages)
{
    if (phys == 0 || len == 0) return NULL;

    uint32_t phys32   = (uint32_t)phys;
    uint32_t page_off = phys32 & 0xFFFu;
    uint32_t first    = phys32 & ~0xFFFu;
    uint32_t span     = page_off + len;
    uint32_t pages    = (span + 0xFFFu) >> 12;

    /* Riserva VA kernel, poi rilascia i frame RAM che l'allocatore ha
     * agganciato: mappiamo memoria fisica del chiamante (MMIO o
     * tabella), non RAM assegnata dal pool. */
    uint32_t virt = kpages_alloc(pages);
    if (!virt) return NULL;
    for (uint32_t i = 0; i < pages; i++)
    {
        uint32_t vp  = virt + i * PAGE_SIZE;
        uint32_t old = paging_get_physical(vp, NULL);
        paging_unmap_page(vp);
        if (old) pmm_free_frame(old & 0xFFFFF000u);
    }

    uint32_t flags = PTE_PRESENT | PTE_WRITABLE;
    if (cache_disable) flags |= PTE_CACHE_DISABLE;

    for (uint32_t i = 0; i < pages; i++)
    {
        paging_map_page(virt + i * PAGE_SIZE, first + i * PAGE_SIZE, flags);
    }

    *out_virt  = virt;
    *out_pages = pages;
    return (void *)(virt + page_off);
}

void mmio_unmap(uint32_t virt, uint32_t pages)
{
    if (!virt || !pages) return;
    for (uint32_t i = 0; i < pages; i++)
    {
        uint32_t vp = virt + i * PAGE_SIZE;
        paging_unmap_page(vp);
        kpage_free(vp);
    }
}
