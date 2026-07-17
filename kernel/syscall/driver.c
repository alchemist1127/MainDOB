#include "syscall/syscall.h"
#include "proc/process.h"
#include "proc/thread.h"
#include "proc/scheduler.h"
#include "proc/workqueue.h"
#include "proc/wait.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/vregion.h"
#include "mm/dma_pool.h"
#include "ipc/channel.h"
#include "time/timer.h"
#include "time/units.h"
#include "arch/x86/irq.h"
#include "arch/x86/ioapic.h"
#include "arch/x86/intr.h"
#include "arch/x86/isr.h"
#include "arch/x86/ports.h"
#include "arch/x86/pci_cfg.h"
#include "arch/x86/tlb.h"
#include "arch/x86/cpu.h"
#include "sync/spinlock.h"
#include "lib/string.h"
#include "lib/format.h"
#include "console/console.h"
#include "kernel.h"
#include "syscall/video_boomerang.h"
#include "irq/discovery.h"
#include "irq/pirq.h"
#include "arch/x86/idt.h"
#include "arch/x86/gdt.h"

/* Layer syscall driver-hardware (SYS 50..67, 97, 100..111).
 *
 * Prima ondata: tutto cio' che serve ai driver su consegna PIC — I/O
 * port, forwarding IRQ in userspace (linee condivise, unmask
 * collettivo, timer di sicurezza), MMIO, DMA tracciata, claim
 * dispositivi, timer asincroni, watchdog, sezioni critiche, PCI config
 * legacy. I verbi di mascheramento sono gia' biforcati PIC/IOAPIC: la
 * migrazione della consegna (blocco PCI) non tocchera' questo file.
 *
 * Seconda ondata (con la migrazione IOAPIC): risoluzione empirica
 * INTx->GSI (SYS_IRQ_REGISTER_PCI/PCI_CLAIM/FIND_FREE/WIRE_DEVICE),
 * MSI, e il boomerang video (SYS_REGISTER_VIDEO_DRIVER). Qui rispondono
 * -1 come su hardware senza IOAPIC: ABI coerente, mai un buco. */

/* I verbi di mascheramento/steering delle linee sono del controller
 * unificato (arch/x86/intr.c): intr_line_mask/unmask risolvono la
 * linea sull'8259 o sulla voce IOAPIC secondo il modo di consegna
 * attivo, e intr_route_line_to_cpu fa lo steering per-CPU. */

/* ======================================================================
 * Forwarding IRQ in userspace
 *
 * Un driver si registra su una linea con la porta da notificare; al
 * fire il kernel maschera (level-triggered: senza mask la linea
 * ritornerebbe subito, l'EOI e' gia' partito), notifica OGNI sharer, e
 * riabilita solo quando l'ultimo ha riportato irq_done (unmask
 * collettivo). La contabilita' dei done e' PER-SHARER (done_this_fire):
 * un doppio report non salda il debito di un vicino. Un timer di
 * sicurezza (backstop di sola liveness) sblocca la linea se un driver
 * non riporta: linea mai incastrata, il muto finisce nel log CON il suo
 * PID. NON sgancia nessuno: un driver e' dichiarato morto solo dal
 * reaper (irq_cleanup), mai da una scadenza mancata — un lento vivo
 * tiene la sua linea.
 *
 * DISCOVERY SU LINEE CONDIVISE: i fire delle linee legate (GSI >= 16)
 * vengono notificati anche ai pending della risoluzione INTx — su
 * ICH-class il GSI di un device in risoluzione puo' coincidere con uno
 * gia' legato, e senza il piggyback quel device non risolverebbe mai.
 * Un probe assertito senza claim entro PROBE_CLAIM_TIMEOUT_MS non viene
 * parcheggiato finche' esistono pending: entra in backoff
 * (PROBE_BACKOFF_MS) e riapre — il park definitivo resta solo per gli
 * assert orfani a risoluzioni concluse.
 *
 * REGOLA: ipc_notify puo' svegliare (e wake-preemptare) — MAI sotto
 * s_irqfwd_lock: snapshot delle porte dentro, notify fuori.
 * Ordine lock: s_irqfwd_lock -> lock timer (arm/cancel); il layer
 * timer non prende mai questo lock: aciclico.
 * ==================================================================== */

#define MAX_IRQ_FORWARD      32u
#define MAX_SHARERS_PER_LINE 8
#define IRQ_DONE_TIMEOUT_MS  50u

/* SYS_IRQ_DONE with ecx == IRQ_DONE_MINE means "this IRQ was MY device":
 * the source is serviced, so the round closes at once instead of waiting
 * on the other sharers. A distinctive 32-bit sentinel, not a bool: a
 * legacy caller that leaves stale garbage in ecx will (bar a 1-in-2^32
 * collision) not trip it and keeps the wait-for-all semantics. */
#define IRQ_DONE_MINE        0x4D494E45u   /* 'MINE' */

/* La finestra di claim della discovery e' SEPARATA dal timeout di
 * irq_done: il claimant deve svegliarsi, leggere lo status del proprio
 * device e chiamare SYS_IRQ_PCI_CLAIM — sotto carico di boot i 50 ms
 * del protocollo done sono troppo stretti e perdere la finestra
 * significava (prima) parcheggiare il GSI per sempre. */
#define PROBE_CLAIM_TIMEOUT_MS 250u

/* Backoff di un probe assertito senza claimant MENTRE una risoluzione
 * e' in corso: la linea resta mascherata per questo intervallo e poi
 * riapre. Limita una sorgente level orfana a un fire per periodo senza
 * mai chiudere la porta alla discovery (il claimant tipico e' un
 * device hotplug che si annuncera' tra minuti, non millisecondi). */
#define PROBE_BACKOFF_MS     200u

typedef struct
{
    uint32_t notify_port;
    pid_t    owner_pid;
    bool     used;
    bool     done_this_fire;        /* report already in for this fire   */
} irq_sharer_t;

typedef struct
{
    irq_sharer_t sharers[MAX_SHARERS_PER_LINE];
    uint8_t      sharer_count;
    uint8_t      pending_done;      /* irq_done still owed this fire      */
    bool         masked;
    bool         probe_armed;       /* GSI armed for INTx discovery       */
    bool         probe_backoff;     /* probe masked: timer == reopen      */
    timer_t      safety_timer;
} irq_line_t;

static irq_line_t irq_lines[MAX_IRQ_FORWARD];
static spinlock_t s_irqfwd_lock = SPINLOCK_INIT;

/* Empirical INTx resolution state (defined further down): the forward
 * handler and the safety timer read it, so declare it here. */
#define MAX_PCI_PENDING  16
typedef struct
{
    bool     used;
    pid_t    pid;
    uint32_t notify_port;
    uint8_t  bus, slot, func;
} pci_pending_t;
static pci_pending_t s_pci_pending[MAX_PCI_PENDING];
static uint32_t      s_pci_pending_count;
static uint32_t      s_msi_notify_port[256];
static void          pci_discovery_disarm_unbound(void);

/* Orchestrators live at the bottom of the file (C forces callees-first),
 * so the executive blocks and the ISR/timer registration in
 * irq_attach_sharer can name them, these two are forward-declared. */
static void irq_forward_handler(isr_regs_t *regs);
static void irq_line_watchdog(void *arg);

/* ======================================================================
 * Executive blocks — one job each. Unless noted, the caller holds
 * s_irqfwd_lock; wakes always happen OUTSIDE it (snapshot in, notify out).
 * ==================================================================== */

/* Reset a sharer slot. Line-level fallout (mask on empty, unmask, timer)
 * stays with the caller, which knows the context. */
static void sharer_detach_slot(irq_line_t *line, int slot)
{
    line->sharers[slot].used           = false;
    line->sharers[slot].notify_port    = 0;
    line->sharers[slot].owner_pid      = 0;
    line->sharers[slot].done_this_fire = false;
    if (line->sharer_count > 0)
    {
        line->sharer_count--;
    }
}

/* Park a discovery GSI for good: mask + drop the gate. The line does not
 * exist again until a new SYS_IRQ_REGISTER_PCI re-arms it. */
static void probe_park(irq_line_t *line, uint32_t irq)
{
    ioapic_mask_gsi(irq);
    intr_clear_vector((uint8_t)(IRQ_BASE_VECTOR + irq));
    line->probe_armed   = false;
    line->probe_backoff = false;
    line->masked        = false;
    line->pending_done  = 0;
}

/* This pid's sharer slot on the line, or NULL. */
static irq_sharer_t *sharer_of(irq_line_t *line, pid_t pid)
{
    for (int s = 0; s < MAX_SHARERS_PER_LINE; s++)
    {
        if (line->sharers[s].used && line->sharers[s].owner_pid == pid)
        {
            return &line->sharers[s];
        }
    }
    return NULL;
}

/* Open a bound-line fire: mask, seed the done-accounting, collect the
 * sharer notify ports into `ports`. Returns how many were collected. */
static uint32_t line_open_fire(irq_line_t *line, uint32_t irq, uint32_t *ports)
{
    intr_line_mask((uint8_t)irq);
    line->masked       = true;
    line->pending_done = line->sharer_count;

    uint32_t n = 0;
    for (int i = 0; i < MAX_SHARERS_PER_LINE; i++)
    {
        if (!line->sharers[i].used)
        {
            continue;
        }
        line->sharers[i].done_this_fire = false;
        if (line->sharers[i].notify_port != 0)
        {
            ports[n++] = line->sharers[i].notify_port;
        }
    }
    return n;
}

/* Close a bound-line fire: cancel the backstop, unmask. */
static void line_close_fire(irq_line_t *line, uint32_t irq)
{
    timer_cancel(&line->safety_timer);
    line->masked = false;
    intr_line_unmask((uint8_t)irq);
}

/* Collect every pending discovery owner's notify port into `ports`.
 * Returns the count. */
static uint32_t pending_ports(uint32_t *ports)
{
    uint32_t n = 0;
    for (int i = 0; i < MAX_PCI_PENDING; i++)
    {
        if (s_pci_pending[i].used && s_pci_pending[i].notify_port != 0)
        {
            ports[n++] = s_pci_pending[i].notify_port;
        }
    }
    return n;
}

/* Wake `n` ports with this line's bit. Runs with the lock released. */
static void notify_ports(const uint32_t *ports, uint32_t n, uint32_t irq)
{
    for (uint32_t i = 0; i < n; i++)
    {
        ipc_notify(ports[i], (1u << irq));
    }
}

/* Bound-line fire. Mask, seed accounting, collect the notify ports
 * (sharers, plus any discovery listener overlapping this GSI: on
 * ICH-class PIRQs a resolving device can share a bound line, and without
 * this it would never hear its own assert), arm the backstop. If the
 * timer heap is empty, run the pass ungraced: unmask now and still
 * notify — a repeated notify is the worst case, never a wedge. Returns
 * the number of ports to wake. */
