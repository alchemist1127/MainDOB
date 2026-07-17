#include "mm/shm.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/kheap.h"
#include "mm/vregion.h"
#include "krt/handle_table.h"
#include "krt/refcount.h"
#include "lib/string.h"
#include "console/console.h"
#include "kernel.h"
#include "proc/process.h"
#include "arch/x86/tlb.h"

/* SHM 1.1 — segmenti fisici condivisi. Componenti standard riusati:
 * krt/handle_table per gli id (generation anti-ABA), krt/refcount per
 * il ciclo di vita (un ref per mapping + uno della tabella), vregion
 * per il tracciamento nel processo. I frame sono liberati dall'ULTIMO
 * unmap dopo la destroy — mai mentre qualcuno li vede. */

#define SHM_MAX             128
#define SHM_MAX_PAGES       1024u       /* 4 MB per segmento              */
#define SHM_USER_BASE       0x60000000u
#define SHM_USER_LIMIT      0x70000000u

typedef struct
{
    uint32_t   *frames;                 /* array di frame fisici          */
    uint32_t    pages;
    refcount_t  refcount;
    pid_t       creator;
} shm_segment_t;

static handle_table_t s_shm_table;

/* === Verbi ============================================================== */

static shm_segment_t *segment_build(uint32_t pages, pid_t creator)
{
    shm_segment_t *seg = (shm_segment_t *)kcalloc(1, sizeof(shm_segment_t));
    if (seg == NULL)
    {
        return NULL;
    }
    seg->frames = (uint32_t *)kcalloc(pages, sizeof(uint32_t));
    if (seg->frames == NULL)
    {
        kfree(seg);
        return NULL;
    }

    for (uint32_t i = 0; i < pages; i++)
    {
        seg->frames[i] = pmm_alloc_frame(PMM_ZONE_ANY);
        if (seg->frames[i] == 0)
        {
            for (uint32_t j = 0; j < i; j++)
            {
                pmm_free_frame(seg->frames[j]);
            }
            kfree(seg->frames);
            kfree(seg);
            return NULL;
        }
        memset((void *)(seg->frames[i] + KERNEL_VMA), 0, PAGE_SIZE);
    }

    seg->pages   = pages;
    seg->creator = creator;
    refcount_init(&seg->refcount, 1);   /* ref della tabella              */
    return seg;
}

static void segment_release(shm_segment_t *seg)
{
    if (refcount_dec(&seg->refcount))
    {
        for (uint32_t i = 0; i < seg->pages; i++)
        {
            pmm_free_frame(seg->frames[i]);
        }
        kfree(seg->frames);
        kfree(seg);
    }
}

