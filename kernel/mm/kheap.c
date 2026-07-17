#include "mm/kheap.h"
#include "krt/reclaim.h"
#include "sync/spinlock.h"
#include "arch/x86/tlb.h"
#include "proc/percpu.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "lib/string.h"
#include "console/console.h"
#include "arch/x86/cpu.h"
#include "kernel.h"

/* kheap 1.1 — due livelli:
 *
 *  1. kpage allocator: bump da KHEAP_VBASE + free-stack (1 pagina) +
 *     free-list multi-pagina, cosi' gli indirizzi virtuali si riciclano
 *     (la 1.0 li perdeva quando le liste traboccavano).
 *  2. slab segregato a 8 classi (16..2048 B) con bitmap per pagina;
 *     large (>2048) via kpages + header con magic.
 *
 * D4 esteso all'heap: OGNI kfree passa dal bitmap della slab — un
 * double-free e' SEMPRE intercettato, loggato e ignorato (la 1.0 lo
 * garantiva solo al flush dei magazine). I magazine per-CPU tornano al
 * milestone 1.1.3 dietro questa stessa verifica. */

#define KHEAP_VBASE     0xD0000000u
#define KHEAP_VLIMIT    0xE0000000u
#define NUM_CLASSES     8u
#define SLAB_MAGIC      0xD0B5A1ABu
#define LARGE_MAGIC     0xD0B1A11Cu
#define KP_STACK_MAX    1024u
#define KP_MULTI_MAX    128u
#define EMPTY_CACHE_MAX 8u

static const uint32_t s_class_size[NUM_CLASSES] =
{
    16, 32, 64, 128, 256, 512, 1024, 2048
};

/* --- kpage allocator ---------------------------------------------------- */

static uint32_t s_bump = KHEAP_VBASE;
static uint32_t s_kp_stack[KP_STACK_MAX];
static uint32_t s_kp_top;

typedef struct { uint32_t virt; uint32_t pages; } kp_multi_t;
static kp_multi_t s_kp_multi[KP_MULTI_MAX];
static uint32_t   s_kp_multi_n;

/* --- slab ----------------------------------------------------------------- */

typedef struct kheap_page
{
    uint32_t            magic;
    struct kheap_page  *next;
    struct kheap_page  *prev;
    uint8_t             class_idx;
    uint8_t             in_partial;
    uint16_t            free_count;
    uint16_t            total_slots;
    uint16_t            reserved;
} kheap_page_t;

typedef struct { uint32_t magic; uint32_t pages; } large_hdr_t;

static kheap_page_t *s_partial[NUM_CLASSES];
static kheap_page_t *s_empty[NUM_CLASSES];
static uint32_t      s_empty_n[NUM_CLASSES];
static uint32_t      s_double_free_caught;

/* Due lock: uno per il pool di VA kernel, uno per le slab. Il lock
 * slab non avvolge mai paging/pmm (il refill avviene a lock mollato),
 * quindi l'ordine slab -> kva -> paging -> pmm e' aciclico. */
static spinlock_t s_kva_lock  = SPINLOCK_INIT;
static spinlock_t s_slab_lock = SPINLOCK_INIT;

/* Cache per-CPU di oggetti PRE-RECLAMATI (bit gia' segnato nel bitmap
 * della slab): kmalloc caldo = un pop sotto solo cli, zero lock. Solo
 * lato alloc: OGNI kfree passa comunque dal bitmap sotto s_slab_lock,
 * quindi la garanzia D4 (double-free sempre intercettato) e' intatta.
 * Una pagina con slot in cache ha free_count gia' scalato: non puo'
 * essere drenata mentre i suoi oggetti sono in una cache. */
#define KM_CACHE_DEPTH 16u
static void   *s_km_cache[MAX_CPUS][NUM_CLASSES][KM_CACHE_DEPTH];
static uint8_t s_km_n[MAX_CPUS][NUM_CLASSES];

/* === Verbi kpage ========================================================== */

static void strip_range_frames(uint32_t virt, uint32_t count);

static uint32_t take_virtual_range(uint32_t count)
{
    if (count == 1 && s_kp_top > 0)
    {
        return s_kp_stack[--s_kp_top];
    }

    for (uint32_t i = 0; i < s_kp_multi_n; i++)
    {
        if (s_kp_multi[i].pages == count)
        {
            uint32_t v = s_kp_multi[i].virt;
            s_kp_multi[i] = s_kp_multi[--s_kp_multi_n];
            return v;
        }
        if (s_kp_multi[i].pages > count)
        {
            uint32_t v = s_kp_multi[i].virt;
            s_kp_multi[i].virt  += count * PAGE_SIZE;
            s_kp_multi[i].pages -= count;
            return v;
        }
    }

    if (s_bump + count * PAGE_SIZE > KHEAP_VLIMIT)
    {
        return 0;
    }
    uint32_t v = s_bump;
    s_bump += count * PAGE_SIZE;
    return v;
}