static uint32_t line_fire_bound(irq_line_t *line, uint32_t irq,
                                uint32_t *ports, bool *ungraced)
{
    uint32_t n = line_open_fire(line, irq, ports);

    if (irq >= 16 && s_pci_pending_count > 0)
    {
        n += pending_ports(ports + n);
    }

    if (!timer_arm_in(&line->safety_timer, MS_TO_NS(IRQ_DONE_TIMEOUT_MS)))
    {
        line->pending_done = 0;
        line->masked       = false;
        intr_line_unmask((uint8_t)irq);
        *ungraced = true;
    }
    return n;
}

/* Discovery fire: a GSI armed for resolution asserts while a driver is
 * still hunting its line. Mask, collect the pending owners, give them
 * the claim window (theirs — PROBE_CLAIM_TIMEOUT_MS — not the done
 * window: the claimant must wake, read hardware and bind). Heap empty ->
 * park now and wake nobody. Returns the ports to wake (0 if parked). */
static uint32_t line_fire_probe(irq_line_t *line, uint32_t irq,
                                uint32_t *ports, bool *parked)
{
    intr_line_mask((uint8_t)irq);
    line->masked = true;

    uint32_t n = pending_ports(ports);

    if (!timer_arm_in(&line->safety_timer, MS_TO_NS(PROBE_CLAIM_TIMEOUT_MS)))
    {
        probe_park(line, irq);
        *parked = true;
        return 0;
    }
    return n;
}

/* Attach `proc` as a sharer of the line (caller holds s_irqfwd_lock).
 * Factored: the second-wave INTx resolver reuses the same bind path. */
static int32_t irq_attach_sharer(uint32_t line_idx, process_t *proc,
                                 uint32_t notify_port_id)
{
    irq_line_t *line = &irq_lines[line_idx];

    /* Idempotent re-registration: refresh this pid's port. */
    for (int s = 0; s < MAX_SHARERS_PER_LINE; s++)
    {
        if (line->sharers[s].used && line->sharers[s].owner_pid == proc->pid)
        {
            line->sharers[s].notify_port = notify_port_id;
            intr_line_unmask((uint8_t)line_idx);
            return 0;
        }
    }

    int slot = -1;
    for (int s = 0; s < MAX_SHARERS_PER_LINE; s++)
    {
        if (!line->sharers[s].used)
        {
            slot = s;
            break;
        }
    }
    if (slot < 0)
    {
        kprintf("[IRQ ] line %u: sharer table full, PID %d refused\n",
                line_idx, proc->pid);
        return -1;
    }

    line->sharers[slot].used        = true;
    line->sharers[slot].owner_pid   = proc->pid;
    line->sharers[slot].notify_port = notify_port_id;
    /* Da qui il processo possiede (anche in condivisione) una linea
     * IRQ instradata sulla sua home: INCHIODATO — mai migrato dal
     * work stealing (vedi process.h, campo pinned). */
    proc->pinned = true;
    /* A fire in flight was never delivered to this sharer: it owes no
     * done and is not mute — it joins already square. */
    line->sharers[slot].done_this_fire = true;
    line->sharer_count++;

    if (line->sharer_count == 1)
    {
        timer_init(&line->safety_timer, irq_line_watchdog,
                   (void *)(uintptr_t)line_idx);
        irq_register_handler((uint8_t)line_idx, irq_forward_handler);
        /* So the IOAPIC migration finds this handler on the legacy line,
         * and steer to the driver's home core (no-op under PIC / UP):
         * the ISR and the IPC wake run where the driver lives. */
        intr_bridge_legacy_handler((uint8_t)line_idx, irq_forward_handler);
        intr_route_line_to_cpu((uint8_t)line_idx, proc->home_cpu);
        kprintf("[IRQ ] line %u -> driver '%s' (PID %d, CPU%u)\n",
                line_idx, proc->name, proc->pid, proc->home_cpu);
    }
    else
    {
        kprintf("[IRQ ] line %u: +sharer '%s' (PID %d, %u total)\n",
                line_idx, proc->name, proc->pid,
                (uint32_t)line->sharer_count);
    }

    irq_discovery_promote((uint8_t)line_idx, proc->pid);
    intr_line_unmask((uint8_t)line_idx);
    return 0;
}

/* ======================================================================
 * Orchestrators — the algorithm in the open, calling the blocks above.
 * ==================================================================== */

/* ISR entry, registered per line. A bound line runs a shared INTx round;
 * an armed discovery GSI runs the probe; anything else is a stray. The
 * discriminant is read under the lock, the wakes happen after it. */
static void irq_forward_handler(isr_regs_t *regs)
{
    uint32_t irq = regs->vector - IRQ_BASE_VECTOR;
    if (irq >= MAX_IRQ_FORWARD)
    {
        return;
    }

    uint32_t ports[MAX_SHARERS_PER_LINE + MAX_PCI_PENDING];
    uint32_t n = 0;
    bool     ungraced = false;
    bool     parked   = false;

    uint32_t lfl = spinlock_acquire_irqsave(&s_irqfwd_lock);
    irq_line_t *line = &irq_lines[irq];

    if (line->sharer_count > 0)
    {
        n = line_fire_bound(line, irq, ports, &ungraced);
    }
    else if (line->probe_armed && s_pci_pending_count > 0)
    {
        n = line_fire_probe(line, irq, ports, &parked);
    }
    spinlock_release_irqrestore(&s_irqfwd_lock, lfl);

    if (ungraced)
    {
        kprintf("[IRQ ] line %u: timer heap exhausted, ungraced pass "
                "(immediate unmask)\n", irq);
    }
    if (parked)
    {
        kprintf("[IRQ ] GSI %u: timer heap exhausted, probe parked\n", irq);
    }
    notify_ports(ports, n, irq);
}

/* Liveness backstop (IRQ context, timer callback). Three cases:
 *   1. discovery in backoff -> the timer IS the reopen;
 *   2. discovery assert whose claim window lapsed -> backoff if someone
 *      is still resolving (never park while a resolution runs, or one
 *      orphan assert at boot would kill the future claimant's hotplug),
 *      else park;
 *   3. bound line: not every sharer reported -> force the unmask so
 *      neighbours are not starved. It DETACHES NO ONE. A driver is
 *      declared dead only by the reaper (irq_cleanup), never by a missed
 *      deadline: a live-but-slow driver keeps its line. The unacked
 *      owners are named in the log, nothing more. */
static void irq_line_watchdog(void *arg)
{
    uint32_t irq = (uint32_t)(uintptr_t)arg;
    if (irq >= MAX_IRQ_FORWARD)
    {
        return;
    }

    uint32_t lfl = spinlock_acquire_irqsave(&s_irqfwd_lock);
    irq_line_t *line = &irq_lines[irq];
    if (!line->masked)
    {
        spinlock_release_irqrestore(&s_irqfwd_lock, lfl);
        return;
    }

    if (line->sharer_count == 0 && line->probe_armed)
    {
        if (line->probe_backoff)
        {
            line->probe_backoff = false;
            line->masked        = false;
            ioapic_unmask_gsi(irq);
            spinlock_release_irqrestore(&s_irqfwd_lock, lfl);
            return;
        }

        if (s_pci_pending_count > 0)
        {
            if (timer_arm_in(&line->safety_timer,
                             MS_TO_NS(PROBE_BACKOFF_MS)))
            {
                line->probe_backoff = true;
                spinlock_release_irqrestore(&s_irqfwd_lock, lfl);
                kprintf("[IRQ ] GSI %u: assert with no claim in %u ms, "
                        "backoff %u ms (%u driver resolving)\n", irq,
                        PROBE_CLAIM_TIMEOUT_MS, PROBE_BACKOFF_MS,
                        s_pci_pending_count);
                return;
            }
        }

        probe_park(line, irq);
        spinlock_release_irqrestore(&s_irqfwd_lock, lfl);
        kprintf("[IRQ ] GSI %u: assertion with no claimant, parked\n", irq);
        return;
    }

    /* Case 3: bound-line timeout. Name the mutes, force the unmask. */
    uint32_t reported = (uint32_t)(line->sharer_count - line->pending_done);
    uint32_t total    = line->sharer_count;

    char mute_list[96];
    int  ml = 0;
    mute_list[0] = '\0';
    for (int s = 0; s < MAX_SHARERS_PER_LINE; s++)
    {
        irq_sharer_t *sh = &line->sharers[s];
        if (!sh->used || sh->done_this_fire)
        {
            continue;
        }
        if (ml < (int)sizeof(mute_list) - 1)
        {
            ml += snprintf(mute_list + ml, sizeof(mute_list) - (size_t)ml,
                           " PID %d", sh->owner_pid);
            if (ml > (int)sizeof(mute_list) - 1)
            {
                ml = (int)sizeof(mute_list) - 1;
            }
        }
    }

    line->pending_done = 0;
    line->masked       = false;
    intr_line_unmask((uint8_t)irq);
    spinlock_release_irqrestore(&s_irqfwd_lock, lfl);

    kprintf("[IRQ ] line %u: irq_done timeout (%u of %u reported; "
            "mute:%s), forced unmask\n", irq, reported, total, mute_list);
}

/* A sharer reports this fire serviced (or that the IRQ was not its
 * device). Clear its debt; when the last debt clears, close the round
 * and unmask. A late or spurious done is a harmless no-op. Returns
 * whether the caller was a sharer of this line. */
/* A sharer reports it is done with this fire. `handled` means the IRQ
 * was ITS device: the source is serviced and the line will deassert, so
 * the round closes now instead of waiting on the other sharers — a fast
 * device is no longer taxed by a slow neighbour. If it was not the
 * source, the round closes when the last debt clears, as before. A
 * second source on the line simply re-fires (one clean round each). A
 * late or spurious done is a harmless no-op. Returns whether the caller
 * was a sharer of this line. */
static bool irq_line_ack(uint32_t irq, pid_t pid, bool handled)
{
    uint32_t lfl = spinlock_acquire_irqsave(&s_irqfwd_lock);
    irq_line_t *line = &irq_lines[irq];

    irq_sharer_t *me = sharer_of(line, pid);
    if (me == NULL)
    {
        spinlock_release_irqrestore(&s_irqfwd_lock, lfl);
        return false;
    }

    if (line->masked && line->pending_done > 0 && !me->done_this_fire)
    {
        me->done_this_fire = true;
        line->pending_done--;
        if (handled || line->pending_done == 0)
        {
            line_close_fire(line, irq);
        }
    }
    spinlock_release_irqrestore(&s_irqfwd_lock, lfl);
    return true;
}

static int32_t sys_irq_done(isr_regs_t *regs)
{
    uint32_t irq_num = regs->ebx;
    if (irq_num >= MAX_IRQ_FORWARD || !caller_is_driver())
    {
        return -1;
    }
    bool handled = (regs->ecx == IRQ_DONE_MINE);
    return irq_line_ack(irq_num, process_current()->pid, handled) ? 0 : -1;
}

