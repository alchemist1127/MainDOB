#ifndef MAINDOB_SYNC_SEMAPHORE_H
#define MAINDOB_SYNC_SEMAPHORE_H

#include "lib/types.h"
#include "sync/spinlock.h"
#include "proc/wait.h"

/* Semaforo contatore classico (vedi semaphore.c). Contesto thread,
 * wait puo' bloccare: MAI da IRQ. signal e' sicura ovunque (wake via
 * wait-queue, nessuna attesa). */

typedef struct
{
    spinlock_t   guard;
    int32_t      count;
    wait_queue_t waiters;
} semaphore_t;

void semaphore_init(semaphore_t *s, int32_t initial);

/* Decrementa; blocca finche' count > 0. */
void semaphore_wait(semaphore_t *s);

/* Come wait ma con timeout: WAIT_OK o WAIT_TIMEOUT (proc/wait.h). */
int  semaphore_wait_timeout(semaphore_t *s, uint32_t timeout_ms);

/* Tenta senza bloccare: true se preso. */
bool semaphore_trywait(semaphore_t *s);

/* Incrementa e sveglia un attendente. */
void semaphore_signal(semaphore_t *s);

#endif /* MAINDOB_SYNC_SEMAPHORE_H */
