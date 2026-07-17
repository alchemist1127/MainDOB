/* MainDOB ATA/IDE Driver — BMIDE DMA with LBA48 and PIO fallback.
 *
 * Protocol — see ata_protocol.h for the full wire description.
 * Quick reference:
 *   code=1   READ              arg0=lba arg1=count arg2=disk
 *   code=2   WRITE             arg0=lba arg1=count arg2=disk
 *   code=3   IDENTIFY          arg2=disk -> 512-byte IDENTIFY blob
 *   code=10  LIST_DISKS        -> ATA_MAX_DISKS * ata_disk_info_t
 *   code=20  RESCAN_PARTITIONS arg0=disk  (stub until libdob/dob/partition lands)
 *   code=100 ATA_PACKET        ATAPI passthrough (cdrom driver)
 *
 * Multi-disk model. The driver tracks up to 4 IDE slots (2 channels *
 * 2 drives). Slot 0 = primary master is the fast path: BMIDE DMA,
 * LBA48, full retry+recover. Slots 1..3 use polling-based PIO LBA28
 * — adequate for low-traffic uses (the disk utility, install probing)
 * but capped at 128 GB per disk. Pre-step-1 callers (DobFileSystem,
 * bootfs) that omit arg2 implicitly select slot 0 and see no change.
 *
 * Design goals: performance and reliability.
 *   - Bus Master IDE DMA: 1 IRQ per whole command (not per sector).
 *   - LBA48 when supported.
 *   - UDMA mode auto-negotiated via SET FEATURES, re-applied after soft reset.
 *   - PIO fallback if anything in the DMA path fails.
 *   - PRDT split on 64KB boundaries (BMIDE hardware requirement).
 *   - 3-attempt retry with soft reset on error.
 *   - Early-load friendly: does PCI scan itself when hotplug is absent.
 *
 * Notes on BMIDE quirks (QEMU PIIX3 and others):
 *   - The ACT bit (BM_STATUS bit 0) is sticky on several implementations —
 *     it does not self-clear on EOT, only on clearing Start. We therefore
 *     do NOT use ACT to determine success; we rely on ATA status + ERR bit.
 *   - The IRQ completion can arrive before the driver reaches its wait,
 *     so we drain stale IRQs BEFORE issuing the command, not inside the wait.
 *   - ATA_FEATURES is zeroed right before each DMA command because a prior
 *     SET FEATURES leaves 0x03 in the register and some emulations sample it.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <dob/server.h>
#include <dob/ipc.h>
#include <dob/types.h>
#include <dob/hotplug_driver.h>
#include <dob/hotplug_events.h>
#include <dob/registry.h>
#include <dob/thread.h>
#include <dob/ata_protocol.h>
#include <dob/partition.h>

/* === ATA registers (primary controller, legacy ports) === */
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_FEATURES    0x1F1
#define ATA_SECT_COUNT  0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_CMD_STATUS  0x1F7
#define ATA_ALT_STATUS  0x3F6

/* === ATA status / error bits === */
#define ATA_SR_BSY      0x80
#define ATA_SR_DF       0x20
#define ATA_SR_DRQ      0x08
#define ATA_SR_ERR      0x01

/* === ATA commands === */
#define ATA_CMD_READ_PIO        0x20
#define ATA_CMD_WRITE_PIO       0x30
#define ATA_CMD_READ_DMA        0xC8    /* LBA28 */
#define ATA_CMD_WRITE_DMA       0xCA    /* LBA28 */
#define ATA_CMD_READ_DMA_EXT    0x25    /* LBA48 */
#define ATA_CMD_WRITE_DMA_EXT   0x35    /* LBA48 */
#define ATA_CMD_FLUSH_CACHE     0xE7
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA
#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_SET_FEATURES    0xEF
#define ATA_CMD_SMART           0xB0
#define ATA_CMD_DATA_SET_MGMT   0x06    /* DATA SET MANAGEMENT (carries TRIM) */
#define ATA_DSM_FEATURE_TRIM    0x01    /* Features bit selecting the TRIM operation */

/* SMART sub-command (loaded into Features register before issuing 0xB0).
 * READ_DATA returns one sector of vendor SMART attributes. */
#define ATA_SMART_READ_DATA     0xD0
/* SMART command-block "magic" loaded into LBA mid/hi to validate the
 * SMART sub-command (per ATA spec). */
#define ATA_SMART_LBA_MID       0x4F
#define ATA_SMART_LBA_HI        0xC2

/* === BMIDE registers (offsets from BAR4, primary channel) === */
#define BM_CMD          0x00    /* bit0 Start, bit3 direction (1 = read-from-disk) */
#define BM_STATUS       0x02    /* bit1 Error, bit2 IRQ (W1C) — ACT bit ignored */
#define BM_PRDT         0x04    /* physical PRDT base (32-bit, 4-byte aligned) */

#define BM_CMD_START    0x01
#define BM_CMD_READ     0x08    /* direction: device -> memory */
#define BM_STATUS_ERR   0x02
#define BM_STATUS_IRQ   0x04

/* === PCI config === */
#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define SECTOR_SIZE     512
#define MAX_SECTORS     128                         /* per request; fits 64KB DMA */
#define DMA_BUF_SIZE    (MAX_SECTORS * SECTOR_SIZE) /* 64 KB */
#define MAX_PRDT        2                           /* 64KB buffer crosses ≤1 boundary */

#define ATA_TIMEOUT_MS      2000
#define DMA_TIMEOUT_MS      5000
#define FLUSH_TIMEOUT_MS    5000
#define MAX_RETRIES         3

/* PRDT entry (little-endian, packed, 8 bytes) */
typedef struct
{
    uint32_t phys;
    uint16_t bytes;   /* 0 = 64KB, must not cross a 64KB boundary */
    uint16_t flags;   /* bit15 = EOT */
} __attribute__((packed)) prd_t;

/* === Globals === */
static uint32_t irq_port = 0;
static uint8_t  irq_num  = 14;

static uint16_t bm_base   = 0;     /* BAR4 masked, primary channel base */
static bool     dma_ok    = false; /* full DMA path ready */

/* Consecutive runtime DMA failures. On real hardware whose bus-master IDE
 * does not actually work (e.g. some Compaq Armada-era chipsets) the drive
 * still ADVERTISES UDMA, so dma_ok comes up true, but every dma_xfer then
 * fails. After a few such failures we give up on DMA entirely for this disk
 * and run PIO-only — which the same chips handle fine (it is how IDENTIFY
 * and SMART already succeed). On QEMU dma_xfer does not fail, so this never
 * trips and the fast DMA path is kept. */
static int      dma_fail_streak = 0;
#define DMA_FAIL_LIMIT  3
static bool     lba48_ok  = false;
static uint8_t  udma_mode = 0xFF;  /* negotiated UDMA mode, 0xFF = none */

static uint8_t *dma_buf        = NULL;
static uint32_t dma_buf_phys   = 0;
static prd_t   *prdt           = NULL;
static uint32_t prdt_phys      = 0;

/* PCI location (for pci_enable_bus_master in standalone path) */
static uint8_t pci_bus = 0, pci_slot = 0, pci_func = 0;
static bool    pci_found = false;

/* === Low-level waits === */

/* 400ns delay via 4 alt-status reads */
static void
ata_delay(void)
{
    io_inb(ATA_ALT_STATUS);
    io_inb(ATA_ALT_STATUS);
    io_inb(ATA_ALT_STATUS);
    io_inb(ATA_ALT_STATUS);
}

/* Drain any pending IRQ notifications left on our port.
 * Acks the device (reads ATA status) and W1Cs BMIDE IRQ/ERR bits.
 * Called only when we are NOT expecting an IRQ (e.g. before issuing DMA). */
static void
drain_irqs(void)
{
    dob_msg_t m;
    memset(&m, 0, sizeof(m));
    while (dob_ipc_receive_nowait(irq_port, &m) == DOB_OK)
    {
        if (m.type == 3)
        {
            io_inb(ATA_CMD_STATUS);
            if (bm_base)
                io_outb(bm_base + BM_STATUS, BM_STATUS_IRQ | BM_STATUS_ERR);
            irq_done(irq_num);
        }
        memset(&m, 0, sizeof(m));
    }
}

/* PIO-style wait: fast-path on BSY=0, then block on IRQ with timeout.
 * Used by PIO path and by IDENTIFY/SET FEATURES. */
static uint8_t
ata_wait_irq(uint32_t timeout_ms)
{
    drain_irqs();

    uint8_t st = io_inb(ATA_ALT_STATUS);
    if (!(st & ATA_SR_BSY))
        return io_inb(ATA_CMD_STATUS);

    int tid = timer_set(irq_port, timeout_ms, 0);
    for (;;)
    {
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        if (dob_ipc_receive(irq_port, &msg) != DOB_OK)
        {
            timer_cancel_async(tid);
            return 0;
        }
        if (msg.type == 3)
        {
            st = io_inb(ATA_CMD_STATUS);
            irq_done(irq_num);
            timer_cancel_async(tid);
            return st;
        }
        if (msg.code == 70)
        {
            debug_print("[ata] IRQ timeout\n");
            return 0;
        }
    }
}

/* Forward decls for the polling helpers (defined later, with the multi-slot
 * scan). Used by dma_wait_irq's polled-completion fallback and by the
 * primary-master IDENTIFY, both of which appear before the definitions. */
static bool ide_poll_core     (uint16_t alt, uint32_t budget_ms,
                               bool want_drq);
static bool ide_poll_bsy_clear(uint16_t alt, uint32_t budget_ms);
static bool ide_poll_drq(uint16_t alt, uint32_t budget_ms);

/* Strict wait for DMA completion: NO drain, NO fast-path.
 * Caller must drain before issuing the command, so the only IRQ in the
 * queue is the one we are waiting for. */
static uint8_t
dma_wait_irq(uint32_t timeout_ms)
{
    int tid = timer_set(irq_port, timeout_ms, 0);
    for (;;)
    {
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        if (dob_ipc_receive(irq_port, &msg) != DOB_OK)
        {
            timer_cancel_async(tid);
            return 0;
        }
        if (msg.type == 3)
        {
            /* The IRQ is only the completion NOTIFICATION. On a shared/compat
             * IRQ line (PIIX4 IDE) a spurious notification — or a stale one
             * from a prior op that landed just after drain_irqs — can arrive
             * with BSY still set. Sampling ATA_CMD_STATUS mid-transfer then
             * returns BSY/garbage ERR/DRQ bits, which dma_xfer scores as an
             * ATA error: the intermittent write failure. Confirm BSY=0 first
             * (near-free when the IRQ is genuine — BSY is already clear), then
             * read the final status, exactly like the polling fallback below.
             * Never return 0 (= "failed"). */
            ide_poll_bsy_clear(ATA_ALT_STATUS, 12000u);
            uint8_t st = io_inb(ATA_CMD_STATUS);
            irq_done(irq_num);
            timer_cancel_async(tid);
            return st ? st : ATA_SR_DRQ;
        }
        if (msg.code == 70)
        {
            /* IRQ never arrived within the budget. On the real E500 the
             * PIIX4 IDE interrupt (routed to IRQ 11 in compatibility mode)
             * may not reach us here even though the Bus-Master DMA transfer
             * itself completes in hardware — the interrupt is only the
             * NOTIFICATION, not the transfer. So instead of failing the
             * read (which made the disk detected-but-unreadable), poll the
             * ATA status to completion: wait for BSY=0 with a bounded
             * budget, then return the command-status register exactly as the
             * IRQ path would. Harmless where the IRQ does arrive (that path
             * returns first); essential where it doesn't. */
            debug_print("[ata] DMA IRQ timeout -> polling completion\n");
            if (ide_poll_bsy_clear(ATA_ALT_STATUS, 12000u))
            {
                uint8_t st = io_inb(ATA_CMD_STATUS);
                /* Acknowledge the (possibly latched) controller IRQ line so a
                 * later real interrupt isn't left asserted. */
                irq_done(irq_num);
                return st ? st : ATA_SR_DRQ;  /* never report 0 = "failed" */
            }
            return 0;
        }
    }
}

/* Poll BSY=0 via timer sleep — used after SRST, before IRQs are trusted */
static bool
ata_wait_not_busy(uint32_t timeout_ms)
{
    uint32_t step = 5;
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms)
    {
        if (!(io_inb(ATA_ALT_STATUS) & ATA_SR_BSY))
            return true;
        int tid = timer_set(irq_port, step, 0);
        dob_msg_t m; memset(&m, 0, sizeof(m));
        dob_ipc_receive(irq_port, &m);
        if (m.type == 3) { io_inb(ATA_CMD_STATUS); irq_done(irq_num); }
        (void)tid;
        elapsed += step;
    }
    return !(io_inb(ATA_ALT_STATUS) & ATA_SR_BSY);
}

/* === Soft reset === */
static void
ata_soft_reset(void)
{
    io_outb(ATA_ALT_STATUS, 0x04);
    ata_delay();
    io_outb(ATA_ALT_STATUS, 0x00);
    ata_delay();
    ata_wait_not_busy(ATA_TIMEOUT_MS);
}