static int32_t sys_irq_register(isr_regs_t *regs)
{
    uint32_t irq_num = regs->ebx;
    uint32_t notify_port_id = regs->ecx;

    if (irq_num >= MAX_IRQ_FORWARD || !caller_is_driver())
    {
        return -1;
    }

    process_t *proc = process_current();
    uint32_t lfl = spinlock_acquire_irqsave(&s_irqfwd_lock);
    int32_t r = irq_attach_sharer(irq_num, proc, notify_port_id);
    spinlock_release_irqrestore(&s_irqfwd_lock, lfl);
    return r;
}

/* Release every IRQ registration of the dead process: a crashed driver
 * must leave no line masked, and make the kernel post to no destroyed
 * port. This is the authoritative detach — not a missed deadline. */
static void irq_cleanup(pid_t pid)
{
    uint32_t lfl = spinlock_acquire_irqsave(&s_irqfwd_lock);

    /* PCI pending of this pid (died before claiming its GSI): if the set
     * empties, disarm the still-unbound GSIs. */
    bool removed_pending = false;
    for (int i = 0; i < MAX_PCI_PENDING; i++)
    {
        if (s_pci_pending[i].used && s_pci_pending[i].pid == pid)
        {
            s_pci_pending[i].used = false;
            if (s_pci_pending_count > 0)
            {
                s_pci_pending_count--;
            }
            removed_pending = true;
        }
    }
    if (removed_pending && s_pci_pending_count == 0)
    {
        pci_discovery_disarm_unbound();
    }

    for (uint32_t i = 0; i < MAX_IRQ_FORWARD; i++)
    {
        irq_line_t *line = &irq_lines[i];
        bool removed = false;

        for (int s = 0; s < MAX_SHARERS_PER_LINE; s++)
        {
            if (line->sharers[s].used && line->sharers[s].owner_pid == pid)
            {
                /* If the dead one still owed a done for the fire in
                 * flight, discount it — only if it truly did. */
                if (line->masked && line->pending_done > 0 &&
                    !line->sharers[s].done_this_fire)
                {
                    line->pending_done--;
                }
                sharer_detach_slot(line, s);
                removed = true;
                kprintf("[IRQ ] line %u: -sharer PID %d (teardown)\n", i, pid);
            }
        }
        if (!removed)
        {
            continue;
        }

        if (line->sharer_count == 0)
        {
            timer_cancel(&line->safety_timer);
            intr_line_mask((uint8_t)i);
            line->masked = false;
            line->pending_done = 0;
        }
        else if (line->masked && line->pending_done == 0)
        {
            timer_cancel(&line->safety_timer);
            line->masked = false;
            intr_line_unmask((uint8_t)i);
        }
    }
    irq_discovery_release_for_pid(pid);
    spinlock_release_irqrestore(&s_irqfwd_lock, lfl);

    /* MSI ports and allocatable vectors of the dead: clear the ports
     * BEFORE the vectors are reassigned (a stale port would notify a
     * dead owner), then free the vectors. */
    for (int v = 0x50; v <= 0xDF; v++)
    {
        if (intr_vector_owner((uint8_t)v) == pid)
        {
            s_msi_notify_port[v] = 0;
        }
    }
    intr_release_for_pid(pid);
}

/* ======================================================================
 * I/O port
 * ==================================================================== */

static int32_t sys_io_port_in(isr_regs_t *regs)
{
    uint16_t port = (uint16_t)regs->ebx;
    uint32_t size = regs->ecx;

    if (!caller_is_driver())
    {
        return -1;
    }
    switch (size)
    {
        case 1: return (int32_t)inb(port);
        case 2: return (int32_t)inw(port);
        case 4: return (int32_t)inl(port);
        default: return -1;
    }
}

static int32_t sys_io_port_out(isr_regs_t *regs)
{
    uint16_t port  = (uint16_t)regs->ebx;
    uint32_t size  = regs->ecx;
    uint32_t value = regs->edx;

    if (!caller_is_driver())
    {
        return -1;
    }
    switch (size)
    {
        case 1: outb(port, (uint8_t)value); return 0;
        case 2: outw(port, (uint16_t)value); return 0;
        case 4: outl(port, value); return 0;
        default: return -1;
    }
}

/* ======================================================================
 * MMIO in userspace (SYS_MMAP_PHYS)
 * ==================================================================== */

static int32_t sys_mmap_phys(isr_regs_t *regs)
{
    uint32_t phys_addr = regs->ebx;
    uint32_t size      = regs->ecx;

    process_t *proc = process_current();
    if (proc == NULL || !caller_is_driver() || size == 0)
    {
        return 0;
    }

    uint32_t offset = phys_addr & 0xFFFu;
    phys_addr &= ~0xFFFu;
    size = ALIGN_UP(size + offset, PAGE_SIZE);
    uint32_t pages = size / PAGE_SIZE;
    uint32_t phys_end = phys_addr + size;

    /* Deny-list: regioni fisiche sensibili. */
    extern uint32_t _kernel_phys_end;
    uint32_t kstart = 0x00100000u;
    uint32_t kend   = ALIGN_UP((uint32_t)&_kernel_phys_end, PAGE_SIZE);

    if (phys_addr == 0 || phys_addr < 0x1000u)
    {
        kprintf("[MMAP] Negato: PID %d su pagina 0/BIOS (0x%08x)\n",
                proc->pid, phys_addr);
        return 0;
    }
    if (phys_end > kstart && phys_addr < kend)
    {
        kprintf("[MMAP] Negato: PID %d su RAM kernel 0x%08x-0x%08x\n",
                proc->pid, phys_addr, phys_end);
        return 0;
    }
    if (pages > 4096u)
    {
        kprintf("[MMAP] Negato: PID %d chiede %u pagine (max 4096)\n",
                proc->pid, pages);
        return 0;
    }

    /* RAM allocata = heap/stack/page table altrui: mai mappabile.
     * Sopra la RAM e' MMIO (BAR PCI): sempre ok. Eccezione: VGA legacy. */
    uint32_t ram_top = pmm_get_ram_top();
    bool legacy_vga = (phys_addr >= 0xA0000u && phys_end <= 0xC0000u);
    if (phys_addr < ram_top && !legacy_vga)
    {
        for (uint32_t p = 0; p < pages; p++)
        {
            if (pmm_is_frame_allocated(phys_addr + p * PAGE_SIZE))
            {
                kprintf("[MMAP] Negato: PID %d su RAM allocata 0x%08x\n",
                        proc->pid, phys_addr + p * PAGE_SIZE);
                return 0;
            }
        }
    }

    mutex_lock(&proc->vm_lock);
    uint32_t virt = vregion_alloc(&proc->vm_regions, pages,
                                  0x80000000u, 0xBFFDFFFFu,
                                  VREG_DEVICE | VREG_USER_RW, -1);
    if (virt == 0)
    {
        mutex_unlock(&proc->vm_lock);
        return 0;
    }
    for (uint32_t i = 0; i < pages; i++)
    {
        /* PCD: MMIO non-cachable. */
        paging_map_page(virt + i * PAGE_SIZE, phys_addr + i * PAGE_SIZE,
                        PTE_PRESENT | PTE_WRITABLE | PTE_USER |
                        PTE_CACHE_DISABLE);
    }
    mutex_unlock(&proc->vm_lock);

    return (int32_t)(virt + offset);
}

/* ======================================================================
 * DMA tracciata
 *
 * Oltre a track/untrack, il pool e' MISURABILE (task manager): pagine
 * DMA per PID e pressione degli slot (64 totali: un driver che perde
 * buffer li esaurisce, e la tabella piena e' il sintomo n.1 di un leak
 * DMA). Accessori sotto s_dma_lock; chiamati FUORI dal lifecycle-lock
 * (sys_task_snapshot li interroga dopo il collettore): nessun nesting.
 * ==================================================================== */

#define DMA_TRACK_MAX 64

typedef struct
{
    bool     used;
    pid_t    owner_pid;
    uint32_t virt;
    uint32_t phys;
    uint32_t pages;
} dma_region_t;

static dma_region_t s_dma[DMA_TRACK_MAX];
static spinlock_t   s_dma_lock = SPINLOCK_INIT;

/* Rilascio unificato della fisica DMA: prima chiede al pool, e solo
 * se il range e' estraneo lo rende al PMM. Verbo esecutivo unico per
 * TUTTI i percorsi di restituzione (free esplicito, path d'errore,
 * cleanup alla morte): la provenienza del buffer non e' piu' un fatto
 * da ricordare ma una domanda che si fa al momento giusto. */
static void dma_release_phys(uint32_t phys, uint32_t pages)
{
    if (!dma_pool_free(phys, pages))
    {
        pmm_free_contiguous(phys, pages);
    }
}

/* Pagine DMA tracciate di un PID (task manager). */
uint32_t dma_track_pages_of(pid_t pid)
{
    uint32_t pages = 0;
    uint32_t fl = spinlock_acquire_irqsave(&s_dma_lock);
    for (int i = 0; i < DMA_TRACK_MAX; i++)
    {
        if (s_dma[i].used && s_dma[i].owner_pid == pid)
        {
            pages += s_dma[i].pages;
        }
    }
    spinlock_release_irqrestore(&s_dma_lock, fl);
    return pages;
}

/* Pressione degli slot: (usati << 16) | massimo. */
uint32_t dma_track_slot_pressure(void)
{
    uint32_t used = 0;
    uint32_t fl = spinlock_acquire_irqsave(&s_dma_lock);
    for (int i = 0; i < DMA_TRACK_MAX; i++)
    {
        if (s_dma[i].used)
        {
            used++;
        }
    }
    spinlock_release_irqrestore(&s_dma_lock, fl);
    return (used << 16) | (uint32_t)DMA_TRACK_MAX;
}

static bool dma_track(pid_t pid, uint32_t virt, uint32_t phys, uint32_t pages)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_dma_lock);
    for (int i = 0; i < DMA_TRACK_MAX; i++)
    {
        if (!s_dma[i].used)
        {
            s_dma[i].used      = true;
            s_dma[i].owner_pid = pid;
            s_dma[i].virt      = virt;
            s_dma[i].phys      = phys;
            s_dma[i].pages     = pages;
            spinlock_release_irqrestore(&s_dma_lock, fl);
            return true;
        }
    }
    spinlock_release_irqrestore(&s_dma_lock, fl);
    return false;
}

static bool dma_untrack(uint32_t virt, pid_t pid,
                        uint32_t *phys, uint32_t *pages)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_dma_lock);
    for (int i = 0; i < DMA_TRACK_MAX; i++)
    {
        if (s_dma[i].used && s_dma[i].virt == virt &&
            s_dma[i].owner_pid == pid)
        {
            *phys  = s_dma[i].phys;
            *pages = s_dma[i].pages;
            s_dma[i].used = false;
            spinlock_release_irqrestore(&s_dma_lock, fl);
            return true;
        }
    }
    spinlock_release_irqrestore(&s_dma_lock, fl);
    return false;
}

