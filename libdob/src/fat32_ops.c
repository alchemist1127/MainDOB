/* MainDOB FAT32 formatter — mkfs.fat32 over the block layer.
 *
 * Writes a fresh, mountable FAT32 filesystem into a partition window.
 * The general layout we produce:
 *
 *   sector 0                 BPB (boot sector with FAT32 parameters)
 *   sector 1                 FSInfo (free-cluster hint, last-allocated hint)
 *   sector 6                 Backup BPB (copy of sector 0)
 *   sector reserved_sectors  FAT 1
 *   sector reserved_sectors  FAT 2 (if num_fats == 2)
 *     + fat_size
 *   first data cluster       Empty root directory (cluster 2, EOC in FAT)
 *
 * Defaults follow the Microsoft FAT32 spec recommendations:
 *   - 2 FATs (caller can request 1 via mkfs_options_t.num_fats)
 *   - 32 reserved sectors
 *   - sectors_per_cluster chosen by partition size (table below)
 *   - hidden_sectors = partition_start_lba (FAT spec requirement so the
 *     same image can be booted with the partition offset baked in)
 *
 * No bootstrap code is written into the boot region — a freshly
 * mkfs'd partition is mountable as data, not bootable. Making it
 * bootable is a separate step performed by the bootloader installer
 * (step 11 of the disk-utility work).
 *
 * Detect (probe) does a single block_read of sector 0 and checks the
 * 0x55AA signature + the "FAT32" string at offset 82. Cheap, used by
 * DobDisk to label partition rows in the UI.
 */

#include <string.h>
#include <dob/fs_ops.h>
#include <dob/block.h>
#include <dob/partition.h>

#define SECTOR_SIZE        512
#define FSINFO_SECTOR      1
#define BACKUP_BPB_SECTOR  6
#define DEFAULT_RESERVED   32
#define BPB_SIG_BYTE       0x29
#define BS_SIGNATURE       0xAA55
#define FAT32_EOC          0x0FFFFFFFu
#define FAT32_BAD          0x0FFFFFF7u
#define FSINFO_LEAD_SIG    0x41615252u
#define FSINFO_STRUC_SIG   0x61417272u
#define FSINFO_TRAIL_SIG   0xAA550000u

/* The FAT32 boot sector / BPB. 512 bytes total — verified by sizeof
 * assertion below to catch packing surprises early.                 */
typedef struct __attribute__((packed))
{
    uint8_t  jmp[3];                /* EB 58 90 */
    char     oem_name[8];           /* "MAINDOB " */
    uint16_t bytes_per_sector;      /* 512 */
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;          /* 0 for FAT32 */
    uint16_t total_sectors_16;      /* 0 when > 65535 */
    uint8_t  media;                 /* 0xF8 = fixed disk */
    uint16_t fat_size_16;           /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;        /* = partition_start_lba */
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;          /* 2 */
    uint16_t fs_info;               /* 1 */
    uint16_t bk_boot_sector;        /* 6 */
    uint8_t  reserved[12];
    uint8_t  drive_num;             /* 0x80 */
    uint8_t  reserved1;
    uint8_t  boot_sig;              /* 0x29 */
    uint32_t volume_id;
    char     volume_label[11];      /* "NO NAME    " padded with spaces */
    char     fs_type[8];            /* "FAT32   " */
    uint8_t  boot_code[420];        /* unused — no bootstrap */
    uint16_t signature;             /* 0xAA55 */
} fat32_bpb_t;

/* Compile-time size guard. */
_Static_assert(sizeof(fat32_bpb_t) == SECTOR_SIZE,
               "FAT32 BPB struct must be exactly one sector");

/* FSInfo sector (sector 1 by default). */
typedef struct __attribute__((packed))
{
    uint32_t lead_sig;              /* 0x41615252 */
    uint8_t  reserved1[480];
    uint32_t struc_sig;             /* 0x61417272 */
    uint32_t free_count;            /* Free cluster count hint, 0xFFFFFFFF = unknown */
    uint32_t next_free;             /* Last allocated + 1 hint */
    uint8_t  reserved2[12];
    uint32_t trail_sig;             /* 0xAA550000 */
} fat32_fsinfo_t;