/* === SET FEATURES: negotiate UDMA mode. Defined early for retry use. === */
static bool
ata_set_udma(uint8_t mode)
{
    if (!ata_wait_not_busy(ATA_TIMEOUT_MS)) return false;
    io_outb(ATA_DRIVE_HEAD, 0xA0);
    ata_delay();
    io_outb(ATA_FEATURES, 0x03);
    io_outb(ATA_SECT_COUNT, 0x40 | mode);
    io_outb(ATA_CMD_STATUS, ATA_CMD_SET_FEATURES);

    uint8_t st = ata_wait_irq(ATA_TIMEOUT_MS);
    return st && !(st & ATA_SR_ERR);
}

/* Reset + re-negotiate UDMA. Called in retry path so that SRST doesn't
 * silently drop the drive back to PIO-only. */
static void
ata_recover(void)
{
    ata_soft_reset();
    if (dma_ok && udma_mode != 0xFF)
        (void)ata_set_udma(udma_mode);
}

/* === PCI scan (used in standalone mode to find BMIDE BAR4) === */
static uint32_t
pci_read32(uint8_t b, uint8_t s, uint8_t f, uint8_t off)
{
    uint32_t addr = (1u << 31) | ((uint32_t)b << 16) |
                    ((uint32_t)s << 11) | ((uint32_t)f << 8) | (off & 0xFC);
    io_outl(PCI_CONFIG_ADDR, addr);
    return io_inl(PCI_CONFIG_DATA);
}

static void
pci_write32(uint8_t b, uint8_t s, uint8_t f, uint8_t off, uint32_t val)
{
    uint32_t addr = (1u << 31) | ((uint32_t)b << 16) |
                    ((uint32_t)s << 11) | ((uint32_t)f << 8) | (off & 0xFC);
    io_outl(PCI_CONFIG_ADDR, addr);
    io_outl(PCI_CONFIG_DATA, val);
}

/* Scan bus 0 for class 01 (mass storage) subclass 01 (IDE). */
static bool
pci_find_ide(void)
{
    for (uint8_t s = 0; s < 32; s++)
    {
        for (uint8_t f = 0; f < 8; f++)
        {
            uint32_t id = pci_read32(0, s, f, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) continue;

            uint32_t cls = pci_read32(0, s, f, 0x08);
            uint8_t  class_code = (cls >> 24) & 0xFF;
            uint8_t  subclass   = (cls >> 16) & 0xFF;
            if (class_code != 0x01 || subclass != 0x01) continue;

            pci_bus = 0; pci_slot = s; pci_func = f;
            uint32_t bar4 = pci_read32(0, s, f, 0x20);
            if (!(bar4 & 1)) return false;     /* must be I/O space */
            bm_base = (uint16_t)(bar4 & 0xFFFC);
            pci_found = bm_base != 0;
            return pci_found;
        }
    }
    return false;
}

static void
pci_enable_master_standalone(void)
{
    uint32_t cmd = pci_read32(pci_bus, pci_slot, pci_func, 0x04);
    cmd |= 0x06;    /* I/O + Bus Master */
    pci_write32(pci_bus, pci_slot, pci_func, 0x04, cmd);
}

/* === Diagnostics (readable on real hardware via ATA_GET_DIAG) ===
 * On machines without a serial console the debug_print() trail is invisible,
 * so we latch the salient facts here for a userspace probe (atadiag) to
 * read back. last_dma_fail: 0 none, 1 wait timeout, 2 ATA ERR/DF, 3 BMIDE
 * ERR. ctrl_quirk_applied: 1 if we wrote a chipset UDMA-enable register. */
static uint8_t last_dma_fail       = 0;
static uint8_t ctrl_quirk_applied  = 0;   /* B-path was run */
static uint8_t ctrl_quirk_tried    = 0;   /* B-path was attempted (even if no-op) */

/* === IDE DMA bring-up strategy: trust-BIOS first, chipset quirk as fallback ===
 *
 * MaindOB is general-purpose and targets *all* PCI IDE controllers, so it must
 * NOT depend on a per-chip timing table to get DMA working. Maintaining timing
 * tables for every PATA bridge ever made is the trap libata was built to
 * escape. We use a layered approach with safe degradation:
 *
 *   Level A (default, zero per-chip code): trust the firmware.
 *     On real machines the BIOS has already programmed the IDE controller's
 *     timing AND enabled UDMA for the drives it found at POST. We write
 *     NOTHING to the controller, issue SET FEATURES to the drive, and try DMA.
 *     This works on any controller the BIOS handled — i.e. essentially all of
 *     them on real hardware — and is what keeps "compatible with all IDE"
 *     honest. (QEMU also transfers fine with no controller writes.)
 *
 *   Level B (fallback, tiny per-chip table): if DMA actually FAILS at runtime
 *     and we have not yet tried the quirk, set just the controller's
 *     UDMA-enable bit for chipsets we recognise, then retry once. This rescues
 *     boards whose BIOS left UDMA disabled. We touch ONLY the enable bit and
 *     leave the cycle-timing registers exactly as the BIOS set them (per the
 *     design decision: don't compute timing, trust the BIOS for cycles).
 *     The enable bit lives in a different register on every chipset, hence the
 *     small switch — but it is an optimisation, never the critical path.
 *
 *   Level C (universal net): PIO. Already present; covers unknown or broken
 *     controllers and any drive where B did not help.
 *
 * controller_enable_udma() implements Level B. It returns true only if it
 * recognised the chipset AND wrote an enable bit; false means "unknown
 * controller, nothing changed" — the caller then just relies on A or drops to
 * PIO. It deliberately does NOT touch cycle timing.
 *
 * SEPARATE but related: controller_udma_cap() below caps the UDMA mode we
 * negotiate with the DRIVE to what the CONTROLLER can physically clock. This
 * is independent of A/B/C and applies in ALL cases, because a modern drive
 * (or a CF/SSD-to-IDE adapter) will advertise UDMA-5/6 even behind an old
 * ATA-33 bridge. Negotiating UDMA-6 on a PIIX4 makes the drive transmit at
 * 133 MB/s while the bridge samples at its ATA-33 timing -> garbage ->
 * ATA ERR/DF, exactly the failure the on-screen diagnostic shows. Capping the
 * mode is the actual fix for "DMA active but intermittent errors".
 */

/* Maximum UDMA mode the controller can physically sustain, by chipset.
 *   PIIX3            -> UDMA 2  (ATA-33)
 *   PIIX4            -> UDMA 2  (ATA-33; 82371AB/MB top out here)
 *   ICH / ICH0       -> UDMA 2  (ATA-33)
 *   ICH2             -> UDMA 4  (ATA-66 via the 80-wire cable; safe at 4)
 *   VIA / SiS        -> UDMA 2  (conservative: many early parts are ATA-33,
 *                                and we are not reading their cable-detect)
 *   unknown          -> UDMA 2  (safe floor: ATA-33 works on every UDMA bus)
 * Returns 0xFF only if there is no PCI controller at all (then the caller's
 * own logic decides; UDMA won't be used without a bus master anyway). */
static uint8_t
controller_udma_cap(void)
{
    if (!pci_found) return 2;   /* no controller info: assume ATA-33 ceiling */

    uint32_t id = pci_read32(pci_bus, pci_slot, pci_func, 0x00);
    uint16_t vendor = (uint16_t)(id & 0xFFFF);
    uint16_t device = (uint16_t)(id >> 16);

    switch (vendor)
    {
    case 0x8086:   /* Intel */
        switch (device)
        {
        case 0x7010:                       /* PIIX3   */
        case 0x7111: case 0x7199:          /* PIIX4   */
        case 0x2411: case 0x2421:          /* ICH/ICH0 */
            return 2;                      /* ATA-33  */
        case 0x244A: case 0x244B:          /* ICH2(-M) */
            return 4;                      /* ATA-66  */
        default:
            return 2;                      /* unknown Intel IDE: ATA-33 floor */
        }
    case 0x1106:   /* VIA  */ return 2;    /* conservative ATA-33 */
    case 0x1039:   /* SiS  */ return 2;    /* conservative ATA-33 */
    default:                  return 2;    /* unknown vendor: ATA-33 floor */
    }
}

static bool
controller_enable_udma(void)
{
    ctrl_quirk_tried = 1;
    if (!pci_found) return false;

    uint32_t id = pci_read32(pci_bus, pci_slot, pci_func, 0x00);
    uint16_t vendor = (uint16_t)(id & 0xFFFF);
    uint16_t device = (uint16_t)(id >> 16);

    switch (vendor)
    {
    case 0x8086:   /* Intel */
        /* PIIX3/PIIX4 family (82371SB/AB/MB) and the early ICH IDE functions
         * share the UDMACTL layout: config-space 0x48, bit0 = primary master
         * UDMA enable. We set only that bit; cycle timing (0x4A) is left as the
         * BIOS programmed it. Recognised devices:
         *   0x7010 PIIX3, 0x7111 PIIX4, 0x7199 PIIX4-mobile,
         *   0x2411/0x2421 ICH/ICH0, 0x244A/0x244B ICH2(-M). */
        switch (device)
        {
        case 0x7010: case 0x7111: case 0x7199:
        case 0x2411: case 0x2421: case 0x244A: case 0x244B:
        {
            uint32_t reg = pci_read32(pci_bus, pci_slot, pci_func, 0x48);
            if (reg & 0x01) return false;   /* BIOS already enabled it */
            reg |= 0x01;                    /* primary-master UDMA enable */
            pci_write32(pci_bus, pci_slot, pci_func, 0x48, reg);
            ctrl_quirk_applied = 1;
            return true;
        }
        default:
            return false;   /* unknown Intel IDE — trust BIOS / PIO */
        }

    case 0x1106:   /* VIA */
        /* VIA south bridges (e.g. 82C586/596/686) keep per-drive UDMA enable
         * in config-space 0x53 (primary master), bit5 = enable, with the mode
         * in the low bits. We only flip the enable bit and leave the mode/timing
         * the BIOS chose. This covers the common VT82Cxxx IDE function. */
    {
        uint32_t reg = pci_read32(pci_bus, pci_slot, pci_func, 0x50);
        uint8_t  b53 = (uint8_t)(reg >> 24);   /* byte at offset 0x53 */
        if (b53 & 0x20) return false;          /* already enabled */
        b53 |= 0x20;
        reg = (reg & 0x00FFFFFFu) | ((uint32_t)b53 << 24);
        pci_write32(pci_bus, pci_slot, pci_func, 0x50, reg);
        ctrl_quirk_applied = 1;
        return true;
    }

    case 0x1039:   /* SiS */
        /* SiS 5513 and relatives gate UDMA in config-space 0x40 (primary
         * master), bit7 = UDMA enable on most parts. Flip enable only. */
    {
        uint32_t reg = pci_read32(pci_bus, pci_slot, pci_func, 0x40);
        if (reg & 0x80) return false;
        reg |= 0x80;
        pci_write32(pci_bus, pci_slot, pci_func, 0x40, reg);
        ctrl_quirk_applied = 1;
        return true;
    }

    default:
        /* Unknown vendor: do not guess at register layout. Trust the BIOS
         * (Level A) or fall through to PIO (Level C). */
        return false;
    }
}

/* === PRDT build: split the buffer on 64KB physical boundaries === */
static uint32_t
prdt_build(uint32_t buf_phys, uint32_t len)
{
    uint32_t remaining = len;
    uint32_t phys = buf_phys;
    uint32_t n = 0;

    while (remaining > 0 && n < MAX_PRDT)
    {
        uint32_t to_boundary = 0x10000 - (phys & 0xFFFF);
        uint32_t chunk = remaining < to_boundary ? remaining : to_boundary;
        if (chunk > 0x10000) chunk = 0x10000;

        prdt[n].phys  = phys;
        prdt[n].bytes = (chunk == 0x10000) ? 0 : (uint16_t)chunk;
        prdt[n].flags = 0;
        n++;

        phys += chunk;
        remaining -= chunk;
    }
    if (remaining) return 0;
    prdt[n - 1].flags = 0x8000;
    return n;
}

/* === ATA register programming === */
static void
ata_program_lba28(uint32_t lba, uint8_t count)
{
    io_outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    ata_delay();
    io_outb(ATA_FEATURES, 0);
    io_outb(ATA_SECT_COUNT, count);
    io_outb(ATA_LBA_LO,  (uint8_t)(lba));
    io_outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    io_outb(ATA_LBA_HI,  (uint8_t)(lba >> 16));
}

