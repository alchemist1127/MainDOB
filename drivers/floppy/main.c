/* MainDOB Floppy Driver — service "floppy"
 *
 * Hardware: legacy ISA FDC at 0x3F0 + IRQ6, ISA DMA ch.2 (4 KB buffer in
 * <16 MB zone, can't cross a 64 KB boundary by construction). FAT12,
 * 1.44 MB geometry (80×2×18×512).
 *
 * Spawned on-demand by the floppy DAS (config/DAS/floppy.das) when the
 * user double-clicks the desktop icon — not in Startup_modules. Pure IPC
 * server: speaks DobFileSystem opcodes plus FLOPPY_PROBE_MEDIA (64) and
 * DOBFS_FORMAT from the DAS chain. On a successful probe_media it calls
 * dobfiles_OpenMount() once via the EPS stub — only outbound call.
 *
 * Layers (each only calls the layer below):
 *   L0 isa_dma_*  8237 channel-2 programming
 *   L1 fdc_*      FDC primitives (reset, seek, wait-irq, ...)
 *   L2 sector_*   one 512-byte sector via DMA+IRQ
 *   L3 media_* fat12_*  media presence, FAT12 mount/format
 *   L4 fat12_* name_*   FAT12 in-RAM helpers (no I/O)
 *   L5 op_*       dobfs IPC handlers
 *   L6 op_*       custom IPC handlers (probe_media, format)
 *   L7 floppy_*   dispatch + bootstrap
 *
 * No busy-wait anywhere: FDC FIFO handshakes complete in 1–3 polls,
 * everything else is dob_ipc_receive on a timer (zero idle CPU). Motor-off
 * is a kernel timer callback.
 *
 * Disk-change safety: every file op begins with disk_change_check() which
 * inspects the FDC DIR bit 7. If DSKCHG is latched, the in-RAM FAT/root
 * cache is dropped, all open fds are marked stale, and the op returns
 * DOB_ERR_MEDIA_CHANGED *without touching media* — a stale fd can never
 * write an old FAT onto a new disk. */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <dob/types.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/server.h>
#include <dob/hotplug_driver.h>

#include "../../boot/DobFileSystem/dobfs_protocol.h"
#include <DobFiles.h>

/* Constants — FDC, ISA DMA, geometry, FAT12 */

/* -- FDC I/O ports (primary controller, base 0x3F0) -- */
#define FDC_BASE            0x3F0
#define FDC_DOR             (FDC_BASE + 2)  /* Digital Output Register  */
#define FDC_MSR             (FDC_BASE + 4)  /* Main Status  (R)         */
#define FDC_FIFO            (FDC_BASE + 5)  /* Data FIFO                */
#define FDC_DIR             (FDC_BASE + 7)  /* Digital Input  (R)       */
#define FDC_CCR             (FDC_BASE + 7)  /* Config Control (W)       */

/* -- DOR bits -- */
#define DOR_DSEL_MASK       0x03
#define DOR_NRESET          0x04
#define DOR_DMAGATE         0x08   /* enable DMA + IRQ from FDC         */
#define DOR_MOT0            0x10
#define DOR_MOT1            0x20

/* -- MSR bits -- */
#define MSR_RQM             0x80   /* data register ready               */
#define MSR_DIO             0x40   /* 1 = FDC→CPU, 0 = CPU→FDC          */

/* -- DIR bit -- */
#define DIR_DSKCHG          0x80   /* sticky disk-change latch          */

/* -- FDC commands -- */
#define FDC_CMD_SPECIFY     0x03
#define FDC_CMD_RECALIBRATE 0x07
#define FDC_CMD_SENSE_INT   0x08
#define FDC_CMD_SEEK        0x0F
#define FDC_CMD_READ_DATA   0x46   /* MFM, no MT, no SK                 */
#define FDC_CMD_WRITE_DATA  0x45   /* MFM, no MT                        */

/* -- ISA 8237 DMA controller (master, channels 0..3) -- */
#define DMA_MASK_REG        0x0A   /* single-channel mask               */
#define DMA_MODE_REG        0x0B
#define DMA_CLEAR_FF        0x0C
#define DMA_CH2_ADDR        0x04
#define DMA_CH2_COUNT       0x05
#define DMA_CH2_PAGE        0x81

/* Channel-2 mode bytes. Bit layout:
 *   [7:6] mode  (01 = single-transfer)
 *   [5]   decrement (0 = increment)
 *   [4]   auto-init  (0 = off)
 *   [3:2] transfer  (01 = write-to-mem = READ from disk;
 *                    10 = read-from-mem = WRITE to disk)
 *   [1:0] channel   (10 = channel 2)                                  */
#define DMA_MODE_READ       0x46   /* FDC → memory (floppy READ)        */
#define DMA_MODE_WRITE      0x4A   /* memory → FDC (floppy WRITE)       */

#define DMA_MASK_CH2        0x06   /* mask channel 2                    */
#define DMA_UNMASK_CH2      0x02   /* unmask channel 2                  */

/* -- IRQ + message codes -- */
#define FDC_IRQ_NUM         6
#define MSG_TIMER           70
#define IPC_TYPE_IRQ        3

/* -- Geometry (1.44 MB) -- */
#define BPS                 512
#define SECTORS_PER_TRACK   18
#define HEADS               2
#define CYLINDERS           80
#define TOTAL_SECTORS       (CYLINDERS * HEADS * SECTORS_PER_TRACK)

/* -- FAT12 layout (standard 1.44 MB) -- */
#define RESERVED_SECTORS    1
#define NUM_FATS            2
#define SECTORS_PER_FAT     9
#define ROOT_DIR_ENTRIES    224
#define DIR_ENTRY_SIZE      32
#define ROOT_DIR_BYTES      (ROOT_DIR_ENTRIES * DIR_ENTRY_SIZE)
#define ROOT_DIR_SECTORS    (ROOT_DIR_BYTES / BPS)
#define FAT_BYTES           (SECTORS_PER_FAT * BPS)
#define FAT_START_LBA       RESERVED_SECTORS
#define ROOT_DIR_START_LBA  (RESERVED_SECTORS + NUM_FATS * SECTORS_PER_FAT)
#define DATA_START_LBA      (ROOT_DIR_START_LBA + ROOT_DIR_SECTORS)

#define FAT12_FREE          0x000
#define FAT12_BAD           0xFF7
#define FAT12_EOC_MIN       0xFF8

#define ATTR_VOLUME_ID      0x08
#define ATTR_DIRECTORY      0x10
#define ATTR_ARCHIVE        0x20
#define ATTR_LFN            0x0F

/* -- Sizing / timing -- */
#define MAX_UNITS           2
#define MAX_OPEN_FILES      32
#define MOTOR_OFF_DELAY_MS  2000
#define FDC_TIMEOUT_MS      3000
#define FDC_FIFO_POLL_MAX   100000   /* upper bound for FIFO handshake  */

/* -- DAS-facing opcode (shared with floppy.das) -- */
#define FLOPPY_PROBE_MEDIA  64
#define FLOPPY_EJECT        65

/* Types */

typedef struct __attribute__((packed))
{
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  ctime_tenth;
    uint16_t ctime;
    uint16_t cdate;
    uint16_t adate;
    uint16_t cluster_hi;      /* always 0 on FAT12 */
    uint16_t mtime;
    uint16_t mdate;
    uint16_t cluster_lo;
    uint32_t size;
} fat_dirent_t;

