#ifndef MAINDOB_PROC_ELF_H
#define MAINDOB_PROC_ELF_H

#include "lib/types.h"
#include "proc/process.h"

typedef struct
{
    bool     success;
    uint32_t entry_point;
    uint32_t brk;               /* primo indirizzo libero dopo i segmenti */
} elf_load_result_t;

/* Carica i PT_LOAD nel PD di `proc`. NON tiene interrupt disabilitati
 * durante la copia dei segmenti (D6): la copia avviene tramite
 * paging_map_in, che commuta CR3 solo per la singola PTE. */
elf_load_result_t elf_load(process_t *proc, const void *data, uint32_t size);

/* Carica un oggetto condiviso .mem (ET_DYN, solo R_386_RELATIVE) nel PD
 * di proc, in un range dedicato (0x60000000..0x6FFFFFFF, condiviso con
 * SHM tramite lo stesso vregion_alloc: non collidono). Risolve
 * __mem_exports / __mem_init per offset. Nessun unload: le pagine
 * vivono quanto il processo. */
typedef struct
{
    uint32_t base;
    uint32_t exports_offset;
    uint32_t init_offset;
    bool     success;
} elf_shared_result_t;

elf_shared_result_t elf_load_shared(process_t *proc, const void *data,
                                    uint32_t size);

#endif
