/* MainDOB Hotplug Manager
 *
 * Real-time hardware detection, driver matching, and bubble lifecycle.
 * Every device+driver pair lives in an isolated "bubble" that can be
 * created and torn down independently.
 *
 * Driver mappings are loaded from /SYSTEM/CONFIG/DAS/<name>.das at
 * startup. Each .das file describes one supported device class: its hardware
 * fingerprint, the driver to spawn, the service name, and (for GUI
 * devices) the desktop icon and action sequences. Adding a driver
 * means dropping a .das file and the corresponding .mdl in place; no
 * recompilation, no edits to this module.
 *
 * The algorithm:
 *   1. Load DAS database from /SYSTEM/CONFIG/DAS/
 *   2. Scan PCI bus → build hardware table
 *   3. For each device, das_match() picks the most specific .das
 *      whose signature fits, and we spawn the driver it names.
 *   4. Enter main loop: IPC handling + reaping
 *   5. On rescan: diff old vs new, create/teardown bubbles
 *
 * IPC protocol:
 *   100 ATTACH         (to driver)  payload=hotplug_device_t
 *   101 DETACH         (to driver)
 *   110 READY          (from driver) arg0=service_port
 *   111 RELEASED       (from driver)
 *   112 FAILED         (from driver)
 *   200 LIST           (query) -> reply.payload=bubble_info[]
 *   201 FIND_DEVICE    (query) arg0=vendor arg1=device -> bubble_info
 *   202 RESCAN         (force rescan)
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/spawn.h>
#include <dob/types.h>
#include <dob/hotplug.h>
#include <dob/hotplug_events.h>
#include <dob/device_icon.h>
#include <DobSettings.h>
#include <DobPopup.h>   /* runner failure popups */         /* declareSetting/getSetting for show_system_partition */
#include <DobFileSystem.h>       /* dobfs_GetMountedOn for root-partition probe */

#include "das.h"

/* Configuration */

#define MAX_BUBBLES     64
#define MAX_HW_DEVICES  128
#define DETACH_TIMEOUT  2000    /* ms to wait for driver RELEASED */

/*  * Data structures
 *
 * hw_device_t is shared via <dob/hotplug_events.h> so subscribers
 * (dobinterface) deserialize broadcast payloads with the same layout.
 * bubble_t is private to this module.
 */

typedef struct
{
    uint8_t         state;
    uint32_t        id;
    hw_device_t     hw;
    char            driver_name[128];
    char            service_name[32];
    int32_t         driver_pid;
    uint32_t        driver_port;
    uint32_t        driver_gen;    /* incarnazione della driver_port al bind:
                                    * la DETACH la verifica (anti-ABA) contro
                                    * un id riciclato da un altro processo */
    uint32_t        service_port;
    uint32_t        detach_deadline;
    bool            legacy;        /* true = legacy static bubble (no driver spawn) */
    int16_t         das_idx;       /* DAS entry index, -1 if no match */

    /* Fallback-chain tracking. When a driver fails (FAILED message
     * or process death during ATTACH), hotplug walks the candidate
     * list to find the next-best DAS for the same hardware and
     * re-spawns. `das_candidates` is the sorted list (most specific
     * first) and `next_candidate` is the index of the *next* DAS to
     * try if the current one fails. When next_candidate >= count, the
     * fallback chain is exhausted and the bubble truly dies. */
    int             das_candidates[8];
    uint8_t         das_candidate_count;
    uint8_t         next_candidate;

    /* BUS_TYPE_SUBDEVICE only — identity of the inner device inside its
     * provider (e.g. AHCI port index). Conserved so that ICON_ACTIVATED
     * and MENU_ACTIVATED can rebuild the das_run_ctx_t that the DAS
     * action primitives expect. Ignored for PCI and legacy bubbles. */
    char            provider_service[32];
    uint32_t        provider_token;
} bubble_t;

/* Global state */

static hw_device_t hw_table[MAX_HW_DEVICES];
static uint32_t    hw_count = 0;

static bubble_t    bubbles[MAX_BUBBLES];
static uint32_t    next_bubble_id = 1;
static int32_t     my_port = -1;

/* Tool functions — PCI bus access */

/* PCI config read. Delegates to the kernel via pci_config_read() rather
 * than poking 0xCF8/0xCFC from userspace directly. The reason is ECAM:
 * the kernel's SYS_PCI_READ picks the memory-mapped path automatically
 * on PCI Express machines (where an MCFG region covers the bus) and the
 * legacy port path everywhere else. Reading config space from userspace
 * via the ports would stay capped at the first 256 bytes and miss the
 * extended capabilities that live above it on PCIe parts.
 *
 * Offset stays uint8_t here because every current caller reads standard
 * (sub-256) config registers; code that needs extended space (offsets
 * 256..4095) should call pci_config_read() directly with a wide offset. */
static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    return pci_config_read(bus, dev, func, off & 0xFC);
}

static void scan_one_function(uint8_t bus, uint8_t dev, uint8_t func,
                               hw_device_t *table, uint32_t *count, uint32_t max)
{
    if (*count >= max) return;
    uint32_t reg0 = pci_read(bus, dev, func, 0x00);
    uint16_t vendor = reg0 & 0xFFFF;
    if (vendor == 0xFFFF || vendor == 0x0000) return;

    hw_device_t *d = &table[(*count)++];
    d->vendor_id  = vendor;
    d->device_id  = (reg0 >> 16) & 0xFFFF;
    uint32_t reg2 = pci_read(bus, dev, func, 0x08);
    d->class_code = (reg2 >> 24) & 0xFF;
    d->subclass   = (reg2 >> 16) & 0xFF;
    d->prog_if    = (reg2 >> 8) & 0xFF;
    d->revision   = reg2 & 0xFF;
    /* Subsystem IDs live at config offset 0x2C (vendor) / 0x2E (device).
     * For type-0 headers only — type-1 (PCI bridges) and type-2
     * (CardBus) put other things there. We read them unconditionally
     * because hotplug's scan filter rejects bridges before reaching
     * here, but a defensive 0 is fine on weird hardware. */
    uint32_t reg11 = pci_read(bus, dev, func, 0x2C);
    d->subsystem_vendor_id = reg11 & 0xFFFF;
    d->subsystem_device_id = (reg11 >> 16) & 0xFFFF;
    d->bus  = bus;
    d->slot = dev;
    d->func = func;
    d->irq_line = pci_read(bus, dev, func, 0x3C) & 0xFF;
    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_read(bus, dev, func, 0x10 + i * 4);
    d->present = true;
}

static void pci_scan_all(hw_device_t *table, uint32_t *count, uint32_t max)
{
    *count = 0;
    for (uint32_t bus = 0; bus < 256; bus++)
    {
        for (uint8_t dev = 0; dev < 32; dev++)
        {
            uint32_t reg0 = pci_read((uint8_t)bus, dev, 0, 0x00);
            if ((reg0 & 0xFFFF) == 0xFFFF) continue;

            scan_one_function((uint8_t)bus, dev, 0, table, count, max);

            uint32_t hdr = pci_read((uint8_t)bus, dev, 0, 0x0C);
            if ((hdr >> 16) & 0x80)
                for (uint8_t f = 1; f < 8; f++)
                    scan_one_function((uint8_t)bus, dev, f, table, count, max);
        }
    }
}

/* Tool functions — manifest matching */