static bool dma_region_owned(uint32_t phys, uint32_t len, pid_t pid)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_dma_lock);
    for (int i = 0; i < DMA_TRACK_MAX; i++)
    {
        if (s_dma[i].used && s_dma[i].owner_pid == pid &&
            phys >= s_dma[i].phys &&
            phys + len <= s_dma[i].phys + s_dma[i].pages * PAGE_SIZE)
        {
            spinlock_release_irqrestore(&s_dma_lock, fl);
            return true;
        }
    }
    spinlock_release_irqrestore(&s_dma_lock, fl);
    return false;
}

static int32_t sys_dma_alloc(isr_regs_t *regs)
{
    uint32_t size = regs->ebx;
    process_t *proc = process_current();
    if (proc == NULL || !caller_is_driver() || size == 0)
    {
        return 0;
    }

    size = ALIGN_UP(size, PAGE_SIZE);
    uint32_t pages = size / PAGE_SIZE;

    /* Routing LOGICO della contiguita': prima la riserva di boot
     * (mm/dma_pool — contiguita' garantita anche dopo settimane di
     * churn), poi il PMM diretto con PREFER_LOW (< 16 MB per i
     * controller legacy; un bus-master PCI a 32 bit lavora ovunque).
     * Il free e' simmetrico: dma_release_phys interroga il pool e solo
     * se estraneo rende al PMM — nessuno deve ricordare la provenienza. */
    uint32_t phys = dma_pool_alloc(pages);
    if (phys == 0)
    {
        phys = pmm_alloc_contiguous(pages, PMM_ZONE_PREFER_LOW);
    }
    if (phys == 0)
    {
        return 0;
    }

    mutex_lock(&proc->vm_lock);
    uint32_t virt = vregion_alloc(&proc->vm_regions, pages,
                                  0x70000000u, 0x7FEFFFFFu,
                                  VREG_DMA | VREG_USER_RW, -1);
    if (virt == 0)
    {
        mutex_unlock(&proc->vm_lock);
        dma_release_phys(phys, pages);
        return 0;
    }
    for (uint32_t i = 0; i < pages; i++)
    {
        /* UC OBBLIGATORIO, non un'ottimizzazione mancata al contrario:
         * questa memoria la legge/scrive un BUS MASTER. I driver
         * scrivono descrittori (TD/QH, PRD, ring) con store volatile e
         * ZERO barrier — contratto ereditato dall'1.0, che mappava UC
         * dopo la campagna sul ferro (Armada/Extensa: descrittori in
         * cache WB e device che legge la RAM = corruzione DMA
         * intermittente, invisibile su QEMU che emula coerenza
         * perfetta). Stessa classe di PTE_CACHE_DISABLE di mmap_phys:
         * cio' che condividi con l'hardware non passa dalla cache. */
        paging_map_page(virt + i * PAGE_SIZE, phys + i * PAGE_SIZE,
                        PTE_PRESENT | PTE_WRITABLE | PTE_USER |
                        PTE_CACHE_DISABLE);
    }
    mutex_unlock(&proc->vm_lock);

    if (!dma_track(proc->pid, virt, phys, pages))
    {
        mutex_lock(&proc->vm_lock);
        for (uint32_t i = 0; i < pages; i++)
        {
            paging_unmap_page(virt + i * PAGE_SIZE);
        }
        paging_release_empty_user_tables(virt, virt + pages * PAGE_SIZE);
        tlb_shootdown_aspace(proc->page_directory, virt, pages);
        vregion_free(&proc->vm_regions, virt);
        mutex_unlock(&proc->vm_lock);
        dma_release_phys(phys, pages);
        kprintf("[DMA ] tracking pieno: alloc di PID %d rifiutata\n",
                proc->pid);
        return 0;
    }

    /* Contratto 1.0 VERO (verificato sul sorgente 1.0 e sulla libc
     * condivisa, che legge con "=b"(phys)): la fisica torna in EBX,
     * il virtuale in EAX. La versione precedente la scriveva nei
     * primi 4 byte del buffer credendolo il contratto: la libc
     * continuava a leggere EBX — cioe' l'argomento size rimasto nel
     * registro. Ogni driver programmava quindi il device con
     * "fisici" come 1024 o 4096: DMA del device nelle PRIME PAGINE
     * della RAM (corruzione silenziosa di sistema), buffer del
     * driver mai scritto (IDENTIFY "riuscita" a zeri: il disco
     * fantasma 0 KB in DobDisk), e primi 4 byte di ogni buffer
     * clobberati. */
    regs->ebx = phys;
    return (int32_t)virt;
}

static int32_t sys_dma_free(isr_regs_t *regs)
{
    uint32_t virt = regs->ebx;
    process_t *proc = process_current();
    if (proc == NULL || !caller_is_driver())
    {
        return -1;
    }

    uint32_t phys = 0;
    uint32_t pages = 0;
    if (!dma_untrack(virt, proc->pid, &phys, &pages))
    {
        return -1;                  /* ignota o non sua                 */
    }

    mutex_lock(&proc->vm_lock);
    for (uint32_t i = 0; i < pages; i++)
    {
        paging_unmap_page(virt + i * PAGE_SIZE);
    }
    paging_release_empty_user_tables(virt, virt + pages * PAGE_SIZE);
    tlb_shootdown_aspace(proc->page_directory, virt, pages);
    vregion_free(&proc->vm_regions, virt);
    mutex_unlock(&proc->vm_lock);

    dma_release_phys(phys, pages);
    return 0;
}

static int32_t sys_dma_verify(isr_regs_t *regs)
{
    process_t *proc = process_current();
    if (proc == NULL || !caller_is_driver())
    {
        return 0;
    }
    return dma_region_owned(regs->ebx, regs->ecx, proc->pid) ? 1 : 0;
}

/* Alla morte: sgancia il tracking e restituisce la fisica. Le pagine
 * utente muoiono col PD del processo; qui conta solo che i frame
 * contigui tornino al pool. */
static void dma_cleanup(pid_t pid)
{
    for (int i = 0; i < DMA_TRACK_MAX; i++)
    {
        uint32_t fl = spinlock_acquire_irqsave(&s_dma_lock);
        bool mine = s_dma[i].used && s_dma[i].owner_pid == pid;
        uint32_t phys = s_dma[i].phys;
        uint32_t pages = s_dma[i].pages;
        if (mine)
        {
            s_dma[i].used = false;
        }
        spinlock_release_irqrestore(&s_dma_lock, fl);
        if (mine)
        {
            dma_release_phys(phys, pages);
        }
    }
}

/* ======================================================================
 * Claim dispositivi
 * ==================================================================== */

#define DEV_CLAIM_MAX 32

typedef struct
{
    bool     used;
    pid_t    owner_pid;
    uint32_t irq;
    uint32_t phys_base;
    uint32_t phys_size;
    uint32_t notify_port;
} dev_claim_t;

static dev_claim_t s_dev_claims[DEV_CLAIM_MAX];
static spinlock_t  s_dev_lock = SPINLOCK_INIT;

static int32_t sys_device_claim(isr_regs_t *regs)
{
    process_t *proc = process_current();
    if (proc == NULL || !caller_is_driver())
    {
        return -1;
    }

    uint32_t irq       = regs->ebx;
    uint32_t phys_base = regs->ecx;
    uint32_t phys_size = regs->edx;
    uint32_t port      = regs->esi;

    uint32_t fl = spinlock_acquire_irqsave(&s_dev_lock);

    for (int i = 0; i < DEV_CLAIM_MAX; i++)
    {
        if (s_dev_claims[i].used && s_dev_claims[i].irq == irq && irq != 0)
        {
            spinlock_release_irqrestore(&s_dev_lock, fl);
            return -1;              /* IRQ gia' rivendicato             */
        }
    }

    int id = -1;
    for (int i = 0; i < DEV_CLAIM_MAX; i++)
    {
        if (!s_dev_claims[i].used)
        {
            id = i;
            break;
        }
    }
    if (id < 0)
    {
        spinlock_release_irqrestore(&s_dev_lock, fl);
        return -1;
    }

    s_dev_claims[id].used        = true;
    s_dev_claims[id].owner_pid   = proc->pid;
    s_dev_claims[id].irq         = irq;
    s_dev_claims[id].phys_base   = phys_base;
    s_dev_claims[id].phys_size   = phys_size;
    s_dev_claims[id].notify_port = port;
    spinlock_release_irqrestore(&s_dev_lock, fl);
    return id;
}

static int32_t sys_device_release(isr_regs_t *regs)
{
    int id = (int)regs->ebx;
    process_t *proc = process_current();
    if (id < 0 || id >= DEV_CLAIM_MAX || proc == NULL || !caller_is_driver())
    {
        return -1;
    }

    uint32_t fl = spinlock_acquire_irqsave(&s_dev_lock);
    if (!s_dev_claims[id].used || s_dev_claims[id].owner_pid != proc->pid)
    {
        spinlock_release_irqrestore(&s_dev_lock, fl);
        return -1;
    }
    s_dev_claims[id].used = false;
    spinlock_release_irqrestore(&s_dev_lock, fl);
    return 0;
}

static void dev_claim_cleanup(pid_t pid)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_dev_lock);
    for (int i = 0; i < DEV_CLAIM_MAX; i++)
    {
        if (s_dev_claims[i].used && s_dev_claims[i].owner_pid == pid)
        {
            s_dev_claims[i].used = false;
        }
    }
    spinlock_release_irqrestore(&s_dev_lock, fl);
}

/* ======================================================================
 * Timer asincroni (notifica via porta)
 * ==================================================================== */

#define TIMER_POOL_MAX  64
#define TIMER_EVT_FIRED 70u         /* code IPC del fire (ABI)          */

typedef struct
{
    bool     used;
    pid_t    owner_pid;
    uint32_t notify_port;
    uint32_t timer_id;
    timer_t  ktimer;
} async_timer_t;

static async_timer_t s_timer_pool[TIMER_POOL_MAX];
static spinlock_t    s_timer_pool_lock = SPINLOCK_INIT;

/* Contesto IRQ, dentro il drain: ipc_post_kernel usa il pool
 * preallocato, mai kmalloc, mai blocca. */
static void async_timer_fire(void *arg)
{
    async_timer_t *at = (async_timer_t *)arg;
    if (at == NULL || !at->used)
    {
        return;
    }

    ipc_post_kernel(at->notify_port, TIMER_EVT_FIRED, at->timer_id, 0, 0);

    if (at->ktimer.period_ns == 0)
    {
        at->used = false;           /* one-shot: slot libero            */
    }
}

