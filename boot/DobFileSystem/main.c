/* MainDOB DobFileSystem - Full FAT32 Implementation
 * Reads/writes FAT32 filesystem on ATA disk via IPC.
 * Maps MainDOB layout (/SYSTEM, /DATA) to FAT32 directories on disk.
 *
 * Protocol:
 *   code=1  OPEN    payload=path  arg0=flags  -> reply.arg0=fd
 *   code=2  READ    arg0=fd arg1=size         -> reply.payload=data reply.arg0=bytes_read
 *   code=3  WRITE   arg0=fd payload=data      -> reply.arg0=bytes_written
 *   code=4  CLOSE   arg0=fd
 *   code=5  STAT    payload=path              -> reply.arg0=size_lo arg1=type(0=file,1=dir) arg2=size_hi
 *   code=6  READDIR payload=path              -> reply.payload=entries (newline separated)
 *   code=7  MKDIR   payload=path
 *   code=8  UNLINK  payload=path
 *   code=9  SANDBOX_CHECK  arg0=pid payload=path arg1=write -> reply.arg0=allowed(1/0)
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>        /* SYS_LIVE_QUERY / SYS_LIVE_READ in live mode */
#include <dob/server.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/types.h>
#include <DobFiles.h>           /* dobfiles_OpenMount, used by OPEN_VIEW */
#include <DobFileSystem.h>      /* dobfs_GetMountedOn, root-alias detection */
#include <dob/partition.h>      /* mbr_table_t for token-based lba lookup */
#include <dob/mem.h>            /* dob_mem_load — loads exfat.mem at runtime */
#include "dobfs_protocol.h"
#include "exfat_api.h"          /* exfat_api_t / exfat_file_t / exfat_volume_t */

/* Constants */

#define SECTOR_SIZE         512
#define DOBFS_MAX_FDS         256
#define DOBFS_PATH_MAX        512
#define DIR_ENTRY_SIZE      32

#define FAT_CACHE_SECTORS   128 /* Cache 128 sectors of FAT = 64KB = 16384 entries.
                                 * Raised from 32 (16KB): typical Dob boot touches
                                 * ~8000 clusters — at 4KB entries a miss every few
                                 * reads is the main FS latency.  64KB keeps the
                                 * entire FAT in cache for filesystems up to ~2GB. */

/* Aliases — protocol header uses DOBFS_O_ prefix, internal code uses O_ */
#define O_READ      DOBFS_O_READ
#define O_WRITE     DOBFS_O_WRITE
#define O_CREATE    DOBFS_O_CREATE
#define O_APPEND    DOBFS_O_APPEND
#define O_TRUNC     DOBFS_O_TRUNC

/* FAT32 special cluster values */
#define FAT32_FREE      0x00000000
#define FAT32_EOC_MIN   0x0FFFFFF8
#define FAT32_EOC       0x0FFFFFFF
#define FAT32_BAD       0x0FFFFFF7

/* Directory entry attributes */
#define ATTR_VOLUME_ID  0x08
#define ATTR_DIRECTORY  0x10
#define ATTR_ARCHIVE    0x20
#define ATTR_LFN        0x0F  /* Long filename entry */

/* FAT32 On-disk Structures */

/* BIOS Parameter Block + FAT32 Extended */
typedef struct
{
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;     /* 0 for FAT32 */
    uint16_t total_sectors_16;     /* 0 for FAT32 */
    uint8_t  media_type;
    uint16_t fat_size_16;          /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32 extended fields */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} __attribute__((packed)) fat32_bpb_t;

/* Standard 8.3 directory entry */
typedef struct
{
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_hi;
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} __attribute__((packed)) fat32_dirent_t;

/* Long Filename directory entry */
typedef struct
{
    uint8_t  order;
    uint16_t name1[5];     /* UCS-2 chars 1-5 */
    uint8_t  attr;         /* Always 0x0F */
    uint8_t  type;         /* 0 for LFN */
    uint8_t  checksum;
    uint16_t name2[6];     /* UCS-2 chars 6-11 */
    uint16_t zero;
    uint16_t name3[2];     /* UCS-2 chars 12-13 */
} __attribute__((packed)) fat32_lfn_t;

/* Runtime State */

/* Parsed filesystem geometry */
static struct
{
    bool     mounted;
    uint32_t partition_lba;       /* Start LBA of FAT32 partition (0 = unpartitioned) */
    uint32_t partition_sectors;   /* Sectors in this partition (from BPB total_sectors_32) */
    uint32_t fat_start_lba;        /* First sector of FAT */
    uint32_t fat_size_sectors;     /* Sectors per FAT */
    uint32_t data_start_lba;       /* First sector of data region */
    uint32_t root_cluster;         /* Root directory cluster number */
    uint8_t  sectors_per_cluster;
    uint32_t bytes_per_cluster;
    uint32_t total_clusters;
    uint8_t  num_fats;
} fs;

/* === Multi-instance mount mode (disk-utility step 5) ===
 *
 * Without --mount we are the boot-time root mount: register as
 * "DobFileSystem", probe ahci then ata, MBR-detect first FAT32
 * partition, apply the full SYSTEM/DATA sandbox. The whole codebase
 * before step 5 ran exclusively in this mode and that path is
 * unchanged.
 *
 * With --mount provider=X selector=N lba=L id=K fs=fat32 we are a
 * secondary mount spawned by the DAS partition_fat32 action: register
 * as "dobfs_<K>", bind to driver X disk N, use lba=L as the partition
 * offset directly (no MBR re-parse), skip ensure_layout, and run with
 * a lenient sandbox (the partition has no SYSTEM/DATA structure). */
static bool      mount_secondary        = false;
/* Unmount subscribers (DobFiles satellites viewing THIS mount) and the
 * deferred-exit flag for DOBFS_SHUTDOWN — set in the handler, acted on
 * AFTER the reply goes out. */
#define UNMOUNT_SUBS_MAX 4
static uint32_t  unmount_subs[UNMOUNT_SUBS_MAX];
static uint8_t   unmount_sub_count       = 0;
static uint32_t  disk_selector          = 0;
static uint32_t  secondary_id           = 0;
static uint32_t  secondary_partition_lba = 0;    /* From --mount lba=L or derived from token */
static uint32_t  secondary_partition_index = 0;  /* From --mount token=T high 8 bits; -1 if unused */
static bool      secondary_lba_from_token = false; /* If true, derive lba from MBR at init */
static bool      is_root_alias          = false;  /* True when this --mount targets the
                                                   * same partition the root DobFileSystem
                                                   * already serves. We register normally
                                                   * (so wait_service dobfs_<token> succeeds)
                                                   * but never touch the disk; OPEN_VIEW
                                                   * forwards to the root service instead
                                                   * of duplicating the mount. */
static char      explicit_provider[32]  = {0};    /* "" = auto, else "ata" / "ahci" / "usbms_<token>".
                                                   * Was [8]: fine for "ata"/"ahci"/"usbms_0" but it
                                                   * silently TRUNCATED any longer provider name
                                                   * ("usbms_16", and every epoch-tagged usbms_<token>),
                                                   * so the secondary mount bound to a bogus provider
                                                   * name and every sector read failed. */
static char      service_name[32]       = "DobFileSystem";

/* Streaming I/O constants */
#define FD_WP_INTERVAL       64     /* Record waypoint every N clusters */
#define FD_MAX_WAYPOINTS     16     /* Max waypoints per fd */
#define FAT_PREALLOC_COUNT   32     /* Batch-allocate this many clusters */

/* Transient write resilience. A cluster store can fail TRANSIENTLY on real
 * hardware — a flash stick NAKing/timing-out a NAND commit, or a UHCI port
 * just resumed (the first write after wake can be dropped). Reissuing the
 * SAME cluster is idempotent (no fd position/buffer state changes until
 * success), so fd_flush_write retries before reporting a hard error. This is
 * the write-side analogue of the copy loop's read retry; without it a single
 * transient surfaced as a per-file "operazione completata con N errori". */
#define FD_FLUSH_RETRIES     5      /* cluster-store attempts before failing */
#define FD_FLUSH_RETRY_MS    50     /* settle delay between attempts (ms) */

/* Multi-cluster read buffer.
 *
 * f->buf is sized to hold many clusters (rather than a single one) so that
 * fd_fill_read can batch-read every consecutive cluster it can walk in the
 * FAT before issuing I/O.  For a non-fragmented file that's one driver IPC
 * per 64 KB instead of one per cluster (~50 IPCs avoided per 200 KB read). */
#define FD_READ_BUF_SIZE     (64 * 1024)

/* Open file descriptor — streaming I/O pipeline */
typedef struct
{
    bool     used;
    pid_t    owner;
    char     path[DOBFS_PATH_MAX];
    uint32_t flags;
    uint32_t first_cluster;
    uint32_t dir_cluster;
    uint32_t dir_entry_index;
    uint64_t file_size;     /* 64-bit: FAT32 holds <=4 GB; exFAT routing (Phase 3) fills >4 GB */
    uint64_t offset;        /* 64-bit fd cursor */
    bool     is_dir;

    /* exFAT (Phase 3): when is_exfat, this fd is served by exfat.mem and
     * exfat_file holds its state; the FAT32 fields below stay zeroed. */
    bool         is_exfat;
    exfat_file_t exfat_file;

    /* Streaming buffer (malloc'd, FD_READ_BUF_SIZE bytes or bytes_per_cluster,
     * whichever is larger — write path uses one cluster at a time). */
    uint8_t *buf;
    uint32_t buf_pos;               /* write cursor within buffer */
    uint32_t buf_valid;             /* valid bytes in buffer (reads) */
    bool     buf_dirty;             /* buffer has unflushed write data */

    /* First cluster index covered by buf on reads.  When buf_valid is
     * non-zero the buffer contains bytes for the span
     * [buf_start_cluster_idx, buf_start_cluster_idx + N) where N is
     * derived from buf_valid and bytes_per_cluster.  Lets handle_read
     * tell whether an offset falls inside the cached span. */
    uint32_t buf_start_cluster_idx;

    /* Cluster cursor — O(1) sequential, O(delta) seek */
    uint32_t cur_cluster;
    uint32_t cur_cluster_idx;
    uint32_t chain_len;             /* cached, 0 = unknown */
    uint32_t chain_last;            /* last cluster in chain */

    /* Waypoints — every FD_WP_INTERVAL clusters */
    uint32_t wp_cluster[FD_MAX_WAYPOINTS];
    uint32_t wp_idx[FD_MAX_WAYPOINTS];
    uint32_t wp_count;

    bool     meta_dirty;            /* file_size/first_cluster changed */

    /* Non-destructive O_TRUNC: the file's original cluster chain, kept alive
     * while the new content is written to a FRESH chain. Freed at close only
     * after the dirent is repointed to the new chain. 0 = no pending truncate. */
    uint32_t trunc_old_chain;
} dobfs_fd_t;

static dobfs_fd_t fd_table[DOBFS_MAX_FDS];
static uint32_t ata_port = 0;
static bool use_ahci = false;  /* true = AHCI protocol (arg0=port, arg1=lba, arg2=count) */

/* === Live-CD mode ===
 *
 * Set once at startup by setup_live_mode() when the kernel reports a
 * non-zero live-blob size via SYS_LIVE_QUERY. In live mode every
 * sector read goes straight to the kernel's RAM-resident FAT32 blob
 * via SYS_LIVE_READ, and every sector write is refused — the live
 * environment is read-only by design. The ATA service is left alone:
 * the installer (Fase 3) talks to it directly via libdob/block.c
 * when it needs to write the target disk, without going through this
 * mounted volume. */
static bool     live_mode      = false;
static uint32_t live_sectors   = 0;  /* Total sectors available in the live blob */

/* Sector-level I/O buffer */
static uint8_t sector_buf[SECTOR_SIZE * 128];

/* Shared cluster I/O buffers.  Sized for the LARGEST cluster fat32_mount
 * accepts: sectors_per_cluster up to 128 => 128 * 512 = 64 KB.  These were
 * previously SECTOR_SIZE*64 (32 KB), which was smaller than a 64 KB cluster.
 * On a large volume (e.g. the ~20 GB system partition on the Compaq) FAT32 is
 * formatted with 64 KB clusters; the mount succeeded but every directory
 * operation that buffers a whole cluster here overflowed or was refused, so
 * dir listing returned empty (no DAS loaded, no settings, no GUI). Matching
 * the buffers to the mount's accepted maximum fixes it.  Single-threaded, so
 * static sharing is safe; static (not stack) to avoid stack overflow. */
#define MAX_CLUSTER_BYTES   (SECTOR_SIZE * 128)   /* 64 KB = 128-sector cluster */
static uint8_t cluster_data[MAX_CLUSTER_BYTES];

/* dir_iterate needs its own buffer because it's called from within
 * functions that use cluster_work_buf (e.g. ensure_dir → dir_find_entry → dir_iterate).
 * Using the same buffer would corrupt data mid-operation. */
static uint8_t cluster_iter_buf[MAX_CLUSTER_BYTES];
static uint8_t cluster_work_buf[MAX_CLUSTER_BYTES];

/* Path copy buffers — the IPC buffer (where msg->payload lives) gets
 * overwritten every time DobFileSystem makes a sub-IPC call to the ATA driver.
 * We MUST copy paths out before any disk operation. */
static char ipc_path1[DOBFS_PATH_MAX];
static char ipc_path2[DOBFS_PATH_MAX];

/* FAT cache: caches a contiguous range of FAT sectors */
static uint32_t fat_cache[FAT_CACHE_SECTORS * SECTOR_SIZE / 4];
static uint32_t fat_cache_start = 0xFFFFFFFF;  /* First FAT sector cached */
static bool     fat_cache_dirty = false;
/* Dirty-range within the cached window: only sectors [lo,hi] (offsets from
 * fat_cache_start) were modified, so only those need writing back — not the
 * whole 128-sector window. Valid only while fat_cache_dirty is true. This is
 * what keeps a save from pushing ~256 sectors of FAT per flush over USB. */
static uint32_t fat_dirty_lo = 0;
static uint32_t fat_dirty_hi = 0;

/* Dob sandbox: path-based access control.
 * The kernel tracks each process's home_dir (its sandbox root).
 * DobFileSystem enforces access based on it:
 *   - /SYSTEM/CONFIG/ is RESERVED: only config, init, vfs can access it.
 *     This check fires BEFORE the driver bypass — even a driver with full
 *     hardware access cannot read or write system configuration.
 *   - Drivers bypass all other checks (they're system services)
 *   - Clients can READ from: /DATA/, /SYSTEM/PROGRAMS/, /SYSTEM/GAMES/
 *   - /SYSTEM/DRIVERS/ is NOT in the read whitelist by design: the sandbox
 *     denial prevents the Visible=0 check from hiding drivers in the module manager.
 *     Drivers are system services accessed via IPC, not direct file reads.
 *   - Clients can WRITE to: their own home_dir + /DATA/ (for creative
 *     output like documents, images, music — as specified in the project)
 * No per-folder capability bits. The process's home_dir IS its sandbox. */

