/* MainDOB Filesystem Operations — pluggable formatter interface.
 *
 * Today fat32 only; the indirection exists so that when DobFS the
 * native filesystem lands, adding a second fs_ops_t entry is a 50-line
 * change here and a new fs_ops_for_mbr_type() lookup result. DobDisk
 * and DobLiveSetup talk to this header, never to fat32_ops.c directly.
 *
 * Each operation takes a `block_index` referring to a disk in the
 * block layer (see libdob/dob/block.h). Partition coordinates are
 * given as (start_lba, sectors) — the formatter writes within that
 * window, the caller has already laid out the partition entry in
 * the MBR via libdob/dob/partition.h.
 *
 * Usage / "df" queries do NOT live here. For a mounted partition,
 * DobDisk and friends query DOBFS_DF on the dobfs_<token> instance.
 * For an unmounted partition, capacity from the MBR is the only
 * available figure — there's no "used %" without mounting. Avoiding
 * unmounted scans keeps fs_ops minimal and avoids re-implementing
 * FAT traversal outside DobFileSystem.
 */

#ifndef MAINDOB_DOB_FS_OPS_H
#define MAINDOB_DOB_FS_OPS_H

#include <dob/types.h>

/* Caller options for mkfs. NULL means use sensible defaults
 * (label = "NO NAME", num_fats = 2, cluster size by partition size,
 * 512-byte logical sectors). */
typedef struct
{
    const char *label;            /* Up to 11 chars; longer is truncated. */
    uint8_t     num_fats;         /* 1 or 2; default 2 if zero (FAT32 only). */
    uint32_t    bytes_per_sector; /* Logical sector size: 512 or 4096; 0 = 512.
                                   * FAT32 supports 512 only; exFAT supports both. */
    uint32_t    cluster_size;     /* Cluster size in bytes (power of 2); 0 = pick
                                   * automatically by volume size. Must be >= one
                                   * logical sector. FAT32 caps at 64 KB. */
} mkfs_options_t;

typedef struct fs_ops
{
    const char *name;            /* "fat32", "dobfs", ... */
    uint8_t     mbr_type;        /* MBR partition type byte to stamp. */

    /* mkfs — initialise the partition with an empty filesystem.
     * partition_start_lba is the disk-relative LBA of sector 0 of
     * the partition (= the BPB's `hidden_sectors` for FAT32).
     * sectors is the partition's size. Returns true on success. */
    bool (*mkfs)(int block_index,
                 uint32_t partition_start_lba,
                 uint32_t sectors,
                 const mkfs_options_t *opts);

    /* Probe — does the partition appear to hold this filesystem?
     * Reads the first sector and checks the magic. Used by DobDisk
     * to populate the partition list with filesystem labels. */
    bool (*detect)(int block_index, uint32_t partition_start_lba);
} fs_ops_t;

/* Look up fs_ops by MBR partition type byte. Returns NULL if no
 * registered formatter handles that type. */
const fs_ops_t *fs_ops_for_mbr_type(uint8_t mbr_type);

/* Look up by name (case-sensitive). */
const fs_ops_t *fs_ops_for_name(const char *name);

/* Pre-declared formatter instances. */
extern const fs_ops_t fat32_ops;
extern const fs_ops_t exfat_ops;

#endif /* MAINDOB_DOB_FS_OPS_H */
