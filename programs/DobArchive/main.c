/* MainDOB DobArchive — service "archiveN" (N = first free slot 0..15).
 *
 * Streaming, seek-based: opens the archive once, every read goes through
 * dobfs_Seek + dobfs_Read so we never slurp the whole file (scales to the
 * 4 GiB ZIP format limit).
 *
 * Spawned on demand with the archive path as the first non-flag argv.
 * The optional flag "--silent" (anywhere in argv) suppresses the default
 * DobFiles auto-mount, for headless callers like DobInstaller.
 *
 * Two disjoint opcode ranges on the same port:
 *    1..18     dobfs_protocol  — OPEN/READ/CLOSE/STAT/READDIR/SEEK (v2);
 *                                writes return DOB_ERR_DENIED (R/O).
 *    100..199  archive_protocol — COUNT / GET_FORMAT / ENTRY_INFO /
 *                                 READ_ENTRY / EXTRACT_TO / CLOSE
 *
 * Lifecycle: parse argv; dobfs_Open arc_fd (kept for the whole server
 * lifetime); if the file is 0 bytes, stamp the empty-archive marker; parse
 * tar or zip into a flat metadata index (data stays on disk); pick a free
 * archiveN slot, register; call dobfiles_OpenMount(service, "/") (DobFiles
 * replies before callback readdir, so no deadlock); serve until
 * ARCHIVE_CLOSE.
 *
 * Streaming invariant: arc_fd has one shared cursor. Every read path must
 * dobfs_Seek first — never assume the cursor is where you left it.
 *
 * Layers:
 *   L0  arcio_*    seek/read on the persistent fd
 *   L0b skeleton_* stamp an empty archive on a 0-byte file
 *   L1  tar_*      ustar parser
 *   L2  zip_*      zip central-directory parser (stored entries)
 *   L3  index_*    unified entry table + directory synthesis
 *   L4  path_*     virtual path → entry index
 *   L5  op_fs_*    dobfs_protocol handlers
 *   L6  op_arch_*  archive_protocol handlers
 *   L7  archive_*  dispatch + bootstrap */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <dob/types.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/server.h>

#include <DobFileSystem.h>
#include <DobFiles.h>
#include <DobPopup.h>

#include "../../boot/DobFileSystem/dobfs_protocol.h"
#include "archive_protocol.h"

/* Tunables */

#define MAX_ENTRIES        4096       /* index slots — metadata only    */
#define MAX_FDS              32       /* concurrent dobfs opens          */
#define MAX_ARCHIVE_SLOTS    16       /* archive0..archive15 pool        */

#define TAR_BLOCK_SIZE      512
#define TAR_NAME_LEN        100
#define TAR_MAGIC_USTAR     "ustar"

#define ZIP_EOCD_SIG        0x06054b50u
#define ZIP_CDFH_SIG        0x02014b50u
#define ZIP_LFH_SIG         0x04034b50u
#define ZIP_EOCD_MIN_LEN    22
#define ZIP_MAX_COMMENT     65535u

#define READDIR_BUF_SIZE    4096
#define READ_REPLY_CAP      4096
#define EOCD_SCAN_WINDOW    (ZIP_EOCD_MIN_LEN + ZIP_MAX_COMMENT)

/* State */

typedef struct
{
    char     path[ARCHIVE_MAX_PATH];
    uint32_t size;
    uint32_t comp_size;
    uint32_t data_offset;    /* valid if !staged                  */
    uint32_t mode;
    uint32_t mtime;
    uint32_t crc;            /* per-entry CRC-32; populated by zip_index
                              * for non-staged entries so that deflate
                              * entries can be re-emitted verbatim with
                              * their original CRC. Staged entries
                              * compute CRC at emit time. 0 for TAR. */
    uint8_t  type;
    uint8_t  compression;
    uint8_t  synthetic;
    uint8_t  staged;         /* 1: data lives in stage_buf, not on disk */
    uint8_t  _pad[3];

    /* Populated iff staged==1. Owned by the entry: kept until the
     * entry is flushed to disk or removed. For directories this
     * stays NULL. */
    uint8_t *stage_buf;
    uint32_t stage_cap;
} entry_t;

typedef struct
{
    bool     in_use;
    bool     writable;       /* true for FS_WRITE fds                 */
    uint32_t entry_idx;
    uint32_t pos;
} fd_entry_t;

static int        arc_fd     = -1;
static uint32_t   arc_size   = 0;
static uint8_t    arc_format = ARCHIVE_FMT_UNKNOWN;

static entry_t    entries[MAX_ENTRIES];
static uint32_t   entry_count = 0;

static fd_entry_t fds[MAX_FDS];

static char       service_name[16];

/* Archive path on disk — needed by rebuild_and_flush to open the
 * temporary file + rename into place. Captured once at bootstrap. */
static char       archive_path_g[ARCHIVE_MAX_PATH];

/* Set by any handler that mutates the archive state (write, unlink,
 * mkdir, rename). rebuild_and_flush clears it after a successful
 * on-disk commit. */
static bool       arc_dirty = false;

static uint8_t    read_reply_buf[READ_REPLY_CAP];
static char       readdir_buf[READDIR_BUF_SIZE];

/*  *  L0  —  arcio_*  : seek+read helpers on the persistent archive fd
 *
 *  Every function in the parser and every handler that delivers entry
 *  bytes funnels through these two helpers. They are the ONLY places
 *  that touch arc_fd directly. If you need to pull bytes from the
 *  archive, do it here.
 */

static bool
arcio_read_at(uint32_t off, void *dst, uint32_t n)
{
    if (arc_fd < 0)                                   return false;
    if (dobfs_Seek(arc_fd, (long)off, FS_SEEK_SET) < 0) return false;

    uint8_t *p = (uint8_t *)dst;
    uint32_t left = n;
    while (left > 0)
    {
        int got = dobfs_Read(arc_fd, p, left);
        if (got <= 0) return false;
        p    += (uint32_t)got;
        left -= (uint32_t)got;
    }
    return true;
}

static int
arcio_read_partial(uint32_t off, void *dst, uint32_t max)
{
    if (arc_fd < 0)                                   return -1;
    if (dobfs_Seek(arc_fd, (long)off, FS_SEEK_SET) < 0) return -1;

    uint8_t *p = (uint8_t *)dst;
    uint32_t left = max;
    uint32_t total = 0;
    while (left > 0)
    {
        int got = dobfs_Read(arc_fd, p + total, left);
        if (got < 0)  return -1;
        if (got == 0) break;
        total += (uint32_t)got;
        left  -= (uint32_t)got;
    }
    return (int)total;
}

/* L0b  —  skeleton_*  : stamp a minimal archive on a 0-byte file */

static const uint8_t EMPTY_ZIP_BYTES[22] = {
    0x50, 0x4B, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t
skeleton_format_from_extension(const char *basename)
{
    for (const char *p = basename; *p; p++)
    {
        if (*p != '.') continue;
        char c1 = (p[1] >= 'A' && p[1] <= 'Z') ? (char)(p[1] + 32) : p[1];
        char c2 = (p[2] >= 'A' && p[2] <= 'Z') ? (char)(p[2] + 32) : p[2];
        char c3 = (p[3] >= 'A' && p[3] <= 'Z') ? (char)(p[3] + 32) : p[3];
        if (c1 == 'z' && c2 == 'i' && c3 == 'p' && p[4] == '\0') return ARCHIVE_FMT_ZIP;
        if (c1 == 't' && c2 == 'a' && c3 == 'r' && p[4] == '\0') return ARCHIVE_FMT_TAR;
    }
    return ARCHIVE_FMT_TAR;
}

static uint8_t
skeleton_stamp_empty(const char *path, const char *basename)
{
    uint8_t fmt = skeleton_format_from_extension(basename);

    int fd = dobfs_Open(path, FS_WRITE | FS_TRUNC);
    if (fd < 0) return ARCHIVE_FMT_UNKNOWN;

    if (fmt == ARCHIVE_FMT_ZIP)
    {
        int n = dobfs_Write(fd, EMPTY_ZIP_BYTES, sizeof(EMPTY_ZIP_BYTES));
        dobfs_Close(fd);
        if (n != (int)sizeof(EMPTY_ZIP_BYTES)) return ARCHIVE_FMT_UNKNOWN;
        return ARCHIVE_FMT_ZIP;
    }

    static uint8_t zero_block[TAR_BLOCK_SIZE];
    int n1 = dobfs_Write(fd, zero_block, TAR_BLOCK_SIZE);
    int n2 = dobfs_Write(fd, zero_block, TAR_BLOCK_SIZE);
    dobfs_Close(fd);
    if (n1 != TAR_BLOCK_SIZE || n2 != TAR_BLOCK_SIZE) return ARCHIVE_FMT_UNKNOWN;
    return ARCHIVE_FMT_TAR;
}

/* L1  —  tar_*   : ustar parser (streaming) */

static uint32_t
tar_parse_octal(const char *p, uint32_t len)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < len && p[i]; i++)
    {
        char c = p[i];
        if (c == ' ' || c == '\0') { if (v) break; else continue; }
        if (c < '0' || c > '7')    break;
        v = (v << 3) | (uint32_t)(c - '0');
    }
    return v;
}

