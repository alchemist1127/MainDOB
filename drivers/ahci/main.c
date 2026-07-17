/* MainDOB AHCI / SATA III Driver
 *
 * Full SATA III controller driver. Enumerates all ports, detects device
 * type (HDD, SSD, optical), supports DMA read/write, TRIM for SSDs,
 * and ATAPI PACKET commands for optical drives.
 *
 * This is a Dob driver module: hotplug spawns it, gives it the PCI
 * device info (BAR5 = ABAR), and it runs as an isolated bubble.
 *
 * Protocol — see ahci_protocol.h for the full wire description.
 * Quick reference:
 *   code=1  READ              arg0=port arg1=lba arg2=count -> payload
 *   code=2  WRITE             arg0=port arg1=lba arg2=count payload=data
 *   code=3  IDENTIFY          arg0=port -> payload=512 bytes
 *   code=4  TRIM              arg0=port arg1=lba arg2=count (SSD only)
 *   code=5  ATAPI_PACKET      arg0=port payload=12-byte CDB
 *   code=10 LIST_PORTS        -> payload=sata_port_info_t[]
 *   code=11 EJECT             arg0=port (optical only)
 *   code=20 RESCAN_PARTITIONS arg0=port (stub until libdob/dob/partition lands)
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
#include <dob/ahci_protocol.h>
#include <dob/partition.h>

/* HBA register offsets */
#define HBA_GHC     0x04
#define HBA_IS      0x08
#define HBA_PI      0x0C
#define HBA_VS      0x10
#define HBA_CAP     0x00              /* Host capabilities */
#define HBA_CAP_SSNTF (1u << 29)      /* Supports SNotification Register */

/* Port register offsets (base = 0x100 + port * 0x80) */
#define PORT_CLB    0x00
#define PORT_CLBU   0x04
#define PORT_FB     0x08
#define PORT_FBU    0x0C
#define PORT_IS     0x10
#define PORT_IE     0x14
#define PORT_IE_DHRS (1u << 0)   /* D2H Register FIS interrupt */
/* PIO Setup FIS interrupt. REQUIRED for PIO data-in commands (SMART READ
 * DATA, runtime IDENTIFY): per AHCI spec their ending status arrives in
 * the PIO Setup FIS — with the I bit — and NOT in a trailing D2H Register
 * FIS. With only DHRS enabled, a PIO-in command completes silently: PxCI
 * clears, PxTFD shows a happy 0x50, but no interrupt fires and the waiter
 * times out. DMA commands (D2H) and non-data commands (D2H) were immune,
 * and boot-time IDENTIFY was saved by its polling path — which is exactly
 * the observed signature on the Extensa 5220 (SMART ENABLE ok, SMART READ
 * DATA timeout, ST=0x50). QEMU raises interrupts more liberally and
 * masked this for the whole development history. */
#define PORT_IE_PSS  (1u << 1)   /* PIO Setup FIS interrupt */
#define PORT_IE_SDBS (1u << 3)   /* Set Device Bits FIS (async notification) */
/* PhyRdy Change: the controller raises PxIS.PRCS (bit 22) when the port's
 * PhyRdy signal changes — i.e. a device is hot-plugged in or pulled out.
 * Enabling PxIE.PRCS is how disk hotplug is detected. The change latches
 * in PxSERR.DIAG.N (bit 16) and must be cleared there (writing PxIS alone
 * does not clear it), or the interrupt re-asserts forever. */
#define PORT_IE_PRCS (1u << 22)  /* PhyRdy Change interrupt enable */
#define PORT_IS_PRCS (1u << 22)  /* PhyRdy Change status */
#define PORT_SERR_DIAG_N (1u << 16) /* PhyRdy Change (SERR.DIAG.N) */
#define PORT_CMD    0x18
#define PORT_TFD    0x20
#define PORT_SIG    0x24
#define PORT_SSTS   0x28
#define PORT_SERR   0x30
#define PORT_CI     0x38
#define PORT_SNTF   0x3C    /* SATA Notification (async notify pending bits) */

/* === ATAPI media-change (GESN) === */
#define ATAPI_CMD_GET_EVENT_STATUS  0x4A   /* GET EVENT STATUS NOTIFICATION */
#define GESN_CLASS_MEDIA            4      /* media event class bit / code */
#define GESN_MEDIA_NEW             0x02   /* NewMedia event */
#define GESN_MEDIA_REMOVAL         0x03   /* MediaRemoval event */

/* === ATAPI media-presence fallback (real hardware) === */
#define ATAPI_CMD_TEST_UNIT_READY   0x00   /* universal "is media ready?" */
#define ATAPI_CMD_REQUEST_SENSE     0x03   /* fetch sense data after a CHECK */
#define SENSE_NOT_READY             0x02   /* sense key: not ready */
#define SENSE_UNIT_ATTENTION        0x06   /* sense key: media changed, retry */
#define ASC_BECOMING_READY          0x04   /* media spinning up */
#define ASC_MEDIUM_NOT_PRESENT      0x3A   /* no disc */

#define PORT_CMD_ST   (1 << 0)
#define PORT_CMD_SUD  (1 << 1)    /* Spin-Up Device */
#define PORT_CMD_POD  (1 << 2)    /* Power On Device */
#define PORT_CMD_FRE  (1 << 4)
#define PORT_CMD_FR   (1 << 14)
#define PORT_CMD_CR   (1 << 15)
#define PORT_CMD_ICC_ACTIVE (1u << 28)  /* Interface Communication Control = Active */
#define PORT_TFD_BSY  (1 << 7)
#define PORT_TFD_ERR  (1 << 0)

#define FIS_TYPE_REG_H2D 0x27

/* ATA commands */
#define ATA_CMD_READ_DMA_EX   0x25
#define ATA_CMD_WRITE_DMA_EX  0x35
#define ATA_CMD_IDENTIFY       0xEC
#define ATA_CMD_IDENTIFY_ATAPI 0xA1
#define ATA_CMD_PACKET         0xA0
#define ATA_CMD_DATA_SET_MGMT  0x06  /* TRIM */
#define ATA_CMD_SMART          0xB0
#define ATA_SMART_READ_DATA    0xD0  /* SMART sub-command -> Features reg */
#define ATA_SMART_ENABLE       0xD8  /* SMART ENABLE OPERATIONS (non-data) */
/* SMART command-block magic (ATA spec): LBA mid = 0x4F, LBA hi = 0xC2.
 * Packed into the lba argument of issue_cmd as bits [15:8] = mid and
 * [23:16] = hi (lba0 stays 0); issue_cmd splits lba into the FIS bytes. */
#define ATA_SMART_MAGIC_LBA    (((uint64_t)0xC2u << 16) | ((uint64_t)0x4Fu << 8))

/* Port signatures */
#define SIG_ATA    0x00000101   /* SATA disk (HDD or SSD) */
#define SIG_ATAPI  0xEB140101   /* SATAPI (CD/DVD/BD) */

/* Device-type constants now live in ahci_protocol.h (AHCI_DEV_*).
 * Keep the short local aliases so the existing in-file logic reads
 * the same — every assignment / comparison in main.c uses these. */
#define DEV_NONE    AHCI_DEV_NONE
/* Device-icon subdevice token: (0x7D << 24) | port. The HIGH BYTE keeps
 * it disjoint from the partition tokens (partition_index 0..3 << 24 |
 * selector: a disk's partition #0 token EQUALS the bare port number!)
 * and from the optical token (bare port). 0x7D keeps the decimal form
 * under INT32_MAX for the DAS $token -> atoi round-trip; DobDisk masks
 * the low 24 bits back to the port (--select sata_<token>). */
#define AHCI_DISK_ICON_TOKEN(p)  ((0x7Du << 24) | (uint32_t)(p))

#define DEV_HDD     AHCI_DEV_HDD
#define DEV_SSD     AHCI_DEV_SSD
#define DEV_OPTICAL AHCI_DEV_OPTICAL

#define SECTOR_SIZE      512
#define MAX_PORTS        AHCI_MAX_PORTS
#define MAX_PRDT_ENTRIES 8
#define DMA_BUF_SIZE     (128 * SECTOR_SIZE)

/* FIS Register H2D */
typedef struct
{
    uint8_t  fis_type;
    uint8_t  flags;
    uint8_t  command;
    uint8_t  feature_lo;
    uint8_t  lba0, lba1, lba2;
    uint8_t  device;
    uint8_t  lba3, lba4, lba5;
    uint8_t  feature_hi;
    uint16_t count;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  reserved[4];
} __attribute__((packed)) fis_h2d_t;

typedef struct
{
    uint16_t flags;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} __attribute__((packed)) cmd_header_t;

typedef struct
{
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;
} __attribute__((packed)) prdt_entry_t;

typedef struct
{
    uint8_t      cfis[64];
    uint8_t      acmd[16];
    uint8_t      reserved[48];
    prdt_entry_t prdt[MAX_PRDT_ENTRIES];
} __attribute__((packed, aligned(128))) cmd_table_t;

/* Per-port state */
typedef struct
{
    uint8_t       type;             /* DEV_NONE / DEV_HDD / DEV_SSD / DEV_OPTICAL */
    bool          present;
    uint32_t      sig;
    uint64_t      sector_count;     /* Total sectors (from IDENTIFY) */
    char          model[41];        /* Model string (from IDENTIFY) */
    char          serial[21];       /* Serial number */
    bool          trim_supported;
    bool          gesn_supported;    /* optical: GESN usable (else TUR fallback) */

    cmd_header_t *cmd_list;
    uint32_t      cmd_list_phys;
    uint8_t      *fis_area;
    uint32_t      fis_area_phys;
    cmd_table_t  *cmd_table;
    uint32_t      cmd_table_phys;
    uint8_t      *dma_buf;
    uint32_t      dma_buf_phys;

    /* Per-port command serialization. issue_cmd uses command slot 0 and
     * a single cmd_table/dma_buf per port, so two concurrent commands on
     * the SAME port would clobber each other's FIS/PRDT and race on PxCI.
     * cmd_busy is a one-deep lock taken across issue_cmd under
     * enter_critical; a second request to a busy port is rejected with
     * a retryable error rather than corrupting the in-flight one.
     * Different ports remain fully concurrent (state is per-port). */
    volatile bool cmd_busy;
} port_state_t;

/* Public info for LIST_PORTS query — now defined in ahci_protocol.h. */

/* === Globals === */

static volatile uint8_t *hba_base = NULL;
static port_state_t ports[MAX_PORTS];
static uint32_t port_count = 0;
static uint32_t irq_port = 0;
static uint8_t  irq_num  = 0;

/* === Star dispatcher state (Tappa A) ===
 * The AHCI controller has a single shared IRQ. A dedicated dispatcher
 * thread is the ONLY reader of irq_port; it demuxes per port via HBA_IS
 * and forwards a completion message to the waiter on that port's
 * cmd_done_port. ahci_wait_done() waits on its port's completion port
 * (plus a timer), never on irq_port -- so there is never more than one
 * reader of irq_port. cmd_inflight[p] tells the dispatcher a command is
 * outstanding on port p (set/cleared under enter_critical). */
static uint32_t cmd_done_port[MAX_PORTS];
static volatile bool cmd_inflight[MAX_PORTS];

/* === Command-failure diagnostics (per port) ===
 * Filled by issue_cmd_feat at the moment a command fails, so the LAST
 * failure's hardware state is available to user-facing paths (SMART in
 * DobDisk) on machines with no serial/boot log — the popup becomes the
 * diagnostic table. [0]=reason, [1]=PxTFD, [2]=PxSERR, [3]=step (set by
 * the caller: which stage of a multi-command sequence failed). */
#define CMDDIAG_OK       0
#define CMDDIAG_BUSY     1   /* slot-0 one-deep lock contended */
#define CMDDIAG_TIMEOUT  2   /* no completion within AHCI_CMD_TIMEOUT_MS */
#define CMDDIAG_TFD_ERR  3   /* drive aborted: PxTFD.STS.ERR set */
static uint32_t cmd_diag[MAX_PORTS][4];