static bool hw_same(const hw_device_t *a, const hw_device_t *b)
{
    if (a->bus_type != b->bus_type) return false;

    if (a->bus_type == BUS_TYPE_LEGACY_FDC)
        return a->io_base == b->io_base;

    if (a->bus_type == BUS_TYPE_LEGACY_IDE_ATAPI)
        /* IDE ATAPI bubbles are keyed by channel base + slot index.
         * The slot index is stored in hw.func like the floppy does. */
        return a->io_base == b->io_base && a->func == b->func;

    if (a->bus_type == BUS_TYPE_SUBDEVICE)
        /* SUBDEVICE identity is (provider_service, provider_token). The
         * hw descriptor alone is not enough — hw is class-level and
         * identical across all sub-devices of the same class. Callers
         * that need to find a specific SUBDEVICE bubble should go
         * through bubble_find_by_subdev, defined below. */
        return false;

    /* PCI: bus:slot:func + vendor:device */
    return a->bus == b->bus && a->slot == b->slot && a->func == b->func &&
           a->vendor_id == b->vendor_id && a->device_id == b->device_id;
}

/* Tool functions — bubble lifecycle */

static bubble_t *bubble_alloc(void)
{
    for (int i = 0; i < MAX_BUBBLES; i++)
        if (bubbles[i].state == BUBBLE_EMPTY)
            return &bubbles[i];
    return NULL;
}

static bubble_t *bubble_find_by_hw(const hw_device_t *hw)
{
    for (int i = 0; i < MAX_BUBBLES; i++)
        if (bubbles[i].state != BUBBLE_EMPTY && hw_same(&bubbles[i].hw, hw))
            return &bubbles[i];
    return NULL;
}

static bubble_t *bubble_find_by_pid(int32_t pid)
{
    for (int i = 0; i < MAX_BUBBLES; i++)
        if (bubbles[i].state != BUBBLE_EMPTY && bubbles[i].driver_pid == pid)
            return &bubbles[i];
    return NULL;
}

/* Spawn the driver named in DAS entry `dx` for the given hardware,
 * pre-populated bubble `b`. Caller has already filled b->hw, b->id,
 * b->das_candidates etc. Returns true on successful spawn (driver
 * process started — actual ATTACH is async). */
static bool
bubble_spawn_for_das(bubble_t *b, int dx)
{
    const das_entry_t *de = das_get(dx);
    if (!de || !de->driver[0]) return false;

    b->das_idx = (int16_t)dx;
    b->state   = BUBBLE_SPAWNING;
    strncpy(b->driver_name, de->driver, sizeof(b->driver_name) - 1);
    b->driver_name[sizeof(b->driver_name) - 1] = '\0';

    if (de->service[0])
    {
        strncpy(b->service_name, de->service, sizeof(b->service_name) - 1);
        b->service_name[sizeof(b->service_name) - 1] = '\0';
    }
    else
    {
        b->service_name[0] = '\0';
    }

    /* Route through spawn_file (DobFS) rather than legacy spawn()
     * (kernel bootfs / direct ATA PIO).  Hotplug runs concurrently with
     * the userspace ATA driver, and any direct bootfs read at this point
     * races with the driver on shared IDE registers — corrupting
     * dir_buf and producing spurious "Not found" failures.  spawn_file
     * goes through DobFileSystem IPC which already serializes access. */
    b->driver_pid = spawn_file_sync(de->driver, NULL);
    if (b->driver_pid < 0)
    {
        printf("[hotplug] Failed to spawn %s\n", de->driver);
        return false;
    }

    /* INVARIANTE anti-ABA: il kernel garantisce che un PID e' unico fra
     * i VIVI — se il figlio appena nato ha il PID X, ogni ALTRA bolla
     * che dichiara X parla di un morto (il suo driver e' uscito e il
     * PID e' stato riciclato prima del giro del reaper). Va ritirata
     * ORA, non al prossimo reap: (1) proc_status(X) da qui in poi vede
     * VIVO il nuovo figlio, quindi il reaper non la ritirerebbe MAI;
     * (2) bubble_find_by_pid scandisce in ordine d'array e la bolla
     * stantia, se precede, INTERCETTA l'attach del nuovo driver — che
     * si ritrova a pilotare il DEVICE SBAGLIATO (visto dal ferro: uhci
     * attaccato al BDF dell'AHCI, controller muto sulla linea 20). */
    for (int i = 0; i < MAX_BUBBLES; i++)
    {
        bubble_t *o = &bubbles[i];
        if (o != b && o->state != BUBBLE_EMPTY &&
            o->driver_pid == b->driver_pid)
        {
            printf("[hotplug] Bubble #%u: PID %d riciclato dal nuovo "
                   "spawn, bolla stantia ritirata\n", o->id, o->driver_pid);
            memset(o, 0, sizeof(bubble_t));
        }
    }

    make_driver(b->driver_pid);

    /* That's it. No ATTACH sent here.
     * The driver will call dob_driver_attach() which sends HOTPLUG_READY
     * to us. We reply with the device info. This is race-free: the driver
     * controls the timing, not a hardcoded sleep. */
    b->state = BUBBLE_ATTACHING;
    printf("[hotplug] Bubble #%u: spawned %s (PID %d) for %04x:%04x\n",
           b->id, de->driver, b->driver_pid,
           b->hw.vendor_id, b->hw.device_id);
    return true;
}

/* Spawn driver, send ATTACH, transition to ATTACHING */
static void
bubble_create(const hw_device_t *hw)
{
    if (bubble_find_by_hw(hw)) return;

    /* Gather every DAS that matches this device, sorted by specificity
     * (most specific first). A device with no matching DAS is silently
     * ignored. */
    int candidates[8];
    int n = das_match_all(hw, candidates, 8);

    /* Diagnostic: report what the matcher decided for every PCI device.
     * Helps spotting cases where a previously-supported device suddenly
     * has no DAS match. */
    if (hw->bus_type == BUS_TYPE_PCI)
    {
        printf("[hotplug] PCI %02x:%02x.%x  %04x:%04x  cls %02x:%02x:%02x"
               "  -> %d DAS candidate(s)\n",
               hw->bus, hw->slot, hw->func,
               hw->vendor_id, hw->device_id,
               hw->class_code, hw->subclass, hw->prog_if, n);
    }

    if (n <= 0) return;

    bubble_t *b = bubble_alloc();
    if (!b) { printf("[hotplug] No free bubble slots!\n"); return; }

    memset(b, 0, sizeof(bubble_t));
    b->id = next_bubble_id++;
    b->hw = *hw;

    b->das_candidate_count = (uint8_t)n;
    for (int i = 0; i < n; i++) b->das_candidates[i] = candidates[i];
    b->next_candidate = 1;   /* if [0] fails, try [1] next */

    if (!bubble_spawn_for_das(b, candidates[0]))
    {
        /* spawn_file_sync failed (the .mdl is missing or corrupt).
         * Try the next candidate immediately if there is one. */
        while (b->next_candidate < b->das_candidate_count)
        {
            int dx = b->das_candidates[b->next_candidate++];
            if (bubble_spawn_for_das(b, dx)) return;
        }
        /* All candidates exhausted. Free the slot. */
        b->state = BUBBLE_EMPTY;
    }
}

/* Try to revive a failing bubble with the next DAS candidate. Called
 * from the driver-failure paths (FAILED message, ATTACH-time crash).
 * Returns true if a fallback was started, false if the chain is
 * exhausted (caller should mark the bubble DEAD as before). */