static bool
tar_block_is_zero(const uint8_t *hdr)
{
    for (uint32_t i = 0; i < TAR_BLOCK_SIZE; i++)
        if (hdr[i]) return false;
    return true;
}

static bool
tar_block_is_ustar(const uint8_t *hdr)
{
    return memcmp(hdr + 257, TAR_MAGIC_USTAR, 5) == 0;
}

static bool
tar_normalise_name(const char *src, uint8_t typeflag, char *out, uint32_t out_cap)
{
    uint32_t skip = 0;
    for (;;)
    {
        if (src[skip] == '/') { skip++; continue; }
        if (src[skip] == '.' && (src[skip + 1] == '/' || src[skip + 1] == '\0'))
            { skip += (src[skip + 1] == '/') ? 2 : 1; continue; }
        break;
    }
    uint32_t slen = 0;
    while (skip + slen < TAR_NAME_LEN && src[skip + slen]) slen++;
    if (slen == 0) return false;
    if (typeflag == '5' && src[skip + slen - 1] == '/') slen--;
    if (slen == 0) return false;
    if (1 + slen + 1 > out_cap) return false;
    out[0] = '/';
    memcpy(out + 1, src + skip, slen);
    out[1 + slen] = '\0';
    return true;
}

static bool
tar_index(void)
{
    uint8_t hdr[TAR_BLOCK_SIZE];
    uint32_t off = 0;

    while (off + TAR_BLOCK_SIZE <= arc_size)
    {
        if (!arcio_read_at(off, hdr, TAR_BLOCK_SIZE)) return false;

        if (tar_block_is_zero(hdr))
        {
            if (off + 2 * TAR_BLOCK_SIZE <= arc_size)
            {
                uint8_t peek[TAR_BLOCK_SIZE];
                if (!arcio_read_at(off + TAR_BLOCK_SIZE, peek, TAR_BLOCK_SIZE))
                    return false;
                if (tar_block_is_zero(peek)) break;
            }
            off += TAR_BLOCK_SIZE;
            continue;
        }

        if (!tar_block_is_ustar(hdr)) return false;

        const char *name     = (const char *)(hdr + 0);
        uint32_t    mode     = tar_parse_octal((const char *)(hdr + 100), 8);
        uint32_t    size     = tar_parse_octal((const char *)(hdr + 124), 12);
        uint32_t    mtime    = tar_parse_octal((const char *)(hdr + 136), 12);
        uint8_t     typeflag = hdr[156];

        uint32_t data_off   = off + TAR_BLOCK_SIZE;
        uint32_t data_end   = data_off + size;
        uint32_t padded_end = (data_end + TAR_BLOCK_SIZE - 1)
                            & ~(uint32_t)(TAR_BLOCK_SIZE - 1);
        if (data_end > arc_size) return false;

        bool keep = (typeflag == '0' || typeflag == '\0' || typeflag == '5');
        if (keep && entry_count < MAX_ENTRIES)
        {
            char vpath[ARCHIVE_MAX_PATH];
            if (tar_normalise_name(name, typeflag, vpath, sizeof(vpath)))
            {
                entry_t *e = &entries[entry_count++];
                memset(e, 0, sizeof(*e));
                strncpy(e->path, vpath, sizeof(e->path) - 1);
                e->size        = size;
                e->comp_size   = size;
                e->data_offset = data_off;
                e->mode        = mode;
                e->mtime       = mtime;
                e->type        = (typeflag == '5') ? ARCHIVE_TYPE_DIR
                                                   : ARCHIVE_TYPE_FILE;
                e->compression = 0;
            }
        }
        off = padded_end;
    }

    arc_format = ARCHIVE_FMT_TAR;
    return true;
}

/* L2  —  zip_*   : central-directory parser (streaming) */

static uint16_t
zip_read16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t
zip_read32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint8_t  eocd_tail[EOCD_SCAN_WINDOW];
static uint32_t eocd_tail_size = 0;
static uint32_t eocd_tail_base = 0;

static bool
zip_load_tail(void)
{
    uint32_t n = arc_size < EOCD_SCAN_WINDOW ? arc_size : EOCD_SCAN_WINDOW;
    eocd_tail_base = arc_size - n;
    eocd_tail_size = n;
    return arcio_read_at(eocd_tail_base, eocd_tail, n);
}

static uint32_t
zip_find_eocd(void)
{
    if (arc_size < ZIP_EOCD_MIN_LEN)       return 0xFFFFFFFFu;
    if (!zip_load_tail())                  return 0xFFFFFFFFu;
    if (eocd_tail_size < ZIP_EOCD_MIN_LEN) return 0xFFFFFFFFu;

    for (uint32_t i = eocd_tail_size - ZIP_EOCD_MIN_LEN; ; i--)
    {
        if (zip_read32(eocd_tail + i) == ZIP_EOCD_SIG)
        {
            uint16_t clen = zip_read16(eocd_tail + i + 20);
            if ((uint32_t)i + ZIP_EOCD_MIN_LEN + clen == eocd_tail_size)
                return eocd_tail_base + i;
        }
        if (i == 0) break;
    }
    return 0xFFFFFFFFu;
}

static bool
zip_index(void)
{
    uint32_t eocd_off = zip_find_eocd();
    if (eocd_off == 0xFFFFFFFFu) return false;

    uint32_t   eocd_local = eocd_off - eocd_tail_base;
    const uint8_t *eocd   = eocd_tail + eocd_local;

    uint16_t total_entries = zip_read16(eocd + 10);
    uint32_t cd_size       = zip_read32(eocd + 12);
    uint32_t cd_offset     = zip_read32(eocd + 16);

    if (cd_offset > arc_size || cd_offset + cd_size > arc_size) return false;
    if (cd_size == 0 && total_entries == 0)
    {
        arc_format = ARCHIVE_FMT_ZIP;
        return true;
    }

    #define CD_WINDOW 8192
    static uint8_t cdwin[CD_WINDOW];

    uint32_t consumed = 0;
    uint32_t win_base = 0;
    uint32_t win_have = 0;

    for (uint32_t i = 0; i < total_entries; i++)
    {
        if (consumed + 46 > cd_size) return false;

        uint32_t rel = consumed - win_base;
        if (rel + 46 > win_have)
        {
            win_base = consumed;
            uint32_t want = cd_size - consumed;
            if (want > CD_WINDOW) want = CD_WINDOW;
            if (!arcio_read_at(cd_offset + win_base, cdwin, want)) return false;
            win_have = want;
            rel = 0;
        }

        if (zip_read32(cdwin + rel) != ZIP_CDFH_SIG) return false;

        uint16_t method       = zip_read16(cdwin + rel + 10);
        uint32_t mtime_dosdate = zip_read32(cdwin + rel + 12);
        uint32_t crc32_val    = zip_read32(cdwin + rel + 16);
        uint32_t comp_size    = zip_read32(cdwin + rel + 20);
        uint32_t uncomp_size  = zip_read32(cdwin + rel + 24);
        uint16_t name_len     = zip_read16(cdwin + rel + 28);
        uint16_t extra_len    = zip_read16(cdwin + rel + 30);
        uint16_t comment_len  = zip_read16(cdwin + rel + 32);
        uint32_t ext_attrs    = zip_read32(cdwin + rel + 38);
        uint32_t lfh_offset   = zip_read32(cdwin + rel + 42);

        uint32_t rec_size = 46u + name_len + extra_len + comment_len;
        if (consumed + rec_size > cd_size) return false;

        if (rel + rec_size > win_have)
        {
            if (rec_size > CD_WINDOW) return false;
            win_base = consumed;
            uint32_t want = cd_size - consumed;
            if (want > CD_WINDOW) want = CD_WINDOW;
            if (!arcio_read_at(cd_offset + win_base, cdwin, want)) return false;
            win_have = want;
            rel = 0;
        }

        const char *name = (const char *)(cdwin + rel + 46);

        uint32_t data_off = 0;
        uint8_t lfh[30];
        if (lfh_offset + 30 <= arc_size
         && arcio_read_at(lfh_offset, lfh, 30)
         && zip_read32(lfh) == ZIP_LFH_SIG)
         {
            uint16_t lfh_name_len  = zip_read16(lfh + 26);
            uint16_t lfh_extra_len = zip_read16(lfh + 28);
            data_off = lfh_offset + 30 + lfh_name_len + lfh_extra_len;
            if (data_off + comp_size > arc_size) data_off = 0;
        }

        bool is_dir = (name_len > 0 && name[name_len - 1] == '/');

        if (entry_count < MAX_ENTRIES && (data_off != 0 || (is_dir && comp_size == 0)))
        {
            entry_t *e = &entries[entry_count++];
            memset(e, 0, sizeof(*e));

            uint32_t o = 0;
            if (name_len > 0 && name[0] != '/') e->path[o++] = '/';
            uint32_t copy = name_len;
            if (is_dir && copy > 0) copy--;
            if (o + copy >= sizeof(e->path))
            {
                entry_count--;
            } else
            {
                memcpy(e->path + o, name, copy);
                e->path[o + copy]  = '\0';
                e->size            = uncomp_size;
                e->comp_size       = comp_size;
                e->data_offset     = data_off;
                e->mode            = (ext_attrs >> 16) & 0xFFFFu;
                e->mtime           = mtime_dosdate;
                e->crc             = crc32_val;
                e->type            = is_dir ? ARCHIVE_TYPE_DIR : ARCHIVE_TYPE_FILE;
                e->compression     = (uint8_t)method;
            }
        }

        consumed += rec_size;
    }

    arc_format = ARCHIVE_FMT_ZIP;
    return true;
}