static uint32_t map_into(shm_segment_t *seg, process_t *proc, int32_t id)
{
    mutex_lock(&proc->vm_lock);
    uint32_t vaddr = vregion_alloc(&proc->vm_regions, seg->pages,
                                   SHM_USER_BASE, SHM_USER_LIMIT,
                                   VREG_SHARED | VREG_USER_RW, id);
    if (vaddr == 0)
    {
        mutex_unlock(&proc->vm_lock);
        return 0;
    }

    for (uint32_t i = 0; i < seg->pages; i++)
    {
        paging_map_in(proc->page_directory, vaddr + i * PAGE_SIZE,
                      seg->frames[i],
                      PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    vregion_t *r = vregion_find_exact(&proc->vm_regions, vaddr);
    if (r != NULL)
    {
        r->committed = seg->pages;
    }
    mutex_unlock(&proc->vm_lock);
    return vaddr;
}

static void unmap_from(shm_segment_t *seg, process_t *proc, uint32_t vaddr)
{
    mutex_lock(&proc->vm_lock);
    for (uint32_t i = 0; i < seg->pages; i++)
    {
        /* Solo smonta, NEL PD del processo (che puo' non essere quello
         * corrente: la cleanup gira dal reaper). I frame appartengono
         * al segmento (refcount). */
        paging_unmap_in(proc->page_directory, vaddr + i * PAGE_SIZE);
    }
    /* Prima che l'ultimo unref possa liberare i frame del segmento,
     * nessuna CPU che esegue questo AS deve tenerne traduzioni. */
    tlb_shootdown_aspace(proc->page_directory, vaddr, seg->pages);
    vregion_free(&proc->vm_regions, vaddr);
    mutex_unlock(&proc->vm_lock);
}

/* Trova il mapping di `id` nel processo (vregion SHARED con backing_id). */
static vregion_t *find_mapping(process_t *proc, int32_t id)
{
    for (vregion_t *r = proc->vm_regions.head; r != NULL; r = r->next)
    {
        if ((r->flags & VREG_SHARED) && r->backing_id == id)
        {
            return r;
        }
    }
    return NULL;
}

/* === API ================================================================ */

void shm_init(void)
{
    if (!handle_table_init(&s_shm_table, SHM_MAX, HT_REUSE_LIFO))
    {
        kpanic("shm: impossibile allocare la tabella");
    }
    kprintf("[SHM ] Memoria condivisa pronta (max %u segmenti).\n",
            (uint32_t)SHM_MAX);
}

int32_t shm_create(uint32_t size, process_t *creator, uint32_t *out_vaddr)
{
    if (size == 0)
    {
        return -1;
    }
    uint32_t pages = ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE;
    if (pages > SHM_MAX_PAGES)
    {
        return -1;
    }

    shm_segment_t *seg = segment_build(pages, creator->pid);
    if (seg == NULL)
    {
        return -1;
    }

    handle_ref_t ref = handle_table_insert(&s_shm_table, seg);
    if (ref.id == 0)
    {
        segment_release(seg);
        return -1;
    }

    refcount_inc(&seg->refcount);       /* ref del mapping del creatore   */
    uint32_t vaddr = map_into(seg, creator, (int32_t)ref.id);
    if (vaddr == 0)
    {
        refcount_dec(&seg->refcount);
        handle_table_remove(&s_shm_table, ref.id);
        segment_release(seg);
        return -1;
    }

    if (out_vaddr != NULL)
    {
        *out_vaddr = vaddr;
    }
    return (int32_t)ref.id;
}

int32_t shm_map(int32_t id, process_t *proc, uint32_t *out_vaddr)
{
    shm_segment_t *seg =
        (shm_segment_t *)handle_table_get(&s_shm_table, (uint32_t)id);
    if (seg == NULL)
    {
        return -1;
    }
    if (find_mapping(proc, id) != NULL)
    {
        return -1;                      /* gia' mappato in questo proc    */
    }

    refcount_inc(&seg->refcount);
    uint32_t vaddr = map_into(seg, proc, id);
    if (vaddr == 0)
    {
        refcount_dec(&seg->refcount);
        return -1;
    }
    if (out_vaddr != NULL)
    {
        *out_vaddr = vaddr;
    }
    return 0;
}

int32_t shm_unmap(int32_t id, process_t *proc)
{
    shm_segment_t *seg =
        (shm_segment_t *)handle_table_get(&s_shm_table, (uint32_t)id);
    vregion_t *r = find_mapping(proc, id);
    if (seg == NULL || r == NULL)
    {
        return -1;
    }

    unmap_from(seg, proc, r->base);
    segment_release(seg);               /* droppa il ref del mapping      */
    return 0;
}

void shm_cleanup_process(process_t *proc)
{
    /* Gira dal reaper a processo senza thread: la ricerca sotto lock e
     * l'unmap fuori (unmap_from riprende il lock) non hanno finestre
     * sfruttabili — nessun altro puo' mutare queste vregion. */
    for (;;)
    {
        int32_t backing = -1;
        mutex_lock(&proc->vm_lock);
        for (vregion_t *c = proc->vm_regions.head; c != NULL; c = c->next)
        {
            if (c->flags & VREG_SHARED)
            {
                backing = c->backing_id;
                break;
            }
        }
        mutex_unlock(&proc->vm_lock);
        if (backing < 0)
        {
            return;
        }
        shm_unmap(backing, proc);
    }
}
