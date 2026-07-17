#include "boot/bootfs.h"
#include "boot/disk.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "lib/string.h"
#include "console/console.h"
#include "kernel.h"

#define SECTOR_SIZE       512u
#define MAX_CLUSTER_SIZE  (64u * SECTOR_SIZE)   /* 32 KB               */
#define MAX_FILE_SIZE     (512u * 1024u)
#define MAX_LIVE_BLOB     (256u * 1024u * 1024u)
#define ATTR_LFN          0x0F
#define ATTR_DIR          0x10
#define LFN_MAX_SLOTS     18

/* Buffer statici: zero heap in fase di boot, una operazione per volta. */
static uint8_t s_dir_buf[MAX_CLUSTER_SIZE];
static uint8_t s_file_buf[MAX_FILE_SIZE];
static uint8_t s_fat_cache[SECTOR_SIZE];
static uint32_t s_fat_cache_sector = UINT32_MAX;
static bool s_disabled;

static struct
{
    uint32_t part_lba;
    uint8_t  sectors_per_cluster;
    uint32_t fat_start;
    uint32_t fat_sectors;
    uint32_t data_start;
    uint32_t root_cluster;
    uint32_t bytes_per_cluster;
} s_fs;

typedef struct __attribute__((packed))
{
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  reserved;
    uint8_t  ctime_tenths;
    uint16_t ctime, cdate, adate;
    uint16_t cluster_hi;
    uint16_t mtime, mdate;
    uint16_t cluster_lo;
    uint32_t file_size;
} fat32_dirent_t;

/* === Backend a blocchi (ramdisk) ======================================== */

static const uint8_t *s_ram_base;
static uint32_t       s_ram_sectors;
static uint32_t       s_ram_bytes;

/* Backend a blocchi: unico punto, due sorgenti (RAM live o disco ATA).
 * Il resto del parser FAT32 e' identico — stessa lezione del 1.0. */
typedef bool (*block_read_fn)(uint32_t lba, uint32_t count, void *buf);

static bool ramdisk_read(uint32_t lba, uint32_t count, void *buf)
{
    if (s_ram_base == NULL || (uint64_t)lba + count > s_ram_sectors)
    {
        return false;
    }
    memcpy(buf, s_ram_base + lba * SECTOR_SIZE, count * SECTOR_SIZE);
    return true;
}

static block_read_fn s_block_read = boot_disk_read;

static bool block_read(uint32_t lba, uint32_t count, void *buf)
{
    return s_block_read(lba, count, buf);
}

static bool read_sectors(uint32_t lba, uint32_t count, void *buf)
{
    return block_read(s_fs.part_lba + lba, count, buf);
}

static uint32_t read_cluster(uint32_t cluster, void *buf)
{
    if (cluster < 2)
    {
        return 0;
    }
    uint32_t lba = s_fs.data_start +
                   (cluster - 2) * s_fs.sectors_per_cluster;
    if (!read_sectors(lba, s_fs.sectors_per_cluster, buf))
    {
        return 0;
    }
    return s_fs.bytes_per_cluster;
}

static uint32_t fat_next(uint32_t cluster)
{
    uint32_t off    = cluster * 4;
    uint32_t sector = s_fs.fat_start + off / SECTOR_SIZE;

    if (sector != s_fat_cache_sector)   /* 128 entry/settore: cache      */
    {
        if (!read_sectors(sector, 1, s_fat_cache))
        {
            return 0x0FFFFFFF;
        }
        s_fat_cache_sector = sector;
    }
    uint32_t val;
    memcpy(&val, s_fat_cache + off % SECTOR_SIZE, 4);
    return val & 0x0FFFFFFF;
}

static bool fat_eoc(uint32_t cluster)
{
    return cluster >= 0x0FFFFFF8;
}

/* === Verbi directory ===================================================== */

/* Slot LFN validati DUE volte (qui e nel chiamante): il byte slot viene
 * dritto dal disco — un'immagine ostile con slot=63 scriverebbe 800+
 * byte oltre il buffer (difesa osservata nel 1.0, conservata). */
static void lfn_extract(const uint8_t *entry, char *out, int slot)
{
    static const int offsets[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };

    if (slot < 1 || slot > LFN_MAX_SLOTS)
    {
        return;
    }

    int base = (slot - 1) * 13;
    int idx  = 0;
    for (int i = 0; i < 13; i++)
    {
        uint16_t ch = (uint16_t)(entry[offsets[i]] |
                                 (entry[offsets[i] + 1] << 8));
        if (ch == 0 || ch == 0xFFFF)
        {
            out[base + idx] = '\0';
            return;
        }
        out[base + idx] = (char)(ch & 0xFF);    /* solo ASCII             */
        idx++;
    }
}

