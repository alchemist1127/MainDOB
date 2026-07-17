#include "mm/dma_pool.h"
#include "mm/pmm.h"
#include "sync/spinlock.h"
#include "lib/string.h"
#include "console/console.h"

/* Pool DMA — vedi il contratto in dma_pool.h.
 *
 * 4 MB (1024 pagine) presi con PMM_ZONE_PREFER_LOW: a boot il bitmap
 * PMM e' vergine, quindi la riserva cade sotto i 16 MB dove serve anche
 * ai controller legacy. Dentro il pool, bitmap a pagine e ricerca
 * first-fit: 1024 bit = 32 parole, la scansione e' spiccia e sotto un
 * lock proprio (mai annidato con quello del PMM: il PMM si tocca solo
 * a init, a pool ancora invisibile). */

#define DMA_POOL_PAGES  1024u                   /* 4 MB                 */
#define DMA_POOL_WORDS  (DMA_POOL_PAGES / 32u)

static uint32_t   s_base_phys;                  /* 0 = pool spento      */
static uint32_t   s_bitmap[DMA_POOL_WORDS];     /* bit=1 -> in uso      */
static uint32_t   s_free;
static spinlock_t s_lock = SPINLOCK_INIT;

/* === Verbi bitmap (chiamante tiene s_lock) ============================== */

static inline bool page_used(uint32_t idx)
{
    return (s_bitmap[idx / 32u] >> (idx % 32u)) & 1u;
}

static inline void page_set(uint32_t idx)
{
    s_bitmap[idx / 32u] |= 1u << (idx % 32u);
}

static inline void page_clear(uint32_t idx)
{
    s_bitmap[idx / 32u] &= ~(1u << (idx % 32u));
}

/* BEST-fit di `n` pagine contigue nel pool; DMA_POOL_PAGES se assenti.
 * Anti-residuo: il first-fit spezza sistematicamente i run grandi in
 * testa al pool e la frammentazione interna cresce col churn dei
 * driver; il best-fit consuma i buchi che gia' combaciano e conserva i
 * run grandi per chi ne avra' bisogno. Su 1024 bit la scansione intera
 * costa quanto quella parziale: stessa spesa, stato migliore. */
static uint32_t find_run(uint32_t n)
{
    uint32_t best = DMA_POOL_PAGES, best_len = DMA_POOL_PAGES + 1u;
    uint32_t run = 0;
    for (uint32_t i = 0; i <= DMA_POOL_PAGES; i++)
    {
        if (i < DMA_POOL_PAGES && !page_used(i))
        {
            run++;
            continue;
        }
        if (run >= n && run < best_len)     /* buco chiuso: candidato   */
        {
            best_len = run;
            best     = i - run;
            if (run == n)
            {
                break;                      /* combacia esatto: perfetto */
            }
        }
        run = 0;
    }
    return best;
}

/* === API ================================================================ */

void dma_pool_init(void)
{
    s_base_phys = pmm_alloc_contiguous(DMA_POOL_PAGES, PMM_ZONE_PREFER_LOW);
    if (s_base_phys == 0)
    {
        kprintf("[DMAP] Riserva non disponibile: buffer DMA dal PMM "
                "diretto.\n");
        return;
    }
    memset(s_bitmap, 0, sizeof(s_bitmap));
    s_free = DMA_POOL_PAGES;
    kprintf("[DMAP] Riserva contigua pronta: %u KB @ 0x%08x.\n",
            DMA_POOL_PAGES * (PAGE_SIZE / 1024u), s_base_phys);
}

uint32_t dma_pool_alloc(uint32_t pages)
{
    if (s_base_phys == 0 || pages == 0 || pages > DMA_POOL_PAGES)
    {
        return 0;
    }

    uint32_t fl = spinlock_acquire_irqsave(&s_lock);
    uint32_t start = find_run(pages);
    if (start == DMA_POOL_PAGES)
    {
        spinlock_release_irqrestore(&s_lock, fl);
        return 0;                       /* pieno: il chiamante ripiega  */
    }
    for (uint32_t i = 0; i < pages; i++)
    {
        page_set(start + i);
    }
    s_free -= pages;
    spinlock_release_irqrestore(&s_lock, fl);

    return s_base_phys + start * PAGE_SIZE;
}

bool dma_pool_free(uint32_t phys, uint32_t pages)
{
    if (s_base_phys == 0 ||
        phys <  s_base_phys ||
        phys + pages * PAGE_SIZE > s_base_phys + DMA_POOL_PAGES * PAGE_SIZE)
    {
        return false;                   /* non nostro: al PMM           */
    }

    uint32_t start = (phys - s_base_phys) / PAGE_SIZE;

    uint32_t fl = spinlock_acquire_irqsave(&s_lock);
    for (uint32_t i = 0; i < pages; i++)
    {
        /* Regola D4 come nel PMM: un double-free nel pool si vede, si
         * conta implicitamente (bit gia' zero) e non si propaga. */
        if (page_used(start + i))
        {
            page_clear(start + i);
            s_free++;
        }
        else
        {
            kprintf("[DMAP] DOUBLE-FREE pagina %u - ignorato\n", start + i);
        }
    }
    spinlock_release_irqrestore(&s_lock, fl);
    return true;
}

uint32_t dma_pool_free_pages(void)
{
    return (s_base_phys != 0) ? s_free : 0;
}