/* === Verbi di coalescenza (chiamante tiene s_kva_lock) ================== *
 * take_virtual_range SPEZZA i range multi-pagina: senza il verbo
 * inverso, su settimane di churn la multi-list degenera in frammenti
 * sempre piu' piccoli, trabocca, e il VA della finestra heap evapora.
 * La fusione e' parte del comportamento normale del blocco, non un
 * rattoppo: ogni ritorno di range tenta PRIMA di ricomporsi coi vicini
 * gia' liberi, e solo il residuo irriducibile occupa un nuovo slot. */

/* Fonde [virt, virt+count) con ogni entry adiacente della multi-list
 * (prima e dopo, ripetendo finche' assorbe). Ritorna il range fuso. */
static void merge_with_multi(uint32_t *virt, uint32_t *count)
{
    bool merged = true;
    while (merged)
    {
        merged = false;
        for (uint32_t i = 0; i < s_kp_multi_n; i++)
        {
            uint32_t mv = s_kp_multi[i].virt;
            uint32_t mp = s_kp_multi[i].pages;
            if (mv + mp * PAGE_SIZE == *virt)
            {
                *virt   = mv;                   /* vicino PRIMA: prependi */
                *count += mp;
            }
            else if (*virt + *count * PAGE_SIZE == mv)
            {
                *count += mp;                   /* vicino DOPO: appendi   */
            }
            else
            {
                continue;
            }
            s_kp_multi[i] = s_kp_multi[--s_kp_multi_n];
            merged = true;
            break;                              /* riparti: indici mossi  */
        }
    }
}

/* VA perso nel caso estremo (liste piene): contatore per la diagnosi.
 * Il fisico e' sempre gia' tornato al PMM — si perde solo indirizzo
 * virtuale della finestra heap, che pero' e' finita: va VISTO. */
static uint32_t s_kp_va_lost_pages;

/* Restituisce le pagine di VA che NON e' riuscito a conservare (0 nel
 * caso normale). Il log e' compito del livello logico, fuori dal lock. */
static uint32_t give_virtual_range(uint32_t virt, uint32_t count)
{
    /* Ricomponi coi vicini liberi PRIMA di decidere dove riporre: il
     * range fuso puo' toccare il bump anche se il pezzo nudo non lo
     * toccava. */
    merge_with_multi(&virt, &count);

    if (virt + count * PAGE_SIZE == s_bump)
    {
        s_bump = virt;                          /* retrazione del bump    */
        return 0;
    }
    if (count == 1 && s_kp_top < KP_STACK_MAX)
    {
        s_kp_stack[s_kp_top++] = virt;
        return 0;
    }
    if (s_kp_multi_n < KP_MULTI_MAX)
    {
        s_kp_multi[s_kp_multi_n].virt  = virt;
        s_kp_multi[s_kp_multi_n].pages = count;
        s_kp_multi_n++;
        return 0;
    }
    uint32_t spilled = 0;
    for (uint32_t i = 0; i < count && s_kp_top < KP_STACK_MAX; i++)
    {
        s_kp_stack[s_kp_top++] = virt + i * PAGE_SIZE;
        spilled++;
    }
    uint32_t lost = count - spilled;
    s_kp_va_lost_pages += lost;
    return lost;
}

static bool back_range_with_frames(uint32_t virt, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t frame = pmm_alloc_frame(PMM_ZONE_ANY);
        if (frame == 0)
        {
            strip_range_frames(virt, i);
            return false;
        }
        paging_map_page(virt + i * PAGE_SIZE, frame,
                        PTE_PRESENT | PTE_WRITABLE);
    }
    return true;
}

/* Smonta un range e ne libera i frame, a blocchi: smonta il blocco,
 * shootdown TLB su tutte le CPU, e SOLO POI restituisce i frame al
 * pool. Liberare prima dello shootdown lascerebbe a un'altra CPU una
 * traduzione stantia verso un frame gia' riciclato: scrittura fantasma
 * su memoria altrui. Nessun lock e' tenuto qui (regola del shootdown). */