typedef struct
{
    bool    mounted;
    uint8_t fat[FAT_BYTES];
    uint8_t root_dir[ROOT_DIR_BYTES];
} unit_state_t;

typedef struct
{
    bool     in_use;
    bool     stale;          /* media changed — all ops return CHANGED */
    uint8_t  unit;
    int      dir_idx;
    uint32_t pos;
    uint32_t size;
    uint8_t  flags;
    uint16_t first_cluster;
} fd_entry_t;

typedef enum { MEDIA_OK, MEDIA_ABSENT, MEDIA_HW_FAULT } media_probe_t;
typedef enum { DISK_OK,  DISK_CHANGED }                 disk_check_t;
typedef enum { MOUNT_OK, MOUNT_BAD_FS, MOUNT_HW_FAULT,
               MOUNT_NO_MEDIA }                         mount_result_t;

/* Module state */

static unit_state_t units[MAX_UNITS];
static fd_entry_t   fds[MAX_OPEN_FILES];

static uint32_t irq_port        = 0;
static int      motor_timer_id  = -1;
static bool     fdc_initialized = false;

/* Single shared 512-byte DMA transfer buffer (allocated in a 4 KB page
 * from the kernel DMA zone at boot — guaranteed <16 MB and naturally
 * 64 KB-boundary-safe because a single 4 KB page cannot straddle a
 * 64 KB boundary). */
static uint8_t  *dma_buf      = NULL;
static uint32_t  dma_buf_phys = 0;

/* Per-unit list of IPC ports to notify on eject. Populated via the
 * DOBFS_SUBSCRIBE_UNMOUNT opcode, iterated and cleared by op_eject.
 * 4 slots is enough for any reasonable number of concurrent viewers
 * on a single floppy — the usual case is 1 (the DobFiles window that
 * the mount surfaced). */
#define MAX_UNMOUNT_SUBS 4
static uint32_t unmount_subs[MAX_UNITS][MAX_UNMOUNT_SUBS];

/*  *  L0 — ISA DMA channel 2
 *
 *  Program the 8237 for a single-transfer cycle on channel 2, with the
 *  phys address and length our caller specifies. The FDC will trigger
 *  DRQ2 during the execution phase of READ_DATA/WRITE_DATA and the DMA
 *  controller will move bytes into (or out of) our buffer without any
 *  CPU involvement.
 */

static void
isa_dma_program(uint32_t phys, uint16_t len, uint8_t mode)
{
    uint16_t count = (uint16_t)(len - 1);   /* 8237 uses len-1 */

    io_outb(DMA_MASK_REG, DMA_MASK_CH2);                      /* mask ch2  */
    io_outb(DMA_CLEAR_FF, 0xFF);                              /* flip-flop */
    io_outb(DMA_CH2_ADDR, (uint8_t)( phys        & 0xFF));
    io_outb(DMA_CH2_ADDR, (uint8_t)((phys >>  8) & 0xFF));
    io_outb(DMA_CH2_PAGE, (uint8_t)((phys >> 16) & 0xFF));
    io_outb(DMA_CLEAR_FF, 0xFF);
    io_outb(DMA_CH2_COUNT, (uint8_t)( count       & 0xFF));
    io_outb(DMA_CH2_COUNT, (uint8_t)((count >> 8) & 0xFF));
    io_outb(DMA_MODE_REG, mode);
    io_outb(DMA_MASK_REG, DMA_UNMASK_CH2);                    /* unmask    */
}

static void
isa_dma_prepare_read(uint32_t phys, uint16_t len)
{
    isa_dma_program(phys, len, DMA_MODE_READ);
}

static void
isa_dma_prepare_write(uint32_t phys, uint16_t len)
{
    isa_dma_program(phys, len, DMA_MODE_WRITE);
}

/* L1 — FDC primitives */

/* Flush any IRQ notifications left over from a previous command.
 * Stale notifications would otherwise make the next fdc_wait_irq
 * return instantly on the wrong event. */
static void
fdc_drain_irq(void)
{
    dob_msg_t drain;
    memset(&drain, 0, sizeof(drain));
    while (dob_ipc_receive_nowait(irq_port, &drain) == DOB_OK)
    {
        if (drain.type == IPC_TYPE_IRQ)
            irq_done(FDC_IRQ_NUM);
        memset(&drain, 0, sizeof(drain));
    }
}

/* Block until the FDC raises IRQ6 or the timer fires.
 * Zero CPU while waiting — we are parked inside a kernel IPC receive. */
static bool
fdc_wait_irq(uint32_t timeout_ms)
{
    int tid = timer_set(irq_port, timeout_ms, 0);

    for (;;)
    {
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        if (dob_ipc_receive(irq_port, &msg) != DOB_OK)
        {
            timer_cancel_async(tid);
            return false;
        }
        if (msg.type == IPC_TYPE_IRQ)
        {
            irq_done(FDC_IRQ_NUM);
            timer_cancel_async(tid);
            return true;
        }
        if (msg.code == MSG_TIMER)
            return false;
    }
}

/* Wait for the FIFO handshake in the requested direction.
 * This is not a busy-wait in the traditional sense: the FDC signals
 * readiness via MSR.RQM within a few io_inb latencies (microseconds),
 * and the loop exits on the first ready cycle. We only bound it so
 * a wedged controller can't hang the driver forever. */
static bool
fdc_fifo_ready(bool cpu_to_fdc)
{
    for (int i = 0; i < FDC_FIFO_POLL_MAX; i++)
    {
        uint8_t msr = io_inb(FDC_MSR);
        if (msr & MSR_RQM)
        {
            bool dio = (msr & MSR_DIO) != 0;
            if (cpu_to_fdc == !dio)
                return true;
        }
    }
    return false;
}

static bool
fdc_send_byte(uint8_t b)
{
    if (!fdc_fifo_ready(true)) return false;
    io_outb(FDC_FIFO, b);
    return true;
}

static int
fdc_recv_byte(void)
{
    if (!fdc_fifo_ready(false)) return -1;
    return (int)io_inb(FDC_FIFO);
}

static bool
fdc_sense_interrupt(uint8_t *st0_out, uint8_t *cyl_out)
{
    if (!fdc_send_byte(FDC_CMD_SENSE_INT)) return false;
    int st0 = fdc_recv_byte();
    int pcn = fdc_recv_byte();
    if (st0 < 0 || pcn < 0) return false;
    if (st0_out) *st0_out = (uint8_t)st0;
    if (cyl_out) *cyl_out = (uint8_t)pcn;
    return true;
}

/* Cancel the pending motor-off timer, if armed. */
static void
fdc_motor_timer_cancel(void)
{
    if (motor_timer_id >= 0)
    {
        timer_cancel_async(motor_timer_id);
        motor_timer_id = -1;
    }
}

/* Energise the spindle and select the drive. DMA gate stays on. */
static void
fdc_motor_on(uint8_t unit)
{
    fdc_motor_timer_cancel();
    uint8_t dor = DOR_NRESET | DOR_DMAGATE | (unit & DOR_DSEL_MASK);
    dor |= (unit == 0) ? DOR_MOT0 : DOR_MOT1;
    io_outb(FDC_DOR, dor);
}