/* === Media-change state (Tappa B/C) ===
 * media_present[p]: last-known media state for an optical port, so we
 * only emit an event on an actual transition. media_event_port: the
 * dispatcher posts here (non-blocking) when an async notification fires
 * on an optical port; the dedicated media handler thread wakes, issues
 * GESN (a blocking ATAPI command — must NOT run inside the dispatcher,
 * which has to stay free to deliver that very command's completion), and
 * in Tappa C emits SUBDEVICE_APPEARED/GONE. media_irq_pending carries the
 * port number(s) flagged by the dispatcher. */
static volatile bool media_present[MAX_PORTS];
static uint32_t media_event_port = 0;

/* === Disk hotplug state ===
 * port_hotplug_port: the dispatcher posts here (non-blocking) when a
 * PhyRdy Change (PxIS.PRCS) fires on a port — a disk was inserted or
 * removed. A dedicated hotplug thread wakes, re-reads SSTS, and does the
 * blocking bring-up (COMRESET, port_setup, IDENTIFY) or teardown, then
 * announces/retracts the subdevice and partitions. This work must NOT run
 * in the dispatcher, which has to stay free to deliver command
 * completions (the IDENTIFY issued during bring-up completes through it).
 * hotplug_enabled records whether the controller supports staying
 * resident for hotplug (CAP.SPM/SXS or simply: we always arm it on q35/
 * ICH8-class HBAs, which support PhyRdy-change reporting). */
static uint32_t port_hotplug_port = 0;

/* The service name THIS instance won ("ahci", or "ahci_N" for a secondary
 * controller). Every announce (partitions, opticals, subdevices) must carry
 * this — never the literal "ahci" — or the icons/mounts of a secondary
 * instance would resolve to a DIFFERENT process's port. */
static char g_service_name[16] = "ahci";

/* Boot/insert bring-up retry budget, per port. A port whose link is UP but
 * whose port_setup failed (DET settled late, IDENTIFY flaked right out of
 * COMRESET, IRQ delivery not yet claimed under empirical GSI resolution)
 * would otherwise be lost FOREVER: PxIS.PRCS only fires on a phy CHANGE,
 * and the link is already up — the classic "disk sometimes doesn't appear"
 * boot race. The hotplug thread retries with backoff instead; the counter
 * caps it so a genuinely broken port can't loop. Reset on success/removal. */
#define AHCI_BRINGUP_RETRY_MAX 5
static uint8_t bringup_retries[MAX_PORTS];

#define AHCI_CMD_TIMEOUT_MS    5000
#define AHCI_ATAPI_TIMEOUT_MS 10000
#define AHCI_STOP_TIMEOUT_MS   500
#define AHCI_BSY_TIMEOUT_MS   2000

/* A disk freshly out of COMRESET can fail (or zero-answer) its very
 * first IDENTIFY on real silicon while QEMU answers instantly. Retry a
 * few times with a short backoff before declaring the port unusable. */
#define AHCI_IDENTIFY_RETRIES    4
#define AHCI_IDENTIFY_RETRY_MS  150

/* === Hardware tools === */

static volatile uint32_t *
port_regs(int p)
{
    return (volatile uint32_t *)(hba_base + 0x100 + p * 0x80);
}

/* === Star dispatcher (Tappa A) ===
 *
 * Single reader of irq_port. The kernel delivers each hardware IRQ as a
 * notification (msg.type == 3) on irq_port. On each IRQ we read HBA_IS
 * to find which port(s) asserted, clear PxIS + HBA_IS, ack with
 * irq_done(), and -- for any port that has a command in flight -- post a
 * completion message to that port's cmd_done_port so its waiter wakes.
 *
 * This thread NEVER blocks on anything but irq_port, and nobody else
 * reads irq_port, so the "two readers race" cannot occur. Per-port state
 * cmd_inflight[p] e' un latch one-shot: l'issuer lo arma (PxCI=1) sotto
 * enter_critical, il dispatcher lo consuma sotto enter_critical quando
 * posta il wakeup. Cosi' un completamento sveglia esattamente un waiter
 * e un secondo IRQ back-to-back sulla stessa porta non ri-posta un
 * wakeup stantio (che diventerebbe un timeout sul comando successivo).
 * Il waiter valida comunque PxCI/PxTFD a ogni risveglio.
 */
static void
ahci_dispatcher_thread(void *arg)
{
    (void)arg;
    volatile uint32_t *hba = (volatile uint32_t *)hba_base;

    for (;;)
    {
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        if (dob_ipc_receive(irq_port, &msg) != DOB_OK)
            continue;

        if (msg.type != 3)   /* only hardware IRQ notifications here */
            continue;

        uint32_t is = hba[HBA_IS / 4];

        /* Empirical GSI resolution (IOAPIC mode): while our GSI is unknown
         * (irq_register_pci returned 0), the kernel notifies us for one
         * candidate GSI at a time, the GSI carried in msg.arg0. HBA_IS != 0
         * means this controller is the source, so claim that GSI; ignore
         * notifications that are not ours — we are not yet a sharer and owe
         * no irq_done. */
        if (irq_num == 0)
        {
            if (is == 0)
                continue;            /* not our controller — stay pending */
            uint32_t bits = msg.arg0;
            uint8_t  gsi  = 0;
            while (bits && !(bits & 1u)) { bits >>= 1; gsi++; }
            irq_num = gsi;
            irq_pci_claim(gsi);      /* bind; we are now a sharer */
            /* fall through to service this interrupt */
        }

        for (int p = 0; p < MAX_PORTS; p++)
        {
            if (!(is & (1u << p)))
                continue;

            volatile uint32_t *pr = port_regs(p);

            /* Snapshot PxIS before clearing, to detect the async
             * notification (Set Device Bits FIS, bit 3) and the PhyRdy
             * Change (bit 22, disk hotplug). */
            uint32_t pis = pr[PORT_IS / 4];
            uint32_t sntf = pr[PORT_SNTF / 4];

            /* Clear the port's interrupt status, the SNTF pending bit,
             * then the HBA-level bit. The PhyRdy-change latch in
             * PxSERR.DIAG.N is cleared separately below (writing PxIS does
             * not clear it). */
            pr[PORT_IS / 4]   = pis;
            if (sntf)
                pr[PORT_SNTF / 4] = sntf;
            hba[HBA_IS / 4]   = (1u << p);

            /* PhyRdy Change => a device was inserted or removed on this
             * port. Clear the SERR latch (else the IRQ re-asserts), then
             * hand the port number to the hotplug thread for the blocking
             * bring-up/teardown. Don't touch ports[] here. */
            if ((pis & PORT_IS_PRCS) && port_hotplug_port)
            {
                pr[PORT_SERR / 4] = PORT_SERR_DIAG_N;  /* clear PhyRdy latch */
                dob_msg_t hp;
                memset(&hp, 0, sizeof(hp));
                hp.code = 3;            /* port-change event */
                hp.arg0 = (uint32_t)p;  /* which port */
                dob_ipc_post(port_hotplug_port, &hp);
            }

            /* STORM DIAGNOSTIC: if this IRQ came with no command in
             * flight and CI=0, it's a spurious/persistent D2H FIS. After
             * clearing PxIS above, re-read it: if the bit is instantly
             * back, the source isn't consumed by clearing PxIS, and we
             * need to find what re-asserts it. Throttled to 1-in-200. */
            if (cmd_inflight[p] == 0 && !(pr[PORT_CI / 4] & 1))
            {
                static uint32_t storm_n = 0;
                if ((storm_n++ % 200) == 0)
                {
                    uint32_t pis2 = pr[PORT_IS / 4];
                    char d[160];
                    snprintf(d, sizeof(d),
                             "[sata-storm] p%d PxIS(after)=0x%08x CMD=0x%08x TFD=0x%08x SERR=0x%08x SNTF=0x%08x (n=%u)\n",
                             p, pis2, pr[PORT_CMD / 4], pr[PORT_TFD / 4],
                             pr[PORT_SERR / 4], pr[PORT_SNTF / 4], storm_n);
                    debug_print(d);
                }
            }

            /* Wake the waiter for this port. cmd_inflight[p] e' un LATCH
             * one-shot posseduto dall'issuer dal PxCI=1 fino a quando il
             * dispatcher lo consuma QUI: azzerarlo in questo punto (sotto
             * enter_critical, la stessa regione con cui l'issuer lo arma)
             * garantisce che UN completamento generi UN solo wakeup. Se un
             * secondo IRQ arriva sulla stessa porta prima che il waiter
             * giri (comandi back-to-back, cache hit), trova il latch gia'
             * consumato e non ri-posta un wakeup stantio su un done_port di
             * un comando concluso — la sorgente dei timeout intermittenti
             * sul comando SUCCESSIVO. Il waiter valida comunque PxCI. */
            enter_critical();
            bool wake = cmd_inflight[p] && cmd_done_port[p];
            if (wake)
                cmd_inflight[p] = false;
            exit_critical();
            if (wake)
            {
                dob_msg_t done;
                memset(&done, 0, sizeof(done));
                done.code = 1;   /* completion signal */
                dob_ipc_post(cmd_done_port[p], &done);
            }

            /* Async notification (Set Device Bits FIS) on an optical
             * port => possible media change. Don't issue GESN here (it
             * blocks and its completion must come back through THIS
             * dispatcher). Forward the port number to the media handler
             * thread, which does the blocking work. */
            if ((pis & PORT_IE_SDBS) && ports[p].type == DEV_OPTICAL &&
                media_event_port)
            {
                dob_msg_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.code = 2;            /* media event */
                ev.arg0 = (uint32_t)p;  /* which port */
                dob_ipc_post(media_event_port, &ev);
            }
        }

        irq_done(irq_num);
    }
}

/* Wait for AHCI command completion on port p.
 *
 * Waits on this port's completion port (fed by the dispatcher) plus a
 * timeout timer on the same port. Never reads irq_port. Mirrors the old
 * ahci_wait_irq logic (check PxCI / task-file error), only the wakeup
 * source changed: dispatcher message instead of a raw IRQ. */
static bool
ahci_wait_done(int p, uint32_t timeout_ms)
{
    volatile uint32_t *pr = port_regs(p);
    uint32_t done_port = cmd_done_port[p];

    /* Init-time path: before the dispatcher exists and before this
     * port's completion port is created (cmd_done_port[p] == 0), there
     * is no one to deliver a wakeup. This happens for the IDENTIFY issued
     * during port_setup. Fall back to bounded direct polling of PxCI --
     * exactly what a pre-dispatcher driver would do. Short and only at
     * init, so no runtime CPU cost. */
    if (done_port == 0)
    {
        for (uint32_t waited = 0; waited < timeout_ms; waited++)
        {
            if (!(pr[PORT_CI / 4] & 1))
                return !(pr[PORT_TFD / 4] & PORT_TFD_ERR);
            if (pr[PORT_TFD / 4] & PORT_TFD_ERR)
                return false;
            sleep_ms(1);
        }
        debug_print("[sata] Command timeout (init)!\n");
        return false;
    }

    /* Drain any stale completion messages from a previous command. */
    {
        dob_msg_t drain;
        memset(&drain, 0, sizeof(drain));
        while (dob_ipc_receive_nowait(done_port, &drain) == DOB_OK)
            memset(&drain, 0, sizeof(drain));
    }

    /* Fast path: already done. */
    if (!(pr[PORT_CI / 4] & 1))
        return !(pr[PORT_TFD / 4] & PORT_TFD_ERR);

    int tid = timer_set(done_port, timeout_ms, 0);

    for (;;)
    {
        if (!(pr[PORT_CI / 4] & 1))
        {
            timer_cancel_async(tid);
            return !(pr[PORT_TFD / 4] & PORT_TFD_ERR);
        }

        /* Task file error (PxIS TFES bit 30) — dispatcher cleared PxIS,
         * but the BSY/ERR in PxTFD persists, so check that. */
        if (pr[PORT_TFD / 4] & PORT_TFD_ERR)
        {
            timer_cancel_async(tid);
            return false;
        }

        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        if (dob_ipc_receive(done_port, &msg) != DOB_OK)
        {
            timer_cancel_async(tid);
            return false;
        }

        if (msg.code == 70)  /* Timer expired */
        {
            /* Final PxCI re-check before declaring timeout: if the wakeup
             * was lost (an interrupt source not enabled in PxIE, a race
             * with the latch) but the HBA did finish, take the late
             * success — slow beats broken. This is the safety net that
             * would have degraded the PSS bug (see PORT_IE_PSS) from
             * "SMART permanently broken" to "SMART slow". */
            if (!(pr[PORT_CI / 4] & 1))
                return !(pr[PORT_TFD / 4] & PORT_TFD_ERR);
            debug_print("[sata] Command timeout!\n");
            return false;
        }

        /* Otherwise it's a completion message from the dispatcher: loop
         * back and re-check PxCI (handles spurious / coalesced wakeups). */
    }
}


