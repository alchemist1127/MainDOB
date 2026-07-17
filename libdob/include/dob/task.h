#ifndef DOB_TASK_H
#define DOB_TASK_H

/* ABI di SYS_TASK_SNAPSHOT — copia IDENTICA di kernel/proc/tasksnap.h
 * (se tocchi qui, tocchi la'; il kernel inchioda il layout con assert
 * statici). Il kernel espone CONTATORI MONOTONI (ns di CPU per
 * processo, ns di ozio per core, l'adesso): le percentuali le fa il
 * chiamante col delta tra due poll. */

#include <dob/types.h>
#include <sys/syscall.h>

#define DOB_TASKSNAP_VERSION   1u
#define DOB_TASKSNAP_MAX_CPUS  8u
#define DOB_TASKSNAP_MAX_ROWS  64u
#define DOB_TASKSNAP_NAME_LEN  24u

#define DOB_TASK_STATE_ALIVE   1u
#define DOB_TASK_STATE_ZOMBIE  2u
#define DOB_TASK_STATE_OTHER   0u

typedef struct
{
    uint32_t version;
    uint32_t ncpu;
    uint64_t now_ns;
    uint64_t cpu_idle_ns[DOB_TASKSNAP_MAX_CPUS];
    uint32_t ram_total_mb;
    uint32_t ram_free_mb;
    uint32_t nproc;
    uint32_t dma_free_frames;           /* frame liberi in zona DMA <16MB*/
    uint32_t dma_slots;                 /* (slot usati << 16) | massimo  */
} dob_tasksnap_hdr_t;

typedef struct
{
    int32_t  pid;                       /* 0 = kernel (aggregato)        */
    uint8_t  state;
    uint8_t  home_cpu;
    uint8_t  pinned;
    uint8_t  nthreads;
    uint8_t  priority;                  /* migliore fra i thread (0=alta)*/
    uint8_t  _pad[3];
    uint32_t mem_pages;                 /* RAM, senza aperture device    */
    uint32_t dev_pages;                 /* aperture MMIO/BAR             */
    uint32_t dma_pages;                 /* buffer DMA tracciati          */
    uint64_t cpu_ns;
    char     name[DOB_TASKSNAP_NAME_LEN];
} dob_tasksnap_row_t;

typedef struct
{
    dob_tasksnap_hdr_t hdr;
    dob_tasksnap_row_t rows[DOB_TASKSNAP_MAX_ROWS];
} dob_tasksnap_t;

/* Ritorna il numero di righe (>=0) o -1. */
static inline int dob_task_snapshot(dob_tasksnap_t *snap)
{
    return syscall2(SYS_TASK_SNAPSHOT, (int)snap, (int)sizeof(*snap));
}

#endif
