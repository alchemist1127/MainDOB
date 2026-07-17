#ifndef MAINDOB_MM_PAGING_H
#define MAINDOB_MM_PAGING_H

#include "lib/types.h"

#define PTE_PRESENT         (1u << 0)
#define PTE_WRITABLE        (1u << 1)
#define PTE_USER            (1u << 2)
#define PTE_CACHE_DISABLE   (1u << 4)
#define PTE_GLOBAL          (1u << 8)

#define PAGE_DIR_INDEX(v)   (((uint32_t)(v)) >> 22)
#define PAGE_TABLE_INDEX(v) ((((uint32_t)(v)) >> 12) & 0x3FFu)
#define PTE_FRAME(e)        ((e) & 0xFFFFF000u)

#define KERNEL_PD_INDEX     768u        /* 0xC0000000 >> 22               */
#define RECURSIVE_PD_INDEX  1023u

void     paging_init(void);
void     paging_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
void     paging_unmap_page(uint32_t virt);
/* Anti-residuo: libera le page table UTENTE svuotate nel range (vedi
 * paging.c). Il chiamante fa il proprio shootdown dopo. -> byte resi. */
uint32_t paging_release_empty_user_tables(uint32_t start, uint32_t end);
uint32_t paging_get_physical(uint32_t virt, uint32_t *out_flags);
uint32_t paging_kernel_directory(void);
uint32_t paging_current_directory(void);
void     paging_switch_directory(uint32_t pd_phys);
uint32_t paging_create_directory(void);         /* condivide le PDE kernel */
void     paging_destroy_directory(uint32_t pd_phys);
void     paging_map_in(uint32_t pd_phys, uint32_t virt, uint32_t phys,
                       uint32_t flags);
void     paging_unmap_in(uint32_t pd_phys, uint32_t virt);
uint32_t paging_get_physical_in(uint32_t pd_phys, uint32_t virt);
bool     paging_selftest(void);
/* Estende il direct-map kernel (phys -> KERNEL_VMA+phys) fino a
 * phys_end. Idempotente. Limite: 256 MB (poi inizia il kheap). */
bool     paging_ensure_direct_map(uint32_t phys_end);

/* Tetto del direct-map kernel: il piu' alto indirizzo fisico
 * raggiungibile via KERNEL_VMA+phys. Il PMM lo consulta per non
 * distribuire mai un frame irraggiungibile (invariante: ogni frame
 * allocabile e' direct-map-reachable). E' una costante di policy, quindi
 * valido anche PRIMA di paging_init (il PMM parte per primo). Il
 * direct-map appartiene a paging: paging pubblica il proprio limite. */
uint32_t paging_direct_map_limit(void);

#endif