static bool
bubble_try_next_candidate(bubble_t *b)
{
    /* Reap the previous driver's zombie, if any, before starting a
     * new one in this slot. Status 2 = exited-but-not-waited. */
    if (b->driver_pid > 0)
    {
        int status = proc_status(b->driver_pid);
        if (status == 2) waitpid(b->driver_pid);
    }

    while (b->next_candidate < b->das_candidate_count)
    {
        int dx = b->das_candidates[b->next_candidate++];
        const das_entry_t *de = das_get(dx);
        if (!de || !de->driver[0]) continue;

        printf("[hotplug] Bubble #%u: falling back to %s\n",
               b->id, de->driver);

        /* Reset transient driver state — bubble identity stays */
        b->driver_pid    = 0;
        b->driver_port   = 0;
        b->service_port  = 0;
        b->driver_name[0]  = '\0';
        b->service_name[0] = '\0';

        if (bubble_spawn_for_das(b, dx)) return true;
        /* spawn failed too — try the one after that */
    }
    return false;
}

/* Tell driver to release resources, start timeout */
static void bubble_teardown(bubble_t *b)
{
    if (b->state == BUBBLE_DETACHING || b->state == BUBBLE_DEAD) return;

    printf("[hotplug] Detaching bubble #%u (%s)\n", b->id, b->driver_name);
    b->state = BUBBLE_DETACHING;
    b->detach_deadline = (uint32_t)clock_ms() + DETACH_TIMEOUT;

    /* Unregister service so no new clients connect */
    if (b->service_port && b->service_name[0])
    {
        dob_registry_unregister(b->service_name);
    }

    /* Send DETACH to driver */
    if (b->driver_port)
    {
        /* Verifica anti-ABA: se la porta del driver e' stata riciclata
         * da un altro processo (driver morto/respawn durante il boot),
         * la generazione non combacia piu' -> NON mandare il DETACH a un
         * estraneo. Salta pulito. */
        if (b->driver_gen != 0 &&
            dob_ipc_port_generation(b->driver_port) != b->driver_gen)
        {
            /* porta riciclata: il driver di prima non esiste piu' */
        }
        else
        {
        dob_msg_t msg = {0}, reply = {0};
        msg.code = HOTPLUG_DETACH;
        dob_ipc_call(b->driver_port, &msg, &reply);

        if (reply.code == HOTPLUG_RELEASED)
        {
            /* Driver cleaned up immediately — good */
            kill(b->driver_pid);
            b->state = BUBBLE_DEAD;
        }
        }
    }
    else
    {
        /* No port — just kill */
        kill(b->driver_pid);
        b->state = BUBBLE_DEAD;
    }
}

/* Force-kill bubbles that missed their detach deadline */
static uint32_t
bubble_reap_dead(void)
{
    uint32_t now = (uint32_t)clock_ms();
    uint32_t reaped = 0;

    for (int i = 0; i < MAX_BUBBLES; i++)
    {
        bubble_t *b = &bubbles[i];

        if (b->state == BUBBLE_DETACHING && now >= b->detach_deadline)
        {
            printf("[hotplug] Bubble #%u: driver unresponsive, force-killing PID %d\n",
                   b->id, b->driver_pid);
            kill(b->driver_pid);
            b->state = BUBBLE_DEAD;
        }

        if (b->state == BUBBLE_DEAD)
        {
            memset(b, 0, sizeof(bubble_t));
            reaped++;
        }

        /* Check if driver process died unexpectedly */
        if (b->state == BUBBLE_LIVE || b->state == BUBBLE_ATTACHING)
        {
            int status = proc_status(b->driver_pid);
            if (status == 0 || status == 2)
            {
                if (status == 2) waitpid(b->driver_pid);

                /* If the driver died DURING ATTACH (never reached LIVE)
                 * try the next candidate. Crashes after LIVE are not
                 * retried — the driver was working when it failed, so
                 * a different driver from the chain is unlikely to be
                 * the right fix and might be in worse shape after the
                 * previous one's partial init. */
                if (b->state == BUBBLE_ATTACHING &&
                    bubble_try_next_candidate(b))
                {
                    printf("[hotplug] Bubble #%u: ATTACH crash, falling back\n",
                           b->id);
                    /* fallback spawn started; bubble stays alive */
                    continue;
                }

                /* A driver that died while still ATTACHING (never sent
                 * HOTPLUG_READY, never went LIVE) with no further candidate
                 * is not a crash of a working driver — it bowed out during
                 * bring-up. The canonical case is a singleton driver like
                 * AHCI: hotplug spawns one instance per matching controller,
                 * but only the first to register the service stays; the
                 * extras exit at their duplicate guard (before attach, so
                 * the bubble is still ATTACHING). That is expected and must
                 * not be logged as "crashed". Only a death in LIVE state is
                 * a genuine driver crash. */
                if (b->state == BUBBLE_ATTACHING)
                {
                    printf("[hotplug] Bubble #%u: driver exited during attach "
                           "(PID %d), bubble retired\n", b->id, b->driver_pid);
                }
                else
                {
                    printf("[hotplug] Bubble #%u: driver crashed (PID %d), bubble destroyed\n",
                           b->id, b->driver_pid);
                }
                memset(b, 0, sizeof(bubble_t));
                reaped++;
            }
        }
    }

    return reaped;
}

/* Tool functions — rescan and diff */

/* Forward declarations — publish_* are defined further below with the
 * subscriber broadcast block, but pci_rescan_and_diff needs to call
 * them when diffing PCI state. */
static void publish_appeared(const bubble_t *b);
static void publish_gone(uint32_t device_id);

/* Returns number of changes (appeared + disappeared) */
static uint32_t pci_rescan_and_diff(void)
{
    hw_device_t new_table[MAX_HW_DEVICES];
    uint32_t new_count = 0;
    uint32_t changes = 0;

    pci_scan_all(new_table, &new_count, MAX_HW_DEVICES);

    /* Detect disappeared: in old table but not in new */
    for (uint32_t i = 0; i < hw_count; i++)
    {
        bool found = false;
        for (uint32_t j = 0; j < new_count; j++)
            if (hw_same(&hw_table[i], &new_table[j])) { found = true; break; }

        if (!found)
        {
            bubble_t *b = bubble_find_by_hw(&hw_table[i]);
            if (b)
            {
                publish_gone(b->id);
                bubble_teardown(b);
                changes++;
            }
        }
    }

    /* Detect appeared: in new table but not in old */
    for (uint32_t j = 0; j < new_count; j++)
    {
        bool found = false;
        for (uint32_t i = 0; i < hw_count; i++)
            if (hw_same(&hw_table[i], &new_table[j])) { found = true; break; }

        if (!found)
        {
            printf("[hotplug] New device: %04x:%04x class %02x:%02x\n",
                   new_table[j].vendor_id, new_table[j].device_id,
                   new_table[j].class_code, new_table[j].subclass);
            bubble_create(&new_table[j]);
            bubble_t *nb = bubble_find_by_hw(&new_table[j]);
            if (nb) publish_appeared(nb);
            changes++;
        }
    }

    /* Update hw_table to current state */
    memcpy(hw_table, new_table, new_count * sizeof(hw_device_t));
    hw_count = new_count;

    return changes;
}

