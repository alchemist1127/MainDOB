/* MainDOB IRQ Discovery — reservation table for cooperative IRQ sharing.
 *
 * Driver flow when its default IRQ line is taken:
 *   1. irq_find_free()           → granted with a short lease
 *   2. driver does its hardware setup (irq_wire_device)
 *   3. irq_register() on the granted line → promotes lease to
 *      permanent ownership.
 *
 * No timers: the lease timeout is checked lazily on the next
 * find_free call. Idle cost zero.
 */

#ifndef MAINDOB_IRQ_DISCOVERY_H
#define MAINDOB_IRQ_DISCOVERY_H

#include "lib/types.h"

#define IRQ_DISCOVERY_LINES          16
#define IRQ_RESERVATION_TIMEOUT_MS   2000

/* Initialize the discovery table. Marks kernel-owned legacy IRQs as
 * permanently taken so they can never be handed out to userspace drivers. */
void irq_discovery_init(void);

/* Returns a free line, reserved for 'caller' with a 2-second lease.
 * Idempotent: if 'caller' already holds a non-expired reservation,
 * the same line is returned with its lease refreshed.
 * Returns -1 if all lines are taken. */
int irq_discovery_find_free(pid_t caller);

/* Promote a reservation to permanent ownership. Called from sys_irq_register
 * upon successful registration, regardless of whether the line came from
 * find_free or was a direct-hit. */
void irq_discovery_promote(uint8_t line, pid_t owner);

/* Release all reservations and ownerships held by a dead process. Called
 * from irq_cleanup when a driver process terminates. */
void irq_discovery_release_for_pid(pid_t pid);

#endif /* MAINDOB_IRQ_DISCOVERY_H */
