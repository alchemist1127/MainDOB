#ifndef MAINDOB_SYNC_EVENT_GROUP_H
#define MAINDOB_SYNC_EVENT_GROUP_H

#include "lib/types.h"
#include "lib/list.h"
#include "sync/spinlock.h"
#include "proc/wait.h"

/* Event group: bitmask a 32 bit di condizioni con attese combinatorie
 * (i valori di modi e op sono ABI delle SYS_EVENT_*).
 *
 * Modi di attesa:
 *   RT_ALL_SET   — tutti i bit del pattern a 1
 *   RT_ANY_SET   — almeno un bit del pattern a 1
 *   RT_ALL_CLEAR — tutti i bit del pattern a 0
 *   RT_ANY_CLEAR — almeno un bit del pattern a 0
 *
 * Operazioni:
 *   set    — OR dei bit, sveglia i waiter che ora combaciano
 *   clear  — AND NOT dei bit, idem
 *   pulse  — set + wake + clear atomico (trigger a fronte)
 *   poison — gruppo morto, tutti svegli con WAIT_CANCELLED
 *   reset  — flags a 0, poison rimosso
 *
 * Il record del waiter (pattern, modo, snapshot risultato) vive sullo
 * stack del waiter, agganciato alla lista del gruppo — come il
 * pending_reply IPC. Il modulo e' autocontenuto: thread_t non sa nulla
 * degli eventi. */

#define RT_ALL_SET      0
#define RT_ANY_SET      1
#define RT_ALL_CLEAR    2
#define RT_ANY_CLEAR    3

#define EVENT_OP_SET    0
#define EVENT_OP_CLEAR  1
#define EVENT_OP_PULSE  2
#define EVENT_OP_POISON 3
#define EVENT_OP_RESET  4

#define MAX_EVENT_GROUPS 32

void     event_group_init_subsystem(void);

/* Ciclo di vita. create: gid >= 0, o -1 se pieni. */
int32_t  event_group_create(pid_t owner);
void     event_group_destroy(int32_t gid);
void     event_group_cleanup_process(pid_t pid);

/* Modifica flags + wake dei waiter combacianti. 0 o -1. */
int32_t  event_group_set(int32_t gid, uint32_t bits);
int32_t  event_group_clear(int32_t gid, uint32_t bits);
int32_t  event_group_pulse(int32_t gid, uint32_t bits);
int32_t  event_group_poison(int32_t gid);
int32_t  event_group_reset(int32_t gid);

/* Lettura non bloccante. */
uint32_t event_group_get_flags(int32_t gid);

/* Attesa bloccante con timeout opzionale (0 = per sempre). out_flags
 * riceve lo snapshot dei flag al risveglio. Ritorna WAIT_OK,
 * WAIT_TIMEOUT o WAIT_CANCELLED (proc/wait.h). */
int      event_group_wait(int32_t gid, uint32_t pattern, uint8_t mode,
                          uint32_t timeout_ms, uint32_t *out_flags);

#endif /* MAINDOB_SYNC_EVENT_GROUP_H */
