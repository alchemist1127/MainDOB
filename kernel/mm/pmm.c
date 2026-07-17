#include "mm/pmm.h"
#include "mm/paging.h"
#include "boot/boot_info.h"
#include "lib/string.h"
#include "console/console.h"
#include "arch/x86/cpu.h"
#include "sync/spinlock.h"
#include "kernel.h"

/* PMM 1.1 — bitmap a due zone (DMA <16 MB / NORMAL), hint di ricerca.
 *
 * Regola D4: OGNI free verifica lo stato del frame; un double-free
 * viene loggato, contato e IGNORATO — mai propagato.
 * Mutua esclusione: un unico spinlock irqsave (le AP allocano frame
 * dal momento in cui eseguono thread schedulati). */

#define MAX_FRAMES  (1024u * 1024u)         /* 4 GB / 4 KB                */
#define BM_WORDS    (MAX_FRAMES / 32u)

static uint32_t s_bitmap[BM_WORDS];         /* bit=1 -> frame usato       */
static uint32_t s_total_frames;             /* estensione GREZZA della RAM:
                                             * confine RAM/MMIO + bound del
                                             * bitmap (pmm_get_ram_top,
                                             * mmap_phys dei driver)        */
static uint32_t s_alloc_ceiling_frame;      /* tetto ALLOCABILE = min(RAM,
                                             * direct-map): l'allocatore non
                                             * distribuisce mai oltre, cosi'
                                             * ogni frame e' raggiungibile a
                                             * KERNEL_VMA+F                  */
static uint32_t s_dma_limit_frame;
static uint32_t s_free_frames;
static uint32_t s_dma_free;
static uint32_t s_hint_dma;
static uint32_t s_hint_normal;
static uint32_t s_double_free_caught;
static spinlock_t s_lock = SPINLOCK_INIT;

extern uint32_t _kernel_phys_end;           /* dal linker script          */

/* === Verbi bitmap ======================================================= */

static inline bool frame_is_used(uint32_t f)
{
    return (s_bitmap[f / 32u] >> (f % 32u)) & 1u;
}

static inline void frame_set_used(uint32_t f)
{
    s_bitmap[f / 32u] |= 1u << (f % 32u);
    s_free_frames--;
    if (f < s_dma_limit_frame)
    {
        s_dma_free--;
    }
}

static inline void frame_set_free(uint32_t f)
{
    s_bitmap[f / 32u] &= ~(1u << (f % 32u));
    s_free_frames++;
    if (f < s_dma_limit_frame)
    {
        s_dma_free++;
        if (f < s_hint_dma)
        {
            s_hint_dma = f;
        }
    }
    else if (f < s_hint_normal)
    {
        s_hint_normal = f;
    }
}

/* Primo frame libero in [start, end), partendo dall'hint e wrappando.
 * Scansione a parole con BSF; ogni bit libero della parola viene
 * verificato contro i limiti (nessun bit valido viene saltato). */
static uint32_t zone_find_free(uint32_t start, uint32_t end, uint32_t hint)
{
    uint32_t from = (hint >= start && hint < end) ? hint : start;

    for (int pass = 0; pass < 2; pass++)
    {
        uint32_t lo = (pass == 0) ? from : start;
        uint32_t hi = (pass == 0) ? end  : from;

        for (uint32_t w = lo / 32u; w < (hi + 31u) / 32u; w++)
        {
            uint32_t candidates = ~s_bitmap[w];
            while (candidates != 0)
            {
                uint32_t bit = (uint32_t)__builtin_ctz(candidates);
                uint32_t f   = w * 32u + bit;
                if (f >= start && f < end)
                {
                    return f;
                }
                candidates &= candidates - 1u;      /* prossimo bit       */
            }
        }
    }
    return UINT32_MAX;
}

static uint32_t zone_find_contiguous(uint32_t start, uint32_t end, uint32_t n)
{
    uint32_t run   = 0;
    uint32_t begin = start;

    for (uint32_t f = start; f < end; f++)
    {
        if (frame_is_used(f))
        {
            run = 0;
            begin = f + 1;
        }
        else if (++run == n)
        {
            return begin;
        }
    }
    return UINT32_MAX;
}

/* === Verbi di inizializzazione ========================================== */

/* Verbo esecutivo: top fisico GREZZO della RAM usabile, dalla memory map
 * di boot. Solo entry AVAILABLE: le regioni riservate (ECAM PCIe, flash
 * BIOS, finestre LAPIC) stanno in alto e gonfierebbero il top facendo
 * scambiare i BAR MMIO per RAM (lezione del 1.0). Nessuna decisione: misura. */