/* Cut power to the spindle immediately. */
static void
fdc_motor_off_now(void)
{
    io_outb(FDC_DOR, DOR_NRESET | DOR_DMAGATE);
    motor_timer_id = -1;
}

/* True if at least one fd is currently open. Used by fdc_motor_arm_off
 * to keep the spindle energised across long inter-read gaps for as long
 * as a client is actively using the drive — see the comment on
 * fdc_motor_arm_off below. */
static bool
any_fd_in_use(void)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++)
        if (fds[i].in_use)
            return true;
    return false;
}

/* Arm a one-shot timer to turn the motor off after MOTOR_OFF_DELAY_MS.
 * The main loop catches the expiry as an MSG_TIMER and calls motor_off.
 *
 * Skipped entirely while at least one file descriptor is open. Letting the
 * motor cycle off mid-session would re-latch the sticky DSKCHG bit (the
 * disk-change line floats high on a powered-down drive) and the next
 * op_read would interpret that as a real media change, mark the fd stale
 * and unmount the volume. Keeping the spindle on for the duration of a
 * session — i.e. until op_close releases the last fd — eliminates that
 * window. The motor still arms-off normally after directory ops, stat
 * probes, and any other one-shot use that doesn't leave an fd behind. */
static void
fdc_motor_arm_off(void)
{
    fdc_motor_timer_cancel();
    if (any_fd_in_use()) return;
    motor_timer_id = timer_set(irq_port, MOTOR_OFF_DELAY_MS, 0);
}

/* Program the mechanical timings and enable DMA mode (ND = 0).
 * SRT = 13, HUT = 15, HLT = 13 — conservative values that match what
 * the original IBM BIOS and modern Linux use. */
static bool
fdc_specify(void)
{
    if (!fdc_send_byte(FDC_CMD_SPECIFY))       return false;
    if (!fdc_send_byte(0xDF))                  return false;  /* SRT|HUT */
    if (!fdc_send_byte((uint8_t)(13 << 1)))    return false;  /* HLT, ND=0 */
    return true;
}

static bool
fdc_reset(void)
{
    fdc_drain_irq();

    io_outb(FDC_DOR, 0x00);                         /* assert reset     */

    /* Hold reset ~1 ms via a blocking timer. No busy-wait. Any IRQ that
     * fires during the park is drained to keep the queue clean for the
     * post-release wait below. */
    {
        int tid = timer_set(irq_port, 1, 0);
        for (;;)
        {
            dob_msg_t msg;
            memset(&msg, 0, sizeof(msg));
            if (dob_ipc_receive(irq_port, &msg) != DOB_OK)
            {
                timer_cancel_async(tid);
                break;
            }
            if (msg.code == MSG_TIMER) break;
            if (msg.type == IPC_TYPE_IRQ) irq_done(FDC_IRQ_NUM);
        }
    }

    io_outb(FDC_DOR, DOR_NRESET | DOR_DMAGATE);     /* release reset    */
    if (!fdc_wait_irq(FDC_TIMEOUT_MS))               return false;

    /* The FDC enters polling mode on reset and raises 4 phantom
     * interrupts — one per drive — which we must clear via sense_int. */
    uint8_t st0, pcn;
    for (int i = 0; i < 4; i++)
        if (!fdc_sense_interrupt(&st0, &pcn))
            return false;

    io_outb(FDC_CCR, 0x00);                         /* 500 kbps         */
    if (!fdc_specify())                             return false;
    return true;
}

/* Lazy initialisation: first real operation brings the hardware up.
 * Until then we look like a perfectly healthy service that simply
 * hasn't been asked to do anything yet. */
static bool
ensure_fdc_init(void)
{
    if (fdc_initialized) return true;
    if (!fdc_reset())    return false;
    fdc_initialized = true;
    return true;
}

static bool
fdc_recalibrate(uint8_t unit)
{
    fdc_drain_irq();
    if (!fdc_send_byte(FDC_CMD_RECALIBRATE))   return false;
    if (!fdc_send_byte(unit & 0x03))           return false;
    if (!fdc_wait_irq(FDC_TIMEOUT_MS))         return false;
    uint8_t st0, pcn;
    if (!fdc_sense_interrupt(&st0, &pcn))      return false;
    return (st0 & 0xC0) == 0;                  /* IC = 00 ⇒ normal */
}

static bool
fdc_seek(uint8_t unit, uint8_t cyl, uint8_t head)
{
    fdc_drain_irq();
    if (!fdc_send_byte(FDC_CMD_SEEK))                          return false;
    if (!fdc_send_byte((uint8_t)((head << 2) | (unit & 0x03)))) return false;
    if (!fdc_send_byte(cyl))                                   return false;
    if (!fdc_wait_irq(FDC_TIMEOUT_MS))                         return false;
    uint8_t st0, pcn;
    if (!fdc_sense_interrupt(&st0, &pcn))                      return false;
    return ((st0 & 0xC0) == 0) && (pcn == cyl);
}

static void
lba_to_chs(uint32_t lba, uint8_t *cyl, uint8_t *head, uint8_t *sec)
{
    *cyl  = (uint8_t)(lba / (HEADS * SECTORS_PER_TRACK));
    uint32_t r = lba % (HEADS * SECTORS_PER_TRACK);
    *head = (uint8_t)(r / SECTORS_PER_TRACK);
    *sec  = (uint8_t)((r % SECTORS_PER_TRACK) + 1);
}

/*  *  L2 — Sector I/O (full-duplex via ISA DMA)
 *
 *  sector_read and sector_write are peers: same structure, same single
 *  IRQ-per-sector wait, both use the shared 512-byte DMA buffer. There
 *  is no privileged direction — reads and writes are equal citizens.
 *
 *  Sequence for BOTH operations:
 *      1. motor_on
 *      2. recalibrate (cheap on already-calibrated drive, safe after
 *         reset or media change)
 *      3. seek to the target (cyl,head)
 *      4. program ISA DMA ch.2 for the correct direction
 *      5. issue READ_DATA / WRITE_DATA with the CHS parameters
 *      6. single fdc_wait_irq — the FDC drives DMA autonomously and
 *         asserts IRQ6 at end-of-sector
 *      7. drain the 7-byte result phase (ST0/ST1/ST2/C/H/R/N)
 *      8. copy data to/from the caller buffer
 */

static bool
sector_xfer_cmd(uint8_t opcode, uint8_t unit,
                uint8_t cyl, uint8_t head, uint8_t sec)
{
    if (!fdc_send_byte(opcode))                                return false;
    if (!fdc_send_byte((uint8_t)((head << 2) | (unit & 0x03)))) return false;
    if (!fdc_send_byte(cyl))                                   return false;
    if (!fdc_send_byte(head))                                  return false;
    if (!fdc_send_byte(sec))                                   return false;
    if (!fdc_send_byte(2))                                     return false;   /* 512 B  */
    if (!fdc_send_byte(SECTORS_PER_TRACK))                     return false;   /* EOT    */
    if (!fdc_send_byte(0x1B))                                  return false;   /* GAP3   */
    if (!fdc_send_byte(0xFF))                                  return false;   /* DTL    */
    return true;
}

static bool
sector_drain_result(void)
{
    for (int i = 0; i < 7; i++)
        if (fdc_recv_byte() < 0)
            return false;
    return true;
}

