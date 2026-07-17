/* MainDOB Partition Library — MBR parse / serialise + subdevice scanner.
 *
 * Two layers:
 *
 *   Layer A — pure MBR operations (in-memory, no I/O).
 *     partition_mbr_parse()     : 512-byte buffer  -> mbr_table_t
 *     partition_mbr_serialize() : mbr_table_t      -> 512-byte buffer
 *     partition_mbr_init_empty(): zero a new disk's MBR with signature
 *
 *   Layer B — partition scanner that talks to hotplug.
 *     partition_scan_announce(): read sector 0 via a caller-supplied
 *       reader callback, parse, then emit HOTPLUG_SUBDEVICE_APPEARED /
 *       _GONE events for the deltas vs the last scan of the same
 *       (provider_service, native_selector). Library tracks the
 *       previously-announced set per-disk so the caller does not have
 *       to. Safe to call from boot init or from a RESCAN_PARTITIONS
 *       opcode handler.
 *
 * MBR semantics covered here are deliberately minimal: 4 primary
 * partitions, no extended/logical chain. Sufficient for the live-
 * install flow and DobDisk's v1 capability list.
 *
 * Filesystem awareness: today only FAT32 (types 0x0B / 0x0C) is
 * surfaced as a "volume" subdevice. The class_code/subclass tagging
 * is provisional (0xDB:0x01 = MainDOB filesystem volume / FAT32) and
 * will move to a dedicated volume_fs field on sub_device_t in step 6
 * of the disk-utility work. The DAS file that turns these into
 * desktop icons lands in step 7.
 */

#ifndef MAINDOB_DOB_PARTITION_H
#define MAINDOB_DOB_PARTITION_H

#include <dob/types.h>

/* === MBR constants === */
#define MBR_MAX_PRIMARY     4
#define MBR_SECTOR_LBA      0
#define MBR_SECTOR_SIZE   512
#define MBR_PT_OFFSET     446      /* Byte offset of the partition table */
#define MBR_SIG_LO        0x55
#define MBR_SIG_HI        0xAA

/* Partition type bytes we care about. */
#define MBR_TYPE_EMPTY        0x00
#define MBR_TYPE_EXFAT        0x07
#define MBR_TYPE_FAT32_CHS    0x0B
#define MBR_TYPE_FAT32_LBA    0x0C

/* Note: the subdevice tag for "MainDOB-recognised filesystem volume"
 * lives in <dob/hotplug_events.h> as VOLUME_FS_FAT32 (and friends).
 * Older revisions of this header exported provisional VOLUME_SUBDEV_*
 * constants here; those have moved to a proper field on sub_device_t
 * (sub.volume_fs) since step 6 of the disk-utility work. */

/* === Layer A: pure MBR operations === */

typedef struct
{
    uint8_t  active;       /* 0 or 0x80 (bootable flag preserved verbatim) */
    uint8_t  type;         /* MBR partition type byte */
    uint32_t start_lba;    /* Starting LBA */
    uint32_t sectors;      /* Sector count */
} mbr_partition_t;

typedef struct
{
    bool             valid_signature;            /* True iff bytes 510..511 = 0x55 0xAA */
    mbr_partition_t  entries[MBR_MAX_PRIMARY];
} mbr_table_t;

/* Parse a 512-byte buffer into mbr_table_t. Empty / invalid entries
 * surface as type=0, sectors=0 — caller iterates and filters. */
void partition_mbr_parse(const void *sector_buf, mbr_table_t *out);

/* Serialise mbr_table_t back into sector_buf. Bytes 0..445 (boot
 * code) and any unrecognised bytes are preserved as already in
 * sector_buf — caller passes the original sector to retain whatever
 * boot record may be there. Bytes 446..509 are overwritten with the
 * table, 510..511 with 0x55 0xAA. */
void partition_mbr_serialize(const mbr_table_t *table, void *sector_buf);

/* Zero a freshly-allocated 512-byte buffer and write only the MBR
 * signature. Use when initialising a brand-new disk that has no
 * boot record yet. */
void partition_mbr_init_empty(void *sector_buf);

/* Helper: returns true if the partition type byte is a FAT32
 * variant we know how to mount. */
bool partition_type_is_fat32(uint8_t type);
bool partition_type_is_exfat(uint8_t type);

/* === Layer B: subdevice scanner === */

/* Caller-supplied sector reader. Returns true on success.
 * ctx is whatever the caller stashed in partition_scan_ctx_t.ctx. */
typedef bool (*partition_read_sector_fn)(void *ctx, uint32_t lba, void *out);

typedef struct
{
    /* I/O hook — usually wraps the driver's own internal read. */
    partition_read_sector_fn read_sector;
    void                    *ctx;

    /* Identity for the hotplug subdevice protocol. provider_service
     * must match the driver's registry name ("ata" or "ahci"). The
     * native_selector is the driver-internal disk identifier
     * (slot 0..3 for ATA, port number for AHCI). */
    const char *provider_service;
    uint32_t    native_selector;
} partition_scan_ctx_t;

/* Read MBR via ctx->read_sector, diff against the last announced set
 * for (provider_service, native_selector), and fire SUBDEVICE_APPEARED
 * for new FAT32 partitions and SUBDEVICE_GONE for removed ones.
 *
 * First call for a given (service, selector) emits APPEARED for every
 * present FAT32 partition. Subsequent calls emit only deltas.
 *
 * Returns true on success. Failures (sector read error, hotplug not
 * registered yet) return false and leave internal state unchanged. */
bool partition_scan_announce(const partition_scan_ctx_t *ctx);

/* Reset the library's internal "last announced" state for a single
 * disk. Used on driver detach so the per-disk slot is freed without
 * spurious GONE emissions (the bubbles are torn down by hotplug's
 * sudden-removal path anyway). */
void partition_scan_forget(const char *provider_service,
                           uint32_t    native_selector);

#endif /* MAINDOB_DOB_PARTITION_H */