static uint32_t mmap_usable_top(void)
{
    uint32_t max_addr = 0;

    for (uint32_t i = 0; i < g_boot_info.mmap_count; i++)
    {
        const boot_mmap_entry_t *e = &g_boot_info.mmap[i];

        if (e->type != 1u || e->base > 0xFFFFFFFFull)
        {
            continue;
        }

        uint32_t end = (uint32_t)(e->base + e->length);
        if (end > max_addr)
        {
            max_addr = end;
        }
    }

    return max_addr;
}

/* Logica: dai due fatti — RAM grezza presente e tetto del direct-map
 * (di proprieta' di paging) — deriva i DUE limiti che fino a ieri erano
 * conflati in s_total_frames, e decide ESPLICITAMENTE cosa fare della RAM
 * oltre il direct-map (mai troncamento silenzioso). */
static void measure_usable_ram(void)
{
    uint32_t raw_top = mmap_usable_top();

    s_total_frames = ADDR_TO_FRAME(raw_top);
    if (s_total_frames > MAX_FRAMES)            /* bound del bitmap statico */
    {
        s_total_frames = MAX_FRAMES;
    }

    s_dma_limit_frame = ADDR_TO_FRAME(PMM_DMA_LIMIT);
    if (s_dma_limit_frame > s_total_frames)
    {
        s_dma_limit_frame = s_total_frames;
    }

    /* Tetto allocabile: mai oltre il direct-map kernel. */
    uint32_t dm_top_frame = ADDR_TO_FRAME(paging_direct_map_limit());
    s_alloc_ceiling_frame = (s_total_frames < dm_top_frame)
                          ? s_total_frames : dm_top_frame;

    /* RAM oltre il direct-map: decisione esplicita, non troncamento muto. */
    if (s_total_frames > s_alloc_ceiling_frame)
    {
#ifdef MAINDOB_STRICT_RAM_CEILING
        kpanic("PMM: RAM (%u MB) eccede il direct-map kernel (%u MB) - "
               "piattaforma non supportata (build strict)",
               (s_total_frames * PAGE_SIZE) >> 20,
               (s_alloc_ceiling_frame * PAGE_SIZE) >> 20);
#else
        kprintf("[PMM ] ATTENZIONE: RAM %u MB, ma il direct-map ne indirizza "
                "%u MB: %u MB oltre il tetto IGNORATI (non allocabili).\n",
                (s_total_frames * PAGE_SIZE) >> 20,
                (s_alloc_ceiling_frame * PAGE_SIZE) >> 20,
                ((s_total_frames - s_alloc_ceiling_frame) * PAGE_SIZE) >> 20);
#endif
    }
}

static void release_available_regions(void)
{
    memset(s_bitmap, 0xFF, sizeof(s_bitmap));   /* tutto usato di default */
    s_free_frames = 0;
    s_dma_free    = 0;

    for (uint32_t i = 0; i < g_boot_info.mmap_count; i++)
    {
        const boot_mmap_entry_t *e = &g_boot_info.mmap[i];
        if (e->type != 1u || e->base > 0xFFFFFFFFull)
        {
            continue;
        }

        uint32_t fs = ADDR_TO_FRAME(ALIGN_UP((uint32_t)e->base, PAGE_SIZE));
        uint32_t fe = ADDR_TO_FRAME(ALIGN_DOWN((uint32_t)(e->base + e->length),
                                               PAGE_SIZE));
        /* Il tetto e' s_alloc_ceiling_frame, non s_total_frames: i frame
         * oltre il direct-map NON vengono liberati -> restano riservati ->
         * l'allocatore non li vede mai, e mmap_phys li tratta come RAM
         * occupata (nega la mappatura). Un solo punto impone l'invariante. */
        for (uint32_t f = fs; f < fe && f < s_alloc_ceiling_frame; f++)
        {
            frame_set_free(f);
        }
    }
}

static void reserve_critical_regions(void)
{
    /* Primo MB (IVT, BDA, VGA, BIOS) + immagine kernel + moduli GRUB. */
    pmm_mark_used(0, 0x100000);
    pmm_mark_used(0x100000, (uint32_t)&_kernel_phys_end - 0x100000);

    for (uint32_t i = 0; i < g_boot_info.module_count; i++)
    {
        pmm_mark_used(g_boot_info.modules[i].phys_start,
                      g_boot_info.modules[i].phys_end -
                      g_boot_info.modules[i].phys_start);
    }
}

/* === Verbi di allocazione =============================================== */