static void
ata_program_lba48(uint64_t lba, uint16_t count)
{
    io_outb(ATA_DRIVE_HEAD, 0x40);   /* LBA mode, master */
    ata_delay();
    /* High bytes first */
    io_outb(ATA_FEATURES, 0);
    io_outb(ATA_SECT_COUNT, (uint8_t)(count >> 8));
    io_outb(ATA_LBA_LO,  (uint8_t)(lba >> 24));
    io_outb(ATA_LBA_MID, (uint8_t)(lba >> 32));
    io_outb(ATA_LBA_HI,  (uint8_t)(lba >> 40));
    /* Low bytes */
    io_outb(ATA_FEATURES, 0);
    io_outb(ATA_SECT_COUNT, (uint8_t)count);
    io_outb(ATA_LBA_LO,  (uint8_t)(lba));
    io_outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    io_outb(ATA_LBA_HI,  (uint8_t)(lba >> 16));
}

/* === DMA core ===
 *
 * Critical ordering:
 *   - drain_irqs() is done BEFORE issuing the command (not inside the wait),
 *     so we never eat our own completion IRQ in a fast emulator.
 *   - We read BM_STATUS BEFORE clearing Start, so sticky bits (ERR, IRQ) are
 *     preserved — ACT is intentionally ignored.
 *   - Success criterion: no ATA ERR/DF and no BM ERR. IRQ bit is informative
 *     but we don't require it (some emulations don't set it consistently).
 */
static bool
dma_xfer(uint64_t lba, uint16_t count, bool write)
{
    /* Guarantee nIEN=0 (device IRQs ENABLED) before anything else.
     *
     * This is the DMA twin of the PIO flag bug. The PIO path sets nIEN=1 to
     * poll interrupt-free and must release it on every exit; if any PIO exit
     * (or a soft reset, which also lands in the control register) leaves
     * nIEN=1, the device stays silent. DMA completion is IRQ-driven, so a
     * stale nIEN=1 makes dma_wait_irq() time out on EVERY transfer that
     * follows — the operation "starts" but the IRQ never comes, and other
     * programs can no longer read the disk. dma_xfer must not assume the
     * control register is clean; it must assert nIEN=0 itself. */
    io_outb(ATA_ALT_STATUS, 0x00);   /* SRST=0, nIEN=0: IRQs on for DMA */

    if (!ata_wait_not_busy(ATA_TIMEOUT_MS)) return false;

    uint32_t bytes = (uint32_t)count * SECTOR_SIZE;
    if (!prdt_build(dma_buf_phys, bytes)) return false;

    /* Reset BM state: Start=0, clear ERR/IRQ W1C bits */
    io_outb(bm_base + BM_CMD, 0);
    io_outb(bm_base + BM_STATUS, BM_STATUS_IRQ | BM_STATUS_ERR);

    /* Program PRDT, direction (Start still 0) */
    io_outl(bm_base + BM_PRDT, prdt_phys);
    io_outb(bm_base + BM_CMD, write ? 0 : BM_CMD_READ);

    /* Drain any leftover IRQs BEFORE issuing — critical on fast emulators */
    drain_irqs();

    /* Program ATA, issue DMA command */
    if (lba48_ok)
    {
        ata_program_lba48(lba, count);
        io_outb(ATA_CMD_STATUS,
                write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT);
    }
    else
    {
        ata_program_lba28((uint32_t)lba, (uint8_t)count);
        io_outb(ATA_CMD_STATUS,
                write ? ATA_CMD_WRITE_DMA : ATA_CMD_READ_DMA);
    }

    /* Kick the bus master */
    io_outb(bm_base + BM_CMD,
            (write ? 0 : BM_CMD_READ) | BM_CMD_START);

    /* Strict wait — one IRQ per whole command */
    uint8_t st = dma_wait_irq(DMA_TIMEOUT_MS);

    /* Sample BM status BEFORE clearing Start (preserves sticky ERR/IRQ) */
    uint8_t bm_st = io_inb(bm_base + BM_STATUS);

    /* Stop and clear W1C */
    io_outb(bm_base + BM_CMD, 0);
    io_outb(bm_base + BM_STATUS, BM_STATUS_IRQ | BM_STATUS_ERR);

    if (!st)
    {
        last_dma_fail = 1;
        debug_print("[ata] DMA wait timeout\n");
        return false;
    }
    if (st & (ATA_SR_ERR | ATA_SR_DF))
    {
        io_inb(ATA_ERROR);
        last_dma_fail = 2;
        debug_print("[ata] DMA ATA error\n");
        return false;
    }
    if (bm_st & BM_STATUS_ERR)
    {
        last_dma_fail = 3;
        debug_print("[ata] BMIDE error\n");
        return false;
    }
    return true;
}

/* DATA SET MANAGEMENT (TRIM) on the primary master via the bus-master
 * DMA engine. Sends one 512-byte range block holding a single entry:
 *   bits [47:0]  = starting LBA
 *   bits [63:48] = sector count (the on-wire field; 0 means 65536)
 * This is a memory->device transfer (the "write" BM direction). Requires
 * the DMA path (dma_ok) — there is no PIO DSM here, which is fine because
 * TRIM-capable SSDs run with DMA. Primary-master only: this driver's DMA
 * engine addresses slot 0 exactly like dma_xfer. Mirrors the AHCI op-4
 * contract. NOTE: no block-layer caller exists yet (parity with AHCI,
 * whose op-4 is likewise unwired) — exercise + hardware-validate this
 * only once block_trim() is added. */
static bool
ata_dsm_trim(uint64_t lba, uint32_t count)
{
    if (!dma_ok || !dma_buf)          return false;
    if (count == 0 || count > 65536)  return false;

    uint16_t count_field = (count == 65536) ? 0 : (uint16_t)count;

    /* One TRIM range entry into the DMA buffer; rest of the block zero. */
    memset(dma_buf, 0, SECTOR_SIZE);
    uint64_t *entry = (uint64_t *)dma_buf;
    *entry = ((uint64_t)count_field << 48) | (lba & 0x0000FFFFFFFFFFFFull);

    io_outb(ATA_ALT_STATUS, 0x00);   /* SRST=0, nIEN=0: IRQ-driven completion */
    if (!ata_wait_not_busy(ATA_TIMEOUT_MS)) return false;

    if (!prdt_build(dma_buf_phys, SECTOR_SIZE)) return false;

    /* Reset BM, clear W1C sticky bits, program PRDT, WRITE direction. */
    io_outb(bm_base + BM_CMD, 0);
    io_outb(bm_base + BM_STATUS, BM_STATUS_IRQ | BM_STATUS_ERR);
    io_outl(bm_base + BM_PRDT, prdt_phys);
    io_outb(bm_base + BM_CMD, 0);    /* direction = write (no BM_CMD_READ) */

    drain_irqs();

    /* Issue DATA SET MANAGEMENT: Features = TRIM, one 512-byte range block,
     * LBA fields unused (0). LBA28-style master select. */
    io_outb(ATA_DRIVE_HEAD, 0xE0);   /* LBA mode, master */
    ata_delay();
    io_outb(ATA_FEATURES, ATA_DSM_FEATURE_TRIM);
    io_outb(ATA_SECT_COUNT, 1);
    io_outb(ATA_LBA_LO, 0);
    io_outb(ATA_LBA_MID, 0);
    io_outb(ATA_LBA_HI, 0);
    io_outb(ATA_CMD_STATUS, ATA_CMD_DATA_SET_MGMT);

    /* Kick the bus master (write direction). */
    io_outb(bm_base + BM_CMD, BM_CMD_START);

    uint8_t st    = dma_wait_irq(DMA_TIMEOUT_MS);
    uint8_t bm_st = io_inb(bm_base + BM_STATUS);

    io_outb(bm_base + BM_CMD, 0);
    io_outb(bm_base + BM_STATUS, BM_STATUS_IRQ | BM_STATUS_ERR);

    if (!st)
    {
        last_dma_fail = 1;
        debug_print("[ata] DSM TRIM wait timeout\n");
        return false;
    }
    if (st & (ATA_SR_ERR | ATA_SR_DF))
    {
        io_inb(ATA_ERROR);
        last_dma_fail = 2;
        debug_print("[ata] DSM TRIM ATA error\n");
        return false;
    }
    if (bm_st & BM_STATUS_ERR)
    {
        last_dma_fail = 3;
        debug_print("[ata] DSM TRIM BMIDE error\n");
        return false;
    }
    return true;
}

/* === PIO fallback ===
 *
 * Single-sector, polled, interrupt-free transfers. Three deliberate
 * choices, all driven by the kind of controller this fallback exists for
 * (notably mSATA SSDs behind an mSATA->IDE bridge, e.g. on Armada-class
 * laptops):
 *
 *   1. One sector per command. These bridges frequently mishandle a
 *      multi-sector PIO command (a large sector count in a single READ)
 *      while handling single-sector commands reliably. Issuing one
 *      command per sector trades a little throughput for correctness.
 *
 *   2. Poll DRQ via ide_poll_drq() rather than waiting on the device IRQ.
 *      PIO interrupt delivery on these bridges is unreliable; the status
 *      register is the dependable, interrupt-independent signal.
 *
 *   3. Disable device interrupts (nIEN=1) for the duration of the polled
 *      transfer. Because we poll instead of listening on the IRQ port, a
 *      device interrupt raised on command completion would be left
 *      unacknowledged: the kernel masks the shared line, waits for an
 *      irq_done that never arrives, times out, and force-unmasks — once
 *      per command. nIEN keeps the device quiet so the polled path stays
 *      self-contained. It is restored on every exit so the IRQ-driven
 *      paths (DMA, cache flush) are unaffected. ata_identify() polls under
 *      nIEN for the same reason.
 */
static bool
pio_read_lba28(uint32_t lba, uint8_t count, void *buffer)
{
    const uint16_t alt = ATA_ALT_STATUS;
    uint16_t *buf = (uint16_t *)buffer;

    io_outb(alt, 0x02);   /* nIEN = 1: device IRQs off while we poll */

    for (uint8_t s = 0; s < count; s++)
    {
        if (!ata_wait_not_busy(ATA_TIMEOUT_MS))           { io_outb(alt, 0x00); return false; }
        ata_program_lba28(lba + s, 1);
        io_outb(ATA_CMD_STATUS, ATA_CMD_READ_PIO);

        if (!ide_poll_drq(alt, 12000u))                 { io_outb(alt, 0x00); return false; }
        for (int i = 0; i < SECTOR_SIZE / 2; i++)
            buf[s * (SECTOR_SIZE / 2) + i] = io_inw(ATA_DATA);
    }

    io_outb(alt, 0x00);   /* nIEN = 0: restore IRQ delivery */
    return true;
}

static bool
pio_write_lba28(uint32_t lba, uint8_t count, const void *buffer)
{
    /* Same model as pio_read_lba28: one sector per command, DRQ polled,
     * device interrupts disabled (nIEN=1) for the duration, restored on
     * every exit. See the comment on pio_read_lba28 for the rationale. */
    const uint16_t alt = ATA_ALT_STATUS;
    const uint16_t *buf = (const uint16_t *)buffer;

    io_outb(alt, 0x02);   /* nIEN = 1 */

    for (uint8_t s = 0; s < count; s++)
    {
        if (!ata_wait_not_busy(ATA_TIMEOUT_MS))           { io_outb(alt, 0x00); return false; }
        ata_program_lba28(lba + s, 1);
        io_outb(ATA_CMD_STATUS, ATA_CMD_WRITE_PIO);

        if (!ide_poll_drq(alt, 12000u))                 { io_outb(alt, 0x00); return false; }
        for (int i = 0; i < SECTOR_SIZE / 2; i++)
            io_outw(ATA_DATA, buf[s * (SECTOR_SIZE / 2) + i]);
    }

    /* Flush the write cache once, polled to completion. */
    if (!ata_wait_not_busy(ATA_TIMEOUT_MS))               { io_outb(alt, 0x00); return false; }
    io_outb(ATA_CMD_STATUS, ATA_CMD_FLUSH_CACHE);
    ata_delay();
    bool ok = ide_poll_bsy_clear(alt, 60000u);

    io_outb(alt, 0x00);   /* nIEN = 0: restore IRQ delivery */
    return ok;
}

/* === High-level read/write with retry === */
/* Read up to MAX_SECTORS (128) sectors in a single command. The DMA buffer
 * is exactly MAX_SECTORS*512, and the ATA sector-count register is 8-bit
 * (where 0 means 256), so callers MUST NOT pass count > MAX_SECTORS or 0.
 * ata_read() below enforces this by splitting larger requests. */
