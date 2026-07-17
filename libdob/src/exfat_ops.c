/* MainDOB exFAT formatter — mkfs.exfat over the block layer.
 *
 * Writes a fresh, mountable exFAT filesystem into a partition window,
 * mirroring the algorithm in boot/DobFileSystem/exfat.c (the exfat.mem
 * driver), which has been validated by a host round-trip test. Kept as a
 * native libdob formatter (like fat32_ops) so DobDisk / DobLiveSetup can
 * format via fs_ops without loading the .mem.
 *
 *   sector 0 / 12        Main Boot Sector + backup
 *   sectors 1-8 / 13-20  Extended Boot Sectors (ExtendedBootSignature)
 *   sectors 9-10 / 21-22 OEM Parameters + Reserved
 *   sector 11 / 23       Boot Checksum
 *   FatOffset            FAT (1 FAT)
 *   ClusterHeapOffset    Allocation Bitmap, Up-case Table, empty root dir
 *
 * Supports 512- and 4096-byte LOGICAL sectors. `sectors` is the partition
 * size in 512-byte PHYSICAL sectors; all exFAT on-disk offsets are in
 * logical sectors, and writes go through the 512-byte block layer (a logical
 * sector spans s512 = 1 or 8 physical sectors). All arithmetic is 32-bit:
 * MainDOB userspace links no compiler-rt, and fs_ops passes a 32-bit sector
 * count, so no 64-bit divide/multiply/shift helpers are pulled in.
 *
 * Up-case table: a 60-byte compressed table covering 0000-FFFF with ASCII
 * a-z -> A-Z folding (consistent with the exfat.mem NameHash up-casing).
 * Detect probes sector 0 for the 0x55AA signature and the "EXFAT   " name.
 */

#include <string.h>
#include <dob/fs_ops.h>
#include <dob/block.h>
#include <dob/partition.h>

#define SECTOR_SIZE   512
#define EXFAT_EOC     0xFFFFFFFFu
#define ENTRY_BYTES   32

/* Boot region checksum (spec Fig. 1): rotate-add over the bytes, skipping
 * VolumeFlags (106,107) and PercentInUse (112) of the first sector. */
static uint32_t
boot_checksum_add(uint32_t sum, const uint8_t *sec, uint32_t len, bool first)
{
    for (uint32_t i = 0; i < len; i++)
    {
        if (first && (i == 106u || i == 107u || i == 112u)) continue;
        sum = (uint32_t)(((sum & 1u) ? 0x80000000u : 0u) + (sum >> 1) + sec[i]);
    }
    return sum;
}

/* Up-case table checksum (spec Fig. 3). */
static uint32_t
upcase_checksum(const uint8_t *t, uint32_t len)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
        sum = (uint32_t)(((sum & 1u) ? 0x80000000u : 0u) + (sum >> 1) + t[i]);
    return sum;
}

/* Zero a run of physical 512-byte sectors. */
#define ZERO_CHUNK_SECTORS  64
static bool
zero_region(int bi, uint32_t start_lba, uint32_t count)
{
    static uint8_t zero[ZERO_CHUNK_SECTORS * SECTOR_SIZE];
    memset(zero, 0, sizeof(zero));
    while (count > 0)
    {
        uint32_t n = (count > ZERO_CHUNK_SECTORS) ? ZERO_CHUNK_SECTORS : count;
        if (!block_write(bi, start_lba, n, zero)) return false;
        start_lba += n;
        count     -= n;
    }
    return true;
}

/* Cluster size (log2 bytes) by volume size, never below one logical sector. */
static uint32_t
choose_clus_shift(uint32_t sectors, uint32_t bps_shift)
{
    uint32_t cs;
    if      (sectors <= 524288u)   cs = 12u;   /* <= 256 MB -> 4 KB   */
    else if (sectors <= 67108864u) cs = 15u;   /* <= 32 GB  -> 32 KB  */
    else                           cs = 17u;   /* else      -> 128 KB */
    if (cs < bps_shift) cs = bps_shift;
    return cs;
}

/* === exfat_ops vtable === */