_Static_assert(sizeof(fat32_fsinfo_t) == SECTOR_SIZE,
               "FAT32 FSInfo struct must be exactly one sector");

/* === Helpers === */

/* Pick a sensible cluster size for a partition of the given byte size.
 * Mirrors the table in Microsoft's FAT32 spec, MB units. Worst case
 * we round down to 1 sector for tiny partitions.
 *
 * Takes uint32_t sectors (not bytes) so all math stays in 32 bits.
 * MainDOB userspace doesn't link compiler-rt, so 64-bit divides by
 * non-power-of-2 constants pull __udivdi3 and fail at link time.
 * sectors >> 11 = sectors / 2048 = MB (with 512 B sectors), which
 * the compiler emits as a shift. */
static uint8_t
choose_sectors_per_cluster(uint32_t sectors)
{
    uint32_t mb = sectors >> 11;
    if (mb <       64)  return 1;    /* ≤ 64 MB:   512 B clusters */
    if (mb <      128)  return 2;    /* ≤128 MB:   1 KB           */
    if (mb <      256)  return 4;    /* ≤256 MB:   2 KB           */
    if (mb < 8u * 1024) return 8;    /* ≤  8 GB:   4 KB           */
    if (mb < 16u * 1024) return 16;  /* ≤ 16 GB:   8 KB           */
    if (mb < 32u * 1024) return 32;  /* ≤ 32 GB:  16 KB           */
    return 64;                       /* >  32 GB:  32 KB           */
}

/* Microsoft's "good enough" approximation for FAT size in sectors,
 * adapted correctly for FAT32. Each 512-byte sector holds 128 32-bit
 * FAT entries, so the divisor uses 128 (not the 256 that the classic
 * FAT12/16 form of this formula uses for its 2-byte entries). With 256
 * the FAT came out ~half the needed size, leaving the upper half of the
 * volume's clusters with no FAT slot -- reads of those clusters ran off
 * the end of the FAT and were miscounted as "used", which is what made
 * the disk-usage bar report ~50% on an empty partition. */
static uint32_t
compute_fat_size_32(uint32_t total_sectors,
                    uint32_t reserved,
                    uint8_t  num_fats,
                    uint8_t  sectors_per_cluster)
{
    uint32_t numerator   = total_sectors - reserved;
    uint32_t denominator = (128u * sectors_per_cluster) + num_fats;
    return (numerator + denominator - 1) / denominator;
}

/* Pseudo-random 32-bit volume serial. Uses a couple of process-local
 * state bits xored together — no urandom in MainDOB userspace, but
 * for a volume serial any non-zero distinct value is sufficient. */
static uint32_t
make_volume_id(uint32_t partition_start_lba, uint32_t sectors)
{
    /* Simple xor-shift hash of inputs + a few "wobbly" globals. */
    extern void *malloc(uint32_t n);   /* For its address; arbitrary entropy */
    uint32_t v = 0xDEADBEEFu;
    v ^= partition_start_lba;
    v *= 1664525u;
    v += sectors;
    v ^= (uint32_t)(uintptr_t)&make_volume_id;
    v *= 1013904223u;
    v ^= (uint32_t)(uintptr_t)malloc;
    if (v == 0) v = 0x12345678u;
    return v;
}

/* Pad a label to exactly 11 bytes, space-filled, no NUL. Uppercase ASCII
 * is the only safe charset for FAT short-name volume labels; lowercase
 * works in practice on Windows/Linux but we keep things conservative. */
static void
fill_volume_label(char out[11], const char *src)
{
    static const char DEFAULT_LABEL[12] = "NO NAME    ";
    const char *s = (src && *src) ? src : DEFAULT_LABEL;
    int i;
    for (i = 0; i < 11 && s[i]; i++)
    {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[i] = c;
    }
    for (; i < 11; i++) out[i] = ' ';
}

/* Zero-fill a chunk of sectors on the block device. Used by both
 * the FAT region and the empty root cluster. Writes in MAX_WRITE-
 * sector chunks because the AHCI/ATA WRITE opcode is capped per
 * request (128 sectors). */