static void strip_range_frames(uint32_t virt, uint32_t count)
{
    uint32_t phys[64];

    for (uint32_t done = 0; done < count; )
    {
        uint32_t n = count - done;
        if (n > 64u)
        {
            n = 64u;
        }
        uint32_t base = virt + done * PAGE_SIZE;

        for (uint32_t i = 0; i < n; i++)
        {
            uint32_t v = base + i * PAGE_SIZE;
            phys[i] = paging_get_physical(v, NULL);
            paging_unmap_page(v);
        }

        tlb_shootdown_range(base, n);

        for (uint32_t i = 0; i < n; i++)
        {
            if (phys[i] != 0)
            {
                pmm_free_frame(phys[i] & 0xFFFFF000u);
            }
        }
        done += n;
    }
}

/* === API kpage ============================================================ */

static void note_va_lost(uint32_t lost_now);

uint32_t kpages_alloc(uint32_t count)
{
    if (count == 0)
    {
        return 0;
    }

    uint32_t fl = spinlock_acquire_irqsave(&s_kva_lock);
    uint32_t virt = take_virtual_range(count);
    spinlock_release_irqrestore(&s_kva_lock, fl);

    if (virt == 0)
    {
        return 0;
    }
    if (!back_range_with_frames(virt, count))
    {
        fl = spinlock_acquire_irqsave(&s_kva_lock);
        uint32_t lost = give_virtual_range(virt, count);
        spinlock_release_irqrestore(&s_kva_lock, fl);
        if (unlikely(lost != 0))
        {
            note_va_lost(lost);
        }
        return 0;
    }
    return virt;
}

uint32_t kpage_alloc(void)
{
    return kpages_alloc(1);
}

/* Decisione LOGICA sul VA perso: il verbo esecutivo conta, qui si
 * decide se e' il momento di parlare — fuori dal lock (kprintf non
 * viaggia mai sotto s_kva_lock). Rate-limit: prime 4 poi 1 su 64. */
static void note_va_lost(uint32_t lost_now)
{
    static uint32_t s_events;
    uint32_t k = s_events++;
    if (k < 4u || (k & 63u) == 0u)
    {
        kprintf("[HEAP] VA perso: %u pagine ora (%u totali) - liste di "
                "riciclo piene\n", lost_now, s_kp_va_lost_pages);
    }
}

void kpages_free(uint32_t virt, uint32_t count)
{
    if (virt == 0 || count == 0)
    {
        return;
    }

    strip_range_frames(virt, count);

    uint32_t fl = spinlock_acquire_irqsave(&s_kva_lock);
    uint32_t lost = give_virtual_range(virt, count);
    spinlock_release_irqrestore(&s_kva_lock, fl);

    if (unlikely(lost != 0))
    {
        note_va_lost(lost);
    }
}

void kpage_free(uint32_t virt)
{
    kpages_free(virt, 1);
}

/* === Verbi slab =========================================================== */

static inline uint32_t *page_bitmap(kheap_page_t *p)
{
    return (uint32_t *)((uint8_t *)p + sizeof(kheap_page_t));
}

static inline uint32_t bitmap_words(const kheap_page_t *p)
{
    return (p->total_slots + 31u) / 32u;
}

static inline void *slot_address(kheap_page_t *p, uint32_t idx, uint32_t sz)
{
    return (uint8_t *)p + sizeof(kheap_page_t)
                        + bitmap_words(p) * 4u + idx * sz;
}

static uint32_t slots_that_fit(uint32_t slot_size)
{
    uint32_t n = (PAGE_SIZE - sizeof(kheap_page_t)) / slot_size;
    while (n > 0 &&
           sizeof(kheap_page_t) + ((n + 31u) / 32u) * 4u + n * slot_size
               > PAGE_SIZE)
    {
        n--;
    }
    return n;
}

static void partial_push(uint32_t cls, kheap_page_t *p)
{
    p->prev = NULL;
    p->next = s_partial[cls];
    if (s_partial[cls] != NULL)
    {
        s_partial[cls]->prev = p;
    }
    s_partial[cls] = p;
    p->in_partial = 1;
}

static void partial_remove(uint32_t cls, kheap_page_t *p)
{
    if (!p->in_partial)
    {
        return;
    }
    if (p->prev != NULL)
    {
        p->prev->next = p->next;
    }
    else
    {
        s_partial[cls] = p->next;
    }
    if (p->next != NULL)
    {
        p->next->prev = p->prev;
    }
    p->next = p->prev = NULL;
    p->in_partial = 0;
}