/* Case-insensitive prefix check */
static bool path_starts_with_ci(const char *path, const char *prefix, uint32_t plen)
{
    for (uint32_t i = 0; i < plen; i++)
    {
        char a = path[i], b = prefix[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return false;
    }
    return true;
}

static bool path_eq_ci(const char *a, const char *b)
{
    while (*a && *b)
    {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == *b;
}

/* Case-insensitive suffix test (e.g. does the path end in ".mem"?). */
static bool path_ends_with_ci(const char *path, const char *suffix)
{
    uint32_t pl = strlen(path), sl = strlen(suffix);
    if (sl > pl) return false;
    const char *p = path + (pl - sl);
    for (uint32_t i = 0; i < sl; i++)
    {
        char a = p[i], b = suffix[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return false;
    }
    return true;
}

/* Whitelist check for /SYSTEM/CONFIG/ access.
 * Returns true only if the caller is one of the pre-selected modules
 * allowed to touch the configuration area. */
static bool
config_area_allowed(pid_t sender_pid)
{
    char home[128];
    if (get_home_dir(sender_pid, home, sizeof(home)) != 0)
        return false;

    /* Match by process name embedded in home_dir.
     * home_dir format: "/SYSTEM/PROGRAMS/<name>/" */
    /* Exact basename match. Extract last non-empty path component.
     * (strstr would allow spoofing — e.g. "my_config_tool" contains "config") */
    int hlen = (int)strlen(home);
    if (hlen > 0 && home[hlen - 1] == '/') hlen--;
    int ls = -1;
    for (int i = hlen - 1; i >= 0; i--)
    {
        if (home[i] == '/') { ls = i; break; }
    }
    if (ls < 0) return false;

    const char *bname = home + ls + 1;
    int blen = hlen - ls - 1;

    if (blen == 6  && memcmp(bname, "config", 6) == 0) return true;
    if (blen == 4  && memcmp(bname, "init", 4) == 0) return true;
    /* phase2_init reads /SYSTEM/CONFIG/Startup_modules on the exFAT root,
     * after the pivot, to launch the rest of the system (root-on-exFAT). */
    if (blen == 11 && memcmp(bname, "phase2_init", 11) == 0) return true;
    if (blen == 13 && memcmp(bname, "DobFileSystem", 13) == 0) return true;
    /* hotplug owns the Device Automation Script database under
     * /SYSTEM/CONFIG/DAS — it reads *.das at boot to map detected
     * devices to desktop icons. Migrated from dobinterface in an
     * earlier build; the whitelist entry was missed at the time. */
    if (blen == 7  && memcmp(bname, "hotplug", 7) == 0) return true;
    /* dobinterface kept here defensively in case future widgets
     * read DAS metadata directly. Harmless if unused. */
    if (blen == 12 && memcmp(bname, "dobinterface", 12) == 0) return true;
    /* DobInstaller writes Startup_modules entries and DAS files when
     * installing a .dbp package. Whitelisted so it can persist the
     * additions without going through the config server indirection. */
    if (blen == 12 && memcmp(bname, "DobInstaller", 12) == 0) return true;
    /* MainDOB_Setup is the live-CD installer wizard. It reads the
     * .das files under /SYSTEM/CONFIG/DAS/ to populate the driver-
     * selection step and (once 3.9 lands) writes Startup_modules +
     * install_info on the freshly-formatted target system. */
    if (blen == 13 && memcmp(bname, "MainDOB_Setup", 13) == 0) return true;
    return false;
}

static bool
sandbox_check(pid_t sender_pid, const char *path, bool is_write)
{
    /* Secondary mount: this partition has no SYSTEM/DATA structure, no
     * per-program homedir mapping under it, and no system files to
     * protect — it's user content surfaced via DobFiles. The mount is
     * reached only through the DAS action that double-clicks the
     * desktop icon and the resulting view, so any caller that already
     * found our service name has been blessed by that flow. Skip the
     * whole permission matrix below. */
    if (mount_secondary)
    {
        (void)sender_pid; (void)path; (void)is_write;
        return true;
    }

    /* RESERVED AREA: /SYSTEM/CONFIG/ is locked down.
     * Checked BEFORE the driver bypass — nobody gets in unless whitelisted. */
    if (path_starts_with_ci(path, "/SYSTEM/CONFIG/", 15) ||
        path_eq_ci(path, "/SYSTEM/CONFIG"))
    {
        /* Public carve-out: /SYSTEM/CONFIG/common_files/ (and the directory
         * itself) is the shared, sandbox-free area -- ANY program may read and
         * write it (fonts, shared .mem, other app resources). This is the one
         * deliberately-unprotected corner of the otherwise locked-down config
         * tree; everything else under CONFIG stays whitelist-only. Without this,
         * List shows files here (List skips the sandbox) but Open/Mkdir are
         * DENIED -- e.g. DobWrite's font scanner lists Arial_Narrow.ttf yet
         * cannot open it, so it never appears in the font menu. */
        if (path_starts_with_ci(path, "/SYSTEM/CONFIG/common_files/", 28) ||
            path_eq_ci(path, "/SYSTEM/CONFIG/common_files"))
            return true;

        /* Narrow, read-only exception: the DAS database subtree.
         * USB host-controller drivers consume the device-level DAS
         * (i .das in /SYSTEM/CONFIG/DAS/USB) at enumeration time to map a
         * just-enumerated device to its icon/action — that is the
         * designed pipeline, the exact same data hotplug reads for PCI.
         * Without this, the driver's dobfs_Open got DENIED while List
         * (which skips the sandbox) showed the file: field signature
         * "List rc=0, 1 entry / Open fd=-1" on the Armada E500.
         * Drivers only ever READ here; writes stay whitelisted-only. */
        if (!is_write && is_driver(sender_pid) &&
            (path_starts_with_ci(path, "/SYSTEM/CONFIG/DAS/", 19) ||
             path_eq_ci(path, "/SYSTEM/CONFIG/DAS")))
            return true;

        return config_area_allowed(sender_pid);
    }

    /* Drivers bypass all other file access checks (they're system services) */
    if (is_driver(sender_pid))
        return true;

    /* Read access: shared areas */
    if (!is_write)
    {
        /* Shared OS objects (PIC .mem modules like exfat.mem / iso9660.mem)
         * are loadable by ANY process, driver or not. Rationale: loading the
         * BYTES of a shared object grants nothing — running its code with
         * hardware privilege is gated separately by the kernel's make_driver
         * (a non-driver that reads a .mem still cannot do I/O ports/IRQ/DMA).
         * So the right gate for *loading shared code* is execution privilege,
         * NOT this file-read ACL. Without this, a SECONDARY mount (which loads
         * exfat.mem cross-process, through us) had to be a driver purely to
         * pass this check — an accident of how it was spawned. Allowing .mem
         * reads makes shared-object loading privilege-independent for every
         * bus and every helper, present and future, instead of one-off driver
         * promotions. The /SYSTEM/CONFIG/ lock above still runs first, so this
         * never widens access to the protected configuration area. */
        if (path_ends_with_ci(path, ".mem")) return true;

        if (path_starts_with_ci(path, "/DATA/", 6)) return true;
        if (path_starts_with_ci(path, "/SYSTEM/PROGRAMS/", 17)) return true;
        if (path_starts_with_ci(path, "/SYSTEM/GAMES/", 14)) return true;
        /* Also allow reading the directories themselves (no trailing slash) */
        if (path_eq_ci(path, "/DATA") || path_eq_ci(path, "/SYSTEM"))
            return true;
        if (path_eq_ci(path, "/SYSTEM/PROGRAMS") ||
            path_eq_ci(path, "/SYSTEM/GAMES"))
            return true;
    }

    /* Write access: query the caller's home directory from the kernel.
     * Each program is sandboxed to its own folder (e.g. /SYSTEM/PROGRAMS/myapp/).
     * Creative programs (word processors, paint, etc.) can also write to /DATA/
     * subdirectories for their output. */
    if (is_write)
    {
        /* Always allow writing to /DATA/ (creative output area) */
        if (path_starts_with_ci(path, "/DATA/", 6))
            return true;

        /* Check if path is under the caller's home directory */
        char home[128];
        if (get_home_dir(sender_pid, home, sizeof(home)) == 0 && home[0] != '\0')
        {
            uint32_t hlen = strlen(home);
            if (path_starts_with_ci(path, home, hlen))
                return true;
        }
    }

    return false;
}

/* ATA IPC Communication */

static int ata_search_attempts = 0;
#define ATA_MAX_BLOCKING_ATTEMPTS 2

/* Probe the kernel for a live ramdisk. Called once from main(), well
 * before any sector I/O. SYS_LIVE_QUERY returns the size of the
 * RAM-resident FAT32 blob in bytes (0 = not booted live), so we
 * decide our backend up front and never need to re-check it. */
static void
setup_live_mode(void)
{
    int size_bytes = syscall0(SYS_LIVE_QUERY);
    if (size_bytes <= 0) return;
    live_mode    = true;
    live_sectors = (uint32_t)size_bytes / SECTOR_SIZE;
}

static void
find_ata_driver(void)
{
    if (ata_port != 0) return;

    /* Secondary mount: bound by argv to a specific driver. Skip the
     * dual-probe path entirely — using the wrong driver would talk
     * to a different disk than the user asked for. */
    if (explicit_provider[0])
    {
        ata_port = dob_registry_wait(explicit_provider, 3000);
        if (!ata_port) ata_port = dob_registry_find(explicit_provider);
        if (ata_port && explicit_provider[0] == 'a' && explicit_provider[1] == 'h')
            use_ahci = true;
        return;
    }

    if (ata_search_attempts < ATA_MAX_BLOCKING_ATTEMPTS)
    {
        /* Ask who owns the boot disk FIRST. The driver that actually
         * loaded the system (ata.mdl on IDE, ahci.mdl on SATA) registers
         * the "bootdisk" marker with its own port as soon as it claims the
         * role — which happens before we need it. Using it skips the blind
         * ahci-then-ata probe below.
         *
         * Why this matters: that probe did dob_registry_wait("ahci", 3000)
         * UNCONDITIONALLY first. On an IDE-only machine there is no AHCI, so
         * it blocked for the full 3-second timeout before even trying "ata"
         * — and during those 3s (plus the subsequent mount time) we had not
         * yet registered "DobFileSystem", so every client that looked us up
         * got "DobFileSystem not found". Querying bootdisk resolves the
         * driver instantly and closes that window. We still must learn
         * whether it's ahci or ata for the IPC message layout, so we match
         * the bootdisk port against the two service ports. */
        uint32_t bd = dob_registry_wait("bootdisk", 3000);
        if (bd)
        {
            uint32_t ahci_port = dob_registry_find("ahci");
            if (ahci_port && ahci_port == bd) use_ahci = true;
            ata_port = bd;
            debug_print(use_ahci ? "[dobfs] root disk via bootdisk marker (ahci)\n"
                                 : "[dobfs] root disk via bootdisk marker (ata)\n");
            return;
        }
        /* LOUD fallback: the silent path here cost a full debug session —
         * an AHCI init slower than these windows made us fall through to
         * "ata" on a SATA root, and the boot died with NO log at all. Every
         * step below states what it resolved (or that it timed out), so the
         * next storage-ordering regression is visible in the boot log. */
        debug_print("[dobfs] no bootdisk marker after 3s, probing ahci/ata\n");

        /* First 2 attempts: blocking wait (gives hotplug time to spawn
         * the driver at boot). After 2 failed attempts, switch to instant
         * lookup — prevents every DobFileSystem request from blocking
         * for 6 seconds when no disk driver is available. */
        ata_search_attempts++;

        ata_port = dob_registry_wait("ahci", 3000);
        if (ata_port)
        {
            use_ahci = true;
            debug_print("[dobfs] root provider: ahci (probe)\n");
            return;
        }

        ata_port = dob_registry_wait("ata", 3000);
        if (ata_port)
        {
            debug_print("[dobfs] root provider: ata (probe; NOTE: if root is "
                        "SATA, the ahci claim arrived too late)\n");
            return;
        }

        debug_print("[dobfs] NO disk driver found (attempt blocked 6-9s); "
                    "root mount will fail until one registers\n");
        return;
    }

    /* After max attempts: instant lookup, no blocking.
     * If hotplug eventually spawns the driver, the next call will find it. */
    ata_port = dob_registry_find("ahci");
    if (ata_port) { use_ahci = true; return; }

    ata_port = dob_registry_find("ata");
}

static bool
disk_read_sectors(uint32_t lba, uint32_t count, void *buf)
{
    /* Live mode: read straight from the kernel's RAM-resident blob.
     * Skips the ATA registry lookup and IPC round-trip entirely —
     * the kernel does the bounds check and the memcpy for us. */
    if (live_mode)
    {
        uint32_t phys_lba = lba + fs.partition_lba;
        return syscall3(SYS_LIVE_READ, (int)phys_lba, (int)count, (int)(uintptr_t)buf) == 0;
    }

    find_ata_driver();
    if (!ata_port) return false;

    /* Apply partition offset for all filesystem operations */
    uint32_t phys_lba = lba + fs.partition_lba;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = 1;

    if (use_ahci)
    {
        msg.arg0 = disk_selector;   /* AHCI port (0 by default) */
        msg.arg1 = phys_lba;
        msg.arg2 = count;
    }
    else
    {
        msg.arg0 = phys_lba;
        msg.arg1 = count;
        msg.arg2 = disk_selector;   /* ATA disk id (0 by default, step-1 extension) */
    }

    if (dob_ipc_call(ata_port, &msg, &reply) != DOB_OK)
        return false;

    if (reply.payload && reply.payload_size >= count * SECTOR_SIZE)
    {
        memcpy(buf, reply.payload, count * SECTOR_SIZE);
        return true;
    }
    return false;
}

static bool
disk_write_sectors(uint32_t lba, uint32_t count, const void *buf)
{
    /* Live mode is read-only by design: there is no write syscall
     * for the ramdisk and no semantic for a "session-local" change
     * that would survive nothing. Anything that lands here in live
     * mode is a programming error somewhere upstream — log it once
     * via debug_print so it surfaces. */
    if (live_mode)
    {
        (void)lba; (void)count; (void)buf;
        debug_print("[DobFileSystem] disk_write rejected: live mode is read-only\n");
        return false;
    }

    find_ata_driver();
    if (!ata_port) { debug_print("[DobFileSystem] disk_write: no ata_port\n"); return false; }

    /* Apply partition offset */
    uint32_t phys_lba = lba + fs.partition_lba;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = 2;
    msg.payload = (void *)buf;
    msg.payload_size = count * SECTOR_SIZE;

    if (use_ahci)
    {
        msg.arg0 = disk_selector;
        msg.arg1 = phys_lba;
        msg.arg2 = count;
    }
    else
    {
        msg.arg0 = phys_lba;
        msg.arg1 = count;
        msg.arg2 = disk_selector;
    }

    dob_status_t ret = dob_ipc_call(ata_port, &msg, &reply);
    if (ret != DOB_OK)
    {
        debug_print("[DobFileSystem] disk_write FAILED (IPC)\n");
        return false;
    }
    /* THE missing check: dob_ipc_call's DOB_OK only says the message
     * round-tripped — the HANDLER's verdict is reply.code. Ignoring it
     * turned every provider-side write error into a silent success:
     * field symptom, a freshly created file that stays forever empty
     * (the FAT flush was being refused by the provider and nobody
     * noticed). */
    if (reply.code != DOB_OK)
    {
        char m[96];
        sprintf(m, "[DobFileSystem] disk_write: provider error %d "
                   "(lba=%u n=%u)\n", (int)reply.code, phys_lba, count);
        debug_print(m);
        return false;
    }
    return true;
}

/* Commit the device write cache to NAND. Secondary (removable) mounts only:
 * the USB block service (usbms) implements op 4 = FLUSH; the boot ATA path
 * is left exactly as before. Best-effort — a flush failure must never fail
 * the operation that triggered it, so the result is ignored. Paired with
 * usbms dropping its per-write SYNCHRONIZE CACHE: the FS now decides WHEN to
 * commit (called from fat_flush, i.e. once per close / metadata change),
 * instead of the device committing after every single data batch. */
static void disk_flush(void)
{
    if (!mount_secondary || live_mode) return;
    find_ata_driver();
    if (!ata_port) return;
    dob_msg_t msg = {0}, reply = {0};
    msg.code = 4;   /* FLUSH */
    dob_ipc_call(ata_port, &msg, &reply);
}

/* Read a raw sector WITHOUT partition offset (for MBR detection) */
static bool
disk_read_raw_sector(uint32_t lba, void *buf)
{
    if (live_mode)
        return syscall3(SYS_LIVE_READ, (int)lba, 1, (int)(uintptr_t)buf) == 0;

    find_ata_driver();
    if (!ata_port) return false;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = 1;
    if (use_ahci) { msg.arg0 = disk_selector; msg.arg1 = lba; msg.arg2 = 1; }
    else          { msg.arg0 = lba; msg.arg1 = 1; msg.arg2 = disk_selector; }

    if (dob_ipc_call(ata_port, &msg, &reply) != DOB_OK) return false;
    if (!reply.payload || reply.payload_size < SECTOR_SIZE) return false;
    memcpy(buf, reply.payload, SECTOR_SIZE);
    return true;
}

/* Detect MBR partition table and set fs.partition_lba.
 * If sector 0 has MBR signature (0x55AA) and a valid partition entry,
 * use the first partition's start LBA. Otherwise assume unpartitioned. */
static void detect_partition(void)
{
    fs.partition_lba = 0;

    uint8_t mbr[SECTOR_SIZE];
    if (!disk_read_raw_sector(0, mbr))
    {
        debug_print("[DobFileSystem] Cannot read sector 0 for partition detection.\n");
        return;
    }

    /* Check MBR signature */
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
    {
        debug_print("[DobFileSystem] No MBR signature — assuming unpartitioned disk.\n");
        return;
    }

    /* Scan ALL FOUR primary partition slots for the first FAT32
     * (type 0x0B / 0x0C), not just slot 0. This MUST match the kernel's
     * bootfs_init() logic: the installer does not guarantee the system
     * partition lands in slot 0 — on the Compaq install it sits in slot 1.
     * The kernel was already fixed to scan all slots (that is why it loads
     * the modules correctly), but detect_partition here used to read only
     * slot 0. The result: the kernel found the system in slot 1 and booted,
     * but DobFileSystem mounted slot 0 — a different, valid-but-wrong FAT32
     * partition with no /SYSTEM tree — so the mount "succeeded" yet every
     * DAS/settings lookup came up empty: no DAS, no settings, no GUI.
     * On a single-partition disk (e.g. QEMU) slot 0 is the system and the
     * old code happened to work, which is why this never reproduced there.
     *
     * The MBR table is four 16-byte entries at offset 446; type byte at +4,
     * start-LBA dword at +8. */
    for (int slot = 0; slot < 4; slot++)
    {
        uint8_t *part = mbr + 446 + slot * 16;
        uint8_t type = part[4];
        if (type == 0x0B || type == 0x0C)
        {
            uint32_t start_lba = part[8] | (part[9] << 8)
                               | (part[10] << 16) | (part[11] << 24);
            if (start_lba > 0 && start_lba < 0xFFFFFFFF)
            {
                fs.partition_lba = start_lba;
                {
                    char d[64];
                    snprintf(d, sizeof(d),
                             "[DobFileSystem] FAT32 in slot %d at LBA %u\n",
                             slot, start_lba);
                    debug_print(d);
                }
                return;
            }
        }
    }

    /* Check if sector 0 itself has a valid FAT32 BPB (unpartitioned / superfloppy) */
    if (mbr[0] == 0xEB || mbr[0] == 0xE9)
    {
        /* Looks like a BPB jump instruction — unpartitioned FAT32 */
        return;
    }

    debug_print("[DobFileSystem] MBR present but no FAT32 partition found.\n");
}

/* FAT32 Geometry Helpers */

/* Convert cluster number to first sector LBA */
static uint32_t cluster_to_lba(uint32_t cluster)
{
    return fs.data_start_lba + (cluster - 2) * fs.sectors_per_cluster;
}

/* Small cluster cache.
 *
 * Reading a file means walking a directory chain, and on this PIO path
 * every cluster read is a per-sector IPC round-trip to the ATA driver —
 * the dominant cost at boot. Without a cache, opening each of the dozen
 * DAS files (and the settings, and desktop resources) re-reads the very
 * same system directories from disk over and over: root, then SYSTEM,
 * then CONFIG, then DAS, for every single lookup. Those directories fit
 * in a handful of clusters, so a tiny cache collapses dozens of disk
 * reads into a few.
 *
 * A few slots (not one) so a full path — root / SYSTEM / CONFIG / DAS —
 * stays resident instead of evicting itself as the walk descends. Simple
 * LRU. write_cluster updates or invalidates the matching slot so the
 * cache can never serve stale data after a directory or file mutation;
 * correctness is never traded for speed. The cache is dropped wholesale
 * on (re)mount, when fs geometry changes underneath it. */
#define CLUSTER_CACHE_SLOTS  3
#define CLUSTER_CACHE_BYTES  MAX_CLUSTER_BYTES   /* max cluster this fs supports (64 KB) */

static struct {
    uint32_t cluster;                       /* 0 = empty slot */
    uint32_t lru;                           /* higher = more recently used */
    uint8_t  data[CLUSTER_CACHE_BYTES];
} cluster_cache[CLUSTER_CACHE_SLOTS];
static uint32_t cluster_cache_clock = 0;    /* monotonic LRU stamp */

static void cluster_cache_reset(void)
{
    for (int i = 0; i < CLUSTER_CACHE_SLOTS; i++)
    {
        cluster_cache[i].cluster = 0;
        cluster_cache[i].lru     = 0;
    }
    cluster_cache_clock = 0;
}

/* Drop one cluster from the cache. Used by the few paths that write a
 * cluster's sectors directly (cluster allocation zero-fill) instead of
 * through write_cluster — a freshly allocated cluster may reuse a number
 * that was previously cached as a directory or file cluster. */
static void cluster_cache_invalidate(uint32_t cluster)
{
    for (int i = 0; i < CLUSTER_CACHE_SLOTS; i++)
        if (cluster_cache[i].cluster == cluster)
        {
            cluster_cache[i].cluster = 0;
            return;
        }
}

/* Read one full cluster into buf, via the cache. */
static bool read_cluster(uint32_t cluster, void *buf)
{
    if (cluster < 2) return false;

    uint32_t bytes = fs.sectors_per_cluster * SECTOR_SIZE;

    /* Hit? */
    for (int i = 0; i < CLUSTER_CACHE_SLOTS; i++)
    {
        if (cluster_cache[i].cluster == cluster)
        {
            memcpy(buf, cluster_cache[i].data, bytes);
            cluster_cache[i].lru = ++cluster_cache_clock;
            return true;
        }
    }

    /* Miss: read from disk. */
    if (!disk_read_sectors(cluster_to_lba(cluster), fs.sectors_per_cluster, buf))
        return false;

    /* Insert into the LRU slot (prefer an empty one). */
    int victim = 0;
    for (int i = 0; i < CLUSTER_CACHE_SLOTS; i++)
    {
        if (cluster_cache[i].cluster == 0) { victim = i; break; }
        if (cluster_cache[i].lru < cluster_cache[victim].lru) victim = i;
    }
    if (bytes <= sizeof(cluster_cache[victim].data))
    {
        cluster_cache[victim].cluster = cluster;
        cluster_cache[victim].lru     = ++cluster_cache_clock;
        memcpy(cluster_cache[victim].data, buf, bytes);
    }
    return true;
}

/* Write one full cluster from buf, keeping the cache coherent. */
static bool write_cluster(uint32_t cluster, const void *buf)
{
    if (cluster < 2) return false;

    uint32_t bytes = fs.sectors_per_cluster * SECTOR_SIZE;

    if (!disk_write_sectors(cluster_to_lba(cluster), fs.sectors_per_cluster, buf))
        return false;

    /* Keep any cached copy in sync with what we just wrote. */
    for (int i = 0; i < CLUSTER_CACHE_SLOTS; i++)
    {
        if (cluster_cache[i].cluster == cluster)
        {
            if (bytes <= sizeof(cluster_cache[i].data))
                memcpy(cluster_cache[i].data, buf, bytes);
            else
                cluster_cache[i].cluster = 0;   /* can't hold it: drop it */
            cluster_cache[i].lru = ++cluster_cache_clock;
            break;
        }
    }
    return true;
}

/* FAT Table Access (Cached) */

/* Ensure the FAT sector containing 'cluster' is in cache */
static void fat_flush(void);   /* fwd: fat_cache_load flushes the old window */

static bool fat_cache_load(uint32_t cluster)
{
    /* Each FAT sector holds 128 cluster entries (512 / 4) */
    uint32_t fat_sector_offset = cluster / 128;
    uint32_t cache_base = (fat_sector_offset / FAT_CACHE_SECTORS) * FAT_CACHE_SECTORS;
    uint32_t cache_lba = fs.fat_start_lba + cache_base;

    /* Guard: a cluster whose FAT entry would sit at or past the end of
     * the FAT is out of range -- it does not exist in this filesystem.
     * Without this, the n = fat_end - cache_lba lines below underflow
     * (cache_lba > fat_end yields a ~4-billion sector count), the driver
     * rejects the read, and the caller misreads the failure as data.
     * Refuse cleanly instead. fat_read() turns a false return into
     * FAT32_EOC, which safely terminates any chain walk. */
    if (cache_lba >= fs.fat_start_lba + fs.fat_size_sectors)
        return false;

    if (cache_lba == fat_cache_start)
        return true;  /* Already cached */

    /* Flush the old window's dirty range before loading a new range */
    fat_flush();

    /* Load new range */
    uint32_t n = FAT_CACHE_SECTORS;
    if (cache_lba + n > fs.fat_start_lba + fs.fat_size_sectors)
        n = (fs.fat_start_lba + fs.fat_size_sectors) - cache_lba;

    if (!disk_read_sectors(cache_lba, n, fat_cache))
        return false;

    fat_cache_start = cache_lba;
    return true;
}

/* Read FAT entry for a cluster */
static uint32_t fat_read(uint32_t cluster)
{
    if (!fat_cache_load(cluster)) return FAT32_EOC;

    uint32_t fat_sector_offset = cluster / 128;
    uint32_t cache_base = (fat_sector_offset / FAT_CACHE_SECTORS) * FAT_CACHE_SECTORS;
    uint32_t index_in_cache = cluster - (cache_base * 128);

    return fat_cache[index_in_cache] & 0x0FFFFFFF;
}

/* Write FAT entry for a cluster */
static bool fat_write(uint32_t cluster, uint32_t value)
{
    if (!fat_cache_load(cluster)) return false;

    uint32_t fat_sector_offset = cluster / 128;
    uint32_t cache_base = (fat_sector_offset / FAT_CACHE_SECTORS) * FAT_CACHE_SECTORS;
    uint32_t index_in_cache = cluster - (cache_base * 128);

    /* Preserve upper 4 bits */
    fat_cache[index_in_cache] = (fat_cache[index_in_cache] & 0xF0000000) | (value & 0x0FFFFFFF);

    /* Extend the dirty range to cover this entry's sector */
    uint32_t dsec = index_in_cache / (SECTOR_SIZE / 4);   /* 128 entries/sector */
    if (!fat_cache_dirty)         fat_dirty_lo = fat_dirty_hi = dsec;
    else if (dsec < fat_dirty_lo) fat_dirty_lo = dsec;
    else if (dsec > fat_dirty_hi) fat_dirty_hi = dsec;
    fat_cache_dirty = true;
    return true;
}

/* Flush FAT cache to disk */
static void fat_flush(void)
{
    if (!fat_cache_dirty || fat_cache_start == 0xFFFFFFFF) return;

    /* Write back ONLY the dirty sector range, not the whole 128-sector window.
     * A typical save touches a handful of FAT sectors; flushing the full window
     * x2 copies meant ~256 sector writes over USB at every close (the seconds
     * of "stuck at 100%"). */
    uint32_t off     = fat_dirty_lo;
    uint32_t n       = fat_dirty_hi - fat_dirty_lo + 1;
    uint32_t fat_end = fs.fat_start_lba + fs.fat_size_sectors;
    if (fat_cache_start + off + n > fat_end)
        n = fat_end - (fat_cache_start + off);
    uint32_t  lba = fat_cache_start + off;
    uint32_t *buf = fat_cache + off * (SECTOR_SIZE / 4);

    if (!disk_write_sectors(lba, n, buf))
        debug_print("[dobfs] FAT FLUSH FAILED (copy 1)\n");
    if (fs.num_fats > 1 &&
        !disk_write_sectors(lba + fs.fat_size_sectors, n, buf))
        debug_print("[dobfs] FAT FLUSH FAILED (copy 2)\n");

    fat_cache_dirty = false;

    /* Metadata just landed in the device's write cache. On a removable mount
     * commit the whole cache to NAND now — a single SYNCHRONIZE CACHE flushes
     * the file data, the dirent and the FAT together — so a pulled stick stays
     * consistent. No-op on the boot FS (left to the ATA path as before). */
    disk_flush();
}

/* Check if cluster is end-of-chain */
static bool fat_is_eoc(uint32_t value)
{
    return value >= FAT32_EOC_MIN;
}

/* Hint: last known free cluster, avoids scanning from 2 every time */
static uint32_t next_free_hint = 2;

/* Allocate one free cluster, link it after 'prev' (or 0 for new chain).
 * If 'zero' is true, the cluster data is zeroed (needed for directories). */
static uint32_t fat_alloc_cluster(uint32_t prev, bool zero)
{
    /* Scan from hint, then wrap around */
    uint32_t limit = fs.total_clusters + 2;
    uint32_t start = (next_free_hint >= 2 && next_free_hint < limit) ? next_free_hint : 2;

    for (uint32_t pass = 0; pass < 2; pass++)
    {
        uint32_t begin = (pass == 0) ? start : 2;
        uint32_t end   = (pass == 0) ? limit : start;
        for (uint32_t c = begin; c < end; c++)
        {
            if (fat_read(c) == FAT32_FREE)
            {
                fat_write(c, FAT32_EOC);
                if (prev >= 2)
                    fat_write(prev, c);

                if (zero)
                {
                    /* Bulk zero: one write for the entire cluster */
                    memset(cluster_work_buf, 0, fs.bytes_per_cluster);
                    disk_write_sectors(cluster_to_lba(c),
                                       fs.sectors_per_cluster, cluster_work_buf);
                    cluster_cache_invalidate(c);  /* reused number: drop stale copy */
                }

                next_free_hint = c + 1;
                return c;
            }
        }
    }
    return 0;  /* Disk full */
}

/* Free entire cluster chain starting at 'start' */
static void fat_free_chain(uint32_t start)
{
    uint32_t c = start;
    while (c >= 2 && !fat_is_eoc(c) && c != FAT32_BAD)
    {
        uint32_t next = fat_read(c);
        fat_write(c, FAT32_FREE);
        c = next;
    }
}

/* 8.3 Name Encoding/Decoding */

/* Convert a FAT32 8.3 directory entry name to readable string */
static void decode_83_name(const fat32_dirent_t *de, char *out)
{
    int pos = 0;

    /* Copy name, trim trailing spaces */
    for (int i = 0; i < 8; i++)
    {
        if (de->name[i] == ' ') break;
        out[pos++] = de->name[i];
    }

    /* Extension */
    if (de->ext[0] != ' ')
    {
        out[pos++] = '.';
        for (int i = 0; i < 3; i++)
        {
            if (de->ext[i] == ' ') break;
            out[pos++] = de->ext[i];
        }
    }

    out[pos] = '\0';

    /* Convert to lowercase for display (common convention) */
    for (int i = 0; out[i]; i++)
    {
        if (out[i] >= 'A' && out[i] <= 'Z')
            out[i] += 32;
    }
}

/* Encode a filename into FAT32 8.3 format (uppercase, space-padded) */
static bool encode_83_name(const char *name, char out_name[8], char out_ext[3])
{
    memset(out_name, ' ', 8);
    memset(out_ext, ' ', 3);

    if (!name || name[0] == '\0' || name[0] == '.') return false;

    /* Find dot for extension */
    const char *dot = NULL;
    for (int i = (int)strlen(name) - 1; i >= 0; i--)
    {
        if (name[i] == '.') { dot = &name[i]; break; }
    }

    /* Copy base name (up to 8 chars) */
    int len = dot ? (int)(dot - name) : (int)strlen(name);
    if (len > 8) len = 8;
    for (int i = 0; i < len; i++)
    {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;  /* Uppercase */
        out_name[i] = c;
    }

    /* Copy extension (up to 3 chars) */
    if (dot)
    {
        const char *e = dot + 1;
        int elen = (int)strlen(e);
        if (elen > 3) elen = 3;
        for (int i = 0; i < elen; i++)
        {
            char c = e[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            out_ext[i] = c;
        }
    }

    return true;
}

/* LFN (Long Filename) Support */

/* Compute 8.3 checksum for LFN verification */
static uint8_t lfn_checksum(const char name[8], const char ext[3])
{
    uint8_t sum = 0;
    for (int i = 0; i < 8; i++)
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)name[i];
    for (int i = 0; i < 3; i++)
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)ext[i];
    return sum;
}

/* Extract UCS-2 chars from an LFN entry into a buffer */
static void lfn_extract_chars(const fat32_lfn_t *lfn, char *buf, int offset)
{
    int p = offset;
    for (int i = 0; i < 5; i++) buf[p++] = (char)(lfn->name1[i] & 0xFF);
    for (int i = 0; i < 6; i++) buf[p++] = (char)(lfn->name2[i] & 0xFF);
    for (int i = 0; i < 2; i++) buf[p++] = (char)(lfn->name3[i] & 0xFF);
}

/* Directory Operations */

/* Callback for directory iteration */
typedef bool (*dir_iter_fn)(fat32_dirent_t *de, const char *long_name,
                            uint32_t dir_cluster, uint32_t entry_index,
                            void *ctx);

/* Iterate over all entries in a directory.
 * Assembles LFN entries and passes the final result to the callback.
 * Stops if callback returns false. */
static bool dir_iterate(uint32_t dir_cluster, dir_iter_fn fn, void *ctx)
{
    uint8_t *cluster_buf = cluster_iter_buf;
    if (fs.bytes_per_cluster > MAX_CLUSTER_BYTES) return false;

    char lfn_buf[280];  /* 20 LFN entries * 13 chars = 260 max + safety margin */
    bool has_lfn = false;
    uint32_t cluster = dir_cluster;
    uint32_t global_index = 0;

    while (cluster >= 2 && !fat_is_eoc(cluster))
    {
        if (!read_cluster(cluster, cluster_buf))
            return false;

        /* Determine if this is the last cluster in the dir chain. In the
         * middle of a multi-cluster directory, a 0x00 first-byte does NOT
         * mean end-of-dir — it just means "this slot is empty, skip it."
         * Only in the final cluster does 0x00 signal the true EOD.
         *
         * Prior versions returned at the first 0x00 unconditionally, which
         * made any directory whose last slot of cluster N was 0x00 invisible
         * past that slot — even if cluster N+1 had real entries. Happened
         * reliably for files added via the dir_add_entry "extend" path,
         * because that path leaves a trailing 0x00 in the old cluster. */
        uint32_t next_cluster = fat_read(cluster);
        bool is_last_cluster = fat_is_eoc(next_cluster);

        uint32_t entries = fs.bytes_per_cluster / DIR_ENTRY_SIZE;
        fat32_dirent_t *de = (fat32_dirent_t *)cluster_buf;

        for (uint32_t i = 0; i < entries; i++, de++, global_index++)
        {
            /* End of directory — but only if this is the last cluster.
             * Mid-chain 0x00 is just a vacated slot; keep scanning. */
            if (de->name[0] == 0x00)
            {
                if (is_last_cluster)
                    return true;
                has_lfn = false;
                continue;
            }

            /* Deleted entry */
            if ((uint8_t)de->name[0] == 0xE5)
            {
                has_lfn = false;
                continue;
            }

            /* LFN entry */
            if (de->attr == ATTR_LFN)
            {
                fat32_lfn_t *lfn = (fat32_lfn_t *)de;
                int seq = lfn->order & 0x3F;
                if (lfn->order & 0x40)
                {
                    /* First (last in sequence) LFN entry */
                    memset(lfn_buf, 0, sizeof(lfn_buf));
                    has_lfn = true;
                }
                if (has_lfn && seq > 0 && seq <= 20)
                    lfn_extract_chars(lfn, lfn_buf, (seq - 1) * 13);
                continue;
            }

            /* Skip volume label */
            if (de->attr & ATTR_VOLUME_ID)
            {
                has_lfn = false;
                continue;
            }

            /* Regular entry: provide long name or decoded 8.3 */
            char short_name[13];
            decode_83_name(de, short_name);

            const char *display_name = has_lfn ? lfn_buf : short_name;
            if (!fn(de, display_name, cluster, i, ctx))
                return false;

            has_lfn = false;
        }

        cluster = next_cluster;
    }

    return true;
}

/* --- Find entry by name --- */

typedef struct
{
    const char     *target;
    fat32_dirent_t  result;
    uint32_t        result_cluster;
    uint32_t        result_index;
    bool            found;
} find_ctx_t;

static bool find_entry_cb(fat32_dirent_t *de, const char *long_name,
                           uint32_t dir_cluster, uint32_t entry_index, void *ctx)
{
    find_ctx_t *fc = (find_ctx_t *)ctx;

    /* Case-insensitive compare */
    const char *a = fc->target;
    const char *b = long_name;
    bool match = true;
    while (*a && *b)
    {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) { match = false; break; }
        a++; b++;
    }
    if (*a || *b) match = false;

    if (match)
    {
        fc->result = *de;
        fc->result_cluster = dir_cluster;
        fc->result_index = entry_index;
        fc->found = true;
        return false;  /* Stop iteration */
    }
    return true;
}

/* Find entry 'name' in directory at 'dir_cluster'.
 * Returns true if found, fills 'out_de', 'out_cluster', 'out_index'. */
static bool dir_find_entry(uint32_t dir_cluster, const char *name,
                            fat32_dirent_t *out_de,
                            uint32_t *out_cluster, uint32_t *out_index)
{
    find_ctx_t ctx;
    ctx.target = name;
    ctx.found = false;
    dir_iterate(dir_cluster, find_entry_cb, &ctx);
    if (ctx.found)
    {
        if (out_de) *out_de = ctx.result;
        if (out_cluster) *out_cluster = ctx.result_cluster;
        if (out_index) *out_index = ctx.result_index;
    }
    return ctx.found;
}

/* --- LFN Write Support --- */

/* Check if a filename requires Long Filename entries.
 * Returns true if the name cannot be represented as a valid 8.3 entry.
 * FAT32 8.3 rules: max 8 base + 3 ext, uppercase A-Z 0-9 and limited
 * punctuation only, no leading dot, exactly 0 or 1 dot. */
static bool
needs_lfn(const char *name)
{
    int len = (int)strlen(name);
    if (len == 0 || len > 12)
        return true;

    /* Leading dot not allowed in 8.3 */
    if (name[0] == '.')
        return true;

    /* Find last dot for extension split */
    const char *dot = NULL;
    int dot_count = 0;
    for (int i = 0; i < len; i++)
    {
        if (name[i] == '.')
        {
            dot = &name[i];
            dot_count++;
        }
    }

    /* Multiple dots require LFN */
    if (dot_count > 1)
        return true;

    int base_len = dot ? (int)(dot - name) : len;
    int ext_len = dot ? (len - base_len - 1) : 0;

    if (base_len == 0 || base_len > 8 || ext_len > 3)
        return true;

    /* Check for characters invalid in 8.3 */
    for (int i = 0; i < len; i++)
    {
        char c = name[i];
        if (c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' ||
            c == '[' || c == ']')
            return true;

        /* 8.3 entries are stored uppercase. Any lowercase letter means
         * the original case would be lost without LFN. Names like
         * "Desktop", "Music", "Pictures" need LFN to preserve case. */
        if (c >= 'a' && c <= 'z')
            return true;
    }

    return false;
}

/* Generate a unique 8.3 short name for an LFN entry.
 * Strips invalid chars, takes first 6 of base, appends ~N.
 * Checks uniqueness against dir_cluster via dir_find_entry. */
static void
generate_short_name(const char *long_name, uint32_t dir_cluster,
                    char out_name[8], char out_ext[3])
{
    memset(out_name, ' ', 8);
    memset(out_ext, ' ', 3);

    int len = (int)strlen(long_name);

    /* Find last dot */
    const char *dot = NULL;
    for (int i = len - 1; i >= 0; i--)
    {
        if (long_name[i] == '.')
        {
            dot = &long_name[i];
            break;
        }
    }

    /* Extract and uppercase the extension */
    if (dot)
    {
        const char *e = dot + 1;
        int elen = (int)strlen(e);
        if (elen > 3) elen = 3;
        int epos = 0;
        for (int i = 0; i < elen; i++)
        {
            char c = e[i];
            if (c == ' ') continue;
            if (c >= 'a' && c <= 'z') c -= 32;
            out_ext[epos++] = c;
        }
    }

    /* Build basis name: strip spaces, periods, and invalid chars, then uppercase */
    char basis[64];
    int bpos = 0;
    int base_end = dot ? (int)(dot - long_name) : len;
    for (int i = 0; i < base_end && bpos < 63; i++)
    {
        char c = long_name[i];
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        basis[bpos++] = c;
    }
    basis[bpos] = '\0';

    /* Try ~1 through ~99 until we find a unique name */
    for (int n = 1; n <= 99; n++)
    {
        char suffix[8];
        int slen = 0;
        suffix[slen++] = '~';
        if (n >= 10)
            suffix[slen++] = '0' + (n / 10);
        suffix[slen++] = '0' + (n % 10);
        suffix[slen] = '\0';

        int base_chars = 8 - slen;
        if (base_chars > bpos) base_chars = bpos;

        memset(out_name, ' ', 8);
        for (int i = 0; i < base_chars; i++)
            out_name[i] = basis[i];
        for (int i = 0; i < slen; i++)
            out_name[base_chars + i] = suffix[i];

        /* Decode to readable string and check uniqueness */
        char check[13];
        int cp = 0;
        for (int i = 0; i < 8; i++)
        {
            if (out_name[i] == ' ') break;
            check[cp++] = out_name[i];
        }
        if (out_ext[0] != ' ')
        {
            check[cp++] = '.';
            for (int i = 0; i < 3; i++)
            {
                if (out_ext[i] == ' ') break;
                check[cp++] = out_ext[i];
            }
        }
        check[cp] = '\0';

        fat32_dirent_t tmp;
        if (!dir_find_entry(dir_cluster, check, &tmp, NULL, NULL))
            return;  /* Name is unique */
    }
}

/* Build a single LFN directory entry.
 * seq: 1-based sequence (seq=1 covers chars 0..12, seq=2 covers 13..25, etc.)
 * is_last: if true, sets the 0x40 flag (final LFN entry in the sequence) */
static void
build_lfn_entry(fat32_lfn_t *lfn, int seq, bool is_last,
                const char *name, uint8_t checksum)
{
    /* Pad entire struct with 0xFF first */
    memset(lfn, 0xFF, sizeof(fat32_lfn_t));

    lfn->order = (uint8_t)seq;
    if (is_last)
        lfn->order |= 0x40;
    lfn->attr = ATTR_LFN;
    lfn->type = 0;
    lfn->checksum = checksum;
    lfn->zero = 0;

    int name_len = (int)strlen(name);
    int offset = (seq - 1) * 13;

    /* name1: chars 0..4 of this segment */
    for (int i = 0; i < 5; i++)
    {
        int ci = offset + i;
        if (ci < name_len)
            lfn->name1[i] = (uint16_t)(unsigned char)name[ci];
        else if (ci == name_len)
            lfn->name1[i] = 0x0000;
        else
            lfn->name1[i] = 0xFFFF;
    }

    /* name2: chars 5..10 */
    for (int i = 0; i < 6; i++)
    {
        int ci = offset + 5 + i;
        if (ci < name_len)
            lfn->name2[i] = (uint16_t)(unsigned char)name[ci];
        else if (ci == name_len)
            lfn->name2[i] = 0x0000;
        else
            lfn->name2[i] = 0xFFFF;
    }

    /* name3: chars 11..12 */
    for (int i = 0; i < 2; i++)
    {
        int ci = offset + 11 + i;
        if (ci < name_len)
            lfn->name3[i] = (uint16_t)(unsigned char)name[ci];
        else if (ci == name_len)
            lfn->name3[i] = 0x0000;
        else
            lfn->name3[i] = 0xFFFF;
    }
}

/* --- Add entry to directory --- */

/* Write a set of LFN entries + one 8.3 short entry into a directory cluster buffer.
 * 'de' points to the first slot; there must be (lfn_count + 1) contiguous slots. */
static void
write_dir_entries(fat32_dirent_t *de, uint32_t slot,
                  const char *name, int lfn_count,
                  const char n83[8], const char e83[3],
                  uint32_t first_cluster, uint32_t size, uint8_t attr)
{
    uint8_t chk = lfn_checksum(n83, e83);

    /* LFN entries: written in reverse sequence order (highest seq first) */
    for (int l = lfn_count; l >= 1; l--)
    {
        build_lfn_entry((fat32_lfn_t *)&de[slot], l,
                        (l == lfn_count), name, chk);
        slot++;
    }

    /* 8.3 short entry */
    memset(&de[slot], 0, DIR_ENTRY_SIZE);
    memcpy(de[slot].name, n83, 8);
    memcpy(de[slot].ext, e83, 3);
    de[slot].attr = attr;
    de[slot].first_cluster_hi = (uint16_t)(first_cluster >> 16);
    de[slot].first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    de[slot].file_size = size;
}

static bool
dir_add_entry(uint32_t dir_cluster, const char *name,
              uint32_t first_cluster, uint32_t size, uint8_t attr)
{
    bool use_lfn = needs_lfn(name);

    char n83[8], e83[3];
    int lfn_count = 0;
    uint32_t total_slots;

    if (use_lfn)
    {
        generate_short_name(name, dir_cluster, n83, e83);
        lfn_count = ((int)strlen(name) + 12) / 13;
        total_slots = (uint32_t)lfn_count + 1;
    }
    else
    {
        if (!encode_83_name(name, n83, e83))
            return false;
        total_slots = 1;
    }

    /* Scan directory for a contiguous run of free slots */
    uint8_t *cluster_buf = cluster_work_buf;
    uint32_t cluster = dir_cluster;

    while (cluster >= 2 && !fat_is_eoc(cluster))
    {
        if (!read_cluster(cluster, cluster_buf))
            return false;

        uint32_t entries_per = fs.bytes_per_cluster / DIR_ENTRY_SIZE;
        fat32_dirent_t *de = (fat32_dirent_t *)cluster_buf;

        uint32_t run_start = 0;
        uint32_t run_len = 0;

        for (uint32_t i = 0; i < entries_per; i++)
        {
            if (de[i].name[0] == 0x00 || (uint8_t)de[i].name[0] == 0xE5)
            {
                if (run_len == 0)
                    run_start = i;
                run_len++;

                if (run_len >= total_slots)
                {
                    write_dir_entries(de, run_start, name, lfn_count,
                                     n83, e83, first_cluster, size, attr);
                    write_cluster(cluster, cluster_buf);
                    return true;
                }
            }
            else
            {
                run_len = 0;
            }
        }

        uint32_t next = fat_read(cluster);
        if (fat_is_eoc(next))
        {
            /* Directory full in this cluster: extend with a new one.
             *
             * Spec note: once the directory spans multiple clusters, any
             * 0x00 first-byte slot in the cluster we're leaving becomes a
             * spec-violation — the FAT spec says 0x00 means "this slot is
             * free AND all subsequent slots in the directory are also free
             * (anywhere, including extended clusters)". A strict FAT reader
             * honouring this would stop at such a 0x00 and never discover
             * the new cluster's entries.
             *
             * dir_iterate tolerates mid-chain 0x00 for our own safety, but
             * to keep the on-disk image compatible with external FAT tools
             * (mtools, Linux fatfs, Windows) we also normalise here: every
             * 0x00 first-byte slot in the now-intermediate cluster gets
             * rewritten to 0xE5 ("free and reusable, but something follows").
             */
            fat32_dirent_t *patch = (fat32_dirent_t *)cluster_buf;
            bool patched = false;
            for (uint32_t k = 0; k < entries_per; k++)
            {
                if (patch[k].name[0] == 0x00)
                {
                    patch[k].name[0] = (char)0xE5;
                    patched = true;
                }
            }
            if (patched)
                write_cluster(cluster, cluster_buf);

            uint32_t new_cl = fat_alloc_cluster(cluster, true);
            if (new_cl == 0) return false;

            /* New cluster is zeroed by fat_alloc_cluster → all slots free */
            if (!read_cluster(new_cl, cluster_buf)) return false;
            fat32_dirent_t *de2 = (fat32_dirent_t *)cluster_buf;
            write_dir_entries(de2, 0, name, lfn_count,
                             n83, e83, first_cluster, size, attr);
            write_cluster(new_cl, cluster_buf);
            return true;
        }
        cluster = next;
    }

    return false;
}

/* --- Remove entry from directory --- */

/* Remove a directory entry AND its associated LFN entries.
 * Walks backwards from entry_index to mark all preceding LFN
 * entries as deleted (0xE5), preventing ghost LFN slot leaks. */
static bool dir_remove_entry(uint32_t dir_cluster, uint32_t entry_cluster,
                              uint32_t entry_index)
{
    (void)dir_cluster;  /* Kept in API for future cross-cluster LFN cleanup */
    uint8_t *cluster_buf = cluster_work_buf;
    if (!read_cluster(entry_cluster, cluster_buf))
        return false;

    fat32_dirent_t *de = (fat32_dirent_t *)cluster_buf;

    /* Mark the 8.3 entry as deleted */
    de[entry_index].name[0] = (char)0xE5;

    /* Walk backwards to delete associated LFN entries */
    if (entry_index > 0)
    {
        uint32_t i = entry_index - 1;
        while (1)
        {
            if (de[i].attr != ATTR_LFN)
                break;
            de[i].name[0] = (char)0xE5;
            if (i == 0) break;
            i--;
        }
    }

    return write_cluster(entry_cluster, cluster_buf);
}

/* --- Update entry size and cluster in directory --- */

static bool dir_update_entry(uint32_t entry_cluster, uint32_t entry_index,
                              uint32_t new_size, uint32_t new_first_cluster)
{
    uint8_t *cluster_buf = cluster_work_buf;
    if (!read_cluster(entry_cluster, cluster_buf))
        return false;

    fat32_dirent_t *de = &((fat32_dirent_t *)cluster_buf)[entry_index];
    de->file_size = new_size;
    if (new_first_cluster != 0xFFFFFFFF)
    {
        de->first_cluster_hi = (uint16_t)(new_first_cluster >> 16);
        de->first_cluster_lo = (uint16_t)(new_first_cluster & 0xFFFF);
    }

    return write_cluster(entry_cluster, cluster_buf);
}

/* Path Resolution */

/* Resolve a MainDOB path to a directory cluster + final component name.
 * Example: "/DATA/Documents/hello.txt" ->
 *   parent_cluster = cluster of "Documents" dir,
 *   out_basename = "hello.txt"
 * Returns true if the parent directory exists. */
static bool resolve_path(const char *path,
                          uint32_t *out_parent_cluster,
                          const char **out_basename)
{
    if (!path || path[0] != '/') return false;

    uint32_t cluster = fs.root_cluster;

    if (path[1] == '\0')
    {
        *out_parent_cluster = cluster;
        *out_basename = "";
        return true;
    }

    /* Find the last component by finding the last '/' */
    const char *last_sep = path;
    for (const char *s = path + 1; *s; s++)
    {
        if (*s == '/' && *(s + 1) != '\0')
            last_sep = s;
    }

    /* If last_sep == path, there's only one component after root */
    if (last_sep == path)
    {
        *out_parent_cluster = cluster;
        *out_basename = path + 1;
        /* Strip trailing slash if present */
        return true;
    }

    /* Walk all intermediate directories */
    const char *p = path + 1;
    while (p < last_sep + 1)
    {
        char component[DOBFS_PATH_MAX];
        int clen = 0;
        while (*p && *p != '/' && clen < DOBFS_PATH_MAX - 1)
            component[clen++] = *p++;
        component[clen] = '\0';
        if (*p == '/') p++;

        fat32_dirent_t de;
        if (!dir_find_entry(cluster, component, &de, NULL, NULL))
            return false;
        if (!(de.attr & ATTR_DIRECTORY))
            return false;

        cluster = DE_CLUSTER(de);
    }

    *out_parent_cluster = cluster;
    *out_basename = last_sep + 1;
    return true;
}

/* Resolve full path to the entry itself (not just parent).
 * Returns true if entry exists, fills dirent. */
static bool resolve_full_path(const char *path, fat32_dirent_t *out_de,
                               uint32_t *out_dir_cluster,
                               uint32_t *out_entry_index)
{
    if (strcmp(path, "/") == 0)
    {
        /* Root directory: synthesize a dirent */
        if (out_de)
        {
            memset(out_de, 0, sizeof(*out_de));
            out_de->attr = ATTR_DIRECTORY;
            out_de->first_cluster_hi = (uint16_t)(fs.root_cluster >> 16);
            out_de->first_cluster_lo = (uint16_t)(fs.root_cluster & 0xFFFF);
        }
        if (out_dir_cluster) *out_dir_cluster = fs.root_cluster;
        if (out_entry_index) *out_entry_index = 0;
        return true;
    }

    uint32_t parent_cluster;
    const char *basename;
    if (!resolve_path(path, &parent_cluster, &basename))
        return false;

    if (!basename || basename[0] == '\0')
    {
        /* Path is a directory itself (trailing slash) */
        if (out_de)
        {
            memset(out_de, 0, sizeof(*out_de));
            out_de->attr = ATTR_DIRECTORY;
            out_de->first_cluster_hi = (uint16_t)(parent_cluster >> 16);
            out_de->first_cluster_lo = (uint16_t)(parent_cluster & 0xFFFF);
        }
        return true;
    }

    uint32_t ec, ei;
    fat32_dirent_t de;
    if (!dir_find_entry(parent_cluster, basename, &de, &ec, &ei))
        return false;

    if (out_de) *out_de = de;
    if (out_dir_cluster) *out_dir_cluster = ec;
    if (out_entry_index) *out_entry_index = ei;
    return true;
}

/* Path Validation & Sandbox */

static bool validate_path(const char *path)
{
    if (!path || path[0] != '/') return false;
    if (strcmp(path, "/") == 0) return true;

    /* Secondary mounts hold arbitrary user content with no enforced
     * top-level structure. Any path is acceptable on those — the
     * sandbox_check that runs alongside is also a no-op for secondary
     * mounts, so write operations can land anywhere on the volume.
     *
     * The structural restriction below applies only to the root
     * mount, where /SYSTEM and /DATA are the canonical layout. */
    if (mount_secondary) return true;

    if (path_starts_with_ci(path, "/SYSTEM/", 8) || path_eq_ci(path, "/SYSTEM")) return true;
    if (path_starts_with_ci(path, "/DATA/", 6) || path_eq_ci(path, "/DATA")) return true;
    return false;
}

static bool path_is_safe(const char *path)
{
    const char *p = path;
    while (*p)
    {
        if (p[0] == '.' && p[1] == '.') return false;
        p++;
    }
    return true;
}

/* === exFAT support (Phase 3): volume routing via exfat.mem ===
 *
 * When a mounted volume is exFAT (rather than FAT32) DobFileSystem
 * delegates every operation — open/stat/read/readdir AND write/create/
 * mkdir/unlink/rename/truncate/flush — to the exfat.mem shared object.
 * The .mem is loaded once, lazily, the first time an exFAT volume is
 * seen. Secondary mounts (USB / data partitions) reach exfat.mem by
 * reading it from the already-mounted root FS via the dobfs stub, so
 * there is no circular dependency for the non-root case.
 *
 * exFAT is full read-write here: exfat.mem implements the write entries
 * (create/write/mkdir/unlink/rename/ftrunc), so write-intent operations
 * on an exFAT mount are served, not rejected. Live-CD mode stays
 * read-only by the same generic gate that covers FAT32. */
#define EXFAT_MEM_PATH   "/SYSTEM/OperatingSystem/DobFileSystem/exfat.mem"
#define EXFAT_BLOB_MAX   (192 * 1024)   /* generous: exfat.mem is tens of KB */

static struct
{
    bool            active;     /* the current mount is exFAT        */
    exfat_api_t    *api;        /* .mem vtable (loaded once, lazily) */
    exfat_volume_t *vol;        /* mounted volume handle             */
} ex;

/* exfat.mem sector callbacks: partition-relative 512-byte sectors,
 * straight through to the block layer (disk_*_sectors already apply the
 * partition offset). Return 0 on success, -1 on error. */
static int exfat_rd(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    return disk_read_sectors(lba, count, buf) ? 0 : -1;
}

static int exfat_wr(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    (void)ctx;
    return disk_write_sectors(lba, count, buf) ? 0 : -1;
}

/* Lazily load exfat.mem (once per process). */
static bool ensure_exfat_loaded(void)
{
    if (ex.api) return true;

    static uint8_t blob[EXFAT_BLOB_MAX];
    int fd = dobfs_Open(EXFAT_MEM_PATH, FS_READ);
    if (fd < 0)
    {
        debug_print("[DobFileSystem] cannot open exfat.mem\n");
        return false;
    }
    uint32_t total = 0;
    int got;
    while (total < sizeof(blob) &&
           (got = dobfs_Read(fd, blob + total, sizeof(blob) - total)) > 0)
        total += (uint32_t)got;
    dobfs_Close(fd);
    if (total == 0 || total == sizeof(blob))   /* empty, or too big to trust */
    {
        debug_print("[DobFileSystem] exfat.mem unreadable or oversize\n");
        return false;
    }

    ex.api = (exfat_api_t *)dob_mem_load(blob, total);
    if (!ex.api)
    {
        debug_print("[DobFileSystem] dob_mem_load(exfat.mem) failed\n");
        return false;
    }
    return true;
}

/* Drop any active exFAT mount. Idempotent. */
static void exfat_teardown(void)
{
    if (ex.active && ex.api && ex.vol)
        ex.api->unmount(ex.vol);
    ex.vol    = NULL;
    ex.active = false;
}

/* Mount the current volume as exFAT. Returns true on success. */
static bool exfat_try_mount(void)
{
    if (!ensure_exfat_loaded()) return false;
    exfat_teardown();
    ex.vol = ex.api->mount(exfat_rd, exfat_wr, NULL);
    if (!ex.vol) return false;
    ex.active  = true;
    fs.mounted = true;   /* satisfy the generic "mounted" gate; every routed
                          * handler checks ex.active and serves exFAT before
                          * touching the FAT32 `fs` geometry below */
    return true;
}

/* FAT32 Mount */

static bool fat32_mount(void)
{
    uint8_t boot_sector[SECTOR_SIZE];
    if (!disk_read_sectors(0, 1, boot_sector))
    {
        debug_print("[DobFileSystem] Failed to read boot sector\n");
        return false;
    }

    /* exFAT carries "EXFAT   " at byte offset 3 and none of the FAT32 BPB
     * fields. Detect it here and route the whole volume to exfat.mem. */
    {
        static const char exsig[8] = { 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' ' };
        bool is_ex = true;
        for (int i = 0; i < 8; i++)
            if (boot_sector[3 + i] != (uint8_t)exsig[i]) { is_ex = false; break; }
        if (is_ex)
            return exfat_try_mount();
    }
    /* Not exFAT: drop any stale exFAT mount and parse as FAT32. */
    exfat_teardown();

    fat32_bpb_t *bpb = (fat32_bpb_t *)boot_sector;

    /* Validate — every check below rejects a sector that looks like
     * garbage / non-FAT32. Without these, mounting a partition that
     * happens to contain random bytes lets through a "successful"
     * mount with absurd values for total_clusters (4 GiB-ish via
     * uint32 underflow), which then crashes any operation using it. */
    if (bpb->bytes_per_sector != SECTOR_SIZE)
    {
        debug_print("[DobFileSystem] Unsupported sector size\n");
        return false;
    }

    if (bpb->fat_size_16 != 0 || bpb->root_entry_count != 0)
    {
        debug_print("[DobFileSystem] Not FAT32 (looks like FAT12/16)\n");
        return false;
    }

    /* Sectors-per-cluster must be a power of 2, 1..128. */
    uint8_t spc = bpb->sectors_per_cluster;
    if (spc == 0 || (spc & (spc - 1)) != 0 || spc > 128)
    {
        debug_print("[DobFileSystem] Invalid sectors_per_cluster\n");
        return false;
    }

    /* num_fats: spec says 2; allow 1 too. Anything else is junk. */
    if (bpb->num_fats == 0 || bpb->num_fats > 2)
    {
        debug_print("[DobFileSystem] Invalid num_fats\n");
        return false;
    }

    /* reserved_sectors must be > 0 (sector 0 itself is part of the
     * reserved region). Typical value: 32. */
    if (bpb->reserved_sectors == 0)
    {
        debug_print("[DobFileSystem] Invalid reserved_sectors\n");
        return false;
    }

    /* fat_size_32 must be set (FAT32 always uses the 32-bit field).
     * total_sectors: per spec, *either* total_sectors_16 *or* total_sectors_32
     * is non-zero. Smaller volumes (< 65536 sectors) tend to use the
     * 16-bit field; larger ones use the 32-bit field. mkfs.fat picks
     * one based on volume size. Reject only if BOTH are zero. */
    if (bpb->fat_size_32 == 0)
    {
        debug_print("[DobFileSystem] Empty FAT size\n");
        return false;
    }
    uint32_t total_sectors = bpb->total_sectors_32;
    if (total_sectors == 0) total_sectors = bpb->total_sectors_16;
    if (total_sectors == 0)
    {
        debug_print("[DobFileSystem] Empty total sectors\n");
        return false;
    }

    /* Metadata mustn't exceed the partition size — protects the
     * subtraction below from underflowing. */
    uint64_t metadata = (uint64_t)bpb->reserved_sectors
                     + (uint64_t)bpb->num_fats * (uint64_t)bpb->fat_size_32;
    if (metadata >= total_sectors)
    {
        debug_print("[DobFileSystem] Metadata larger than partition: "
                    "not a valid FAT32 filesystem\n");
        return false;
    }

    /* Root cluster must be at least 2 (clusters 0 and 1 are reserved). */
    if (bpb->root_cluster < 2)
    {
        debug_print("[DobFileSystem] Invalid root_cluster\n");
        return false;
    }

    fs.sectors_per_cluster = bpb->sectors_per_cluster;
    fs.bytes_per_cluster = (uint32_t)bpb->sectors_per_cluster * SECTOR_SIZE;
    fs.fat_start_lba = bpb->reserved_sectors;
    fs.fat_size_sectors = bpb->fat_size_32;
    fs.num_fats = bpb->num_fats;
    fs.root_cluster = bpb->root_cluster;
    fs.partition_sectors = total_sectors;

    uint32_t total_data_sectors = total_sectors
        - bpb->reserved_sectors
        - (bpb->num_fats * bpb->fat_size_32);
    fs.data_start_lba = bpb->reserved_sectors + (bpb->num_fats * bpb->fat_size_32);
    fs.total_clusters = total_data_sectors / bpb->sectors_per_cluster;
    fs.mounted = true;

    /* Drop any cached clusters from a previous mount: geometry
     * (sectors_per_cluster, data_start_lba) may differ now. */
    cluster_cache_reset();

    return true;
}

/* FAT32 Auto-Format — creates a minimal filesystem on a blank disk */

static bool fat32_format(void)
{
    debug_print("[DobFileSystem] Formatting disk as FAT32...\n");

    /* Get disk size via ATA IDENTIFY */
    dob_msg_t msg = {0}, reply = {0};
    msg.code = 3;  /* IDENTIFY */
    if (dob_ipc_call(ata_port, &msg, &reply) != DOB_OK || !reply.payload)
    {
        debug_print("[DobFileSystem] Cannot identify disk\n");
        return false;
    }

    /* Copy identify data locally — next IPC call overwrites the buffer */
    uint16_t ident[256];
    memcpy(ident, reply.payload, SECTOR_SIZE);

    /* Words 60-61: total LBA28 sectors */
    uint32_t total_sectors = (uint32_t)ident[60] | ((uint32_t)ident[61] << 16);
    if (total_sectors == 0)
    {
        debug_print("[DobFileSystem] Disk reports 0 sectors\n");
        return false;
    }

    /* Compute geometry */
    uint8_t spc = 8;  /* Sectors per cluster = 4KB clusters */
    if (total_sectors > 524288) spc = 16;  /* >256MB: 8KB clusters */
    uint32_t reserved = 32;
    uint32_t num_fats = 2;

    /* FAT size: each cluster needs 4 bytes in FAT */
    uint32_t data_sectors_est = total_sectors - reserved;
    uint32_t clusters_est = data_sectors_est / spc;
    uint32_t fat_entries = clusters_est + 2;  /* clusters + 2 reserved */
    uint32_t fat_bytes = fat_entries * 4;
    uint32_t fat_sectors = (fat_bytes + SECTOR_SIZE - 1) / SECTOR_SIZE;

    uint32_t data_start = reserved + fat_sectors * num_fats;
    uint32_t data_sectors = total_sectors - data_start;
    uint32_t total_clusters = data_sectors / spc;
    uint32_t root_cluster = 2;

    /* 1. Write boot sector (BPB) */
    uint8_t boot[SECTOR_SIZE];
    memset(boot, 0, SECTOR_SIZE);

    boot[0] = 0xEB; boot[1] = 0x58; boot[2] = 0x90;  /* Jump */
    memcpy(boot + 3, "MSDOS5.0", 8);  /* OEM */

    /* BPB fields (little-endian) */
    boot[11] = (SECTOR_SIZE & 0xFF); boot[12] = (SECTOR_SIZE >> 8);  /* bytes_per_sector */
    boot[13] = spc;                     /* sectors_per_cluster */
    boot[14] = (reserved & 0xFF); boot[15] = (reserved >> 8);  /* reserved_sectors */
    boot[16] = (uint8_t)num_fats;       /* num_fats */
    boot[17] = 0; boot[18] = 0;        /* root_entry_count = 0 (FAT32) */
    boot[19] = 0; boot[20] = 0;        /* total_sectors_16 = 0 */
    boot[21] = 0xF8;                    /* media_type */
    boot[22] = 0; boot[23] = 0;        /* fat_size_16 = 0 (FAT32) */
    boot[24] = 63; boot[25] = 0;       /* sectors_per_track */
    boot[26] = 255; boot[27] = 0;      /* num_heads */
    /* hidden_sectors = 0 (bytes 28-31) */
    boot[32] = (total_sectors & 0xFF);
    boot[33] = ((total_sectors >> 8) & 0xFF);
    boot[34] = ((total_sectors >> 16) & 0xFF);
    boot[35] = ((total_sectors >> 24) & 0xFF);
    /* FAT32 extended */
    boot[36] = (fat_sectors & 0xFF);
    boot[37] = ((fat_sectors >> 8) & 0xFF);
    boot[38] = ((fat_sectors >> 16) & 0xFF);
    boot[39] = ((fat_sectors >> 24) & 0xFF);
    /* ext_flags = 0, fs_version = 0 */
    boot[44] = (root_cluster & 0xFF);
    boot[45] = ((root_cluster >> 8) & 0xFF);
    boot[46] = ((root_cluster >> 16) & 0xFF);
    boot[47] = ((root_cluster >> 24) & 0xFF);
    boot[48] = 1; boot[49] = 0;        /* fs_info_sector = 1 */
    boot[50] = 6; boot[51] = 0;        /* backup_boot_sector = 6 */
    boot[66] = 0x29;                    /* boot_sig */
    boot[67] = 0x12; boot[68] = 0x34; boot[69] = 0x56; boot[70] = 0x78; /* volume_id */
    memcpy(boot + 71, "MAINDOB    ", 11);  /* volume_label */
    memcpy(boot + 82, "FAT32   ", 8);      /* fs_type */
    boot[510] = 0x55; boot[511] = 0xAA;   /* signature */

    if (!disk_write_sectors(0, 1, boot))
    {
        debug_print("[DobFileSystem] Failed to write boot sector\n");
        return false;
    }

    /* Also write backup boot sector at sector 6 */
    disk_write_sectors(6, 1, boot);

    /* 2. Write FSInfo sector */
    uint8_t fsinfo[SECTOR_SIZE];
    memset(fsinfo, 0, SECTOR_SIZE);
    fsinfo[0] = 0x52; fsinfo[1] = 0x52; fsinfo[2] = 0x61; fsinfo[3] = 0x41;  /* RRaA */
    fsinfo[484] = 0x72; fsinfo[485] = 0x72; fsinfo[486] = 0x41; fsinfo[487] = 0x61;  /* rrAa */
    /* Free count */
    uint32_t free_clusters = total_clusters - 1;  /* minus root */
    fsinfo[488] = (free_clusters & 0xFF);
    fsinfo[489] = ((free_clusters >> 8) & 0xFF);
    fsinfo[490] = ((free_clusters >> 16) & 0xFF);
    fsinfo[491] = ((free_clusters >> 24) & 0xFF);
    /* Next free = 3 (after root) */
    fsinfo[492] = 3; fsinfo[493] = 0; fsinfo[494] = 0; fsinfo[495] = 0;
    fsinfo[510] = 0x55; fsinfo[511] = 0xAA;

    disk_write_sectors(1, 1, fsinfo);
    disk_write_sectors(7, 1, fsinfo);  /* backup */

    /* 3. Zero FAT tables and set entries 0-2 */
    uint8_t zero[SECTOR_SIZE];
    memset(zero, 0, SECTOR_SIZE);

    /* Zero both FATs */
    for (uint32_t f = 0; f < num_fats; f++)
    {
        uint32_t fat_base = reserved + f * fat_sectors;
        for (uint32_t s = 0; s < fat_sectors && s < 256; s++)
            disk_write_sectors(fat_base + s, 1, zero);
    }

    /* Set FAT[0], FAT[1], FAT[2] = EOC markers */
    uint8_t fat_first[SECTOR_SIZE];
    memset(fat_first, 0, SECTOR_SIZE);
    uint32_t *fat = (uint32_t *)fat_first;
    fat[0] = 0x0FFFFFF8;  /* Media type marker */
    fat[1] = 0x0FFFFFFF;  /* EOC */
    fat[2] = 0x0FFFFFFF;  /* Root directory cluster EOC */

    disk_write_sectors(reserved, 1, fat_first);
    disk_write_sectors(reserved + fat_sectors, 1, fat_first);  /* Second FAT */

    /* 4. Zero root directory cluster */
    for (uint32_t s = 0; s < spc; s++)
        disk_write_sectors(data_start + s, 1, zero);

    debug_print("[DobFileSystem] Format complete.\n");
    return true;
}

/* exFAT format — lay down a fresh exFAT volume on the disk via exfat.mem's
 * mkfs. The volume spans the disk from the partition base to its end. After
 * this, fat32_mount() detects the "EXFAT   " signature and mounts via exFAT. */
static bool exfat_format(const char *label)
{
    if (live_mode)
    {
        debug_print("[DobFileSystem] exFAT format rejected: live mode is read-only\n");
        return false;
    }
    if (!ensure_exfat_loaded())
        return false;

    find_ata_driver();
    if (!ata_port) { debug_print("[DobFileSystem] exFAT format: no ata_port\n"); return false; }

    debug_print("[DobFileSystem] Formatting disk as exFAT...\n");

    /* Disk size via ATA IDENTIFY (words 60-61 = LBA28 sector count). */
    dob_msg_t msg = {0}, reply = {0};
    msg.code = 3;  /* IDENTIFY */
    if (dob_ipc_call(ata_port, &msg, &reply) != DOB_OK || !reply.payload)
    {
        debug_print("[DobFileSystem] exFAT format: cannot identify disk\n");
        return false;
    }
    uint16_t ident[256];
    memcpy(ident, reply.payload, SECTOR_SIZE);
    uint32_t total_sectors = (uint32_t)ident[60] | ((uint32_t)ident[61] << 16);
    if (total_sectors == 0 || total_sectors <= fs.partition_lba)
    {
        debug_print("[DobFileSystem] exFAT format: bad disk size\n");
        return false;
    }

    uint64_t vol_sectors = (uint64_t)(total_sectors - fs.partition_lba);
    int rc = ex.api->mkfs(exfat_rd, exfat_wr, NULL, vol_sectors, SECTOR_SIZE, label);
    if (rc != 0)
    {
        debug_print("[DobFileSystem] exFAT mkfs failed\n");
        return false;
    }
    debug_print("[DobFileSystem] exFAT format complete.\n");
    return true;
}

/* Ensure MainDOB directory layout exists on disk */

/* Allocate a cluster for a new directory and write . and .. entries.
 * Returns the cluster number, or 0 on failure. */
static uint32_t create_dir_cluster(uint32_t parent_cluster)
{
    uint32_t cl = fat_alloc_cluster(0, true);  /* zero=true: directory */
    if (cl == 0) return 0;

    /* Write . and .. entries into the zeroed cluster */
    uint8_t *cbuf = cluster_work_buf;
    if (!read_cluster(cl, cbuf))
        memset(cbuf, 0, fs.bytes_per_cluster);
    fat32_dirent_t *ents = (fat32_dirent_t *)cbuf;

    memset(ents[0].name, ' ', 8);
    memset(ents[0].ext, ' ', 3);
    ents[0].name[0] = '.';
    ents[0].attr = ATTR_DIRECTORY;
    ents[0].first_cluster_hi = (uint16_t)(cl >> 16);
    ents[0].first_cluster_lo = (uint16_t)(cl & 0xFFFF);

    memset(ents[1].name, ' ', 8);
    memset(ents[1].ext, ' ', 3);
    ents[1].name[0] = '.';
    ents[1].name[1] = '.';
    ents[1].attr = ATTR_DIRECTORY;
    ents[1].first_cluster_hi = (uint16_t)(parent_cluster >> 16);
    ents[1].first_cluster_lo = (uint16_t)(parent_cluster & 0xFFFF);

    write_cluster(cl, cbuf);
    return cl;
}

static void ensure_dir(uint32_t parent, const char *name)
{
    fat32_dirent_t de;
    if (dir_find_entry(parent, name, &de, NULL, NULL))
        return;  /* Already exists */

    uint32_t cl = create_dir_cluster(parent);
    if (cl == 0) return;

    dir_add_entry(parent, name, cl, 0, ATTR_DIRECTORY);
    fat_flush();
}

static void ensure_layout(void)
{
    uint32_t root = fs.root_cluster;

    ensure_dir(root, "SYSTEM");
    ensure_dir(root, "DATA");

    /* Find SYSTEM and DATA clusters */
    fat32_dirent_t de;
    uint32_t sys_cl = 0, data_cl = 0;

    if (dir_find_entry(root, "SYSTEM", &de, NULL, NULL))
        sys_cl = DE_CLUSTER(de);
    if (dir_find_entry(root, "DATA", &de, NULL, NULL))
        data_cl = DE_CLUSTER(de);

    if (sys_cl)
    {
        ensure_dir(sys_cl, "PROGRAMS");
        ensure_dir(sys_cl, "DRIVERS");
        ensure_dir(sys_cl, "OperatingSystem");
        ensure_dir(sys_cl, "GAMES");
        ensure_dir(sys_cl, "CONFIG");
    }

    if (data_cl)
    {
        ensure_dir(data_cl, "Desktop");
        ensure_dir(data_cl, "Documents");
        ensure_dir(data_cl, "Downloads");
        ensure_dir(data_cl, "Music");
        ensure_dir(data_cl, "Video");
        ensure_dir(data_cl, "Pictures");
        ensure_dir(data_cl, "Screenshots");
        ensure_dir(data_cl, "Other files");
    }

    fat_flush();
}

/*  * Streaming I/O Pipeline
 * ===================================================================
 * BANANA model: data flows through a per-fd buffer. Writes accumulate
 * until a full cluster is ready, then flush to disk. Reads prefetch
 * one cluster at a time. Cursor cached for O(1) sequential access.
 * Metadata (dir entry) updated only at close.
 */

/* Record a waypoint at the current cursor position */
static void fd_record_waypoint(dobfs_fd_t *f)
{
    if (f->cur_cluster_idx % FD_WP_INTERVAL != 0) return;
    if (f->wp_count >= FD_MAX_WAYPOINTS) return;
    if (f->wp_count > 0 && f->wp_idx[f->wp_count - 1] == f->cur_cluster_idx)
        return;

    f->wp_cluster[f->wp_count] = f->cur_cluster;
    f->wp_idx[f->wp_count] = f->cur_cluster_idx;
    f->wp_count++;
}

/* Navigate cursor to a specific cluster index.
 * O(1) for sequential, O(64) worst case via waypoints. */
static bool fd_seek_cursor(dobfs_fd_t *f, uint32_t target_idx)
{
    if (f->first_cluster < 2) return false;

    /* Already there */
    if (f->cur_cluster >= 2 && f->cur_cluster_idx == target_idx)
        return true;

    /* One step forward — hot path for sequential I/O */
    if (f->cur_cluster >= 2 && f->cur_cluster_idx + 1 == target_idx)
    {
        uint32_t next = fat_read(f->cur_cluster);
        if (next < 2 || fat_is_eoc(next)) return false;
        f->cur_cluster = next;
        f->cur_cluster_idx = target_idx;
        fd_record_waypoint(f);
        return true;
    }

    /* Find best starting point: waypoints or current cursor */
    uint32_t start_cluster = f->first_cluster;
    uint32_t start_idx = 0;

    for (uint32_t i = f->wp_count; i > 0; i--)
    {
        if (f->wp_idx[i - 1] <= target_idx)
        {
            start_cluster = f->wp_cluster[i - 1];
            start_idx = f->wp_idx[i - 1];
            break;
        }
    }

    if (f->cur_cluster >= 2 && f->cur_cluster_idx <= target_idx
        && f->cur_cluster_idx >= start_idx)
    {
        start_cluster = f->cur_cluster;
        start_idx = f->cur_cluster_idx;
    }

    /* Walk from start to target */
    uint32_t c = start_cluster;
    for (uint32_t i = start_idx; i < target_idx; i++)
    {
        uint32_t next = fat_read(c);
        if (next < 2 || fat_is_eoc(next)) return false;
        c = next;
    }

    f->cur_cluster = c;
    f->cur_cluster_idx = target_idx;
    fd_record_waypoint(f);
    return true;
}

/* Batch-allocate clusters, linking after 'prev'.
 * Returns first new cluster, or 0 on failure.
 * out_last/out_allocated return chain info to avoid re-walking. */
static uint32_t fat_alloc_batch(uint32_t prev, uint32_t count, bool zero,
                                uint32_t *out_last, uint32_t *out_allocated)
{
    uint32_t first_new = 0, last = prev, allocated = 0;
    uint32_t limit = fs.total_clusters + 2;
    uint32_t start = (next_free_hint >= 2 && next_free_hint < limit) ? next_free_hint : 2;

    for (uint32_t pass = 0; pass < 2 && allocated < count; pass++)
    {
        uint32_t begin = (pass == 0) ? start : 2;
        uint32_t end   = (pass == 0) ? limit : start;
        for (uint32_t c = begin; c < end && allocated < count; c++)
        {
            if (fat_read(c) != FAT32_FREE) continue;

            fat_write(c, FAT32_EOC);
            if (last >= 2) fat_write(last, c);
            if (!first_new) first_new = c;
            last = c;
            allocated++;

            /* A freshly allocated cluster reuses a number that may still sit in
             * the read cache with its pre-free contents — drop it so the next
             * read refills from disk. (Previously done only for zeroed/dir
             * clusters; the file path, zero=false, left stale copies behind.) */
            cluster_cache_invalidate(c);
            if (zero)
            {
                memset(cluster_work_buf, 0, fs.bytes_per_cluster);
                disk_write_sectors(cluster_to_lba(c),
                                   fs.sectors_per_cluster, cluster_work_buf);
            }
        }
    }

    if (allocated > 0)
        next_free_hint = last + 1;

    if (out_last)      *out_last = last;
    if (out_allocated) *out_allocated = allocated;
    return first_new;
}

/* Ensure chain has enough clusters for end_offset bytes.
 * Uses cached chain_len/chain_last, batch-allocates from the end. */
static bool fd_ensure_clusters(dobfs_fd_t *f, uint32_t end_offset)
{
    if (end_offset == 0) return true;

    uint32_t needed = (end_offset + fs.bytes_per_cluster - 1) / fs.bytes_per_cluster;

    /* First cluster: allocate new chain */
    if (f->first_cluster < 2)
    {
        uint32_t batch = needed;
        if (batch < FAT_PREALLOC_COUNT) batch = FAT_PREALLOC_COUNT;
        uint32_t alloc_last = 0, alloc_count = 0;
        uint32_t nc = fat_alloc_batch(0, batch, false, &alloc_last, &alloc_count);
        if (!nc && batch > needed)
            nc = fat_alloc_batch(0, needed, false, &alloc_last, &alloc_count);
        if (!nc)
        {
            debug_print("[dobfs] ALLOC FAIL: no free cluster for new chain\n");
            return false;
        }

        f->first_cluster = nc;
        f->cur_cluster = nc;
        f->cur_cluster_idx = 0;
        f->meta_dirty = true;

        /* Use output params — no re-walk needed */
        f->chain_len = alloc_count;
        f->chain_last = alloc_last;
        return (alloc_count >= needed);
    }

    /* Lazy-compute chain_len if unknown */
    if (f->chain_len == 0)
    {
        uint32_t c = f->first_cluster, len = 1;
        while (!fat_is_eoc(fat_read(c))) { c = fat_read(c); len++; }
        f->chain_len = len;
        f->chain_last = c;
    }

    if (needed <= f->chain_len) return true;

    /* Extend from chain_last */
    uint32_t to_add = needed - f->chain_len;
    uint32_t batch = to_add;
    if (batch < FAT_PREALLOC_COUNT) batch = FAT_PREALLOC_COUNT;

    uint32_t alloc_last = 0, alloc_count = 0;
    uint32_t nc = fat_alloc_batch(f->chain_last, batch, false,
                                  &alloc_last, &alloc_count);
    if (!nc && batch > to_add)
        nc = fat_alloc_batch(f->chain_last, to_add, false,
                             &alloc_last, &alloc_count);
    if (!nc) return false;

    /* Use output params — no re-walk needed */
    f->chain_last = alloc_last;
    f->chain_len += alloc_count;

    return (f->chain_len >= needed);
}

/* Flush fd write buffer to disk (one cluster). */
static bool fd_flush_write(dobfs_fd_t *f)
{
    if (!f->buf_dirty || f->buf_pos == 0) return true;

    uint32_t cluster_idx = (uint32_t)(f->offset - f->buf_pos) / fs.bytes_per_cluster;
    if (!fd_seek_cursor(f, cluster_idx))
    {
        char m[96];
        sprintf(m, "[dobfs] FLUSH FAIL: seek idx=%u first_cl=%u\n",
                cluster_idx, f->first_cluster);
        debug_print(m);
        return false;
    }

    if (f->buf_pos < fs.bytes_per_cluster)
    {
        /* Partial cluster: read-modify-write. Merge once here so the store
         * below is a pure (idempotent) cluster write we can safely retry. */
        if (!read_cluster(f->cur_cluster, cluster_data))
            memset(cluster_data, 0, fs.bytes_per_cluster);
        memcpy(cluster_data, f->buf, f->buf_pos);
    }
    /* Bytes to commit: the merged image for a partial cluster, else the
     * buffer as-is for a full one. */
    const void *out = (f->buf_pos < fs.bytes_per_cluster) ? (const void *)cluster_data
                                                          : (const void *)f->buf;

    /* Retry the cluster store on transient device-side write errors. On
     * failure write_cluster touches no state and fd_flush_write has not yet
     * cleared buf_dirty/buf_pos, so each attempt rewrites exactly the same
     * bytes to the same LBA. A genuinely persistent error still fails out
     * after the budget. */
    bool ok = false;
    for (int t = 0; t < FD_FLUSH_RETRIES && !ok; t++)
    {
        if (t) sleep_ms(FD_FLUSH_RETRY_MS);   /* let a warming/busy device settle */
        ok = write_cluster(f->cur_cluster, out);
    }
    if (!ok)
    {
        char m[96];
        sprintf(m, "[dobfs] FLUSH FAIL: write_cluster cl=%u after %d tries\n",
                f->cur_cluster, FD_FLUSH_RETRIES);
        debug_print(m);
        return false;
    }

    f->buf_dirty = false;
    f->buf_pos = 0;
    /* The buffer just held write data; clear the read cache markers so
     * any following handle_read refills from disk. */
    f->buf_valid = 0;
    f->buf_start_cluster_idx = 0;
    return true;
}

/* Fill fd read buffer from disk.
 *
 * Batch-read contiguous clusters in a single I/O.  Walk the FAT starting
 * at cluster_idx, collecting consecutive clusters (fat_read(c) == c+1)
 * into one disk_read_sectors call until either
 *   - the buffer is full, or
 *   - we hit a non-contiguous cluster in the chain, or
 *   - we hit EOF / EOC.
 *
 * For a non-fragmented file this collapses dozens of IPC round-trips
 * into a single bulk transfer. */
static bool fd_fill_read(dobfs_fd_t *f)
{
    uint32_t cluster_idx = (uint32_t)f->offset / fs.bytes_per_cluster;

    if (!fd_seek_cursor(f, cluster_idx)) return false;

    /* How many clusters fit in our buffer? */
    uint32_t max_clusters_in_buf = FD_READ_BUF_SIZE / fs.bytes_per_cluster;
    if (max_clusters_in_buf == 0) max_clusters_in_buf = 1;

    /* How many clusters remain in the file from our current cluster? */
    uint32_t file_start_byte = cluster_idx * fs.bytes_per_cluster;
    uint32_t remaining_bytes = (f->file_size > file_start_byte)
                                ? f->file_size - file_start_byte : 0;
    uint32_t remaining_clusters =
        (remaining_bytes + fs.bytes_per_cluster - 1) / fs.bytes_per_cluster;

    uint32_t want = max_clusters_in_buf;
    if (want > remaining_clusters) want = remaining_clusters;
    if (want == 0) { f->buf_valid = 0; return false; }

    /* Walk the FAT forward from cur_cluster, batching consecutive clusters. */
    uint32_t run_start = f->cur_cluster;
    uint32_t run_count = 1;
    uint32_t prev = run_start;
    while (run_count < want)
    {
        uint32_t next = fat_read(prev);
        if (fat_is_eoc(next)) break;
        if (next != prev + 1) break;        /* discontiguous — stop batching */
        run_count++;
        prev = next;
    }

    /* One bulk read for the whole contiguous run. */
    uint32_t lba   = cluster_to_lba(run_start);
    uint32_t count = run_count * fs.sectors_per_cluster;
    if (!disk_read_sectors(lba, count, f->buf)) return false;

    /* Valid bytes in buffer: full clusters, clamped to EOF. */
    uint32_t span_start = cluster_idx * fs.bytes_per_cluster;
    uint32_t span_end   = span_start + run_count * fs.bytes_per_cluster;
    if (span_end > f->file_size) span_end = f->file_size;

    f->buf_valid            = span_end - span_start;
    f->buf_pos              = f->offset - span_start;
    f->buf_start_cluster_idx = cluster_idx;
    return true;
}

/* Free fd buffer and reset streaming state */
static void fd_free_buf(dobfs_fd_t *f)
{
    if (f->buf) { free(f->buf); f->buf = NULL; }
    f->buf_pos = 0;
    f->buf_valid = 0;
    f->buf_dirty = false;
}

/* DobFileSystem Handlers */

/* Reclaim fds owned by dead processes.
 * Flushes dirty data and metadata before freeing to prevent data loss.
 * Called periodically and when alloc_fd can't find a free slot. */
static void fd_reclaim_orphans(void)
{
    bool flushed_any = false;
    for (int i = 3; i < DOBFS_MAX_FDS; i++)
    {
        if (!fd_table[i].used) continue;
        int status = proc_status(fd_table[i].owner);
        if (status == 0 || status == 2)
        {
            dobfs_fd_t *f = &fd_table[i];

            /* Flush pending writes before freeing — prevents data loss.
             * These failures used to be SILENT: the field symptom is a
             * file that exists but stays empty, with nothing on the
             * console. Name the broken link. */
            if (f->buf_dirty && !fd_flush_write(f))
                debug_print("[dobfs] CLOSE: data flush FAILED\n");
            if (f->meta_dirty && f->dir_cluster != 0)
            {
                if (!dir_update_entry(f->dir_cluster, f->dir_entry_index,
                                      f->file_size, f->first_cluster))
                    debug_print("[dobfs] CLOSE: dirent update FAILED\n");
            }
            else if (f->meta_dirty)
                debug_print("[dobfs] CLOSE: meta dirty but dir_cluster=0!\n");

            fd_free_buf(f);
            f->used = false;
            flushed_any = true;
        }
    }
    if (flushed_any)
        fat_flush();
}

/* Counter for periodic orphan cleanup */
static uint32_t msg_counter = 0;
#define ORPHAN_CHECK_INTERVAL 64

static int alloc_fd(pid_t owner)
{
    for (int i = 3; i < DOBFS_MAX_FDS; i++)
    {
        if (!fd_table[i].used)
        {
            memset(&fd_table[i], 0, sizeof(dobfs_fd_t));
            fd_table[i].used = true;
            fd_table[i].owner = owner;
            return i;
        }
    }

    /* Table full — try reclaiming fds from dead processes */
    fd_reclaim_orphans();

    for (int i = 3; i < DOBFS_MAX_FDS; i++)
    {
        if (!fd_table[i].used)
        {
            memset(&fd_table[i], 0, sizeof(dobfs_fd_t));
            fd_table[i].used = true;
            fd_table[i].owner = owner;
            return i;
        }
    }

    return -1;
}

static dob_status_t handle_open(dob_msg_t *msg, dob_msg_t *reply)
{
    if (!msg->payload || !fs.mounted) return DOB_ERR_INVALID;
    strncpy(ipc_path1, (const char *)msg->payload, DOBFS_PATH_MAX - 1);
    ipc_path1[DOBFS_PATH_MAX - 1] = '\0';
    const char *path = ipc_path1;
    uint32_t flags = msg->arg0;

    if (!validate_path(path) || !path_is_safe(path))
        return DOB_ERR_DENIED;

    /* Sandbox enforcement */
    bool is_write = (flags & (O_WRITE | O_CREATE | O_TRUNC | O_APPEND)) != 0;
    if (!sandbox_check(msg->sender_pid, path, is_write))
        return DOB_ERR_DENIED;

    if (ex.active)
    {
        exfat_file_t xf;
        bool created_now = false;
        int rc = ex.api->open(ex.vol, path, &xf);
        if (rc != 0 && (flags & O_CREATE))
        {
            int cr = ex.api->create(ex.vol, path);
            if (cr == -4) return DOB_ERR_NO_SPACE;
            if (cr < 0)   return DOB_ERR_INVALID;
            rc = ex.api->open(ex.vol, path, &xf);
            created_now = true;
        }
        if (rc == -2) return DOB_ERR_HW_FAULT;
        if (rc != 0)  return DOB_ERR_NOT_FOUND;

        /* A just-created file's dirent is only in the device write cache;
         * mark the handle dirty so close() commits it (and flushes the
         * device cache) even if nothing is written -- e.g. a bare touch. */
        if (created_now) xf.dirty = true;

        /* O_TRUNC empties an existing file's contents. */
        if ((flags & O_TRUNC) && !xf.is_dir && xf.size > 0)
        {
            if (ex.api->ftrunc(ex.vol, &xf, 0) < 0) return DOB_ERR_HW_FAULT;
        }

        int xfd = alloc_fd(msg->sender_pid);
        if (xfd < 0) return DOB_ERR_NO_MEMORY;

        dobfs_fd_t *xf_fd = &fd_table[xfd];
        strncpy(xf_fd->path, path, DOBFS_PATH_MAX - 1);
        xf_fd->path[DOBFS_PATH_MAX - 1] = '\0';
        xf_fd->flags      = flags;
        xf_fd->is_exfat   = true;
        xf_fd->exfat_file = xf;
        xf_fd->file_size  = xf.size;
        xf_fd->is_dir     = xf.is_dir;
        /* O_APPEND starts the cursor at EOF; exfat_file.pos drives I/O. */
        xf_fd->offset         = (flags & O_APPEND) ? xf.size : 0;
        xf_fd->exfat_file.pos = xf_fd->offset;
        reply->arg0 = (uint32_t)xfd;
        return DOB_OK;
    }

    fat32_dirent_t de;
    uint32_t dc, di;
    bool exists = resolve_full_path(path, &de, &dc, &di);

    if (!exists && !(flags & O_CREATE))
        return DOB_ERR_NOT_FOUND;

    if (!exists && (flags & O_CREATE))
    {
        uint32_t parent_cluster;
        const char *basename;
        if (!resolve_path(path, &parent_cluster, &basename))
            return DOB_ERR_NOT_FOUND;

        if (!dir_add_entry(parent_cluster, basename, 0, 0, ATTR_ARCHIVE))
            return DOB_ERR_INTERNAL;

        fat_flush();

        if (!resolve_full_path(path, &de, &dc, &di))
            return DOB_ERR_INTERNAL;
    }

    int fd = alloc_fd(msg->sender_pid);
    if (fd < 0) return DOB_ERR_NO_MEMORY;

    dobfs_fd_t *f = &fd_table[fd];
    strncpy(f->path, path, DOBFS_PATH_MAX - 1);
    f->path[DOBFS_PATH_MAX - 1] = '\0';
    f->flags = flags;
    f->first_cluster = DE_CLUSTER(de);
    f->dir_cluster = dc;
    f->dir_entry_index = di;
    f->file_size = de.file_size;
    f->is_dir = (de.attr & ATTR_DIRECTORY) != 0;
    f->offset = (flags & O_APPEND) ? de.file_size : 0;

    /* Initialize streaming I/O state.
     * Buffer sized for multi-cluster batched reads — at least
     * FD_READ_BUF_SIZE, but never smaller than one cluster (write path
     * still operates cluster-at-a-time). */
    uint32_t buf_size = FD_READ_BUF_SIZE;
    if (buf_size < fs.bytes_per_cluster) buf_size = fs.bytes_per_cluster;
    f->buf = (uint8_t *)malloc(buf_size);
    f->buf_pos = 0;
    f->buf_valid = 0;
    f->buf_dirty = false;
    f->buf_start_cluster_idx = 0;
    f->cur_cluster = f->first_cluster;
    f->cur_cluster_idx = 0;
    f->chain_len = 0;
    f->chain_last = 0;
    f->wp_count = 0;
    f->meta_dirty = false;
    f->trunc_old_chain = 0;

    if ((flags & O_TRUNC) && !f->is_dir && f->first_cluster >= 2)
    {
        /* Non-destructive truncate. Do NOT free the chain or touch the dirent
         * yet: the dirent keeps pointing at the original content. Stash the old
         * chain and reset first_cluster to 0 so writes allocate a FRESH chain
         * (the same path used to create a new file, which is known to work).
         * At close, once the new content is safely on disk, we repoint the
         * dirent to the new chain and free the old one. If the save fails, the
         * dirent still references the old chain -> the content is never lost. */
        f->trunc_old_chain = f->first_cluster;
        f->first_cluster = 0;
        f->file_size = 0;
        f->offset = 0;
        f->cur_cluster = 0;
        f->cur_cluster_idx = 0;
        f->chain_len = 0;
        f->chain_last = 0;
        f->meta_dirty = true;   /* force the dirent repoint at close (also for an empty save) */
    }

    reply->arg0 = (uint32_t)fd;
    return DOB_OK;
}

static dob_status_t handle_read(dob_msg_t *msg, dob_msg_t *reply)
{
    uint32_t fd = msg->arg0;
    uint32_t size = msg->arg1;

    if (fd >= DOBFS_MAX_FDS || !fd_table[fd].used) return DOB_ERR_INVALID;
    if (fd_table[fd].owner != msg->sender_pid) return DOB_ERR_DENIED;
    if (!(fd_table[fd].flags & O_READ)) return DOB_ERR_DENIED;
    if (!fs.mounted) return DOB_ERR_INTERNAL;

    dobfs_fd_t *f = &fd_table[fd];

    if (f->is_exfat)
    {
        uint32_t n = size;
        if (n > sizeof(sector_buf)) n = sizeof(sector_buf);
        int got = ex.api->read(ex.vol, &f->exfat_file, sector_buf, n);
        if (got < 0) return DOB_ERR_HW_FAULT;
        f->offset = f->exfat_file.pos;   /* mirror the .mem cursor for SEEK/stat */
        reply->payload      = sector_buf;
        reply->payload_size = (uint32_t)got;
        reply->arg0         = (uint32_t)got;
        return DOB_OK;
    }

    if (f->offset >= f->file_size)
    {
        reply->arg0 = 0;
        return DOB_OK;
    }
    if (f->offset + size > f->file_size)
        size = f->file_size - f->offset;
    if (size > sizeof(sector_buf))
        size = sizeof(sector_buf);

    uint32_t bytes_read = 0;

    while (bytes_read < size)
    {
        /* The buffer may hold multiple consecutive clusters.  Check
         * whether the current offset still falls inside the cached span
         * [buf_start_cluster_idx * bpc, buf_start_cluster_idx * bpc + buf_valid). */
        uint32_t span_start = f->buf_start_cluster_idx * fs.bytes_per_cluster;
        uint32_t span_end   = span_start + f->buf_valid;

        bool in_span = (f->buf_valid > 0) &&
                       (f->offset >= span_start) &&
                       (f->offset < span_end);

        if (!in_span)
        {
            if (!f->buf || !fd_fill_read(f))
                break;
            span_start = f->buf_start_cluster_idx * fs.bytes_per_cluster;
        }

        uint32_t off_in_buf = f->offset - span_start;
        uint32_t avail = f->buf_valid - off_in_buf;
        uint32_t to_copy = size - bytes_read;
        if (to_copy > avail) to_copy = avail;
        if (to_copy == 0) break;

        memcpy(sector_buf + bytes_read, f->buf + off_in_buf, to_copy);
        bytes_read += to_copy;
        f->offset += to_copy;
    }

    reply->payload = sector_buf;
    reply->payload_size = bytes_read;
    reply->arg0 = bytes_read;
    return DOB_OK;
}

static dob_status_t handle_write(dob_msg_t *msg, dob_msg_t *reply)
{
    uint32_t fd = msg->arg0;

    if (fd >= DOBFS_MAX_FDS || !fd_table[fd].used) return DOB_ERR_INVALID;
    if (fd_table[fd].owner != msg->sender_pid) return DOB_ERR_DENIED;
    if (!(fd_table[fd].flags & O_WRITE)) return DOB_ERR_DENIED;
    if (!fs.mounted) return DOB_ERR_INTERNAL;
    if (!msg->payload || msg->payload_size == 0) { reply->arg0 = 0; return DOB_OK; }

    dobfs_fd_t *f = &fd_table[fd];

    if (f->is_exfat)
    {
        uint32_t wn = msg->payload_size;
        if (wn > sizeof(sector_buf)) wn = sizeof(sector_buf);
        memcpy(sector_buf, msg->payload, wn);          /* copy out of the IPC buffer */
        int wr = ex.api->write(ex.vol, &f->exfat_file, sector_buf, wn);
        if (wr < 0)
        {
#ifdef MAINDOB_DEBUG
            char d[160];
            snprintf(d, sizeof(d),
                     "[dobfs] exFAT write FAILED rc=%d wn=%u pos=%llu %s\n",
                     wr, wn, (unsigned long long)f->exfat_file.pos, f->path);
            debug_print(d);
#endif
            return (wr == -4) ? DOB_ERR_NO_SPACE : DOB_ERR_HW_FAULT;
        }
        f->offset    = f->exfat_file.pos;
        f->file_size = f->exfat_file.size;
        reply->arg0  = (uint32_t)wr;
        return DOB_OK;
    }

    if (!f->buf) { reply->arg0 = 0; return DOB_ERR_INTERNAL; }

    /* The read buffer may hold up to FD_READ_BUF_SIZE of cached cluster
     * data from a previous read on this fd.  Before a write, invalidate
     * that cache and reset buf_pos: the write path uses buf as a
     * single-cluster staging area starting from offset 0. */
    if (!f->buf_dirty)
    {
        f->buf_valid = 0;
        f->buf_pos = 0;
        f->buf_start_cluster_idx = 0;
    }

    uint32_t write_size = msg->payload_size;

    /* Copy payload out of IPC buffer before any sub-IPC calls */
    if (write_size > sizeof(sector_buf))
        write_size = sizeof(sector_buf);
    memcpy(sector_buf, msg->payload, write_size);
    const uint8_t *src = sector_buf;

    /* Pre-allocate clusters for the entire write */
    if (!fd_ensure_clusters(f, (uint32_t)(f->offset + write_size)))
        return DOB_ERR_NO_MEMORY;

    /* Position cursor at current cluster if not set */
    if (f->cur_cluster < 2 && f->first_cluster >= 2)
    {
        uint32_t target_idx = (uint32_t)f->offset / fs.bytes_per_cluster;
        fd_seek_cursor(f, target_idx);
    }

    /* Stream data through buffer — flush full clusters to disk */
    uint32_t bytes_written = 0;
    uint32_t bpc = fs.bytes_per_cluster;

    while (bytes_written < write_size)
    {
        uint32_t space = bpc - f->buf_pos;
        uint32_t chunk = write_size - bytes_written;
        if (chunk > space) chunk = space;

        memcpy(f->buf + f->buf_pos, src + bytes_written, chunk);
        f->buf_pos += chunk;
        f->buf_dirty = true;
        bytes_written += chunk;
        f->offset += chunk;

        /* Buffer full — flush one cluster to disk */
        if (f->buf_pos >= bpc)
        {
            if (!fd_flush_write(f))
                return DOB_ERR_INTERNAL;   /* disk write error: report it,
                                            * don't mask it as a short write
                                            * (callers read that as success
                                            * and silently truncate). */
            /* Advance cursor to next cluster */
            fd_seek_cursor(f, f->cur_cluster_idx + 1);
        }
    }

    /* Update file size (metadata deferred to close) */
    if (f->offset > f->file_size)
    {
        f->file_size = f->offset;
        f->meta_dirty = true;
    }

    reply->arg0 = bytes_written;
    return DOB_OK;
}

static dob_status_t handle_close(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    uint32_t fd = msg->arg0;
    if (fd >= DOBFS_MAX_FDS || !fd_table[fd].used) return DOB_ERR_INVALID;
    if (fd_table[fd].owner != msg->sender_pid) return DOB_ERR_DENIED;

    dobfs_fd_t *f = &fd_table[fd];

    if (f->is_exfat)
    {
        bool was_dirty = f->exfat_file.dirty;
        if (ex.api && ex.vol)
            ex.api->flush(ex.vol, &f->exfat_file);   /* persist pending exFAT writes */
        if (was_dirty)
            disk_flush();   /* commit the device write cache to NAND (USB
                             * SYNCHRONIZE CACHE; no-op on the boot disk) so a
                             * pulled stick keeps what we just wrote. */
        fd_free_buf(f);
        f->used = false;
        return DOB_OK;
    }

    bool did_write = false;

    /* Flush remaining write buffer to disk */
    bool flush_ok = true;
    if (f->buf_dirty)
    {
        flush_ok = fd_flush_write(f);
        did_write = true;
    }

    /* Trim excess preallocated clusters.
     * file_size determines how many clusters we actually need. */
    if (flush_ok && did_write && f->first_cluster >= 2 && !f->is_dir)
    {
        uint32_t needed = ((uint32_t)f->file_size + fs.bytes_per_cluster - 1)
                          / fs.bytes_per_cluster;
        if (needed == 0) needed = 1;  /* Keep at least 1 cluster for 0-byte files */

        if (f->chain_len > needed)
        {
            /* Walk to the last cluster we need, then free the rest */
            uint32_t c = f->first_cluster;
            for (uint32_t i = 1; i < needed; i++)
            {
                uint32_t next = fat_read(c);
                if (next < 2 || fat_is_eoc(next)) break;
                c = next;
            }
            uint32_t tail = fat_read(c);
            if (tail >= 2 && !fat_is_eoc(tail))
            {
                fat_write(c, FAT32_EOC);
                fat_free_chain(tail);
            }
            f->chain_len = needed;
            f->chain_last = c;
        }
    }

    /* Commit the non-destructive truncate, or do the normal metadata update.
     *
     * For a truncated file the dirent still points at the ORIGINAL chain. Only
     * now that the new content is on disk do we repoint it and free the old
     * chain — and only if the flush succeeded. If anything failed we leave the
     * dirent on the old chain (original content intact) and orphan the new,
     * partial chain. An empty save (no data written) has flush_ok == true and
     * first_cluster == 0, so the dirent is correctly repointed to {0,0}. */
    if (f->trunc_old_chain >= 2)
    {
        if (flush_ok &&
            dir_update_entry(f->dir_cluster, f->dir_entry_index,
                             f->file_size, f->first_cluster))
        {
            fat_free_chain(f->trunc_old_chain);  /* repoint committed: old chain obsolete */
            did_write = true;
        }
        /* else: save failed -> keep old chain + old dirent, orphan new chain */
        f->trunc_old_chain = 0;
        f->meta_dirty = false;
    }
    else if (f->meta_dirty)
    {
        dir_update_entry(f->dir_cluster, f->dir_entry_index,
                         f->file_size, f->first_cluster);
        did_write = true;
    }

    /* Only flush FAT if we actually modified it */
    if (did_write)
        fat_flush();

    fd_free_buf(f);
    f->used = false;
    return DOB_OK;
}

/* DOBFS_SEEK — reposition fd->offset.
 *
 * Computes the new absolute position from (offset, whence), clamps
 * read-only fds to [0, file_size], then invalidates the read buffer
 * if it no longer covers the new offset. The cluster cursor is
 * advanced/rewound lazily by the next handle_read/handle_write via
 * fd_seek_cursor() — no preemptive traversal here.
 *
 * Write-dirty buffers must be flushed BEFORE the seek, otherwise the
 * staged bytes would end up at the new (wrong) location when finally
 * written. */
static dob_status_t handle_seek(dob_msg_t *msg, dob_msg_t *reply)
{
    uint32_t fd      = msg->arg0;
    uint32_t off_lo  = msg->arg1;       /* offset low 32 bits  */
    uint32_t whence  = msg->arg2;
    uint32_t off_hi  = msg->arg3;       /* offset high 32 bits */

    if (fd >= DOBFS_MAX_FDS || !fd_table[fd].used) return DOB_ERR_INVALID;
    if (fd_table[fd].owner != msg->sender_pid)     return DOB_ERR_DENIED;

    dobfs_fd_t *f = &fd_table[fd];

    if (f->is_exfat)
    {
        int64_t soff = (int64_t)(((uint64_t)off_hi << 32) | (uint64_t)off_lo);
        int64_t np   = 0;
        switch (whence)
        {
            case DOBFS_SEEK_SET: np = soff;                                break;
            case DOBFS_SEEK_CUR: np = (int64_t)f->exfat_file.pos  + soff;  break;
            case DOBFS_SEEK_END: np = (int64_t)f->exfat_file.size + soff;  break;
            default:             return DOB_ERR_INVALID;
        }
        if (np < 0) return DOB_ERR_INVALID;
        if (!(f->flags & O_WRITE) && np > (int64_t)f->exfat_file.size)
            np = (int64_t)f->exfat_file.size;   /* read-only fds clamp to EOF */
        f->exfat_file.pos = (uint64_t)np;
        f->offset         = (uint64_t)np;
        reply->arg0 = (uint32_t)np;
        reply->arg1 = (uint32_t)((uint64_t)np >> 32);
        return DOB_OK;
    }

    /* Flush any pending write staging before the cursor moves. */
    if (f->buf_dirty)
    {
        if (!fd_flush_write(f)) return DOB_ERR_INTERNAL;
        f->buf_dirty = false;
    }

    /* Reassemble the 64-bit offset. For SEEK_CUR/END it is a signed
     * delta (the high word carries the sign for negative deltas);
     * for SEEK_SET it is an absolute, non-negative position. */
    int64_t soff = (int64_t)(((uint64_t)off_hi << 32) | (uint64_t)off_lo);
    int64_t np   = 0;

    switch (whence)
    {
        case DOBFS_SEEK_SET:  np = soff;                            break;
        case DOBFS_SEEK_CUR:  np = (int64_t)f->offset    + soff;    break;
        case DOBFS_SEEK_END:  np = (int64_t)f->file_size + soff;    break;
        default:              return DOB_ERR_INVALID;
    }

    if (np < 0) return DOB_ERR_INVALID;

    /* Clamp read-only fds to EOF. Write-capable fds keep the raw
     * value — the write path extends the file if/when bytes land
     * past the current size. */
    if (!(f->flags & O_WRITE) && np > (int64_t)f->file_size)
        np = (int64_t)f->file_size;

    /* This backend is FAT32: a file (and any meaningful position) cannot
     * exceed 4 GB-1, so reject beyond-range seeks. The 64-bit wire exists
     * for exFAT, whose fds Phase 3 routes to their own seek path rather
     * than through this FAT32 handler. */
    if (np > (int64_t)0xFFFFFFFFu) return DOB_ERR_INVALID;

    f->offset = (uint64_t)np;

    /* Invalidate the read buffer if the new offset is outside its
     * cached span. The next read refills lazily. */
    uint32_t span_start = f->buf_start_cluster_idx * fs.bytes_per_cluster;
    uint32_t span_end   = span_start + f->buf_valid;
    if (f->buf_valid == 0 ||
        (uint32_t)f->offset < span_start || (uint32_t)f->offset >= span_end)
    {
        f->buf_valid = 0;
        f->buf_pos   = 0;
    }

    reply->arg0 = (uint32_t)f->offset;            /* new position, low 32  */
    reply->arg1 = (uint32_t)(f->offset >> 32);    /* new position, high 32 */
    return DOB_OK;
}

static dob_status_t handle_stat(dob_msg_t *msg, dob_msg_t *reply)
{
    if (!msg->payload) return DOB_ERR_INVALID;
    strncpy(ipc_path1, (const char *)msg->payload, DOBFS_PATH_MAX - 1);
    ipc_path1[DOBFS_PATH_MAX - 1] = '\0';
    const char *path = ipc_path1;
    if (!validate_path(path) || !path_is_safe(path)) return DOB_ERR_DENIED;
    /* No sandbox_check: browsing/stat is allowed everywhere.
     * The sandbox restricts open/write/mkdir/unlink, not navigation. */
    if (!fs.mounted) return DOB_ERR_INTERNAL;

    if (ex.active)
    {
        exfat_stat_t xst;
        int rc = ex.api->stat(ex.vol, path, &xst);
        if (rc == -2) return DOB_ERR_HW_FAULT;
        if (rc != 0)  return DOB_ERR_NOT_FOUND;
        reply->arg0 = (uint32_t)xst.size;            /* size low 32  */
        reply->arg1 = xst.is_dir ? 1 : 0;            /* type         */
        reply->arg2 = (uint32_t)(xst.size >> 32);    /* size high 32 */
        return DOB_OK;
    }

    fat32_dirent_t de;
    if (!resolve_full_path(path, &de, NULL, NULL))
        return DOB_ERR_NOT_FOUND;

    reply->arg0 = de.file_size;                        /* size, low 32 */
    reply->arg1 = (de.attr & ATTR_DIRECTORY) ? 1 : 0;  /* type         */
    reply->arg2 = 0;   /* size, high 32 — FAT32 files are <=4 GB; Phase 3
                        * exFAT routing fills this from the 64-bit size */
    return DOB_OK;
}

/* --- READDIR callback --- */

typedef struct
{
    char    *buf;
    int      pos;
    int      max;
} readdir_ctx_t;

static bool readdir_cb(fat32_dirent_t *de, const char *long_name,
                        uint32_t dir_cluster, uint32_t entry_index, void *ctx)
{
    (void)dir_cluster; (void)entry_index;
    readdir_ctx_t *rc = (readdir_ctx_t *)ctx;

    /* Skip . and .. */
    if (long_name[0] == '.' && (long_name[1] == '\0' || (long_name[1] == '.' && long_name[2] == '\0')))
        return true;

    int nlen = (int)strlen(long_name);
    char type = (de->attr & ATTR_DIRECTORY) ? 'D' : 'F';

    char sizebuf[12];
    int si = snprintf(sizebuf, sizeof(sizebuf), "%u", de->file_size);

    /* name\tD\tsize\n */
    int need = nlen + 1 + 1 + 1 + si + 1;
    if (rc->pos + need < rc->max)
    {
        memcpy(rc->buf + rc->pos, long_name, nlen);
        rc->pos += nlen;
        rc->buf[rc->pos++] = '\t';
        rc->buf[rc->pos++] = type;
        rc->buf[rc->pos++] = '\t';
        memcpy(rc->buf + rc->pos, sizebuf, si);
        rc->pos += si;
        rc->buf[rc->pos++] = '\n';
    }
    return true;
}

static dob_status_t handle_readdir(dob_msg_t *msg, dob_msg_t *reply)
{
    if (!msg->payload) return DOB_ERR_INVALID;
    strncpy(ipc_path1, (const char *)msg->payload, DOBFS_PATH_MAX - 1);
    ipc_path1[DOBFS_PATH_MAX - 1] = '\0';
    const char *path = ipc_path1;

    if (!validate_path(path) || !path_is_safe(path)) return DOB_ERR_DENIED;
    /* No sandbox_check: directory listing is allowed everywhere.
     * The sandbox restricts open/write/mkdir/unlink, not navigation. */
    if (!fs.mounted) return DOB_ERR_INTERNAL;

    if (ex.active)
    {
        static exfat_dirent_t xents[128];
        int xn = ex.api->readdir(ex.vol, path, xents,
                                 (int)(sizeof(xents) / sizeof(xents[0])));
        if (xn == -2) return DOB_ERR_HW_FAULT;
        if (xn < 0)   return DOB_ERR_NOT_FOUND;

        static char xbuf[4096];
        uint32_t xoff = 0;
        for (int i = 0; i < xn; i++)
        {
            int wrote = snprintf(xbuf + xoff, sizeof(xbuf) - xoff,
                                 "%s\t%c\t%u\n",
                                 xents[i].name,
                                 xents[i].is_dir ? 'D' : 'F',
                                 (unsigned)xents[i].size);
            if (wrote <= 0 || (uint32_t)wrote >= sizeof(xbuf) - xoff) break;
            xoff += (uint32_t)wrote;
        }
        xbuf[xoff] = '\0';
        reply->payload      = xbuf;
        reply->payload_size = xoff;
        return DOB_OK;
    }

    fat32_dirent_t de;
    if (!resolve_full_path(path, &de, NULL, NULL))
        return DOB_ERR_NOT_FOUND;
    if (!(de.attr & ATTR_DIRECTORY))
        return DOB_ERR_INVALID;

    uint32_t dir_cluster = DE_CLUSTER(de);

    static char dirbuf[4096];
    readdir_ctx_t ctx;
    ctx.buf = dirbuf;
    ctx.pos = 0;
    ctx.max = sizeof(dirbuf) - 1;

    dir_iterate(dir_cluster, readdir_cb, &ctx);

    dirbuf[ctx.pos] = '\0';

    reply->payload = dirbuf;
    reply->payload_size = (uint32_t)ctx.pos;
    return DOB_OK;
}

static dob_status_t handle_mkdir(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    if (!msg->payload) return DOB_ERR_INVALID;
    strncpy(ipc_path1, (const char *)msg->payload, DOBFS_PATH_MAX - 1);
    ipc_path1[DOBFS_PATH_MAX - 1] = '\0';
    const char *path = ipc_path1;
    if (!validate_path(path) || !path_is_safe(path)) return DOB_ERR_DENIED;
    if (!sandbox_check(msg->sender_pid, path, true)) return DOB_ERR_DENIED;
    if (!fs.mounted) return DOB_ERR_INTERNAL;
    if (ex.active)
    {
        int rc = ex.api->mkdir(ex.vol, path);
        if (rc == -4) return DOB_ERR_NO_SPACE;
        if (rc == -1) return DOB_ERR_INVALID;
        if (rc < 0)   return DOB_ERR_HW_FAULT;
        disk_flush();   /* commit the new directory to NAND (USB) */
        return DOB_OK;
    }

    /* Check if already exists */
    fat32_dirent_t de;
    if (resolve_full_path(path, &de, NULL, NULL))
        return DOB_ERR_INVALID;  /* Already exists */

    uint32_t parent_cluster;
    const char *basename;
    if (!resolve_path(path, &parent_cluster, &basename))
        return DOB_ERR_NOT_FOUND;

    uint32_t cl = create_dir_cluster(parent_cluster);
    if (cl == 0) return DOB_ERR_NO_MEMORY;

    if (!dir_add_entry(parent_cluster, basename, cl, 0, ATTR_DIRECTORY))
    {
        fat_free_chain(cl);
        return DOB_ERR_INTERNAL;
    }

    fat_flush();
    return DOB_OK;
}

/* Callback for directory emptiness check: returns false (stop) if a real
 * entry is found (not . or ..). Sets *(bool*)ctx = true if non-empty. */
static bool unlink_dir_empty_cb(fat32_dirent_t *de, const char *long_name,
                                 uint32_t dir_cluster, uint32_t entry_index, void *ctx)
{
    (void)de; (void)dir_cluster; (void)entry_index;
    /* Skip . and .. */
    if (long_name[0] == '.' &&
        (long_name[1] == '\0' || (long_name[1] == '.' && long_name[2] == '\0')))
        return true;  /* Continue */

    *(bool *)ctx = true;
    return false;  /* Stop: found a real entry, directory is not empty */
}

static dob_status_t handle_unlink(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    if (!msg->payload) return DOB_ERR_INVALID;
    strncpy(ipc_path1, (const char *)msg->payload, DOBFS_PATH_MAX - 1);
    ipc_path1[DOBFS_PATH_MAX - 1] = '\0';
    const char *path = ipc_path1;
    if (!validate_path(path) || !path_is_safe(path)) return DOB_ERR_DENIED;
    if (!sandbox_check(msg->sender_pid, path, true)) return DOB_ERR_DENIED;
    if (!fs.mounted) return DOB_ERR_INTERNAL;
    if (ex.active)
    {
        int rc = ex.api->unlink(ex.vol, path);
        if (rc == 0)  { disk_flush(); return DOB_OK; }
        if (rc == -5) return DOB_ERR_DENIED;       /* directory not empty */
        if (rc == -2) return DOB_ERR_HW_FAULT;
        if (rc == -1) return DOB_ERR_NOT_FOUND;
        return DOB_ERR_INVALID;
    }

    fat32_dirent_t de;
    uint32_t dc, di;
    if (!resolve_full_path(path, &de, &dc, &di))
        return DOB_ERR_NOT_FOUND;

    /* Don't delete non-empty directories.
     * Must check ALL clusters of the directory, not just the first.
     * Use dir_iterate which follows the full cluster chain. */
    if (de.attr & ATTR_DIRECTORY)
    {
        uint32_t cl = DE_CLUSTER(de);

        /* Callback returns false (stop iteration) on first real entry found */
        bool has_entries = false;
        dir_iterate(cl, unlink_dir_empty_cb, &has_entries);
        if (has_entries)
            return DOB_ERR_DENIED;  /* Directory not empty */
    }

    /* Free cluster chain */
    uint32_t first = DE_CLUSTER(de);
    if (first >= 2)
        fat_free_chain(first);

    /* Mark directory entry as deleted.
     * dc is the cluster where the entry was found by resolve_full_path,
     * di is the index within that cluster. */
    dir_remove_entry(dc, dc, di);

    fat_flush();
    return DOB_OK;
}

/* code=12 RENAME: payload = "old_path\0new_path" */
static dob_status_t handle_rename(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    if (!msg->payload || msg->payload_size < 4) return DOB_ERR_INVALID;
    if (!fs.mounted) return DOB_ERR_INTERNAL;

    /* Copy both paths out of IPC buffer before any disk I/O */
    const char *raw = (const char *)msg->payload;
    uint32_t old_len = strlen(raw);
    if (old_len + 2 > msg->payload_size) return DOB_ERR_INVALID;

    strncpy(ipc_path1, raw, DOBFS_PATH_MAX - 1);
    ipc_path1[DOBFS_PATH_MAX - 1] = '\0';
    strncpy(ipc_path2, raw + old_len + 1, DOBFS_PATH_MAX - 1);
    ipc_path2[DOBFS_PATH_MAX - 1] = '\0';

    const char *old_path = ipc_path1;
    const char *new_path = ipc_path2;

    if (!validate_path(old_path) || !path_is_safe(old_path)) return DOB_ERR_DENIED;
    if (!validate_path(new_path) || !path_is_safe(new_path)) return DOB_ERR_DENIED;

    /* Sandbox: need write access to both paths */
    if (!sandbox_check(msg->sender_pid, old_path, true)) return DOB_ERR_DENIED;
    if (!sandbox_check(msg->sender_pid, new_path, true)) return DOB_ERR_DENIED;

    if (ex.active)
    {
        int rc = ex.api->rename(ex.vol, old_path, new_path);
        if (rc == 0)  { disk_flush(); return DOB_OK; }
        if (rc == -5) return DOB_ERR_DENIED;       /* target exists */
        if (rc == -4) return DOB_ERR_NO_SPACE;
        if (rc == -2) return DOB_ERR_HW_FAULT;
        return DOB_ERR_INVALID;
    }

    /* Resolve old entry */
    fat32_dirent_t de;
    uint32_t dc, di;
    if (!resolve_full_path(old_path, &de, &dc, &di))
        return DOB_ERR_NOT_FOUND;

    /* Get cluster and size from old entry */
    uint32_t first_cl = DE_CLUSTER(de);
    uint32_t file_size = de.file_size;
    uint8_t attr = de.attr;

    /* Resolve new parent directory */
    uint32_t new_parent;
    const char *new_basename;
    if (!resolve_path(new_path, &new_parent, &new_basename))
        return DOB_ERR_NOT_FOUND;

    /* Check new name doesn't already exist */
    fat32_dirent_t dup;
    if (resolve_full_path(new_path, &dup, NULL, NULL))
        return DOB_ERR_DENIED;  /* Already exists */

    /* Remove old entry */
    dir_remove_entry(dc, dc, di);

    /* Add new entry with same data */
    if (!dir_add_entry(new_parent, new_basename, first_cl, file_size, attr))
    {
        /* Rollback: try to restore old entry (best effort) */
        uint32_t old_parent;
        const char *old_basename;
        if (resolve_path(old_path, &old_parent, &old_basename))
            dir_add_entry(old_parent, old_basename, first_cl, file_size, attr);
        fat_flush();
        return DOB_ERR_INTERNAL;
    }

    fat_flush();
    return DOB_OK;
}

static dob_status_t handle_sandbox_check(dob_msg_t *msg, dob_msg_t *reply)
{
    if (!msg->payload) return DOB_ERR_INVALID;
    const char *path = (const char *)msg->payload;
    pid_t target_pid = (pid_t)msg->arg0;
    bool is_write = (msg->arg1 != 0);

    reply->arg0 = sandbox_check(target_pid, path, is_write) ? 1 : 0;
    return DOB_OK;
}

/* === Multi-instance opcodes (step 5) === */

static void try_mount(void);   /* fwd; defined after the dispatcher */

/* GET_MOUNTED — describe this instance's binding. Returns the
 * fixed-size wire struct dobfs_mounted_info_t. Always valid even
 * before the FS is mounted (partition_lba/partition_sectors will
 * be zero pre-mount; client can detect with partition_sectors==0). */
static dob_status_t handle_get_mounted(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)msg;
    static dobfs_mounted_info_t info;
    memset(&info, 0, sizeof(info));

    /* Provider name from the driver routing we ended up using. */
    const char *prov;
    if (explicit_provider[0]) prov = explicit_provider;
    else                      prov = use_ahci ? "ahci" : "ata";
    strncpy(info.provider, prov, sizeof(info.provider) - 1);

    info.selector          = disk_selector;
    info.partition_lba     = fs.partition_lba;
    info.partition_sectors = fs.partition_sectors;
    strncpy(info.fs_type, ex.active ? "exfat" : "fat32", sizeof(info.fs_type) - 1);
    info.mount_id      = mount_secondary ? secondary_id : 0;
    info.is_root_mount = mount_secondary ? 0 : 1;

    reply->payload      = &info;
    reply->payload_size = sizeof(info);
    return DOB_OK;
}