static bool
sector_read(uint8_t unit, uint32_t lba, uint8_t *out)
{
    uint8_t cyl, head, sec;
    lba_to_chs(lba, &cyl, &head, &sec);

    fdc_motor_on(unit);
    if (!fdc_recalibrate(unit))                    return false;
    if (!fdc_seek(unit, cyl, head))                return false;

    fdc_drain_irq();
    isa_dma_prepare_read(dma_buf_phys, BPS);

    if (!sector_xfer_cmd(FDC_CMD_READ_DATA, unit, cyl, head, sec)) return false;
    if (!fdc_wait_irq(FDC_TIMEOUT_MS))             return false;
    if (!sector_drain_result())                    return false;

    memcpy(out, dma_buf, BPS);
    return true;
}

static bool
sector_write(uint8_t unit, uint32_t lba, const uint8_t *in)
{
    uint8_t cyl, head, sec;
    lba_to_chs(lba, &cyl, &head, &sec);

    memcpy(dma_buf, in, BPS);

    fdc_motor_on(unit);
    if (!fdc_recalibrate(unit))                    return false;
    if (!fdc_seek(unit, cyl, head))                return false;

    fdc_drain_irq();
    isa_dma_prepare_write(dma_buf_phys, BPS);

    if (!sector_xfer_cmd(FDC_CMD_WRITE_DATA, unit, cyl, head, sec)) return false;
    if (!fdc_wait_irq(FDC_TIMEOUT_MS))             return false;
    if (!sector_drain_result())                    return false;

    return true;
}

/* L3 — Media presence, disk-change, mount, format */

/* Probe whether a disk is physically in the drive. On exit with
 * MEDIA_OK, DSKCHG is guaranteed to be cleared — the FDC has a known
 * head position at cylinder 0 and the disk has been "touched".
 *
 * Called from op_probe_media only. File ops use disk_change_check()
 * instead (much cheaper, no seeks). */
static media_probe_t
media_probe(uint8_t unit)
{
    if (!ensure_fdc_init())          return MEDIA_HW_FAULT;

    fdc_motor_on(unit);

    /* Recalibrate: move head to track 0. If this fails on a drive that
     * we know is electrically present, it almost always means no disk. */
    if (!fdc_recalibrate(unit))      return MEDIA_ABSENT;

    /* If DSKCHG is already clear we are done — the head is at track 0
     * and the FDC has a consistent position. */
    if (!(io_inb(FDC_DIR) & DIR_DSKCHG))
        return MEDIA_OK;

    /* DSKCHG latched. The spec says it clears on a successful seek to
     * a new cylinder. Step 0 → 1 → 0 to exercise the mechanism. */
    if (!fdc_seek(unit, 1, 0))       return MEDIA_ABSENT;
    if (!fdc_seek(unit, 0, 0))       return MEDIA_ABSENT;

    if (io_inb(FDC_DIR) & DIR_DSKCHG)
        return MEDIA_ABSENT;         /* still latched ⇒ drive is empty */

    return MEDIA_OK;
}

/* Disk-change check for file ops. DIR's DSKCHG bit is per-drive and
 * only meaningful when the drive is selected and its motor is on, so
 * we rewrite DOR before sampling.
 *
 * Fast path: DSKCHG clear → one port read, done.
 *
 * Slow path: DSKCHG is sticky and a motor-off/on cycle latches it
 * spuriously. Disambiguate by seeking 0 → 1 → 0 and re-sampling: if
 * the bit clears, the disk is still there; if it stays, it's really
 * been swapped. */
static disk_check_t
disk_change_check(uint8_t unit)
{
    fdc_motor_on(unit);
    if (!(io_inb(FDC_DIR) & DIR_DSKCHG))
        return DISK_OK;

    /* Sticky bit. Try the seek-dance to disambiguate spurious latch
     * (from spin-down) vs real media swap. Any seek failure here is
     * treated as a real change — same conservative stance as
     * media_probe. */
    if (!fdc_seek(unit, 1, 0)) return DISK_CHANGED;
    if (!fdc_seek(unit, 0, 0)) return DISK_CHANGED;

    if (io_inb(FDC_DIR) & DIR_DSKCHG)
        return DISK_CHANGED;

    return DISK_OK;
}

static void
invalidate_fds_for_unit(uint8_t unit)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++)
        if (fds[i].in_use && fds[i].unit == unit)
            fds[i].stale = true;
}

/* L4 — FAT12 in-RAM helpers (pure, no I/O) */

static uint16_t
fat12_get(unit_state_t *u, uint16_t cluster)
{
    uint32_t off = (uint32_t)cluster + (cluster / 2);
    if (off + 1 >= FAT_BYTES) return FAT12_EOC_MIN;
    uint16_t v = (uint16_t)u->fat[off] | ((uint16_t)u->fat[off + 1] << 8);
    return (cluster & 1) ? (v >> 4) : (v & 0x0FFF);
}

static void
fat12_set(unit_state_t *u, uint16_t cluster, uint16_t value)
{
    uint32_t off = (uint32_t)cluster + (cluster / 2);
    if (off + 1 >= FAT_BYTES) return;
    uint16_t v = (uint16_t)u->fat[off] | ((uint16_t)u->fat[off + 1] << 8);
    if (cluster & 1) v = (v & 0x000F) | ((value & 0x0FFF) << 4);
    else             v = (v & 0xF000) |  (value & 0x0FFF);
    u->fat[off]     = (uint8_t)(v & 0xFF);
    u->fat[off + 1] = (uint8_t)(v >> 8);
}

static uint16_t
fat12_alloc(unit_state_t *u)
{
    uint16_t total = (uint16_t)(TOTAL_SECTORS - DATA_START_LBA + 2);
    for (uint16_t c = 2; c < total; c++)
    {
        if (fat12_get(u, c) == FAT12_FREE)
        {
            fat12_set(u, c, FAT12_EOC_MIN);
            return c;
        }
    }
    return 0;
}

static void
fat12_free_chain(unit_state_t *u, uint16_t start)
{
    uint16_t c = start;
    while (c >= 2 && c < FAT12_BAD)
    {
        uint16_t next = fat12_get(u, c);
        fat12_set(u, c, FAT12_FREE);
        if (next < 2 || next >= FAT12_BAD) break;
        c = next;
    }
}

static uint32_t
cluster_to_lba(uint16_t cluster)
{
    return DATA_START_LBA + (uint32_t)(cluster - 2);
}

static uint16_t
chain_seek(unit_state_t *u, uint16_t start, uint32_t pos)
{
    if (start < 2) return 0;
    uint16_t c = start;
    uint32_t skip = pos / BPS;
    while (skip-- > 0)
    {
        uint16_t next = fat12_get(u, c);
        if (next < 2 || next >= FAT12_BAD) break;
        c = next;
    }
    return c;
}