/*  * Subscriber broadcast — APPEARED / GONE events
 *
 * Subscribers (dobinterface is the primary one) register via
 * HOTPLUG_SUBSCRIBE. They receive a replay of every currently known
 * device at subscribe time, then live broadcasts thereafter. We only
 * store subscriber port IDs; they never time out, and a crashed
 * subscriber just means the next dob_ipc_post silently fails.
 */

#define MAX_SUBSCRIBERS        4
#define BUBBLE_DETECTED        6   /* Legacy static bubble, no driver spawn */

static uint32_t subscribers[MAX_SUBSCRIBERS];
static int      subscriber_count = 0;

static void publish_appeared(const bubble_t *b)
{
    if (subscriber_count == 0) return;
    if (b->das_idx < 0) return;   /* No DAS match -> no desktop icon. */

    /* kind=system devices have no UI presence: the driver is spawned
     * automatically at match time and that is the whole story. Skip
     * the GUI_DEVICE_ATTACH so no icon shows up. */
    const das_entry_t *de = das_get(b->das_idx);
    if (de && de->kind == DEV_KIND_SYSTEM) return;

    static gui_device_attach_t payload;
    if (das_fill_attach_payload(b->das_idx, b->id, b->hw.func, &payload) < 0) return;

    for (int i = 0; i < subscriber_count; i++)
    {
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.code         = GUI_DEVICE_ATTACH;
        msg.payload      = &payload;
        msg.payload_size = sizeof(payload);
        dob_ipc_post(subscribers[i], &msg);
    }
}

static void publish_gone(uint32_t device_id)
{
    if (subscriber_count == 0) return;

    static gui_device_detach_t payload;
    payload.device_id = device_id;

    for (int i = 0; i < subscriber_count; i++)
    {
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.code         = GUI_DEVICE_DETACH;
        msg.payload      = &payload;
        msg.payload_size = sizeof(payload);
        dob_ipc_post(subscribers[i], &msg);
    }
}

/* Create a legacy static bubble — used at boot for the floppy
 * controller. Legacy bubbles never get a driver spawned at detection
 * time; they sit in BUBBLE_DETECTED until someone decides to mount
 * them on user action. They are not polled, not probed at runtime,
 * and never removed — the floppy drive simply exists for the whole
 * session. The `unit` parameter encodes the drive sub-index (0 or 1
 * for floppies); it is stored in hw.func so that DAS variable
 * substitution ($unit, $unit1) and per-unit IPC routing work
 * uniformly with the PCI path. */
static bubble_t *legacy_bubble_create(uint8_t bus_type, uint16_t io_base,
                                      uint8_t unit)
{
    bubble_t *b = bubble_alloc();
    if (!b) { printf("[hotplug] No free bubble slots for legacy!\n"); return NULL; }

    memset(b, 0, sizeof(bubble_t));
    b->id       = next_bubble_id++;
    b->state    = BUBBLE_DETECTED;
    b->legacy   = true;
    b->hw.bus_type = bus_type;
    b->hw.io_base  = io_base;
    b->hw.func     = unit;
    b->hw.present  = true;
    b->das_idx     = (int16_t)das_match(&b->hw);

    printf("[hotplug] Legacy bubble #%u: bus_type=%u io_base=0x%x unit=%u\n",
           b->id, bus_type, io_base, unit);
    return b;
}

/* Handle HOTPLUG_CREATE_LEGACY_BUBBLE — a one-shot helper (see
 * boot/floppyprobe) sends this during early boot, once per legacy
 * device it detected. We create the static bubble and broadcast
 * APPEARED to any subscriber already listening. Hotplug carries no
 * bus-specific probing itself: all "this hardware exists" knowledge
 * lives in the helper that sends this message.
 *
 * Called from the main dispatcher, not at init time. Arrives after
 * dobinterface has (or hasn't) already subscribed; publish_appeared
 * handles both cases. */
static void handle_create_legacy_bubble(dob_msg_t *msg)
{
    if (!msg->payload || msg->payload_size < sizeof(hotplug_legacy_create_t))
        return;

    hotplug_legacy_create_t req;
    memcpy(&req, msg->payload, sizeof(req));

    bubble_t *b = legacy_bubble_create(req.bus_type, req.io_base, req.unit);
    if (!b) return;

    publish_appeared(b);
}

/* Find a SUBDEVICE bubble by (provider_service, provider_token). Used
 * by the GONE handler to locate the matching bubble for teardown. */
static bubble_t *bubble_find_subdev(const char *provider_service,
                                    uint32_t provider_token)
{
    for (int i = 0; i < MAX_BUBBLES; i++)
    {
        bubble_t *b = &bubbles[i];
        if (b->state == BUBBLE_EMPTY) continue;
        if (b->hw.bus_type != BUS_TYPE_SUBDEVICE) continue;
        if (b->provider_token != provider_token) continue;
        if (strncmp(b->provider_service, provider_service,
                    sizeof(b->provider_service)) != 0) continue;
        return b;
    }
    return NULL;
}

/* Returns true iff this subdevice describes the partition MainDOB
 * booted from AND the user-visible "show_system_partition" setting
 * is off. Called on every SUBDEVICE_APPEARED for a filesystem
 * volume so the decision tracks setting changes at runtime — the
 * next RESCAN_PARTITIONS will hide or show the icon as appropriate. */
static bool subdev_is_hidden_root(const hotplug_subdev_appeared_t *req)
{
    if (req->sub.volume_fs == 0)            return false;
    if (req->sub.partition_start_lba == 0)  return false;

    /* User opt-in to show overrides everything. */
    const char *v = getSetting("show_system_partition");
    if (!v || strcmp(v, "false") != 0) return false;  /* default-or-true shows */

    /* Identify the running root mount. */
    dobfs_mounted_info_t info;
    if (dobfs_GetMountedOn("DobFileSystem", &info) != 0) return false;
    if (!info.is_root_mount)                              return false;
    if (strcmp(info.provider, req->sub.provider_service) != 0) return false;

    /* The subdevice token packs disk index in the low 24 bits (see
     * partition.c:make_token). Comparing it against info.selector
     * is essential — two partitions on different disks may share a
     * start_lba (2048 is the conventional first-usable sector), so
     * matching on lba alone wrongly flags any disk-N partition at
     * lba 2048 as "the root partition". */
    uint32_t sub_selector = req->sub.provider_token & 0x00FFFFFFu;
    if (info.selector != sub_selector) return false;

    return info.partition_lba == req->sub.partition_start_lba;
}

/* Handle HOTPLUG_SUBDEVICE_APPEARED: a bus driver (ahci.mdl at present)
 * tells us it has found, inside itself, a sub-device that deserves its
 * own desktop icon. This is the real-time path for CDROM media
 * insertion on AHCI: when the drive signals media-change via its GESN
 * asynchronous notification IRQ, the AHCI driver sends us this message
 * and we conjure a child bubble.
 *
 * We ignore duplicates (same provider+token already known): providers
 * occasionally re-fire APPEARED on reconnect, and we don't want ghost
 * bubbles stacking up. Subscribers get exactly one GUI_DEVICE_ATTACH
 * per unique (provider, token) pair. */
