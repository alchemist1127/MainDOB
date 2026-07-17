/* MainDOB Block Layer — unified disk enumeration and I/O.
 *
 * Abstracts over the underlying disk drivers (ata, ahci, future
 * USB-MSC) so clients see one flat list of disks numbered 0..N-1 and
 * issue sector reads/writes by disk index regardless of which driver
 * serves the device. ATA's arg-layout (arg2 = disk slot) and AHCI's
 * (arg0 = port) are hidden inside per-driver adapters.
 *
 * Typical use:
 *
 *   int n = block_enumerate();
 *   for (int i = 0; i < n; i++)
 *   {
 *       const block_disk_t *d = block_get(i);
 *       printf("disk %d: %s — %llu sectors\n",
 *              i, d->model, (unsigned long long)d->total_sectors);
 *   }
 *
 *   uint8_t sector0[BLOCK_SECTOR_SIZE];
 *   if (block_read(0, 0, 1, sector0)) { ... }
 *
 * Re-enumeration:
 *   block_enumerate() can be called again — it rebuilds the table.
 *   Disk indices are NOT stable across enumerations if hardware
 *   appeared or disappeared. Hold a reference only across one
 *   enumeration cycle.
 *
 * Scope:
 *   This library only surfaces HDDs and SSDs. Optical drives are
 *   served by the dedicated cdrom driver and don't appear here.
 *
 * Concurrency:
 *   Not thread-safe — the internal table is a process-wide global.
 *   Intended for single-threaded utility processes (DobDisk,
 *   DobLiveSetup, per-mount DobFileSystem instances). Add external
 *   serialization if you call it from multiple threads.
 */

#ifndef MAINDOB_DOB_BLOCK_H
#define MAINDOB_DOB_BLOCK_H

#include <dob/types.h>

#define BLOCK_MAX_DISKS    32   /* Hard cap across all drivers */
#define BLOCK_SECTOR_SIZE 512

typedef enum
{
    BLOCK_BUS_NONE = 0,
    BLOCK_BUS_ATA  = 1,   /* Parallel ATA / IDE */
    BLOCK_BUS_SATA = 2,   /* SATA via AHCI */
    BLOCK_BUS_USB  = 3,   /* USB mass storage (usbms_<port> services) */
} block_bus_t;

typedef enum
{
    BLOCK_KIND_UNKNOWN = 0,
    BLOCK_KIND_HDD     = 1,
    BLOCK_KIND_SSD     = 2,
} block_kind_t;

/* Opaque driver-routing handle. Clients must not inspect it. */
struct block_driver_class;

typedef struct
{
    int          index;             /* Position in the current enumeration */
    block_bus_t  bus;
    block_kind_t kind;
    bool         trim_supported;    /* Drive advertises DATA SET MANAGEMENT TRIM.
                                     * Execution also needs a DMA path on the
                                     * serving driver (see block_trim). */
    uint64_t     total_sectors;     /* Capacity in 512-byte sectors */
    uint32_t     sector_size;       /* Always BLOCK_SECTOR_SIZE today */
    uint32_t     native_sector_size; /* Device physical sector size (512 or
                                      * 4096). The block layer always reads/
                                      * writes in 512-byte units; a formatter
                                      * may align a new volume to this. */
    char         model[64];         /* Drive model, NUL-terminated */

    /* Driver routing — opaque to the client, used by block_read/write. */
    const struct block_driver_class *driver_class;
    uint32_t     native_selector;
} block_disk_t;

/* (Re)build the disk table by querying every known disk driver
 * service. Drivers not currently registered in the registry are
 * silently skipped — their absence isn't an error. Returns the
 * number of disks found (0..BLOCK_MAX_DISKS). */
int block_enumerate(void);

/* Number of disks in the current table. 0 before block_enumerate(). */
int block_count(void);

/* Returns a pointer to the i-th disk entry, or NULL on out-of-range.
 * The pointer is valid until the next block_enumerate(). */
const block_disk_t *block_get(int i);