static bool
name_to_83(const char *name, char out[11])
{
    memset(out, ' ', 11);
    int i = 0, j = 0;
    while (name[i] && name[i] != '.' && j < 8)
    {
        char c = name[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[j++] = c;
    }
    if (name[i] == '.')
    {
        i++;
        int k = 0;
        while (name[i] && k < 3)
        {
            char c = name[i++];
            if (c >= 'a' && c <= 'z') c -= 32;
            out[8 + k++] = c;
        }
    }
    return name[i] == '\0';
}

static void
name_from_83(const fat_dirent_t *de, char out[13])
{
    int o = 0;
    for (int i = 0; i < 8 && de->name[i] != ' '; i++) out[o++] = de->name[i];
    if (de->ext[0] != ' ')
    {
        out[o++] = '.';
        for (int i = 0; i < 3 && de->ext[i] != ' '; i++) out[o++] = de->ext[i];
    }
    out[o] = '\0';
}

static int
root_find(unit_state_t *u, const char name83[11])
{
    for (int i = 0; i < ROOT_DIR_ENTRIES; i++)
    {
        fat_dirent_t *de = &((fat_dirent_t *)u->root_dir)[i];
        if ((uint8_t)de->name[0] == 0x00) return -1;   /* end-of-list  */
        if ((uint8_t)de->name[0] == 0xE5) continue;    /* deleted      */
        if (de->attr == ATTR_LFN)         continue;
        if (memcmp(de->name, name83,     8) == 0
         && memcmp(de->ext,  name83 + 8, 3) == 0)
            return i;
    }
    return -1;
}

static int
root_alloc(unit_state_t *u)
{
    for (int i = 0; i < ROOT_DIR_ENTRIES; i++)
    {
        fat_dirent_t *de = &((fat_dirent_t *)u->root_dir)[i];
        if ((uint8_t)de->name[0] == 0x00 || (uint8_t)de->name[0] == 0xE5)
            return i;
    }
    return -1;
}

static bool
path_parse(const char *path, uint8_t *unit_out, const char **rel_out)
{
    if (!path)                              return false;
    if (path[0] != '/' || path[1] != 'u')   return false;
    if (path[2] < '0' || path[2] > '9')     return false;
    *unit_out = (uint8_t)(path[2] - '0');
    if (*unit_out >= MAX_UNITS)             return false;
    if (path[3] == '\0') { *rel_out = "/"; return true; }
    if (path[3] != '/')                     return false;
    *rel_out = path + 3;
    return true;
}

/* Persist both FAT copies and the root directory back to the diskette.
 * Called after any metadata mutation (allocation, dir entry update). */
static bool
fat12_flush_metadata(uint8_t unit)
{
    unit_state_t *u = &units[unit];
    for (int copy = 0; copy < NUM_FATS; copy++)
    {
        uint32_t base = FAT_START_LBA + copy * SECTORS_PER_FAT;
        for (int s = 0; s < SECTORS_PER_FAT; s++)
            if (!sector_write(unit, base + s, u->fat + s * BPS))
                return false;
    }
    for (int s = 0; s < ROOT_DIR_SECTORS; s++)
        if (!sector_write(unit, ROOT_DIR_START_LBA + s,
                          u->root_dir + s * BPS))
            return false;
    return true;
}

/* Mount: read boot sector, validate, cache FAT + root directory.
 * Precondition: media_probe has just returned MEDIA_OK (DSKCHG clear). */
static mount_result_t
fat12_mount(uint8_t unit)
{
    if (unit >= MAX_UNITS) return MOUNT_HW_FAULT;

    unit_state_t *u = &units[unit];
    u->mounted = false;

    uint8_t boot[BPS];
    if (!sector_read(unit, 0, boot))                     return MOUNT_HW_FAULT;
    if (boot[510] != 0x55 || boot[511] != 0xAA)          return MOUNT_BAD_FS;

    uint16_t bps = (uint16_t)boot[11] | ((uint16_t)boot[12] << 8);
    if (bps != BPS)                                      return MOUNT_BAD_FS;

    for (int s = 0; s < SECTORS_PER_FAT; s++)
        if (!sector_read(unit, FAT_START_LBA + s, u->fat + s * BPS))
            return MOUNT_HW_FAULT;
    for (int s = 0; s < ROOT_DIR_SECTORS; s++)
        if (!sector_read(unit, ROOT_DIR_START_LBA + s,
                         u->root_dir + s * BPS))
            return MOUNT_HW_FAULT;

    u->mounted = true;
    return MOUNT_OK;
}

/* Quick format: write a fresh BPB, two empty FATs with the media-
 * descriptor marker, and a zeroed root directory. The data area is
 * left untouched — the FAT makes every previous file invisible and
 * zeroing 1.4 MB of sectors would take ~30 s for no added safety. */
static bool
fat12_format(uint8_t unit)
{
    uint8_t sec[BPS];

    /* --- Boot sector / BPB --- */
    memset(sec, 0, sizeof(sec));
    sec[0] = 0xEB; sec[1] = 0x3C; sec[2] = 0x90;
    memcpy(sec + 3, "MAINDOB ", 8);
    sec[11] = (uint8_t)(BPS & 0xFF); sec[12] = (uint8_t)(BPS >> 8);
    sec[13] = 1;
    sec[14] = (uint8_t)RESERVED_SECTORS; sec[15] = 0;
    sec[16] = NUM_FATS;
    sec[17] = (uint8_t)(ROOT_DIR_ENTRIES & 0xFF);
    sec[18] = (uint8_t)(ROOT_DIR_ENTRIES >> 8);
    sec[19] = (uint8_t)(TOTAL_SECTORS & 0xFF);
    sec[20] = (uint8_t)(TOTAL_SECTORS >> 8);
    sec[21] = 0xF0;
    sec[22] = SECTORS_PER_FAT; sec[23] = 0;
    sec[24] = SECTORS_PER_TRACK; sec[25] = 0;
    sec[26] = HEADS; sec[27] = 0;
    sec[510] = 0x55; sec[511] = 0xAA;
    if (!sector_write(unit, 0, sec)) return false;

    /* --- Both FATs --- */
    memset(sec, 0, sizeof(sec));
    sec[0] = 0xF0; sec[1] = 0xFF; sec[2] = 0xFF;
    for (int copy = 0; copy < NUM_FATS; copy++)
    {
        uint32_t base = FAT_START_LBA + copy * SECTORS_PER_FAT;
        if (!sector_write(unit, base, sec)) return false;
        uint8_t zero[BPS];
        memset(zero, 0, sizeof(zero));
        for (int s = 1; s < SECTORS_PER_FAT; s++)
            if (!sector_write(unit, base + s, zero)) return false;
    }

    /* --- Root directory --- */
    {
        uint8_t zero[BPS];
        memset(zero, 0, sizeof(zero));
        for (int s = 0; s < ROOT_DIR_SECTORS; s++)
            if (!sector_write(unit, ROOT_DIR_START_LBA + s, zero))
                return false;
    }

    units[unit].mounted = false;
    return (fat12_mount(unit) == MOUNT_OK);
}

/*  *  L5 — dobfs IPC handlers
 *
 *  Every file operation begins with a single disk_change_check. If the
 *  media has been (or may have been) swapped since the last mount, we
 *  invalidate ALL cached state for that unit, mark every open file
 *  descriptor as stale, and return DOB_ERR_MEDIA_CHANGED without
 *  touching the hardware any further.
 */

/* Prelude helper — returns a dob_status_t if the op must abort, or
 * DOB_OK if the caller can proceed. */
static dob_status_t
op_prelude(uint8_t unit)
{
    if (unit >= MAX_UNITS)              return DOB_ERR_INVALID;
    if (!units[unit].mounted)           return DOB_ERR_INVALID;

    if (disk_change_check(unit) == DISK_CHANGED)
    {
        invalidate_fds_for_unit(unit);
        units[unit].mounted = false;
        return DOB_ERR_MEDIA_CHANGED;
    }
    return DOB_OK;
}

static dob_status_t
op_prelude_fd(int fd)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES) return DOB_ERR_INVALID;
    if (!fds[fd].in_use)                return DOB_ERR_INVALID;
    if (fds[fd].stale)                  return DOB_ERR_MEDIA_CHANGED;
    return op_prelude(fds[fd].unit);
}