/* Wait for a port register condition using timer-based sleep.
 * Used only during init (port_stop/port_start) where no IRQ is available.
 * Thread truly sleeps between checks — not a busy-loop. */
static bool
ahci_wait_reg(volatile uint32_t *reg, uint32_t mask, uint32_t expected,
              uint32_t timeout_ms, uint32_t interval_ms)
{
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms)
    {
        if ((*reg & mask) == expected)
            return true;

        int tid = timer_set(irq_port, interval_ms, 0);
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        dob_ipc_receive(irq_port, &msg);
        if (msg.type == 3) irq_done(irq_num);  /* Discard stale IRQ */
        (void)tid;
        elapsed += interval_ms;
    }
    return (*reg & mask) == expected;
}

static void
port_stop(int p)
{
    volatile uint32_t *pr = port_regs(p);
    pr[PORT_CMD / 4] &= ~PORT_CMD_ST;
    pr[PORT_CMD / 4] &= ~PORT_CMD_FRE;

    ahci_wait_reg(&pr[PORT_CMD / 4],
                  PORT_CMD_CR | PORT_CMD_FR, 0,
                  AHCI_STOP_TIMEOUT_MS, 50);
}

static void
port_start(int p)
{
    volatile uint32_t *pr = port_regs(p);

    if (pr[PORT_TFD / 4] & PORT_TFD_BSY)
        ahci_wait_reg(&pr[PORT_TFD / 4], PORT_TFD_BSY, 0,
                      AHCI_BSY_TIMEOUT_MS, 50);

    pr[PORT_IE / 4]  = PORT_IE_DHRS     /* D2H Register FIS interrupt */
                     | PORT_IE_PSS;     /* PIO Setup FIS (PIO data-in!) */
    pr[PORT_CMD / 4] |= PORT_CMD_FRE;
    pr[PORT_CMD / 4] |= PORT_CMD_ST;
}

static bool
port_alloc_dma(port_state_t *ps)
{
    /* Idempotent: allocate each per-port DMA region only once. There is no
     * dma_free syscall, so on a hotplug re-insertion we reuse the buffers
     * allocated the first time (teardown preserves the pointers). This
     * also bounds total DMA to one set per physical port. */
    if (!ps->cmd_list)
        ps->cmd_list = (cmd_header_t *)dma_alloc(32 * sizeof(cmd_header_t), &ps->cmd_list_phys);
    if (!ps->fis_area)
        ps->fis_area = (uint8_t *)dma_alloc(256, &ps->fis_area_phys);
    if (!ps->cmd_table)
        ps->cmd_table = (cmd_table_t *)dma_alloc(sizeof(cmd_table_t), &ps->cmd_table_phys);
    if (!ps->dma_buf)
        ps->dma_buf = (uint8_t *)dma_alloc(DMA_BUF_SIZE, &ps->dma_buf_phys);

    if (!ps->cmd_list || !ps->fis_area || !ps->cmd_table || !ps->dma_buf)
        return false;

    memset(ps->cmd_list, 0, 32 * sizeof(cmd_header_t));
    memset(ps->fis_area, 0, 256);
    return true;
}

static bool
issue_cmd_feat(port_state_t *ps, int p, uint8_t command, uint64_t lba,
               uint16_t count, bool is_write, uint32_t byte_count,
               uint8_t smart_feature)
{
    volatile uint32_t *pr = port_regs(p);

    /* Serialize against another command already in flight on THIS port.
     * Take the one-deep lock atomically; bail (retryable) if busy so we
     * never overwrite an outstanding command's slot-0 FIS/PRDT. Different
     * ports are unaffected. */
    enter_critical();
    if (ps->cmd_busy)
    {
        exit_critical();
        cmd_diag[p][0] = CMDDIAG_BUSY;
        cmd_diag[p][1] = pr[PORT_TFD / 4];
        cmd_diag[p][2] = pr[PORT_SERR / 4];
        return false;
    }
    ps->cmd_busy = true;
    exit_critical();

    pr[PORT_IS / 4] = 0xFFFFFFFF;

    memset(&ps->cmd_list[0], 0, sizeof(cmd_header_t));
    ps->cmd_list[0].flags = sizeof(fis_h2d_t) / 4;
    if (is_write) ps->cmd_list[0].flags |= (1 << 6);
    if (command == ATA_CMD_PACKET) ps->cmd_list[0].flags |= (1 << 5); /* ATAPI */
    ps->cmd_list[0].prdtl = 1;
    ps->cmd_list[0].ctba = ps->cmd_table_phys;
    ps->cmd_list[0].ctbau = 0;

    memset(ps->cmd_table, 0, sizeof(cmd_table_t));

    fis_h2d_t *fis = (fis_h2d_t *)ps->cmd_table->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->flags = 0x80;
    fis->command = command;
    /* Device register: bit 6 (LBA mode) belongs to LBA-addressed commands.
     * SMART is a non-LBA taskfile — LBA mid/hi carry the 0x4F/0xC2 magic,
     * not an address — and libata issues it with Device = 0. Some
     * real-hardware drives are strict here and abort SMART with bit 6 set
     * (QEMU's model accepts anything, hiding the difference). Keep bit 6
     * for everything else: IDENTIFY & co. already work with it on every
     * machine in the fleet, so the change is deliberately SMART-only. */
    fis->device = (command == ATA_CMD_SMART) ? 0 : (1 << 6);

    fis->lba0 = (uint8_t)(lba);
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    fis->count = count;

    if (command == ATA_CMD_DATA_SET_MGMT)
        fis->feature_lo = 0x01; /* TRIM feature */
    if (command == ATA_CMD_SMART)
        fis->feature_lo = smart_feature; /* caller-selected SMART sub-cmd */

    /* Non-data commands (byte_count == 0, e.g. SMART ENABLE OPERATIONS)
     * carry no PRDT: prdtl stays as set above only for data transfers.
     * (dbc is "byte count - 1", so 0 would otherwise wrap to 4GB.) */
    if (byte_count > 0)
    {
        ps->cmd_table->prdt[0].dba = ps->dma_buf_phys;
        ps->cmd_table->prdt[0].dbau = 0;
        ps->cmd_table->prdt[0].dbc = (byte_count - 1) | (1u << 31);
    }
    else
    {
        ps->cmd_list[0].prdtl = 0;
    }

    /* Mark a command in flight and issue it atomically wrt the
     * dispatcher (short critical region: flag + PxCI write only). */
    enter_critical();
    cmd_inflight[p] = true;
    pr[PORT_CI / 4] = 1;
    exit_critical();

    bool ok = ahci_wait_done(p, AHCI_CMD_TIMEOUT_MS);

    if (ok)
    {
        cmd_diag[p][0] = CMDDIAG_OK;
    }
    else
    {
        /* Sample the failure state NOW, before anything clears it: PxTFD
         * persists after a task-file error (STS.ERR + the ATA Error
         * register in bits 15:8); PxSERR persists until explicitly
         * cleared. Distinguish "drive aborted" from "no answer". */
        uint32_t tfd = pr[PORT_TFD / 4];
        cmd_diag[p][0] = (tfd & PORT_TFD_ERR) ? CMDDIAG_TFD_ERR
                                              : CMDDIAG_TIMEOUT;
        cmd_diag[p][1] = tfd;
        cmd_diag[p][2] = pr[PORT_SERR / 4];
    }

    /* Il dispatcher consuma cmd_inflight[p] quando posta il wakeup. Se
     * ahci_wait_done e' uscito senza quel wakeup (fast-path gia' done,
     * errore PxTFD, o timeout) il latch e' ancora armato: azzeralo qui,
     * sotto la stessa regione critica dell'arming, cosi' un IRQ tardivo
     * non trova un latch orfano. */
    enter_critical();
    cmd_inflight[p] = false;
    exit_critical();
    ps->cmd_busy = false;
    return ok;
}

/* Compatibility wrapper: every pre-existing call site. For ATA_CMD_SMART
 * it selects READ DATA; port_smart_read_data uses issue_cmd_feat directly
 * when it needs a different sub-command (ENABLE OPERATIONS). */
static bool
issue_cmd(port_state_t *ps, int p, uint8_t command, uint64_t lba,
          uint16_t count, bool is_write, uint32_t byte_count)
{
    return issue_cmd_feat(ps, p, command, lba, count, is_write, byte_count,
                          ATA_SMART_READ_DATA);
}

static bool
issue_atapi(port_state_t *ps, int p, const uint8_t *cdb, uint32_t xfer_len)
{
    volatile uint32_t *pr = port_regs(p);

    /* Same one-deep per-port serialization as issue_cmd: an ATAPI packet
     * and a data command share slot 0 / cmd_table / dma_buf on this port. */
    enter_critical();
    if (ps->cmd_busy)
    {
        exit_critical();
        return false;
    }
    ps->cmd_busy = true;
    exit_critical();

    pr[PORT_IS / 4] = 0xFFFFFFFF;

    memset(&ps->cmd_list[0], 0, sizeof(cmd_header_t));
    ps->cmd_list[0].flags = (sizeof(fis_h2d_t) / 4) | (1 << 5); /* ATAPI */
    ps->cmd_list[0].prdtl = (xfer_len > 0) ? 1 : 0;
    ps->cmd_list[0].ctba = ps->cmd_table_phys;

    memset(ps->cmd_table, 0, sizeof(cmd_table_t));
    memcpy(ps->cmd_table->acmd, cdb, 12);

    fis_h2d_t *fis = (fis_h2d_t *)ps->cmd_table->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->flags = 0x80;
    fis->command = ATA_CMD_PACKET;
    fis->feature_lo = 0x01; /* DMA */
    fis->lba1 = (uint8_t)(xfer_len);
    fis->lba2 = (uint8_t)(xfer_len >> 8);

    if (xfer_len > 0)
    {
        ps->cmd_table->prdt[0].dba = ps->dma_buf_phys;
        ps->cmd_table->prdt[0].dbau = 0;
        ps->cmd_table->prdt[0].dbc = (xfer_len - 1) | (1u << 31);
    }

    enter_critical();
    cmd_inflight[p] = true;
    pr[PORT_CI / 4] = 1;
    exit_critical();

    bool ok = ahci_wait_done(p, AHCI_ATAPI_TIMEOUT_MS);

    /* Vedi issue_cmd: clear idempotente del latch sotto critical per i
     * percorsi senza wakeup del dispatcher. */
    enter_critical();
    cmd_inflight[p] = false;
    exit_critical();
    ps->cmd_busy = false;
    return ok;
}

/* === Media-presence detection (robust, real-hardware aware) ===
 *
 * QEMU answers everything instantly and cleanly; real optical drives do
 * not. This routine is built for the messy real cases:
 *   - After a media change, the first command returns CHECK CONDITION
 *     with sense key UNIT ATTENTION (0x06). That is not an error -- it is
 *     the drive telling us the media changed. We must swallow it and retry.
 *   - A freshly inserted disc is "becoming ready" (sense 0x02 / ASC 0x04)
 *     for a few seconds while it spins up. We retry with backoff rather
 *     than declaring "no media".
 *   - "Medium not present" (ASC 0x3A) is the authoritative "tray empty".
 *   - Some drives don't implement GESN, or implement it wrongly. So the
 *     authoritative presence test is TEST UNIT READY (universal), with
 *     REQUEST SENSE to interpret a CHECK. GESN, when supported, is only
 *     what wakes us (the async notification); TUR is what we trust.
 *
 * Called ONLY from the media handler thread (issue_atapi blocks on a
 * dispatcher-delivered completion; the dispatcher must stay free).
 * Returns true if a disc is loaded and ready, false otherwise. */
static bool
ahci_request_sense(int p, uint8_t *sense_key, uint8_t *asc)
{
    port_state_t *ps = &ports[p];
    uint8_t cdb[12];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = ATAPI_CMD_REQUEST_SENSE;
    cdb[4] = 18;                       /* allocation length */

    memset(ps->dma_buf, 0, 18);
    if (!issue_atapi(ps, p, cdb, 18))
        return false;

    const uint8_t *s = ps->dma_buf;
    if (sense_key) *sense_key = s[2] & 0x0F;
    if (asc)       *asc       = s[12];
    return true;
}