/* L3  —  index_*   : unified entry table */

static int
index_find_exact(const char *path)
{
    for (uint32_t i = 0; i < entry_count; i++)
        if (strcmp(entries[i].path, path) == 0) return (int)i;
    return -1;
}

static bool
index_add_synthetic_dir(const char *path)
{
    if (index_find_exact(path) >= 0) return true;
    if (entry_count >= MAX_ENTRIES)  return false;
    entry_t *e = &entries[entry_count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->type      = ARCHIVE_TYPE_DIR;
    e->synthetic = 1;
    return true;
}

static void
index_synthesise_parents(void)
{
    uint32_t real = entry_count;
    for (uint32_t i = 0; i < real; i++)
    {
        const char *p = entries[i].path;
        char buf[ARCHIVE_MAX_PATH];
        for (uint32_t k = 1; p[k]; k++)
        {
            if (p[k] != '/') continue;
            if (k >= sizeof(buf)) break;
            memcpy(buf, p, k);
            buf[k] = '\0';
            if (!index_add_synthetic_dir(buf)) return;
        }
    }
}

static bool
index_build(void)
{
    entry_count = 0;
    memset(entries, 0, sizeof(entries));

    if (arc_size >= ZIP_EOCD_MIN_LEN
     && zip_find_eocd() != 0xFFFFFFFFu)
     {
        if (zip_index()) { index_synthesise_parents(); return true; }
        entry_count = 0;
    }

    if (arc_size >= TAR_BLOCK_SIZE)
    {
        uint8_t first[TAR_BLOCK_SIZE];
        if (arcio_read_at(0, first, TAR_BLOCK_SIZE))
        {
            bool is_ustar = tar_block_is_ustar(first);
            bool is_empty = (arc_size >= 2 * TAR_BLOCK_SIZE) && tar_block_is_zero(first);
            if (is_ustar || is_empty)
            {
                if (tar_index()) { index_synthesise_parents(); return true; }
                entry_count = 0;
            }
        }
    }

    return false;
}

/* L4  —  path_*   : virtual path resolution */

static void
path_normalise(const char *in, char *out, uint32_t out_cap)
{
    uint32_t o = 0;
    if (!in || in[0] != '/')
    {
        if (out_cap > 1) out[o++] = '/';
    }
    char prev = 0;
    for (uint32_t i = 0; in && in[i] && o + 1 < out_cap; i++)
    {
        char c = in[i];
        if (c == '/' && prev == '/') continue;
        out[o++] = c;
        prev = c;
    }
    if (o > 1 && out[o - 1] == '/') o--;
    if (o == 0 && out_cap > 1) out[o++] = '/';
    out[o] = '\0';
}

static int
path_to_entry(const char *vpath)
{
    char norm[ARCHIVE_MAX_PATH];
    path_normalise(vpath, norm, sizeof(norm));
    if (norm[0] == '/' && norm[1] == '\0') return -2;
    return index_find_exact(norm);
}

static bool
path_is_immediate_child(const char *dir, const char *child)
{
    uint32_t dlen = (uint32_t)strlen(dir);
    uint32_t clen = (uint32_t)strlen(child);
    if (dlen == 1 && dir[0] == '/')
    {
        if (clen < 2 || child[0] != '/') return false;
        for (uint32_t i = 1; i < clen; i++)
            if (child[i] == '/') return false;
        return true;
    }
    if (clen <= dlen + 1)              return false;
    if (memcmp(dir, child, dlen) != 0) return false;
    if (child[dlen] != '/')            return false;
    for (uint32_t i = dlen + 1; i < clen; i++)
        if (child[i] == '/') return false;
    return true;
}

static const char *
path_basename(const char *path)
{
    const char *slash = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') slash = p;
    return (slash == path && *slash == '/') ? slash + 1 : slash + 1;
}

/*  *  L5b  —  stage_*  : per-entry write staging buffers
 *
 *  When a writable fd is opened on a new or truncated entry, the
 *  entry's data_offset becomes meaningless (no on-disk bytes yet).
 *  Instead the entry owns a malloc'd stage_buf that grows as writes
 *  arrive. rebuild_and_flush consumes these buffers when emitting the
 *  new archive on disk.
 *
 *  Ownership: the entry owns stage_buf. It is freed by
 *    - entry_discard_staging() when an unlink removes the entry, or
 *    - rebuild_and_flush() after successfully writing the entry to
 *      disk (the entry's data_offset is then updated to point to the
 *      new on-disk location and `staged` is cleared).
 */

static bool
stage_ensure_cap(entry_t *e, uint32_t need)
{
    if (need <= e->stage_cap) return true;

    /* Growth strategy: at least double, start at 4 KB. The ceiling
     * is practical memory — if malloc fails we surface NO_MEMORY to
     * the caller, which will usually mean "couldn't fit this chunk"
     * rather than "archive is broken". */
    uint32_t newcap = e->stage_cap ? e->stage_cap * 2 : 4096;
    while (newcap < need)
    {
        uint32_t doubled = newcap * 2;
        if (doubled < newcap) return false;   /* overflow */
        newcap = doubled;
    }

    uint8_t *nb = (uint8_t *)malloc(newcap);
    if (!nb) return false;
    if (e->stage_buf && e->size > 0)
        memcpy(nb, e->stage_buf, e->size);
    if (e->stage_buf) free(e->stage_buf);
    e->stage_buf = nb;
    e->stage_cap = newcap;
    return true;
}

/* Begin staging on an entry: clears size, flips staged, allocates
 * a minimal buffer so subsequent writes always see a non-NULL buf. */
static bool
stage_begin(entry_t *e)
{
    if (e->stage_buf) { free(e->stage_buf); e->stage_buf = NULL; e->stage_cap = 0; }
    e->size        = 0;
    e->comp_size   = 0;
    e->data_offset = 0;
    e->staged      = 1;
    e->compression = 0;
    return stage_ensure_cap(e, 4096);
}

/* Free staging for an entry being removed. */
static void
entry_discard_staging(entry_t *e)
{
    if (e->stage_buf) { free(e->stage_buf); e->stage_buf = NULL; }
    e->stage_cap = 0;
    e->staged    = 0;
}

/* Append `n` bytes to an entry's staging buffer at the given position.
 * Extends the entry size if we wrote past the current end. */
static bool
stage_write_at(entry_t *e, uint32_t pos, const void *src, uint32_t n)
{
    uint64_t end = (uint64_t)pos + (uint64_t)n;
    if (end > 0xFFFFFFFFu) return false;         /* 4 GiB per-entry cap */
    if (!stage_ensure_cap(e, (uint32_t)end)) return false;
    memcpy(e->stage_buf + pos, src, n);
    if ((uint32_t)end > e->size) e->size = (uint32_t)end;
    e->comp_size = e->size;
    return true;
}

/* Remove an entry from the table (shifts the tail down by one). Also
 * frees its staging if it had any. */
static void
entry_remove_at(uint32_t idx)
{
    if (idx >= entry_count) return;
    entry_discard_staging(&entries[idx]);
    for (uint32_t i = idx; i + 1 < entry_count; i++)
        entries[i] = entries[i + 1];
    entry_count--;
    memset(&entries[entry_count], 0, sizeof(entries[entry_count]));
}

/*  *  L5c  —  emit_*  : format-specific archive serialisation
 *
 *  These helpers read entry bytes — from stage_buf if staged, from
 *  the old archive via arcio_read_partial otherwise — and write them
 *  to the output fd of the new archive. They never touch arc_fd
 *  for writing; rebuild_and_flush is the single owner of the
 *  temporary output fd.
 */

/* Write `n` bytes to dst_fd, looping over short writes. Returns true
 * on exact success. */
static bool
emit_write(int dst_fd, const void *src, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)src;
    uint32_t left = n;
    while (left)
    {
        int w = dobfs_Write(dst_fd, p, left);
        if (w <= 0) return false;
        p    += (uint32_t)w;
        left -= (uint32_t)w;
    }
    return true;
}

/* Feed the data bytes of an entry to dst_fd. For staged entries the
 * source is the stage buffer; for on-disk entries we stream chunks
 * via arcio_read_partial so we never hold more than READ_REPLY_CAP
 * in RAM at once. */