#define ZERO_CHUNK_SECTORS  64

static bool
zero_region(int block_index, uint32_t start_lba, uint32_t count)
{
    static uint8_t zero[ZERO_CHUNK_SECTORS * SECTOR_SIZE];
    memset(zero, 0, sizeof(zero));

    while (count > 0)
    {
        uint32_t n = (count > ZERO_CHUNK_SECTORS) ? ZERO_CHUNK_SECTORS : count;
        if (!block_write(block_index, start_lba, n, zero))
            return false;
        start_lba += n;
        count     -= n;
    }
    return true;
}

/* === fat32_ops vtable === */

static bool
fat32_mkfs(int block_index,
           uint32_t partition_start_lba,
           uint32_t sectors,
           const mkfs_options_t *opts)
{
    if (sectors < 1024) return false;   /* Anything smaller than 512KB
                                         * isn't worth formatting. */

    /* MainDOB's FAT32 reader mounts 512-byte logical sectors only, so reject
     * a request for any other logical sector size. (exfat_ops supports 4096.) */
    if (opts && opts->bytes_per_sector && opts->bytes_per_sector != SECTOR_SIZE)
        return false;

    uint8_t  num_fats          = (opts && opts->num_fats) ? opts->num_fats : 2;
    if (num_fats < 1 || num_fats > 2) num_fats = 2;
    uint16_t reserved_sectors  = DEFAULT_RESERVED;
    uint8_t  sectors_per_clus;
    if (opts && opts->cluster_size)
    {
        /* explicit cluster size: must be a power-of-2 multiple of the sector
         * giving 1..128 sectors per cluster (FAT32 caps at 64 KB clusters). */
        uint32_t spc = opts->cluster_size / SECTOR_SIZE;
        if (spc < 1u || spc > 128u || (spc & (spc - 1u)) != 0u ||
            (uint32_t)spc * SECTOR_SIZE != opts->cluster_size)
            return false;
        sectors_per_clus = (uint8_t)spc;
    }
    else
    {
        sectors_per_clus = choose_sectors_per_cluster(sectors);
    }
    uint32_t fat_size          = compute_fat_size_32(
                                     sectors, reserved_sectors,
                                     num_fats, sectors_per_clus);

    if (reserved_sectors + (uint32_t)num_fats * fat_size + sectors_per_clus
        > sectors)
        return false;          /* Partition too small to host the metadata. */

    /* === Build BPB === */
    static uint8_t bpb_buf[SECTOR_SIZE];
    memset(bpb_buf, 0, sizeof(bpb_buf));
    fat32_bpb_t *bpb = (fat32_bpb_t *)bpb_buf;

    bpb->jmp[0] = 0xEB; bpb->jmp[1] = 0x58; bpb->jmp[2] = 0x90;
    memcpy(bpb->oem_name, "MAINDOB ", 8);
    bpb->bytes_per_sector    = SECTOR_SIZE;
    bpb->sectors_per_cluster = sectors_per_clus;
    bpb->reserved_sectors    = reserved_sectors;
    bpb->num_fats            = num_fats;
    bpb->root_entries        = 0;
    bpb->total_sectors_16    = 0;
    bpb->media               = 0xF8;
    bpb->fat_size_16         = 0;
    bpb->sectors_per_track   = 63;
    bpb->num_heads           = 255;
    bpb->hidden_sectors      = partition_start_lba;
    bpb->total_sectors_32    = sectors;
    bpb->fat_size_32         = fat_size;
    bpb->ext_flags           = 0;     /* All FATs mirrored, FAT 0 active */
    bpb->fs_version          = 0;
    bpb->root_cluster        = 2;
    bpb->fs_info             = FSINFO_SECTOR;
    bpb->bk_boot_sector      = BACKUP_BPB_SECTOR;
    bpb->drive_num           = 0x80;
    bpb->boot_sig            = BPB_SIG_BYTE;
    bpb->volume_id           = make_volume_id(partition_start_lba, sectors);
    fill_volume_label(bpb->volume_label, opts ? opts->label : NULL);
    memcpy(bpb->fs_type, "FAT32   ", 8);
    bpb->signature           = BS_SIGNATURE;

    /* === Write sector 0 (primary BPB) === */
    if (!block_write(block_index, partition_start_lba + 0, 1, bpb_buf))
        return false;

    /* === Write sector 6 (backup BPB) === */
    if (!block_write(block_index,
                     partition_start_lba + BACKUP_BPB_SECTOR, 1, bpb_buf))
        return false;

    /* === Write sector 1 (FSInfo) === */
    static uint8_t fsi_buf[SECTOR_SIZE];
    memset(fsi_buf, 0, sizeof(fsi_buf));
    fat32_fsinfo_t *fsi = (fat32_fsinfo_t *)fsi_buf;
    fsi->lead_sig   = FSINFO_LEAD_SIG;
    fsi->struc_sig  = FSINFO_STRUC_SIG;

    /* Free cluster count = total data clusters minus the one we reserve
     * for the empty root directory. */
    uint32_t data_sectors    = sectors
                             - reserved_sectors
                             - (uint32_t)num_fats * fat_size;
    uint32_t total_clusters  = data_sectors / sectors_per_clus;
    fsi->free_count = total_clusters - 1;
    fsi->next_free  = 3;          /* Hint: search from cluster 3 next */
    fsi->trail_sig  = FSINFO_TRAIL_SIG;

    if (!block_write(block_index,
                     partition_start_lba + FSINFO_SECTOR, 1, fsi_buf))
        return false;

    /* Reserved sectors 2..5 and 7..(reserved-1) stay all-zero. We don't
     * explicitly zero them — the disk is assumed fresh, and even if it
     * isn't, FAT32 readers only honour sectors 0/1/6 of the reserved
     * region. Skip to the FAT region. */

    /* === Build the FAT prologue (first sector of every FAT copy) ===
     *   entry 0 = 0x0FFFFFF8 (low byte = media descriptor 0xF8, high
     *             bits set per FAT32 spec) — also matches what mkfs.vfat
     *             produces.
     *   entry 1 = 0x0FFFFFFF (clean-shutdown / no error flags)
     *   entry 2 = 0x0FFFFFFF (EOC for the root directory chain) */
    static uint8_t fat_first[SECTOR_SIZE];
    memset(fat_first, 0, sizeof(fat_first));
    uint32_t *e = (uint32_t *)fat_first;
    e[0] = 0x0FFFFFF8u;
    e[1] = 0x0FFFFFFFu;
    e[2] = FAT32_EOC;

    /* Each FAT: write entry-0 sector, then zero-fill the remainder. */
    for (int f = 0; f < num_fats; f++)
    {
        uint32_t fat_lba = partition_start_lba
                         + reserved_sectors
                         + (uint32_t)f * fat_size;
        if (!block_write(block_index, fat_lba, 1, fat_first))
            return false;
        if (fat_size > 1)
        {
            if (!zero_region(block_index, fat_lba + 1, fat_size - 1))
                return false;
        }
    }

    /* === Zero the first data cluster (root directory, empty) === */
    uint32_t data_lba = partition_start_lba
                      + reserved_sectors
                      + (uint32_t)num_fats * fat_size;
    if (!zero_region(block_index, data_lba, sectors_per_clus))
        return false;

    return true;
}

static bool
fat32_detect(int block_index, uint32_t partition_start_lba)
{
    uint8_t buf[SECTOR_SIZE];
    if (!block_read(block_index, partition_start_lba, 1, buf))
        return false;
    if (buf[510] != 0x55 || buf[511] != 0xAA) return false;
    /* Check FS type string at BPB offset 82. */
    static const char SIG[5] = "FAT32";
    for (int i = 0; i < 5; i++)
        if (buf[82 + i] != (uint8_t)SIG[i]) return false;
    return true;
}

const fs_ops_t fat32_ops =
{
    .name     = "fat32",
    .mbr_type = MBR_TYPE_FAT32_LBA,
    .mkfs     = fat32_mkfs,
    .detect   = fat32_detect,
};