static void handle_subdev_appeared(dob_msg_t *msg)
{
    if (!msg->payload || msg->payload_size < sizeof(hotplug_subdev_appeared_t))
        return;

    hotplug_subdev_appeared_t req;
    memcpy(&req, msg->payload, sizeof(req));

    /* Honour the "show_system_partition" setting: if this is the
     * partition we booted from and the user has chosen to hide it,
     * accept the message silently — no bubble, no icon. The driver
     * sees DOB_OK back and doesn't care that we skipped. */
    if (subdev_is_hidden_root(&req))
    {
        printf("[hotplug] SUBDEV %s:%u is the root partition, hidden by setting\n",
               req.sub.provider_service, req.sub.provider_token);
        return;
    }

    /* Deduplicate */
    if (bubble_find_subdev(req.sub.provider_service, req.sub.provider_token))
    {
        printf("[hotplug] SUBDEV %s:%u already known, ignoring\n",
               req.sub.provider_service, req.sub.provider_token);
        return;
    }

    bubble_t *b = bubble_alloc();
    if (!b)
    {
        printf("[hotplug] No free bubble slots for subdev!\n");
        return;
    }

    memset(b, 0, sizeof(bubble_t));
    b->id               = next_bubble_id++;
    b->state            = BUBBLE_DETECTED;
    b->legacy           = true;   /* reuses the legacy teardown path */
    b->hw.bus_type      = BUS_TYPE_SUBDEVICE;
    b->hw.class_code    = req.sub.class_code;
    b->hw.subclass      = req.sub.subclass;
    b->hw.volume_fs     = req.sub.volume_fs;
    b->hw.partition_start_lba = req.sub.partition_start_lba;
    b->hw.present       = true;
    b->provider_token   = req.sub.provider_token;
    strncpy(b->provider_service, req.sub.provider_service,
            sizeof(b->provider_service) - 1);
    b->das_idx          = (int16_t)das_match(&b->hw);

    printf("[hotplug] SUBDEV bubble #%u: provider=%s token=%u class=%02x:%02x das=%d\n",
           b->id, b->provider_service, b->provider_token,
           b->hw.class_code, b->hw.subclass, (int)b->das_idx);

    publish_appeared(b);
}

/* Handle HOTPLUG_SUBDEVICE_GONE: the provider reports that its inner
 * sub-device has disappeared (AHCI: media ejected, or drive link lost).
 * We find the matching bubble and tear it down — this means broadcast
 * GUI_DEVICE_DETACH to every subscriber, then free the slot. If any
 * cdrom.mdl was spawned by the user against this bubble and is still
 * alive, its own DOBFS_UNMOUNT_NOTIFY path (triggered by the first
 * SCSI failure after ejection) will close the DobFiles window. */
static void handle_subdev_gone(dob_msg_t *msg)
{
    if (!msg->payload || msg->payload_size < sizeof(hotplug_subdev_gone_t))
        return;

    hotplug_subdev_gone_t req;
    memcpy(&req, msg->payload, sizeof(req));

    bubble_t *b = bubble_find_subdev(req.provider_service, req.provider_token);
    if (!b)
    {
        printf("[hotplug] SUBDEV_GONE: unknown %s:%u\n",
               req.provider_service, req.provider_token);
        return;
    }

    printf("[hotplug] SUBDEV bubble #%u gone (%s:%u)\n",
           b->id, b->provider_service, b->provider_token);

    publish_gone(b->id);
    b->state = BUBBLE_EMPTY;
}

/* Handle HOTPLUG_SUBSCRIBE: register the caller's port and immediately
 * replay every currently known device as an APPEARED event. */
static void handle_subscribe(dob_msg_t *msg)
{
    uint32_t port = msg->arg0;
    if (port == 0) return;

    /* Deduplicate — tolerant to repeated subscribe */
    for (int i = 0; i < subscriber_count; i++)
        if (subscribers[i] == port) return;

    if (subscriber_count >= MAX_SUBSCRIBERS)
    {
        printf("[hotplug] Subscriber table full, ignoring port %u\n", port);
        return;
    }

    subscribers[subscriber_count++] = port;
    printf("[hotplug] New subscriber: port %u (total %d)\n",
           port, subscriber_count);

    /* Replay current state to this single subscriber only.
     * We can't use publish_appeared() because it broadcasts to all. */
    int replayed = 0;
    for (int i = 0; i < MAX_BUBBLES; i++)
    {
        if (bubbles[i].state == BUBBLE_EMPTY) continue;
        if (bubbles[i].state == BUBBLE_DEAD)  continue;
        if (bubbles[i].das_idx < 0) continue;   /* no DAS -> no icon */

        /* kind=system bubbles have no UI presence — same filter as
         * publish_appeared(). Without this, a late subscriber (e.g.
         * dobinterface starting up) gets phantom icons for every
         * audio/video/NIC/HBA bubble already running. */
        const das_entry_t *de = das_get(bubbles[i].das_idx);
        if (de && de->kind == DEV_KIND_SYSTEM) continue;

        static gui_device_attach_t rep;
        if (das_fill_attach_payload(bubbles[i].das_idx,
                                    bubbles[i].id,
                                    bubbles[i].hw.func, &rep) < 0)
            continue;

        dob_msg_t m;
        memset(&m, 0, sizeof(m));
        m.code         = GUI_DEVICE_ATTACH;
        m.payload      = &rep;
        m.payload_size = sizeof(rep);
        dob_ipc_post(port, &m);
        replayed++;

        printf("[hotplug] Replay icon for bubble #%u state=%u bus_type=%u io=0x%x\n",
               bubbles[i].id, bubbles[i].state,
               bubbles[i].hw.bus_type, bubbles[i].hw.io_base);
    }
    printf("[hotplug] Subscribe replay complete: %d icons\n", replayed);
}

/* Handle PROVIDER_EJECT_REQ: in this build, the floppy is a static
 * legacy bubble with no driver mounted, so "eject" is a semantic no-op
 * — we still look the bubble up and log the request, and for PCI
 * bubbles (which we do not expect to receive eject for in normal use)
 * we also do nothing. When the real floppy driver arrives in a future
 * build and the bubble state transitions to LIVE on mount, this
 * handler will forward the eject to the driver. Same handler, growing
 * responsibilities — zero throwaway code. */
static void handle_provider_eject(dob_msg_t *msg)
{
    if (!msg->payload || msg->payload_size < sizeof(provider_eject_req_t))
        return;

    const provider_eject_req_t *req =
        (const provider_eject_req_t *)msg->payload;

    bubble_t *target = NULL;
    for (int i = 0; i < MAX_BUBBLES; i++)
    {
        if (bubbles[i].state == BUBBLE_EMPTY) continue;
        if (bubbles[i].id == req->device_id) { target = &bubbles[i]; break; }
    }

    if (!target)
    {
        printf("[hotplug] Eject: unknown device_id %u\n", req->device_id);
        return;
    }

    if (target->state == BUBBLE_DETECTED)
    {
        /* Nothing to unmount. Icon stays, drive stays. */
        printf("[hotplug] Eject on detected-only bubble #%u: no-op\n",
               target->id);
        return;
    }

    /* Future: if state == BUBBLE_LIVE, forward DETACH to driver_port
     * and let the driver tear down. For this build we never reach
     * that state on legacy devices. */
    printf("[hotplug] Eject on bubble #%u state=%u: not yet implemented\n",
           target->id, target->state);
}

/* Handle ICON_ACTIVATED: dobinterface tells us the user double-clicked
 * (or chose "Apri" in the panel) the icon for `device_id`. We look up
 * the bubble, build a runtime context from its hardware facts, and
 * dispatch to the DAS action interpreter. The interpreter handles all
 * spawn/wait/probe/popup logic and error fallback. */