/* DF — total / used / free counts.
 *
 * Used clusters = scan of the FAT counting non-free entries. A FAT
 * entry of 0 is "free", everything else (including EOC marker and
 * the special low values like 0xFFFFFF7 = bad cluster) counts as
 * used. The reserved clusters 0 and 1 are not counted (we iterate
 * 2..total_clusters+1).
 *
 * O(total_clusters) one-time scan per DF call — for the disk-utility
 * use case (rendering the partition usage bar) this is run once per
 * UI refresh, fine. The fat_cache already absorbs most of the I/O
 * cost on repeat scans within the same session. */
static dob_status_t handle_df(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)msg;
    static dobfs_df_info_t out;
    memset(&out, 0, sizeof(out));

    if (!fs.mounted) try_mount();
    if (!fs.mounted) return DOB_ERR_INTERNAL;

    /* Sanity guard: fat32_mount validates the BPB thoroughly, so
     * this should never trip after a successful mount. Kept as a
     * defence in depth — DF iterates total_clusters times and
     * looping over 4 billion entries because of a stray BPB
     * (which we saw on an unformatted partition before the mount-
     * time validation was added) would lock the server for hours. */
    if (fs.total_clusters == 0 || fs.total_clusters > 0x10000000u)
        return DOB_ERR_INTERNAL;

    uint32_t used = 0;
    for (uint32_t c = 2; c < 2 + fs.total_clusters; c++)
    {
        uint32_t v = fat_read(c) & 0x0FFFFFFF;
        if (v != 0) used++;
    }

    out.cluster_size_bytes = fs.bytes_per_cluster;
    out.total_clusters     = fs.total_clusters;
    out.used_clusters      = used;
    out.total_bytes = (uint64_t)fs.total_clusters * fs.bytes_per_cluster;
    out.used_bytes  = (uint64_t)used              * fs.bytes_per_cluster;
    out.free_bytes  = out.total_bytes - out.used_bytes;

    reply->payload      = &out;
    reply->payload_size = sizeof(out);
    return DOB_OK;
}

