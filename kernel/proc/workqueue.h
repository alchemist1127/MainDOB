#ifndef MAINDOB_PROC_WORKQUEUE_H
#define MAINDOB_PROC_WORKQUEUE_H

#include "lib/types.h"

/* Coda di lavoro differito: gli IRQ accodano, idle esegue in contesto
 * thread (dove i lock si possono prendere e si puo' dormire). */

typedef void (*work_fn_t)(void *arg);

void workqueue_init(void);
bool workqueue_add(work_fn_t fn, void *arg);    /* false = coda piena     */
bool workqueue_process_one(void);

#endif