static uint32_t take_frame(uint32_t start, uint32_t end, uint32_t *hint)
{
    uint32_t f = zone_find_free(start, end, *hint);
    if (f == UINT32_MAX)
    {
        return 0;
    }

    frame_set_used(f);
    *hint = f + 1;
    return FRAME_TO_ADDR(f);
}

static bool release_frame_checked(uint32_t f, const char *caller_tag)
{
    /* Tetto allocabile, non estensione RAM: un frame oltre il tetto e'
     * riservato per costruzione e nessun percorso legittimo lo libera —
     * accettarlo qui lo re-indurrebbe nel pool (allocabile ma
     * irraggiungibile via direct-map), violando l'invariante. */
    if (f == 0 || f >= s_alloc_ceiling_frame)
    {
        return false;
    }
    if (!frame_is_used(f))
    {
        /* D4: double-free intercettato su OGNI percorso. */
        s_double_free_caught++;
        kprintf("[PMM ] DOUBLE-FREE frame 0x%08x (%s) - ignorato (#%u)\n",
                FRAME_TO_ADDR(f), caller_tag, s_double_free_caught);
        return false;
    }

    frame_set_free(f);
    return true;
}

/* === API ================================================================ */

void pmm_init(void)
{
    measure_usable_ram();
    release_available_regions();
    reserve_critical_regions();

    s_hint_dma    = 1;
    s_hint_normal = s_dma_limit_frame;

    /* Invariante strutturale: ogni frame allocabile e' raggiungibile via
     * direct-map. Vera per costruzione (s_alloc_ceiling_frame e' clampato),
     * ma verificata al boot: una modifica futura che la violasse si scopre
     * qui, non con un page fault dipendente dal carico mesi dopo. */
    if (s_alloc_ceiling_frame > ADDR_TO_FRAME(paging_direct_map_limit()))
    {
        kpanic("PMM: tetto allocabile (%u frame) oltre il direct-map (%u) "
               "- invariante violata", s_alloc_ceiling_frame,
               ADDR_TO_FRAME(paging_direct_map_limit()));
    }

    kprintf("[PMM ] %u frame gestiti, %u liberi (%u MB), DMA liberi %u\n",
            s_alloc_ceiling_frame, s_free_frames,
            (s_free_frames * PAGE_SIZE) >> 20, s_dma_free);
}

uint32_t pmm_alloc_frame(uint32_t zone_flags)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_lock);
    uint32_t pa;

    if (zone_flags & PMM_ZONE_DMA)
    {
        /* Requisito rigido: solo < 16 MB (il device non sa indirizzare
         * oltre). Nessun ripiego: fuori zona = OOM DMA, non un frame
         * alto che il device non potrebbe usare. */
        pa = take_frame(1, s_dma_limit_frame, &s_hint_dma);
    }
    else if (zone_flags & PMM_ZONE_PREFER_LOW)
    {
        /* Preferenza, non requisito: prima i frame bassi (azzerati
         * presto, minor finestra di gara sul PD kernel condiviso), poi
         * il resto della RAM. Page table e PD kernel. */
        pa = take_frame(1, s_dma_limit_frame, &s_hint_dma);
        if (pa == 0)
        {
            pa = take_frame(s_dma_limit_frame, s_alloc_ceiling_frame,
                            &s_hint_normal);
        }
    }
    else
    {
        /* Ovunque, con la zona DMA come ultima riserva (i frame bassi
         * sono scarsi: si spendono solo se la NORMAL e' vuota). */
        pa = take_frame(s_dma_limit_frame, s_alloc_ceiling_frame, &s_hint_normal);
        if (pa == 0)
        {
            pa = take_frame(1, s_dma_limit_frame, &s_hint_dma);
        }
    }

    spinlock_release_irqrestore(&s_lock, fl);
    return pa;
}

void pmm_free_frame(uint32_t phys_addr)
{
    if (phys_addr & (PAGE_SIZE - 1u))
    {
        kprintf("[PMM ] free non allineato 0x%08x - rifiutato\n", phys_addr);
        return;
    }

    uint32_t fl = spinlock_acquire_irqsave(&s_lock);
    release_frame_checked(ADDR_TO_FRAME(phys_addr), "free_frame");
    spinlock_release_irqrestore(&s_lock, fl);
}