/* OPEN_VIEW — surface a DobFiles window on this mount.
 *
 * Mirrors the floppy driver pattern: the mount process itself calls
 * dobfiles_OpenMount via the EPS stub so the DAS doesn't need to
 * know about DobFiles. Only meaningful for secondary mounts — the
 * root mount is already shown via DobFiles' permanent SYSTEM/DATA
 * windows. Calling OPEN_VIEW on the root mount returns DOB_OK
 * without doing anything, since spawning another DobFiles window
 * at "/" would just clone the home view.
 *
 * Root-alias case: when the desktop icon for the boot partition is
 * activated, the spawned DobFileSystem set is_root_alias and never
 * mounted. Forward OPEN_VIEW to the actual root service so the
 * user gets a DobFiles window rooted at "/" of the running system —
 * which is the sensible thing to show for a click on "the boot
 * disk" icon. */
static dob_status_t handle_open_view(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    /* The hijack target port arrived as arg0 from the DAS ipc_call primitive,
     * which forwarded it from ICON_ACTIVATED (the DobFiles window that opened
     * the mount view). 0 = desktop double-click -> spawn a fresh satellite;
     * !=0 -> hijack that specific window. Same contract the floppy/cdrom
     * drivers already honour; this mount path used to hardcode 0, which is why
     * clicking a partition or pendrive in a dialog's mount view opened a new
     * window instead of navigating the dialog in place. */
    uint32_t hijack_port = msg->arg0;
    if (is_root_alias)
    {
        (void)dobfiles_OpenMount("DobFileSystem", "/", hijack_port);
        return DOB_OK;
    }
    if (!mount_secondary) return DOB_OK;

    (void)dobfiles_OpenMount(service_name, "/", hijack_port);
    return DOB_OK;
}