/* Traduce il handle di routing OPACO del disco `i` nella coppia
 * (nome servizio, selector NATIVO del driver) — l'unica forma che i
 * processi terzi possono passare a uno spawn di DobFileSystem
 * --mount provider=<service> selector=<sel>. E' il layer block, unico
 * proprietario dell'encoding delle istanze (per SATA i bit alti del
 * selector portano l'indice di istanza: su un secondo controller il
 * servizio giusto e' "ahci_N", NON "ahci" — hardcodare il nome e
 * passare il selector codificato manda le letture a un'istanza
 * sbagliata con una porta inesistente). false su indice fuori range. */
bool block_provider_binding(int i, char *service_out, uint32_t cap,
                            uint32_t *native_selector_out);

/* Read `count` sectors starting at `lba` from disk `i` into `buf`.
 * `buf` must hold at least count*BLOCK_SECTOR_SIZE bytes. Bounds
 * checked against disk capacity. */
bool block_read(int i, uint64_t lba, uint32_t count, void *buf);

/* Write `count` sectors starting at `lba` on disk `i` from `buf`. */
bool block_write(int i, uint64_t lba, uint32_t count, const void *buf);

/* Ask the underlying driver to re-scan partitions for disk `i` and
 * emit fresh HOTPLUG_SUBDEVICE_APPEARED / GONE events. Use after
 * committing an MBR write so the desktop icons update. The actual
 * partition-scan logic lives in the driver process (step 4 wires
 * it via libdob/dob/partition); today the driver opcode is a stub
 * that returns DOB_OK without doing anything. */
bool block_rescan_partitions(int i);

/* Commit disk `i`'s volatile write cache to media (SCSI SYNCHRONIZE CACHE
 * on USB). Returns true (no-op) for buses with no flush path. Call after
 * writing a fresh volume so a pulled USB stick keeps it. */
bool block_flush(int i);

/* Read SMART data from disk `i` into a 512-byte caller-provided
 * buffer. Returns false on any driver-level failure (incl. SMART
 * unsupported by the drive). The layout is the raw ATA-7 "SMART
 * READ DATA" structure: vendor attributes start at byte 2, twelve
 * bytes each (id, flags, current, worst, raw[6], reserved), with a
 * zero id marking an unused slot. */
bool block_get_smart(int i, void *buf512);

/* Diagnostic variant: identical to block_get_smart, but on failure fills
 * diag[4] with the serving driver's last-command diagnostics —
 *   diag[0] reason  (BLOCK_SMARTDIAG_*)
 *   diag[1] PxTFD   (AHCI task-file: STS in bits 7:0, ATA ERR in 15:8)
 *   diag[2] PxSERR
 *   diag[3] step    (1 = READ DATA, 2 = ENABLE OPERATIONS, 3 = READ retry)
 * A bus with no diag path (ata/usbms) zero-fills diag. diag may be NULL.
 * Built for log-less physical machines: the caller can render these in
 * its error UI and the screenshot becomes the bug report. */
#define BLOCK_SMARTDIAG_NONE     0  /* no info (transport, or non-AHCI bus) */
#define BLOCK_SMARTDIAG_BUSY     1  /* command slot contended */
#define BLOCK_SMARTDIAG_TIMEOUT  2  /* drive never completed the command */
#define BLOCK_SMARTDIAG_TFD_ERR  3  /* drive ABORTED (see ATA ERR byte) */
bool block_get_smart_diag(int i, void *buf512, uint32_t diag[4]);

/* Issue DATA SET MANAGEMENT TRIM over [lba, lba+count) on disk `i`,
 * telling the SSD those 512-byte sectors are no longer in use. Bounds
 * checked against capacity; a large range is split internally into the
 * per-command limit (65536 sectors / range entry). Returns false if the
 * bus/driver has no TRIM path (block_disk_t.trim_supported false, or the
 * serving driver lacks a DMA path for it) or on any driver-level error.
 *
 * CALLER CONTRACT: only ever pass UNALLOCATED ranges (free space) unless
 * you intend to discard the data — TRIM is destructive for the sectors
 * it covers. */
bool block_trim(int i, uint64_t lba, uint32_t count);

#endif /* MAINDOB_DOB_BLOCK_H */