static bool
emit_entry_data(entry_t *e, int dst_fd)
{
    if (e->staged)
    {
        if (e->size == 0) return true;
        return emit_write(dst_fd, e->stage_buf, e->size);
    }

    /* Non-staged: the bytes on disk are the *compressed* payload.
     * Write comp_size bytes, NOT e->size — for a deflate entry those
     * differ, and using e->size would read past the entry's data into
     * whatever LFH follows, corrupting every compressed entry on
     * rebuild. Stored entries have comp_size == size so the loop
     * reduces to the obvious case. */
    uint32_t total = e->comp_size;
    if (total == 0) return true;

    static uint8_t xfer[READ_REPLY_CAP];
    uint32_t done = 0;
    while (done < total)
    {
        uint32_t chunk = total - done;
        if (chunk > sizeof(xfer)) chunk = sizeof(xfer);
        int got = arcio_read_partial(e->data_offset + done, xfer, chunk);
        if (got <= 0) return false;
        if (!emit_write(dst_fd, xfer, (uint32_t)got)) return false;
        done += (uint32_t)got;
    }
    return true;
}

/* --------------------------------------------------------------
 *  TAR emission
 *
 *  ustar header layout, fixed at 512 bytes:
 *    [  0..100)  name       (NUL-padded)
 *    [100..108)  mode       (octal ASCII + NUL)
 *    [108..116)  uid        (octal ASCII + NUL)
 *    [116..124)  gid        (octal ASCII + NUL)
 *    [124..136)  size       (12-char octal, space or NUL terminator)
 *    [136..148)  mtime      (11-char octal + space)
 *    [148..156)  checksum   ("NNNNNN\0 " after computation)
 *    [156..157)  typeflag   '0' = file, '5' = directory
 *    [157..257)  linkname   (unused, zero)
 *    [257..263)  magic      "ustar\0"
 *    [263..265)  version    "00"
 *    [265..297)  uname      (empty)
 *    [297..329)  gname      (empty)
 *    [329..337)  devmajor   (zero)
 *    [337..345)  devminor   (zero)
 *    [345..500)  prefix     (unused — names > 100 are rejected)
 *    [500..512)  padding    (zero)
 *
 *  Checksum algorithm:
 *    1. Fill the checksum field [148..156) with 8 ASCII spaces.
 *    2. Sum all 512 header bytes as unsigned values.
 *    3. Write the sum into [148..156) as "%06o\0 " — 6 octal digits,
 *       NUL, space. This format is what every tar implementation
 *       expects when validating.
 * -------------------------------------------------------------- */

static void
tar_write_octal(char *dst, uint32_t len, uint32_t val)
{
    /* Right-align an octal representation of `val` into `dst[0..len-1]`,
     * leaving the last byte as either space or NUL depending on the
     * field. We produce NUL-terminated octal for simplicity — GNU tar
     * and libarchive both accept it in all numeric fields. */
    for (uint32_t i = 0; i < len; i++) dst[i] = '0';
    if (len == 0) return;
    dst[len - 1] = '\0';
    for (int32_t i = (int32_t)len - 2; i >= 0 && val > 0; i--)
    {
        dst[i] = (char)('0' + (val & 7));
        val >>= 3;
    }
}

static bool
emit_tar_header(int dst_fd, entry_t *e)
{
    uint8_t hdr[TAR_BLOCK_SIZE];
    memset(hdr, 0, sizeof(hdr));

    /* name: strip the leading '/' (tar paths are relative) */
    const char *nm = e->path;
    if (nm[0] == '/') nm++;
    /* Directory entries carry a trailing '/' in tar convention. */
    uint32_t nlen = (uint32_t)strlen(nm);
    if (e->type == ARCHIVE_TYPE_DIR)
    {
        if (nlen + 1 > 100) return false;
        memcpy(hdr, nm, nlen);
        hdr[nlen] = '/';
    } else
    {
        if (nlen > 100) return false;
        memcpy(hdr, nm, nlen);
    }

    /* mode: default to 0644 (files) or 0755 (dirs) if unset */
    uint32_t mode = e->mode;
    if (mode == 0)
        mode = (e->type == ARCHIVE_TYPE_DIR) ? 0755 : 0644;
    tar_write_octal((char *)(hdr + 100), 8, mode & 0x1FFFFFu);

    /* uid, gid: 0 */
    tar_write_octal((char *)(hdr + 108), 8, 0);
    tar_write_octal((char *)(hdr + 116), 8, 0);

    tar_write_octal((char *)(hdr + 124), 12, (e->type == ARCHIVE_TYPE_DIR) ? 0 : e->size);
    tar_write_octal((char *)(hdr + 136), 12, e->mtime);

    /* Checksum placeholder — 8 spaces. */
    memset(hdr + 148, ' ', 8);

    hdr[156] = (e->type == ARCHIVE_TYPE_DIR) ? '5' : '0';

    /* magic "ustar\0" + version "00" */
    memcpy(hdr + 257, "ustar\0" "00", 8);

    /* Compute checksum. */
    uint32_t sum = 0;
    for (uint32_t i = 0; i < TAR_BLOCK_SIZE; i++) sum += hdr[i];

    /* Write as "NNNNNN\0 " */
    char cs[8];
    for (uint32_t i = 0; i < 6; i++) cs[5 - i] = (char)('0' + (sum & 7)), sum >>= 3;
    cs[6] = '\0';
    cs[7] = ' ';
    memcpy(hdr + 148, cs, 8);

    return emit_write(dst_fd, hdr, TAR_BLOCK_SIZE);
}

static bool
emit_tar_padding(int dst_fd, uint32_t data_len)
{
    uint32_t rem = data_len & (TAR_BLOCK_SIZE - 1);
    if (rem == 0) return true;
    static const uint8_t zeros[TAR_BLOCK_SIZE] = {0};
    return emit_write(dst_fd, zeros, TAR_BLOCK_SIZE - rem);
}

static bool
emit_tar_archive(int dst_fd)
{
    for (uint32_t i = 0; i < entry_count; i++)
    {
        entry_t *e = &entries[i];
        /* Skip synthetic directories — they weren't part of the
         * original archive and the user didn't explicitly add them.
         * They exist only to populate dobfs listings. */
        if (e->synthetic) continue;

        if (!emit_tar_header(dst_fd, e))       return false;
        if (e->type == ARCHIVE_TYPE_FILE)
        {
            if (!emit_entry_data(e, dst_fd))   return false;
            if (!emit_tar_padding(dst_fd, e->size)) return false;
        }
    }
    /* End-of-archive: two zero blocks. */
    static const uint8_t zeros[TAR_BLOCK_SIZE] = {0};
    if (!emit_write(dst_fd, zeros, TAR_BLOCK_SIZE)) return false;
    if (!emit_write(dst_fd, zeros, TAR_BLOCK_SIZE)) return false;
    return true;
}

/* --------------------------------------------------------------
 *  ZIP emission (stored, method 0)
 *
 *  LFH (Local File Header, 30 bytes + name + extra):
 *    signature         4    0x04034b50
 *    version           2    20 (2.0 — minimum for stored/deflate)
 *    flags             2    0
 *    method            2    0 (stored)
 *    mod time/date     4    DOS format (we pass-through mtime field)
 *    crc32             4    CRC of uncompressed data
 *    comp size         4    == uncompressed for method 0
 *    uncomp size       4
 *    name len          2
 *    extra len         2    0
 *    (name)            n
 *    (extra)           0
 *
 *  CDFH (46 bytes + name + extra + comment):
 *    signature         4    0x02014b50
 *    version made by   2    0x0314 (Unix, v2.0)
 *    version needed    2    20
 *    flags             2
 *    method            2
 *    mod time/date     4
 *    crc32             4
 *    comp size         4
 *    uncomp size       4
 *    name len          2
 *    extra len         2
 *    comment len       2
 *    disk #            2    0
 *    int attrs         2    0
 *    ext attrs         4    (mode << 16) for Unix
 *    LFH offset        4
 *    (name)            n
 *
 *  EOCD (22 bytes + comment):
 *    signature         4    0x06054b50
 *    this disk         2    0
 *    disk w/ CD        2    0
 *    entries this disk 2
 *    total entries     2
 *    CD size           4
 *    CD offset         4
 *    comment len       2    0
 * -------------------------------------------------------------- */

/* CRC-32 (IEEE 802.3 polynomial, same as zip + gzip).
 * Compute on the fly to avoid a 1 KB precomputed table in .rodata. */