static bool
ahci_media_present(int p)
{
    port_state_t *ps = &ports[p];

    /* Up to ~5 s of patience for spin-up / unit-attention churn. */
    for (int attempt = 0; attempt < 10; attempt++)
    {
        uint8_t cdb[12];
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = ATAPI_CMD_TEST_UNIT_READY;   /* no data transfer */

        if (issue_atapi(ps, p, cdb, 0))
            return true;   /* ready: media present */

        /* CHECK CONDITION — ask why. */
        uint8_t key = 0, asc = 0;
        if (!ahci_request_sense(p, &key, &asc))
        {
            /* Couldn't even get sense; wait briefly and retry. */
            sleep_ms(200);
            continue;
        }

        if (key == SENSE_UNIT_ATTENTION)
        {
            /* Media changed / reset. Re-poll immediately (the UA is
             * cleared by this very REQUEST SENSE). */
            continue;
        }
        if (key == SENSE_NOT_READY)
        {
            if (asc == ASC_MEDIUM_NOT_PRESENT)
                return false;            /* tray empty: authoritative */
            if (asc == ASC_BECOMING_READY)
            {
                sleep_ms(400);           /* spinning up: wait and retry */
                continue;
            }
            /* Other not-ready: give it one more chance. */
            sleep_ms(200);
            continue;
        }
        /* Any other sense: treat as no reliable media this round. */
        return false;
    }
    return false;   /* never settled: report empty rather than hang */
}
/* Probe whether this optical drive answers GESN at all, so we know if the
 * async-notification path is even meaningful on this hardware. Best
 * effort: a clean GESN response with a sane header => supported. */
static bool
ahci_probe_gesn(int p)
{
    port_state_t *ps = &ports[p];
    uint8_t cdb[12];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = ATAPI_CMD_GET_EVENT_STATUS;
    cdb[1] = 0x01;                          /* polled */
    cdb[4] = (1u << GESN_CLASS_MEDIA);
    cdb[8] = 8;

    memset(ps->dma_buf, 0, 8);
    if (!issue_atapi(ps, p, cdb, 8))
        return false;
    /* Byte 2 low bits = notification class; nonzero header length in
     * bytes 0..1 indicates a real response. Treat any clean reply as
     * "GESN works". */
    const uint8_t *r = ps->dma_buf;
    uint16_t len = ((uint16_t)r[0] << 8) | r[1];
    return len > 0;
}

/* Announce or retract an optical drive's media to hotplug. Mirrors the
 * boot-time SUBDEVICE_APPEARED exactly; GONE uses the same token so
 * hotplug retracts the very bubble it created. */
static void
ahci_announce_optical(int p, bool present)
{
    uint32_t hp = dob_registry_find("hotplug");
    if (!hp) return;

    dob_msg_t m, r;
    memset(&m, 0, sizeof(m));
    memset(&r, 0, sizeof(r));

    if (present)
    {
        hotplug_subdev_appeared_t req;
        memset(&req, 0, sizeof(req));
        req.sub.provider_token = (uint32_t)p;
        req.sub.class_code     = 0x01;   /* storage */
        req.sub.subclass       = 0x05;   /* optical */
        strncpy(req.sub.provider_service, g_service_name,
                sizeof(req.sub.provider_service) - 1);
        m.code         = HOTPLUG_SUBDEVICE_APPEARED;
        m.payload      = &req;
        m.payload_size = sizeof(req);
        (void)dob_ipc_call(hp, &m, &r);
    }
    else
    {
        hotplug_subdev_gone_t req;
        memset(&req, 0, sizeof(req));
        req.provider_token = (uint32_t)p;
        m.code         = HOTPLUG_SUBDEVICE_GONE;
        m.payload      = &req;
        m.payload_size = sizeof(req);
        (void)dob_ipc_call(hp, &m, &r);
    }
}

/* Device-icon announce/retract for a FIXED DISK port (HDD/SSD) — the
 * runtime twin of the boot-time loop in the icons thread. Mirrors
 * ahci_announce_optical: APPEARED with the media-technology subclass
 * (0x08 magnetic / 0x09 solid-state), GONE by token on removal. */
static void
ahci_announce_disk(int p, bool present)
{
    uint32_t hp = dob_registry_find("hotplug");
    if (!hp) return;

    dob_msg_t m, r;
    memset(&m, 0, sizeof(m));
    memset(&r, 0, sizeof(r));

    if (present)
    {
        hotplug_subdev_appeared_t req;
        memset(&req, 0, sizeof(req));
        req.sub.provider_token = AHCI_DISK_ICON_TOKEN(p);
        req.sub.class_code     = 0x01;   /* storage */
        req.sub.subclass       = (ports[p].type == DEV_SSD) ? 0x09 : 0x08;
        strncpy(req.sub.provider_service, g_service_name,
                sizeof(req.sub.provider_service) - 1);
        m.code         = HOTPLUG_SUBDEVICE_APPEARED;
        m.payload      = &req;
        m.payload_size = sizeof(req);
        (void)dob_ipc_call(hp, &m, &r);
    }
    else
    {
        hotplug_subdev_gone_t req;
        memset(&req, 0, sizeof(req));
        req.provider_token = AHCI_DISK_ICON_TOKEN(p);
        m.code         = HOTPLUG_SUBDEVICE_GONE;
        m.payload      = &req;
        m.payload_size = sizeof(req);
        (void)dob_ipc_call(hp, &m, &r);
    }
}

/* Media handler thread: woken by the dispatcher when an async
 * notification fires on an optical port. Determines true media state via
 * the robust detector and, on a real transition, tells hotplug to add or
 * remove the disc's icon. */
static void
ahci_media_thread(void *arg)
{
    (void)arg;
    for (;;)
    {
        dob_msg_t ev;
        memset(&ev, 0, sizeof(ev));
        if (dob_ipc_receive(media_event_port, &ev) != DOB_OK)
            continue;
        if (ev.code != 2)
            continue;

        int p = (int)ev.arg0;
        if (p < 0 || p >= MAX_PORTS || ports[p].type != DEV_OPTICAL)
            continue;

        bool present = ahci_media_present(p);

        /* Act only on an actual change (debounce repeated notifications,
         * which real drives can emit in bursts). Short critical region
         * around the state compare-and-update. */
        bool changed = false;
        enter_critical();
        if (present != media_present[p])
        {
            media_present[p] = present;
            changed = true;
        }
        exit_critical();

        if (!changed)
            continue;

        char buf[80];
        snprintf(buf, sizeof(buf), "[sata-media] port %d: media %s\n",
                 p, present ? "inserted" : "removed");
        debug_print(buf);

        ahci_announce_optical(p, present);
    }
}

/* Copy IDENTIFY string: ATA swaps bytes within each word */
static void
identify_string(const uint16_t *src, char *dst, int words)
{
    for (int i = 0; i < words; i++)
    {
        dst[i * 2]     = (char)(src[i] >> 8);
        dst[i * 2 + 1] = (char)(src[i] & 0xFF);
    }
    dst[words * 2] = '\0';

    /* Trim trailing spaces */
    int end = words * 2 - 1;
    while (end >= 0 && dst[end] == ' ')
        dst[end--] = '\0';
}

/* Identify the device on port p.
 *
 * Returns true when the device answered IDENTIFY with a usable identity:
 *   - HDD/SSD: a *non-zero* capacity (see the LBA28/48 logic below). A
 *     disk we cannot size is not safe to advertise -- block_write trusts
 *     total_sectors for its bounds, so a 0-sector phantom would let the
 *     utility show a disk it can neither read (no MBR -> no partitions,
 *     no free space) nor write safely.
 *   - OPTICAL: a successful ATAPI IDENTIFY round-trip (capacity is
 *     meaningless for removable media).
 *
 * A false return tells port_setup not to advertise the port. */
static bool
port_identify(port_state_t *ps, int p)
{
    uint8_t cmd = (ps->type == DEV_OPTICAL) ? ATA_CMD_IDENTIFY_ATAPI
                                            : ATA_CMD_IDENTIFY;

    for (int attempt = 0; attempt < AHCI_IDENTIFY_RETRIES; attempt++)
    {
        if (attempt != 0)
            sleep_ms(AHCI_IDENTIFY_RETRY_MS);

        memset(ps->dma_buf, 0, SECTOR_SIZE);
        if (!issue_cmd(ps, p, cmd, 0, 0, false, SECTOR_SIZE))
            continue;   /* command did not complete -- retry */

        uint16_t *id = (uint16_t *)ps->dma_buf;

        identify_string(&id[27], ps->model, 20);
        identify_string(&id[10], ps->serial, 10);

        if (ps->type == DEV_OPTICAL)
            return true;   /* no capacity concept for removable media */

        /* Capacity, robust across drive generations (mirrors the ATA
         * driver's identify_extract_total_sectors):
         *   - LBA48 (words 100-103) when the 48-bit address feature set is
         *     advertised (word 83 bit 10) AND the value is non-zero;
         *   - otherwise LBA28 (words 60-61).
         * The previous code read ONLY words 100-103, so any LBA28-only
         * disk (which reports its size in 60-61 and zero in 100-103) was
         * left at sector_count 0. That is exactly the "SATA disk appears
         * but has no partitions and no free space" field failure: the disk
         * is announced, but every read -- LBA 0 included -- is rejected by
         * the block layer's `lba + count > total_sectors` bound. */
        bool     lba48 = (id[83] & (1u << 10)) != 0;
        uint64_t cap48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                         ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
        uint64_t cap28 = (uint64_t)id[60]  | ((uint64_t)id[61]  << 16);

        ps->sector_count = (lba48 && cap48) ? cap48 : cap28;

        /* Rotation rate: word 217. 0x0001 = SSD (non-rotating media) */
        if (id[217] == 0x0001)
            ps->type = DEV_SSD;

        /* TRIM support: word 169 bit 0 */
        if (id[169] & 0x01)
            ps->trim_supported = true;

        {
            char d[160];
            snprintf(d, sizeof(d),
                     "[sata] port %d IDENTIFY ok: lba48=%d cap48=%u "
                     "cap28=%u -> sectors=%u (try %d)\n",
                     p, lba48, (uint32_t)cap48, (uint32_t)cap28,
                     (uint32_t)ps->sector_count, attempt + 1);
            debug_print(d);
        }

        if (ps->sector_count > 0)
            return true;

        /* IDENTIFY completed but reported zero usable sectors -- treat as
         * transient (some bridges need a second IDENTIFY) and retry. */
    }

    {
        char d[112];
        snprintf(d, sizeof(d),
                 "[sata] port %d: IDENTIFY failed / zero capacity after %d "
                 "tries -- not advertising\n",
                 p, AHCI_IDENTIFY_RETRIES);
        debug_print(d);
    }
    return false;
}

/* SMART READ DATA (ATA command 0xB0, sub-command 0xD0) issued through
 * AHCI as a 512-byte data-in. Mirrors the ATA driver's
 * pio_smart_read_data_slot contract: one raw 512-byte SMART data
 * structure (ATA-7 layout: 2-byte revision, 30 x 12-byte vendor
 * attributes, then offline/self-test status). The SMART magic bytes go
 * in LBA mid/hi via ATA_SMART_MAGIC_LBA; issue_cmd loads the sub-command
 * into Features when command == ATA_CMD_SMART.
 *
 * SMART is optional: a drive without it aborts the command (PxTFD.ERR),
 * which issue_cmd surfaces as false. Returns false for optical media (no
 * SMART) and on any command failure. */
/* Issue one SMART sub-command with a small busy-retry budget. SMART is a
 * user-initiated query and may race background I/O (partition pusher,
 * filesystem) for the one-deep slot-0 lock: an instant BUSY fail here
 * turned into a spurious "SMART unsupported" popup. `step` tags cmd_diag
 * so a failure reports WHICH stage died (1=READ#1, 2=ENABLE, 3=READ#2). */