/* Message Dispatcher */

/* On-demand mount: if the filesystem isn't mounted yet but a disk
 * driver is now available, mount it. Called on every file operation.
 * Returns immediately if already mounted. */
static void try_mount(void)
{
    if (fs.mounted) return;

    find_ata_driver();
    if (!ata_port) return;

    /* For secondary mounts, partition_lba was set from argv at start
     * and the partition has no SYSTEM/DATA layout to ensure. */
    if (mount_secondary)
        fs.partition_lba = secondary_partition_lba;
    else
        detect_partition();

    if (fat32_mount())
    {
        if (!mount_secondary)
            ensure_layout();
        debug_print("[DobFileSystem] Mounted on-demand.\n");
    }
}

/* Saved payload buffer: try_mount() makes sub-IPC calls (registry,
 * ATA driver) that overwrite the process's IPC buffer; msg->payload
 * points into it, so we need to snapshot before sub-IPC.
 *
 * Sized for PATH payloads only — Write payloads can hit 64KB and must
 * NOT be copied here (they'd be truncated). Safe because we only copy
 * when !fs.mounted, and the only pre-mount messages are path-based
 * (Open, Stat). Write needs a prior Open, so the FS is always mounted
 * by the time large payloads arrive. */
static char saved_payload[DOBFS_PATH_MAX * 2];
static uint32_t saved_payload_size = 0;