static bool name_match(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (ca != cb)
        {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static void sfn_build(const fat32_dirent_t *de, char *out)
{
    int i = 0;
    for (int j = 0; j < 8 && de->name[j] != ' '; j++)
    {
        out[i++] = de->name[j];
    }
    if (de->ext[0] != ' ')
    {
        out[i++] = '.';
        for (int j = 0; j < 3 && de->ext[j] != ' '; j++)
        {
            out[i++] = de->ext[j];
        }
    }
    out[i] = '\0';
}

static uint32_t dir_find_checked(uint32_t dir_cluster, const char *name,
                                 uint32_t *file_size, bool *is_dir,
                                 bool *found)
{
    *found = false;
    char lfn_name[256];
    int  lfn_slots = 0;
    memset(lfn_name, 0, sizeof(lfn_name));

    for (uint32_t cluster = dir_cluster; !fat_eoc(cluster);
         cluster = fat_next(cluster))
    {
        if (read_cluster(cluster, s_dir_buf) == 0)
        {
            break;
        }

        uint32_t entries = s_fs.bytes_per_cluster / 32u;
        for (uint32_t i = 0; i < entries; i++)
        {
            uint8_t *raw = s_dir_buf + i * 32u;
            if (raw[0] == 0x00)
            {
                return 0;               /* fine directory                 */
            }
            if (raw[0] == 0xE5)
            {
                lfn_slots = 0;
                continue;               /* cancellato                     */
            }

            if (raw[11] == ATTR_LFN)
            {
                int slot = raw[0] & 0x3F;
                if (slot < 1 || slot > LFN_MAX_SLOTS)
                {
                    lfn_slots = 0;
                    continue;
                }
                if (raw[0] & 0x40)
                {
                    lfn_slots = slot;
                    memset(lfn_name, 0, sizeof(lfn_name));
                }
                lfn_extract(raw, lfn_name, slot);
                continue;
            }

            fat32_dirent_t de;
            memcpy(&de, raw, sizeof(de));
            char sfn[13];
            sfn_build(&de, sfn);

            bool match = (lfn_slots > 0 && lfn_name[0] != '\0' &&
                          name_match(lfn_name, name)) ||
                         name_match(sfn, name);
            lfn_slots = 0;

            if (match)
            {
                *found     = true;
                *file_size = de.file_size;
                *is_dir    = (de.attr & ATTR_DIR) != 0;
                return ((uint32_t)de.cluster_hi << 16) | de.cluster_lo;
            }
        }
    }
    return 0;
}

/* === Verbi di inizializzazione ========================================== */

static bool detect_partition(void)
{
    uint8_t mbr[SECTOR_SIZE];
    if (!block_read(0, 1, mbr))
    {
        kprintf("[BFS ] settore 0 illeggibile\n");
        return false;
    }

    s_fs.part_lba = 0;
    if (mbr[510] == 0x55 && mbr[511] == 0xAA)
    {
        uint8_t type = mbr[446 + 4];
        if (type == 0x0B || type == 0x0C)
        {
            memcpy(&s_fs.part_lba, mbr + 446 + 8, 4);
            kprintf("[BFS ] MBR: partizione FAT32 a LBA %u\n", s_fs.part_lba);
        }
        else if (mbr[0] != 0xEB && mbr[0] != 0xE9)
        {
            kprintf("[BFS ] nessuna partizione FAT32\n");
            return false;
        }
        /* altrimenti: superfloppy, BPB al settore 0 */
    }
    return true;
}

static bool parse_bpb(void)
{
    uint8_t bpb[SECTOR_SIZE];
    if (!read_sectors(0, 1, bpb) || bpb[510] != 0x55 || bpb[511] != 0xAA)
    {
        kprintf("[BFS ] BPB illeggibile o firma assente\n");
        return false;
    }

    uint16_t bytes_per_sector;
    memcpy(&bytes_per_sector, bpb + 11, 2);
    if (bytes_per_sector != SECTOR_SIZE)
    {
        kprintf("[BFS ] settori da %u non supportati\n", bytes_per_sector);
        return false;
    }

    uint8_t spc = bpb[13];
    /* Potenza di due in {1..64}: oltre, bytes_per_cluster supererebbe i
     * buffer statici e read_cluster straborderebbe la BSS (difesa 1.0). */
    if (spc == 0 || spc > 64 || (spc & (spc - 1)) != 0)
    {
        kprintf("[BFS ] sectors_per_cluster invalido: %u\n", spc);
        return false;
    }
    s_fs.sectors_per_cluster = spc;

    uint16_t reserved;
    uint8_t  num_fats = bpb[16];
    memcpy(&reserved, bpb + 14, 2);
    memcpy(&s_fs.fat_sectors,  bpb + 36, 4);
    memcpy(&s_fs.root_cluster, bpb + 44, 4);

    s_fs.fat_start         = reserved;
    s_fs.data_start        = reserved + num_fats * s_fs.fat_sectors;
    s_fs.bytes_per_cluster = spc * SECTOR_SIZE;

    kprintf("[BFS ] FAT32: %u sec/cluster, FAT@%u, dati@%u, root=%u\n",
            spc, s_fs.fat_start, s_fs.data_start, s_fs.root_cluster);
    return true;
}

/* === API ================================================================ */

bool bootfs_init(void)
{
    s_block_read = boot_disk_read;
    if (!boot_disk_init())
    {
        return false;
    }
    return detect_partition() && parse_bpb();
}

bool bootfs_init_ramdisk(uint32_t phys_base, uint32_t size)
{
    if (size < SECTOR_SIZE || size > MAX_LIVE_BLOB)
    {
        kprintf("[BFS ] blob live di dimensione insensata (%u byte)\n", size);
        return false;
    }
    if (!paging_ensure_direct_map(phys_base + size))
    {
        return false;
    }

    s_ram_base    = (const uint8_t *)(phys_base + KERNEL_VMA);
    s_ram_bytes   = size;
    s_ram_sectors = size / SECTOR_SIZE;
    s_block_read  = ramdisk_read;

    kprintf("[BFS ] blob live a phys 0x%08x, %u settori (%u KB)\n",
            phys_base, s_ram_sectors, size >> 10);

    return detect_partition() && parse_bpb();
}

const void *bootfs_live_blob_ptr(void)
{
    return s_ram_base;
}

uint32_t bootfs_live_blob_size_bytes(void)
{
    return s_ram_bytes;
}

void *bootfs_read_file(const char *path, uint32_t *size)
{
    if (s_disabled)
    {
        kprintf("[BFS ] read_file DOPO lo shutdown: violazione del "
                "contratto col driver ATA userspace - rifiutato\n");
        return NULL;
    }
    if (path == NULL || path[0] != '/')
    {
        return NULL;
    }

    /* Cammina il path componente per componente dalla root.
     * found=false distingue "dir_find fallita" da "file vuoto trovato"
     * (entrambi cluster 0: FAT32 assegna cluster solo a contenuto). */
    uint32_t cluster   = s_fs.root_cluster;
    uint32_t file_size = 0;
    bool     is_dir    = true;
    const char *p = path + 1;

    while (*p != '\0')
    {
        char component[256];
        uint32_t n = 0;
        while (*p != '\0' && *p != '/' && n < sizeof(component) - 1)
        {
            component[n++] = *p++;
        }
        component[n] = '\0';
        if (*p == '/')
        {
            p++;
        }
        if (n == 0)
        {
            continue;
        }
        if (!is_dir)
        {
            return NULL;                /* path prosegue oltre un file    */
        }

        bool found = false;
        is_dir     = false;
        file_size  = 0;
        cluster = dir_find_checked(cluster, component,
                                   &file_size, &is_dir, &found);
        if (!found)
        {
            return NULL;
        }
    }

    if (is_dir || file_size > MAX_FILE_SIZE)
    {
        return NULL;
    }

    /* Segui la catena cluster. Bounce via s_dir_buf: 32 KB sullo stack
     * kernel (8 KB + guard) sarebbero un overflow certo — i buffer
     * grandi vivono in BSS, mai sullo stack (fase boot single-thread,
     * quindi il riuso di s_dir_buf qui e' sicuro). */
    uint32_t copied = 0;
    for (uint32_t c = cluster; !fat_eoc(c) && copied < file_size;
         c = fat_next(c))
    {
        uint32_t got = read_cluster(c, s_dir_buf);
        if (got == 0)
        {
            return NULL;
        }
        uint32_t want = file_size - copied;
        if (want > got)
        {
            want = got;
        }
        memcpy(s_file_buf + copied, s_dir_buf, want);
        copied += want;
    }

    if (copied != file_size)
    {
        return NULL;
    }
    if (size != NULL)
    {
        *size = file_size;
    }
    return s_file_buf;
}

void bootfs_shutdown(void)
{
    s_disabled = true;
    kprintf("[BFS ] bootfs disabilitato (i moduli ora possiedono il disco).\n");
}