static bool
ata_read_chunk(uint64_t lba, uint16_t count, void *out)
{
    for (int attempt = 0; attempt < MAX_RETRIES; attempt++)
    {
        /* Fast path: DMA, while it is believed available. */
        if (dma_ok)
        {
            if (dma_xfer(lba, count, false))
            {
                memcpy(out, dma_buf, (uint32_t)count * SECTOR_SIZE);
                dma_fail_streak = 0;      /* DMA works: reset the streak */
                return true;
            }
            /* DMA failed THIS time. Before abandoning DMA, try Level B ONCE:
             * some BIOSes leave the controller's UDMA-enable bit clear, and
             * setting it (for chipsets we recognise) may be all that is
             * missing. Only attempted once per session; on unknown chips it is
             * a no-op and we proceed exactly as before. */
            if (!ctrl_quirk_tried && controller_enable_udma())
            {
                debug_print("[ata] enabled controller UDMA, retrying DMA\n");
                (void)ata_set_udma(udma_mode);   /* re-arm drive side */
                continue;                        /* retry DMA, don't degrade */
            }

            /* Do not just retry DMA (on a chip whose bus-master is broken it
             * will keep failing and we would never read anything); fall
             * through to PIO right now. */
            if (++dma_fail_streak >= DMA_FAIL_LIMIT)
            {
                /* DMA has failed repeatedly -> this controller's BMIDE does
                 * not really work. Stop using DMA for this disk; PIO from
                 * here on. (IDENTIFY/SMART already proved PIO is fine.) */
                dma_ok = false;
                debug_print("[ata] DMA unreliable, switching to PIO\n");
            }
        }

        /* PIO path: either DMA was never available, or it just failed and
         * we are falling back. LBA28 PIO covers the boot disk's range. */
        if (pio_read_lba28((uint32_t)lba, (uint8_t)count, out))
            return true;

        debug_print("[ata] read failed, retrying\n");
        ata_recover();
    }
    return false;
}

/* Public read: split an arbitrarily large request into MAX_SECTORS chunks.
 *
 * This is the difference that broke booting the installed system. The DMA
 * buffer is 64 KB (128 sectors) and the 8-bit sector-count register treats
 * 0 as 256, so a single dma_xfer can only move <=128 sectors. The installer
 * does small metadata I/O and never tripped it, but the bootloader and
 * kernel/program loading read large contiguous runs (>128 sectors) in one
 * call -> the count truncated and the copy ran off the end of dma_buf, so
 * "other programs couldn't read from disk". PIO never showed it because it
 * already issues one sector per command. Chunking here makes every caller
 * safe regardless of request size, on both the DMA and PIO paths. */
static bool
ata_read(uint64_t lba, uint16_t count, void *out)
{
    uint8_t *p = (uint8_t *)out;
    while (count > 0)
    {
        uint16_t chunk = count > MAX_SECTORS ? MAX_SECTORS : count;
        if (!ata_read_chunk(lba, chunk, p))
            return false;
        lba   += chunk;
        p     += (uint32_t)chunk * SECTOR_SIZE;
        count -= chunk;
    }
    return true;
}

/* Write up to MAX_SECTORS sectors in one command. Same size constraint as
 * ata_read_chunk; ata_write() splits larger requests. */
static bool
ata_write_chunk(uint64_t lba, uint16_t count, const void *in)
{
    for (int attempt = 0; attempt < MAX_RETRIES; attempt++)
    {
        /* Fast path: DMA write + cache flush, while DMA is believed good. */
        if (dma_ok)
        {
            memcpy(dma_buf, in, (uint32_t)count * SECTOR_SIZE);
            if (dma_xfer(lba, count, true))
            {
                if (ata_wait_not_busy(ATA_TIMEOUT_MS))
                {
                    io_outb(ATA_DRIVE_HEAD, lba48_ok ? 0x40 : 0xE0);
                    ata_delay();
                    io_outb(ATA_CMD_STATUS,
                            lba48_ok ? ATA_CMD_FLUSH_CACHE_EXT : ATA_CMD_FLUSH_CACHE);
                    if (ata_wait_irq(FLUSH_TIMEOUT_MS))
                    {
                        dma_fail_streak = 0;
                        return true;
                    }
                }
                /* command issued but completion/flush failed: fall through */
            }
            /* Level B once: try enabling the controller's UDMA bit before
             * giving up on DMA (see ata_read_chunk for the rationale). */
            if (!ctrl_quirk_tried && controller_enable_udma())
            {
                debug_print("[ata] enabled controller UDMA, retrying DMA\n");
                (void)ata_set_udma(udma_mode);
                continue;
            }
            if (++dma_fail_streak >= DMA_FAIL_LIMIT)
            {
                dma_ok = false;
                debug_print("[ata] DMA unreliable, switching to PIO\n");
            }
        }

        /* PIO fallback (or PIO-only). */
        if (pio_write_lba28((uint32_t)lba, (uint8_t)count, in))
            return true;

        debug_print("[ata] write failed, retrying\n");
        ata_recover();
    }
    return false;
}

/* Public write: split into MAX_SECTORS chunks. See ata_read for why this
 * matters (64 KB DMA buffer + 8-bit sector count where 0 == 256). */
static bool
ata_write(uint64_t lba, uint16_t count, const void *in)
{
    const uint8_t *p = (const uint8_t *)in;
    while (count > 0)
    {
        uint16_t chunk = count > MAX_SECTORS ? MAX_SECTORS : count;
        if (!ata_write_chunk(lba, chunk, p))
            return false;
        lba   += chunk;
        p     += (uint32_t)chunk * SECTOR_SIZE;
        count -= chunk;
    }
    return true;
}

/* === IDENTIFY (PIO, once at init) === */
static bool
ata_identify(void *buffer)
{
    /* Primary-master IDENTIFY, POLLING-based and IRQ-INDEPENDENT.
     *
     * The previous version waited on the ATA IRQ (ata_wait_irq). That works
     * under QEMU, where IDENTIFY completes instantly so the BSY=0 fast-path
     * returns before the IRQ even matters. On the real Armada E500 the disk
     * holds BSY for milliseconds, so the fast-path misses and the code blocks
     * on the interrupt — but the PIIX4 IDE there is routed to IRQ 11 (per the
     * EVEREST PCI dump, cfg 0x3C = 0x0B), in compatibility mode, and the
     * boot-time probe cannot rely on that interrupt being delivered where it
     * waits. Result: timeout -> "no drive" -> the primary-master disk is
     * invisible and the partitioner sees nothing.
     *
     * The other three slots already probe correctly via ata_identify_slot()
     * using nIEN + polling. This brings slot 0 to the same robust method:
     * disable device IRQs (nIEN), drive-select, issue IDENTIFY, poll BSY/DRQ
     * with a generous budget, and PIO the 512-byte block — no interrupt in
     * the path at all. UDMA/LBA48 setup downstream is unchanged; it keys off
     * the IDENTIFY data this fills, not off how we waited. */
    const uint16_t alt  = ATA_ALT_STATUS;  /* 0x3F6 primary alt-status   */

    /* nIEN = 1: device IRQs off while we poll. */
    io_outb(alt, 0x02);

    /* Drive select (master) + 400ns settle. */
    io_outb(ATA_DRIVE_HEAD, 0xA0);
    io_inb(alt); io_inb(alt); io_inb(alt); io_inb(alt);

    if (!ide_poll_bsy_clear(alt, 2400u)) { io_outb(alt, 0x00); return false; }

    /* Zero the command-block registers, then issue IDENTIFY. */
    io_outb(ATA_SECT_COUNT, 0);
    io_outb(ATA_LBA_LO,  0);
    io_outb(ATA_LBA_MID, 0);
    io_outb(ATA_LBA_HI,  0);
    io_outb(ATA_CMD_STATUS, ATA_CMD_IDENTIFY);

    io_inb(alt); io_inb(alt); io_inb(alt); io_inb(alt);

    /* Status == 0 -> nothing on this slot (floating bus). */
    if (io_inb(alt) == 0) { io_outb(alt, 0x00); return false; }

    if (!ide_poll_drq(alt, 2400u)) { io_outb(alt, 0x00); return false; }

    /* Reject ATAPI: it would have set the signature in LBA_MID/HI (0x14/0xEB)
     * and aborted IDENTIFY; if those are non-zero this isn't an ATA HDD. */
    if (io_inb(ATA_LBA_MID) != 0 || io_inb(ATA_LBA_HI) != 0)
    { io_outb(alt, 0x00); return false; }

    uint16_t *buf = (uint16_t *)buffer;
    for (int i = 0; i < SECTOR_SIZE / 2; i++)
        buf[i] = io_inw(ATA_DATA);

    io_outb(alt, 0x00);   /* re-enable device IRQs for normal operation */
    return true;
}

/* Parse IDENTIFY → LBA48 flag, best UDMA mode */
static void
parse_identify(const uint16_t *id, uint8_t *best_udma)
{
    lba48_ok = (id[83] & (1u << 10)) != 0;

    *best_udma = 0xFF;
    if (id[53] & 0x04)
    {
        /* Pick the highest UDMA mode BOTH the drive supports (id[88] low byte)
         * AND the controller can physically clock. A modern drive or CF/SSD
         * adapter advertises UDMA-5/6 even behind an ATA-33 bridge; negotiating
         * that overruns the controller's sampling and yields ATA ERR/DF. The
         * cap keeps us within the bridge's real capability. */
        uint8_t sup = (uint8_t)(id[88] & 0x7F);
        uint8_t cap = controller_udma_cap();
        for (int m = 6; m >= 0; m--)
            if ((sup & (1u << m)) && (uint8_t)m <= cap)
            {
                *best_udma = (uint8_t)m;
                break;
            }
    }
}

/* === Request buffer === */
static uint8_t req_buf[DMA_BUF_SIZE];

/*  ATAPI support.
 *
 *  ATAPI devices (CDROMs) can sit on any of the four IDE slots:
 *     0: primary master    (base 0x1F0, IRQ 14)
 *     1: primary slave     (base 0x1F0, IRQ 14)
 *     2: secondary master  (base 0x170, IRQ 15)   ← QEMU default
 *     3: secondary slave   (base 0x170, IRQ 15)
 *
 *  Scan all four at boot, detect ATAPI by the LBA_MID/LBA_HI
 *  signature (0x14/0xEB per ATAPI spec), emit one
 *  HOTPLUG_CREATE_LEGACY_BUBBLE per device. Bubble is static — IDE
 *  has no media-change IRQ; presence is checked lazily when the user
 *  activates it.
 *
 *  cdrom driver calls in via opcode ATA_PACKET (100) with a CDB +
 *  optional data-in buffer. PIO only. Secondary channel IRQ15 uses
 *  its own receive port so primary/secondary completions can never
 *  cross.
 */

#define ATA_CMD_PACKET       0xA0   /* PACKET (send CDB) */
#define ATAPI_SIG_MID        0x14
#define ATAPI_SIG_HI         0xEB
#define ATAPI_MAX_SLOTS      4
#define ATAPI_CDB_LEN        12
#define ATAPI_XFER_MAX       8192   /* per-request cap, plenty for TOC + PVD */
#define ATA_PACKET_OPCODE    100

typedef struct
{
    bool     present;
    uint8_t  channel;   /* 0 primary, 1 secondary */
    uint8_t  drive;     /* 0 master, 1 slave */
    uint16_t base;      /* 0x1F0 or 0x170 */
    uint16_t alt;       /* 0x3F6 or 0x376 */
    uint8_t  irq;       /* 14 or 15 */
} atapi_slot_t;

static atapi_slot_t atapi_slots[ATAPI_MAX_SLOTS];
static uint32_t     atapi_irq15_port = 0;

/* === ATA HDD slots ===
 *
 * Parallel to atapi_slots[]. Same 4-slot layout (channel*2 + drive),
 * populated by hdd_scan_other_slots() and hdd_register_primary_master().
 * hdd_identify_cache holds the raw 512-byte IDENTIFY blob so the
 * IDENTIFY opcode is a memory copy, not a re-IDENTIFY round-trip.
 *
 * The primary master (slot 0) is special: the existing init path
 * already runs ata_identify() on it at boot to set up UDMA/LBA48,
 * so we just register what we already learned. Slots 1..3 are
 * scanned afterwards via polling — no IRQ infrastructure needed
 * for the secondary channel just for capability probing. */
typedef struct
{
    bool     present;
    uint8_t  channel;
    uint8_t  drive;
    uint16_t base;
    uint16_t alt;
    uint8_t  irq;
    bool     lba48;
    bool     is_ssd;          /* IDENTIFY word 217 == 1 (non-rotating media) */
    bool     trim_supported;  /* IDENTIFY word 169 bit 0 (DSM TRIM supported) */
    uint64_t total_sectors;
    char     model[41];
} hdd_slot_t;

static hdd_slot_t hdd_slots[ATA_MAX_DISKS];
static uint16_t   hdd_identify_cache[ATA_MAX_DISKS][SECTOR_SIZE / 2];

/* Attese ATAPI: stesse prese di ide_poll_* sul core cooperativo
 * (finestra calda + 1 campione/ms dormendo) — era rimasto l'ultimo
 * spin a iterazioni della 1.0. Budget in millisecondi reali. */
static bool
atapi_wait_bsy_clear(uint16_t alt, uint32_t budget_ms)
{
    return ide_poll_core(alt, budget_ms, false);
}

static bool
atapi_wait_drq(uint16_t alt, uint32_t budget_ms)
{
    return ide_poll_core(alt, budget_ms, true);
}

/* Select a slot: write drive/head select and give the device 400ns
 * to settle per ATA spec. Clears all other register state for the
 * coming command. */
static void
atapi_select_slot(const atapi_slot_t *s)
{
    io_outb(s->base + 6, (uint8_t)(0xA0 | (s->drive << 4)));
    /* 400ns spec delay: four reads of ALT_STATUS. */
    io_inb(s->alt); io_inb(s->alt); io_inb(s->alt); io_inb(s->alt);
}

