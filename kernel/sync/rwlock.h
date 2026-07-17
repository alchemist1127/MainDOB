#ifndef MAINDOB_SYNC_RWLOCK_H
#define MAINDOB_SYNC_RWLOCK_H

#include "lib/types.h"
#include "sync/spinlock.h"
#include "proc/wait.h"

/* Lock lettori/scrittore writer-preferring (vedi rwlock.c): con uno
 * scrittore in attesa i nuovi lettori si accodano — niente starvation
 * dello scrittore sotto flusso continuo di letture.
 *
 * readers: >0 = n lettori dentro, 0 = libero, -1 = scrittore dentro.
 * Contesto thread, puo' bloccare: MAI da IRQ. */

typedef struct
{
    spinlock_t   guard;
    int32_t      readers;           /* -1 scrittore, 0 libero, >0 lettori */
    uint32_t     writers_waiting;
    wait_queue_t read_waiters;
    wait_queue_t write_waiters;
} rwlock_t;

void rwlock_init(rwlock_t *rw);

void rwlock_read_lock(rwlock_t *rw);
void rwlock_read_unlock(rwlock_t *rw);

void rwlock_write_lock(rwlock_t *rw);
void rwlock_write_unlock(rwlock_t *rw);

#endif /* MAINDOB_SYNC_RWLOCK_H */
