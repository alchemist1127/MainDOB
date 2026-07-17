/* MainDOB IRQ Discovery — implementation.
 *
 * reservations[] slot states:
 *   holder == 0                     : free
 *   holder != 0, expires_at != 0    : temporary lease (auto-expires)
 *   holder != 0, expires_at == 0    : permanent (released only on
 *                                     process death)
 *
 * Reservations are a soft claim: if the reserver fails to call
 * irq_register() within the lease window, the slot becomes available
 * again to find_free().
 *
 * Kernel-managed IRQs (timer, keyboard, cascade, RTC) are seeded as
 * permanently owned by PID 0xFFFF at init time so they stay out of
 * the cooperative pool.
 */

#include "irq/discovery.h"
#include "time/clock.h"
#include "console/console.h"

typedef struct
{
    pid_t    holder;       /* 0 = free */
    uint64_t expires_at;   /* ms ASSOLUTI A 64 BIT; 0 = permanente.
                            * NON u32: era il bug dei 49,7 giorni di
                            * Windows 95 (GetTickCount) riprodotto qui —
                            * il troncamento di clock_now_ms a 32 bit
                            * wrappava a 2^32 ms e i confronti assoluti
                            * si invertivano: lease inespirabili per
                            * altri 49 giorni, o peggio expires_at
                            * wrappato a 0 = lease PERMANENTE per caso.
                            * In u64 il wrap e' a 584 milioni di anni,
                            * lo stesso confine di units.h; il sentinella
                            * 0 resta irraggiungibile per somma. */
} irq_reservation_t;

static irq_reservation_t reservations[IRQ_DISCOVERY_LINES];

/* Internal helpers */

static bool
is_expired(const irq_reservation_t *r, uint64_t now)
{
    /* Permanent reservations (expires_at == 0) never expire. */
    if (r->expires_at == 0) return false;
    return now >= r->expires_at;
}

static bool
is_free(const irq_reservation_t *r, uint64_t now)
{
    if (r->holder == 0) return true;
    return is_expired(r, now);
}

/* Public API */

void
irq_discovery_init(void)
{
    for (int i = 0; i < IRQ_DISCOVERY_LINES; i++)
    {
        reservations[i].holder     = 0;
        reservations[i].expires_at = 0;
    }

    /* Seed the legacy lines owned by the kernel itself (no PID owns them,
     * so we use PID 0 as the kernel sentinel). These are managed by
     * irq_register_handler() directly — not through sys_irq_register —
     * so they would otherwise appear "free" to our table.
     *   0  = PIT timer
     *   1  = keyboard (PS/2)
     *   2  = cascade (physically unusable)
     *   8  = RTC
     */
    static const uint8_t legacy_lines[] = { 0, 1, 2, 8 };
    for (unsigned i = 0; i < sizeof(legacy_lines); i++)
    {
        uint8_t line = legacy_lines[i];
        reservations[line].holder     = 0xFFFF;   /* kernel sentinel */
        reservations[line].expires_at = 0;
    }

    kprintf("[IRQ]  Discovery table initialized (legacy lines reserved).\n");
}

int
irq_discovery_find_free(pid_t caller)
{
    if (caller == 0) return -1;

    uint64_t now = clock_now_ms();  /* MAI troncare: vedi expires_at */

    /* Idempotency: if caller already has a live reservation, refresh it
     * and hand back the same line. */
    for (int i = 0; i < IRQ_DISCOVERY_LINES; i++)
    {
        if (reservations[i].holder == caller && !is_expired(&reservations[i], now))
        {
            reservations[i].expires_at = now + IRQ_RESERVATION_TIMEOUT_MS;
            return i;
        }
    }

    /* Scan for the lowest-numbered free slot. Lines 3..15 are the normal
     * PCI range; we avoid 0,1,2,8 (seeded as permanent). */
    for (int i = 3; i < IRQ_DISCOVERY_LINES; i++)
    {
        if (is_free(&reservations[i], now))
        {
            reservations[i].holder     = caller;
            reservations[i].expires_at = now + IRQ_RESERVATION_TIMEOUT_MS;
            return i;
        }
    }

    return -1;
}

void
irq_discovery_promote(uint8_t line, pid_t owner)
{
    if (line >= IRQ_DISCOVERY_LINES) return;

    /* Mark as permanently owned by this PID. Overwrites any prior state
     * (the caller of sys_irq_register has already passed the ownership
     * check, so we trust its decision). */
    reservations[line].holder     = owner;
    reservations[line].expires_at = 0;
}

void
irq_discovery_release_for_pid(pid_t pid)
{
    if (pid == 0) return;

    for (int i = 0; i < IRQ_DISCOVERY_LINES; i++)
    {
        if (reservations[i].holder == pid)
        {
            reservations[i].holder     = 0;
            reservations[i].expires_at = 0;
        }
    }
}