static dob_status_t handle_message(dob_msg_t *msg, dob_msg_t *reply)
{
    /* Snapshot only when try_mount actually runs (not yet mounted). */
    saved_payload_size = 0;
    if (!fs.mounted && msg->payload && msg->payload_size > 0)
    {
        uint32_t copy = msg->payload_size;
        if (copy > sizeof(saved_payload) - 1)
            copy = sizeof(saved_payload) - 1;
        memcpy(saved_payload, msg->payload, copy);
        saved_payload[copy] = '\0';
        saved_payload_size = copy;
        msg->payload = saved_payload;
        msg->payload_size = saved_payload_size;
    }

    try_mount();

    dob_status_t result;
    switch (msg->code)
    {
        case DOBFS_OPEN:    result = handle_open(msg, reply); break;
        case DOBFS_READ:    result = handle_read(msg, reply); break;
        case DOBFS_WRITE:   result = handle_write(msg, reply); break;
        case DOBFS_CLOSE:   result = handle_close(msg, reply); break;
        case DOBFS_SEEK:    result = handle_seek(msg, reply); break;
        case DOBFS_STAT:    result = handle_stat(msg, reply); break;
        case DOBFS_READDIR: result = handle_readdir(msg, reply); break;
        case DOBFS_MKDIR:   result = handle_mkdir(msg, reply); break;
        case DOBFS_UNLINK:  result = handle_unlink(msg, reply); break;
        case DOBFS_SANDBOX_CHECK: result = handle_sandbox_check(msg, reply); break;
        case DOBFS_GET_MOUNTED: result = handle_get_mounted(msg, reply); break;
        case DOBFS_DF:          result = handle_df(msg, reply); break;
        case DOBFS_OPEN_VIEW:   result = handle_open_view(msg, reply); break;
        case DOBFS_RENAME:  result = handle_rename(msg, reply); break;
        case DOBFS_FORMAT: { /* FORMAT — format disk (installer only) */
            /* arg0 selects the filesystem: 0 = FAT32 (default, backward
             * compatible), 1 = exFAT. Optional volume label in payload. */
            uint32_t fstype   = msg->arg0;
            const char *flabel = (msg->payload && msg->payload_size > 0)
                                 ? (const char *)msg->payload : (const char *)0;
            bool ok = (fstype == 1u) ? exfat_format(flabel) : fat32_format();
            if (!ok)
                { result = DOB_ERR_INTERNAL; break; }
            if (!fat32_mount())   /* detects EXFAT signature and routes to exFAT */
                { result = DOB_ERR_INTERNAL; break; }
            debug_print("[DobFileSystem] Disk formatted and mounted via IPC.\n");
            result = DOB_OK; break;
        }
        case DOBFS_SUBSCRIBE_UNMOUNT: { /* satellite wants the eject event */
            uint32_t port = msg->arg0;
            if (!port) { result = DOB_ERR_INVALID; break; }
            bool dup = false;
            for (uint8_t i = 0; i < unmount_sub_count; i++)
                if (unmount_subs[i] == port) { dup = true; break; }
            if (!dup && unmount_sub_count < UNMOUNT_SUBS_MAX)
                unmount_subs[unmount_sub_count++] = port;
            result = DOB_OK; break;
        }

        case DOBFS_SHUTDOWN: { /* secondary mounts only: medium is gone */
            if (!mount_secondary) { result = DOB_ERR_DENIED; break; }
            debug_print("[DobFileSystem] SHUTDOWN: flushing and exiting\n");
            /* Flush every open fd (data + metadata), then the FAT. With
             * the medium already unplugged these writes will fail and
             * say so on the console — correct: the data is lost, the
             * log names it. With the medium still present (graceful
             * path) everything lands. */
            for (int i = 3; i < DOBFS_MAX_FDS; i++)
            {
                dobfs_fd_t *f = &fd_table[i];
                if (!f->used) continue;
                if (f->buf_dirty) fd_flush_write(f);
                if (f->meta_dirty && f->dir_cluster != 0)
                    dir_update_entry(f->dir_cluster, f->dir_entry_index,
                                     f->file_size, f->first_cluster);
            }
            fat_flush();
            /* Tell the satellites (DobFiles closes its window on 17). */
            for (uint8_t i = 0; i < unmount_sub_count; i++)
            {
                dob_msg_t n; memset(&n, 0, sizeof(n));
                n.code = DOBFS_UNMOUNT_NOTIFY;
                dob_ipc_post(unmount_subs[i], &n);
            }
            dob_server_request_exit();   /* exit AFTER this reply */
            result = DOB_OK; break;
        }

        case DOBFS_REMOUNT: { /* REMOUNT — unmount and remount filesystem */
            memset(&fs, 0, sizeof(fs));
            memset(fd_table, 0, sizeof(fd_table));
            find_ata_driver();
            if (!ata_port) { result = DOB_ERR_INTERNAL; break; }
            if (mount_secondary)
                fs.partition_lba = secondary_partition_lba;
            else
                detect_partition();
            if (!fat32_mount()) { result = DOB_ERR_INTERNAL; break; }
            debug_print("[DobFileSystem] Remounted via IPC.\n");
            result = DOB_OK; break;
        }
        case DOBFS_ENSURE_LAYOUT: { /* ENSURE_LAYOUT — create SYSTEM/DATA directory structure */
            if (!fs.mounted) { result = DOB_ERR_INTERNAL; break; }
            ensure_layout();
            debug_print("[DobFileSystem] Layout ensured via IPC.\n");
            result = DOB_OK; break;
        }
        default: result = DOB_ERR_INVALID; break;
    }

    /* Periodic orphan cleanup — runs AFTER the handler has finished,
     * so msg->payload is no longer needed and sub-IPC calls are safe. */
    if (++msg_counter >= ORPHAN_CHECK_INTERVAL)
    {
        msg_counter = 0;
        fd_reclaim_orphans();
    }

    return result;
}