static uint32_t
crc32_update(uint32_t crc, const uint8_t *buf, uint32_t n)
{
    crc = ~crc;
    for (uint32_t i = 0; i < n; i++)
    {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

/* CRC of a STAGED entry's stage buffer. Non-staged entries have their
 * original CRC preserved in e->crc by the indexer — the caller in
 * emit_zip_archive must use that, not this helper, because for deflate
 * entries the bytes on disk are compressed and a CRC over them would
 * not match the ZIP spec's CRC-of-uncompressed-data requirement. */
static uint32_t
entry_crc32(entry_t *e)
{
    if (!e->staged || e->size == 0) return 0;
    return crc32_update(0, e->stage_buf, e->size);
}

static void
le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void
le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;       p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Per-entry scratch kept across the two emission passes (LFH + CDFH).
 * We record the LFH offset, CRC, sizes and path so the CDFH pass does
 * not have to re-stream each entry or re-parse anything.
 *
 * NOTE: comp_size and uncomp_size are tracked separately because a
 * preserved deflate entry has comp_size < uncomp_size. method is also
 * tracked so the CDFH pass matches the LFH pass byte-for-byte. */
typedef struct
{
    uint32_t lfh_offset;
    uint32_t comp_size;     /* what we actually wrote to disk */
    uint32_t uncomp_size;   /* original uncompressed size */
    uint32_t crc;
    uint32_t mtime;
    uint32_t mode;
    uint16_t method;        /* 0 = stored, 8 = deflate, ... */
    uint8_t  type;
    uint16_t name_len;
    const char *name;   /* points into entries[i].path after skipping '/' */
} zip_rec_t;

static bool
emit_zip_archive(int dst_fd)
{
    /* First pass: LFHs + data + record each entry's metadata for the
     * CD pass. We count only non-synthetic entries. */
    static zip_rec_t recs[MAX_ENTRIES];
    uint32_t rec_count = 0;
    uint32_t cursor    = 0;   /* bytes written to dst so far */

    for (uint32_t i = 0; i < entry_count; i++)
    {
        entry_t *e = &entries[i];
        if (e->synthetic) continue;
        if (rec_count >= MAX_ENTRIES) return false;

        const char *nm = e->path;
        if (nm[0] == '/') nm++;
        uint32_t nlen = (uint32_t)strlen(nm);
        if (nlen > 0xFFFF) return false;

        /* Directory entries in zip carry a trailing '/'. Allocate one
         * extra byte for that. */
        bool is_dir = (e->type == ARCHIVE_TYPE_DIR);
        uint32_t write_nlen = nlen + (is_dir ? 1 : 0);
        if (write_nlen > 0xFFFF) return false;

        /* Decide what (method, crc, comp_size, uncomp_size) to write:
         *   - directory           → stored, empty, zero CRC
         *   - staged (fresh data) → stored, fresh CRC from stage_buf
         *   - on-disk entry       → passthrough: preserve the original
         *                           method, comp_size, and CRC so that
         *                           deflate entries survive a rebuild
         *                           intact. Stored-on-disk entries work
         *                           through this branch too (method 0,
         *                           comp == uncomp). */
        uint16_t method;
        uint32_t crc, comp_sz, uncomp_sz;

        if (is_dir)
        {
            method = 0; crc = 0; comp_sz = 0; uncomp_sz = 0;
        } else if (e->staged)
        {
            method    = 0;
            crc       = entry_crc32(e);
            comp_sz   = e->size;
            uncomp_sz = e->size;
        } else
        {
            method    = e->compression;
            crc       = e->crc;
            comp_sz   = e->comp_size;
            uncomp_sz = e->size;
        }

        uint8_t lfh[30];
        le32(lfh + 0,  ZIP_LFH_SIG);
        le16(lfh + 4,  20);
        le16(lfh + 6,  0);
        le16(lfh + 8,  method);
        le32(lfh + 10, e->mtime);       /* dos time+date — pass-through */
        le32(lfh + 14, crc);
        le32(lfh + 18, comp_sz);
        le32(lfh + 22, uncomp_sz);
        le16(lfh + 26, (uint16_t)write_nlen);
        le16(lfh + 28, 0);

        uint32_t lfh_offset = cursor;
        if (!emit_write(dst_fd, lfh, 30)) return false;
        if (!emit_write(dst_fd, nm, nlen)) return false;
        if (is_dir)
        {
            uint8_t slash = '/';
            if (!emit_write(dst_fd, &slash, 1)) return false;
        }
        cursor += 30 + write_nlen;

        if (!is_dir && comp_sz > 0)
        {
            if (!emit_entry_data(e, dst_fd)) return false;
            cursor += comp_sz;
        }

        zip_rec_t *r = &recs[rec_count++];
        r->lfh_offset  = lfh_offset;
        r->comp_size   = comp_sz;
        r->uncomp_size = uncomp_sz;
        r->crc         = crc;
        r->mtime       = e->mtime;
        r->mode        = e->mode ? e->mode
                          : (is_dir ? 040755u : 0100644u);
        r->method      = method;
        r->type        = e->type;
        r->name_len    = (uint16_t)write_nlen;
        r->name        = nm;
    }

    /* Second pass: CDFH for each entry. */
    uint32_t cd_offset = cursor;

    for (uint32_t i = 0; i < rec_count; i++)
    {
        zip_rec_t *r = &recs[i];
        uint8_t cdfh[46];
        le32(cdfh + 0,  ZIP_CDFH_SIG);
        le16(cdfh + 4,  0x0314);        /* made by Unix, v2.0 */
        le16(cdfh + 6,  20);
        le16(cdfh + 8,  0);
        le16(cdfh + 10, r->method);
        le32(cdfh + 12, r->mtime);
        le32(cdfh + 16, r->crc);
        le32(cdfh + 20, r->comp_size);
        le32(cdfh + 24, r->uncomp_size);
        le16(cdfh + 28, r->name_len);
        le16(cdfh + 30, 0);             /* extra len */
        le16(cdfh + 32, 0);             /* comment len */
        le16(cdfh + 34, 0);             /* disk # */
        le16(cdfh + 36, 0);             /* int attrs */
        le32(cdfh + 38, r->mode << 16); /* ext attrs: high16 = Unix mode */
        le32(cdfh + 42, r->lfh_offset);

        if (!emit_write(dst_fd, cdfh, 46)) return false;
        /* Name: we may have appended a '/' for directories; recover
         * that here by checking name_len vs strlen. */
        uint32_t raw = (uint32_t)strlen(r->name);
        if (!emit_write(dst_fd, r->name, raw)) return false;
        if (r->name_len > raw)
        {
            uint8_t slash = '/';
            if (!emit_write(dst_fd, &slash, 1)) return false;
        }
        cursor += 46 + r->name_len;
    }
    uint32_t cd_size = cursor - cd_offset;

    /* EOCD */
    uint8_t eocd[22];
    le32(eocd + 0,  ZIP_EOCD_SIG);
    le16(eocd + 4,  0);
    le16(eocd + 6,  0);
    le16(eocd + 8,  (uint16_t)rec_count);
    le16(eocd + 10, (uint16_t)rec_count);
    le32(eocd + 12, cd_size);
    le32(eocd + 16, cd_offset);
    le16(eocd + 20, 0);

    return emit_write(dst_fd, eocd, 22);
}

/*  *  L5d  —  rebuild_and_flush()
 *
 *  The single commit point: write a new copy of the entire archive
 *  to a sibling temp file, close it, atomically rename it over the
 *  original, then re-open arc_fd on the fresh file and re-index so
 *  data_offset fields are valid again for any entry that's still
 *  around. Staging buffers are freed as part of the re-index
 *  (the new entries arrive through index_build with their on-disk
 *  offsets; staged==0 across the board).
 */

static bool
temp_path_from(const char *src, char *out, uint32_t cap)
{
    /* Produce "<path>.tmp" — a sibling file in the same directory.
     * dobfs rename operates within the same service, so siblinghood
     * keeps us on one filesystem by construction. */
    uint32_t sl = (uint32_t)strlen(src);
    if (sl + 4 + 1 > cap) return false;
    memcpy(out, src, sl);
    memcpy(out + sl, ".tmp", 4);
    out[sl + 4] = '\0';
    return true;
}

static bool
rebuild_and_flush(void)
{
    if (!arc_dirty) return true;

    char tmp_path[ARCHIVE_MAX_PATH + 8];
    if (!temp_path_from(archive_path_g, tmp_path, sizeof(tmp_path)))
        return false;

    /* Write the new archive to the sibling temp file. */
    int dst_fd = dobfs_Open(tmp_path, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (dst_fd < 0) return false;

    bool ok = (arc_format == ARCHIVE_FMT_ZIP)
            ? emit_zip_archive(dst_fd)
            : emit_tar_archive(dst_fd);
    dobfs_Close(dst_fd);

    if (!ok)
    {
        (void)dobfs_Unlink(tmp_path);
        return false;
    }

    /* Close the read fd on the original — dobfs rename may fail on
     * a file that still has open fds, and the upcoming unlink of the
     * backup definitely would. We'll re-open on the new archive at
     * the end. */
    if (arc_fd >= 0) { dobfs_Close(arc_fd); arc_fd = -1; }

    /* Three-step atomic swap, because dobfs_Rename refuses to
     * overwrite an existing target (handle_rename in DobFileSystem
     * returns DOB_ERR_DENIED when the new_path resolves).
     *
     *   step 1: archive       -> archive.bak   (preserve the old one)
     *   step 2: archive.tmp   -> archive       (install the new one)
     *   step 3: unlink archive.bak             (commit)
     *
     * If step 2 fails we put the backup back (step 1'). If step 3
     * fails the archive is already correct — the only cost is a
     * stale .bak file on disk, which is survivable and user-visible. */
    char bak_path[ARCHIVE_MAX_PATH + 8];
    {
        uint32_t sl = (uint32_t)strlen(archive_path_g);
        if (sl + 4 + 1 > sizeof(bak_path)) { (void)dobfs_Unlink(tmp_path); return false; }
        memcpy(bak_path, archive_path_g, sl);
        memcpy(bak_path + sl, ".bak", 4);
        bak_path[sl + 4] = '\0';
    }

    (void)dobfs_Unlink(bak_path);   /* clear any stale backup */

    if (dobfs_Rename(archive_path_g, bak_path) != 0)
    {
        (void)dobfs_Unlink(tmp_path);
        arc_fd = dobfs_Open(archive_path_g, FS_READ);
        return false;
    }

    if (dobfs_Rename(tmp_path, archive_path_g) != 0)
    {
        (void)dobfs_Rename(bak_path, archive_path_g);
        (void)dobfs_Unlink(tmp_path);
        arc_fd = dobfs_Open(archive_path_g, FS_READ);
        return false;
    }

    (void)dobfs_Unlink(bak_path);

    /* Re-open + re-stat + re-index against the fresh archive. */
    dobfs_stat_t st;
    if (dobfs_Stat(archive_path_g, &st) != 0) return false;
    arc_size = st.size;

    arc_fd = dobfs_Open(archive_path_g, FS_READ);
    if (arc_fd < 0) return false;

    /* Drop staging before re-index (index_build wipes entries
     * wholesale, but staging pointers aren't freed by memset). */
    for (uint32_t i = 0; i < entry_count; i++)
        entry_discard_staging(&entries[i]);

    if (!index_build()) return false;

    arc_dirty = false;
    return true;
}

static dob_status_t
op_fs_open(dob_msg_t *msg, dob_msg_t *reply)
{
    if (!msg->payload) return DOB_ERR_INVALID;
    const char *path  = (const char *)msg->payload;
    uint32_t    flags = msg->arg0;

    bool want_write = (flags & DOBFS_O_WRITE) != 0;
    bool want_trunc = (flags & DOBFS_O_TRUNC) != 0;

    /* v1 write semantics: we honour CREATE|WRITE|TRUNC (the shape
     * DobFiles emits on drag-drop). WRITE without TRUNC on an
     * existing entry is rejected for now — could be supported in a
     * future bump by copying existing bytes into staging. APPEND is
     * also rejected for the same reason. */
    if ((flags & DOBFS_O_APPEND) && want_write) return DOB_ERR_INVALID;

    char norm[ARCHIVE_MAX_PATH];
    path_normalise(path, norm, sizeof(norm));
    if (norm[0] == '/' && norm[1] == '\0') return DOB_ERR_INVALID;

    int idx = index_find_exact(norm);

    if (idx < 0)
    {
        /* Not found: only acceptable on a create path. */
        if (!(want_write && (flags & DOBFS_O_CREATE))) return DOB_ERR_NOT_FOUND;
        if (entry_count >= MAX_ENTRIES)                return DOB_ERR_NO_MEMORY;
        entry_t *e = &entries[entry_count++];
        memset(e, 0, sizeof(*e));
        strncpy(e->path, norm, sizeof(e->path) - 1);
        e->type        = ARCHIVE_TYPE_FILE;
        e->mode        = 0644;
        e->compression = 0;
        if (!stage_begin(e)) { entry_count--; return DOB_ERR_NO_MEMORY; }
        idx = (int)(entry_count - 1);
    } else
    {
        entry_t *e = &entries[idx];
        if (e->type == ARCHIVE_TYPE_DIR) return DOB_ERR_INVALID;
        if (want_write)
        {
            if (!want_trunc && !(flags & DOBFS_O_CREATE))
                return DOB_ERR_INVALID;   /* see comment above        */
            /* Synthetic parents can be dropped — we're now producing
             * a real entry. They'll be re-synthesised at re-index. */
            e->synthetic = 0;
            if (!stage_begin(e)) return DOB_ERR_NO_MEMORY;
        }
    }

    int fd = -1;
    for (int i = 0; i < MAX_FDS; i++)
        if (!fds[i].in_use) { fd = i; break; }
    if (fd < 0) return DOB_ERR_NO_MEMORY;

    fds[fd].in_use    = true;
    fds[fd].writable  = want_write;
    fds[fd].entry_idx = (uint32_t)idx;
    fds[fd].pos       = 0;

    reply->arg0 = (uint32_t)fd;
    return DOB_OK;
}

static dob_status_t
op_fs_read(dob_msg_t *msg, dob_msg_t *reply)
{
    int fd = (int)msg->arg0;
    if (fd < 0 || fd >= MAX_FDS || !fds[fd].in_use) return DOB_ERR_INVALID;

    entry_t *e = &entries[fds[fd].entry_idx];
    if (e->compression != 0) return DOB_ERR_INVALID;

    uint32_t want = msg->arg1;
    if (want > READ_REPLY_CAP) want = READ_REPLY_CAP;

    if (fds[fd].pos >= e->size) { reply->arg0 = 0; return DOB_OK; }
    uint32_t avail = e->size - fds[fd].pos;
    if (want > avail) want = avail;

    uint32_t got;
    if (e->staged)
    {
        /* Data lives in stage_buf — no seek/read needed, just copy. */
        memcpy(read_reply_buf, e->stage_buf + fds[fd].pos, want);
        got = want;
    } else
    {
        int rc = arcio_read_partial(e->data_offset + fds[fd].pos, read_reply_buf, want);
        if (rc < 0) return DOB_ERR_INTERNAL;
        got = (uint32_t)rc;
    }
    fds[fd].pos += got;

    reply->payload      = read_reply_buf;
    reply->payload_size = got;
    reply->arg0         = got;
    return DOB_OK;
}

static dob_status_t
op_fs_close(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    int fd = (int)msg->arg0;
    if (fd < 0 || fd >= MAX_FDS || !fds[fd].in_use) return DOB_ERR_INVALID;

    bool was_writable = fds[fd].writable;
    fds[fd].in_use   = false;
    fds[fd].writable = false;

    /* The staged bytes written through this fd are now the canonical
     * content of the entry — commit by rebuilding and flushing the
     * archive on disk. rebuild_and_flush will clear `staged` on each
     * entry via re-index, so after it returns all entry data_offsets
     * point at the fresh on-disk archive. */
    if (was_writable)
    {
        arc_dirty = true;
        if (!rebuild_and_flush()) return DOB_ERR_INTERNAL;
    }
    return DOB_OK;
}

static dob_status_t
op_fs_stat(dob_msg_t *msg, dob_msg_t *reply)
{
    if (!msg->payload) return DOB_ERR_INVALID;
    const char *path = (const char *)msg->payload;

    int idx = path_to_entry(path);
    if (idx == -2)
    {
        reply->arg0 = 0;
        reply->arg1 = 1;
        return DOB_OK;
    }
    if (idx < 0) return DOB_ERR_NOT_FOUND;

    reply->arg0 = entries[idx].size;
    reply->arg1 = (entries[idx].type == ARCHIVE_TYPE_DIR) ? 1 : 0;
    return DOB_OK;
}

static dob_status_t
op_fs_readdir(dob_msg_t *msg, dob_msg_t *reply)
{
    if (!msg->payload) return DOB_ERR_INVALID;

    char norm[ARCHIVE_MAX_PATH];
    path_normalise((const char *)msg->payload, norm, sizeof(norm));

    if (!(norm[0] == '/' && norm[1] == '\0'))
    {
        int idx = index_find_exact(norm);
        if (idx < 0 || entries[idx].type != ARCHIVE_TYPE_DIR)
            return DOB_ERR_NOT_FOUND;
    }

    uint32_t o = 0;
    for (uint32_t i = 0; i < entry_count; i++)
    {
        if (!path_is_immediate_child(norm, entries[i].path)) continue;

        const char *base = path_basename(entries[i].path);
        uint32_t blen    = (uint32_t)strlen(base);
        char tbuf[16];
        int tlen = snprintf(tbuf, sizeof(tbuf), "\t%c\t%u\n",
                            entries[i].type == ARCHIVE_TYPE_DIR ? 'D' : 'F',
                            entries[i].size);
        if (tlen < 0) continue;
        if (o + blen + (uint32_t)tlen + 1 >= sizeof(readdir_buf)) break;
        memcpy(readdir_buf + o, base, blen);   o += blen;
        memcpy(readdir_buf + o, tbuf, (uint32_t)tlen); o += (uint32_t)tlen;
    }
    readdir_buf[o] = '\0';
    reply->payload      = readdir_buf;
    reply->payload_size = o + 1;
    return DOB_OK;
}

/* SEEK on a virtual fd (inside an entry), not on the underlying
 * archive fd. The entry's byte stream is virtual — the real seek
 * to disk happens lazily inside op_fs_read via arcio_read_partial. */
static dob_status_t
op_fs_seek(dob_msg_t *msg, dob_msg_t *reply)
{
    int fd = (int)msg->arg0;
    if (fd < 0 || fd >= MAX_FDS || !fds[fd].in_use) return DOB_ERR_INVALID;

    uint32_t uoff   = msg->arg1;
    uint32_t whence = msg->arg2;
    int32_t  soff   = (int32_t)uoff;
    int64_t  np     = 0;

    entry_t *e = &entries[fds[fd].entry_idx];

    switch (whence)
    {
        case DOBFS_SEEK_SET: np = (int64_t)uoff;                        break;
        case DOBFS_SEEK_CUR: np = (int64_t)fds[fd].pos + (int64_t)soff; break;
        case DOBFS_SEEK_END: np = (int64_t)e->size     + (int64_t)soff; break;
        default:             return DOB_ERR_INVALID;
    }
    if (np < 0)                    return DOB_ERR_INVALID;
    if (np > (int64_t)e->size) np = (int64_t)e->size;

    fds[fd].pos = (uint32_t)np;
    reply->arg0 = fds[fd].pos;
    return DOB_OK;
}

static dob_status_t
op_fs_write(dob_msg_t *msg, dob_msg_t *reply)
{
    int fd = (int)msg->arg0;
    if (fd < 0 || fd >= MAX_FDS || !fds[fd].in_use) return DOB_ERR_INVALID;
    if (!fds[fd].writable)                          return DOB_ERR_DENIED;
    if (!msg->payload || msg->payload_size == 0) { reply->arg0 = 0; return DOB_OK; }

    entry_t *e = &entries[fds[fd].entry_idx];
    if (!e->staged) return DOB_ERR_INTERNAL;   /* open should have staged it */

    uint32_t n = msg->payload_size;
    if (!stage_write_at(e, fds[fd].pos, msg->payload, n))
        return DOB_ERR_NO_MEMORY;
    fds[fd].pos += n;

    reply->arg0 = n;
    return DOB_OK;
}

static dob_status_t
op_fs_mkdir(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    if (!msg->payload) return DOB_ERR_INVALID;

    char norm[ARCHIVE_MAX_PATH];
    path_normalise((const char *)msg->payload, norm, sizeof(norm));
    if (norm[0] == '/' && norm[1] == '\0') return DOB_ERR_INVALID;

    int idx = index_find_exact(norm);
    if (idx >= 0)
    {
        /* Already present. If synthetic, promote to real so it
         * survives the next rebuild; otherwise nothing to do. */
        if (entries[idx].synthetic) entries[idx].synthetic = 0;
        arc_dirty = true;
        return rebuild_and_flush() ? DOB_OK : DOB_ERR_INTERNAL;
    }

    if (entry_count >= MAX_ENTRIES) return DOB_ERR_NO_MEMORY;
    entry_t *e = &entries[entry_count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->path, norm, sizeof(e->path) - 1);
    e->type = ARCHIVE_TYPE_DIR;
    e->mode = 0755;

    arc_dirty = true;
    return rebuild_and_flush() ? DOB_OK : DOB_ERR_INTERNAL;
}

/* Count how many entries live inside `dir` (strict descendants, not
 * counting `dir` itself). Used to reject non-empty directory unlink. */
static uint32_t
count_descendants(const char *dir)
{
    uint32_t dlen = (uint32_t)strlen(dir);
    uint32_t n = 0;
    for (uint32_t i = 0; i < entry_count; i++)
    {
        const char *p = entries[i].path;
        uint32_t plen = (uint32_t)strlen(p);
        if (plen <= dlen) continue;
        bool root = (dlen == 1 && dir[0] == '/');
        if (!root)
        {
            if (memcmp(p, dir, dlen) != 0) continue;
            if (p[dlen] != '/') continue;
        } else
        {
            if (p[0] != '/') continue;
        }
        n++;
    }
    return n;
}

static dob_status_t
op_fs_unlink(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    if (!msg->payload) return DOB_ERR_INVALID;

    char norm[ARCHIVE_MAX_PATH];
    path_normalise((const char *)msg->payload, norm, sizeof(norm));
    if (norm[0] == '/' && norm[1] == '\0') return DOB_ERR_INVALID;

    int idx = index_find_exact(norm);
    if (idx < 0) return DOB_ERR_NOT_FOUND;

    entry_t *e = &entries[idx];
    if (e->type == ARCHIVE_TYPE_DIR)
    {
        if (count_descendants(norm) > 0) return DOB_ERR_INVALID;  /* non-empty */
    }

    /* Refuse if any open fd still references this entry. */
    for (int i = 0; i < MAX_FDS; i++)
        if (fds[i].in_use && fds[i].entry_idx == (uint32_t)idx)
            return DOB_ERR_DENIED;

    entry_remove_at((uint32_t)idx);

    /* Any open fd whose entry_idx was after the removed slot must be
     * shifted down by one — entry_remove_at compacts the array. */
    for (int i = 0; i < MAX_FDS; i++)
        if (fds[i].in_use && fds[i].entry_idx > (uint32_t)idx)
            fds[i].entry_idx--;

    arc_dirty = true;
    return rebuild_and_flush() ? DOB_OK : DOB_ERR_INTERNAL;
}

static dob_status_t
op_fs_rename(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)reply;
    /* Payload format: old_path\0new_path\0 */
    if (!msg->payload || msg->payload_size < 4) return DOB_ERR_INVALID;
    const char *op = (const char *)msg->payload;
    uint32_t avail = msg->payload_size;
    uint32_t olen = 0;
    while (olen < avail && op[olen] != '\0') olen++;
    if (olen == avail) return DOB_ERR_INVALID;
    const char *np = op + olen + 1;
    uint32_t nlen = 0;
    while (olen + 1 + nlen < avail && np[nlen] != '\0') nlen++;
    if (olen + 1 + nlen == avail) return DOB_ERR_INVALID;

    char o_norm[ARCHIVE_MAX_PATH], n_norm[ARCHIVE_MAX_PATH];
    path_normalise(op, o_norm, sizeof(o_norm));
    path_normalise(np, n_norm, sizeof(n_norm));
    if (o_norm[0] == '/' && o_norm[1] == '\0') return DOB_ERR_INVALID;
    if (n_norm[0] == '/' && n_norm[1] == '\0') return DOB_ERR_INVALID;

    int src = index_find_exact(o_norm);
    if (src < 0) return DOB_ERR_NOT_FOUND;
    if (index_find_exact(n_norm) >= 0) return DOB_ERR_INVALID;

    strncpy(entries[src].path, n_norm, sizeof(entries[src].path) - 1);
    entries[src].path[sizeof(entries[src].path) - 1] = '\0';

    arc_dirty = true;
    return rebuild_and_flush() ? DOB_OK : DOB_ERR_INTERNAL;
}

/* FORMAT remains denied — we would otherwise blow away the archive. */
static dob_status_t
op_fs_denied(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)msg; (void)reply;
    return DOB_ERR_DENIED;
}

/* L6  —  op_arch_*   : archive_protocol handlers */

static dob_status_t
op_arch_count(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)msg;
    uint32_t n = 0;
    for (uint32_t i = 0; i < entry_count; i++)
        if (!entries[i].synthetic) n++;
    reply->arg0 = n;
    reply->arg1 = arc_format;
    return DOB_OK;
}

