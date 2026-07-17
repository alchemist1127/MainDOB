#ifndef MAINDOB_MM_SHM_H
#define MAINDOB_MM_SHM_H

#include "lib/types.h"
#include "proc/process.h"

void    shm_init(void);
/* Crea un segmento e lo mappa nel creatore. Ritorna id (>0) o -1;
 * scrive il vaddr del creatore in *out_vaddr. */
int32_t shm_create(uint32_t size, process_t *creator, uint32_t *out_vaddr);
int32_t shm_map(int32_t id, process_t *proc, uint32_t *out_vaddr);
int32_t shm_unmap(int32_t id, process_t *proc);
void    shm_cleanup_process(process_t *proc);

#endif