static int32_t sys_timer_set(isr_regs_t *regs)
{
    uint32_t port     = regs->ebx;
    uint32_t delay_ms = regs->ecx;
    uint32_t repeat   = regs->edx;

    process_t *proc = process_current();
    if (proc == NULL || delay_ms == 0)
    {
        return -1;
    }

    uint32_t fl = spinlock_acquire_irqsave(&s_timer_pool_lock);
    int id = -1;
    for (int i = 0; i < TIMER_POOL_MAX; i++)
    {
        if (!s_timer_pool[i].used)
        {
            id = i;
            break;
        }
    }
    if (id < 0)
    {
        spinlock_release_irqrestore(&s_timer_pool_lock, fl);
        return -1;
    }
    async_timer_t *at = &s_timer_pool[id];
    at->used        = true;
    at->owner_pid   = proc->pid;
    at->notify_port = port;
    at->timer_id    = (uint32_t)id;
    spinlock_release_irqrestore(&s_timer_pool_lock, fl);

    /* NIENTE timer_init qui: il ktimer di ogni slot e' inizializzato
     * UNA volta da driver_syscalls_init. Ri-inizializzarlo a ogni
     * riuso, fuori dal lock della ruota, azzerava heap_idx di un
     * timer potenzialmente ancora armato dalla vita precedente dello
     * slot: heap con doppioni e back-index incoerenti (la race
     * "dobinterface puro"). arm_locked gestisce gia' il ri-armo di un
     * timer vivo: detach verificato + gen++ che rende stantii i fire
     * in volo. */

    /* ms -> ns al confine: il contratto userspace e' in ms, il kernel
     * e' ns-nativo. */
    bool ok = repeat
            ? timer_arm_periodic(&at->ktimer, MS_TO_NS(delay_ms))
            : timer_arm_in(&at->ktimer, MS_TO_NS(delay_ms));
    if (!ok)
    {
        fl = spinlock_acquire_irqsave(&s_timer_pool_lock);
        at->used = false;
        spinlock_release_irqrestore(&s_timer_pool_lock, fl);
        return -1;
    }
    return id;
}

static int32_t sys_timer_cancel(isr_regs_t *regs)
{
    int id = (int)regs->ebx;
    process_t *proc = process_current();
    if (id < 0 || id >= TIMER_POOL_MAX || proc == NULL)
    {
        return -1;
    }

    uint32_t fl = spinlock_acquire_irqsave(&s_timer_pool_lock);
    async_timer_t *at = &s_timer_pool[id];
    if (!at->used || at->owner_pid != proc->pid)
    {
        spinlock_release_irqrestore(&s_timer_pool_lock, fl);
        return -1;
    }
    spinlock_release_irqrestore(&s_timer_pool_lock, fl);

    timer_cancel(&at->ktimer);          /* detach + gen++: fire stantii */

    fl = spinlock_acquire_irqsave(&s_timer_pool_lock);
    at->used = false;                   /* liberato DOPO il cancel, sotto
                                         * lock: un set concorrente non
                                         * riusa uno slot col ktimer
                                         * ancora in volo */
    spinlock_release_irqrestore(&s_timer_pool_lock, fl);
    return 0;
}

static void timer_pool_cleanup(pid_t pid)
{
    for (int i = 0; i < TIMER_POOL_MAX; i++)
    {
        uint32_t fl = spinlock_acquire_irqsave(&s_timer_pool_lock);
        bool mine = s_timer_pool[i].used && s_timer_pool[i].owner_pid == pid;
        spinlock_release_irqrestore(&s_timer_pool_lock, fl);
        if (mine)
        {
            timer_cancel(&s_timer_pool[i].ktimer);
            fl = spinlock_acquire_irqsave(&s_timer_pool_lock);
            s_timer_pool[i].used = false;
            spinlock_release_irqrestore(&s_timer_pool_lock, fl);
        }
    }
}

/* ======================================================================
 * Watchdog (il kernel uccide il processo che non rinfresca)
 * ==================================================================== */

#define WATCHDOG_MAX 16

typedef struct
{
    bool     used;
    pid_t    owner_pid;
    uint32_t timeout_ms;
    timer_t  timer;
} watchdog_entry_t;

static watchdog_entry_t s_watchdogs[WATCHDOG_MAX];
static spinlock_t       s_watchdog_lock = SPINLOCK_INIT;

/* Work item: uccisione fuori dal contesto IRQ. */
/* Deadline raggiunta (contesto IRQ): libera lo slot e differisce il kill
 * sul canale intrusivo non-perdibile (mai la workqueue: un watchdog che
 * non interviene per uno scarto a coda piena vanificherebbe il watchdog). */
static void watchdog_fire(void *arg)
{
    watchdog_entry_t *e = (watchdog_entry_t *)arg;
    if (e == NULL)
    {
        return;
    }

    uint32_t fl = spinlock_acquire_irqsave(&s_watchdog_lock);
    bool was_active = e->used;
    pid_t pid = e->owner_pid;
    e->used = false;
    spinlock_release_irqrestore(&s_watchdog_lock, fl);

    if (was_active)
    {
        process_t *p = process_get_ref(pid);
        if (p != NULL)
        {
            kprintf("[WDOG] PID %d non ha rinfrescato il watchdog: "
                    "terminato\n", pid);
            process_destroy_deferred(p, -9);    /* esito -9, teardown in idle */
            process_put(p);
        }
    }
}

static int32_t sys_watchdog_set(isr_regs_t *regs)
{
    uint32_t timeout_ms = regs->ebx;
    process_t *proc = process_current();
    if (proc == NULL)
    {
        return -1;
    }

    uint32_t fl = spinlock_acquire_irqsave(&s_watchdog_lock);
    int idx = -1;
    for (int i = 0; i < WATCHDOG_MAX; i++)
    {
        if (s_watchdogs[i].used && s_watchdogs[i].owner_pid == proc->pid)
        {
            idx = i;
            break;
        }
    }

    if (timeout_ms == 0)            /* disattiva                        */
    {
        if (idx >= 0)
        {
            s_watchdogs[idx].used = false;
            spinlock_release_irqrestore(&s_watchdog_lock, fl);
            timer_cancel(&s_watchdogs[idx].timer);
            return 0;
        }
        spinlock_release_irqrestore(&s_watchdog_lock, fl);
        return 0;
    }

    if (idx < 0)
    {
        for (int i = 0; i < WATCHDOG_MAX; i++)
        {
            if (!s_watchdogs[i].used)
            {
                idx = i;
                break;
            }
        }
        if (idx < 0)
        {
            spinlock_release_irqrestore(&s_watchdog_lock, fl);
            return -1;
        }
        s_watchdogs[idx].used      = true;
        s_watchdogs[idx].owner_pid = proc->pid;
        /* NIENTE timer_init qui: il timer di ogni slot e' init-once in
         * driver_syscalls_init. Sul riuso si arma soltanto (sotto, cancel
         * + arm): arm_locked gestisce il ri-armo di un timer vivo. */
    }
    s_watchdogs[idx].timeout_ms = timeout_ms;
    spinlock_release_irqrestore(&s_watchdog_lock, fl);

    /* Rinfresco = riarmo: cancel + arm (drift-free non serve qui). */
    timer_cancel(&s_watchdogs[idx].timer);
    if (!timer_arm_in(&s_watchdogs[idx].timer, MS_TO_NS(timeout_ms)))
    {
        uint32_t f2 = spinlock_acquire_irqsave(&s_watchdog_lock);
        s_watchdogs[idx].used = false;
        spinlock_release_irqrestore(&s_watchdog_lock, f2);
        return -1;
    }
    return 0;
}

static void watchdog_cleanup(pid_t pid)
{
    for (int i = 0; i < WATCHDOG_MAX; i++)
    {
        uint32_t fl = spinlock_acquire_irqsave(&s_watchdog_lock);
        bool mine = s_watchdogs[i].used && s_watchdogs[i].owner_pid == pid;
        if (mine)
        {
            s_watchdogs[i].used = false;
        }
        spinlock_release_irqrestore(&s_watchdog_lock, fl);
        if (mine)
        {
            timer_cancel(&s_watchdogs[i].timer);
        }
    }
}

/* ======================================================================
 * Sezioni critiche driver (anti-preemption breve)
 * ==================================================================== */

static int32_t sys_enter_critical(isr_regs_t *regs UNUSED)
{
    if (!caller_is_driver())
    {
        return -1;
    }
    scheduler_enter_critical();
    return 0;
}

static int32_t sys_exit_critical(isr_regs_t *regs UNUSED)
{
    if (!caller_is_driver())
    {
        return -1;
    }
    scheduler_exit_critical();
    return 0;
}

/* ======================================================================
 * PCI config space (legacy 0xCF8/0xCFC; hook ECAM col milestone MCFG)
 * ==================================================================== */

/* I cicli config vivono in arch/x86/pci_cfg sotto un lock di sistema:
 * qui prima c'era una copia privata NON serializzata del ciclo — su SMP
 * un SYS_PCI_READ di un driver su una CPU e un accesso kernel (pirq,
 * MSI) sull'altra si interfogliavano su 0xCF8/0xCFC e leggevano il
 * registro del device SBAGLIATO. I nomi kpci_* restano per i call site
 * storici del file. */

static uint32_t kpci_read32(uint8_t bus, uint8_t slot, uint8_t func,
                            uint32_t off)
{
    return pci_cfg_read32(bus, slot, func, off);
}

static void kpci_write32(uint8_t bus, uint8_t slot, uint8_t func,
                         uint32_t off, uint32_t value)
{
    pci_cfg_write32(bus, slot, func, off, value);
}

static int32_t sys_pci_read(isr_regs_t *regs)
{
    if (!caller_is_driver())
    {
        return -1;
    }
    return (int32_t)kpci_read32((uint8_t)(regs->ebx & 0xFFu),
                                (uint8_t)(regs->ecx & 0x1Fu),
                                (uint8_t)(regs->edx & 0x07u),
                                regs->esi & 0xFFCu);
}

static int32_t sys_pci_write(isr_regs_t *regs)
{
    if (!caller_is_driver())
    {
        return -1;
    }
    uint32_t bsf = regs->ebx;
    kpci_write32((uint8_t)((bsf >> 16) & 0xFFu),
                 (uint8_t)((bsf >> 8) & 0x1Fu),
                 (uint8_t)(bsf & 0x07u),
                 regs->ecx & 0xFFCu,
                 regs->edx);
    return 0;
}

/* ======================================================================
 * Boomerang video (deflettore int 0x85)
 *
 * L'asm codifica offset di struct: le guardie qui fermano la build se
 * una struct cambia sotto di lui.
 * ==================================================================== */

_Static_assert(offsetof(process_t, pid) == 0,
               "video_boomerang.asm: OFFSET_PROC_PID derivato");