/* Entry Point */

/* Parse --mount <opts>
 *
 * <opts> is one or more argv tokens, each token being a comma-separated
 * list of key=value pairs. Both forms are accepted:
 *   --mount provider=ahci token=42 fs=fat32        (space-separated, 3 tokens)
 *   --mount provider=ahci,token=42,fs=fat32        (comma-packed, 1 token)
 *   --mount provider=ahci selector=2 lba=63 id=42  (mixed, explicit override)
 *
 * The comma-packed form is what the DAS uses, because the DAS interpreter
 * has a low cap on tokens per primitive (DAS_MAX_TOKENS_PER_ACTION = 6),
 * and a spawn line with on_fail handler eats most of that budget. Manual
 * invocations (installer, test harnesses) can use either.
 *
 * Two ways to identify the partition:
 *   token=T          packed (partition_index << 24) | (native_selector & 0xFFFFFF);
 *                    init reads sector 0 of the disk to find the partition's
 *                    actual start LBA. This is what the DAS uses — $token
 *                    comes from the subdevice payload.
 *   selector=N lba=L explicit alternative (e.g. an installer that already
 *                    knows the partition geometry).
 *
 * If both are given, explicit selector/lba win. fs= is informational
 * (only fat32 supported today). id= defaults to the token value when
 * unset, so the registered service name is stable. Returns true if
 * --mount was found. */
