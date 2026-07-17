#ifndef PROC_TASKSNAP_H
#define PROC_TASKSNAP_H

/* ABI di SYS_TASK_SNAPSHOT (118) — CONDIVISA con l'userspace
 * (libdob/include/dob/task.h ne tiene una copia identica: se tocchi
 * qui, tocchi la'; gli assert statici in fondo inchiodano il layout).
 *
 * Chiamata: ebx = buffer utente, ecx = capienza in byte.
 * Il kernel scrive un header seguito da hdr.nproc righe e ritorna il
 * numero di righe (>=0) o -1. Righe oltre la capienza: troncate, mai
 * un overflow — il chiamante dimensiona per DOB_TASKSNAP_MAX_ROWS.
 *
 * Percentuali: il kernel NON le calcola. Espone contatori monotoni
 * (ns di CPU per processo, ns di ozio per core, l'adesso) e il task
 * manager fa i delta tra due poll: e' l'unico modo onesto di misurare
 * "uso" su una finestra scelta dal display, non dal kernel. */

#include "lib/types.h"

#define DOB_TASKSNAP_VERSION   1u
#define DOB_TASKSNAP_MAX_CPUS  8u      /* ABI: core riportati (>= reali) */
#define DOB_TASKSNAP_MAX_ROWS  64u
#define DOB_TASKSNAP_NAME_LEN  24u

/* Stati processo esposti (mappa di PROC_*: valori ABI, non interni). */
#define DOB_TASK_STATE_ALIVE   1u
#define DOB_TASK_STATE_ZOMBIE  2u
#define DOB_TASK_STATE_OTHER   0u

typedef struct
{
    uint32_t version;                   /* DOB_TASKSNAP_VERSION          */
    uint32_t ncpu;                      /* core online (<= MAX_CPUS ABI) */
    uint64_t now_ns;                    /* clock monotonico allo scatto  */
    uint64_t cpu_idle_ns[DOB_TASKSNAP_MAX_CPUS]; /* ozio accumulato      */
    uint32_t ram_total_mb;
    uint32_t ram_free_mb;
    uint32_t nproc;                     /* righe che seguono             */
    uint32_t dma_free_frames;           /* frame liberi in zona DMA <16MB*/
    uint32_t dma_slots;                 /* (slot usati << 16) | massimo  */
} dob_tasksnap_hdr_t;

typedef struct
{
    int32_t  pid;                       /* 0 = kernel (aggregato)        */
    uint8_t  state;                     /* DOB_TASK_STATE_*              */
    uint8_t  home_cpu;
    uint8_t  pinned;                    /* driver IRQ / driver video     */
    uint8_t  nthreads;                  /* vivi (saturato a 255)         */
    uint8_t  priority;                  /* migliore fra i thread (0=alta)*/
    uint8_t  _pad[3];
    uint32_t mem_pages;                 /* pagine RAM (advisory, senza
                                         * le aperture device)           */
    uint32_t dev_pages;                 /* aperture MMIO/BAR (VRAM...)   */
    uint32_t dma_pages;                 /* buffer DMA tracciati          */
    uint64_t cpu_ns;                    /* vivi + morti, monotono        */
    char     name[DOB_TASKSNAP_NAME_LEN];
} dob_tasksnap_row_t;

_Static_assert(sizeof(dob_tasksnap_hdr_t) == 100,
               "tasksnap: header ABI derivato dall'userspace");
_Static_assert(sizeof(dob_tasksnap_row_t) == 56,
               "tasksnap: riga ABI derivata dall'userspace");

/* Collettore (thread.c): riempie fino a `max` righe sotto il
 * lifecycle-lock e ritorna quante. Aggrega per processo camminando i
 * THREAD (mai la proclist: nessun nesting nuovo di lock). */
int task_snapshot_collect(dob_tasksnap_row_t *rows, uint32_t max);

/* Renice per PID (0..3, 0=alta). Ritorna 0 o -1. */
int thread_renice_by_pid(pid_t pid, uint32_t prio);

/* Misura del pool DMA (syscall/driver.c, sotto il proprio lock —
 * chiamare FUORI dal lifecycle-lock). */
uint32_t dma_track_pages_of(pid_t pid);
uint32_t dma_track_slot_pressure(void);

#endif