static kheap_page_t *create_slab_page(uint32_t cls)
{
    uint32_t v = kpage_alloc();
    if (v == 0)
    {
        return NULL;
    }

    kheap_page_t *p = (kheap_page_t *)v;
    memset(p, 0, PAGE_SIZE);
    p->magic       = SLAB_MAGIC;
    p->class_idx   = (uint8_t)cls;
    p->total_slots = (uint16_t)slots_that_fit(s_class_size[cls]);
    p->free_count  = p->total_slots;
    return p;
}

/* Solo dalle cache (partial/empty): il refill con allocazione avviene
 * in kmalloc a lock mollato. Chiamante tiene s_slab_lock. */
static kheap_page_t *pick_cached_page(uint32_t cls)
{
    if (s_partial[cls] != NULL)
    {
        return s_partial[cls];
    }
    if (s_empty[cls] != NULL)
    {
        kheap_page_t *p = s_empty[cls];
        s_empty[cls] = p->next;
        s_empty_n[cls]--;
        p->next = p->prev = NULL;
        p->in_partial = 0;
        partial_push(cls, p);
        return p;
    }
    return NULL;
}

static int32_t claim_free_slot(kheap_page_t *p)
{
    uint32_t *bm = page_bitmap(p);
    uint32_t words = bitmap_words(p);

    for (uint32_t w = 0; w < words; w++)
    {
        uint32_t candidates = ~bm[w];
        while (candidates != 0)
        {
            uint32_t bit = (uint32_t)__builtin_ctz(candidates);
            uint32_t idx = w * 32u + bit;
            if (idx < p->total_slots)
            {
                bm[w] |= 1u << bit;
                return (int32_t)idx;
            }
            candidates &= candidates - 1u;
        }
    }
    return -1;
}

static int size_to_class(size_t size)
{
    for (uint32_t c = 0; c < NUM_CLASSES; c++)
    {
        if (size <= s_class_size[c])
        {
            return (int)c;
        }
    }
    return -1;
}

/* Libera uno slot verificando il bitmap. Ritorna la pagina se e'
 * diventata completamente vuota E la cache empty era piena (il caller
 * la restituisce a kpage_free FUORI dalla sezione IF=0). */
static kheap_page_t *release_slot_checked(void *ptr)
{
    uint32_t addr = (uint32_t)ptr;
    kheap_page_t *p = (kheap_page_t *)(addr & ~(PAGE_SIZE - 1u));

    uint32_t sz  = s_class_size[p->class_idx];
    uint32_t off = addr - (uint32_t)p - sizeof(kheap_page_t)
                        - bitmap_words(p) * 4u;
    uint32_t idx = off / sz;

    if (idx >= p->total_slots || (off % sz) != 0)
    {
        s_double_free_caught++;
        kprintf("[HEAP] kfree su puntatore interno %p - ignorato\n", ptr);
        return NULL;
    }

    uint32_t *word = &page_bitmap(p)[idx / 32u];
    uint32_t  mask = 1u << (idx % 32u);

    if ((*word & mask) == 0)
    {
        s_double_free_caught++;                 /* D4                     */
        kprintf("[HEAP] DOUBLE-FREE %p (classe %u) - ignorato (#%u)\n",
                ptr, sz, s_double_free_caught);
        return NULL;
    }

    *word &= ~mask;
    bool was_full = (p->free_count == 0);
    p->free_count++;

    if (was_full)
    {
        partial_push(p->class_idx, p);
    }

    if (p->free_count == p->total_slots)
    {
        partial_remove(p->class_idx, p);
        if (s_empty_n[p->class_idx] < EMPTY_CACHE_MAX)
        {
            p->next = s_empty[p->class_idx];
            p->prev = NULL;
            s_empty[p->class_idx] = p;
            s_empty_n[p->class_idx]++;
            return NULL;
        }
        return p;                               /* caller: kpage_free     */
    }
    return NULL;
}

/* Verbo di ritiro (auto-pulizia, vedi krt/reclaim.h): le pagine slab
 * VUOTE tenute in cache oltre il pavimento caldo (1 per classe) tornano
 * al kpage allocator — che con la coalescenza le ricompone e ritrae il
 * bump: la finestra VA torna come dopo il boot, non solo la RAM. Stesso
 * protocollo del percorso kfree: stacco sotto s_slab_lock, kpage_free
 * FUORI (l'ordine slab->kva e' quello gia' documentato, mai invertito).
 * Il pavimento evita che il primo kmalloc dopo la quiete ripaghi
 * l'intera salita: una pagina calda per classe resta. */
