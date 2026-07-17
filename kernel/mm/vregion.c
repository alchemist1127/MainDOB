#include "mm/vregion.h"
#include "mm/kheap.h"
#include "mm/pmm.h"
#include "lib/string.h"

/* Lista ordinata per base. O(n): accettabile per il numero di regioni
 * tipico di un processo mainDOB (<32). Se un profilo futuro mostra
 * processi con centinaia di regioni si passa a un albero, dietro
 * questa stessa API. */

/* === Verbi ============================================================== */

static void link_between(vregion_list_t *vl, vregion_t *n,
                         vregion_t *prev, vregion_t *next)
{
    n->prev = prev;
    n->next = next;

    if (prev != NULL)
    {
        prev->next = n;
    }
    else
    {
        vl->head = n;
    }

    if (next != NULL)
    {
        next->prev = n;
    }
    else
    {
        vl->tail = n;
    }
    vl->count++;
    vl->total_pages += n->pages;
    if (n->flags & VREG_DEVICE)
    {
        vl->device_pages += n->pages;
    }
}

static void unlink_node(vregion_list_t *vl, vregion_t *n)
{
    if (n->prev != NULL)
    {
        n->prev->next = n->next;
    }
    else
    {
        vl->head = n->next;
    }

    if (n->next != NULL)
    {
        n->next->prev = n->prev;
    }
    else
    {
        vl->tail = n->prev;
    }
    vl->total_pages -= n->pages;
    if (n->flags & VREG_DEVICE)
    {
        vl->device_pages -= n->pages;
    }
    vl->count--;
}

static vregion_t *make_node(uint32_t base, uint32_t pages,
                            uint32_t flags, int32_t backing_id)
{
    vregion_t *n = (vregion_t *)kcalloc(1, sizeof(vregion_t));
    if (n == NULL)
    {
        return NULL;
    }
    n->base       = base;
    n->pages      = pages;
    n->flags      = flags;
    n->backing_id = backing_id;
    return n;
}

/* === API ================================================================ */

void vregion_init(vregion_list_t *vl)
{
    vl->head  = NULL;
    vl->tail  = NULL;
    vl->count = 0;
    vl->total_pages = 0;
    vl->device_pages = 0;
}

void vregion_destroy(vregion_list_t *vl)
{
    vregion_t *c = vl->head;
    while (c != NULL)
    {
        vregion_t *next = c->next;
        kfree(c);
        c = next;
    }
    vregion_init(vl);
}

uint32_t vregion_alloc(vregion_list_t *vl, uint32_t pages,
                       uint32_t min_addr, uint32_t max_addr,
                       uint32_t flags, int32_t backing_id)
{
    if (pages == 0)
    {
        return 0;
    }

    uint32_t size = pages * PAGE_SIZE;
    uint32_t cand = ALIGN_UP(min_addr, PAGE_SIZE);
    vregion_t *prev = NULL;
    vregion_t *cur  = vl->head;

    while (cur != NULL)
    {
        uint32_t cur_end = cur->base + cur->pages * PAGE_SIZE;
        if (cur_end <= cand)
        {
            prev = cur;
            cur = cur->next;
            continue;
        }
        if (cand + size <= cur->base && cand + size <= max_addr)
        {
            break;                              /* buco trovato           */
        }
        cand = ALIGN_UP(cur_end, PAGE_SIZE);
        prev = cur;
        cur = cur->next;
    }

    if (cand + size > max_addr || cand + size < cand)
    {
        return 0;
    }

    vregion_t *n = make_node(cand, pages, flags, backing_id);
    if (n == NULL)
    {
        return 0;
    }
    link_between(vl, n, prev, cur);
    return cand;
}

vregion_t *vregion_insert(vregion_list_t *vl, uint32_t base,
                          uint32_t pages, uint32_t flags)
{
    base = ALIGN_DOWN(base, PAGE_SIZE);
    uint32_t end = base + pages * PAGE_SIZE;
    if (end < base)
    {
        return NULL;                            /* wrap 32-bit            */
    }

    vregion_t *prev = NULL;
    vregion_t *cur  = vl->head;
    while (cur != NULL && cur->base < end)
    {
        uint32_t cur_end = cur->base + cur->pages * PAGE_SIZE;
        if (base < cur_end && end > cur->base)
        {
            return NULL;                        /* overlap                */
        }
        prev = cur;
        cur = cur->next;
    }

    vregion_t *n = make_node(base, pages, flags, -1);
    if (n == NULL)
    {
        return NULL;
    }
    link_between(vl, n, prev, cur);
    return n;
}

uint32_t vregion_free(vregion_list_t *vl, uint32_t base)
{
    vregion_t *n = vregion_find_exact(vl, base);
    if (n == NULL)
    {
        return 0;
    }
    uint32_t pages = n->pages;
    unlink_node(vl, n);
    kfree(n);
    return pages;
}

bool vregion_resize(vregion_list_t *vl, uint32_t base, uint32_t new_pages)
{
    vregion_t *n = vregion_find_exact(vl, base);
    if (n == NULL || new_pages == 0)
    {
        return false;
    }
    if (new_pages > (UINT32_MAX - base) / PAGE_SIZE)
    {
        return false;                           /* wrap 32-bit            */
    }
    if (new_pages > n->pages && n->next != NULL &&
        base + new_pages * PAGE_SIZE > n->next->base)
    {
        return false;                           /* invaderebbe il vicino  */
    }
    if (new_pages < n->committed)
    {
        n->committed = new_pages;
    }
    vl->total_pages += new_pages - n->pages;   /* delta, anche negativo */
    if (n->flags & VREG_DEVICE)
    {
        vl->device_pages += new_pages - n->pages;
    }
    n->pages = new_pages;
    return true;
}

vregion_t *vregion_find(vregion_list_t *vl, uint32_t addr)
{
    for (vregion_t *c = vl->head; c != NULL; c = c->next)
    {
        if (c->base > addr)
        {
            return NULL;
        }
        if (addr < c->base + c->pages * PAGE_SIZE)
        {
            return c;
        }
    }
    return NULL;
}

vregion_t *vregion_find_exact(vregion_list_t *vl, uint32_t base)
{
    for (vregion_t *c = vl->head; c != NULL; c = c->next)
    {
        if (c->base == base)
        {
            return c;
        }
        if (c->base > base)
        {
            return NULL;
        }
    }
    return NULL;
}

uint32_t vregion_total_committed(vregion_list_t *vl)
{
    uint32_t total = 0;
    for (vregion_t *c = vl->head; c != NULL; c = c->next)
    {
        total += c->committed;
    }
    return total;
}