static bool
smart_issue(port_state_t *ps, int p, uint8_t sub, uint32_t byte_count,
            uint32_t step)
{
    for (int attempt = 0; attempt < 4; attempt++)
    {
        if (issue_cmd_feat(ps, p, ATA_CMD_SMART, ATA_SMART_MAGIC_LBA,
                           byte_count ? 1 : 0, false, byte_count, sub))
        {
            return true;
        }
        cmd_diag[p][3] = step;
        if (cmd_diag[p][0] != CMDDIAG_BUSY)
            return false;          /* real failure: no point retrying */
        sleep_ms(30);              /* transient contention: retry */
    }
    return false;
}

static bool
port_smart_read_data(port_state_t *ps, int p, void *out512)
{
    if (ps->type == DEV_OPTICAL)
        return false;

    memset(ps->dma_buf, 0, SECTOR_SIZE);
    if (!smart_issue(ps, p, ATA_SMART_READ_DATA, SECTOR_SIZE, 1))
    {
        /* First failure: SMART may simply be DISABLED on the drive.
         * The enabled/disabled flag is persistent DRIVE-side state
         * (IDENTIFY word 85 bit 0) and some BIOSes/vendors ship or
         * leave it off — QEMU's model is always enabled, which is why
         * this only ever surfaced on real disks (Toshiba MK1246GSX on
         * the Extensa 5220). Issue SMART ENABLE OPERATIONS (0xD8,
         * non-data) and retry READ DATA once. A drive with no SMART
         * feature set at all aborts the ENABLE too, and we return
         * false exactly as before. */
        debug_print("[sata] SMART READ DATA aborted; trying SMART "
                    "ENABLE OPERATIONS + retry\n");
        if (!smart_issue(ps, p, ATA_SMART_ENABLE, 0, 2))
            return false;

        memset(ps->dma_buf, 0, SECTOR_SIZE);
        if (!smart_issue(ps, p, ATA_SMART_READ_DATA, SECTOR_SIZE, 3))
            return false;
    }

    memcpy(out512, ps->dma_buf, SECTOR_SIZE);
    return true;
}

/* === Port setup: detect + alloc + configure === */

static bool
port_setup(int p)
{
    volatile uint32_t *pr = port_regs(p);
    port_state_t *ps = &ports[p];

    /* Reset link/identity state but keep any DMA already allocated for
     * this port (no dma_free exists; a hotplug re-insert reuses it). */
    cmd_header_t *cl = ps->cmd_list;  uint32_t clp = ps->cmd_list_phys;
    uint8_t      *fa = ps->fis_area;  uint32_t fap = ps->fis_area_phys;
    cmd_table_t  *ct = ps->cmd_table; uint32_t ctp = ps->cmd_table_phys;
    uint8_t      *db = ps->dma_buf;   uint32_t dbp = ps->dma_buf_phys;

    memset(ps, 0, sizeof(port_state_t));

    ps->cmd_list  = cl; ps->cmd_list_phys  = clp;
    ps->fis_area  = fa; ps->fis_area_phys  = fap;
    ps->cmd_table = ct; ps->cmd_table_phys = ctp;
    ps->dma_buf   = db; ps->dma_buf_phys   = dbp;

    /* Bring the port up before sampling SSTS. Set Spin-Up Device and
     * Power On Device, and request the Active power state, then poll DET
     * until the PHY link establishes (DET==3). Reading SSTS immediately
     * after AHCI-enable -- as the previous code did -- races the link
     * negotiation: on QEMU (and real silicon) DET is often still 1
     * (device present, PHY not yet ready) for a short window, so an
     * attached disk was missed and the controller looked empty. */
    pr[PORT_CMD / 4] |= PORT_CMD_SUD | PORT_CMD_POD | PORT_CMD_ICC_ACTIVE;

    /* Two-stage PHY wait. Link DETECTION is fast on any sane HBA (QEMU is
     * immediate; real silicon signals COMINIT in milliseconds): give it a
     * short grace. Only if a device IS detected but the link is still
     * negotiating (DET==1, e.g. staggered spin-up) extend to the full 1 s.
     * The old unconditional 1 s per port made init O(seconds) on a 6-port
     * controller with empty ports — and since v53 claims the "ahci" name
     * only AFTER init, that blew DobFileSystem's 3 s root-probe window on
     * installed boots: no root mount, no UI. Empty ports now cost 100 ms. */
    ahci_wait_reg(&pr[PORT_SSTS / 4], 0x0F, 3, 100, 1);
    if ((pr[PORT_SSTS / 4] & 0x0F) == 1)                 /* detected, not ready */
        ahci_wait_reg(&pr[PORT_SSTS / 4], 0x0F, 3, 1000, 5);

    uint32_t ssts = pr[PORT_SSTS / 4];
    uint8_t det = ssts & 0x0F;
    uint8_t ipm = (ssts >> 8) & 0x0F;


    if (det != 3 || ipm != 1)
        return false;

    ps->present = true;
    ps->sig = pr[PORT_SIG / 4];

    if (ps->sig == SIG_ATA)
        ps->type = DEV_HDD;  /* Refined to DEV_SSD after IDENTIFY */
    else if (ps->sig == SIG_ATAPI)
        ps->type = DEV_OPTICAL;
    else
        return false;  /* Unknown device type */

    if (!port_alloc_dma(ps))
    {
        return false;
    }

    port_stop(p);

    pr[PORT_CLB / 4]  = ps->cmd_list_phys;
    pr[PORT_CLBU / 4] = 0;
    pr[PORT_FB / 4]   = ps->fis_area_phys;
    pr[PORT_FBU / 4]  = 0;
    pr[PORT_SERR / 4] = 0xFFFFFFFF;
    pr[PORT_IS / 4]   = 0xFFFFFFFF;

    port_start(p);

    /* IDENTIFY the device. For a HDD/SSD, a false return means we could
     * not learn a safe capacity -- do NOT advertise the port as a block
     * device in that case (better a clean "no disk" than a phantom
     * 0-sector disk the utility shows but can neither read nor safely
     * write). The caller's "empty port" path re-arms hotplug listen, so a
     * later re-plug still gets another chance. Optical drives are kept on
     * a successful ATAPI IDENTIFY; a transient miss there still leaves a
     * usable icon, so we don't fail the port for them. */
    bool id_ok = port_identify(ps, p);
    if (ps->type != DEV_OPTICAL && !id_ok)
    {
        ps->present = false;
        return false;
    }

    /* For optical drives, additionally enable the Set Device Bits FIS
     * interrupt (async notification) IF the controller advertises SNTF
     * support (CAP.SSNTF). A SATAPI drive raises it when the media
     * changes -- our no-polling runtime detection. On controllers/drives
     * that lack it the icon is still correct at boot; we just won't get
     * live updates (acceptable, and avoids enabling a feature the HBA
     * doesn't implement -- which on real silicon can wedge the port). */
    if (ps->type == DEV_OPTICAL)
    {
        volatile uint32_t *hba = (volatile uint32_t *)hba_base;
        uint32_t cap = hba[HBA_CAP / 4];
        bool ssntf = (cap & HBA_CAP_SSNTF) != 0;
        if (ssntf)
            pr[PORT_IE / 4] |= PORT_IE_SDBS;

        char d[112];
        snprintf(d, sizeof(d),
                 "[sata-media] port %d: CAP=0x%08x SSNTF=%d SDBS-enabled=%d PxIE=0x%08x\n",
                 p, cap, ssntf, ssntf, pr[PORT_IE / 4]);
        debug_print(d);
    }

    /* Arm PhyRdy-change so a future *removal* of this device raises an
     * interrupt and the hotplug thread can retract it. Clear any stale
     * SERR latch first. */
    pr[PORT_SERR / 4] = PORT_SERR_DIAG_N;
    pr[PORT_IE / 4] |= PORT_IE_PRCS;

    return true;
}

/* === Init: scan all ports === */

/* Defined later (after the partition/announce helpers it calls); forward-
 * declared so init_hardware can spawn it. */
static void ahci_hotplug_thread(void *arg);