/* Build the DAS runtime context from a bubble. For BUS_TYPE_SUBDEVICE
 * bubbles, this also carries provider_service + provider_token so the
 * DAS action can expand $provider and $token into the "service:index"
 * payload the CDROM driver expects. For legacy/PCI bubbles the new
 * fields stay zero/empty and are simply not referenced by their
 * DAS files. */
static void build_run_ctx(const bubble_t *b, das_run_ctx_t *out)
{
    memset(out, 0, sizeof(*out));
    out->device_id = b->id;
    out->unit      = b->hw.func;       /* legacy: drive unit; pci: function */
    out->io_base   = b->hw.io_base;
    if (b->hw.bus_type == BUS_TYPE_SUBDEVICE)
    {
        strncpy(out->provider_service, b->provider_service,
                sizeof(out->provider_service) - 1);
        out->provider_token = b->provider_token;
    }
}

/* ===== Action runner (plug-and-play guarantee) =====
 *
 * Actions used to run INSIDE hotplug's event loop: every double-click
 * parked this process for the whole chain (spawn driver -> WAIT_READY
 * -> PREPARE_VOLUME -> wait_service -> OPEN_VIEW). While parked,
 * ATTACH/GONE announcements queued: icons stopped telling the truth,
 * later clicks died, and ANY weak link in the chain froze the whole
 * desktop (the field's "ghost icon / zombie" class — every instance of
 * it, whatever the leaf cause). The fix is the system's own bubble
 * philosophy applied to actions: each activation runs in a DISPOSABLE
 * RUNNER PROCESS (this same binary with --run-action). hotplug never
 * blocks again: icons stay truthful by construction, and a wedged
 * chain costs one leaked process + its own popup, not the desktop. */
#define HOTPLUG_MDL "/SYSTEM/OperatingSystem/hotplug/hotplug.mdl"

static void spawn_action_runner(int das_idx, int menu_idx,
                                const das_run_ctx_t *ctx)
{
    char a_idx[12], a_menu[12], a_dev[12], a_unit[12], a_io[12],
         a_tok[12], a_hij[12];
    sprintf(a_idx,  "%d", das_idx);
    sprintf(a_menu, "%d", menu_idx);
    sprintf(a_dev,  "%u", (unsigned)ctx->device_id);
    sprintf(a_unit, "%u", (unsigned)ctx->unit);
    sprintf(a_io,   "%u", (unsigned)ctx->io_base);
    sprintf(a_tok,  "%u", (unsigned)ctx->provider_token);
    sprintf(a_hij,  "%u", (unsigned)ctx->hijack_target_port);
    const char *av[] = { "--run-action", a_idx, a_menu, a_dev, a_unit,
                         a_io, ctx->provider_service[0]
                                   ? ctx->provider_service : "-",
                         a_tok, a_hij, NULL };
    /* spawn_file_DRIVER, not plain spawn_file: the runner must be a
     * driver process — sys_make_driver requires caller->is_driver, and
     * the action's own spawn step (usbms via spawn_file_driver) would
     * otherwise die with a silent -1. Field: "adesso non si apre
     * proprio" — runner alive, first step dead. hotplug is a driver,
     * so the promotion chain holds: hotplug -> runner -> usbms. */
    if (spawn_file_driver(HOTPLUG_MDL, av) < 0)
    {
        /* Degraded fallback: run inline (old behaviour) rather than
         * doing nothing. Spawn failure here is OOM-level trouble. */
        printf("[hotplug] action runner spawn FAILED; running inline\n");
        if (menu_idx < 0) das_run_action(das_idx, ctx);
        else              das_run_menu(das_idx, menu_idx, ctx);
    }
}

static void handle_icon_activated(dob_msg_t *msg)
{
    if (!msg->payload || msg->payload_size < sizeof(icon_activated_t))
        return;

    const icon_activated_t *req =
        (const icon_activated_t *)msg->payload;

    bubble_t *target = NULL;
    for (int i = 0; i < MAX_BUBBLES; i++)
    {
        if (bubbles[i].state == BUBBLE_EMPTY) continue;
        if (bubbles[i].id == req->device_id) { target = &bubbles[i]; break; }
    }

    if (!target)
    {
        printf("[hotplug] Activate: unknown device_id %u\n", req->device_id);
        return;
    }
    if (target->das_idx < 0)
    {
        printf("[hotplug] Activate: bubble #%u has no DAS\n", target->id);
        return;
    }

    das_run_ctx_t ctx;
    build_run_ctx(target, &ctx);
    /* Carry the activator's hijack target port into the DAS run
     * context — the ipc_call primitive forwards it as arg0 of every
     * driver call, and floppy/cdrom probe handlers route their
     * OpenMount call accordingly. Old senders (pre-extension) leave
     * the field at 0 by virtue of the payload struct's zero-init
     * default; that's the desktop-double-click behaviour. */
    ctx.hijack_target_port = req->hijack_target_port;

    printf("[hotplug] Activate bubble #%u via DAS idx=%d (runner)\n",
           target->id, (int)target->das_idx);
    spawn_action_runner((int)target->das_idx, -1, &ctx);
}

/* Handle MENU_ACTIVATED: dobinterface tells us the user picked entry
 * `menu_idx` from the right-side context panel for the icon belonging
 * to `device_id`. Same lookup as ICON_ACTIVATED, but we dispatch to
 * das_run_menu instead of das_run_action — the DAS file's declarative
 * menu {} section is the single source of truth for what each entry
 * does. dobinterface stays 100% agnostic. */
static void handle_menu_activated(dob_msg_t *msg)
{
    if (!msg->payload || msg->payload_size < sizeof(menu_activated_t))
        return;

    const menu_activated_t *req =
        (const menu_activated_t *)msg->payload;

    bubble_t *target = NULL;
    for (int i = 0; i < MAX_BUBBLES; i++)
    {
        if (bubbles[i].state == BUBBLE_EMPTY) continue;
        if (bubbles[i].id == req->device_id) { target = &bubbles[i]; break; }
    }

    if (!target)
    {
        printf("[hotplug] Menu: unknown device_id %u\n", req->device_id);
        return;
    }
    if (target->das_idx < 0)
    {
        printf("[hotplug] Menu: bubble #%u has no DAS\n", target->id);
        return;
    }

    das_run_ctx_t ctx;
    build_run_ctx(target, &ctx);

    printf("[hotplug] Menu bubble #%u DAS idx=%d entry=%u\n",
           target->id, (int)target->das_idx, req->menu_idx);
    spawn_action_runner((int)target->das_idx, (int)req->menu_idx, &ctx);
}

/* Tool functions — IPC message handling */