static void
apply_mount_kv(const char *key, const char *val,
               bool *have_explicit_lba, bool *have_explicit_selector,
               bool *have_id, uint32_t *token, bool *have_token)
{
    if (strcmp(key, "provider") == 0)
        strncpy(explicit_provider, val, sizeof(explicit_provider) - 1);
    else if (strcmp(key, "selector") == 0)
    {
        disk_selector = (uint32_t)atoi(val);
        *have_explicit_selector = true;
    }
    else if (strcmp(key, "lba") == 0)
    {
        secondary_partition_lba = (uint32_t)atoi(val);
        *have_explicit_lba = true;
    }
    else if (strcmp(key, "token") == 0)
    {
        *token      = (uint32_t)atoi(val);
        *have_token = true;
    }
    else if (strcmp(key, "id") == 0)
    {
        secondary_id = (uint32_t)atoi(val);
        *have_id     = true;
    }
    /* fs=<name> is informational. Only fat32 is supported today;
     * an unrecognised value will fail at fat32_mount. */
}

static bool
parse_mount_argv(int argc, char **argv)
{
    /* Loop from i=0: DAS-spawned children see argv[0] = binary path
     * (das_exec_one passes it as child_argv[0]), while manually spawned
     * children see argv[0] = the first real argument (e.g. "--mount").
     * Starting at 0 finds --mount in either case; argv[0]=path simply
     * doesn't compare equal to "--mount" and is skipped. */
    int mount_idx = -1;
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], "--mount") == 0) { mount_idx = i; break; }
    if (mount_idx < 0) return false;

    bool have_explicit_lba      = false;
    bool have_explicit_selector = false;
    bool have_id                = false;
    uint32_t token              = 0;
    bool     have_token         = false;

    for (int i = mount_idx + 1; i < argc; i++)
    {
        /* Each token may itself be a comma-separated list. Tokenise
         * destructively (we own the argv strings on MainDOB — they
         * came from a heap-copied blob built by libdob/include/dob/spawn.h). */
        char *cursor = argv[i];
        while (*cursor)
        {
            char *comma = strchr(cursor, ',');
            if (comma) *comma = '\0';

            char *eq = strchr(cursor, '=');
            if (eq)
            {
                *eq = '\0';
                apply_mount_kv(cursor, eq + 1,
                               &have_explicit_lba, &have_explicit_selector,
                               &have_id, &token, &have_token);
            }
            if (!comma) break;
            cursor = comma + 1;
        }
    }

    /* Decode token if present. The bitfield split mirrors
     * libdob/src/partition.c:make_token. */
    if (have_token)
    {
        uint32_t token_selector       = token & 0x00FFFFFFu;
        uint32_t token_partition_idx  = (token >> 24) & 0xFFu;
        if (!have_explicit_selector) disk_selector             = token_selector;
        if (!have_explicit_lba)
        {
            secondary_partition_index  = token_partition_idx;
            secondary_lba_from_token   = true;
        }
        if (!have_id) secondary_id = token;
    }
    return true;
}

/* ===== Root-on-exFAT pivot (boot-time root mount only) =====
 *
 * Model: the FAT32 partition is a minimal boot stub (kernel, GRUB, the
 * disk driver, DobFileSystem and exfat.mem). The bulk of the system —
 * /SYSTEM, programs, /DATA — lives on a large exFAT partition. After the
 * kernel has loaded the boot modules from FAT32 and we have mounted the
 * FAT32 stub as the initial root, we read exfat.mem from the stub and
 * re-point the live filesystem at the exFAT partition: from that moment
 * every file access is served from exFAT ("la lettura si sposta").
 *
 * The chicken-and-egg: ensure_exfat_loaded() reads exfat.mem with
 * dobfs_Open(), an IPC to "DobFileSystem" — i.e. to ourselves. In main(),
 * before the dispatch loop, that deadlocks. So the pivot loads exfat.mem
 * via the FAT32 INTERNAL read path (resolve_full_path + read_cluster),
 * never IPC. The marker /SYSTEM/CONFIG/Root_volume on the stub holds the
 * exFAT partition's absolute start LBA (decimal). Absent marker => no
 * pivot, classic FAT32 root, zero behavioural change. */

/* Read a file from the currently-mounted FAT32 into buf by walking the
 * cluster chain directly (no IPC). Returns bytes read, or -1 on error. */
static int fat32_read_file_internal(const char *path, uint8_t *buf, uint32_t cap)
{
    fat32_dirent_t de;
    if (!resolve_full_path(path, &de, NULL, NULL)) return -1;
    if (de.attr & ATTR_DIRECTORY)                  return -1;

    uint32_t size = de.file_size;
    if (size > cap) size = cap;

    static uint8_t cbuf[MAX_CLUSTER_BYTES];
    uint32_t off = 0;
    uint32_t cl  = DE_CLUSTER(de);
    while (off < size && cl >= 2 && !fat_is_eoc(cl))
    {
        if (!read_cluster(cl, cbuf)) return -1;
        uint32_t chunk = fs.bytes_per_cluster;
        if (chunk > size - off) chunk = size - off;
        memcpy(buf + off, cbuf, chunk);
        off += chunk;
        cl = fat_read(cl);
    }
    return (int)off;
}

/* Load exfat.mem from the FAT32 stub (internal read) into the .mem
 * loader, setting ex.api. Idempotent. */
static bool load_exfat_mem_internal(void)
{
    if (ex.api) return true;
    static uint8_t blob[EXFAT_BLOB_MAX];
    int n = fat32_read_file_internal(EXFAT_MEM_PATH, blob, sizeof(blob));
    if (n <= 0)
    {
        debug_print("[DobFileSystem] pivot: exfat.mem missing on FAT32 stub\n");
        return false;
    }
    ex.api = (exfat_api_t *)dob_mem_load(blob, (uint32_t)n);
    if (!ex.api)
    {
        debug_print("[DobFileSystem] pivot: dob_mem_load(exfat.mem) failed\n");
        return false;
    }
    return true;
}

/* Read /SYSTEM/CONFIG/Root_volume (the exFAT root partition INDEX, 0..3),
 * look it up in the MBR, verify it is exFAT, and return its absolute start
 * LBA. Returns false if the marker is absent, or the slot is not a valid
 * exFAT volume (=> no pivot). Re-deriving the LBA from the MBR each boot
 * keeps the marker valid across repartitioning as long as the exFAT volume
 * stays in the same MBR slot, and the signature check is a safety net
 * against pivoting onto a wrong/clobbered partition. */
static bool resolve_root_volume_lba(uint32_t *out_lba)
{
    uint8_t buf[64];
    int n = fat32_read_file_internal("/SYSTEM/CONFIG/Root_volume", buf, sizeof(buf) - 1);
    if (n <= 0) return false;
    buf[n] = '\0';

    /* Parse the leading decimal partition index. */
    uint32_t v = 0;
    bool any = false;
    for (int i = 0; i < n; i++)
    {
        uint8_t c = buf[i];
        if (c >= '0' && c <= '9') { v = v * 10u + (uint32_t)(c - '0'); any = true; }
        else if (any)             break;
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        else                      break;
    }
    if (!any || v >= MBR_MAX_PRIMARY) return false;

    /* Look the partition up in the MBR (raw absolute sector 0). */
    uint8_t mbr[SECTOR_SIZE];
    if (!disk_read_raw_sector(0, mbr)) return false;
    mbr_table_t tbl;
    partition_mbr_parse(mbr, &tbl);
    if (!tbl.valid_signature) return false;

    const mbr_partition_t *e = &tbl.entries[v];
    if (e->sectors == 0)            return false;
    if (e->type != MBR_TYPE_EXFAT)  return false;

    /* Confirm the exFAT boot-sector signature at the partition start. */
    uint8_t bs[SECTOR_SIZE];
    if (!disk_read_raw_sector(e->start_lba, bs)) return false;
    static const char exsig[8] = { 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' ' };
    for (int k = 0; k < 8; k++)
        if (bs[3 + k] != (uint8_t)exsig[k]) return false;

    *out_lba = e->start_lba;
    return true;
}

int main(int argc, char **argv)
{
    set_priority(1);
    debug_print("[DobFileSystem] Starting DobFileSystem...\n");
    memset(fd_table, 0, sizeof(fd_table));
    memset(&fs, 0, sizeof(fs));

    /* Argv branch: secondary mount mode if --mount is present.
     * Service name and mount geometry come from argv; we skip MBR
     * detection (we already know the partition_lba) and ensure_layout
     * (this isn't the SYSTEM/DATA root). */
    mount_secondary = parse_mount_argv(argc, argv);
    if (mount_secondary)
        snprintf(service_name, sizeof(service_name), "dobfs_%u", secondary_id);

    /* Register the service FIRST, before any disk I/O.
     *
     * Previously we mounted the filesystem and only then registered, so
     * the whole find_ata_driver + detect_partition + fat32_mount +
     * ensure_layout sequence ran while we were still invisible in the
     * registry. Any client doing dob_registry_wait("DobFileSystem", ...)
     * during that window — and on slow hardware or if that sequence hangs
     * or faults — got "DobFileSystem not found". Registering up front
     * makes the service resolvable immediately; the dispatcher already
     * returns an error for file ops while fs.mounted is false, so requests
     * that arrive before the mount completes are refused cleanly (the
     * client's reconnect/retry then succeeds once we finish mounting)
     * rather than failing to find us at all. This is the push model: we
     * announce ourselves, then mount. */
    /* ROOT mount only: register up-front (boot ordering: early clients
     * must resolve us and get clean "not mounted yet" errors).
     *
     * SECONDARY mounts register AFTER the mount instead: their main is
     * sequential, so a pre-mount registration made hotplug's
     * wait_service succeed instantly and the next action step
     * (OPEN_VIEW) park inside our queue until the mount finished — and
     * a WEDGED mount (Armada field) turned that park into a permanent
     * hotplug freeze with no popup. Post-mount registration makes
     * wait_service the bounded "mount done" signal: a stuck mount now
     * costs an 8 s timeout and a mount_failed popup, never a freeze. */
    if (!mount_secondary)
    {
        dob_server_init(service_name);
        dob_server_register(handle_message);
        debug_print("[DobFileSystem] Registered (pre-mount).\n");
    }

    /* Live-CD detection — only meaningful for the PRIMARY mount. A
     * secondary instance is bound by --mount to a real partition on
     * a real disk (the user clicked a partition icon, hotplug spawned
     * us with a token) and must always go through the ATA driver,
     * regardless of how the system itself was booted. */
    if (!mount_secondary)
    {
        setup_live_mode();
        if (live_mode)
            debug_print("[DobFileSystem] live-CD mode: serving RAM blob, read-only.\n");
    }

    /* Mount at startup: find the disk driver (blocking), detect the
     * partition (root mount only), and mount FAT32. The service is
     * already registered (above), so clients that look us up during the
     * mount get a clean "not mounted yet" error and retry, instead of
     * "not found". */
    find_ata_driver();
    {
        char d[80];
        snprintf(d, sizeof(d),
                 "[DobFileSystem] find_ata_driver: ata_port=%u use_ahci=%d\n",
                 ata_port, use_ahci);
        debug_print(d);
    }
    if (ata_port)
    {
        /* Secondary mount via token: read sector 0 and look up the
         * partition at secondary_partition_index. We do this only
         * once at startup; the result is stashed in secondary_partition_lba
         * so REMOUNT can re-derive it without re-reading the MBR. */
        if (mount_secondary && secondary_lba_from_token &&
            secondary_partition_lba == 0)
        {
            uint8_t mbr_buf[SECTOR_SIZE];
            if (disk_read_raw_sector(0, mbr_buf))
            {
                mbr_table_t tbl;
                partition_mbr_parse(mbr_buf, &tbl);
                if (tbl.valid_signature &&
                    secondary_partition_index < MBR_MAX_PRIMARY &&
                    tbl.entries[secondary_partition_index].sectors > 0)
                {
                    secondary_partition_lba =
                        tbl.entries[secondary_partition_index].start_lba;
                }
            }
        }

        /* Detect the "this is actually the root partition" case.
         *
         * The hotplug subdevice emitted by libdob/dob/partition does
         * not (today) suppress partitions already in use by the root
         * mount, so a desktop icon for the root partition appears.
         * Double-clicking it spawns *us* with --mount pointed at the
         * same disk geometry as the running root. Mounting twice
         * would clobber driver state at best and corrupt the
         * filesystem at worst.
         *
         * Solution: ask the root DobFileSystem who it is. If its
         * provider+selector+lba matches what we were spawned for,
         * we set is_root_alias and stop short of fat32_mount. We
         * still register as dobfs_<token> so wait_service in the
         * DAS action succeeds; OPEN_VIEW then forwards to the root
         * service so the user gets a DobFiles window on "/", which
         * is exactly what they'd want for the boot disk anyway.    */
        if (mount_secondary)
        {
            dobfs_mounted_info_t root_info;
            if (dobfs_GetMountedOn("DobFileSystem", &root_info) == 0 &&
                root_info.is_root_mount)
            {
                const char *want_prov = explicit_provider[0]
                                       ? explicit_provider
                                       : (use_ahci ? "ahci" : "ata");
                if (strcmp(root_info.provider, want_prov) == 0 &&
                    root_info.selector       == disk_selector &&
                    root_info.partition_lba  == secondary_partition_lba)
                {
                    is_root_alias = true;
                    debug_print("[DobFileSystem] root-alias mode "
                                "(--mount targets root partition)\n");
                }
            }
        }

        if (is_root_alias)
        {
            /* Skip mount: leave fs zeroed. The dispatcher rejects
             * file ops in this state, which is fine — the only
             * opcode we expect to receive is OPEN_VIEW. */
        }
        else
        {
            if (mount_secondary)
                fs.partition_lba = secondary_partition_lba;
            else
            {
                debug_print("[DobFileSystem] root: detecting partition...\n");
                detect_partition();
                {
                    char d[80];
                    snprintf(d, sizeof(d),
                             "[DobFileSystem] partition_lba=%u use_ahci=%d\n",
                             fs.partition_lba, use_ahci);
                    debug_print(d);
                }
            }

            debug_print("[DobFileSystem] mounting FAT32...\n");
            if (fat32_mount())
            {
                /* ensure_layout creates /SYSTEM and /DATA subtrees if
                 * missing, which means writes. Skip in live mode (the
                 * live blob already carries the full layout and the
                 * volume is read-only anyway) and on secondary mounts
                 * (those are arbitrary user content, not the canonical
                 * root layout). */
                if (!mount_secondary && !live_mode)
                    ensure_layout();
                debug_print("[DobFileSystem] Mounted at startup.\n");

                /* Root-on-exFAT pivot. Root mount, non-live only, gated on
                 * the /SYSTEM/CONFIG/Root_volume marker. Both reads below run
                 * while the root is still FAT32; only then do we re-point the
                 * partition offset and switch the live mount to exFAT. On any
                 * failure the FAT32 root is left intact. */
                if (!mount_secondary && !live_mode)
                {
                    uint32_t exfat_lba = 0;
                    if (resolve_root_volume_lba(&exfat_lba) &&
                        load_exfat_mem_internal())
                    {
                        uint32_t saved_lba = fs.partition_lba;
                        fs.partition_lba = exfat_lba;
                        if (exfat_try_mount())
                        {
                            debug_print("[DobFileSystem] root pivoted to exFAT volume.\n");
                        }
                        else
                        {
                            fs.partition_lba = saved_lba;   /* keep FAT32 root intact */
                            debug_print("[DobFileSystem] exFAT pivot FAILED; staying on FAT32.\n");
                        }
                    }
                }
            }
            else
            {
                debug_print("[DobFileSystem] FAT32 mount FAILED.\n");
            }
        }
    }

    if (mount_secondary)
    {
        if (!fs.mounted && !is_root_alias)
        {
            /* No registration: hotplug's wait_service dobfs_<id> times
             * out (bounded) and reports mount_failed. A registered but
             * broken secondary would only collect parked callers. */
            debug_print("[DobFileSystem] secondary mount failed; "
                        "exiting unregistered\n");
            return 1;
        }
        dob_server_init(service_name);
        dob_server_register(handle_message);
        debug_print("[DobFileSystem] Registered (post-mount).\n");
    }

    /* Root: registered up front; mount may have failed, in which case
     * the dispatcher returns clean errors and try_mount() retries. */
    debug_print("[DobFileSystem] Ready.\n");
    dob_server_loop();
    /* Reached only via dob_server_request_exit (DOBFS_SHUTDOWN). */
    debug_print("[DobFileSystem] secondary mount exiting\n");
    return 0;
}