static dob_status_t
op_open(dob_msg_t *msg, dob_msg_t *reply)
{
    if (!msg->payload) return DOB_ERR_INVALID;
    const char *path  = (const char *)msg->payload;
    uint32_t    flags = msg->arg0;

    uint8_t unit;
    const char *rel;
    if (!path_parse(path, &unit, &rel)) return DOB_ERR_INVALID;

    dob_status_t pre = op_prelude(unit);
    if (pre != DOB_OK)                  { fdc_motor_arm_off(); return pre; }

    while (*rel == '/') rel++;
    if (*rel == '\0')                   { fdc_motor_arm_off(); return DOB_ERR_INVALID; }

    char name83[11];
    if (!name_to_83(rel, name83))       { fdc_motor_arm_off(); return DOB_ERR_INVALID; }

    unit_state_t *u = &units[unit];
    int dir_idx = root_find(u, name83);

    if (dir_idx < 0)
    {
        if (!(flags & DOBFS_O_CREATE))  { fdc_motor_arm_off(); return DOB_ERR_NOT_FOUND; }
        dir_idx = root_alloc(u);
        if (dir_idx < 0)                { fdc_motor_arm_off(); return DOB_ERR_NO_SPACE; }
        fat_dirent_t *de = &((fat_dirent_t *)u->root_dir)[dir_idx];
        memset(de, 0, sizeof(*de));
        memcpy(de->name, name83,     8);
        memcpy(de->ext,  name83 + 8, 3);
        de->attr = ATTR_ARCHIVE;
    }

    fat_dirent_t *de = &((fat_dirent_t *)u->root_dir)[dir_idx];

    int fd = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++)
        if (!fds[i].in_use) { fd = i; break; }
    if (fd < 0)                         { fdc_motor_arm_off(); return DOB_ERR_NO_MEMORY; }

    fds[fd].in_use        = true;
    fds[fd].stale         = false;
    fds[fd].unit          = unit;
    fds[fd].dir_idx       = dir_idx;
    fds[fd].pos           = (flags & DOBFS_O_APPEND) ? de->size : 0;
    fds[fd].size          = de->size;
    fds[fd].flags         = (uint8_t)flags;
    fds[fd].first_cluster = de->cluster_lo;

    if (flags & DOBFS_O_TRUNC)
    {
        if (de->cluster_lo) fat12_free_chain(u, de->cluster_lo);
        de->cluster_lo        = 0;
        de->size              = 0;
        fds[fd].size          = 0;
        fds[fd].first_cluster = 0;
        fat12_flush_metadata(unit);
    }

    fdc_motor_arm_off();
    reply->arg0 = (uint32_t)fd;
    return DOB_OK;
}

static dob_status_t
op_read(dob_msg_t *msg, dob_msg_t *reply)
{
    int fd = (int)msg->arg0;
    dob_status_t pre = op_prelude_fd(fd);
    if (pre != DOB_OK) return pre;
    if (!(fds[fd].flags & DOBFS_O_READ)) return DOB_ERR_INVALID;

    static uint8_t outbuf[BPS];
    uint32_t want = msg->arg1;
    if (want > BPS) want = BPS;

    if (fds[fd].pos >= fds[fd].size) { reply->arg0 = 0; return DOB_OK; }
    uint32_t avail = fds[fd].size - fds[fd].pos;
    if (want > avail) want = avail;

    unit_state_t *u = &units[fds[fd].unit];
    uint16_t c = chain_seek(u, fds[fd].first_cluster, fds[fd].pos);
    if (c < 2)                          { fdc_motor_arm_off(); return DOB_ERR_INTERNAL; }

    uint8_t sec[BPS];
    if (!sector_read(fds[fd].unit, cluster_to_lba(c), sec))
    { fdc_motor_arm_off(); return DOB_ERR_HW_FAULT; }

    uint32_t off  = fds[fd].pos % BPS;
    uint32_t copy = (want < (BPS - off)) ? want : (BPS - off);
    memcpy(outbuf, sec + off, copy);

    fds[fd].pos += copy;
    fdc_motor_arm_off();

    reply->payload      = outbuf;
    reply->payload_size = copy;
    reply->arg0         = copy;
    return DOB_OK;
}

static dob_status_t
op_write(dob_msg_t *msg, dob_msg_t *reply)
{
    int fd = (int)msg->arg0;
    dob_status_t pre = op_prelude_fd(fd);
    if (pre != DOB_OK) return pre;
    if (!(fds[fd].flags & DOBFS_O_WRITE)) return DOB_ERR_INVALID;
    if (!msg->payload || msg->payload_size == 0) { reply->arg0 = 0; return DOB_OK; }

    unit_state_t *u = &units[fds[fd].unit];
    uint32_t left   = msg->payload_size;
    if (left > BPS) left = BPS;
    const uint8_t *src = (const uint8_t *)msg->payload;

    /* Locate or allocate the cluster that covers pos. */
    uint16_t c;
    if (fds[fd].first_cluster == 0)
    {
        c = fat12_alloc(u);
        if (c == 0)                     { fdc_motor_arm_off(); return DOB_ERR_NO_SPACE; }
        fds[fd].first_cluster = c;
    }
    else
    {
        c = fds[fd].first_cluster;
        uint32_t skip = fds[fd].pos / BPS;
        while (skip-- > 0)
        {
            uint16_t next = fat12_get(u, c);
            if (next >= 2 && next < FAT12_BAD) { c = next; continue; }
            uint16_t nc = fat12_alloc(u);
            if (nc == 0)                { fdc_motor_arm_off(); return DOB_ERR_NO_SPACE; }
            fat12_set(u, c, nc);
            c = nc;
        }
    }

    /* Read-modify-write to preserve untouched bytes in the target
     * sector. Two sector I/Os for a partial write — acceptable given
     * the mechanical cost of a floppy access. */
    uint8_t sec[BPS];
    if (!sector_read(fds[fd].unit, cluster_to_lba(c), sec))
    { fdc_motor_arm_off(); return DOB_ERR_HW_FAULT; }

    uint32_t off = fds[fd].pos % BPS;
    if (off + left > BPS) left = BPS - off;
    memcpy(sec + off, src, left);

    if (!sector_write(fds[fd].unit, cluster_to_lba(c), sec))
    { fdc_motor_arm_off(); return DOB_ERR_HW_FAULT; }

    fds[fd].pos += left;
    if (fds[fd].pos > fds[fd].size) fds[fd].size = fds[fd].pos;

    /* Write-through metadata: update the directory entry and flush so
     * a sudden eject doesn't lose the FAT update. */
    fat_dirent_t *de = &((fat_dirent_t *)u->root_dir)[fds[fd].dir_idx];
    de->cluster_lo = fds[fd].first_cluster;
    de->size       = fds[fd].size;
    fat12_flush_metadata(fds[fd].unit);

    fdc_motor_arm_off();
    reply->arg0 = left;
    return DOB_OK;
}