static void
handle_driver_ready(dob_msg_t *msg, dob_msg_t *reply)
{
    bubble_t *b = bubble_find_by_pid(msg->sender_pid);
    if (!b)
    {
        reply->code = -1;
        return;
    }

    b->service_port = msg->arg0;
    b->driver_port = msg->arg0;
    /* Incarnazione della porta del driver: il DETACH la verifica, cosi'
     * un id riciclato da un altro processo non riceve il nostro DETACH
     * (anti-ABA, come dobinterface con owner_gen). Il driver e' vivo e
     * bloccato in questo ATTACH: gen stabile qui. */
    b->driver_gen = dob_ipc_port_generation(msg->arg0);

    /* Build device info payload and send it back.
     * This IS the attach: the driver asked "what's my device?" and we answer. */
    static hotplug_device_t info;
    memset(&info, 0, sizeof(info));
    info.vendor_id  = b->hw.vendor_id;
    info.device_id  = b->hw.device_id;
    info.class_code = b->hw.class_code;
    info.subclass   = b->hw.subclass;
    info.prog_if    = b->hw.prog_if;
    info.bus        = b->hw.bus;
    info.slot       = b->hw.slot;
    info.func       = b->hw.func;
    memcpy(info.bar, b->hw.bar, sizeof(info.bar));
    info.irq_line   = b->hw.irq_line;
    info.bubble_id  = b->id;

    reply->code = 0;
    reply->payload = &info;
    reply->payload_size = sizeof(info);

    b->state = BUBBLE_LIVE;
    printf("[hotplug] Bubble #%u LIVE: %s (port %u) for %04x:%04x\n",
           b->id, b->service_name, b->service_port,
           b->hw.vendor_id, b->hw.device_id);
}

static void handle_driver_released(dob_msg_t *msg)
{
    bubble_t *b = bubble_find_by_pid(msg->sender_pid);
    if (!b) return;

    /* Order matters: mark the bubble DEAD BEFORE killing the driver.
     * hotplug is single-threaded and runs bubble_reap_dead() after every
     * message, so the only states the reaper can observe are (a) bubble
     * still LIVE with the driver still alive [no-op], or (b) bubble DEAD
     * [clean memset reap]. If we killed first and set DEAD second, a
     * driver that also exits on its own (e.g. the AHCI duplicate calling
     * dob_driver_released() then _exit) could be seen dead while the
     * bubble was still LIVE, which the reaper misreports as
     * "driver crashed". Setting DEAD first removes that window: a clean
     * RELEASE is never logged as a crash. */
    b->state = BUBBLE_DEAD;
    kill(b->driver_pid);
}

static void handle_driver_failed(dob_msg_t *msg)
{
    bubble_t *b = bubble_find_by_pid(msg->sender_pid);
    if (!b) return;

    printf("[hotplug] Bubble #%u: driver %s reported failure\n",
           b->id, b->driver_name);
    kill(b->driver_pid);

    /* Driver explicitly said "I can't drive this device." Try the
     * next candidate in the fallback chain — exactly the case where
     * a generic VBE / SB16 / NE2000 catch-all should pick up after
     * the more specific driver gave up. */
    if (bubble_try_next_candidate(b)) return;

    b->state = BUBBLE_DEAD;
}

static void handle_list_query(dob_msg_t *reply)
{
    static hotplug_bubble_info_t infos[MAX_BUBBLES];
    uint32_t n = 0;

    for (int i = 0; i < MAX_BUBBLES; i++)
    {
        bubble_t *b = &bubbles[i];
        if (b->state == BUBBLE_EMPTY) continue;

        hotplug_bubble_info_t *out = &infos[n++];
        out->bubble_id    = b->id;
        out->vendor_id    = b->hw.vendor_id;
        out->device_id    = b->hw.device_id;
        out->class_code   = b->hw.class_code;
        out->subclass     = b->hw.subclass;
        out->state        = b->state;
        out->driver_pid   = b->driver_pid;
        out->service_port = b->service_port;
        strncpy(out->driver_name, b->driver_name, 31);
        strncpy(out->service_name, b->service_name, 31);
        out->driver_name [31] = '\0';
        out->service_name[31] = '\0';
        out->das_name[0] = '\0';
        if (b->das_idx >= 0)
        {
            const das_entry_t *de = das_get(b->das_idx);
            if (de)
            {
                strncpy(out->das_name, de->name, sizeof(out->das_name) - 1);
                out->das_name[sizeof(out->das_name) - 1] = '\0';
            }
        }
    }

    reply->arg0 = n;
    reply->payload = infos;
    reply->payload_size = n * sizeof(hotplug_bubble_info_t);
}

static void handle_find_device(dob_msg_t *msg, dob_msg_t *reply)
{
    uint16_t vid = (uint16_t)msg->arg0;
    uint16_t did = (uint16_t)msg->arg1;

    for (int i = 0; i < MAX_BUBBLES; i++)
    {
        bubble_t *b = &bubbles[i];
        if (b->state == BUBBLE_LIVE &&
            b->hw.vendor_id == vid && b->hw.device_id == did)
        {
            static hotplug_bubble_info_t info;
            info.bubble_id    = b->id;
            info.vendor_id    = b->hw.vendor_id;
            info.device_id    = b->hw.device_id;
            info.class_code   = b->hw.class_code;
            info.subclass     = b->hw.subclass;
            info.state        = b->state;
            info.driver_pid   = b->driver_pid;
            info.service_port = b->service_port;
            strncpy(info.driver_name, b->driver_name, 31);
            strncpy(info.service_name, b->service_name, 31);

            reply->payload = &info;
            reply->payload_size = sizeof(info);
            reply->code = 0;
            return;
        }
    }
    reply->code = -1;
}

/* Main — the algorithm */

/* Floppy detection lives in boot/floppyprobe (a one-shot process tagged
 * needs:hotplug that wakes up after our registration and sends
 * HOTPLUG_CREATE_LEGACY_BUBBLE for each drive it finds).
 *
 * ATAPI detection lives in the ata driver itself: ata spawns a
 * background thread which does dob_registry_wait("hotplug", ...) + a
 * CREATE_LEGACY_BUBBLE call per slot found. Hotplug needs zero
 * bus-specific code — both paths plug into the same existing
 * HOTPLUG_CREATE_LEGACY_BUBBLE handler. */