/* Drain stale IRQ notifications left over from the previous PACKET.
 *
 * After a successful command the IRQ port can carry one extra notify
 * bit (ATAPI drives in QEMU sometimes re-assert IRQ post-completion,
 * and the port's notify_bits accumulator can hold one stale wake).
 * Without draining, the next atapi_wait_irq returns instantly with
 * the previous command's idle status (BSY=0, DRQ=0), and the data
 * phase reads zero bytes silently.
 *
 * Same pattern as fdc_drain_irq in the floppy driver. Per-channel —
 * primary and secondary use separate ports. */
static void
atapi_drain_irq(uint8_t channel)
{
    uint32_t port = (channel == 0) ? irq_port : atapi_irq15_port;
    uint8_t  irq  = (channel == 0) ? irq_num  : 15;
    if (!port) return;

    dob_msg_t drain;
    while (1)
    {
        memset(&drain, 0, sizeof(drain));
        if (dob_ipc_receive_nowait(port, &drain) != DOB_OK) break;
        if (drain.type == 3)   /* IPC_MSG_NOTIFY */
        {
            /* Read status to deassert the device's internal IRQ line,
             * then unmask at the PIC. Same pattern as the wait path. */
            uint16_t st_port = (channel == 0) ? ATA_CMD_STATUS : 0x177;
            (void)io_inb(st_port);
            irq_done(irq);
        }
    }
}

/* Wait on the channel's IRQ with a timeout. Reads status once the
 * notification arrives, acks BMIDE if we're on the primary, and
 * returns the latched status byte. Returns 0 on timeout. */
static uint8_t
atapi_wait_irq(uint8_t channel, uint32_t timeout_ms)
{
    uint32_t port = (channel == 0) ? irq_port : atapi_irq15_port;
    uint8_t  irq  = (channel == 0) ? irq_num  : 15;
    uint16_t cmd  = (channel == 0) ? ATA_CMD_STATUS : 0x177;

    int tid = timer_set(port, timeout_ms, 0);
    for (;;)
    {
        dob_msg_t m; memset(&m, 0, sizeof(m));
        if (dob_ipc_receive(port, &m) != DOB_OK)
        {
            timer_cancel_async(tid);
            return 0;
        }
        if (m.type == 3)
        {
            uint8_t st = io_inb(cmd);
            irq_done(irq);
            timer_cancel_async(tid);
            return st;
        }
        if (m.code == 70)   /* TIMER */
        {
            return 0;
        }
    }
}

/* Is this slot an ATAPI device?
 *
 * After a soft reset (or immediately after power-up with no activity),
 * an ATAPI device posts its signature into LBA_MID=0x14, LBA_HI=0xEB.
 * An ATA HDD posts 0x00/0x00. Absent drive reads 0xFF/0xFF.
 *
 * We don't issue any commands here — we just select the slot and read
 * the signature registers. This is idempotent and cannot disturb an
 * HDD that's already live on the same bus. */
/* Per-channel soft reset, required before reading the ATAPI signature.
 * An ATAPI drive only posts its 0x14/0xEB identifying bytes into
 * LBA_MID/LBA_HI immediately after a reset. If anyone (us, BIOS,
 * GRUB) has issued any command on the channel since the last reset,
 * those registers contain stale data. We reset both channels and
 * wait for BSY to clear on each. */
static void
atapi_channel_soft_reset(uint16_t alt)
{
    /* Set SRST bit in the device control register, wait ≥5µs,
     * clear it, wait for the device to clear BSY. */
    io_outb(alt, 0x06);   /* nIEN | SRST */
    for (volatile int i = 0; i < 64; i++) io_inb(alt);   /* ≥5µs */
    io_outb(alt, 0x02);   /* nIEN, SRST cleared */
    for (volatile int i = 0; i < 64; i++) io_inb(alt);

    /* Wait up to ~2s for BSY=0 on the channel. */
    for (int i = 0; i < 200000; i++)
    {
        if (!(io_inb(alt) & ATA_SR_BSY)) break;
        for (volatile int k = 0; k < 32; k++) io_inb(alt);
    }

    /* Re-enable interrupts — our IRQ-based wait paths need nIEN=0. */
    io_outb(alt, 0x00);
}

/* Scan all four IDE slots and populate atapi_slots[].
 * Called once at boot from main(). Returns the number of ATAPI
 * devices found. Safe to call after the HDD primary-master has
 * already been IDENTIFY'd: we issue a per-channel soft reset
 * before reading the signature, which is the only way to force
 * the drive to re-post its identifying bytes. The HDD path
 * re-selects primary master at the end, so ata_read / ata_write
 * keep working as before. */
static int
atapi_scan_all(void)
{
    debug_print("[ata] ATAPI scan: resetting both IDE channels\n");

    const struct { uint8_t ch, dr; uint16_t base, alt; uint8_t irq; } spec[4] = {
        { 0, 0, 0x1F0, 0x3F6, 14 },
        { 0, 1, 0x1F0, 0x3F6, 14 },
        { 1, 0, 0x170, 0x376, 15 },
        { 1, 1, 0x170, 0x376, 15 },
    };

    int found = 0;
    /* Reset both channels exactly once, not once per slot: SRST
     * affects the whole channel (both master and slave). */
    atapi_channel_soft_reset(0x3F6);   /* primary   */
    atapi_channel_soft_reset(0x376);   /* secondary */

    for (int i = 0; i < ATAPI_MAX_SLOTS; i++)
    {
        atapi_slot_t *s = &atapi_slots[i];
        memset(s, 0, sizeof(*s));
        s->channel = spec[i].ch;
        s->drive   = spec[i].dr;
        s->base    = spec[i].base;
        s->alt     = spec[i].alt;
        s->irq     = spec[i].irq;

        /* Debug: dump what the slot actually says, so we can see
         * whether QEMU exposes the drive at all. */
        atapi_select_slot(s);
        uint8_t lba_mid = io_inb(s->base + 4);
        uint8_t lba_hi  = io_inb(s->base + 5);
        uint8_t status  = io_inb(s->alt);

        char diag[64];
        const char *tag = "empty";
        if (status == 0xFF)                               tag = "floating";
        else if (lba_mid == ATAPI_SIG_MID &&
                 lba_hi  == ATAPI_SIG_HI)                 tag = "ATAPI";
        else if (lba_mid == 0x00 && lba_hi == 0x00)       tag = "ATA-HDD";
        snprintf(diag, sizeof(diag),
                 "[ata] slot %d: mid=%02x hi=%02x st=%02x -> %s\n",
                 i, lba_mid, lba_hi, status, tag);
        debug_print(diag);

        if (lba_mid == ATAPI_SIG_MID && lba_hi == ATAPI_SIG_HI &&
            status != 0xFF)
        {
            s->present = true;
            found++;
        }
    }

    /* Restore primary master selection for the HDD path. */
    io_outb(0x1F0 + 6, 0xA0);
    io_inb(0x3F6);

    return found;
}

/* Background thread body: wait for hotplug to register, then push
 * one CREATE_LEGACY_BUBBLE per ATAPI drive we found at scan time.
 *
 * Why a thread: the ata server loop must keep running — DobFileSystem
 * hits us for every disk sector, and we start long before hotplug
 * exists. Blocking the main thread on dob_registry_wait would freeze
 * the whole filesystem. A background worker lets us wait without
 * starving anyone.
 *
 * The worker exits silently after pushing. If hotplug never appears
 * (bug somewhere upstream), dob_registry_wait times out and we log
 * a note but do no damage. */
static void
atapi_pusher_thread(void *arg)
{
    (void)arg;

    uint32_t hp = dob_registry_wait("hotplug", 60000);
    if (!hp)
    {
        debug_print("[ata] hotplug never appeared, ATAPI icons skipped\n");
        return;
    }

    for (int i = 0; i < ATAPI_MAX_SLOTS; i++)
    {
        if (!atapi_slots[i].present) continue;

        hotplug_legacy_create_t req;
        req.bus_type = BUS_TYPE_LEGACY_IDE_ATAPI;
        req.unit     = (uint8_t)i;
        req.io_base  = atapi_slots[i].base;

        dob_msg_t m, r;
        memset(&m, 0, sizeof(m));
        memset(&r, 0, sizeof(r));
        m.code         = HOTPLUG_CREATE_LEGACY_BUBBLE;
        m.payload      = &req;
        m.payload_size = sizeof(req);
        (void)dob_ipc_call(hp, &m, &r);

        debug_print("[ata] Published ATAPI slot to hotplug\n");
    }
}

/* Boot-time partition pusher: same pattern as atapi_pusher_thread,
 * but iterates the HDD slots and calls partition_scan_announce on
 * each present disk. First-time call from partition_scan_announce
 * emits APPEARED for every FAT32 partition found, so the desktop
 * icons appear without the user having to issue an explicit RESCAN.
 *
 * Runs once at boot; subsequent rescans come in via the
 * ATA_OP_RESCAN_PARTITIONS opcode (issued by DobDisk after writing
 * an MBR, or by anyone else who needs a fresh announce). */
static bool ata_partition_announce(uint32_t disk);   /* fwd; defined near handle_message */

/* HDD partition pusher: waits for hotplug to be registered, then
 * triggers an MBR scan for every present HDD slot.
 *
 * The scan itself is NOT done in this thread. Reading the MBR over
 * DMA from a background thread while the main thread is concurrently
 * serving READ/WRITE IPC from DobFileSystem races on the shared
 * dma_buf and the bus-master DMA engine — the buffer ends up holding
 * stale data from whichever transfer finished last, and emitted
 * subdevices fail because the MBR looks empty.
 *
 * Fix: post an ATA_OP_RESCAN_PARTITIONS message to our own server
 * port. The main thread receives it through dob_server_loop and
 * runs partition_scan_announce in the same execution context that
 * serves every other disk-I/O request — fully serialised. This
 * thread is just an event-driven trigger, no hardware access.
 *
 * dob_ipc_post is fire-and-forget; the main thread answers no one.
 * We post one msg per present disk. */
static void
hdd_partition_pusher_thread(void *arg)
{
    (void)arg;

    uint32_t hp = dob_registry_wait("hotplug", 60000);
    if (!hp)
    {
        debug_print("[ata] hotplug never appeared, partition icons skipped\n");
        return;
    }

    /* Also wait for the GUI to be up before announcing partitions.
     *
     * Boot race: the partition APPEARED is a fire-and-forget post to
     * hotplug, which re-broadcasts it to its current subscribers. CD and
     * floppy bubbles are created synchronously during hotplug's own
     * startup, so they always exist by the time dobinterface subscribes
     * and are caught by the subscribe-replay. The partition bubble, born
     * from this async post, instead races dobinterface's subscription
     * (dobinterface subscribes to hotplug as its LAST init step): if we
     * announce before that, the broadcast reaches no GUI and the icon is
     * lost, even though DobDisk-triggered rescans later work fine because
     * by then the GUI is listening. Waiting for "dobinterface" to register
     * closes the window — the announce then lands with the subscriber
     * present. This is a cosmetic-ordering gate only; disk I/O for
     * DobFileSystem is served by the main thread regardless and is not
     * blocked by this wait. If the GUI never comes up (headless/boot
     * failure) we time out and proceed: the announce is harmless with no
     * subscribers, and a later manual rescan still works. */
    (void)dob_registry_wait("dobinterface", 60000);

    /* dob_registry_wait returns when dobinterface REGISTERS, but it
     * subscribes to hotplug a few synchronous calls later (its final init
     * step). Give it a short settle so our announce broadcast lands with
     * the subscription already in place rather than in the tiny gap
     * between register and subscribe. Cheap and only at boot. */
    sleep_ms(250);

    uint32_t my = dob_server_get_port();
    if (!my) return;

    for (uint32_t i = 0; i < ATA_MAX_DISKS; i++)
    {
        if (!hdd_slots[i].present) continue;
        dob_msg_t m;
        memset(&m, 0, sizeof(m));
        m.code = ATA_OP_RESCAN_PARTITIONS;
        m.arg0 = i;
        dob_ipc_post(my, &m);
    }
    debug_print("[ata] Partition scan scheduled on every present disk\n");
}

/* Execute one ATAPI PACKET command via PIO.
 *   slot  — index into atapi_slots[]
 *   cdb   — 12-byte SCSI command descriptor block
 *   buf   — destination for data-in bytes (NULL if alloc_len == 0)
 *   alloc_len — maximum bytes the caller wants; ATAPI reports the
 *               actual transfer in two byte-count registers.
 *   *out_transferred — populated with the real transfer size.
 *
 * Returns true on successful completion (good or no-data), false on
 * device error, timeout, or protocol violation. Data-out is not
 * supported — CDROMs are read-only from our perspective, and every
 * command we issue (TEST_UNIT_READY, READ_TOC, READ(10), REQUEST_SENSE,
 * START_STOP_UNIT) is either zero-data or data-in. */