_Static_assert(offsetof(thread_t, owner) == 164,
               "video_boomerang.asm: OFFSET_THREAD_OWNER derivato");
_Static_assert(offsetof(percpu_t, current) == 4,
               "video_boomerang.asm: OFFSET_PERCPU_CURRENT derivato");
_Static_assert(offsetof(percpu_t, cpu_index) == 8,
               "video_boomerang.asm: OFFSET_PERCPU_CPU_INDEX derivato");
_Static_assert(MAX_CPUS == 32,
               "video_boomerang.asm: BM_MAX_CPUS deve seguire MAX_CPUS");
_Static_assert((GDT_SEL_UCODE) == 0x1B && (GDT_SEL_UDATA) == 0x23,
               "video_boomerang.asm: selettori utente derivati");

/* regs->ebx = entry del dispatcher, ecx = cima stack di dispatch,
 * edx = buffer payload — tutti VA nello user space del driver. Slot
 * unico: un secondo driver puo' solo essere lo stesso PID (refresh). */
static int32_t sys_register_video_driver(isr_regs_t *regs)
{
    process_t *proc = process_current();
    if (proc == NULL || !caller_is_driver() || regs->ebx == 0)
    {
        return -1;
    }
    if (g_video_driver_cr3 != 0 && g_video_driver_pid != (uint32_t)proc->pid)
    {
        return -1;                  /* slot occupato da un altro vivo   */
    }

    uint32_t fl = irq_save();       /* pubblicazione atomica dello slot */
    g_video_driver_entry  = regs->ebx;
    g_dispatch_stack_top  = regs->ecx;
    g_driver_payload_buf  = regs->edx;
    g_video_driver_cr3    = proc->page_directory;
    g_video_driver_pid    = (uint32_t)proc->pid;
    /* Il boomerang presta questo CR3 a thread di ALTRI core: per lui
     * l'invariante "solo la home e' sul CR3" gia' non vale — resta
     * inchiodato, il work stealing non lo tocca. */
    proc->pinned = true;
    irq_restore(fl);

    kprintf("[VID ] Driver video PID %d registrato (entry 0x%08x).\n",
            proc->pid, regs->ebx);
    return 0;
}

static void video_boomerang_init(void)
{
    idt_set_gate(VIDEO_BOOMERANG_INT, (uint32_t)int_85_entry,
                 GDT_SEL_KCODE, IDT_FLAG_INT_USER);
    idt_set_gate(VIDEO_BOOMERANG_RET, (uint32_t)int_86_return,
                 GDT_SEL_KCODE, IDT_FLAG_INT_USER);
    kprintf("[VID ] Boomerang int 0x85 armato (slot libero).\n");
}

/* ======================================================================
 * Recupero da fault col trasporto boomerang in volo
 *
 * Modello migrating-thread: durante la fase in-driver process_current()
 * resta il CHIAMANTE. Senza questo recupero, un fault del dispatcher
 * ring-3 del driver (o delle copie ring-0 eseguite in CR3 driver)
 * verrebbe imputato al chiamante — ucciso al posto del colpevole — e
 * g_boomerang_lock resterebbe preso per sempre: ogni dv_* successiva
 * di qualunque processo girerebbe in .lock_wait a ring 0 con IF=0,
 * murando una CPU alla prima chiamata e l'altra alla successiva.
 *
 * Il recupero: se lo slot per-CPU e' ACTIVE, il fault appartiene alla
 * fase in-driver. Spegne il flag, rilascia il lock (il detentore era
 * questa CPU), torna al CR3 del chiamante e riscrive *regs come farebbe
 * int 0x86, con rc = DV_ERR_RESET: il chiamante sopravvive con un
 * errore e il driver resta registrato (il fault puo' essere indotto da
 * argomenti marci di un client). Copre anche i fault ring-0 delle
 * copie: l'epilogo dello stub chiude con `popa; add esp,8; iretd` e il
 * CS RPL3 scritto qui fa consumare all'iretd anche SS:ESP, deposti
 * nello scratch dello stack abbandonato di int_85_entry.
 * ==================================================================== */

/* Vista C dello slot di trasporto per-CPU. L'ASM (video_boomerang.asm)
 * e' l'autorita' sul layout: questi offset lo rispecchiano e vanno
 * tenuti allineati a mano insieme agli altri (BM_*). */
#define BM_SLOT_EIP      0u
#define BM_SLOT_CS       4u
#define BM_SLOT_EFLAGS   8u
#define BM_SLOT_ESP     12u
#define BM_SLOT_SS      16u
#define BM_SLOT_CR3     20u
#define BM_SLOT_EBX     24u
#define BM_SLOT_ECX     28u
#define BM_SLOT_EDX     32u
#define BM_SLOT_ESI     36u
#define BM_SLOT_EDI     40u
#define BM_SLOT_EBP     44u
#define BM_SLOT_PID     48u
#define BM_SLOT_SCRATCH 52u                 /* opcode dv_ del chiamante */
#define BM_SLOT_ACTIVE  60u
#define BM_SLOT_STRIDE  (64u + 16384u)      /* BM_BOUNCE + G_PAYLOAD_MAX */

extern uint8_t  g_bm[];                     /* slot per-CPU (asm)       */
extern uint32_t g_boomerang_lock;           /* fase in-driver (asm)     */

static inline uint32_t bm_slot_read(const uint8_t *slot, uint32_t off)
{
    uint32_t v;
    memcpy(&v, slot + off, sizeof(v));
    return v;
}

/* Vero se QUESTA CPU sta eseguendo la fase in-driver del boomerang
 * (slot per-CPU ACTIVE). In quella fase il CR3 installato e' quello
 * del DRIVER video ma process_current() resta il CHIAMANTE
 * (migrating-thread): ogni syscall che tocca lo spazio di
 * indirizzamento deve quindi agire sul processo del driver — vedi
 * video_boomerang_as_process(). */
bool video_boomerang_in_driver_phase(void)
{
    percpu_t *pc = this_cpu();
    if (pc == NULL) return false;
    uint8_t *slot = g_bm + (uint32_t)pc->cpu_index * BM_SLOT_STRIDE;
    return *(volatile uint32_t *)(slot + BM_SLOT_ACTIVE) != 0;
}

/* Il processo il cui spazio di indirizzamento e' installato ADESSO:
 * il driver video durante la fase in-driver, il processo corrente
 * altrimenti. Le syscall che modificano l'AS (sbrk in testa: la
 * malloc del driver gira dentro il dispatch boomerang) devono usare
 * QUESTO, non process_current(): contabilita' del brk e mappature
 * devono vivere nello stesso processo, o il heap del driver e quello
 * del chiamante si corrompono a vicenda in silenzio. */
process_t *video_boomerang_as_process(void)
{
    if (video_boomerang_in_driver_phase() && g_video_driver_pid != 0)
    {
        process_t *dp = process_get_by_pid((pid_t)g_video_driver_pid);
        if (dp != NULL) return dp;
    }
    return process_current();
}

bool video_boomerang_fault_recover(isr_regs_t *regs)
{
    percpu_t *pc = this_cpu();
    if (pc == NULL)
    {
        return false;
    }

    uint8_t *slot = g_bm + (uint32_t)pc->cpu_index * BM_SLOT_STRIDE;
    volatile uint32_t *active =
        (volatile uint32_t *)(slot + BM_SLOT_ACTIVE);
    if (*active == 0)
    {
        return false;                       /* fault ordinario          */
    }

    /* Il flag e' per-CPU e il fault gira sulla CPU del trasporto: qui
     * non serve alcun lock. Ordine: prima spegni ACTIVE (un secondo
     * fault nel recupero non deve rientrare), poi libera il lock. */
    *active = 0;
    g_boomerang_lock = 0;

    uint32_t caller_cr3 = bm_slot_read(slot, BM_SLOT_CR3);
    __asm__ volatile ("mov %0, %%cr3" :: "r"(caller_cr3) : "memory");

    /* Fotografa il fault PRIMA di ricostruire il frame di ritorno: il
     * vettore, l'errore, l'EIP nel driver e (per i #PF) CR2 dicono in
     * un colpo COSA e' esploso — heap del driver (malloc concorrente
     * main-thread vs dispatch?), VRAM non mappata, stack di dispatch,
     * deref NULL. La riga anonima di prima ha fatto perdere tre giri
     * di caccia a un bug bimodale. */
    uint32_t f_vec = regs->vector;
    uint32_t f_err = regs->error_code;
    uint32_t f_eip = regs->eip;
    uint32_t f_cr2 = 0;
    if (f_vec == 14)
    {
        __asm__ volatile ("mov %%cr2, %0" : "=r"(f_cr2));
    }

    process_t *caller = process_current();
    kprintf("[VID ] fault nel dispatcher boomerang (driver PID %u): "
            "chiamante '%s' (PID %d) ripristinato con DV_ERR_RESET, "
            "lock liberato.\n",
            g_video_driver_pid,
            caller ? caller->name : "?", caller ? caller->pid : -1);
    kprintf("[VID ]   dettaglio: vec %u err 0x%x eip 0x%08x%s cr2 "
            "0x%08x\n", f_vec, f_err, f_eip,
            (f_vec == 14) ? "" : " (no #PF:)", f_cr2);
    /* La chiamata in volo, dal contratto dello slot: opcode (EAX di
     * ingresso, in SCRATCH) + argomenti scalari (EBX/ECX/EDX) +
     * payload del chiamante (ESI=VA, EDI=size). Con questi, il fault
     * si mappa a mano libera: opcode -> case del dispatcher ->
     * funzione dv_ -> struct -> QUALE campo conteneva cr2. */
    kprintf("[VID ]   in volo: opcode %u args(0x%x, 0x%x, 0x%x) "
            "payload VA 0x%08x size %u\n",
            bm_slot_read(slot, BM_SLOT_SCRATCH),
            bm_slot_read(slot, BM_SLOT_EBX),
            bm_slot_read(slot, BM_SLOT_ECX),
            bm_slot_read(slot, BM_SLOT_EDX),
            bm_slot_read(slot, BM_SLOT_ESI),
            bm_slot_read(slot, BM_SLOT_EDI));

    /* Ricostruisci il ritorno al chiamante come farebbe int 0x86. I
     * segmenti dati tornano ai selettori utente: se il fault e' nato a
     * ring 0 lo stub li ha salvati kernel, ma il ritorno e' a ring 3. */
    regs->eip     = bm_slot_read(slot, BM_SLOT_EIP);
    regs->cs      = bm_slot_read(slot, BM_SLOT_CS);
    regs->eflags  = bm_slot_read(slot, BM_SLOT_EFLAGS);
    regs->useresp = bm_slot_read(slot, BM_SLOT_ESP);
    regs->ss      = bm_slot_read(slot, BM_SLOT_SS);
    regs->ebx     = bm_slot_read(slot, BM_SLOT_EBX);
    regs->ecx     = bm_slot_read(slot, BM_SLOT_ECX);
    regs->edx     = bm_slot_read(slot, BM_SLOT_EDX);
    regs->esi     = bm_slot_read(slot, BM_SLOT_ESI);
    regs->edi     = bm_slot_read(slot, BM_SLOT_EDI);
    regs->ebp     = bm_slot_read(slot, BM_SLOT_EBP);
    regs->eax     = (uint32_t)(int32_t)-8;  /* DV_ERR_RESET (video.h)   */
    regs->ds = regs->es = regs->fs = regs->gs = GDT_SEL_UDATA;

    return true;
}