static dob_status_t
op_arch_get_format(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)msg;
    reply->arg0 = arc_format;
    return DOB_OK;
}

static int
arch_resolve_native_index(uint32_t idx)
{
    uint32_t seen = 0;
    for (uint32_t i = 0; i < entry_count; i++)
    {
        if (entries[i].synthetic) continue;
        if (seen == idx) return (int)i;
        seen++;
    }
    return -1;
}

static dob_status_t
op_arch_entry_info(dob_msg_t *msg, dob_msg_t *reply)
{
    int internal = arch_resolve_native_index(msg->arg0);
    if (internal < 0) return DOB_ERR_NOT_FOUND;

    static archive_entry_info_t info;
    memset(&info, 0, sizeof(info));
    entry_t *e = &entries[internal];
    strncpy(info.path, e->path, sizeof(info.path) - 1);
    info.size            = e->size;
    info.compressed_size = e->comp_size;
    info.mode            = e->mode;
    info.mtime           = e->mtime;
    info.type            = e->type;
    info.compression     = e->compression;

    reply->payload      = &info;
    reply->payload_size = sizeof(info);
    return DOB_OK;
}

static dob_status_t
op_arch_read_entry(dob_msg_t *msg, dob_msg_t *reply)
{
    int internal = arch_resolve_native_index(msg->arg0);
    if (internal < 0) return DOB_ERR_NOT_FOUND;

    entry_t *e = &entries[internal];
    if (e->type != ARCHIVE_TYPE_FILE) return DOB_ERR_INVALID;
    if (e->compression != 0)          return DOB_ERR_INVALID;

    uint32_t offset = msg->arg1;
    uint32_t want   = msg->arg2;
    if (want > READ_REPLY_CAP) want = READ_REPLY_CAP;
    if (offset >= e->size) { reply->arg0 = 0; return DOB_OK; }
    uint32_t avail = e->size - offset;
    if (want > avail) want = avail;

    int got = arcio_read_partial(e->data_offset + offset, read_reply_buf, want);
    if (got < 0) return DOB_ERR_INTERNAL;

    reply->payload      = read_reply_buf;
    reply->payload_size = (uint32_t)got;
    reply->arg0         = (uint32_t)got;
    return DOB_OK;
}

