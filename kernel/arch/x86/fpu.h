#ifndef MAINDOB_ARCH_FPU_H
#define MAINDOB_ARCH_FPU_H

#include "lib/types.h"

/* FPU/SSE EAGER (impianto osservato dal 1.0, che era corretto):
 * niente lazy-switch via CR0.TS — su un microkernel con IPC frequente
 * il #NM per-switch costa piu' del fxsave incondizionato, ed elimina
 * un'intera classe di bug (stato FP condiviso tra thread = corruzione
 * silenziosa dei calcoli). Floor architetturale: Pentium III (FXSR+SSE)
 * — entrambe le macchine target lo superano. Se manca: fail loud. */

#define FPU_AREA_SIZE   512u
#define FPU_AREA_ALIGN  16u

void fpu_init_this_cpu(void);           /* BSP ora; ogni AP al 1.1.3      */
void fpu_seed_thread_area(void *area);  /* immagine canonica pulita       */

static inline void fpu_save(void *area)
{
    __asm__ volatile ("fxsave (%0)" : : "r"(area) : "memory");
}

static inline void fpu_restore(const void *area)
{
    __asm__ volatile ("fxrstor (%0)" : : "r"(area) : "memory");
}

#endif
