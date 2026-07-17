#ifndef MAINDOB_MM_VREGION_H
#define MAINDOB_MM_VREGION_H

#include "lib/types.h"

/* Tracciamento regioni virtuali per-address-space. Nessun lock
 * interno: l'esclusione e' del chiamante, che tiene process->vm_lock
 * per l'intera sequenza composta (find + uso del puntatore incluso). */

#define VREG_READ       (1u << 0)
#define VREG_WRITE      (1u << 1)
#define VREG_EXEC       (1u << 2)
#define VREG_USER       (1u << 3)
#define VREG_CODE       (1u << 8)
#define VREG_HEAP       (1u << 10)
#define VREG_STACK      (1u << 11)
#define VREG_MMAP       (1u << 12)
#define VREG_SHARED     (1u << 13)
#define VREG_DMA        (1u << 14)  /* buffer DMA contiguo (driver)   */
#define VREG_DEVICE     (1u << 15)
#define VREG_IPC        (1u << 16)
#define VREG_FIXED      (1u << 17)
#define VREG_USER_RW    (VREG_READ | VREG_WRITE | VREG_USER)

typedef struct vregion
{
    uint32_t        base;
    uint32_t        pages;
    uint32_t        flags;
    int32_t         backing_id;     /* es. id SHM, -1 = nessuno           */
    uint32_t        committed;
    struct vregion *next;
    struct vregion *prev;
} vregion_t;

typedef struct
{
    vregion_t *head;
    vregion_t *tail;
    uint32_t   count;
    /* Pagine totali di tutte le regioni. Mantenuto dai verbi di
     * mutazione (insert/remove/resize, gia' sotto il vm_lock del
     * chiamante); letto SENZA lock dallo snapshot del task manager:
     * u32 allineata, mai strappata su i386 — istantanea advisory. */
    uint32_t   total_pages;
    /* Sottoinsieme di total_pages con VREG_DEVICE: aperture MMIO/BAR
     * mappate (VRAM, registri) — memoria DEL DISPOSITIVO, zero frame
     * fisici. Contarle come RAM gonfiava di 16 MB la colonna memoria
     * del driver video nel task manager. */
    uint32_t   device_pages;
} vregion_list_t;

void       vregion_init(vregion_list_t *vl);
void       vregion_destroy(vregion_list_t *vl);
uint32_t   vregion_alloc(vregion_list_t *vl, uint32_t pages,
                         uint32_t min_addr, uint32_t max_addr,
                         uint32_t flags, int32_t backing_id);
vregion_t *vregion_insert(vregion_list_t *vl, uint32_t base,
                          uint32_t pages, uint32_t flags);
uint32_t   vregion_free(vregion_list_t *vl, uint32_t base);
bool       vregion_resize(vregion_list_t *vl, uint32_t base,
                          uint32_t new_pages);
vregion_t *vregion_find(vregion_list_t *vl, uint32_t addr);
vregion_t *vregion_find_exact(vregion_list_t *vl, uint32_t base);
uint32_t   vregion_total_committed(vregion_list_t *vl);

#endif