static bool
init_hardware(hotplug_device_t *dev)
{
    uint32_t abar_phys = dev->bar[5] & 0xFFFFFFF0;
    if (!abar_phys) return false;

    pci_enable_bus_master(dev);

    hba_base = (volatile uint8_t *)mmap_phys(abar_phys, 0x4000);
    if (!hba_base) return false;

    volatile uint32_t *hba = (volatile uint32_t *)hba_base;
    hba[HBA_GHC / 4] |= (1u << 31);   /* AHCI Enable */

    /* Register IRQ BEFORE port setup — port_stop/port_start need it */
    irq_port = (uint32_t)port_create();

    /* Register by device identity; the kernel resolves the line (PIC
     * delivery) or the GSI (IOAPIC delivery — possibly pending, in which
     * case the dispatcher claims it on the first interrupt). */
    int reg = irq_register_pci(dev->bus, dev->slot, dev->func, irq_port);
    if (reg > 0)
        irq_num = (uint8_t)reg;
    else if (reg == 0)
        irq_num = 0;                  /* IOAPIC: GSI pending until first IRQ */
    else if (dev->irq_line > 0 && dev->irq_line < 32)
    {
        irq_num = dev->irq_line;      /* PIC fallback: config Interrupt Line */
        irq_register(irq_num, irq_port);
    }

    /* HBA Interrupt Enable is deferred to just before the dispatcher thread
     * starts (see below). Enabling it here would let the long, polled init
     * phase (COMRESET/IDENTIFY per port) assert interrupts that no thread is
     * yet draining; under empirical GSI resolution those would be parked
     * before the dispatcher could claim the GSI. Init needs no IRQs — it
     * polls PxCI — so global IE stays off until the dispatcher is live. */

    uint32_t pi = hba[HBA_PI / 4];
    uint32_t ver = hba[HBA_VS / 4];


    debug_print("[sata] AHCI version ");
    char vbuf[16];
    snprintf(vbuf, sizeof(vbuf), "%u.%u\n", ver >> 16, ver & 0xFFFF);
    debug_print(vbuf);

    port_count = 0;
    for (int p = 0; p < MAX_PORTS; p++)
    {
        if (!(pi & (1u << p)))
            continue;

        /* TEMP DIAGNOSTIC (build 110): no disk is being detected behind
         * the AHCI controller. Log the raw SSTS for each implemented port
         * so we can tell an empty port (DET=0, disk not actually attached
         * -> a QEMU wiring issue) from a port whose link never reaches
         * DET=3 (a driver bring-up issue). */

        if (port_setup(p))
        {
            port_state_t *ps = &ports[p];
            port_count++;

            char buf[128];
            const char *type_str = "???";
            if (ps->type == DEV_HDD) type_str = "HDD";
            if (ps->type == DEV_SSD) type_str = "SSD";
            if (ps->type == DEV_OPTICAL) type_str = "OPT";

            snprintf(buf, sizeof(buf), "[sata] Port %d: %s %s",
                     p, type_str, ps->model);
            debug_print(buf);

            if (ps->type != DEV_OPTICAL && ps->sector_count > 0)
            {
                uint32_t mb = (uint32_t)(ps->sector_count >> 11);
                snprintf(buf, sizeof(buf), " (%u MB)", (uint32_t)mb);
                debug_print(buf);
            }
            if (ps->trim_supported)
                debug_print(" [TRIM]");
            debug_print("\n");
        }
        else
        {
            /* Implemented port with no device right now. To detect a LATER
             * hot-insert we must leave the port actively LISTENING, not just
             * arm the PRCS interrupt. QEMU's AHCI model (and real silicon
             * with staggered spin-up) only raises PhyRdy Change on a port
             * whose PHY is powered and spun up; a port left with SUD=0 never
             * sees the new device and the interrupt never fires — which is
             * exactly the "hotplug does nothing" symptom.
             *
             * So mirror the front half of port_setup, minus IDENTIFY (there
             * is no device to identify yet): allocate DMA, program PxCLB/FB
             * so the HBA can post the receive-FIS on insertion, power the
             * PHY (SUD|POD|ICC_ACTIVE), enable FRE, clear latches, and arm
             * PxIE.PRCS. When a disk is later plugged in, the dispatcher
             * sees PxIS.PRCS and hands the port to ahci_hotplug_thread,
             * which runs the blocking bring-up + announce. */
            volatile uint32_t *pr = port_regs(p);
            port_state_t *ps = &ports[p];

            char buf[112];
            snprintf(buf, sizeof(buf),
                     "[sata] Port %d empty: arming hotplug listen "
                     "(SSTS=0x%08x SIG=0x%08x)\n",
                     p, pr[PORT_SSTS / 4], pr[PORT_SIG / 4]);
            debug_print(buf);

            if (port_alloc_dma(ps))
            {
                port_stop(p);

                pr[PORT_CLB / 4]  = ps->cmd_list_phys;
                pr[PORT_CLBU / 4] = 0;
                pr[PORT_FB / 4]   = ps->fis_area_phys;
                pr[PORT_FBU / 4]  = 0;

                /* Power + spin up the PHY so it performs detection. */
                pr[PORT_CMD / 4] |= PORT_CMD_SUD | PORT_CMD_POD |
                                    PORT_CMD_ICC_ACTIVE;
                /* Enable receive-FIS so an insertion's D2H FIS lands. */
                pr[PORT_CMD / 4] |= PORT_CMD_FRE;

                pr[PORT_SERR / 4] = 0xFFFFFFFF;   /* clear all latches */
                pr[PORT_IS / 4]   = 0xFFFFFFFF;
                pr[PORT_IE / 4]   = PORT_IE_PRCS; /* hotplug detection */
            }
            else
            {
                /* DMA exhausted: fall back to detection-only. Better than
                 * nothing, though insertion may be missed on QEMU. */
                snprintf(buf, sizeof(buf),
                         "[sata] Port %d: DMA alloc failed, "
                         "detection-only\n", p);
                debug_print(buf);
                pr[PORT_SERR / 4] = 0xFFFFFFFF;
                pr[PORT_IS / 4]   = 0xFFFFFFFF;
                pr[PORT_IE / 4]   = PORT_IE_PRCS;
            }
        }
    }

    /* The HBA is up and mapped; that is success. Zero attached disks is
     * a perfectly valid state (an AHCI controller with empty ports), not
     * an init failure -- we must still register the "ahci" service so
     * the block layer and disk utility can query it, and so a disk that
     * appears later has somewhere to bind. Reporting failure here caused
     * the driver to _exit and tear its bubble down, which is why an
     * empty-port AHCI controller left the utility with no service to
     * talk to. */
    /* All ports initialized. From here on the dispatcher thread becomes
     * the SOLE reader of irq_port. We deliberately spawn it only now:
     * during init, port_stop/port_start/port_setup use ahci_wait_reg
     * which itself receives on irq_port, so the dispatcher must not be
     * running yet (else two readers). Create one completion port per
     * IMPLEMENTED port (not just per present one): a port that is empty at
     * boot can gain a disk at runtime, and the hotplug bring-up issues an
     * IDENTIFY whose completion ahci_wait_done delivers via this port. */
    for (int p = 0; p < MAX_PORTS; p++)
    {
        cmd_inflight[p] = false;
        if (pi & (1u << p))
            cmd_done_port[p] = (uint32_t)port_create();
    }

    /* Disk-hotplug handler: create its wakeup port and spawn the thread
     * before the dispatcher, since the dispatcher posts PhyRdy-change
     * events here as soon as port_hotplug_port is non-zero. Armed whenever
     * the controller has at least one implemented port (always true). */
    port_hotplug_port = (uint32_t)port_create();
    dob_thread_spawn(ahci_hotplug_thread, NULL);

    /* Media handler for optical ports. Always created: a port empty at
     * boot can gain an optical drive at runtime (via the hotplug thread),
     * and its media-change async notifications post here. */
    for (int p = 0; p < MAX_PORTS; p++)
        media_present[p] = false;
    media_event_port = (uint32_t)port_create();
    dob_thread_spawn(ahci_media_thread, NULL);

    /* Enable HBA interrupt delivery now: the dispatcher is about to start,
     * so from here an assertion is drained promptly — and under empirical
     * GSI resolution the dispatcher claims the controller's GSI on that
     * first interrupt. (Deferred from init_hardware; see the rationale
     * there.) */
    ((volatile uint32_t *)hba_base)[HBA_GHC / 4] |= (1u << 1);

    dob_thread_spawn(ahci_dispatcher_thread, NULL);

    /* SELF-HEALING SWEEP: any implemented port whose link is UP (DET==3)
     * but that is NOT marked present had its boot bring-up fail or its
     * PhyRdy latch consumed during init (SUD raced the PORT_IS clear in
     * the empty-port arming above). PRCS never fires again for it — the
     * link is already up — so, unrecovered, the disk is invisible until
     * reboot: the intermittent "installation disk sometimes missing"
     * symptom. Post a synthetic port-change event per such port; the
     * hotplug thread (just spawned) re-runs the full bring-up with its
     * retry budget, now with the dispatcher live and IRQs enabled. */
    for (int p = 0; p < MAX_PORTS; p++)
    {
        if (!(pi & (1u << p)) || ports[p].present)
            continue;
        volatile uint32_t *pr = port_regs(p);
        if ((pr[PORT_SSTS / 4] & 0x0F) == 3)
        {
            char buf[96];
            snprintf(buf, sizeof(buf),
                     "[sata] Port %d: link up but not present after init — "
                     "posting recovery bring-up\n", p);
            debug_print(buf);
            dob_msg_t rp;
            memset(&rp, 0, sizeof(rp));
            rp.code = 3;
            rp.arg0 = (uint32_t)p;
            dob_ipc_post(port_hotplug_port, &rp);
        }
    }

    /* Now that the dispatcher is running, GESN can complete (its
     * completion is delivered by the dispatcher). Seed current media
     * state so the first runtime async event is seen as a real
     * transition rather than a spurious one. Per-port filtered, so it is
     * a no-op when no optical drive is present at boot. */
    {
        for (int p = 0; p < MAX_PORTS; p++)
        {
            if (ports[p].present && ports[p].type == DEV_OPTICAL)
            {
                /* Does this drive answer GESN? (real drives vary). The
                 * async path still works via the SDBS interrupt either
                 * way; this just records capability for diagnostics. */
                ports[p].gesn_supported = ahci_probe_gesn(p);
                /* Authoritative initial media state via the robust
                 * detector (TUR + sense), so the first runtime event is
                 * a true transition. */
                media_present[p] = ahci_media_present(p);

                char buf[80];
                snprintf(buf, sizeof(buf),
                         "[sata-media] port %d optical: gesn=%d media=%d at boot\n",
                         p, ports[p].gesn_supported, media_present[p]);
                debug_print(buf);
            }
        }
    }

    return true;
}

/* === IPC reply buffers (per port) ===
 * One buffer per port instead of a single shared one: a reply's payload
 * pointer stays valid until the server has sent it, so two reads serviced
 * for different ports must not share the staging buffer. Indexed by port,
 * matching ports[]/cmd_done_port[]. */
static uint8_t reply_buf[MAX_PORTS][DMA_BUF_SIZE];

/* === Partition scan support ===
 *
 * Wraps issue_cmd in a partition_read_sector_fn closure so
 * libdob/dob/partition.c can fetch sector 0 of any port via our
 * internal DMA read path (no IPC round-trip back to ourselves). */

static bool
ahci_partition_read_sector(void *ctx, uint32_t lba, void *out)
{
    int p = (int)(uintptr_t)ctx;
    if (p < 0 || p >= MAX_PORTS || !ports[p].present) return false;
    port_state_t *ps = &ports[p];
    if (ps->type != DEV_HDD && ps->type != DEV_SSD)   return false;
    if (!issue_cmd(ps, p, ATA_CMD_READ_DMA_EX, lba, 1, false, SECTOR_SIZE))
        return false;
    memcpy(out, ps->dma_buf, SECTOR_SIZE);
    return true;
}

static bool
ahci_partition_announce(int port)
{
    if (port < 0 || port >= MAX_PORTS || !ports[port].present) return false;
    if (ports[port].type != DEV_HDD && ports[port].type != DEV_SSD) return false;

    partition_scan_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));
    pctx.read_sector      = ahci_partition_read_sector;
    pctx.ctx              = (void *)(uintptr_t)port;
    pctx.provider_service = g_service_name;
    pctx.native_selector  = (uint32_t)port;
    return partition_scan_announce(&pctx);
}

/* Background thread: wait for hotplug, then announce partitions on
 * every present HDD/SSD port. Same pattern as ata's
 * hdd_partition_pusher_thread. */
static void
ahci_partition_pusher_thread(void *arg)
{
    (void)arg;
    uint32_t hp = dob_registry_wait("hotplug", 60000);
    if (!hp)
    {
        debug_print("[sata] hotplug never appeared, device icons skipped\n");
        return;
    }

    /* Optical drives first: SUBDEVICE_APPEARED per drive, DAS-matched by
     * hotplug (config/DAS/cdrom_ahci.das) into a desktop icon. Token =
     * AHCI port index (the cdrom driver binds via its argv). Moved here
     * from main() so the server loop is never delayed by hotplug waits. */
    for (int p = 0; p < MAX_PORTS; p++)
    {
        if (!ports[p].present || ports[p].type != DEV_OPTICAL) continue;

        hotplug_subdev_appeared_t req;
        memset(&req, 0, sizeof(req));
        req.sub.provider_token = (uint32_t)p;
        req.sub.class_code     = 0x01;   /* storage  */
        req.sub.subclass       = 0x05;   /* optical  */
        strncpy(req.sub.provider_service, g_service_name,
                sizeof(req.sub.provider_service) - 1);

        dob_msg_t m, r;
        memset(&m, 0, sizeof(m));
        memset(&r, 0, sizeof(r));
        m.code         = HOTPLUG_SUBDEVICE_APPEARED;
        m.payload      = &req;
        m.payload_size = sizeof(req);
        (void)dob_ipc_call(hp, &m, &r);
    }

    /* Fixed disks: a DEVICE-level icon per port, distinguishing the
     * media technology — 0x01:0x08 magnetic (HDD) vs 0x01:0x09 solid
     * state (word 217 == 1 at IDENTIFY). Until now a fixed disk had NO
     * desktop identity of its own (only its FAT32/exFAT partitions got
     * icons), so a GPT/foreign disk was INVISIBLE on the desktop and
     * the only AHCI icon around was the optical drive's — field-read
     * as "AHCI takes the SSD for an optical disc" on the CQ62. Token =
     * port; double-click opens DobDisk preselected (--select sata_N). */
    for (int p = 0; p < MAX_PORTS; p++)
    {
        if (!ports[p].present) continue;
        if (ports[p].type != DEV_HDD && ports[p].type != DEV_SSD) continue;

        hotplug_subdev_appeared_t req;
        memset(&req, 0, sizeof(req));
        req.sub.provider_token = AHCI_DISK_ICON_TOKEN(p);
        req.sub.class_code     = 0x01;   /* storage */
        req.sub.subclass       = (ports[p].type == DEV_SSD) ? 0x09 : 0x08;
        strncpy(req.sub.provider_service, g_service_name,
                sizeof(req.sub.provider_service) - 1);

        dob_msg_t m, r;
        memset(&m, 0, sizeof(m));
        memset(&r, 0, sizeof(r));
        m.code         = HOTPLUG_SUBDEVICE_APPEARED;
        m.payload      = &req;
        m.payload_size = sizeof(req);
        (void)dob_ipc_call(hp, &m, &r);
    }

    for (int p = 0; p < MAX_PORTS; p++)
    {
        if (!ports[p].present) continue;
        if (ports[p].type != DEV_HDD && ports[p].type != DEV_SSD) continue;
        if (ahci_partition_announce(p))
            debug_print("[sata] Initial partition scan announced\n");
    }
}

/* === Disk hotplug (runtime port insert/remove) === */

/* A read_sector callback that always fails: used to drive
 * partition_scan_announce on a port whose disk just vanished. The library
 * is delta-based per (service, selector), so a scan that yields zero
 * partitions emits SUBDEVICE_GONE for every partition previously announced
 * on that port. */
static bool
ahci_partition_read_gone(void *ctx, uint32_t lba, void *out)
{
    (void)ctx; (void)lba; (void)out;
    return false;
}

/* Retract every partition previously announced for a port (disk removed),
 * then free that port's DMA so a future insert re-allocates cleanly. */
static void
ahci_disk_retract(int p)
{
    partition_scan_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));
    pctx.read_sector      = ahci_partition_read_gone;
    pctx.ctx              = (void *)(uintptr_t)p;
    pctx.provider_service = g_service_name;
    pctx.native_selector  = (uint32_t)p;
    (void)partition_scan_announce(&pctx);   /* emits GONE for the deltas */
}