static void video_boomerang_cleanup(pid_t pid)
{
    if (g_video_driver_pid == 0 || g_video_driver_pid != (uint32_t)pid)
    {
        return;
    }
    uint32_t fl = irq_save();
    g_video_driver_cr3   = 0;
    g_video_driver_entry = 0;
    g_video_driver_pid   = 0;
    g_dispatch_stack_top = 0;
    g_driver_payload_buf = 0;
    irq_restore(fl);
    kprintf("[VID ] Driver video PID %d morto: slot liberato.\n", pid);
}

/* ======================================================================
 * Risoluzione empirica PCI INTx -> GSI (consegna IOAPIC)
 *
 * In modo APIC il byte Interrupt Line del PCI non ha senso e non c'e'
 * _PRT ACPI da consultare (nessun interprete AML, per scelta). Invece
 * di cablare la mappa INTx->PIRQ->GSI di ogni chipset, il kernel la
 * IMPARA dall'hardware: arma ogni ingresso IOAPIC del range PCI e,
 * quando uno asserisce, il driver il cui device mostra l'interrupt come
 * proprio lo rivendica. Unico approccio compatibile con la regola
 * microkernel (il kernel non conosce i registri di stato dei device) e
 * indipendente dal chipset: hardware nuovo, zero modifiche al kernel.
 * ==================================================================== */

#define MAX_PCI_BINDINGS 32

typedef struct
{
    bool    used;
    uint8_t bus, slot, func;
    uint8_t gsi;
} pci_binding_t;

static pci_binding_t s_pci_bindings[MAX_PCI_BINDINGS];

/* Tutte sotto s_irqfwd_lock (le entry point lo prendono). */

static int pci_pending_add(pid_t pid, uint32_t port,
                           uint8_t bus, uint8_t slot, uint8_t func)
{
    for (int i = 0; i < MAX_PCI_PENDING; i++)
    {
        if (s_pci_pending[i].used && s_pci_pending[i].pid == pid &&
            s_pci_pending[i].bus == bus && s_pci_pending[i].slot == slot &&
            s_pci_pending[i].func == func)
        {
            s_pci_pending[i].notify_port = port;    /* idempotente */
            return 0;
        }
    }
    for (int i = 0; i < MAX_PCI_PENDING; i++)
    {
        if (!s_pci_pending[i].used)
        {
            s_pci_pending[i].used        = true;
            s_pci_pending[i].pid         = pid;
            s_pci_pending[i].notify_port = port;
            s_pci_pending[i].bus         = bus;
            s_pci_pending[i].slot        = slot;
            s_pci_pending[i].func        = func;
            s_pci_pending_count++;
            return 0;
        }
    }
    return -1;
}

static pci_pending_t *pci_pending_find(pid_t pid)
{
    for (int i = 0; i < MAX_PCI_PENDING; i++)
    {
        if (s_pci_pending[i].used && s_pci_pending[i].pid == pid)
        {
            return &s_pci_pending[i];
        }
    }
    return NULL;
}

static void pci_binding_put(uint8_t bus, uint8_t slot, uint8_t func,
                            uint8_t gsi)
{
    for (int i = 0; i < MAX_PCI_BINDINGS; i++)
    {
        if (s_pci_bindings[i].used && s_pci_bindings[i].bus == bus &&
            s_pci_bindings[i].slot == slot && s_pci_bindings[i].func == func)
        {
            s_pci_bindings[i].gsi = gsi;
            return;
        }
    }
    for (int i = 0; i < MAX_PCI_BINDINGS; i++)
    {
        if (!s_pci_bindings[i].used)
        {
            s_pci_bindings[i].used = true;
            s_pci_bindings[i].bus  = bus;
            s_pci_bindings[i].slot = slot;
            s_pci_bindings[i].func = func;
            s_pci_bindings[i].gsi  = gsi;
            return;
        }
    }
}

static bool pci_binding_lookup(uint8_t bus, uint8_t slot, uint8_t func,
                               uint8_t *out_gsi)
{
    for (int i = 0; i < MAX_PCI_BINDINGS; i++)
    {
        if (s_pci_bindings[i].used && s_pci_bindings[i].bus == bus &&
            s_pci_bindings[i].slot == slot && s_pci_bindings[i].func == func)
        {
            if (out_gsi != NULL)
            {
                *out_gsi = s_pci_bindings[i].gsi;
            }
            return true;
        }
    }
    return false;
}

/* Arma per discovery ogni GSI PCI (16..31) coperto dall'IOAPIC e non
 * gia' legato: installa il forward handler e apre la voce. Chiamante
 * tiene s_irqfwd_lock. */
static void pci_discovery_arm(void)
{
    for (uint32_t gsi = 16; gsi < MAX_IRQ_FORWARD; gsi++)
    {
        if (!ioapic_covers_gsi(gsi))
        {
            continue;
        }
        irq_line_t *line = &irq_lines[gsi];
        if (line->sharer_count > 0 || line->probe_armed)
        {
            continue;
        }
        /* safety_timer: init-once in driver_syscalls_init. Per
         * gsi >= 16 l'aggancio completo (binding + gate nella
         * finestra 0x30) lo fa il bridge; irq_register_handler e'
         * per le sole linee legacy < 16 e qui sarebbe un no-op. */
        intr_bridge_legacy_handler((uint8_t)gsi, irq_forward_handler);
        line->probe_armed = true;
        line->masked      = false;
        ioapic_unmask_gsi(gsi);
    }
}

static void pci_discovery_disarm_unbound(void)
{
    for (uint32_t gsi = 16; gsi < MAX_IRQ_FORWARD; gsi++)
    {
        irq_line_t *line = &irq_lines[gsi];
        if (!line->probe_armed || line->sharer_count > 0)
        {
            continue;
        }
        timer_cancel(&line->safety_timer);
        ioapic_mask_gsi(gsi);
        intr_clear_vector((uint8_t)(IRQ_BASE_VECTOR + gsi));
        line->probe_armed   = false;
        line->probe_backoff = false;
        line->masked        = false;
        line->pending_done  = 0;
    }
}

static int32_t sys_irq_register_pci(isr_regs_t *regs)
{
    uint32_t bsf  = regs->ebx;
    uint32_t port = regs->ecx;
    if (!caller_is_driver())
    {
        return -1;
    }
    process_t *proc = process_current();
    uint8_t bus  = (uint8_t)((bsf >> 16) & 0xFFu);
    uint8_t slot = (uint8_t)((bsf >> 8) & 0x1Fu);
    uint8_t func = (uint8_t)(bsf & 0x07u);

    if (!intr_ioapic_delivery_active())
    {
        /* Modo PIC: risolvi via Interrupt Line (config 0x3C). */
        uint8_t iline = (uint8_t)(kpci_read32(bus, slot, func, 0x3C) & 0xFFu);
        if (iline == 0 || iline >= 16)
        {
            return -1;
        }
        uint32_t lfl = spinlock_acquire_irqsave(&s_irqfwd_lock);
        int32_t ar = irq_attach_sharer(iline, proc, port);
        spinlock_release_irqrestore(&s_irqfwd_lock, lfl);
        return (ar != 0) ? -1 : (int32_t)iline;
    }

    /* Modo IOAPIC: binding gia' imparato -> lega subito. */
    uint8_t gsi;
    uint32_t lfl = spinlock_acquire_irqsave(&s_irqfwd_lock);
    if (pci_binding_lookup(bus, slot, func, &gsi))
    {
        int32_t ar = irq_attach_sharer(gsi, proc, port);
        spinlock_release_irqrestore(&s_irqfwd_lock, lfl);
        kprintf("[IRQ ] 0:%u.%u: GSI %u dalla cache binding (PID %d)\n",
                slot, func, gsi, proc->pid);
        return (ar != 0) ? -1 : (int32_t)gsi;
    }
    spinlock_release_irqrestore(&s_irqfwd_lock, lfl);

    /* Risoluzione DETERMINISTICA dal chipset (backend ICH del layer
     * pirq): su ICH-class INTx -> PIRQ -> GSI e' letto dai registri,
     * non indovinato. Elimina in blocco la classe dei claim sbagliati
     * della discovery (un device con status pendente che rivendica il
     * GSI di un vicino perche' notificato dal fire ALTRUI). Fuori dal
     * lock: legge solo PCI config. */
    if (pirq_resolve_gsi(bus, slot, func, &gsi) &&
        gsi >= 16 && gsi < MAX_IRQ_FORWARD && ioapic_covers_gsi(gsi))
    {
        lfl = spinlock_acquire_irqsave(&s_irqfwd_lock);
        pci_binding_put(bus, slot, func, gsi);
        int32_t ar = irq_attach_sharer(gsi, proc, port);
        spinlock_release_irqrestore(&s_irqfwd_lock, lfl);
        kprintf("[IRQ ] 0:%u.%u: INTx risolto dal chipset -> GSI %u\n",
                slot, func, gsi);
        return (ar != 0) ? -1 : (int32_t)gsi;
    }

    /* Sconosciuto: entra in pending e arma la discovery. Il GSI si
     * apprende al primo interrupt del device (dispatcher del driver +
     * SYS_IRQ_PCI_CLAIM). Ritorna 0 = "GSI pendente". La riga di log e'
     * deliberata: un fallthrough qui su chipset ICH e' un'ANOMALIA
     * (pin illeggibile, race, BDF sbagliato) e deve avere un nome nel
     * log invece di degradare in silenzio nella via fragile. */
    kprintf("[IRQ ] 0:%u.%u: INTx non risolto dal chipset, discovery "
            "empirica (PID %d)\n", slot, func, proc->pid);
    lfl = spinlock_acquire_irqsave(&s_irqfwd_lock);
    pci_pending_add(proc->pid, port, bus, slot, func);
    pci_discovery_arm();
    spinlock_release_irqrestore(&s_irqfwd_lock, lfl);
    return 0;
}

