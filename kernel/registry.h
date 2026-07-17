#ifndef MAINDOB_REGISTRY_H
#define MAINDOB_REGISTRY_H

#include "lib/types.h"

#define REGISTRY_NAME_MAX 64
#define REGISTRY_MAX      256

void     registry_init(void);
int32_t  registry_register(const char *name, uint32_t port_id, int32_t owner_pid);
void     registry_unregister(const char *name, int32_t owner_pid);
uint32_t registry_find(const char *name);           /* 0 = non trovato    */
uint32_t registry_wait(const char *name, uint32_t timeout_ms);
void     registry_cleanup_owner(int32_t owner_pid);

/* Parcheggio needs: se `need_name` e' registrato sblocca subito `t`,
 * altrimenti lo tiene BLOCKED finche' il nome non appare. Il record e'
 * kmalloc'd (il thread parcheggiato non ha uno stack in esecuzione su
 * cui appoggiarlo) e liberato allo sblocco o alla morte del processo. */
struct thread;
void     registry_park_or_start(const char *need_name, struct thread *t);

#endif