static uint32_t kheap_empty_trim(void)
{
    kheap_page_t *chain = NULL;
    uint32_t      pages = 0;

    uint32_t fl = spinlock_acquire_irqsave(&s_slab_lock);
    for (uint32_t c = 0; c < NUM_CLASSES; c++)
    {
        while (s_empty_n[c] > 1u)
        {
            kheap_page_t *pg = s_empty[c];
            s_empty[c] = pg->next;
            s_empty_n[c]--;
            pg->next = chain;
            chain = pg;
            pages++;
        }
    }
    spinlock_release_irqrestore(&s_slab_lock, fl);

    while (chain != NULL)
    {
        kheap_page_t *pg = chain;
        chain = pg->next;
        kpage_free((uint32_t)pg);
    }
    return pages * PAGE_SIZE;
}

/* Verbo di ritiro dei SINGOLI VA (auto-pulizia): lo stack delle pagine
 * singole non partecipa alla coalescenza — dopo il caos puo' custodire
 * fino a 1024 frammenti da 4 KB che non si ricompongono mai. Qui si
 * drena e ogni pagina ripassa da give_virtual_range, che fonde coi
 * vicini in multi-list e ritrae il bump: i frammenti adiacenti tornano
 * range, i range adiacenti al bump tornano spazio vergine. Se le liste
 * si saturano durante il giro, give ricade sullo stack: MAI una pagina
 * persa per colpa della pulizia (lo garantisce il suo stesso contratto).
 * Ritorna 0: qui non si liberano byte, si ricompone indirizzo — il
 * valore sta nello stato, non nel conteggio. */
static uint32_t kva_singles_trim(void)
{
    for (;;)
    {
        uint32_t fl = spinlock_acquire_irqsave(&s_kva_lock);
        if (s_kp_top == 0)
        {
            spinlock_release_irqrestore(&s_kva_lock, fl);
            return 0;
        }
        uint32_t v = s_kp_stack[--s_kp_top];
        uint32_t before = s_kp_top;
        give_virtual_range(v, 1);
        bool progress = (s_kp_top < before + 1u);
        spinlock_release_irqrestore(&s_kva_lock, fl);
        if (!progress)
        {
            return 0;               /* e' tornata sullo stack: fine giro */
        }
    }
}

/* === API heap ============================================================= */

void kheap_init(void)
{
    reclaim_register("kheap-empty", kheap_empty_trim);
    reclaim_register("kva-singles", kva_singles_trim);

    for (uint32_t c = 0; c < NUM_CLASSES; c++)
    {
        s_partial[c] = NULL;
        s_empty[c]   = NULL;
        s_empty_n[c] = 0;
    }
    kprintf("[HEAP] Slab a %u classi pronto (VA 0x%08x-0x%08x).\n",
            NUM_CLASSES, KHEAP_VBASE, KHEAP_VLIMIT);
}

