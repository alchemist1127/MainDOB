#ifndef MAINDOB_SYNC_MUTEX_H
#define MAINDOB_SYNC_MUTEX_H

#include "lib/types.h"
#include "sync/spinlock.h"
#include "proc/wait.h"

struct thread;

typedef struct mutex
{
    spinlock_t   guard;
    uint32_t     locked;
    int32_t      owner_tid;
    uint32_t     boost_prio;    /* piu' alta richiesta dai waiter         */
    wait_queue_t waiters;
} mutex_t;

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
int  mutex_lock_timeout(mutex_t *m, uint32_t timeout_ms);
bool mutex_trylock(mutex_t *m);
void mutex_unlock(mutex_t *m);

/* Rilascio orfani al reap di un thread morto. */
void mutex_release_all_owned(struct thread *t);

#endif