static bool
exfat_mkfs(int block_index,
           uint32_t partition_start_lba,
           uint32_t sectors,
           const mkfs_options_t *opts)
{
    uint32_t bps = (opts && opts->bytes_per_sector) ? opts->bytes_per_sector : 512u;
    if (bps != 512u && bps != 4096u) return false;
    if (sectors < 2048u) return false;            /* < 1 MB not worth it */

    uint32_t bps_shift = (bps == 4096u) ? 12u : 9u;
    uint32_t s512      = 1u << (bps_shift - 9u);  /* physical sectors / logical sector */
    uint32_t base      = partition_start_lba;
    uint32_t vol_lsec  = sectors >> (bps_shift - 9u);
    if (vol_lsec < 64u) return false;

    /* cluster size: explicit override (power of 2, >= one logical sector,
     * <= 32 MB) or automatic by volume size. */
    uint32_t clus_shift;
    if (opts && opts->cluster_size &&
        (opts->cluster_size & (opts->cluster_size - 1u)) == 0u)
    {
        uint32_t cs = 0u, v = opts->cluster_size;
        while (v > 1u) { v >>= 1; cs++; }
        if (cs < bps_shift || cs > 25u) return false;   /* invalid for this volume */
        clus_shift = cs;
    }
    else
    {
        clus_shift = choose_clus_shift(sectors, bps_shift);
    }
    uint32_t spc_shift     = clus_shift - bps_shift;
    uint32_t spc           = 1u << spc_shift;
    uint32_t cluster_bytes = 1u << clus_shift;

    /* layout (logical sectors); FAT estimate overestimates, which is safe */
    uint32_t fat_offset    = 128u;
    uint32_t est_clusters  = (vol_lsec - fat_offset) >> spc_shift;
    uint32_t fat_bytes     = (est_clusters + 2u) * 4u;
    uint32_t fat_len       = (fat_bytes + (bps - 1u)) >> bps_shift;
    uint32_t heap_offset   = (fat_offset + fat_len + (spc - 1u)) & ~(spc - 1u);
    fat_len                = heap_offset - fat_offset;

    uint32_t cluster_count = (vol_lsec - heap_offset) >> spc_shift;
    if (cluster_count < 4u) return false;

    uint32_t bitmap_bytes   = (cluster_count + 7u) / 8u;
    uint32_t bmp_clusters   = (bitmap_bytes + cluster_bytes - 1u) / cluster_bytes;
    uint32_t bitmap_cluster = 2u;
    uint32_t upcase_cluster = bitmap_cluster + bmp_clusters;
    uint32_t root_cluster   = upcase_cluster + 1u;
    uint32_t used_clusters  = bmp_clusters + 2u;
    if (root_cluster >= (bps >> 2)) return false;  /* chains must fit FAT lsec 0 */

    uint32_t lba_bmp  = heap_offset + (bitmap_cluster - 2u) * spc;
    uint32_t lba_up   = heap_offset + (upcase_cluster - 2u) * spc;
    uint32_t lba_root = heap_offset + (root_cluster   - 2u) * spc;

    static uint8_t sec[4096];

    /* up-case table (60 bytes, compressed: identity + a-z->A-Z) + checksum */
    uint8_t up[60];
    memset(up, 0, sizeof(up));
    up[0]=0xFFu; up[1]=0xFFu; up[2]=0x61u; up[3]=0x00u;
    for (int i = 0; i < 26; i++) { up[4 + i*2] = (uint8_t)(0x41 + i); up[5 + i*2] = 0x00u; }
    up[56]=0xFFu; up[57]=0xFFu; up[58]=0x85u; up[59]=0xFFu;
    uint32_t up_csum = upcase_checksum(up, 60u);

    /* ---------- Main Boot Sector (logical 0) ---------- */
    memset(sec, 0, bps);
    sec[0]=0xEBu; sec[1]=0x76u; sec[2]=0x90u;
    memcpy(sec + 3, "EXFAT   ", 8);
    /* VolumeLength: low 32 bits = vol_lsec, high 32 bits stay zero (< 2 TB). */
    sec[72]=(uint8_t)vol_lsec;        sec[73]=(uint8_t)(vol_lsec>>8);
    sec[74]=(uint8_t)(vol_lsec>>16);  sec[75]=(uint8_t)(vol_lsec>>24);
    sec[80]=(uint8_t)fat_offset;         sec[81]=(uint8_t)(fat_offset>>8);
    sec[82]=(uint8_t)(fat_offset>>16);   sec[83]=(uint8_t)(fat_offset>>24);
    sec[84]=(uint8_t)fat_len;            sec[85]=(uint8_t)(fat_len>>8);
    sec[86]=(uint8_t)(fat_len>>16);      sec[87]=(uint8_t)(fat_len>>24);
    sec[88]=(uint8_t)heap_offset;        sec[89]=(uint8_t)(heap_offset>>8);
    sec[90]=(uint8_t)(heap_offset>>16);  sec[91]=(uint8_t)(heap_offset>>24);
    sec[92]=(uint8_t)cluster_count;      sec[93]=(uint8_t)(cluster_count>>8);
    sec[94]=(uint8_t)(cluster_count>>16);sec[95]=(uint8_t)(cluster_count>>24);
    sec[96]=(uint8_t)root_cluster;       sec[97]=(uint8_t)(root_cluster>>8);
    sec[98]=(uint8_t)(root_cluster>>16); sec[99]=(uint8_t)(root_cluster>>24);
    sec[100]=0x4Eu; sec[101]=0x49u; sec[102]=0x41u; sec[103]=0x4Du; /* VolumeSerialNumber */
    sec[104]=0x00u; sec[105]=0x01u;                                 /* FileSystemRevision 1.00 */
    sec[108]=(uint8_t)bps_shift;
    sec[109]=(uint8_t)spc_shift;
    sec[110]=1u;                                                    /* NumberOfFats */
    sec[111]=0x80u;
    sec[112]=(uint8_t)((used_clusters * 100u) / cluster_count);     /* PercentInUse */
    for (int i = 120; i < 510; i++) sec[i] = 0xF4u;
    sec[510]=0x55u; sec[511]=0xAAu;

    uint32_t bsum = boot_checksum_add(0u, sec, bps, true);
    if (!block_write(block_index, base + 0u,         s512, sec)) return false;
    if (!block_write(block_index, base + 12u * s512, s512, sec)) return false;

    /* ---------- Extended Boot Sectors (1..8) ---------- */
    memset(sec, 0, bps);
    sec[bps-4]=0x00u; sec[bps-3]=0x00u; sec[bps-2]=0x55u; sec[bps-1]=0xAAu;
    for (uint32_t s = 1u; s <= 8u; s++)
    {
        bsum = boot_checksum_add(bsum, sec, bps, false);
        if (!block_write(block_index, base + s * s512,         s512, sec)) return false;
        if (!block_write(block_index, base + (s + 12u) * s512, s512, sec)) return false;
    }

    /* ---------- OEM Parameters (9) + Reserved (10) ---------- */
    memset(sec, 0, bps);
    for (uint32_t s = 9u; s <= 10u; s++)
    {
        bsum = boot_checksum_add(bsum, sec, bps, false);
        if (!block_write(block_index, base + s * s512,         s512, sec)) return false;
        if (!block_write(block_index, base + (s + 12u) * s512, s512, sec)) return false;
    }

    /* ---------- Boot Checksum (11) ---------- */
    memset(sec, 0, bps);
    for (uint32_t i = 0; i < bps; i += 4u)
    {
        sec[i]=(uint8_t)bsum;         sec[i+1]=(uint8_t)(bsum>>8);
        sec[i+2]=(uint8_t)(bsum>>16); sec[i+3]=(uint8_t)(bsum>>24);
    }
    if (!block_write(block_index, base + 11u * s512, s512, sec)) return false;
    if (!block_write(block_index, base + 23u * s512, s512, sec)) return false;

    /* ---------- FAT ---------- */
    memset(sec, 0, bps);
    sec[0]=0xF8u; sec[1]=0xFFu; sec[2]=0xFFu; sec[3]=0xFFu;
    sec[4]=0xFFu; sec[5]=0xFFu; sec[6]=0xFFu; sec[7]=0xFFu;
    for (uint32_t i = 0; i < bmp_clusters; i++)
    {
        uint32_t cl = bitmap_cluster + i;
        uint32_t nx = (i + 1u < bmp_clusters) ? (cl + 1u) : EXFAT_EOC;
        sec[cl*4+0]=(uint8_t)nx; sec[cl*4+1]=(uint8_t)(nx>>8);
        sec[cl*4+2]=(uint8_t)(nx>>16); sec[cl*4+3]=(uint8_t)(nx>>24);
    }
    sec[upcase_cluster*4+0]=0xFFu; sec[upcase_cluster*4+1]=0xFFu;
    sec[upcase_cluster*4+2]=0xFFu; sec[upcase_cluster*4+3]=0xFFu;
    sec[root_cluster*4+0]=0xFFu;   sec[root_cluster*4+1]=0xFFu;
    sec[root_cluster*4+2]=0xFFu;   sec[root_cluster*4+3]=0xFFu;
    if (!block_write(block_index, base + fat_offset * s512, s512, sec)) return false;
    if (fat_len > 1u &&
        !zero_region(block_index, base + (fat_offset + 1u) * s512,
                     (fat_len - 1u) * s512)) return false;

    /* ---------- Allocation Bitmap ---------- */
    memset(sec, 0, bps);
    for (uint32_t i = 0; i < used_clusters; i++)
        sec[i >> 3] = (uint8_t)(sec[i >> 3] | (1u << (i & 7u)));
    if (!block_write(block_index, base + lba_bmp * s512, s512, sec)) return false;
    {
        uint32_t bmp_lsec = bmp_clusters * spc;
        if (bmp_lsec > 1u &&
            !zero_region(block_index, base + (lba_bmp + 1u) * s512,
                         (bmp_lsec - 1u) * s512)) return false;
    }

    /* ---------- Up-case Table ---------- */
    memset(sec, 0, bps);
    memcpy(sec, up, 60u);
    if (!block_write(block_index, base + lba_up * s512, s512, sec)) return false;
    if (spc > 1u &&
        !zero_region(block_index, base + (lba_up + 1u) * s512,
                     (spc - 1u) * s512)) return false;

    /* ---------- Root Directory ---------- */
    memset(sec, 0, bps);
    sec[0] = 0x83u;                                /* Volume Label entry */
    {
        const char *lab = (opts && opts->label) ? opts->label : "";
        uint8_t n = 0;
        while (n < 11u && lab[n])
        {
            char c = lab[n];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            sec[2 + n*2] = (uint8_t)c;
            sec[3 + n*2] = 0x00u;
            n++;
        }
        sec[1] = n;                                /* CharacterCount */
    }
    {
        uint8_t *b = sec + ENTRY_BYTES;            /* Allocation Bitmap entry */
        b[0]=0x81u;
        b[20]=(uint8_t)bitmap_cluster;      b[21]=(uint8_t)(bitmap_cluster>>8);
        b[22]=(uint8_t)(bitmap_cluster>>16);b[23]=(uint8_t)(bitmap_cluster>>24);
        b[24]=(uint8_t)bitmap_bytes;        b[25]=(uint8_t)(bitmap_bytes>>8);
        b[26]=(uint8_t)(bitmap_bytes>>16);  b[27]=(uint8_t)(bitmap_bytes>>24);
    }
    {
        uint8_t *u = sec + 2u * ENTRY_BYTES;       /* Up-case Table entry */
        u[0]=0x82u;
        u[4]=(uint8_t)up_csum;       u[5]=(uint8_t)(up_csum>>8);
        u[6]=(uint8_t)(up_csum>>16); u[7]=(uint8_t)(up_csum>>24);
        u[20]=(uint8_t)upcase_cluster;      u[21]=(uint8_t)(upcase_cluster>>8);
        u[22]=(uint8_t)(upcase_cluster>>16);u[23]=(uint8_t)(upcase_cluster>>24);
        u[24]=60u;
    }
    if (!block_write(block_index, base + lba_root * s512, s512, sec)) return false;
    if (spc > 1u &&
        !zero_region(block_index, base + (lba_root + 1u) * s512,
                     (spc - 1u) * s512)) return false;

    return true;
}

static bool
exfat_detect(int block_index, uint32_t partition_start_lba)
{
    uint8_t buf[SECTOR_SIZE];
    if (!block_read(block_index, partition_start_lba, 1, buf)) return false;
    if (buf[510] != 0x55 || buf[511] != 0xAA) return false;
    static const char SIG[8] = "EXFAT   ";
    for (int i = 0; i < 8; i++)
        if (buf[3 + i] != (uint8_t)SIG[i]) return false;
    return true;
}

const fs_ops_t exfat_ops =
{
    .name     = "exfat",
    .mbr_type = MBR_TYPE_EXFAT,
    .mkfs     = exfat_mkfs,
    .detect   = exfat_detect,
};