static int32_t sys_irq_pci_claim(isr_regs_t *regs)
{
    uint32_t gsi = regs->ebx;
    if (!caller_is_driver() || gsi < 16 || gsi >= MAX_IRQ_FORWARD ||
        !ioapic_covers_gsi(gsi))
    {
        return -1;
    }
    process_t *proc = process_current();

    uint32_t lfl = spinlock_acquire_irqsave(&s_irqfwd_lock);
    pci_pending_t *p = pci_pending_find(proc->pid);
    if (p == NULL)
    {
        spinlock_release_irqrestore(&s_irqfwd_lock, lfl);
        return -1;              /* non in risoluzione: claim stantio */
    }

    uint8_t  bus = p->bus, slot = p->slot, func = p->func;
    uint32_t port = p->notify_port;
    irq_line_t *line = &irq_lines[gsi];

    /* Se il fire era un probe (linea non legata): ferma la finestra di
     * claim e azzera lo stato prima che la linea diventi sharer legato.
     * Se invece il claim arriva dal piggyback su un GSI CONDIVISO gia'
     * legato, il fire in volo appartiene alla contabilita' degli sharer
     * esistenti: timer e pending_done non si toccano. */
    if (line->sharer_count == 0)
    {
        timer_cancel(&line->safety_timer);
        line->masked       = false;
        line->pending_done = 0;
    }
    line->probe_armed   = false;
    line->probe_backoff = false;

    pci_binding_put(bus, slot, func, (uint8_t)gsi);
    p->used = false;
    if (s_pci_pending_count > 0)
    {
        s_pci_pending_count--;
    }

    int32_t r = irq_attach_sharer(gsi, proc, port);
    kprintf("[IRQ ] 0:%u.%u: GSI %u rivendicato empiricamente (PID %d)\n",
            slot, func, gsi, proc->pid);

    /* Se nessun altro driver e' pendente, disarma i GSI ancora
     * non legati (niente sorgente non posseduta che tempesta). */
    if (s_pci_pending_count == 0)
    {
        pci_discovery_disarm_unbound();
    }
    spinlock_release_irqrestore(&s_irqfwd_lock, lfl);
    return (r != 0) ? -1 : (int32_t)gsi;
}

static int32_t sys_irq_find_free(isr_regs_t *regs UNUSED)
{
    if (!caller_is_driver())
    {
        return -1;
    }
    return irq_discovery_find_free(process_current()->pid);
}

static int32_t sys_irq_wire_device(isr_regs_t *regs)
{
    if (!caller_is_driver())
    {
        return -1;
    }
    uint32_t bsf = regs->ebx;
    uint8_t bus  = (uint8_t)((bsf >> 16) & 0xFFu);
    uint8_t slot = (uint8_t)((bsf >> 8) & 0x1Fu);
    uint8_t func = (uint8_t)(bsf & 0x07u);
    uint8_t line = (uint8_t)(regs->ecx & 0x0Fu);
    return pirq_wire_device(bus, slot, func, line);
}

/* ======================================================================
 * MSI (Message Signaled Interrupts)
 *
 * L'Address MSI x86 e' una finestra architetturale fissa verso il
 * Local APIC (dest APIC id nei bit 19:12); il Data porta il vettore
 * (delivery fixed, edge). Non e' l'MMIO del LAPIC: e' l'indirizzo di
 * redirezione che il northbridge decodifica.
 * ==================================================================== */

#define MSI_ADDR_BASE       0xFEE00000u
#define MSI_DATA_FIXED      0x0000u
#define MSI_DATA_EDGE       0x0000u
#define MSI_CTRL_ENABLE     0x0001u
#define MSI_CTRL_64BIT      0x0080u

static void msi_forward_handler(isr_regs_t *regs)
{
    uint32_t vector = regs->vector;
    if (vector < 256 && s_msi_notify_port[vector] != 0)
    {
        ipc_notify(s_msi_notify_port[vector], (1u << (vector & 31)));
    }
}

static int32_t sys_msi_enable(isr_regs_t *regs)
{
    if (!caller_is_driver())
    {
        return -1;
    }
    process_t *proc = process_current();

    uint32_t bsf  = regs->ebx;
    uint8_t  bus  = (uint8_t)((bsf >> 16) & 0xFFu);
    uint8_t  slot = (uint8_t)((bsf >> 8) & 0x1Fu);
    uint8_t  func = (uint8_t)(bsf & 0x07u);
    uint32_t cap  = regs->ecx & 0xFFCu;
    uint32_t port = regs->edx;

    uint32_t cap_hdr = kpci_read32(bus, slot, func, cap);
    if ((cap_hdr & 0xFFu) != 0x05u)     /* id capability MSI = 0x05 */
    {
        kprintf("[MSI ] %02x:%02x.%u cap@0x%x non e' MSI (id 0x%02x)\n",
                bus, slot, func, cap, cap_hdr & 0xFFu);
        return -1;
    }

    int32_t vector = intr_alloc_vector(proc->pid);
    if (vector < 0)
    {
        kprintf("[MSI ] nessun vettore libero per %02x:%02x.%u\n",
                bus, slot, func);
        return -1;
    }

    uint16_t ctrl  = (uint16_t)(cap_hdr >> 16);
    bool     is_64 = (ctrl & MSI_CTRL_64BIT) != 0;

    /* Destinazione = la HOME del driver, come gia' fa lo steering
     * IOAPIC delle linee legacy (intr_route_line_to_cpu): l'ISR e il
     * wake IPC girano dove il driver vive — niente rimbalzo cross-core
     * per ogni interrupt, e il carico interrupt segue la distribuzione
     * dei driver invece di accumularsi tutto sulla BSP. Bit 19:12
     * dell'indirizzo MSI = APIC ID fisico di destinazione. */
    uint32_t dest_apic = (uint32_t)g_cpus[proc->home_cpu].apic_id;
    uint32_t msg_addr  = MSI_ADDR_BASE | (dest_apic << 12);
    uint32_t msg_data  = (uint32_t)vector | MSI_DATA_FIXED | MSI_DATA_EDGE;

    kpci_write32(bus, slot, func, cap + 0x04, msg_addr);
    if (is_64)
    {
        kpci_write32(bus, slot, func, cap + 0x08, 0);
        kpci_write32(bus, slot, func, cap + 0x0C, msg_data);
    }
    else
    {
        kpci_write32(bus, slot, func, cap + 0x08, msg_data);
    }

    /* Handler e porta PRIMA dell'enable: nessun interrupt puo' arrivare
     * a un vettore senza handler. */
    s_msi_notify_port[vector] = port;
    intr_set_vector_handler((uint8_t)vector, msi_forward_handler);

    uint32_t hdr_new = (cap_hdr & 0x0000FFFFu) |
                       ((uint32_t)(ctrl | MSI_CTRL_ENABLE) << 16);
    kpci_write32(bus, slot, func, cap, hdr_new);

    kprintf("[MSI ] %02x:%02x.%u abilitato, vettore 0x%x (%s-bit)\n",
            bus, slot, func, vector, is_64 ? "64" : "32");
    return vector;
}

/* ======================================================================
 * Integrazione
 * ==================================================================== */

void driver_syscalls_init(void)
{
    /* Init-once dei timer del layer: da qui in poi solo arm/cancel
     * (contratto di timer_init: mai su un timer potenzialmente
     * armato).
     *   - ktimer del pool asincrono: fn/arg fissati per slot;
     *   - safety_timer di TUTTE le linee IRQ: prima erano
     *     inizializzati solo dal discovery-arm dei GSI PCI — le linee
     *     legacy (tastiera, mouse, ata, ac97) armavano un timer
     *     AZZERATO a ogni fire: heap_idx 0 spurio -> detach della
     *     testa dell'heap = scadenze ALTRUI cancellate in silenzio. */
    for (int i = 0; i < TIMER_POOL_MAX; i++)
    {
        timer_init(&s_timer_pool[i].ktimer, async_timer_fire,
                   &s_timer_pool[i]);
    }
    for (uint32_t l = 0; l < MAX_IRQ_FORWARD; l++)
    {
        timer_init(&irq_lines[l].safety_timer, irq_line_watchdog,
                   (void *)(uintptr_t)l);
    }
    /*   - timer del pool watchdog: fn/arg fissati per slot. Prima erano
     *     (re)inizializzati nel ramo di riuso di sys_watchdog_set, mentre
     *     la callback dello slot poteva essere in volo (watchdog_fire
     *     libera used PRIMA di finire): stessa race del pool asincrono.
     *     Init-once qui; sul riuso si arma soltanto (arm_locked gestisce
     *     il ri-armo: detach verificato + gen++ sui fire stantii). */
    for (int i = 0; i < WATCHDOG_MAX; i++)
    {
        timer_init(&s_watchdogs[i].timer, watchdog_fire, &s_watchdogs[i]);
    }

    syscall_register(SYS_IO_PORT_IN,      sys_io_port_in);
    syscall_register(SYS_IO_PORT_OUT,     sys_io_port_out);
    syscall_register(SYS_IRQ_REGISTER,    sys_irq_register);
    syscall_register(SYS_IRQ_DONE,        sys_irq_done);
    syscall_register(SYS_MMAP_PHYS,       sys_mmap_phys);
    syscall_register(SYS_DMA_ALLOC,       sys_dma_alloc);
    syscall_register(SYS_DMA_FREE,        sys_dma_free);
    syscall_register(SYS_DMA_VERIFY,      sys_dma_verify);
    syscall_register(SYS_DEVICE_CLAIM,    sys_device_claim);
    syscall_register(SYS_DEVICE_RELEASE,  sys_device_release);
    syscall_register(SYS_TIMER_SET,       sys_timer_set);
    syscall_register(SYS_TIMER_CANCEL,    sys_timer_cancel);
    syscall_register(SYS_WATCHDOG_SET,    sys_watchdog_set);
    syscall_register(SYS_PCI_READ,        sys_pci_read);
    syscall_register(SYS_PCI_WRITE,       sys_pci_write);
    syscall_register(SYS_ENTER_CRITICAL,  sys_enter_critical);
    syscall_register(SYS_EXIT_CRITICAL,   sys_exit_critical);

    syscall_register(SYS_REGISTER_VIDEO_DRIVER, sys_register_video_driver);
    syscall_register(SYS_IRQ_FIND_FREE,         sys_irq_find_free);
    syscall_register(SYS_IRQ_WIRE_DEVICE,       sys_irq_wire_device);
    syscall_register(SYS_MSI_ENABLE,            sys_msi_enable);
    syscall_register(SYS_IRQ_REGISTER_PCI,      sys_irq_register_pci);
    syscall_register(SYS_IRQ_PCI_CLAIM,         sys_irq_pci_claim);

    irq_discovery_init();
    video_boomerang_init();
    kprintf("[DRV ] Layer driver: 23 syscall attive (INTx empirico + MSI).\n");
}

/* Teardown alla morte del processo (chiamato da process_destroy):
 * event-driven, mai spazzato da un timer. */
void driver_cleanup_process(pid_t pid)
{
    irq_cleanup(pid);
    video_boomerang_cleanup(pid);
    dma_cleanup(pid);
    dev_claim_cleanup(pid);
    timer_pool_cleanup(pid);
    watchdog_cleanup(pid);
}