static dob_status_t
op_close(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    int fd = (int)msg->arg0;
    if (fd < 0 || fd >= MAX_OPEN_FILES || !fds[fd].in_use) return DOB_ERR_INVALID;
    fds[fd].in_use = false;
    fds[fd].stale  = false;
    return DOB_OK;
}

/* DOBFS_SEEK — reposition fds[fd].pos.
 *
 * The floppy read/write path re-derives the target cluster from
 * fds[fd].pos on every call (chain_seek at op_read + position walk
 * at op_write). So seeking only needs to update the scalar — no
 * cluster-chain pre-walk required, no cache to invalidate. */
static dob_status_t
op_seek(dob_msg_t *msg, dob_msg_t *reply)
{
    int fd = (int)msg->arg0;
    if (fd < 0 || fd >= MAX_OPEN_FILES || !fds[fd].in_use) return DOB_ERR_INVALID;

    uint32_t uoff   = msg->arg1;
    uint32_t whence = msg->arg2;
    int32_t  soff   = (int32_t)uoff;
    int64_t  np     = 0;

    switch (whence)
    {
        case DOBFS_SEEK_SET: np = (int64_t)uoff;                           break;
        case DOBFS_SEEK_CUR: np = (int64_t)fds[fd].pos  + (int64_t)soff;   break;
        case DOBFS_SEEK_END: np = (int64_t)fds[fd].size + (int64_t)soff;   break;
        default:             return DOB_ERR_INVALID;
    }

    if (np < 0) return DOB_ERR_INVALID;

    if (!(fds[fd].flags & DOBFS_O_WRITE) && np > (int64_t)fds[fd].size)
        np = (int64_t)fds[fd].size;
    if (np > (int64_t)0xFFFFFFFFu) return DOB_ERR_INVALID;

    fds[fd].pos = (uint32_t)np;
    reply->arg0 = fds[fd].pos;
    return DOB_OK;
}

static dob_status_t
op_stat(dob_msg_t *msg, dob_msg_t *reply)
{
    if (!msg->payload) return DOB_ERR_INVALID;
    const char *path = (const char *)msg->payload;
    uint8_t unit;
    const char *rel;
    if (!path_parse(path, &unit, &rel)) return DOB_ERR_INVALID;

    dob_status_t pre = op_prelude(unit);
    if (pre != DOB_OK) return pre;

    while (*rel == '/') rel++;
    if (*rel == '\0')
    {
        reply->arg0 = 0;
        reply->arg1 = 1;
        return DOB_OK;
    }
    char name83[11];
    if (!name_to_83(rel, name83)) return DOB_ERR_INVALID;
    int idx = root_find(&units[unit], name83);
    if (idx < 0) return DOB_ERR_NOT_FOUND;
    fat_dirent_t *de = &((fat_dirent_t *)units[unit].root_dir)[idx];
    reply->arg0 = de->size;
    reply->arg1 = (de->attr & ATTR_DIRECTORY) ? 1 : 0;
    return DOB_OK;
}

static dob_status_t
op_readdir(dob_msg_t *msg, dob_msg_t *reply)
{
    if (!msg->payload) return DOB_ERR_INVALID;
    const char *path = (const char *)msg->payload;
    uint8_t unit;
    const char *rel;
    if (!path_parse(path, &unit, &rel)) return DOB_ERR_INVALID;

    dob_status_t pre = op_prelude(unit);
    if (pre != DOB_OK) return pre;

    static char outbuf[2048];
    int o = 0;
    for (int i = 0; i < ROOT_DIR_ENTRIES && o < (int)sizeof(outbuf) - 16; i++)
    {
        fat_dirent_t *de = &((fat_dirent_t *)units[unit].root_dir)[i];
        if ((uint8_t)de->name[0] == 0x00) break;
        if ((uint8_t)de->name[0] == 0xE5) continue;
        if (de->attr == ATTR_LFN)         continue;
        if (de->attr & ATTR_VOLUME_ID)    continue;
        char name[13];
        name_from_83(de, name);
        int n = (int)strlen(name);
        if (o + n + 1 >= (int)sizeof(outbuf)) break;
        memcpy(outbuf + o, name, n);
        o += n;
        outbuf[o++] = '\n';
    }
    outbuf[o] = '\0';
    reply->payload      = outbuf;
    reply->payload_size = (uint32_t)(o + 1);
    return DOB_OK;
}

static dob_status_t
op_unlink(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    if (!msg->payload) return DOB_ERR_INVALID;
    const char *path = (const char *)msg->payload;
    uint8_t unit;
    const char *rel;
    if (!path_parse(path, &unit, &rel)) return DOB_ERR_INVALID;

    dob_status_t pre = op_prelude(unit);
    if (pre != DOB_OK) return pre;

    while (*rel == '/') rel++;
    char name83[11];
    if (!name_to_83(rel, name83)) return DOB_ERR_INVALID;
    int idx = root_find(&units[unit], name83);
    if (idx < 0) return DOB_ERR_NOT_FOUND;

    fat_dirent_t *de = &((fat_dirent_t *)units[unit].root_dir)[idx];
    if (de->cluster_lo) fat12_free_chain(&units[unit], de->cluster_lo);
    de->name[0] = (char)0xE5;
    fat12_flush_metadata(unit);

    fdc_motor_arm_off();
    return DOB_OK;
}

static dob_status_t
op_unsupported(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)msg; (void)reply;
    return DOB_ERR_INVALID;
}

/* L6 — Custom IPC handlers */

/* Extract the unit number from a NUL-terminated payload ("0" or "1"). */
static uint8_t
payload_unit(const dob_msg_t *msg)
{
    if (!msg->payload || msg->payload_size == 0) return 0;
    const char *p = (const char *)msg->payload;
    if (*p >= '0' && *p <= '9') return (uint8_t)(*p - '0');
    return 0;
}

static dob_status_t
op_probe_media(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    uint8_t unit = payload_unit(msg);
    if (unit >= MAX_UNITS) return DOB_ERR_INVALID;

    /* Any open fd for this unit is about to become stale no matter
     * what — the act of probing is also the act of (potentially)
     * mounting a fresh disk. */
    invalidate_fds_for_unit(unit);
    units[unit].mounted = false;

    media_probe_t mp = media_probe(unit);
    if (mp == MEDIA_HW_FAULT)           { fdc_motor_arm_off(); return DOB_ERR_HW_FAULT; }
    if (mp == MEDIA_ABSENT)             { fdc_motor_arm_off(); return DOB_ERR_NO_MEDIA; }

    mount_result_t mr = fat12_mount(unit);
    if (mr == MOUNT_HW_FAULT)           { fdc_motor_arm_off(); return DOB_ERR_HW_FAULT; }
    if (mr == MOUNT_BAD_FS)             { fdc_motor_arm_off(); return DOB_ERR_BAD_FS;   }
    if (mr == MOUNT_NO_MEDIA)           { fdc_motor_arm_off(); return DOB_ERR_NO_MEDIA; }

    /* Surface the file explorer mounted on this unit. The hijack
     * target port arrived as arg0 from the DAS ipc_call primitive,
     * which forwarded it from ICON_ACTIVATED — 0 (default) means
     * spawn a satellite, !=0 routes the mount to a specific window. */
    char root[8];
    root[0] = '/'; root[1] = 'u'; root[2] = (char)('0' + unit); root[3] = '\0';
    (void)dobfiles_OpenMount("floppy", root, msg->arg0);

    fdc_motor_arm_off();
    return DOB_OK;
}