uint32_t pmm_alloc_contiguous(uint32_t count, uint32_t zone_flags)
{
    if (count == 0)
    {
        return 0;
    }

    uint32_t fl = spinlock_acquire_irqsave(&s_lock);
    uint32_t base;

    if (zone_flags & PMM_ZONE_DMA)
    {
        /* Requisito rigido: solo < 16 MB, nessun ripiego. */
        base = zone_find_contiguous(1, s_dma_limit_frame, count);
    }
    else if (zone_flags & PMM_ZONE_PREFER_LOW)
    {
        /* Preferenza: prima < 16 MB (DMA controller legacy), poi il
         * resto (un bus-master PCI a 32 bit indirizza ovunque). */
        base = zone_find_contiguous(1, s_dma_limit_frame, count);
        if (base == UINT32_MAX)
        {
            base = zone_find_contiguous(s_dma_limit_frame,
                                        s_alloc_ceiling_frame, count);
        }
    }
    else
    {
        /* Ovunque, DMA come ultima riserva. */
        base = zone_find_contiguous(s_dma_limit_frame, s_alloc_ceiling_frame,
                                    count);
        if (base == UINT32_MAX)
        {
            base = zone_find_contiguous(1, s_dma_limit_frame, count);
        }
    }

    if (base == UINT32_MAX)
    {
        spinlock_release_irqrestore(&s_lock, fl);
        return 0;
    }

    for (uint32_t i = 0; i < count; i++)
    {
        frame_set_used(base + i);
    }

    spinlock_release_irqrestore(&s_lock, fl);
    return FRAME_TO_ADDR(base);
}

void pmm_free_contiguous(uint32_t phys_addr, uint32_t count)
{
    if (phys_addr & (PAGE_SIZE - 1u))
    {
        kprintf("[PMM ] free contiguo non allineato 0x%08x - rifiutato\n",
                phys_addr);
        return;
    }

    uint32_t fl = spinlock_acquire_irqsave(&s_lock);
    uint32_t base = ADDR_TO_FRAME(phys_addr);
    for (uint32_t i = 0; i < count; i++)
    {
        release_frame_checked(base + i, "free_contiguous");
    }
    spinlock_release_irqrestore(&s_lock, fl);
}

void pmm_mark_used(uint32_t phys_addr, uint32_t size)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_lock);
    uint32_t fs = ADDR_TO_FRAME(ALIGN_DOWN(phys_addr, PAGE_SIZE));
    uint32_t fe = ADDR_TO_FRAME(ALIGN_UP(phys_addr + size, PAGE_SIZE));

    for (uint32_t f = fs; f < fe && f < s_total_frames; f++)
    {
        if (!frame_is_used(f))
        {
            frame_set_used(f);
        }
    }
    spinlock_release_irqrestore(&s_lock, fl);
}

void pmm_get_stats(pmm_stats_t *out)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_lock);
    /* total_frames = pool GESTITO (allocabile), non l'estensione grezza:
     * su una macchina >256 MB solo il pool sotto il direct-map esiste per
     * l'allocatore, ed e' quello su cui pressione e OOM devono ragionare.
     * A <=256 MB s_alloc_ceiling_frame == s_total_frames: numeri identici. */
    out->total_frames       = s_alloc_ceiling_frame;
    out->free_frames        = s_free_frames;
    out->dma_free           = s_dma_free;
    out->double_free_caught = s_double_free_caught;
    out->pressure = s_alloc_ceiling_frame
                  ? ((s_alloc_ceiling_frame - s_free_frames) * 100u)
                        / s_alloc_ceiling_frame
                  : 100u;
    spinlock_release_irqrestore(&s_lock, fl);
}

bool pmm_is_frame_allocated(uint32_t phys_addr)
{
    uint32_t f = ADDR_TO_FRAME(phys_addr);
    if (f >= s_total_frames)
    {
        return false;                   /* sopra la RAM = MMIO            */
    }
    return frame_is_used(f);
}

uint32_t pmm_get_ram_top(void)
{
    return FRAME_TO_ADDR(s_total_frames);
}

/* === Self-test di boot ================================================== */

bool pmm_selftest(void)
{
    uint32_t a = pmm_alloc_frame(PMM_ZONE_ANY);
    uint32_t b = pmm_alloc_frame(PMM_ZONE_ANY);
    if (a == 0 || b == 0 || a == b)
    {
        return false;
    }

    pmm_free_frame(a);

    uint32_t caught_before = s_double_free_caught;
    pmm_free_frame(a);                          /* double-free voluto     */
    bool caught = (s_double_free_caught == caught_before + 1);

    uint32_t c = pmm_alloc_frame(PMM_ZONE_ANY); /* deve riusare a         */
    bool reused = (c == a);

    pmm_free_frame(b);
    pmm_free_frame(c);

    return caught && reused;
}
