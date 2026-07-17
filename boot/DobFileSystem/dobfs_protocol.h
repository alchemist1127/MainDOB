/* DobFileSystem IPC Protocol
 *
 * Shared between server (main.c) and client stub (DobFileSystem_stub.c).
 * All IPC codes, flags, and helper macros live here so that changes
 * to the protocol are automatically consistent on both sides. */

#ifndef MAINDOB_DOBFS_PROTOCOL_H
#define MAINDOB_DOBFS_PROTOCOL_H

#include <dob/types.h>

/* IPC Operation Codes */

#define DOBFS_OPEN              1
#define DOBFS_READ              2
#define DOBFS_WRITE             3
#define DOBFS_CLOSE             4
#define DOBFS_STAT              5
#define DOBFS_READDIR           6
#define DOBFS_MKDIR             7
#define DOBFS_UNLINK            8
#define DOBFS_SANDBOX_CHECK     9
#define DOBFS_RENAME            12
#define DOBFS_FORMAT            13
#define DOBFS_REMOUNT           14
#define DOBFS_ENSURE_LAYOUT     15

/* Multi-instance mount queries (disk-utility step 5).
 *
 * GET_MOUNTED: returns dobfs_mounted_info_t describing what this
 *   instance is bound to — disk provider, native selector, partition
 *   start LBA, partition size, FS type, mount id. Used by DobDisk's
 *   "no-format-on-active" check (it queries every dobfs_* instance
 *   to find which disk+lba pair to protect from being repartitioned).
 *
 * DF: returns dobfs_df_info_t with capacity / used / free byte
 *   totals plus cluster geometry. Used by DobDisk to render the
 *   per-partition occupation bars.
 *
 * OPEN_VIEW: instructs this instance to surface a DobFiles window
 *   at its mount root. No reply payload. Called by the DAS action
 *   for partition icons, mirroring the floppy driver's pattern
 *   (the mount process itself calls dobfiles_OpenMount via EPS;
 *   the DAS doesn't need to know about DobFiles). */
#define DOBFS_GET_MOUNTED       19
#define DOBFS_DF                20
#define DOBFS_OPEN_VIEW         21
#define DOBFS_SHUTDOWN          22  /* secondary mounts only: flush all,
                                     * UNMOUNT_NOTIFY to subscribers, exit.
                                     * Posted by the block provider (e.g.
                                     * usbms) when its medium disappears. */

/* Removable-media mount lifecycle.
 *
 * Generic protocol: any filesystem service that can disappear under a
 * mounted view (floppy, USB, CD) implements these two opcodes.
 * A client that has switched its local dobfs routing to `service` and
 * wants to be told when that mount goes away calls SUBSCRIBE_UNMOUNT
 * once, passing its own IPC port in arg0. The service stores the port
 * in a small per-unit subscriber list. When the service is asked to
 * unmount (eject) it posts UNMOUNT_NOTIFY to every stored port and
 * clears the list. The subscriber handles the notify by closing any
 * view bound to that mount. Fire-and-forget — no reply expected. */
#define DOBFS_SUBSCRIBE_UNMOUNT 16
#define DOBFS_UNMOUNT_NOTIFY    17

/* Reposition an open fd's internal cursor.
 *
 *   arg0 = fd
 *   arg1 = offset low 32 bits
 *   arg2 = whence (DOBFS_SEEK_SET | _CUR | _END)
 *   arg3 = offset high 32 bits
 *
 * The offset is a 64-bit value carried as (arg3:arg1). For SEEK_CUR/END
 * it is a signed delta (arg3 carries the sign for negative deltas); for
 * SEEK_SET it is an absolute, non-negative position. Older callers that
 * only seek within <=4 GB leave arg3 = 0, which is the correct high word.
 *
 * Reply:
 *   reply.arg0 = new absolute position, low 32 bits
 *   reply.arg1 = new absolute position, high 32 bits
 *   reply.code = DOB_ERR_INVALID if fd is bad or whence unknown
 *
 * Semantics:
 *   - SEEK_SET: new_pos = offset
 *   - SEEK_CUR: new_pos = current + offset
 *   - SEEK_END: new_pos = file_size + offset
 *   - Position is clamped to [0, file_size] on read-only fds; on
 *     write-capable fds seeking past end is allowed (the next write
 *     extends the file, matching POSIX). The FAT32 backend additionally
 *     rejects positions beyond 4 GB-1; exFAT fds (Phase 3) use a 64-bit
 *     seek path. */
#define DOBFS_SEEK              18

#define DOBFS_SEEK_SET          0
#define DOBFS_SEEK_CUR          1
#define DOBFS_SEEK_END          2

/* Open Flags (shared between header and stub) */

#define DOBFS_O_READ    (1 << 0)
#define DOBFS_O_WRITE   (1 << 1)
#define DOBFS_O_CREATE  (1 << 2)
#define DOBFS_O_APPEND  (1 << 3)
#define DOBFS_O_TRUNC   (1 << 4)

/* === Reply payload wire structs === */

/* GET_MOUNTED reply payload. provider is the disk-driver service name
 * ("ata" or "ahci"), selector is its native disk index, partition_lba
 * is the start of the mounted partition, partition_sectors is its
 * size in 512-byte sectors, fs_type is "fat32" (or future fs name),
 * mount_id is the id passed in --mount or 0 for the root mount. */
typedef struct
{
    char     provider[16];
    uint32_t selector;
    uint32_t partition_lba;
    uint32_t partition_sectors;
    char     fs_type[8];
    uint32_t mount_id;
    uint8_t  is_root_mount;       /* 1 = the boot-time root mount */
    uint8_t  _pad[3];
} dobfs_mounted_info_t;

/* DF reply payload. */
typedef struct
{
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint32_t cluster_size_bytes;
    uint32_t total_clusters;
    uint32_t used_clusters;
} dobfs_df_info_t;

/* Helper Macros */

/* Extract 32-bit cluster number from a FAT32 directory entry.
 * Works with both fat32_dirent_t (server) and any struct that
 * has first_cluster_hi / first_cluster_lo fields. */
#define DE_CLUSTER(de) \
    (((uint32_t)(de).first_cluster_hi << 16) | (uint16_t)(de).first_cluster_lo)

/* Variant for pointer-to-dirent */
#define DE_CLUSTER_P(de) \
    (((uint32_t)(de)->first_cluster_hi << 16) | (uint16_t)(de)->first_cluster_lo)

#endif /* MAINDOB_DOBFS_PROTOCOL_H */