static dob_status_t
op_arch_extract_to(dob_msg_t *msg, dob_msg_t *reply)
{
    if (!msg->payload || msg->payload_size == 0) return DOB_ERR_INVALID;
    const char *dest = (const char *)msg->payload;

    int internal = arch_resolve_native_index(msg->arg0);
    if (internal < 0) return DOB_ERR_NOT_FOUND;

    entry_t *e = &entries[internal];
    if (e->type != ARCHIVE_TYPE_FILE) return DOB_ERR_INVALID;
    if (e->compression != 0)          return DOB_ERR_INVALID;

    int dst_fd = dobfs_Open(dest, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (dst_fd < 0) return DOB_ERR_DENIED;

    /* Stream in chunks: the archive fd (via arcio_read_partial) and
     * the destination fd are both real fds, so this loop never holds
     * more than READ_REPLY_CAP bytes in RAM regardless of entry size. */
    static uint8_t xfer_buf[READ_REPLY_CAP];
    uint32_t written = 0;
    while (written < e->size)
    {
        uint32_t chunk = e->size - written;
        if (chunk > sizeof(xfer_buf)) chunk = sizeof(xfer_buf);

        int got = arcio_read_partial(e->data_offset + written, xfer_buf, chunk);
        if (got <= 0) break;

        int wn = dobfs_Write(dst_fd, xfer_buf, (uint32_t)got);
        if (wn <= 0) break;
        written += (uint32_t)wn;
        if ((uint32_t)wn < (uint32_t)got) break;
    }
    dobfs_Close(dst_fd);

    reply->arg0 = written;
    return (written == e->size) ? DOB_OK : DOB_ERR_INTERNAL;
}

static dob_status_t
op_arch_close(dob_msg_t *msg, dob_msg_t *reply)
{
    (void)msg; (void)reply;
    return DOB_OK;
}

/* L7  —  archive_*   : dispatch + bootstrap */

static bool exit_after_reply = false;

static dob_status_t
archive_dispatch(dob_msg_t *msg, dob_msg_t *reply)
{
    switch (msg->code)
    {
        case DOBFS_OPEN:              return op_fs_open    (msg, reply);
        case DOBFS_READ:              return op_fs_read    (msg, reply);
        case DOBFS_CLOSE:             return op_fs_close   (msg, reply);
        case DOBFS_STAT:              return op_fs_stat    (msg, reply);
        case DOBFS_READDIR:           return op_fs_readdir (msg, reply);
        case DOBFS_SEEK:              return op_fs_seek    (msg, reply);
        case DOBFS_WRITE:             return op_fs_write   (msg, reply);
        case DOBFS_MKDIR:             return op_fs_mkdir   (msg, reply);
        case DOBFS_UNLINK:            return op_fs_unlink  (msg, reply);
        case DOBFS_RENAME:            return op_fs_rename  (msg, reply);
        case DOBFS_FORMAT:            return op_fs_denied  (msg, reply);

        case DOBFS_SUBSCRIBE_UNMOUNT: return DOB_OK;

        case ARCHIVE_COUNT:           return op_arch_count      (msg, reply);
        case ARCHIVE_GET_FORMAT:      return op_arch_get_format (msg, reply);
        case ARCHIVE_ENTRY_INFO:      return op_arch_entry_info (msg, reply);
        case ARCHIVE_READ_ENTRY:      return op_arch_read_entry (msg, reply);
        case ARCHIVE_EXTRACT_TO:      return op_arch_extract_to (msg, reply);
        case ARCHIVE_CLOSE: {
            dob_status_t st = op_arch_close(msg, reply);
            exit_after_reply = true;
            return st;
        }

        default:                      return DOB_ERR_INVALID;
    }
}

static void
archive_serve(uint32_t port)
{
    dob_msg_t msg, reply;

    for (;;)
    {
        memset(&reply, 0, sizeof(reply));
        if (dob_ipc_receive(port, &msg) != DOB_OK) continue;
        if (msg.type == 3)                         continue;

        reply.code = (uint32_t)archive_dispatch(&msg, &reply);
        if (msg.type == 1) dob_ipc_reply(msg.sender_tid, &reply);

        if (exit_after_reply)
        {
            if (arc_fd >= 0) { dobfs_Close(arc_fd); arc_fd = -1; }
            _exit(0);
        }
    }
}

static bool
archive_pick_service_slot(char *out, uint32_t out_cap)
{
    for (uint32_t n = 0; n < MAX_ARCHIVE_SLOTS; n++)
    {
        int len = snprintf(out, out_cap, "archive%u", n);
        if (len <= 0 || (uint32_t)len >= out_cap) return false;
        if (dob_registry_find(out) == 0) return true;
    }
    return false;
}

static int
archive_run(const char *archive_path, bool headless)
{
    /* Publish the path to the global so rebuild_and_flush can find
     * it later without threading it through every call site. Without
     * this copy, archive_path_g stayed zero-initialised and the
     * rebuild pipeline silently wrote to "" / ".tmp" / ".bak" —
     * every operation failed, the on-disk archive never changed,
     * and dragged-in files vanished on reopen. */
    strncpy(archive_path_g, archive_path, sizeof(archive_path_g) - 1);
    archive_path_g[sizeof(archive_path_g) - 1] = '\0';

    const char *base = archive_path;
    for (const char *p = archive_path; *p; p++)
        if (*p == '/') base = p + 1;

    char popup_body[320];

    dobfs_stat_t st;
    if (dobfs_Stat(archive_path, &st) != 0)
    {
        snprintf(popup_body, sizeof(popup_body),
                 "'%s': il file non esiste o non e' accessibile.", base);
        dobpopup_Error("DobArchive", popup_body);
        return 1;
    }
    if (st.type != FS_TYPE_FILE)
    {
        snprintf(popup_body, sizeof(popup_body),
                 "'%s': il percorso non e' un file.", base);
        dobpopup_Error("DobArchive", popup_body);
        return 1;
    }
    if (st.size == 0)
    {
        uint8_t stamped = skeleton_stamp_empty(archive_path, base);
        if (stamped == ARCHIVE_FMT_UNKNOWN)
        {
            snprintf(popup_body, sizeof(popup_body),
                     "'%s': impossibile inizializzare l'archivio vuoto.", base);
            dobpopup_Error("DobArchive", popup_body);
            return 1;
        }
        if (dobfs_Stat(archive_path, &st) != 0 || st.size == 0)
        {
            dobpopup_Error("DobArchive", "Errore durante l'inizializzazione dell'archivio.");
            return 1;
        }
    }

    arc_fd = dobfs_Open(archive_path, FS_READ);
    if (arc_fd < 0)
    {
        snprintf(popup_body, sizeof(popup_body),
                 "'%s': errore di apertura dal disco.", base);
        dobpopup_Error("DobArchive", popup_body);
        return 1;
    }
    arc_size = st.size;

    if (!index_build())
    {
        debug_print("[DobArchive] FATAL: unknown or malformed archive\n");
        snprintf(popup_body, sizeof(popup_body),
                 "'%s' non e' un archivio TAR o ZIP valido.", base);
        dobpopup_Error("DobArchive", popup_body);
        dobfs_Close(arc_fd); arc_fd = -1;
        return 1;
    }

    if (!archive_pick_service_slot(service_name, sizeof(service_name)))
    {
        dobpopup_Error("DobArchive", "Troppi archivi aperti.");
        dobfs_Close(arc_fd); arc_fd = -1;
        return 1;
    }

    if (dob_server_init(service_name) != DOB_OK)
    {
        debug_print("[DobArchive] FATAL: server_init failed\n");
        dobfs_Close(arc_fd); arc_fd = -1;
        return 1;
    }

    /* Open a DobFiles mount on top of this archive by default — that
     * is what a user clicking a .zip/.tar expects. Callers that pass
     * "--silent" on argv (e.g. DobInstaller reading a .dbp) get no
     * file-explorer window. */
    if (!headless)
        (void)dobfiles_OpenMount(service_name, "/", 0);

    archive_serve(dob_server_get_port());
    return 0;
}

int
main(int argc, char **argv)
{
    /* Parse argv: recognise "--silent" as a flag anywhere, take the
     * first non-flag argument as the archive path. Order-independent
     * to keep callers simple. */
    bool headless = false;
    const char *archive_path = NULL;
    for (int i = 0; i < argc; i++)
    {
        if (!argv[i] || !argv[i][0]) continue;
        if (strcmp(argv[i], "--silent") == 0)
        {
            headless = true;
            continue;
        }
        if (!archive_path) archive_path = argv[i];
    }

    if (!archive_path)
    {
        debug_print("[DobArchive] FATAL: no archive path in argv\n");
        return 1;
    }
    return archive_run(archive_path, headless);
}