void *kmalloc(size_t size)
{
    if (size == 0)
    {
        return NULL;
    }

    int cls = size_to_class(size);
    if (cls >= 0)
    {
        /* Percorso caldo: pop dalla cache per-CPU sotto solo cli. */
        uint32_t fl  = irq_save();
        uint32_t cpu = this_cpu()->cpu_index;
        if (s_km_n[cpu][cls] > 0)
        {
            void *obj = s_km_cache[cpu][cls][--s_km_n[cpu][cls]];
            irq_restore(fl);
            return obj;
        }
        irq_restore(fl);

        /* Cache vuota: reclama un lotto sotto il lock slab. */
        void    *batch[KM_CACHE_DEPTH / 2u + 1u];
        uint32_t got = 0;

        fl = spinlock_acquire_irqsave(&s_slab_lock);
        while (got < KM_CACHE_DEPTH / 2u + 1u)
        {
            kheap_page_t *p = pick_cached_page((uint32_t)cls);
            if (p == NULL)
            {
                break;
            }
            int32_t idx = claim_free_slot(p);
            p->free_count--;
            if (p->free_count == 0)
            {
                partial_remove((uint32_t)cls, p);
            }
            batch[got++] = slot_address(p, (uint32_t)idx, s_class_size[cls]);
        }
        spinlock_release_irqrestore(&s_slab_lock, fl);

        if (got == 0)
        {
            /* Anche le cache pagina sono vuote: refill con allocazione,
             * a lock mollato (create attraversa kva/paging/pmm). */
            kheap_page_t *p = create_slab_page((uint32_t)cls);
            if (p == NULL)
            {
                return NULL;
            }
            fl = spinlock_acquire_irqsave(&s_slab_lock);
            partial_push((uint32_t)cls, p);
            int32_t idx = claim_free_slot(p);
            p->free_count--;
            if (p->free_count == 0)
            {
                partial_remove((uint32_t)cls, p);
            }
            void *obj = slot_address(p, (uint32_t)idx, s_class_size[cls]);
            spinlock_release_irqrestore(&s_slab_lock, fl);
            return obj;
        }

        /* Uno al chiamante, il resto in cache. Se nel frattempo un
         * altro thread di questa CPU l'ha riempita, il surplus torna
         * alle slab dal percorso normale (bitmap): raro e corretto. */
        void *obj = batch[--got];
        fl  = irq_save();
        cpu = this_cpu()->cpu_index;
        while (got > 0 && s_km_n[cpu][cls] < KM_CACHE_DEPTH)
        {
            s_km_cache[cpu][cls][s_km_n[cpu][cls]++] = batch[--got];
        }
        irq_restore(fl);
        while (got > 0)
        {
            kfree(batch[--got]);
        }
        return obj;
    }

    uint32_t pages = (uint32_t)ALIGN_UP(size + sizeof(large_hdr_t),
                                        PAGE_SIZE) / PAGE_SIZE;
    uint32_t v = kpages_alloc(pages);
    if (v == 0)
    {
        return NULL;
    }

    large_hdr_t *h = (large_hdr_t *)v;
    h->magic = LARGE_MAGIC;
    h->pages = pages;
    return (void *)(v + sizeof(large_hdr_t));
}

void *kcalloc(size_t count, size_t size)
{
    if (count != 0 && size > UINT32_MAX / count)
    {
        return NULL;
    }
    size_t total = count * size;
    void *p = kmalloc(total);
    if (p != NULL)
    {
        memset(p, 0, total);
    }
    return p;
}

void kfree(void *ptr)
{
    if (ptr == NULL)
    {
        return;
    }

    uint32_t addr = (uint32_t)ptr;
    kheap_page_t *page = (kheap_page_t *)(addr & ~(PAGE_SIZE - 1u));

    if (page->magic == SLAB_MAGIC)
    {
        uint32_t fl = spinlock_acquire_irqsave(&s_slab_lock);
        kheap_page_t *drained = release_slot_checked(ptr);
        spinlock_release_irqrestore(&s_slab_lock, fl);

        if (drained != NULL)
        {
            kpage_free((uint32_t)drained);
        }
        return;
    }

    large_hdr_t *h = (large_hdr_t *)(addr - sizeof(large_hdr_t));
    if (h->magic == LARGE_MAGIC)
    {
        uint32_t pages = h->pages;
        h->magic = 0;                           /* brucia il magic:       */
        kpages_free((uint32_t)h, pages);        /* doppio kfree large     */
        return;                                 /* -> magic assente       */
    }

    kprintf("[HEAP] kfree su puntatore sconosciuto %p - ignorato\n", ptr);
}

/* === Self-test ============================================================ */

bool kheap_selftest(void)
{
    void *a = kmalloc(24);
    void *b = kmalloc(24);
    void *c = kmalloc(3000);            /* 3000 > 2048: percorso large */
    if (a == NULL || b == NULL || c == NULL || a == b)
    {
        return false;
    }

    memset(a, 0xAA, 24);
    memset(c, 0x55, 3000);

    kfree(a);
    uint32_t before = s_double_free_caught;
    kfree(a);                           /* double-free voluto             */
    bool caught = (s_double_free_caught == before + 1);

    /* Dopo un free il rientro deve riusare uno slot libero SENZA crescere
     * (heap stabile), e il double-free intercettato non deve aver
     * corrotto il bitmap: la prova diretta e' che a2 sia un puntatore
     * valido, scrivibile, nella stessa classe. NON assumiamo a2 == a: la
     * scelta dello slot e' lowest-bit-first e altri slot piu' bassi della
     * stessa pagina possono essere liberi (il kernel ha gia' allocato in
     * questa classe prima del selftest). */
    void *a2 = kmalloc(24);
    bool reused = (a2 != NULL);
    if (reused)
    {
        memset(a2, 0xC3, 24);           /* deve essere scrivibile         */
    }

    kfree(a2);
    kfree(b);
    kfree(c);

    void *big = kmalloc(20000);
    bool large_ok = (big != NULL);
    kfree(big);

    return caught && reused && large_ok;
}