static bool
atapi_packet_exec(int slot, const uint8_t *cdb,
                  uint8_t *buf, uint32_t alloc_len,
                  uint32_t *out_transferred)
{
    char dbg[96];
    snprintf(dbg, sizeof(dbg),
             "[ata] PACKET slot=%d cdb[0]=0x%02x alloc=%u\n",
             slot, cdb[0], alloc_len);
    debug_print(dbg);

    if (slot < 0 || slot >= ATAPI_MAX_SLOTS)
    {
        debug_print("[ata] PACKET: bad slot index\n");
        return false;
    }
    atapi_slot_t *s = &atapi_slots[slot];
    if (!s->present)
    {
        debug_print("[ata] PACKET: slot not present\n");
        return false;
    }

    if (out_transferred) *out_transferred = 0;

    /* 1. Select slot + wait ready. */
    atapi_select_slot(s);
    if (!atapi_wait_bsy_clear(s->alt, 1200))
    {
        debug_print("[ata] PACKET: BSY never cleared\n");
        return false;
    }

    /* 1b. Drain any stale IRQ notifications from previous PACKETs.
     * Without this, the data-in loop can see a stale notify_bits and
     * read a DRQ=0 status before the drive has processed the new CDB,
     * silently returning total=0 with success. See atapi_drain_irq. */
    atapi_drain_irq(s->channel);

    /* 2. Program byte-count limit in LBA_MID / LBA_HI. The device
     *    will never transfer more per IRQ than this in a single
     *    data-in burst. 2KB (one sector) is the safe minimum ATAPI
     *    spec guarantees; we use the caller-supplied alloc_len
     *    clamped to 8KB. */
    uint16_t byte_limit = alloc_len > 0xFFFE ? 0xFFFE : (uint16_t)alloc_len;
    io_outb(s->base + 1, 0);                 /* features: no DMA */
    io_outb(s->base + 4, byte_limit & 0xFF); /* LBA_MID */
    io_outb(s->base + 5, byte_limit >> 8);   /* LBA_HI  */

    /* 3. Issue PACKET command. Drive goes BSY, then posts DRQ when
     *    it's ready to accept the 12-byte CDB. */
    io_outb(s->base + 7, ATA_CMD_PACKET);

    if (!atapi_wait_drq(s->alt, 6000))
    {
        debug_print("[ata] PACKET: DRQ never set after PACKET cmd\n");
        return false;
    }

    /* 4. Ship the CDB: 12 bytes = 6 word writes to the data port. */
    const uint16_t *w = (const uint16_t *)cdb;
    for (int i = 0; i < ATAPI_CDB_LEN / 2; i++)
        io_outw(s->base, w[i]);

    /* 5. Data-in loop. After writing the CDB the drive goes BSY,
     *    then either (a) DRQ=1 with data to read, (b) DRQ=0 with
     *    BSY=0 meaning command complete, or (c) ERR. Each data-in
     *    burst is preceded by its own IRQ.
     *
     *    The same IRQ that signals "DRQ=0, done" IS the completion
     *    IRQ — no separate IRQ follows. So we track whether the
     *    completion IRQ has already been consumed in the loop.
     *    If it has, step 6 is a no-op. */
    uint32_t total = 0;
    bool done = false;
    uint8_t last_status = 0;
    while (alloc_len > 0 && !done)
    {
        uint8_t st = atapi_wait_irq(s->channel, 3000);
        if (!st)
        {
            debug_print("[ata] PACKET: IRQ wait TIMEOUT in data loop\n");
            return false;
        }
        last_status = st;
        if (st & ATA_SR_ERR)
        {
            debug_print("[ata] PACKET: ERR bit set in data loop\n");
            return false;
        }
        if (!(st & ATA_SR_DRQ)) { done = true; break; }   /* command complete */

        /* Actual transfer size for this burst. */
        uint32_t mid = io_inb(s->base + 4);
        uint32_t hi  = io_inb(s->base + 5);
        uint32_t burst = mid | (hi << 8);
        if (burst == 0 || burst > alloc_len) burst = alloc_len;

        /* Odd byte-counts are legal in ATAPI; pad with a dummy
         * word read and discard the extra byte. */
        uint32_t words = (burst + 1) / 2;
        for (uint32_t i = 0; i < words; i++)
        {
            uint16_t v = io_inw(s->base);
            if (buf && total + 2 <= alloc_len + 1)
            {
                buf[total]     = (uint8_t)(v & 0xFF);
                if (total + 1 < alloc_len)
                    buf[total + 1] = (uint8_t)(v >> 8);
            }
            total += 2;
        }
        if (total > alloc_len) total = alloc_len;
        if (burst >= alloc_len) { alloc_len = 0; }
        else                     alloc_len -= burst;
    }

    /* 6. Wait for the final completion IRQ only if we haven't already
     *    consumed it in the loop above. For zero-data commands
     *    (alloc_len == 0 from the start) or for data-in that stopped
     *    short with DRQ=0, the completion IRQ was the last one we
     *    saw; no extra wait is needed. For data-in that filled our
     *    buffer exactly (alloc_len reached 0 mid-burst), the drive
     *    still owes us a BSY-clear IRQ — that's this wait. */
    if (!done)
    {
        uint8_t final_st = atapi_wait_irq(s->channel, 3000);
        if (final_st == 0)
        {
            debug_print("[ata] PACKET: final IRQ TIMEOUT\n");
            return false;
        }
        if (final_st & ATA_SR_ERR)
        {
            debug_print("[ata] PACKET: final IRQ has ERR bit\n");
            return false;
        }
    }
    else if (last_status & ATA_SR_ERR)
    {
        debug_print("[ata] PACKET: last_status has ERR bit\n");
        return false;
    }

    if (out_transferred) *out_transferred = total;
    {
        char dbg2[64];
        snprintf(dbg2, sizeof(dbg2),
                 "[ata] PACKET: OK transferred=%u\n", total);
        debug_print(dbg2);
    }
    return true;
}

/* === Multi-disk support (slots 1..3) ===
 *
 * Polling-based variants of IDENTIFY / read / write that work on any
 * IDE slot. Slot 0 (primary master) keeps the existing IRQ+DMA fast
 * path; slots 1..3 are PIO LBA28 only — adequate for the disk utility
 * and any DobFS instance pointed at a secondary partition, capped at
 * 128 GB per disk (LBA28 limit). Future work can extend LBA48 PIO
 * and/or IRQ-driven secondary-channel I/O if we ever care about
 * throughput off the primary master. */

/* Parse IDENTIFY model string. Words 27..46 store 40 bytes of ASCII
 * with the two bytes of each word swapped (ATA quirk). */
static void
identify_extract_model(const uint16_t *id, char *out)
{
    for (int i = 0; i < 20; i++)
    {
        out[i * 2]     = (char)(id[27 + i] >> 8);
        out[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    out[40] = '\0';
    for (int i = 39; i >= 0 && out[i] == ' '; i--)
        out[i] = '\0';
}

/* Total sector count. LBA48 capable: 64-bit at words 100..103.
 * Otherwise 32-bit at words 60..61. */
static uint64_t
identify_extract_total_sectors(const uint16_t *id, bool lba48_supp)
{
    if (lba48_supp)
    {
        return  (uint64_t)id[100]
             | ((uint64_t)id[101] << 16)
             | ((uint64_t)id[102] << 32)
             | ((uint64_t)id[103] << 48);
    }
    return (uint64_t)id[60] | ((uint64_t)id[61] << 16);
}

/* Media type + TRIM capability from IDENTIFY:
 *   word 217 == 0x0001  -> non-rotating media (SSD/flash)
 *   word 169 bit 0      -> DATA SET MANAGEMENT (TRIM) supported
 * Both are advisory hints the disk reports; absence just means "unknown",
 * defaulting to HDD / no-TRIM. */
static void
identify_extract_ssd_trim(const uint16_t *id, bool *is_ssd, bool *trim)
{
    if (is_ssd) *is_ssd = (id[217] == 0x0001);
    if (trim)   *trim   = (id[169] & 0x0001) != 0;
}

/* === Poll di stato ATA: budget in TEMPO reale, cooperativo ==============
 * PERCHE' il poll e' cooperativo e in tempo reale: un poll che conta
 * iterazioni (33 letture di porta l'una, ~12 us a giro su QEMU) con
 * budget da 1'000'000 equivale a ~12 SECONDI di spin a prio 2 per un
 * singolo wait fallito (60 s sul flush da 5M). Un comando lento o
 * spurio inchioda la CPU, affama i thread di pari priorita' e mette in
 * convoglio DobFileSystem e ogni suo client: comandi in ritardo, click
 * senza effetto, sistema percepito come fermo. Ora: finestra calda di pochi campioni in
 * spin stretto (le transizioni PIO rapide si chiudono li' in
 * microsecondi), poi UN campione al millisecondo DORMENDO — la CPU
 * torna allo scheduler e il budget e' millisecondi veri. Le firme
 * restano in "us" per non toccare i callsite: 1'000'000 us = 1000 ms. */

#define IDE_POLL_HOT_SAMPLES 200u

static bool
ide_poll_core(uint16_t alt, uint32_t budget_ms, bool want_drq)
{
    for (uint32_t i = 0; i < IDE_POLL_HOT_SAMPLES; i++)
    {
        uint8_t st = io_inb(alt);
        if (want_drq)
        {
            if (st & ATA_SR_ERR)                          return false;
            if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ))  return true;
        }
        else if (!(st & ATA_SR_BSY))
        {
            return true;
        }
    }

    for (uint32_t ms = 0; ms < budget_ms; ms++)
    {
        sleep_ms(1);
        uint8_t st = io_inb(alt);
        if (want_drq)
        {
            if (st & ATA_SR_ERR)                          return false;
            if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ))  return true;
        }
        else if (!(st & ATA_SR_BSY))
        {
            return true;
        }
    }
    return false;
}

/* I budget dei chiamanti sono in MILLISECONDI REALI, tarati sugli
 * effettivi della 1.0 (dove il conteggio a iterazioni valeva ~12 us
 * l'una): 2'400 ms probe/identify, 12'000 ms wait dati, 60'000 ms il
 * flush; 1'200/6'000 ms i passi ATAPI. Dopo la finestra calda si
 * campiona 1 volta al ms DORMENDO: anche 60 s costano zero CPU. */

/* Attende BSY=0 (o timeout). budget_ms reali. */
static bool
ide_poll_bsy_clear(uint16_t alt, uint32_t budget_ms)
{
    return ide_poll_core(alt, budget_ms, false);
}

/* Attende DRQ=1 con BSY=0, fallisce su ERR. budget_ms reali. */
static bool
ide_poll_drq(uint16_t alt, uint32_t budget_ms)
{
    return ide_poll_core(alt, budget_ms, true);
}

/* IDENTIFY on any slot, polling-based. Sets nIEN so we don't pile up
 * uncollected interrupts on the secondary channel. */
static bool
ata_identify_slot(int slot, void *buffer)
{
    if (slot < 0 || slot >= ATA_MAX_DISKS) return false;
    hdd_slot_t *s = &hdd_slots[slot];
    uint16_t base = s->base, alt = s->alt;

    /* nIEN = device IRQs disabled while we poll. */
    io_outb(alt, 0x02);

    /* Drive select with 400ns settle. */
    io_outb(base + 6, 0xA0 | (s->drive << 4));
    io_inb(alt); io_inb(alt); io_inb(alt); io_inb(alt);

    if (!ide_poll_bsy_clear(alt, 2400))
    {
        io_outb(alt, 0x00);
        return false;
    }

    io_outb(base + 1, 0);
    io_outb(base + 2, 0);
    io_outb(base + 3, 0);
    io_outb(base + 4, 0);
    io_outb(base + 5, 0);
    io_outb(base + 7, ATA_CMD_IDENTIFY);

    io_inb(alt); io_inb(alt); io_inb(alt); io_inb(alt);

    /* Status = 0 means the slot is empty (floating bus). */
    if (io_inb(alt) == 0)
    {
        io_outb(alt, 0x00);
        return false;
    }

    if (!ide_poll_drq(alt, 2400))
    {
        io_outb(alt, 0x00);
        return false;
    }

    /* Confirm ATA HDD (not ATAPI) by checking the signature registers.
     * ATAPI would have ERR'd via the IDENTIFY (it needs IDENTIFY_PACKET
     * 0xA1), but be defensive. */
    if (io_inb(base + 4) != 0 || io_inb(base + 5) != 0)
    {
        io_outb(alt, 0x00);
        return false;
    }

    uint16_t *buf = (uint16_t *)buffer;
    for (int i = 0; i < SECTOR_SIZE / 2; i++)
        buf[i] = io_inw(base + 0);

    io_outb(alt, 0x00);
    return true;
}