/* Tear down a port whose device was removed: stop the engine, mark the
 * port not-present, keep only hotplug detection armed. The per-port DMA
 * buffers are intentionally retained: there is no dma_free syscall, and a
 * future insertion on the same port reuses them (port_alloc_dma is
 * idempotent). Safe to call on an already-empty port. */
static void
ahci_port_teardown(int p)
{
    port_stop(p);

    volatile uint32_t *pr = port_regs(p);
    pr[PORT_IE / 4] = PORT_IE_PRCS;   /* keep only hotplug detection armed */

    /* Preserve the DMA pointers/phys across the wipe so a re-insert can
     * reuse them; clear the link/identity state only. */
    cmd_header_t *cl = ports[p].cmd_list;  uint32_t clp = ports[p].cmd_list_phys;
    uint8_t      *fa = ports[p].fis_area;  uint32_t fap = ports[p].fis_area_phys;
    cmd_table_t  *ct = ports[p].cmd_table; uint32_t ctp = ports[p].cmd_table_phys;
    uint8_t      *db = ports[p].dma_buf;   uint32_t dbp = ports[p].dma_buf_phys;

    memset(&ports[p], 0, sizeof(port_state_t));

    ports[p].cmd_list  = cl; ports[p].cmd_list_phys  = clp;
    ports[p].fis_area  = fa; ports[p].fis_area_phys  = fap;
    ports[p].cmd_table = ct; ports[p].cmd_table_phys = ctp;
    ports[p].dma_buf   = db; ports[p].dma_buf_phys   = dbp;

    media_present[p] = false;
}

/* The disk-hotplug thread. Woken by the dispatcher on a PhyRdy Change.
 * Re-reads the port's link state and reconciles it against what we had:
 *   - link up, we had nothing  => bring the port up and announce it
 *   - link down, we had a device => tear it down and retract it
 * The blocking work (COMRESET via port_setup, IDENTIFY, partition read)
 * lives here, never in the dispatcher. */
static void
ahci_hotplug_thread(void *arg)
{
    (void)arg;
    for (;;)
    {
        dob_msg_t ev;
        memset(&ev, 0, sizeof(ev));
        if (dob_ipc_receive(port_hotplug_port, &ev) != DOB_OK)
            continue;
        if (ev.code != 3)
            continue;

        int p = (int)ev.arg0;
        if (p < 0 || p >= MAX_PORTS)
            continue;

        /* Debounce: a hotplug transition bounces DET for a moment. Give
         * the PHY a beat to settle before sampling. */
        sleep_ms(50);

        volatile uint32_t *pr = port_regs(p);
        uint8_t det = pr[PORT_SSTS / 4] & 0x0F;
        bool link_up = (det == 3);
        bool had_dev = ports[p].present;

        if (link_up && !had_dev)
        {
            /* Insertion. port_setup does COMRESET-equivalent bring-up,
             * allocates DMA, IDENTIFYs, and (for optical) arms SDBS. */
            if (port_setup(p))
            {
                bringup_retries[p] = 0;

                /* Re-arm hotplug detection on the now-live port (port_setup
                 * programmed PxIE for normal operation). */
                pr[PORT_IE / 4] |= PORT_IE_PRCS;

                char buf[96];
                const char *t = ports[p].type == DEV_OPTICAL ? "optical"
                              : ports[p].type == DEV_SSD ? "SSD" : "HDD";
                snprintf(buf, sizeof(buf),
                         "[sata-hotplug] port %d: %s inserted\n", p, t);
                debug_print(buf);

                if (ports[p].type == DEV_OPTICAL)
                    ahci_announce_optical(p, true);
                else
                {
                    ahci_announce_disk(p, true);
                    ahci_partition_announce(p);
                }
            }
            else if (bringup_retries[p] < AHCI_BRINGUP_RETRY_MAX)
            {
                /* Link is up but bring-up failed. PRCS will NOT fire again
                 * (no further phy change), so without a retry this disk is
                 * lost until reboot. Backoff, then re-post the event to
                 * ourselves; a genuinely dead port exhausts the budget. */
                bringup_retries[p]++;
                char buf[96];
                snprintf(buf, sizeof(buf),
                         "[sata-hotplug] port %d: bring-up failed, retry %u/%u\n",
                         p, bringup_retries[p], AHCI_BRINGUP_RETRY_MAX);
                debug_print(buf);

                sleep_ms(200u * bringup_retries[p]);
                dob_msg_t rp;
                memset(&rp, 0, sizeof(rp));
                rp.code = 3;
                rp.arg0 = (uint32_t)p;
                dob_ipc_post(port_hotplug_port, &rp);
            }
        }
        else if (!link_up && had_dev)
        {
            /* Removal. */
            char buf[96];
            snprintf(buf, sizeof(buf),
                     "[sata-hotplug] port %d: device removed\n", p);
            debug_print(buf);
            bringup_retries[p] = 0;   /* fresh budget for the next insert */

            bool was_optical = (ports[p].type == DEV_OPTICAL);
            if (was_optical)
                ahci_announce_optical(p, false);
            else
            {
                ahci_announce_disk(p, false);
                ahci_disk_retract(p);
            }

            ahci_port_teardown(p);
        }
        /* link_up==had_dev: spurious / debounced-away transition, ignore. */
    }
}

/* === IPC handler === */

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
        case AHCI_OP_READ: /* 1 */
        {
            int p = (int)msg->arg0;
            uint32_t lba = msg->arg1;
            uint32_t count = msg->arg2;

            if (p < 0 || p >= MAX_PORTS || !ports[p].present)
                return DOB_ERR_INVALID;
            if (count == 0 || count > 128)
                return DOB_ERR_INVALID;

            port_state_t *ps = &ports[p];
            uint32_t bytes = count * SECTOR_SIZE;
            memset(ps->dma_buf, 0, bytes);

            if (!issue_cmd(ps, p, ATA_CMD_READ_DMA_EX, lba, (uint16_t)count, false, bytes))
                return DOB_ERR_INTERNAL;

            memcpy(reply_buf[p], ps->dma_buf, bytes);
            reply->payload = reply_buf[p];
            reply->payload_size = bytes;
            reply->arg0 = bytes;
            return DOB_OK;
        }

        case AHCI_OP_WRITE: /* 2 */
        {
            int p = (int)msg->arg0;
            uint32_t lba = msg->arg1;
            uint32_t count = msg->arg2;

            if (p < 0 || p >= MAX_PORTS || !ports[p].present)
                return DOB_ERR_INVALID;
            if (count == 0 || count > 128)
                return DOB_ERR_INVALID;
            if (!msg->payload || msg->payload_size < count * SECTOR_SIZE)
                return DOB_ERR_INVALID;

            port_state_t *ps = &ports[p];
            uint32_t bytes = count * SECTOR_SIZE;
            memcpy(ps->dma_buf, msg->payload, bytes);

            if (!issue_cmd(ps, p, ATA_CMD_WRITE_DMA_EX, lba, (uint16_t)count, true, bytes))
                return DOB_ERR_INTERNAL;

            return DOB_OK;
        }

        case AHCI_OP_IDENTIFY: /* 3 */
        {
            int p = (int)msg->arg0;
            if (p < 0 || p >= MAX_PORTS || !ports[p].present)
                return DOB_ERR_INVALID;

            port_state_t *ps = &ports[p];
            uint8_t cmd = (ps->type == DEV_OPTICAL)
                          ? ATA_CMD_IDENTIFY_ATAPI : ATA_CMD_IDENTIFY;

            memset(ps->dma_buf, 0, SECTOR_SIZE);
            if (!issue_cmd(ps, p, cmd, 0, 0, false, SECTOR_SIZE))
                return DOB_ERR_INTERNAL;

            memcpy(reply_buf[p], ps->dma_buf, SECTOR_SIZE);
            reply->payload = reply_buf[p];
            reply->payload_size = SECTOR_SIZE;
            return DOB_OK;
        }

        case AHCI_OP_TRIM: /* 4 — SSD only */
        {
            int p = (int)msg->arg0;
            uint32_t lba = msg->arg1;
            uint32_t count = msg->arg2;

            if (p < 0 || p >= MAX_PORTS || !ports[p].present)
                return DOB_ERR_INVALID;
            if (ports[p].type != DEV_SSD || !ports[p].trim_supported)
                return DOB_ERR_INVALID;

            /* Build TRIM range entry (8 bytes: LBA + count) */
            port_state_t *ps = &ports[p];
            memset(ps->dma_buf, 0, SECTOR_SIZE);
            uint64_t *entry = (uint64_t *)ps->dma_buf;
            *entry = ((uint64_t)count << 48) | (uint64_t)lba;

            if (!issue_cmd(ps, p, ATA_CMD_DATA_SET_MGMT, 0, 1, true, SECTOR_SIZE))
                return DOB_ERR_INTERNAL;

            return DOB_OK;
        }

        case AHCI_OP_ATAPI_PACKET: /* 5 — optical only */
        {
            int p = (int)msg->arg0;
            if (p < 0 || p >= MAX_PORTS || !ports[p].present)
                return DOB_ERR_INVALID;
            if (ports[p].type != DEV_OPTICAL)
                return DOB_ERR_INVALID;
            if (!msg->payload || msg->payload_size < 12)
                return DOB_ERR_INVALID;

            port_state_t *ps = &ports[p];
            uint32_t xfer = (msg->arg1 > 0) ? msg->arg1 : 2048;
            if (xfer > DMA_BUF_SIZE) xfer = DMA_BUF_SIZE;

            memset(ps->dma_buf, 0, xfer);
            if (!issue_atapi(ps, p, (const uint8_t *)msg->payload, xfer))
                return DOB_ERR_INTERNAL;

            memcpy(reply_buf[p], ps->dma_buf, xfer);
            reply->payload = reply_buf[p];
            reply->payload_size = xfer;
            reply->arg0 = xfer;
            return DOB_OK;
        }

        case AHCI_OP_LIST_PORTS: /* 10 */
        {
            static sata_port_info_t infos[MAX_PORTS];
            uint32_t n = 0;

            for (int p = 0; p < MAX_PORTS; p++)
            {
                if (!ports[p].present)
                    continue;

                sata_port_info_t *out = &infos[n++];
                out->port_num = (uint8_t)p;
                out->type = ports[p].type;
                out->sector_count = ports[p].sector_count;
                out->trim_supported = ports[p].trim_supported;
                strncpy(out->model, ports[p].model, 40);
                strncpy(out->serial, ports[p].serial, 20);
            }

            reply->arg0 = n;
            if (n == 0)
            {
                reply->payload = 0;
                reply->payload_size = 0;
            }
            else
            {
                reply->payload = infos;
                reply->payload_size = n * sizeof(sata_port_info_t);
            }
            return DOB_OK;
        }

        case AHCI_OP_EJECT: /* 11 — optical only */
        {
            int p = (int)msg->arg0;
            if (p < 0 || p >= MAX_PORTS || !ports[p].present)
                return DOB_ERR_INVALID;
            if (ports[p].type != DEV_OPTICAL)
                return DOB_ERR_INVALID;

            /* SCSI START STOP UNIT: eject */
            uint8_t cdb[12] = {0};
            cdb[0] = 0x1B;  /* START STOP UNIT */
            cdb[4] = 0x02;  /* LoEj=1, Start=0 → eject */

            port_state_t *ps = &ports[p];
            if (!issue_atapi(ps, p, cdb, 0))
                return DOB_ERR_INTERNAL;

            return DOB_OK;
        }

        case AHCI_OP_RESCAN_PARTITIONS: /* 20 — rescan MBR of one port */
        {
            int p = (int)msg->arg0;
            if (p < 0 || p >= MAX_PORTS || !ports[p].present)
                return DOB_ERR_INVALID;
            if (ports[p].type != DEV_HDD && ports[p].type != DEV_SSD)
                return DOB_ERR_INVALID;
            if (!ahci_partition_announce(p))
                return DOB_ERR_INTERNAL;
            return DOB_OK;
        }

        case AHCI_OP_GET_SMART: /* 21 — arg0 = port, payload = 512 raw SMART */
        {
            int p = (int)msg->arg0;
            if (p < 0 || p >= MAX_PORTS || !ports[p].present)
                return DOB_ERR_INVALID;
            if (ports[p].type == DEV_OPTICAL)
                return DOB_ERR_INVALID;   /* SMART is a HDD/SSD feature */

            if (!port_smart_read_data(&ports[p], p, reply_buf[p]))
            {
                /* Ship the failure diagnostics in the reply args — the
                 * reply struct reaches the client even with an error
                 * code (dob_server_loop sends what the handler filled).
                 * DobDisk renders these in its popup: on log-less
                 * laptops the popup IS the diagnostic table. */
                reply->arg0 = cmd_diag[p][0];   /* reason */
                reply->arg1 = cmd_diag[p][1];   /* PxTFD  */
                reply->arg2 = cmd_diag[p][2];   /* PxSERR */
                reply->arg3 = cmd_diag[p][3];   /* step   */
                return DOB_ERR_INTERNAL;
            }

            reply->payload      = reply_buf[p];
            reply->payload_size = SECTOR_SIZE;
            return DOB_OK;
        }

        default:
            return DOB_ERR_INVALID;
    }
}