static dob_status_t
op_format(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    uint8_t unit = payload_unit(msg);
    if (unit >= MAX_UNITS) return DOB_ERR_INVALID;

    invalidate_fds_for_unit(unit);
    units[unit].mounted = false;

    if (!ensure_fdc_init())             { fdc_motor_arm_off(); return DOB_ERR_HW_FAULT; }

    media_probe_t mp = media_probe(unit);
    if (mp == MEDIA_HW_FAULT)           { fdc_motor_arm_off(); return DOB_ERR_HW_FAULT; }
    if (mp == MEDIA_ABSENT)             { fdc_motor_arm_off(); return DOB_ERR_NO_MEDIA; }

    if (!fat12_format(unit))            { fdc_motor_arm_off(); return DOB_ERR_HW_FAULT; }

    fdc_motor_arm_off();
    return DOB_OK;
}

/* Subscribe the caller's port for unmount notifications on `unit`.
 * Deduplicates: if the port is already registered it's a no-op. If
 * the per-unit table is full the request is silently dropped — 4
 * slots are more than anyone should need, and the worst case is a
 * satellite window that won't auto-close (user can still close it
 * manually). */
static void
unmount_sub_add(uint8_t unit, uint32_t port)
{
    if (unit >= MAX_UNITS || port == 0) return;
    for (int i = 0; i < MAX_UNMOUNT_SUBS; i++)
        if (unmount_subs[unit][i] == port) return;
    for (int i = 0; i < MAX_UNMOUNT_SUBS; i++)
        if (unmount_subs[unit][i] == 0) { unmount_subs[unit][i] = port; return; }
}

/* Fire DOBFS_UNMOUNT_NOTIFY at every subscriber of `unit` and clear
 * the list. Fire-and-forget: the satellite tears down its window and
 * exits, there is nothing to learn from a reply. libdobui dispatches
 * non-GUI posts to event_request just like sync requests. */
static void
unmount_sub_notify_all(uint8_t unit)
{
    if (unit >= MAX_UNITS) return;
    for (int i = 0; i < MAX_UNMOUNT_SUBS; i++)
    {
        uint32_t port = unmount_subs[unit][i];
        if (port == 0) continue;
        dob_msg_t m = {0};
        m.code = DOBFS_UNMOUNT_NOTIFY;
        m.arg0 = unit;
        (void)dob_ipc_post(port, &m);
        unmount_subs[unit][i] = 0;
    }
}

/* User-initiated unmount. The DAS "Espelli" menu entry calls this.
 * We invalidate every cached piece of state for the unit, kill all
 * open file descriptors pointing at it, notify any subscribed
 * viewer (e.g. the DobFiles window surfaced at mount time) so it
 * can close itself, and stop the motor right now — no deferred
 * timer, because by definition the user is about to physically
 * remove the disk. The driver process itself stays alive: the other
 * drive (unit 1 or 0) may still be in use, and even if both are
 * idle, a fresh probe_media on either one is only a few ms away
 * and re-spawning would be wasted work. */
static dob_status_t
op_eject(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    uint8_t unit = payload_unit(msg);
    if (unit >= MAX_UNITS) return DOB_ERR_INVALID;

    invalidate_fds_for_unit(unit);
    units[unit].mounted = false;

    unmount_sub_notify_all(unit);

    fdc_motor_timer_cancel();
    fdc_motor_off_now();
    return DOB_OK;
}

/* DOBFS_SUBSCRIBE_UNMOUNT handler. Payload-less; unit comes from
 * arg1 and the caller's notify port from arg0. We do not reply
 * with a fresh dob_status — the DAS/server framework will send
 * back reply.code=DOB_OK automatically, which is fine because
 * the caller uses dob_ipc_call for this and just wants the ack. */
static dob_status_t
op_subscribe_unmount(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    uint32_t port = msg->arg0;
    uint8_t  unit = (uint8_t)(msg->arg1 & 0xFF);
    if (unit >= MAX_UNITS) return DOB_ERR_INVALID;
    unmount_sub_add(unit, port);
    return DOB_OK;
}

/* L7 — Dispatcher + bootstrap */

static dob_status_t
floppy_dispatch(dob_msg_t *msg, dob_msg_t *reply)
{
    /* Hotplug eject handshake — cancel motor, drop power, exit clean. */
    if (dob_driver_is_detach(msg))
    {
        fdc_motor_timer_cancel();
        fdc_motor_off_now();
        dob_driver_released();
        _exit(0);
    }

    /* Timer expiries — the only non-IPC event we handle here is the
     * deferred motor-off. Anything else is ignored. */
    if (msg->code == MSG_TIMER)
    {
        fdc_motor_off_now();
        return DOB_OK;
    }

    switch (msg->code)
    {
        case DOBFS_OPEN:         return op_open       (msg, reply);
        case DOBFS_READ:         return op_read       (msg, reply);
        case DOBFS_WRITE:        return op_write      (msg, reply);
        case DOBFS_CLOSE:        return op_close      (msg, reply);
        case DOBFS_SEEK:         return op_seek       (msg, reply);
        case DOBFS_STAT:         return op_stat       (msg, reply);
        case DOBFS_READDIR:      return op_readdir    (msg, reply);
        case DOBFS_UNLINK:       return op_unlink     (msg, reply);
        case DOBFS_MKDIR:        return op_unsupported(msg, reply);
        case DOBFS_RENAME:       return op_unsupported(msg, reply);
        case DOBFS_FORMAT:       return op_format     (msg, reply);
        case DOBFS_SUBSCRIBE_UNMOUNT: return op_subscribe_unmount(msg, reply);
        case FLOPPY_PROBE_MEDIA: return op_probe_media(msg, reply);
        case FLOPPY_EJECT:       return op_eject      (msg, reply);
        default:                 return DOB_ERR_INVALID;
    }
}

static int
floppy_driver_run(void)
{
    /* Refuse to run as a second instance — one process owns the shared
     * FDC, period. */
    if (dob_registry_find("floppy"))
        return 0;

    /* Create the IRQ notification port and hook IRQ6. */
    irq_port = (uint32_t)port_create();
    (void)irq_register(FDC_IRQ_NUM, irq_port);

    /* Zero all state. */
    memset(units, 0, sizeof(units));
    memset(fds,   0, sizeof(fds));
    memset(unmount_subs, 0, sizeof(unmount_subs));
    motor_timer_id  = -1;
    fdc_initialized = false;

    /* Allocate the shared 512-byte DMA buffer. We ask for a full page
     * (4 KB); the kernel DMA zone gives us memory <16 MB, and a single
     * page cannot cross a 64 KB boundary — both ISA DMA constraints
     * satisfied for free. */
    dma_buf = (uint8_t *)dma_alloc(4096, &dma_buf_phys);
    if (!dma_buf)
    {
        debug_print("[floppy] FATAL: dma_alloc failed\n");
        return 1;
    }

    /* Register the service BEFORE touching the hardware. If the FDC
     * is dead, the DAS still sees a healthy 'floppy' service and gets
     * a clean DOB_ERR_HW_FAULT from the first probe_media instead of
     * an opaque 'driver crashed'. */
    dob_server_init("floppy");
    dob_server_register(floppy_dispatch);
    dob_server_loop();
    return 0;
}

int
main(void)
{
    return floppy_driver_run();
}