/* PIO LBA28 read on any slot, polling-based. */
static bool
pio_read_slot_lba28(int slot, uint32_t lba, uint8_t count, void *buffer)
{
    if (slot < 0 || slot >= ATA_MAX_DISKS)             return false;
    if (lba > 0x0FFFFFFFu)                              return false;
    hdd_slot_t *s = &hdd_slots[slot];
    uint16_t base = s->base, alt = s->alt;

    io_outb(alt, 0x02);   /* nIEN */

    if (!ide_poll_bsy_clear(alt, 2400)) { io_outb(alt, 0x00); return false; }

    io_outb(base + 6, 0xE0 | (s->drive << 4) | ((lba >> 24) & 0x0F));
    io_inb(alt); io_inb(alt); io_inb(alt); io_inb(alt);
    io_outb(base + 1, 0);
    io_outb(base + 2, count);
    io_outb(base + 3, (uint8_t)(lba));
    io_outb(base + 4, (uint8_t)(lba >> 8));
    io_outb(base + 5, (uint8_t)(lba >> 16));
    io_outb(base + 7, ATA_CMD_READ_PIO);

    uint16_t *buf = (uint16_t *)buffer;
    for (uint8_t sect = 0; sect < count; sect++)
    {
        io_inb(alt); io_inb(alt); io_inb(alt); io_inb(alt);
        if (!ide_poll_drq(alt, 2400)) { io_outb(alt, 0x00); return false; }
        for (int i = 0; i < SECTOR_SIZE / 2; i++)
            buf[sect * (SECTOR_SIZE / 2) + i] = io_inw(base + 0);
    }

    io_outb(alt, 0x00);
    return true;
}

/* SMART READ DATA on any slot, PIO data-in single sector.
 *
 * Issues ATA command 0xB0 with the SMART_READ_DATA (0xD0) sub-command
 * in the Features register and the SMART magic bytes (0x4F, 0xC2) in
 * LBA mid/hi. Returns one 512-byte sector laid out per ATA-7
 * specification:
 *
 *   bytes 0..1   vendor structure revision
 *   bytes 2..361 30 vendor attributes (12 bytes each)
 *                Each attribute:
 *                  0     id        (0 = unused entry)
 *                  1..2  flags
 *                  3     current value
 *                  4     worst value
 *                  5..10 raw value (vendor-specific)
 *                  11    reserved
 *   bytes 362..  offline status, self-test status, etc.
 *
 * SMART is optional. Drives that lack it return command-aborted (ERR
 * set in status, ABRT in error register) — this manifests as either
 * (a) BSY clear with ERR set, or (b) DRQ never asserting. Both cases
 * are caught and reported via false return.
 *
 * Caller must provide a 512-byte buffer. */
static bool
pio_smart_read_data_slot(int slot, void *buffer)
{
    if (slot < 0 || slot >= ATA_MAX_DISKS)        return false;
    if (!hdd_slots[slot].present)                 return false;

    hdd_slot_t *s = &hdd_slots[slot];
    uint16_t base = s->base, alt = s->alt;

    io_outb(alt, 0x02);   /* nIEN — we poll */

    if (!ide_poll_bsy_clear(alt, 2400)) { io_outb(alt, 0x00); return false; }

    /* Select drive — LBA28-style, no LBA bits since this is a CMD-only
     * operation that takes its parameters from Features and LBA mid/hi. */
    io_outb(base + 6, 0xE0 | (s->drive << 4));
    io_inb(alt); io_inb(alt); io_inb(alt); io_inb(alt);

    /* Features = SMART sub-command, sector count = 1 (per spec for
     * READ_DATA), LBA low irrelevant, LBA mid/hi carry the magic. */
    io_outb(base + 1, ATA_SMART_READ_DATA);
    io_outb(base + 2, 1);
    io_outb(base + 3, 0);
    io_outb(base + 4, ATA_SMART_LBA_MID);
    io_outb(base + 5, ATA_SMART_LBA_HI);
    io_outb(base + 7, ATA_CMD_SMART);

    /* Brief 400ns delay then DRQ poll. If the drive does not support
     * SMART it aborts here. */
    io_inb(alt); io_inb(alt); io_inb(alt); io_inb(alt);
    if (!ide_poll_drq(alt, 2400)) { io_outb(alt, 0x00); return false; }

    /* Read 256 16-bit words. */
    uint16_t *buf = (uint16_t *)buffer;
    for (int i = 0; i < SECTOR_SIZE / 2; i++)
        buf[i] = io_inw(base + 0);

    io_outb(alt, 0x00);
    return true;
}

/* PIO LBA28 write on any slot, polling-based. Includes a final
 * FLUSH_CACHE to mirror the primary-master fast path. */
static bool
pio_write_slot_lba28(int slot, uint32_t lba, uint8_t count, const void *buffer)
{
    if (slot < 0 || slot >= ATA_MAX_DISKS)             return false;
    if (lba > 0x0FFFFFFFu)                              return false;
    hdd_slot_t *s = &hdd_slots[slot];
    uint16_t base = s->base, alt = s->alt;

    io_outb(alt, 0x02);   /* nIEN */

    if (!ide_poll_bsy_clear(alt, 2400)) { io_outb(alt, 0x00); return false; }

    io_outb(base + 6, 0xE0 | (s->drive << 4) | ((lba >> 24) & 0x0F));
    io_inb(alt); io_inb(alt); io_inb(alt); io_inb(alt);
    io_outb(base + 1, 0);
    io_outb(base + 2, count);
    io_outb(base + 3, (uint8_t)(lba));
    io_outb(base + 4, (uint8_t)(lba >> 8));
    io_outb(base + 5, (uint8_t)(lba >> 16));
    io_outb(base + 7, ATA_CMD_WRITE_PIO);

    const uint16_t *buf = (const uint16_t *)buffer;
    for (uint8_t sect = 0; sect < count; sect++)
    {
        io_inb(alt); io_inb(alt); io_inb(alt); io_inb(alt);
        if (!ide_poll_drq(alt, 2400)) { io_outb(alt, 0x00); return false; }
        for (int i = 0; i < SECTOR_SIZE / 2; i++)
            io_outw(base + 0, buf[sect * (SECTOR_SIZE / 2) + i]);
    }

    /* Drain BSY, then FLUSH. */
    if (!ide_poll_bsy_clear(alt, 2400)) { io_outb(alt, 0x00); return false; }
    io_outb(base + 7, ATA_CMD_FLUSH_CACHE);
    if (!ide_poll_bsy_clear(alt, 2400)) { io_outb(alt, 0x00); return false; }

    io_outb(alt, 0x00);
    return true;
}

/* Dispatch to fast path (slot 0, DMA-capable) or polling PIO (1..3). */
static bool
ata_read_dispatch(uint32_t disk, uint64_t lba, uint16_t count, void *out)
{
    if (disk >= ATA_MAX_DISKS || !hdd_slots[disk].present) return false;
    if (disk == 0)
        return ata_read(lba, count, out);
    if (lba > 0x0FFFFFFFu || count > 255)                  return false;
    return pio_read_slot_lba28((int)disk, (uint32_t)lba, (uint8_t)count, out);
}

static bool
ata_write_dispatch(uint32_t disk, uint64_t lba, uint16_t count, const void *in)
{
    if (disk >= ATA_MAX_DISKS || !hdd_slots[disk].present) return false;
    if (disk == 0)
        return ata_write(lba, count, in);
    if (lba > 0x0FFFFFFFu || count > 255)                  return false;
    return pio_write_slot_lba28((int)disk, (uint32_t)lba, (uint8_t)count, in);
}

/* Register the primary master (slot 0) from the IDENTIFY blob the
 * existing boot path already collected. Idempotent. */
static void
hdd_register_primary_master(const uint16_t *id)
{
    hdd_slot_t *s = &hdd_slots[0];
    s->present       = true;
    s->channel       = 0;
    s->drive         = 0;
    s->base          = 0x1F0;
    s->alt           = 0x3F6;
    s->irq           = 14;
    s->lba48         = (id[83] & (1u << 10)) != 0;
    s->total_sectors = identify_extract_total_sectors(id, s->lba48);
    identify_extract_ssd_trim(id, &s->is_ssd, &s->trim_supported);
    identify_extract_model(id, s->model);
    memcpy(hdd_identify_cache[0], id, SECTOR_SIZE);
}

/* Scan slots 1..3 for ATA HDDs via polling IDENTIFY.
 *
 * Must run AFTER atapi_scan_all() — that function soft-resets both
 * channels, which is required for ATAPI signature detection but
 * harmless for HDD IDENTIFY (the drive re-reports the same data).
 * Running afterwards also means we can skip slots already claimed by
 * an ATAPI (the LBA_MID/HI check inside ata_identify_slot rejects
 * those). */
static int
hdd_scan_other_slots(void)
{
    const struct { uint8_t ch, dr; uint16_t base, alt; uint8_t irq; } spec[3] = {
        { 0, 1, 0x1F0, 0x3F6, 14 },
        { 1, 0, 0x170, 0x376, 15 },
        { 1, 1, 0x170, 0x376, 15 },
    };
    int found = 0;
    for (int i = 0; i < 3; i++)
    {
        int idx = spec[i].ch * 2 + spec[i].dr;
        hdd_slot_t *s = &hdd_slots[idx];
        s->channel = spec[i].ch;
        s->drive   = spec[i].dr;
        s->base    = spec[i].base;
        s->alt     = spec[i].alt;
        s->irq     = spec[i].irq;

        /* Skip slots already claimed by an ATAPI drive. */
        if (atapi_slots[idx].present) continue;

        uint16_t id[SECTOR_SIZE / 2];
        if (!ata_identify_slot(idx, id)) continue;

        s->present       = true;
        s->lba48         = (id[83] & (1u << 10)) != 0;
        s->total_sectors = identify_extract_total_sectors(id, s->lba48);
        identify_extract_ssd_trim(id, &s->is_ssd, &s->trim_supported);
        identify_extract_model(id, s->model);
        memcpy(hdd_identify_cache[idx], id, SECTOR_SIZE);
        found++;

        char dbg[96];
        snprintf(dbg, sizeof(dbg),
                 "[ata] HDD slot %d: %s (%llu sectors, LBA%s)\n",
                 idx, s->model, (unsigned long long)s->total_sectors,
                 s->lba48 ? "48" : "28");
        debug_print(dbg);
    }

    /* Restore primary master selection so ata_program_lba28/48 keeps
     * working unmodified for slot 0. */
    io_outb(0x1F0 + 6, 0xA0);
    io_inb(0x3F6);
    return found;
}

/* === Partition scan support ===
 *
 * Wraps ata_read_dispatch in a partition_read_sector_fn closure so
 * libdob/dob/partition.c can fetch sector 0 of any disk via our own
 * internal read path (no IPC round-trip back to ourselves). */

static bool
ata_partition_read_sector(void *ctx, uint32_t lba, void *out)
{
    uint32_t disk = (uint32_t)(uintptr_t)ctx;
    if (disk >= ATA_MAX_DISKS || !hdd_slots[disk].present) return false;
    return ata_read_dispatch(disk, lba, 1, out);
}

static bool
ata_partition_announce(uint32_t disk)
{
    if (disk >= ATA_MAX_DISKS || !hdd_slots[disk].present) return false;

    partition_scan_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));
    pctx.read_sector      = ata_partition_read_sector;
    pctx.ctx              = (void *)(uintptr_t)disk;
    pctx.provider_service = "ata";
    pctx.native_selector  = disk;
    return partition_scan_announce(&pctx);
}