/* === Main === */

int
main(void)
{
    hotplug_device_t dev;

    memset(ports, 0, sizeof(ports));

    /* Anti-double-run watchdog, FIRST — before dob_server_init and before
     * dob_driver_attach. The disk driver is launched from Startup_modules
     * so the boot disk is readable before DobFileSystem; but ahci.das also
     * makes hotplug spawn an instance per AHCI controller (this machine has
     * two: 00:04.0 and 00:1f.2). Two drivers sharing the HBA collide on the
     * IRQ and every command times out. The Startup_modules instance starts
     * first and registers 'ahci'; any later duplicate must bow out here,
     * BEFORE dob_server_init (so it never overwrites the winner's registry
     * entry — registration is last-writer-wins).
     *
     * Crucially we must NOT call dob_driver_attach before this point:
     * attach sends HOTPLUG_READY(dob_server_get_port()), but the server
     * isn't initialised yet, so it would send port 0 and the device reply
     * would be routed to a bogus port — which is what was crashing the
     * duplicate instances ("Bubble #N: driver crashed"). The duplicate
     * check needs only the registry, not attach. A duplicate releases its
     * hotplug bubble cleanly; dob_driver_released() just messages hotplug
     * and is harmless if we weren't hotplug-spawned. */
    /* Atomic singleton election. This machine can expose more than one AHCI
     * controller (e.g. 00:04.0 and 00:1f.2), and ahci.das spawns one instance
     * per controller; but "ahci" is a single service and two drivers on one HBA
     * collide on the IRQ. The OLD guard was check-then-register
     * (dob_registry_find then dob_server_init), which RACES under SMP: both
     * instances can pass the find() before either registers, then both attach
     * and corrupt each other. Instead register FIRST — the registry refuses a
     * name held by a live peer atomically under its own lock — and bow out on
     * failure. Init-before-attach is preserved: dob_driver_attach needs a valid
     * server port, and it runs only on the winner, AFTER a successful register.
     * A loser releases its hotplug bubble cleanly and exits before touching HW. */
    /* MULTI-CONTROLLER ELECTION, phase 1: create the port WITHOUT claiming
     * "ahci". The old design registered "ahci" as a global singleton and the
     * loser EXITED — correct against two drivers on ONE HBA, but on a
     * machine with TWO controllers (00:04.0 + 00:1f.2) it killed the second
     * controller's driver, and WHICH instance survived was an SMP timing
     * race. Whenever the winner was the instance bound to the diskless
     * controller, the disk vanished everywhere (no icon, empty DobDisk) —
     * the bimodal boot symptom. Now: every instance initialises ITS
     * controller; the name "ahci" goes to whoever actually has disks
     * (phase 2, after init_hardware). */
    if (dob_server_init_noreg() != DOB_OK)
    {
        debug_print("[sata] server port creation failed.\n");
        dob_driver_released();
        _exit(1);
    }

    /* Now ask hotplug for our device assignment with a valid reply port.
     * Returns false if we were started standalone (not via hotplug). */
    bool via_hotplug = dob_driver_attach(&dev);

    if (!via_hotplug)
    {
        /* Not spawned by hotplug -> we were started directly from
         * Startup_modules so the boot disk (a SATA drive) is readable
         * before DobFileSystem comes up. Scan PCI ourselves for the
         * AHCI controller (class 01:06), exactly as the ATA driver does
         * for IDE in its standalone path. */
        memset(&dev, 0, sizeof(dev));
        bool found = false;
        for (uint8_t s = 0; s < 32 && !found; s++)
        {
            for (uint8_t f = 0; f < 8; f++)
            {
                uint32_t id = pci_config_read(0, s, f, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                uint32_t cls = pci_config_read(0, s, f, 0x08);
                if (((cls >> 24) & 0xFF) != 0x01 ||
                    ((cls >> 16) & 0xFF) != 0x06) continue;

                dev.bus = 0; dev.slot = s; dev.func = f;
                dev.vendor_id = id & 0xFFFF;
                dev.device_id = (id >> 16) & 0xFFFF;
                dev.class_code = 0x01; dev.subclass = 0x06;
                dev.bar[5] = pci_config_read(0, s, f, 0x24) & 0xFFFFFFF0u;
                dev.irq_line = (uint8_t)(pci_config_read(0, s, f, 0x3C) & 0xFF);
                found = true;
                break;
            }
        }
        if (!found)
        {
            /* No AHCI controller on this (standalone) machine. Nothing to
             * undo: the port was created with init_noreg and NO name was
             * claimed yet, so an IDE-only machine never sees a stale
             * 'ahci' entry. (Hotplug-spawned instances always have a
             * controller, so they never reach here.) */
            debug_print("[sata] Standalone: no AHCI controller on PCI.\n");
            _exit(0);
        }
        debug_print("[sata] Standalone: found AHCI controller.\n");
    }

    /* Precedenza al gemello di boot: l'istanza standalone (avviata da
     * Startup_modules) esiste APPOSTA per avere il disco di boot presto.
     * Se questa istanza arriva via hotplug mentre quella sta ancora
     * scandendo/inizializzando, vincere la chiave BDF la ucciderebbe e il
     * root mount resterebbe appeso a NOI, in ritardo di secondi: e' la
     * radice del "disco a volte si', a volte no" (boot bimodale, funzione
     * del timing SMP). Un differimento breve PRIMA del claim rende
     * l'esito deterministico: lo standalone claima per primo e il gemello
     * hotplug perde con grazia; sui controller senza istanza di boot (il
     * secondo HBA) il differimento costa solo mezzo secondo di ritardo. */
    if (via_hotplug)
    {
        sleep_ms(500);
    }

    /* Per-CONTROLLER dedup (replaces the old global singleton for its
     * legitimate purpose): Startup_modules starts one instance and
     * ahci.das makes hotplug start one per controller, so the SAME HBA can
     * get two drivers — which collide on the IRQ and time out every
     * command. Claim an atomic registry key derived from the controller's
     * BDF; the loser exits BEFORE touching hardware. Instances on
     * DIFFERENT controllers claim different keys and coexist. */
    {
        char hw_key[24];
        snprintf(hw_key, sizeof(hw_key), "ahci@%u:%u.%u",
                 dev.bus, dev.slot, dev.func);
        if (dob_registry_register(hw_key, dob_server_get_port()) != DOB_OK)
        {
            char b[64];
            snprintf(b, sizeof(b),
                     "[sata] controller %s already driven; this instance "
                     "exits.\n", hw_key);
            debug_print(b);
            dob_driver_released();
            _exit(0);
        }
    }

    if (!init_hardware(&dev))
    {
        debug_print("[sata] AHCI controller init failed.\n");
        _exit(1);
    }

    /* Zero disks at boot is NO LONGER a reason to exit. With PhyRdy-change
     * hotplug detection armed on every implemented port and the hotplug
     * thread running, an empty controller must stay resident to catch a
     * disk inserted later — that is exactly the runtime-hotplug behaviour
     * we now support. The "ahci" service is registered, so the disk
     * utility and block layer have someone to talk to, and a future insert
     * binds against this live driver. (The old code released the bubble
     * here, which is why an empty controller used to vanish.) */
    char buf[96];
    snprintf(buf, sizeof(buf), "[sata] %u device(s) online at boot.\n",
             port_count);
    debug_print(buf);

    /* MULTI-CONTROLLER ELECTION, phase 2: claim the "ahci" service name.
     * Policy: the instance whose controller HAS devices claims immediately;
     * a diskless instance waits a short grace so a sibling with disks wins
     * first, then claims only if the name is still free (a single empty
     * controller must still expose the service: runtime hotplug and the
     * disk utility need someone to talk to). The registry claim is atomic,
     * so two disk-bearing instances cannot both win: the loser falls back
     * to a unique suffix ("ahci_N") — its icons/mounts still work because
     * every announce carries g_service_name, and DobFileSystem resolves
     * the provider string dynamically; only the fixed-name block clients
     * (DobDisk enumerate) see just the primary until they learn suffixes. */
    /* Priority = BLOCK devices (HDD/SSD). Counting ANY device re-created
     * the race on this machine: CD on one controller, HDD on the other →
     * both instances claimed immediately. The clients that resolve the
     * fixed name (root probe, block layer primary) want the DISKS. */
    uint32_t block_devs = 0;
    for (int p2 = 0; p2 < MAX_PORTS; p2++)
        if (ports[p2].present &&
            (ports[p2].type == DEV_HDD || ports[p2].type == DEV_SSD))
            block_devs++;
    if (block_devs == 0)
        sleep_ms(500);   /* let a disk-bearing sibling take the name */

    if (dob_server_claim_name("ahci") == DOB_OK)
    {
        snprintf(g_service_name, sizeof(g_service_name), "ahci");
    }
    else
    {
        bool named = false;
        for (int n = 1; n < 8 && !named; n++)
        {
            char alt[16];
            snprintf(alt, sizeof(alt), "ahci_%d", n);
            if (dob_server_claim_name(alt) == DOB_OK)
            {
                snprintf(g_service_name, sizeof(g_service_name), "%s", alt);
                named = true;
            }
        }
        snprintf(buf, sizeof(buf),
                 "[sata] 'ahci' taken by a sibling; serving as '%s' "
                 "(%u device(s)).\n", named ? g_service_name : "<unnamed>",
                 port_count);
        debug_print(buf);
    }

    /* Optical/partition announces are deferred to the background pusher
     * thread: they need hotplug (dob_registry_wait), and blocking main()
     * on that delays dob_server_loop — during which clients that already
     * resolved our name (DobFileSystem root probe!) sit on unanswered
     * calls. main() must reach the server loop IMMEDIATELY after the name
     * claim; everything hotplug-dependent runs in the pusher. */

    /* Background partition scanner: waits for hotplug, then emits
     * SUBDEVICE_APPEARED for every FAT32 partition on each HDD/SSD
     * port. Independent thread so the server loop below isn't blocked
     * by the dob_registry_wait. */
    bool have_block_dev = false;
    bool have_any_dev   = false;
    for (int p = 0; p < MAX_PORTS; p++)
    {
        if (!ports[p].present) continue;
        have_any_dev = true;
        if (ports[p].type == DEV_HDD || ports[p].type == DEV_SSD)
            have_block_dev = true;
    }
    if (have_any_dev)      /* opticals too: their announce lives in the pusher now */
        dob_thread_spawn(ahci_partition_pusher_thread, NULL);

    /* Boot-disk role arbitration. Only the instance started from
     * Startup_modules (via_hotplug == false) is a candidate to own the
     * system disk; a hotplug-spawned instance exists to serve a second
     * controller's peripherals and must NOT claim the base disk. The
     * first eligible driver to get here registers the 'bootdisk' marker;
     * any later disk driver sees it taken and leaves the system-disk
     * role alone (it still serves its own 'ahci'/'ata' clients). The
     * marker lives entirely among the disk drivers -- DobFileSystem does
     * not consult it (it keeps its ahci-then-ata auto-probe). */
    if (have_block_dev && !via_hotplug && dob_registry_find("bootdisk") == 0)
    {
        dob_registry_register("bootdisk", dob_server_get_port());
        debug_print("[sata] claimed boot-disk role.\n");
    }

    dob_server_register(handle_message);
    dob_server_loop();
    return 0;
}