int main(int argc, char **argv)
{
    /* Runner mode: this process IS one user action (see
     * spawn_action_runner). Load the DAS set (same files, same order:
     * indices are stable within a boot), rebuild the context, execute,
     * die. No registration, no PCI scan, no event loop: a wedge here
     * leaks one process and one popup explains the failure — the
     * desktop and its icons stay live in the parent hotplug. */
    if (argc >= 9 && strcmp(argv[0], "--run-action") == 0)
    {
        debug_print("[hotplug-runner] start\n");
        das_load_all();
        if (das_count() <= 0)
        {
            debug_print("[hotplug-runner] NO DAS loaded (whitelist?)\n");
            dobpopup_Error("Azione dispositivo",
                           "Impossibile leggere i DAS (runner fuori "
                           "whitelist?): azione annullata.");
            return 1;
        }
        das_run_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        int das_idx  = atoi(argv[1]);
        int menu_idx = atoi(argv[2]);
        ctx.device_id          = (uint32_t)atoi(argv[3]);
        ctx.unit               = (uint8_t)atoi(argv[4]);
        ctx.io_base            = (uint16_t)atoi(argv[5]);
        if (strcmp(argv[6], "-") != 0)
            strncpy(ctx.provider_service, argv[6],
                    sizeof(ctx.provider_service) - 1);
        ctx.provider_token     = (uint32_t)atoi(argv[7]);
        ctx.hijack_target_port = (uint32_t)atoi(argv[8]);
        if (das_idx < 0 || das_idx >= das_count())
        {
            debug_print("[hotplug-runner] das_idx out of range\n");
            dobpopup_Error("Azione dispositivo",
                           "Indice DAS non valido: azione annullata.");
            return 1;
        }
        if (menu_idx < 0) das_run_action(das_idx, &ctx);
        else              das_run_menu(das_idx, menu_idx, &ctx);
        debug_print("[hotplug-runner] done\n");
        return 0;
    }

    printf("[hotplug] Starting hotplug manager...\n");
    memset(bubbles, 0, sizeof(bubbles));
    memset(hw_table, 0, sizeof(hw_table));

    /* 1. Load Device Automation Scripts. Done before any bubble is
     * created so each bubble can record its DAS match index up front.
     * Relies on DobFileSystem already being up — enforced by
     * Startup_modules ordering (DobFileSystem is listed before
     * hotplug), not by a runtime wait. The DAS database is now the
     * sole source of truth for "which driver answers for which
     * device" — there is no flat manifest fallback. */
    das_load_all();

    /* 1.5. Create our IPC port and register "hotplug" BEFORE the PCI scan
     * spawns any driver.
     *
     * HISTORY (Extensa 5220 dead-USB bug): registration used to live at
     * step 4, AFTER every bubble had been spawned. Every driver spawned
     * during the initial scan therefore raced our registry entry: a driver
     * whose dob_driver_attach() ran before step 4 got
     * dob_registry_find("hotplug") == 0, attach returned false, and the
     * bubble sat in ATTACHING forever. usb_uhci had already grown a
     * dob_registry_wait("hotplug", 5000) workaround for exactly this race
     * (see its main()), but usb_ehci had not — so on ICH8 laptops the EHCI
     * gatekeeper never ran, CONFIGFLAG stayed 1 (BIOS legacy), every
     * physical port remained owned by the EHCI controller, and the fully
     * working UHCI companions saw permanently empty ports: no icon, no
     * hotplug. The hotplug-spawned AHCI duplicate lost the same race and
     * leaked a zombie ATTACHING bubble (its RELEASED found no "hotplug"
     * entry to send to).
     *
     * Registering here closes the race BY CONSTRUCTION for every present
     * and future driver: the entry exists before any spawn, so a driver's
     * synchronous HOTPLUG_READY simply parks on our port until the service
     * loop below starts draining it — which is the intended contract ("the
     * driver controls the timing"). We do no synchronous IPC toward any
     * spawned driver during the scan (spawn_file_sync talks to
     * DobFileSystem only), so parked senders cannot deadlock us. */
    my_port = port_create();
    if (my_port < 0)
    {
        printf("[hotplug] FATAL: cannot create IPC port\n");
        _exit(1);
    }
    dob_registry_register("hotplug", (uint32_t)my_port);

    /* 2. Initial PCI scan */
    pci_scan_all(hw_table, &hw_count, MAX_HW_DEVICES);
    printf("[hotplug] Found %u PCI devices\n", hw_count);

    /* 3. Create bubbles for every device that has a matching driver */
    for (uint32_t i = 0; i < hw_count; i++)
        bubble_create(&hw_table[i]);

    /* 4. (Port creation + "hotplug" registration moved to step 1.5, BEFORE
     * the scan, to close the driver-attach race — see the comment there.) */

    /* 4.2. Declare user-visible settings.
     *
     * "show_system_partition" controls whether the desktop shows an
     * icon for the FAT32 partition MainDOB is currently running on.
     * Default is true (shown): users intuitively expect to see a
     * drive icon for every partition, including the system one,
     * and the alternative — having a disk silently disappear from
     * the desktop based on whether it's the one we booted from —
     * is more confusing than helpful. The setting exists for users
     * who explicitly prefer the cleaner look without the redundant
     * icon (since /SYSTEM and /DATA already appear in DobFiles).
     *
     * Declaration is idempotent and lives here (not in the ata/ahci
     * drivers) for two reasons: (a) the daemon would be unreachable
     * from a disk driver during early boot — a driver-side declare
     * would deadlock (driver → settingsd → DobFileSystem → driver);
     * (b) one declaration in one place yields one entry in
     * DobSettings, instead of an "ata" and an "ahci" with identical
     * checkboxes that have to be kept in sync. */
    declareSetting("show_system_partition", SETTING_BOOL,
                   "Mostra la partizione di sistema sul desktop",
                   "true", 0);

    /* 4.5. Floppy detection: boot/floppyprobe runs once after us
     * (tagged `needs:hotplug`) and sends HOTPLUG_CREATE_LEGACY_BUBBLE
     * for each drive it finds via CMOS.
     *
     * 4.6. ATAPI detection: ata driver exposes ATA_LIST_ATAPI (101);
     * the main loop pulls the list on its first iteration via a
     * deferred one-shot. Inline sync IPC here would deadlock against
     * the parked-and-now-unblocked floppyprobe (its
     * HOTPLUG_CREATE_LEGACY_BUBBLE is sync; our registry registration
     * above wakes it). Safer to let the loop drain first. See
     * atapi_query_once below. */

    printf("[hotplug] Ready. Entering service loop.\n");

    /* 5. Main loop: handle IPC + reap dead bubbles.
     * Rescan is triggered by HOTPLUG_RESCAN, or after reaping a
     * crashed driver. */
    for (;;)
    {
        dob_msg_t msg = {0}, reply = {0};
        int32_t ret = dob_ipc_receive((uint32_t)my_port, &msg);
        if (ret != 0)
            continue;

        switch (msg.code)
        {
            case HOTPLUG_READY:
                handle_driver_ready(&msg, &reply);
                break;
            case HOTPLUG_RELEASED:
                handle_driver_released(&msg);
                break;
            case HOTPLUG_FAILED:
                handle_driver_failed(&msg);
                break;
            case HOTPLUG_LIST:
                handle_list_query(&reply);
                break;
            case HOTPLUG_FIND_DEVICE:
                handle_find_device(&msg, &reply);
                break;
            case HOTPLUG_RESCAN:
                pci_rescan_and_diff();
                reply.code = 0;
                break;
            case HOTPLUG_SUBSCRIBE:
                handle_subscribe(&msg);
                reply.code = 0;
                break;
            case HOTPLUG_CREATE_LEGACY_BUBBLE:
                handle_create_legacy_bubble(&msg);
                reply.code = 0;
                break;
            case HOTPLUG_SUBDEVICE_APPEARED:
                handle_subdev_appeared(&msg);
                reply.code = 0;
                break;
            case HOTPLUG_SUBDEVICE_GONE:
                handle_subdev_gone(&msg);
                reply.code = 0;
                break;
            case PROVIDER_EJECT_REQ:
                handle_provider_eject(&msg);
                reply.code = 0;
                break;
            case ICON_ACTIVATED:
                handle_icon_activated(&msg);
                reply.code = 0;
                break;
            case MENU_ACTIVATED:
                handle_menu_activated(&msg);
                reply.code = 0;
                break;
            default:
                reply.code = -1;
                break;
        }

        /* Only reply for sync requests (IPC_MSG_REQUEST = 1).  Signal
         * and notify messages (HOTPLUG_RELEASED, ICON_ACTIVATED, ...)
         * come in via dob_ipc_post with no sender blocked; replying
         * wastes a sys_reply syscall that returns IPC_ERR_INVALID. */
        if (msg.type == 1)
            dob_ipc_reply(msg.sender_tid, &reply);

        /* After each message, reap dead/crashed bubbles.
         * If a driver crashed, its hardware is available again. */
        uint32_t reaped = bubble_reap_dead();
        if (reaped > 0)
            pci_rescan_and_diff();
    }

    return 0;
}