static dob_status_t
handle_message(dob_msg_t *msg, dob_msg_t *reply)
{
    if (dob_driver_is_detach(msg))
    {
        dob_driver_released();
        _exit(0);
    }

    switch (msg->code)
    {
        case ATA_OP_READ: {   /* 1 — arg0=lba, arg1=count, arg2=disk (default 0) */
            uint32_t lba   = msg->arg0;
            uint32_t count = msg->arg1;
            uint32_t disk  = msg->arg2;
            if (disk >= ATA_MAX_DISKS || !hdd_slots[disk].present) return DOB_ERR_INVALID;
            if (count == 0 || count > MAX_SECTORS)                 return DOB_ERR_INVALID;
            if (!ata_read_dispatch(disk, lba, (uint16_t)count, req_buf))
                return DOB_ERR_INTERNAL;
            reply->payload      = req_buf;
            reply->payload_size = count * SECTOR_SIZE;
            reply->arg0         = count * SECTOR_SIZE;
            return DOB_OK;
        }
        case ATA_OP_WRITE: {  /* 2 — arg0=lba, arg1=count, arg2=disk */
            uint32_t lba   = msg->arg0;
            uint32_t count = msg->arg1;
            uint32_t disk  = msg->arg2;
            if (disk >= ATA_MAX_DISKS || !hdd_slots[disk].present) return DOB_ERR_INVALID;
            if (count == 0 || count > MAX_SECTORS)                 return DOB_ERR_INVALID;
            if (!msg->payload || msg->payload_size < count * SECTOR_SIZE)
                return DOB_ERR_INVALID;
            if (!ata_write_dispatch(disk, lba, (uint16_t)count, msg->payload))
                return DOB_ERR_INTERNAL;
            return DOB_OK;
        }
        case ATA_OP_IDENTIFY: {   /* 3 — arg2=disk; returns cached IDENTIFY blob */
            uint32_t disk = msg->arg2;
            if (disk >= ATA_MAX_DISKS || !hdd_slots[disk].present) return DOB_ERR_INVALID;
            reply->payload      = hdd_identify_cache[disk];
            reply->payload_size = SECTOR_SIZE;
            return DOB_OK;
        }
        case ATA_OP_LIST_DISKS: {   /* 10 — enumerate the 4 IDE slots */
            static ata_disk_info_t list[ATA_MAX_DISKS];
            memset(list, 0, sizeof(list));
            for (int i = 0; i < ATA_MAX_DISKS; i++)
            {
                list[i].present       = hdd_slots[i].present ? 1 : 0;
                list[i].channel       = hdd_slots[i].channel;
                list[i].drive         = hdd_slots[i].drive;
                list[i].lba48         = hdd_slots[i].lba48 ? 1 : 0;
                list[i].is_ssd        = hdd_slots[i].is_ssd ? 1 : 0;
                list[i].trim_supported= hdd_slots[i].trim_supported ? 1 : 0;
                list[i].total_sectors = hdd_slots[i].total_sectors;
                memcpy(list[i].model, hdd_slots[i].model, sizeof(list[i].model));
            }
            reply->payload      = list;
            reply->payload_size = sizeof(list);
            reply->arg0         = ATA_MAX_DISKS;
            return DOB_OK;
        }
        case ATA_OP_RESCAN_PARTITIONS: {   /* 20 — rescan MBR of one disk */
            uint32_t disk = msg->arg0;
            if (disk >= ATA_MAX_DISKS || !hdd_slots[disk].present) return DOB_ERR_INVALID;
            if (!ata_partition_announce(disk))                     return DOB_ERR_INTERNAL;
            return DOB_OK;
        }
        case ATA_OP_GET_SMART: {   /* 21 — arg2=disk, payload=512 bytes raw SMART data */
            uint32_t disk = msg->arg2;
            if (disk >= ATA_MAX_DISKS || !hdd_slots[disk].present) return DOB_ERR_INVALID;
            /* Reuse req_buf for the reply. The PIO SMART command is
             * short and stays serialised through the main thread, so
             * concurrent disk I/O cannot trample the buffer between
             * the read and the reply. */
            memset(req_buf, 0, SECTOR_SIZE);
            if (!pio_smart_read_data_slot((int)disk, req_buf))
                return DOB_ERR_INTERNAL;
            reply->payload      = req_buf;
            reply->payload_size = SECTOR_SIZE;
            return DOB_OK;
        }
        case ATA_OP_GET_DIAG: {   /* 22 — no args, returns ata_diag_t */
            static ata_diag_t diag;
            memset(&diag, 0, sizeof(diag));
            diag.dma_ok          = dma_ok ? 1 : 0;
            diag.udma_mode       = udma_mode;
            diag.udma_cap        = controller_udma_cap();
            diag.lba48           = lba48_ok ? 1 : 0;
            diag.quirk_tried     = ctrl_quirk_tried;
            diag.quirk_applied   = ctrl_quirk_applied;
            diag.last_dma_fail   = last_dma_fail;
            diag.dma_fail_streak = (uint8_t)dma_fail_streak;
            diag.pci_found       = pci_found ? 1 : 0;
            diag.bm_base         = bm_base;
            if (pci_found)
            {
                uint32_t id = pci_read32(pci_bus, pci_slot, pci_func, 0x00);
                diag.pci_vendor = (uint16_t)(id & 0xFFFF);
                diag.pci_device = (uint16_t)(id >> 16);
                uint32_t cmd = pci_read32(pci_bus, pci_slot, pci_func, 0x04);
                diag.bus_master_on = (cmd & 0x04) ? 1 : 0;
            }
            reply->payload      = &diag;
            reply->payload_size = sizeof(diag);
            return DOB_OK;
        }
        case ATA_PACKET_OPCODE: {   /* 100 ATA_PACKET — ATAPI SCSI */
            if (!msg->payload || msg->payload_size < ATAPI_CDB_LEN)
                return DOB_ERR_INVALID;

            int slot = (int)msg->arg0;
            if (slot < 0 || slot >= ATAPI_MAX_SLOTS) return DOB_ERR_INVALID;
            if (!atapi_slots[slot].present)          return DOB_ERR_INVALID;

            uint32_t alloc = msg->arg1;
            if (alloc > ATAPI_XFER_MAX) alloc = ATAPI_XFER_MAX;

            static uint8_t atapi_buf[ATAPI_XFER_MAX];
            uint32_t got = 0;
            if (!atapi_packet_exec(slot, (const uint8_t *)msg->payload,
                                   atapi_buf, alloc, &got))
                return DOB_ERR_INTERNAL;

            reply->payload      = atapi_buf;
            reply->payload_size = got;
            reply->arg0         = got;
            return DOB_OK;
        }
        case ATA_OP_TRIM: {   /* 23 — arg0=lba, arg1=count, arg2=disk (slot 0) */
            uint32_t lba   = msg->arg0;
            uint32_t count = msg->arg1;
            uint32_t disk  = msg->arg2;
            if (disk >= ATA_MAX_DISKS || !hdd_slots[disk].present)
                return DOB_ERR_INVALID;
            /* Capability + safety gate, mirroring AHCI op-4: the target must
             * be a present, TRIM-capable SSD — never an HDD or a non-TRIM
             * drive, so this can't fire destructively on the wrong media. */
            if (!hdd_slots[disk].is_ssd || !hdd_slots[disk].trim_supported)
                return DOB_ERR_INVALID;
            /* DSM TRIM here is DMA-only, and this driver's DMA engine
             * addresses slot 0 (primary master) exclusively — slots 1..3 run
             * PIO and have no DSM path. Reject them rather than TRIM slot 0
             * by mistake. */
            if (disk != 0 || !dma_ok)
                return DOB_ERR_INVALID;
            if (count == 0)
                return DOB_ERR_INVALID;
            if (!ata_dsm_trim(lba, count))
                return DOB_ERR_INTERNAL;
            return DOB_OK;
        }
        default:
            return DOB_ERR_INVALID;
    }
}

/* === Init === */
int
main(void)
{
    hotplug_device_t dev;

    /* dob_driver_attach is pure IPC (no hardware touch); do it first so a
     * duplicate can release its bubble cleanly. */
    bool via_hotplug = dob_driver_attach(&dev);

    /* Anti-double-run watchdog (mirrors the AHCI driver). The disk driver
     * runs from Startup_modules; this guards against a second instance
     * (e.g. were an ata.das ever added, hotplug would spawn one too).
     * Two instances would fight over the same IDE PIO registers and the
     * shared IRQ. If 'ata' is already registered, a live instance owns
     * the controller -- bail out before touching any hardware (soft-reset
     * / identify / irq_register) and before dob_server_init, so the loser
     * never overwrites the winner's registry entry. */
    if (dob_registry_find("ata") != 0)
    {
        debug_print("[ata] already running; second instance exits.\n");
        if (via_hotplug)
            dob_driver_released();
        _exit(0);
    }

    irq_port = (uint32_t)port_create();

    if (via_hotplug)
    {
        irq_num  = dev.irq_line ? dev.irq_line : 14;
        pci_bus  = dev.bus;
        pci_slot = dev.slot;
        pci_func = dev.func;
        uint32_t bar4 = dev.bar[4];
        if (bar4 & 1) bm_base = (uint16_t)(bar4 & 0xFFFC);
        pci_found = bm_base != 0;
        pci_enable_bus_master(&dev);
        debug_print("[ata] Attached via hotplug\n");
    }
    else
    {
        irq_num = 14;
        if (pci_find_ide())
        {
            pci_enable_master_standalone();
            debug_print("[ata] Standalone: found IDE controller\n");
        }
        else
        {
            debug_print("[ata] Standalone: no PCI IDE, PIO only\n");
        }
    }
    irq_register(irq_num, irq_port);

    ata_soft_reset();

    uint16_t id[SECTOR_SIZE / 2];
    bool primary_master_found = ata_identify(id);
    if (!primary_master_found)
    {
        debug_print("[ata] No drive found, running anyway\n");
    }
    else
    {
        parse_identify(id, &udma_mode);
        if (udma_mode != 0xFF) debug_print("[ata] UDMA capable\n");
        if (lba48_ok)          debug_print("[ata] LBA48 supported\n");

        if (pci_found && udma_mode != 0xFF)
        {
            /* Level A — trust the BIOS. We do NOT touch the controller here:
             * on real hardware the firmware has already set up timing and
             * UDMA at POST, and on QEMU no controller write is needed either.
             * Just tell the drive to use its best UDMA mode and try DMA. If
             * DMA later fails at runtime, dma_xfer's recovery path invokes
             * Level B (controller_enable_udma) before giving up on DMA. */
            if (!ata_set_udma(udma_mode))
            {
                debug_print("[ata] SET FEATURES UDMA failed, keep PIO\n");
            }
            else
            {
                dma_buf = (uint8_t *)dma_alloc(DMA_BUF_SIZE, &dma_buf_phys);
                prdt    = (prd_t *)dma_alloc(sizeof(prd_t) * MAX_PRDT, &prdt_phys);
                if (dma_buf && prdt && dma_buf_phys && prdt_phys)
                {
                    dma_ok = true;
                    debug_print("[ata] DMA path ready\n");
                }
                else
                {
                    debug_print("[ata] dma_alloc failed, fallback to PIO\n");
                }
            }
        }

        /* Multi-disk step 1: register primary master in hdd_slots[0]
         * with the IDENTIFY blob we just read. */
        hdd_register_primary_master(id);
    }

    dob_server_init("ata");
    dob_server_register(handle_message);
    debug_print(dma_ok ? "[ata] Ready (DMA)\n" : "[ata] Ready (PIO)\n");

    /* ATAPI scan: tell hotplug about every CDROM we find. The bubbles
     * we create here are static — no media-change IRQ on IDE, so the
     * drive icon is permanent and media presence is checked on demand
     * by the cdrom driver. */
    int atapi_count = atapi_scan_all();
    {
        char summary[64];
        snprintf(summary, sizeof(summary),
                 "[ata] ATAPI scan found %d drive(s)\n", atapi_count);
        debug_print(summary);
    }

    /* Multi-disk step 1: probe the remaining 3 IDE slots for ATA HDDs.
     * Polling-based IDENTIFY, doesn't disturb DMA setup of slot 0.
     * Runs AFTER atapi_scan_all so the channel resets needed for the
     * ATAPI signature are out of the way. */
    int extra_hdds = hdd_scan_other_slots();
    {
        char summary[64];
        snprintf(summary, sizeof(summary),
                 "[ata] HDD scan: %d extra disk(s) on slots 1..3\n",
                 extra_hdds);
        debug_print(summary);
    }
    if (atapi_count > 0)
    {
        /* Any ATAPI slot on the secondary channel means we need an
         * IRQ15 listener — without it, atapi_wait_irq on secondary
         * would block forever. We open a dedicated port so primary
         * and secondary IRQs can never be confused. */
        bool need_irq15 = false;
        for (int i = 0; i < ATAPI_MAX_SLOTS; i++)
            if (atapi_slots[i].present && atapi_slots[i].channel == 1)
            { need_irq15 = true; break; }

        if (need_irq15)
        {
            atapi_irq15_port = (uint32_t)port_create();
            if (atapi_irq15_port)
                irq_register(15, atapi_irq15_port);
        }

        /* Start the background pusher. It will wait for hotplug to
         * appear in the registry (event-driven, via dob_registry_wait
         * which is a kernel-side block), then push one CREATE_LEGACY
         * per slot found. The main thread keeps serving disk I/O
         * for DobFileSystem meanwhile. */
        dob_thread_spawn(atapi_pusher_thread, NULL);
    }

    /* Same model for HDD partitions: a background thread waits for
     * hotplug, then calls partition_scan_announce on every present
     * HDD slot. Independent of the ATAPI count — even with no ATAPI
     * we still need to surface partitions for any HDD we found. */
    bool have_hdd = false;
    for (uint32_t i = 0; i < ATA_MAX_DISKS; i++)
        if (hdd_slots[i].present) { have_hdd = true; break; }
    if (have_hdd)
        dob_thread_spawn(hdd_partition_pusher_thread, NULL);

    /* Boot-disk role arbitration (mirrors the AHCI driver). Only the
     * Startup_modules instance (via_hotplug == false) may own the system
     * disk; a hotplug-spawned ata exists for a second IDE controller's
     * peripherals and leaves the base disk to whoever claimed it. First
     * eligible disk driver registers the 'bootdisk' marker; later ones
     * see it taken and don't contest the system-disk role (they still
     * serve their own clients). DobFileSystem ignores this marker. */
    if (have_hdd && !via_hotplug && dob_registry_find("bootdisk") == 0)
    {
        dob_registry_register("bootdisk", dob_server_get_port());
        debug_print("[ata] claimed boot-disk role.\n");
    }

    dob_server_loop();
    return 0;
}
