/* exfat.c — exFAT filesystem parser, PIC ELF shared object (.mem)
 *
 * Loaded by DobFileSystem via dob_mem_load(); see exfat_api.h for the
 * contract and docs/mem.md for the .mem mechanism. FULL READ-WRITE:
 * mount / stat / open / read / readdir, the write set (write / ftrunc /
 * create / mkdir / unlink / rename / flush) AND mkfs are all implemented
 * and exported in __mem_exports. (Live-CD mode is held read-only one
 * level up, by DobFileSystem's disk_write gate, not here.)
 *
 * Self-contained by contract: the ONLY external references are the rd/wr
 * sector callbacks handed to mount() (resolved at call time, not at link
 * time). elf_load_shared accepts only R_386_RELATIVE, so any other
 * unresolved symbol fails the load. There is no inline assembly and no
 * `int` instruction anywhere here: all I/O is the rd/wr callbacks, all
 * arithmetic is plain C. (In particular there is no `int 0x85` — the
 * 0x85 below is the exFAT File-entry TypeCode, a data byte, never an
 * interrupt; it lives in a different namespace from the kernel's video
 * boomerang vector.)
 *
 * 64-bit note: this target links no libgcc, so a 64-bit shift/divide/
 * multiply by a runtime value would pull __lshrdi3 / __udivdi3 / __muldi3
 * and fail the link. Every 64-bit operation here is therefore a constant
 * shift, an add/sub/compare (which GCC lowers inline), or routed through
 * shl64()/shr64() below. There is no `/`, `%` or `*` on a uint64_t.
 */

#include "exfat_api.h"

/* ===== libc-free primitives =====
 * Named memcpy/memset (not exfat_*) because GCC may emit implicit calls
 * to them while lowering struct copies/initialisation, even with
 * -fno-builtin. */

void *memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--)
    {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int v, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--)
    {
        *d++ = (uint8_t)v;
    }
    return dst;
}

/* ===== 64-bit shift helpers (libgcc-free) =====
 * Variable-count 64-bit shifts would otherwise emit __lshrdi3/__ashldi3.
 * These do it with 32-bit ops and constant 32-shifts only, which GCC
 * always lowers inline. Valid for s in 0..63. */

static uint64_t shl64(uint64_t x, unsigned s)
{
    uint32_t lo = (uint32_t)x;
    uint32_t hi = (uint32_t)(x >> 32);
    if (s == 0)
    {
        return x;
    }
    if (s >= 32)
    {
        hi = lo << (s - 32);
        lo = 0;
    }
    else
    {
        hi = (hi << s) | (lo >> (32 - s));
        lo = lo << s;
    }
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static uint64_t shr64(uint64_t x, unsigned s)
{
    uint32_t lo = (uint32_t)x;
    uint32_t hi = (uint32_t)(x >> 32);
    if (s == 0)
    {
        return x;
    }
    if (s >= 32)
    {
        lo = hi >> (s - 32);
        hi = 0;
    }
    else
    {
        lo = (lo >> s) | (hi << (32 - s));
        hi = hi >> s;
    }
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* ===== little-endian field readers ===== */

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* ===== exFAT on-disk constants ===== */

#define EXFAT_ENTRY_EOD         0x00   /* end of directory                */
#define EXFAT_ENTRY_BITMAP      0x81   /* Allocation Bitmap               */
#define EXFAT_ENTRY_UPCASE      0x82   /* Up-case Table                   */
#define EXFAT_ENTRY_LABEL       0x83   /* Volume Label                    */
#define EXFAT_ENTRY_FILE        0x85   /* File Directory Entry (in-use)   */
#define EXFAT_ENTRY_STREAM      0xC0   /* Stream Extension                */
#define EXFAT_ENTRY_FILENAME    0xC1   /* File Name                       */

#define ATTR_DIRECTORY          0x0010 /* FileAttributes bit4             */
#define FLAG_NOFATCHAIN         0x02   /* GeneralSecondaryFlags bit1      */

#define FAT_BAD                 0xFFFFFFF7u   /* >= this = bad / EOC range */

#define ENTRY_BYTES             32u
#define DIR_ENTRIES_PER_SEC     16u    /* 512 / 32                        */
#define MAX_SECONDARY           18u    /* 1 stream + up to 17 file-name   */

/* ===== Volume handle (single mount per .mem instance) ===== */

struct exfat_volume
{
    exfat_rdsec_t rd;
    exfat_wrsec_t wr;
    void         *ctx;

    /* geometry */
    uint8_t  bps_shift;       /* BytesPerSectorShift   (9..12)            */
    uint8_t  spc_shift;       /* SectorsPerClusterShift                   */
    uint8_t  csz_shift;       /* cluster size, bytes   (bps + spc)        */
    uint8_t  s512_shift;      /* logical-sector -> 512-sector (bps - 9)   */
    uint32_t fat_offset;      /* FatOffset, logical sectors               */
    uint32_t fat_length;      /* FatLength, logical sectors               */
    uint32_t cluster_heap;    /* ClusterHeapOffset, logical sectors       */
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint8_t  num_fats;

    /* located at mount (the bitmap is for Phase 2; locating both also
     * confirms the root directory reads) */
    uint32_t bitmap_cluster;
    uint64_t bitmap_bytes;
    uint32_t upcase_cluster;
    uint64_t upcase_bytes;

    /* allocator hint: lowest cluster worth trying on the next bitmap scan */
    uint32_t next_free;

    /* single-sector FAT cache */
    uint8_t  fat_cache[512];
    uint32_t fat_cache_lba;
    bool     fat_cache_valid;

    bool     mounted;
};

static struct exfat_volume g_vol;

/* ===== geometry helpers ===== */

/* 512-byte LBA of the first sector of `cluster` (>= 2). Returns false if
 * it would exceed the 32-bit block-layer LBA (volume past the ~2 TB the
 * 512-byte block layer can address — not an exFAT limit). */
static bool cluster_lba512(exfat_volume_t *v, uint32_t cluster, uint32_t *out)
{
    uint64_t exlog = (uint64_t)v->cluster_heap +
                     shl64((uint64_t)(cluster - 2u), v->spc_shift);
    uint64_t lba   = shl64(exlog, v->s512_shift);
    if (lba > 0xFFFFFFFFull)
    {
        return false;
    }
    *out = (uint32_t)lba;
    return true;
}

/* Read the FAT entry for `cluster`. Returns false on I/O error. */
static bool fat_next(exfat_volume_t *v, uint32_t cluster, uint32_t *next)
{
    uint64_t fat_byte = shl64((uint64_t)v->fat_offset, v->bps_shift) +
                        ((uint64_t)cluster << 2);     /* N * 4, constant shift */
    uint64_t lba64    = fat_byte >> 9;                /* constant shift        */
    if (lba64 > 0xFFFFFFFFull)
    {
        return false;
    }
    uint32_t lba = (uint32_t)lba64;
    uint32_t off = (uint32_t)fat_byte & 511u;         /* entries never straddle 512 */

    if (!v->fat_cache_valid || v->fat_cache_lba != lba)
    {
        if (v->rd(v->ctx, lba, 1, v->fat_cache) != 0)
        {
            v->fat_cache_valid = false;
            return false;
        }
        v->fat_cache_lba   = lba;
        v->fat_cache_valid = true;
    }
    *next = rd32(v->fat_cache + off);
    return true;
}

/* ===== name helpers ===== */

/* UTF-16LE -> UTF-8 (BMP). Surrogate halves become '?' for Phase 1
 * (astral filenames are rare; full surrogate handling is a refinement). */
static void utf16_to_utf8(const uint16_t *src, uint32_t n, char *dst, uint32_t cap)
{
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t cp = src[i];
        if (cp >= 0xD800u && cp <= 0xDFFFu)
        {
            cp = '?';
        }
        if (cp < 0x80u)
        {
            if (o + 1u >= cap) break;
            dst[o++] = (char)cp;
        }
        else if (cp < 0x800u)
        {
            if (o + 2u >= cap) break;
            dst[o++] = (char)(0xC0u | (cp >> 6));
            dst[o++] = (char)(0x80u | (cp & 0x3Fu));
        }
        else
        {
            if (o + 3u >= cap) break;
            dst[o++] = (char)(0xE0u | (cp >> 12));
            dst[o++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            dst[o++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    dst[o] = 0;
}

/* ASCII case-insensitive name compare. Correct for ASCII names; full
 * Unicode case-folding needs the volume up-case table (Phase 1 follow-up;
 * the table location is captured at mount). */
static bool name_eq_ci(const char *a, const char *b)
{
    for (;;)
    {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return false;
        if (ca == 0)  return true;
    }
}

/* ===== entry-set checksum (exFAT 7.4.1) ===== */

static uint16_t entry_set_checksum(const uint8_t *entries, uint16_t count)
{
    uint16_t sum   = 0;
    uint32_t bytes = (uint32_t)count * ENTRY_BYTES;
    for (uint32_t i = 0; i < bytes; i++)
    {
        if (i == 2 || i == 3) continue;   /* skip the SetChecksum field */
        sum = (uint16_t)(((sum & 1u) ? 0x8000u : 0u) + (sum >> 1) + entries[i]);
    }
    return sum;
}

/* ===== directory byte-stream cursor =====
 * Yields successive 32-byte entries from a directory, walking sectors and
 * clusters transparently. `sized` directories (a subdir with a known
 * DataLength) are bounded by bytes_left; the root and FAT-chained dirs
 * follow the FAT to EOC. Iteration also stops at a 0x00 end-of-directory
 * marker (checked by the consumer). */

typedef struct
{
    exfat_volume_t *v;
    uint32_t  cluster;        /* current cluster (>= 2); 0 = ended */
    bool      contiguous;     /* NoFatChain: next cluster is +1     */
    bool      sized;          /* bounded by bytes_left              */
    uint64_t  bytes_left;
    uint32_t  sec_in_clus;    /* 512-sector index within cluster    */
    uint32_t  ent_in_sec;     /* 0..15                              */
    uint32_t  entry_index;    /* count of entries yielded so far     */
    uint32_t  hops;           /* cluster advances taken; bounds the chain
                               * against cyclic/unterminated FAT links     */
    uint8_t   sec[512];
    bool      sec_loaded;
} dir_cur_t;

static void dir_cur_init(dir_cur_t *c, exfat_volume_t *v, uint32_t cluster,
                         bool contiguous, bool sized, uint64_t size)
{
    c->v           = v;
    c->cluster     = cluster;
    c->contiguous  = contiguous;
    c->sized       = sized;
    c->bytes_left  = size;
    c->sec_in_clus = 0;
    c->ent_in_sec  = 0;
    c->entry_index = 0;
    c->hops        = 0;
    c->sec_loaded  = false;
}

/* Next 32-byte entry, or NULL at end of directory. *err set to -2 on I/O
 * error. The returned pointer is valid only until the next call. */
static const uint8_t *dir_cur_next(dir_cur_t *c, int *err)
{
    *err = 0;
    for (;;)
    {
        if (c->cluster < 2)
        {
            return NULL;
        }
        if (c->sized && c->bytes_left == 0)
        {
            return NULL;
        }
        if (!c->sec_loaded)
        {
            uint32_t base;
            if (!cluster_lba512(c->v, c->cluster, &base))
            {
                *err = -2;
                return NULL;
            }
            if (c->v->rd(c->v->ctx, base + c->sec_in_clus, 1, c->sec) != 0)
            {
                *err = -2;
                return NULL;
            }
            c->sec_loaded = true;
        }
        if (c->ent_in_sec < DIR_ENTRIES_PER_SEC)
        {
            const uint8_t *e = c->sec + (c->ent_in_sec * ENTRY_BYTES);
            c->ent_in_sec++;
            c->entry_index++;
            if (c->sized)
            {
                c->bytes_left = (c->bytes_left >= ENTRY_BYTES)
                                ? (c->bytes_left - ENTRY_BYTES) : 0;
            }
            return e;
        }

        /* sector exhausted: advance, crossing the cluster boundary if needed */
        c->ent_in_sec = 0;
        c->sec_loaded = false;
        c->sec_in_clus++;
        uint32_t spc512 = 1u << (c->v->csz_shift - 9);
        if (c->sec_in_clus >= spc512)
        {
            c->sec_in_clus = 0;

            /* Bound the chain: a directory cannot legitimately span more
             * clusters than the volume contains. Exceeding that count means
             * a cyclic or unterminated chain (corruption, or a non-exFAT
             * volume mis-detected as exFAT) — stop with an error instead of
             * looping forever, which would hang the mount and freeze any
             * caller blocked on its reply. */
            if (++c->hops > c->v->cluster_count)
            {
                *err = -3;
                return NULL;
            }

            if (c->contiguous)
            {
                c->cluster++;
            }
            else
            {
                uint32_t nx;
                if (!fat_next(c->v, c->cluster, &nx))
                {
                    *err = -2;
                    return NULL;
                }
                if (nx < 2 || nx >= FAT_BAD)
                {
                    c->cluster = 0;
                    return NULL;
                }
                c->cluster = nx;
            }
        }
    }
}

/* ===== file entry-set parsing ===== */

typedef struct
{
    char     name[EXFAT_NAME_MAX + 1];   /* UTF-8 */
    uint32_t first_cluster;
    uint64_t size;
    uint64_t valid;
    bool     is_dir;
    bool     contiguous;
    uint32_t dir_cluster;   /* first cluster of the directory holding the set */
    uint32_t set_index;     /* entry index of the File entry within that dir   */
    uint8_t  set_count;     /* total entries in the set (1 + SecondaryCount)   */
} file_info_t;

/* Pull the next File entry set from a directory cursor. Returns 1 on a
 * parsed set, 0 at end of directory, -2 on I/O error. Non-file entries
 * (bitmap / up-case / label / unused) are skipped. */
static int dir_next_file(dir_cur_t *c, file_info_t *out)
{
    int err;
    for (;;)
    {
        const uint8_t *e = dir_cur_next(c, &err);
        if (err) return -2;
        if (!e)  return 0;

        uint8_t type = e[0];
        if (type == EXFAT_ENTRY_EOD)      return 0;   /* end of directory */
        if ((type & 0x80u) == 0)          continue;   /* unused slot       */
        if (type != EXFAT_ENTRY_FILE)     continue;   /* not a file set    */

        uint8_t scount = e[1];                        /* SecondaryCount    */
        if (scount < 2 || scount > MAX_SECONDARY)     continue;

        uint32_t file_idx = c->entry_index - 1;       /* index of this File entry */

        uint8_t set[(1u + MAX_SECONDARY) * ENTRY_BYTES];
        memcpy(set, e, ENTRY_BYTES);

        bool ioerr = false;
        for (uint8_t i = 0; i < scount; i++)
        {
            const uint8_t *se = dir_cur_next(c, &err);
            if (err || !se)
            {
                ioerr = (err != 0);
                /* a truncated set with no I/O error is just corrupt: drop it */
                break;
            }
            memcpy(set + (1u + i) * ENTRY_BYTES, se, ENTRY_BYTES);
        }
        if (ioerr)        return -2;
        if (set[0] != EXFAT_ENTRY_FILE) continue;     /* defensive */

        uint16_t want_csum = (uint16_t)rd16(set + 2);
        if (entry_set_checksum(set, (uint16_t)(1u + scount)) != want_csum)
        {
            continue;                                 /* corrupt set: skip */
        }

        const uint8_t *stream = set + ENTRY_BYTES;
        if (stream[0] != EXFAT_ENTRY_STREAM)          continue;

        uint16_t attr     = (uint16_t)rd16(set + 4);
        uint8_t  secflags = stream[1];
        uint8_t  namelen  = stream[3];
        uint32_t firstcl  = rd32(stream + 20);
        uint64_t datalen  = rd64(stream + 24);
        uint64_t validlen = rd64(stream + 8);

        /* reconstruct the name from the File Name (0xC1) entries, which are
         * set-indices 2..scount (index 0 = File entry, index 1 = Stream) */
        uint16_t u16[EXFAT_NAME_MAX + 1];
        uint32_t got = 0;
        for (uint8_t idx = 2; idx <= scount && got < namelen; idx++)
        {
            const uint8_t *ne = set + (uint32_t)idx * ENTRY_BYTES;
            if (ne[0] != EXFAT_ENTRY_FILENAME) continue;
            for (int k = 0; k < 15 && got < namelen; k++)
            {
                u16[got++] = (uint16_t)rd16(ne + 2 + k * 2);
            }
        }
        utf16_to_utf8(u16, got, out->name, sizeof(out->name));

        out->first_cluster = firstcl;
        out->size          = datalen;
        out->valid         = validlen;
        out->is_dir        = (attr & ATTR_DIRECTORY) != 0;
        out->contiguous    = (secflags & FLAG_NOFATCHAIN) != 0;
        out->set_index     = file_idx;
        out->set_count     = (uint8_t)(1u + scount);
        out->dir_cluster   = 0;   /* resolve() fills the directory's cluster */
        return 1;
    }
}

/* ===== path resolution ===== */

/* Resolve `path` to a file_info_t. Returns 1 found, 0 not found, -2 I/O
 * error. "/" or "" resolves to the root directory. */
static int resolve(exfat_volume_t *v, const char *path, file_info_t *out)
{
    uint32_t cur_cluster = v->root_cluster;
    bool     cur_contig  = false;   /* root: FAT-chained */
    uint64_t cur_size    = 0;       /* root: follow FAT, unsized */
    bool     cur_is_dir  = true;

    const char *p = path;
    while (*p == '/') p++;

    if (*p == 0)
    {
        memset(out, 0, sizeof(*out));
        out->first_cluster = v->root_cluster;
        out->is_dir        = true;
        return 1;
    }

    for (;;)
    {
        char comp[EXFAT_NAME_MAX + 1];
        uint32_t cl = 0;
        while (p[cl] && p[cl] != '/' && cl < EXFAT_NAME_MAX)
        {
            comp[cl] = p[cl];
            cl++;
        }
        comp[cl] = 0;
        p += cl;
        while (*p == '/') p++;
        bool last = (*p == 0);

        if (!cur_is_dir) return 0;

        dir_cur_t c;
        dir_cur_init(&c, v, cur_cluster, cur_contig,
                     cur_size != 0, cur_size);

        file_info_t info;
        bool found = false;
        for (;;)
        {
            int r = dir_next_file(&c, &info);
            if (r < 0) return -2;
            if (r == 0) break;
            if (name_eq_ci(comp, info.name))
            {
                found = true;
                break;
            }
        }
        if (!found) return 0;

        if (last)
        {
            *out = info;
            out->dir_cluster = cur_cluster;   /* directory holding this entry set */
            return 1;
        }
        cur_cluster = info.first_cluster;
        cur_contig  = info.contiguous;
        cur_size    = info.size;
        cur_is_dir  = info.is_dir;
    }
}

/* ===== public API: lifecycle ===== */

static exfat_volume_t *exfat_mount(exfat_rdsec_t rd, exfat_wrsec_t wr, void *ctx)
{
    if (g_vol.mounted)
    {
        return NULL;                       /* one volume per .mem instance */
    }

    memset(&g_vol, 0, sizeof(g_vol));
    g_vol.rd  = rd;
    g_vol.wr  = wr;
    g_vol.ctx = ctx;

    uint8_t boot[512];
    if (rd(ctx, 0, 1, boot) != 0)
    {
        return NULL;
    }

    static const char SIG[8] = { 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' ' };
    for (int i = 0; i < 8; i++)
    {
        if (boot[3 + i] != (uint8_t)SIG[i]) return NULL;
    }
    if (boot[0] != 0xEB)                    return NULL;
    if (boot[510] != 0x55 || boot[511] != 0xAA) return NULL;

    g_vol.fat_offset    = rd32(boot + 80);
    g_vol.fat_length    = rd32(boot + 84);
    g_vol.cluster_heap  = rd32(boot + 88);
    g_vol.cluster_count = rd32(boot + 92);
    g_vol.root_cluster  = rd32(boot + 96);
    g_vol.bps_shift     = boot[108];
    g_vol.spc_shift     = boot[109];
    g_vol.num_fats      = boot[110];

    if (g_vol.bps_shift < 9 || g_vol.bps_shift > 12)             return NULL;
    if ((uint32_t)g_vol.bps_shift + g_vol.spc_shift > 25)        return NULL;
    if (g_vol.root_cluster < 2)                                  return NULL;

    g_vol.csz_shift  = (uint8_t)(g_vol.bps_shift + g_vol.spc_shift);
    g_vol.s512_shift = (uint8_t)(g_vol.bps_shift - 9);

    /* scan the root for the Allocation Bitmap and Up-case Table */
    {
        dir_cur_t c;
        dir_cur_init(&c, &g_vol, g_vol.root_cluster, false, false, 0);
        for (;;)
        {
            int err;
            const uint8_t *e = dir_cur_next(&c, &err);
            if (err) return NULL;
            if (!e)  break;

            uint8_t type = e[0];
            if (type == EXFAT_ENTRY_EOD) break;
            if (type == EXFAT_ENTRY_BITMAP)
            {
                if ((e[1] & 1u) == 0)      /* first bitmap (FAT 0) */
                {
                    g_vol.bitmap_cluster = rd32(e + 20);
                    g_vol.bitmap_bytes   = rd64(e + 24);
                }
            }
            else if (type == EXFAT_ENTRY_UPCASE)
            {
                g_vol.upcase_cluster = rd32(e + 20);
                g_vol.upcase_bytes   = rd64(e + 24);
            }
        }
    }

    g_vol.mounted = true;
    return &g_vol;
}

static void exfat_unmount(exfat_volume_t *v)
{
    (void)v;
    g_vol.mounted = false;
}

/* ===== public API: read path ===== */

static int exfat_stat(exfat_volume_t *v, const char *path, exfat_stat_t *out)
{
    file_info_t info;
    int r = resolve(v, path, &info);
    if (r < 0) return -2;
    if (r == 0) return -1;
    out->size   = info.size;
    out->is_dir = info.is_dir;
    return 0;
}

static int exfat_open(exfat_volume_t *v, const char *path, exfat_file_t *out)
{
    file_info_t info;
    int r = resolve(v, path, &info);
    if (r < 0) return -2;
    if (r == 0) return -1;

    out->first_cluster = info.first_cluster;
    out->size          = info.size;
    out->valid         = info.valid;
    out->pos           = 0;
    out->dir_cluster   = info.dir_cluster;              /* entry-set location: directory */
    out->set_offset    = info.set_index * ENTRY_BYTES;  /* ...and byte offset of the set  */
    out->contiguous    = info.contiguous;
    out->is_dir        = info.is_dir;
    out->dirty         = false;
    return 0;
}

static int exfat_read(exfat_volume_t *v, exfat_file_t *f, void *buf, uint32_t n)
{
    if (f->is_dir)        return -1;
    if (f->pos >= f->size) return 0;        /* EOF */

    uint64_t avail = f->size - f->pos;
    uint32_t want  = (n < avail) ? n : (uint32_t)avail;
    uint8_t *dst   = (uint8_t *)buf;
    uint32_t done  = 0;

    uint32_t cluster_bytes = 1u << v->csz_shift;
    uint32_t spc512        = 1u << (v->csz_shift - 9);

    while (done < want)
    {
        uint64_t pos    = f->pos + done;
        uint32_t cl_idx = (uint32_t)shr64(pos, v->csz_shift);
        uint32_t in_cl  = (uint32_t)pos & (cluster_bytes - 1u);

        /* locate the cluster holding cl_idx */
        uint32_t cluster = f->first_cluster;
        if (f->contiguous)
        {
            cluster += cl_idx;
        }
        else
        {
            for (uint32_t i = 0; i < cl_idx; i++)
            {
                uint32_t nx;
                if (!fat_next(v, cluster, &nx) || nx < 2 || nx >= FAT_BAD)
                {
                    return done ? (int)done : -2;
                }
                cluster = nx;
            }
        }

        uint32_t base;
        if (!cluster_lba512(v, cluster, &base))
        {
            return done ? (int)done : -2;
        }

        uint32_t sec = in_cl >> 9;
        uint32_t off = in_cl & 511u;
        uint8_t  tmp[512];

        while (sec < spc512 && done < want)
        {
            if (v->rd(v->ctx, base + sec, 1, tmp) != 0)
            {
                return done ? (int)done : -2;
            }
            uint32_t chunk = 512u - off;
            if (chunk > want - done) chunk = want - done;

            uint64_t abspos = f->pos + done;
            if (abspos >= f->valid)
            {
                memset(dst + done, 0, chunk);          /* past ValidDataLength */
            }
            else if (abspos + chunk > f->valid)
            {
                uint32_t real = (uint32_t)(f->valid - abspos);
                memcpy(dst + done, tmp + off, real);
                memset(dst + done + real, 0, chunk - real);
            }
            else
            {
                memcpy(dst + done, tmp + off, chunk);
            }
            done += chunk;
            off   = 0;
            sec++;
        }
    }

    f->pos += done;
    return (int)done;
}

static int exfat_readdir(exfat_volume_t *v, const char *path,
                         exfat_dirent_t *out, int max)
{
    file_info_t dinfo;
    int r = resolve(v, path, &dinfo);
    if (r < 0) return -2;
    if (r == 0) return -1;
    if (!dinfo.is_dir) return -1;

    bool     is_root = (dinfo.first_cluster == v->root_cluster);
    dir_cur_t c;
    dir_cur_init(&c, v, dinfo.first_cluster,
                 is_root ? false : dinfo.contiguous,
                 is_root ? false : (dinfo.size != 0),
                 is_root ? 0 : dinfo.size);

    int count = 0;
    while (count < max)
    {
        file_info_t info;
        int rr = dir_next_file(&c, &info);
        if (rr < 0) return count ? count : -2;
        if (rr == 0) break;

        uint32_t i = 0;
        while (info.name[i] && i < EXFAT_NAME_MAX)
        {
            out[count].name[i] = info.name[i];
            i++;
        }
        out[count].name[i] = 0;
        out[count].size    = info.size;
        out[count].is_dir  = info.is_dir;
        count++;
    }
    return count;
}

/* ===== write helpers (Phase 4a) ===== */

#define FAT_EOC   0xFFFFFFFFu   /* exFAT end-of-chain marker */

/* Persist FAT[cluster] = value. Keeps the read cache coherent. Writes the
 * first FAT only; volumes with num_fats > 1 (TexFAT) are not maintained.
 * Returns false on I/O error. */
static bool fat_set(exfat_volume_t *v, uint32_t cluster, uint32_t value)
{
    uint64_t fat_byte = shl64((uint64_t)v->fat_offset, v->bps_shift) +
                        ((uint64_t)cluster << 2);     /* * 4, constant shift */
    uint64_t lba64 = fat_byte >> 9;                   /* constant shift      */
    if (lba64 > 0xFFFFFFFFull) return false;
    uint32_t lba = (uint32_t)lba64;
    uint32_t off = (uint32_t)fat_byte & 511u;

    uint8_t sec[512];
    if (v->rd(v->ctx, lba, 1, sec) != 0) return false;
    sec[off + 0] = (uint8_t)(value);
    sec[off + 1] = (uint8_t)(value >> 8);
    sec[off + 2] = (uint8_t)(value >> 16);
    sec[off + 3] = (uint8_t)(value >> 24);
    if (v->wr(v->ctx, lba, 1, sec) != 0) return false;

    if (v->fat_cache_valid && v->fat_cache_lba == lba)
        memcpy(v->fat_cache + off, sec + off, 4);
    return true;
}

/* 512-LBA + byte/bit of `cluster`'s bit in the Allocation Bitmap. The
 * bitmap is treated as contiguous from bitmap_cluster (the mkfs layout and
 * the common on-disk case). */
static bool bitmap_locate(exfat_volume_t *v, uint32_t cluster,
                          uint32_t *lba, uint32_t *byte_off, uint8_t *bit)
{
    uint32_t idx       = cluster - 2u;          /* bit index           */
    uint32_t byteindex = idx >> 3;              /* byte within bitmap  */
    *bit = (uint8_t)(idx & 7u);
    uint32_t base;
    if (!cluster_lba512(v, v->bitmap_cluster, &base)) return false;
    *lba      = base + (byteindex >> 9);
    *byte_off = byteindex & 511u;
    return true;
}

static bool bitmap_set_bit(exfat_volume_t *v, uint32_t cluster, bool used)
{
    uint32_t lba, bo; uint8_t bit;
    if (!bitmap_locate(v, cluster, &lba, &bo, &bit)) return false;
    uint8_t sec[512];
    if (v->rd(v->ctx, lba, 1, sec) != 0) return false;
    if (used) sec[bo] |=  (uint8_t)(1u << bit);
    else      sec[bo] &= (uint8_t)~(1u << bit);
    return v->wr(v->ctx, lba, 1, sec) == 0;
}

/* Allocate one free cluster: scan the bitmap for a 0 bit, set it, return
 * the cluster (>= 2). Returns 0 if the volume is full or on I/O error. */
static uint32_t alloc_cluster(exfat_volume_t *v)
{
    uint32_t total = v->cluster_count;
    if (total == 0) return 0;
    uint32_t start = (v->next_free >= 2) ? v->next_free : 2u;

    uint8_t  sec[512];
    int32_t  cached = -1;
    for (uint32_t k = 0; k < total; k++)
    {
        uint32_t cl = 2u + (((start - 2u) + k) % total);   /* 32-bit modulo */
        uint32_t lba, bo; uint8_t bit;
        if (!bitmap_locate(v, cl, &lba, &bo, &bit)) return 0;
        if ((int32_t)lba != cached)
        {
            if (v->rd(v->ctx, lba, 1, sec) != 0) return 0;
            cached = (int32_t)lba;
        }
        if ((sec[bo] & (uint8_t)(1u << bit)) == 0)
        {
            sec[bo] |= (uint8_t)(1u << bit);
            if (v->wr(v->ctx, lba, 1, sec) != 0) return 0;
            v->next_free = (cl + 1u < 2u + total) ? (cl + 1u) : 2u;
            return cl;
        }
    }
    return 0;   /* full */
}

static void free_cluster(exfat_volume_t *v, uint32_t cluster)
{
    if (cluster < 2) return;
    (void)bitmap_set_bit(v, cluster, false);
    if (cluster < v->next_free) v->next_free = cluster;   /* reuse soon */
}

/* Free a FAT chain of up to `count` clusters starting at `cluster`. */
static void free_chain(exfat_volume_t *v, uint32_t cluster, uint32_t count)
{
    uint32_t cl = cluster;
    for (uint32_t i = 0; i < count && cl >= 2 && cl < FAT_BAD; i++)
    {
        uint32_t nx;
        bool ok = fat_next(v, cl, &nx);
        free_cluster(v, cl);
        if (!ok || nx < 2 || nx >= FAT_BAD) break;
        cl = nx;
    }
}

/* Cluster holding cluster-index `idx` of the open file. False if idx is
 * past the chain or on I/O error. */
static bool cluster_for_index(exfat_volume_t *v, exfat_file_t *f,
                              uint32_t idx, uint32_t *out)
{
    if (f->first_cluster < 2) return false;
    uint32_t cl = f->first_cluster;
    if (f->contiguous)
    {
        *out = cl + idx;
        return true;
    }
    for (uint32_t i = 0; i < idx; i++)
    {
        uint32_t nx;
        if (!fat_next(v, cl, &nx) || nx < 2 || nx >= FAT_BAD) return false;
        cl = nx;
    }
    *out = cl;
    return true;
}

/* Ensure the file owns at least `need` clusters, extending the chain as
 * required. A contiguous file is converted to a FAT chain on the first
 * extension. Updates f->first_cluster / f->contiguous. Returns the new
 * cluster count, or 0 on failure (no space / I/O). */
static uint32_t ensure_clusters(exfat_volume_t *v, exfat_file_t *f,
                                uint32_t need, uint32_t cluster_bytes)
{
    uint32_t have;
    if (f->first_cluster < 2)
        have = 0;
    else
        have = (uint32_t)shr64(f->size + (cluster_bytes - 1u), v->csz_shift);
    if (have == 0 && f->first_cluster >= 2) have = 1;   /* 0-byte but allocated */

    if (need <= have) return have;

    /* lay FAT links over the existing contiguous run before appending */
    if (f->contiguous && have > 1)
        for (uint32_t i = 0; i + 1 < have; i++)
            if (!fat_set(v, f->first_cluster + i, f->first_cluster + i + 1u))
                return 0;
    f->contiguous = false;

    /* find the current last cluster */
    uint32_t last = 0;
    if (have > 0)
    {
        last = f->first_cluster;
        for (uint32_t i = 1; i < have; i++)
        {
            uint32_t nx;
            if (!fat_next(v, last, &nx) || nx < 2 || nx >= FAT_BAD)
            {
                if (!fat_set(v, last, FAT_EOC)) return 0;   /* truncate corrupt tail */
                break;
            }
            last = nx;
        }
        if (!fat_set(v, last, FAT_EOC)) return 0;
    }

    for (uint32_t i = have; i < need; i++)
    {
        uint32_t nc = alloc_cluster(v);
        if (nc == 0) return 0;
        if (!fat_set(v, nc, FAT_EOC)) return 0;
        if (last == 0)            f->first_cluster = nc;
        else if (!fat_set(v, last, nc)) return 0;
        last = nc;
    }
    return need;
}

/* Random access to a FAT-chained directory's 32-byte entries. Directories
 * are assumed FAT-chained (root and Windows-created subdirs are); entry
 * `index` is reached by walking the FAT from dir_cluster. write_back!=0
 * stores `ent`, else reads into it. False on I/O error or past-end. */
static bool dir_entry_io(exfat_volume_t *v, uint32_t dir_cluster,
                         uint32_t index, uint8_t *ent, bool write_back)
{
    uint32_t spc512      = 1u << (v->csz_shift - 9);
    uint32_t per_cluster = spc512 * DIR_ENTRIES_PER_SEC;
    uint32_t cl_idx      = index / per_cluster;        /* 32-bit div */
    uint32_t in_cl       = index % per_cluster;        /* 32-bit mod */

    uint32_t cl = dir_cluster;
    for (uint32_t i = 0; i < cl_idx; i++)
    {
        uint32_t nx;
        if (!fat_next(v, cl, &nx) || nx < 2 || nx >= FAT_BAD) return false;
        cl = nx;
    }
    uint32_t base;
    if (!cluster_lba512(v, cl, &base)) return false;
    uint32_t lba = base + (in_cl / DIR_ENTRIES_PER_SEC);
    uint32_t slot_off = (in_cl % DIR_ENTRIES_PER_SEC) * ENTRY_BYTES;

    uint8_t sec[512];
    if (v->rd(v->ctx, lba, 1, sec) != 0) return false;
    if (write_back)
    {
        memcpy(sec + slot_off, ent, ENTRY_BYTES);
        return v->wr(v->ctx, lba, 1, sec) == 0;
    }
    memcpy(ent, sec + slot_off, ENTRY_BYTES);
    return true;
}

/* Commit the open file's size / valid / first_cluster / NoFatChain flag
 * into its on-disk entry set and fix the set checksum. */
static int entry_meta_commit(exfat_volume_t *v, exfat_file_t *f)
{
    if (f->dir_cluster < 2) return -1;   /* no location (root has none) */

    uint32_t file_index   = f->set_offset / ENTRY_BYTES;
    uint32_t stream_index = file_index + 1u;

    uint8_t fileent[ENTRY_BYTES];
    uint8_t stream[ENTRY_BYTES];
    if (!dir_entry_io(v, f->dir_cluster, file_index,   fileent, false)) return -2;
    if (!dir_entry_io(v, f->dir_cluster, stream_index, stream,  false)) return -2;
    if (fileent[0] != EXFAT_ENTRY_FILE || stream[0] != EXFAT_ENTRY_STREAM) return -1;

    uint8_t sflags = stream[1];
    if (f->contiguous) sflags |=  FLAG_NOFATCHAIN;
    else               sflags &= (uint8_t)~FLAG_NOFATCHAIN;
    stream[1] = sflags;

    uint64_t vv = f->valid;                              /* ValidDataLength @ +8 */
    for (int i = 0; i < 8; i++) { stream[8 + i] = (uint8_t)vv; vv >>= 8; }
    stream[20] = (uint8_t)(f->first_cluster);            /* FirstCluster @ +20 */
    stream[21] = (uint8_t)(f->first_cluster >> 8);
    stream[22] = (uint8_t)(f->first_cluster >> 16);
    stream[23] = (uint8_t)(f->first_cluster >> 24);
    uint64_t sz = f->size;                               /* DataLength @ +24 */
    for (int i = 0; i < 8; i++) { stream[24 + i] = (uint8_t)sz; sz >>= 8; }

    uint8_t scount = fileent[1];
    if (scount < 2 || scount > MAX_SECONDARY) return -1;

    uint8_t set[(1u + MAX_SECONDARY) * ENTRY_BYTES];
    memcpy(set,               fileent, ENTRY_BYTES);
    memcpy(set + ENTRY_BYTES, stream,  ENTRY_BYTES);
    for (uint8_t i = 1; i < scount; i++)
        if (!dir_entry_io(v, f->dir_cluster, file_index + 1u + i,
                          set + (1u + i) * ENTRY_BYTES, false)) return -2;

    uint16_t csum = entry_set_checksum(set, (uint16_t)(1u + scount));
    fileent[2] = (uint8_t)(csum);
    fileent[3] = (uint8_t)(csum >> 8);

    if (!dir_entry_io(v, f->dir_cluster, stream_index, stream,  true)) return -2;
    if (!dir_entry_io(v, f->dir_cluster, file_index,   fileent, true)) return -2;
    return 0;
}

/* ===== create / mkdir helpers (Phase 4b) ===== */

/* Fixed timestamp (2024-01-01 00:00:00) for new entries — MainDOB has no
 * wall clock wired in here. Year is relative to 1980 (2024 -> 44). */
#define EXFAT_FIXED_TS  (((uint32_t)44u << 25) | ((uint32_t)1u << 21) | ((uint32_t)1u << 16))

/* ASCII up-case for the NameHash (mandatory first-128 table: a-z -> A-Z,
 * else identity). Non-ASCII names hash approximately (noted limitation). */
static uint16_t upcase_ascii(uint16_t c)
{
    return (c >= 0x61u && c <= 0x7Au) ? (uint16_t)(c - 0x20u) : c;
}

/* NameHash over the up-cased UTF-16 name (spec 7.6.4): same rotation as the
 * set checksum, over NameLength*2 little-endian bytes, no skipped bytes. */
static uint16_t name_hash(const uint16_t *name, uint8_t namelen)
{
    uint16_t hash = 0;
    for (uint8_t i = 0; i < namelen; i++)
    {
        uint16_t u = upcase_ascii(name[i]);
        hash = (uint16_t)(((hash & 1u) ? 0x8000u : 0u) + (hash >> 1) + (u & 0xFFu));
        hash = (uint16_t)(((hash & 1u) ? 0x8000u : 0u) + (hash >> 1) + (u >> 8));
    }
    return hash;
}

/* UTF-8 -> UTF-16LE (BMP). Returns the number of UTF-16 code units, or 0 on
 * invalid/oversize input. Astral (4-byte) sequences collapse to '?'. */
static uint8_t utf8_to_utf16(const char *s, uint16_t *out, uint8_t cap)
{
    uint8_t n = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p)
    {
        if (n >= cap) return 0;
        uint32_t cp;
        uint8_t c = *p;
        if (c < 0x80u) { cp = c; p += 1; }
        else if ((c & 0xE0u) == 0xC0u)
        {
            if ((p[1] & 0xC0u) != 0x80u) return 0;
            cp = ((uint32_t)(c & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
            p += 2;
        }
        else if ((c & 0xF0u) == 0xE0u)
        {
            if ((p[1] & 0xC0u) != 0x80u || (p[2] & 0xC0u) != 0x80u) return 0;
            cp = ((uint32_t)(c & 0x0Fu) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) |
                 (uint32_t)(p[2] & 0x3Fu);
            p += 3;
        }
        else if ((c & 0xF8u) == 0xF0u) { cp = (uint32_t)'?'; p += 4; }
        else return 0;
        out[n++] = (uint16_t)cp;
    }
    return n;
}

/* Reject names containing characters exFAT forbids (spec 7.7.3). */
static bool name_chars_ok(const uint16_t *name, uint8_t n)
{
    for (uint8_t i = 0; i < n; i++)
    {
        uint16_t c = name[i];
        if (c < 0x20u) return false;
        if (c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
            c == '>' || c == '?' || c == '\\' || c == '|') return false;
    }
    return true;
}

/* Zero a freshly allocated cluster (initialises a new directory to all EOD). */
static bool cluster_zero(exfat_volume_t *v, uint32_t cluster)
{
    uint32_t spc512 = 1u << (v->csz_shift - 9);
    uint32_t base;
    if (!cluster_lba512(v, cluster, &base)) return false;
    uint8_t zero[512];
    memset(zero, 0, sizeof(zero));
    for (uint32_t s = 0; s < spc512; s++)
        if (v->wr(v->ctx, base + s, 1, zero) != 0) return false;
    return true;
}

/* Find `count` consecutive free entries in a FAT-chained directory,
 * extending it (and zeroing the new clusters) if needed. *out_index gets
 * the start entry index; *out_added gets clusters added. False on no-space
 * / I/O error. */
static bool dir_find_free(exfat_volume_t *v, uint32_t dir_cluster,
                          uint32_t count, uint32_t *out_index, uint32_t *out_added)
{
    uint32_t spc512      = 1u << (v->csz_shift - 9);
    uint32_t per_cluster = spc512 * DIR_ENTRIES_PER_SEC;

    uint32_t cl      = dir_cluster;
    uint32_t index   = 0;
    uint32_t run_len = 0;
    uint32_t last_cl = dir_cluster;
    *out_added = 0;

    for (;;)
    {
        uint32_t base;
        if (!cluster_lba512(v, cl, &base)) return false;
        for (uint32_t s = 0; s < spc512; s++)
        {
            uint8_t sec[512];
            if (v->rd(v->ctx, base + s, 1, sec) != 0) return false;
            for (uint32_t e = 0; e < DIR_ENTRIES_PER_SEC; e++)
            {
                uint8_t t = sec[e * ENTRY_BYTES];
                bool freeslot = (t == EXFAT_ENTRY_EOD) || ((t & 0x80u) == 0);
                run_len = freeslot ? (run_len + 1u) : 0u;
                if (run_len >= count) { *out_index = index - (count - 1u); return true; }
                index++;
            }
        }
        last_cl = cl;
        uint32_t nx;
        if (!fat_next(v, cl, &nx) || nx < 2 || nx >= FAT_BAD) break;
        cl = nx;
    }

    /* extend the chain so the trailing free run reaches `count` */
    uint32_t need_more = (run_len < count) ? (count - run_len) : 0u;
    uint32_t add_cl    = (need_more + per_cluster - 1u) / per_cluster;
    if (add_cl == 0) add_cl = 1u;

    uint32_t link_from = last_cl;
    for (uint32_t i = 0; i < add_cl; i++)
    {
        uint32_t nc = alloc_cluster(v);
        if (nc == 0) return false;
        if (!fat_set(v, nc, FAT_EOC))   return false;
        if (!fat_set(v, link_from, nc)) return false;
        if (!cluster_zero(v, nc))       return false;
        link_from = nc;
    }
    *out_added = add_cl;
    *out_index = index - run_len;   /* start of the now-long-enough trailing run */
    return true;
}

/* Build and write a File entry set (File + Stream + FileName entries) for
 * `name` into directory `dir_cluster` at entry `slot`. `first_cluster`/
 * `data_len` describe the allocation (0/0 for an empty file; the dir's
 * cluster/size for a directory). Returns 0 or -2 on I/O error. */
static int write_entry_set(exfat_volume_t *v, uint32_t dir_cluster, uint32_t slot,
                           const uint16_t *name, uint8_t namelen,
                           bool is_dir, uint32_t first_cluster, uint64_t data_len)
{
    uint8_t  name_entries = (uint8_t)((namelen + 14u) / 15u);
    uint8_t  scount       = (uint8_t)(1u + name_entries);   /* Stream + FileName(s) */
    uint16_t hash         = name_hash(name, namelen);

    uint8_t set[(1u + MAX_SECONDARY) * ENTRY_BYTES];
    memset(set, 0, sizeof(set));

    uint8_t *fe = set;                       /* File entry */
    fe[0] = EXFAT_ENTRY_FILE;
    fe[1] = scount;
    uint16_t attr = is_dir ? ATTR_DIRECTORY : 0x0020u /* Archive */;
    fe[4] = (uint8_t)attr; fe[5] = (uint8_t)(attr >> 8);
    uint32_t ts = EXFAT_FIXED_TS;
    fe[8]  = (uint8_t)ts;        fe[9]  = (uint8_t)(ts >> 8);
    fe[10] = (uint8_t)(ts >> 16); fe[11] = (uint8_t)(ts >> 24);
    fe[12] = fe[8]; fe[13] = fe[9]; fe[14] = fe[10]; fe[15] = fe[11];   /* modified  */
    fe[16] = fe[8]; fe[17] = fe[9]; fe[18] = fe[10]; fe[19] = fe[11];   /* accessed  */

    uint8_t *se = set + ENTRY_BYTES;         /* Stream Extension */
    se[0] = EXFAT_ENTRY_STREAM;
    se[1] = 0x01u;                           /* AllocationPossible=1, NoFatChain=0 */
    se[3] = namelen;
    se[4] = (uint8_t)hash; se[5] = (uint8_t)(hash >> 8);
    uint64_t vdl = data_len;                 /* dirs: VDL==DL; new file: 0 */
    for (int i = 0; i < 8; i++) { se[8 + i] = (uint8_t)vdl; vdl >>= 8; }
    se[20] = (uint8_t)first_cluster;        se[21] = (uint8_t)(first_cluster >> 8);
    se[22] = (uint8_t)(first_cluster >> 16); se[23] = (uint8_t)(first_cluster >> 24);
    uint64_t dl = data_len;
    for (int i = 0; i < 8; i++) { se[24 + i] = (uint8_t)dl; dl >>= 8; }

    uint16_t done = 0;                       /* File Name entries */
    for (uint8_t i = 0; i < name_entries; i++)
    {
        uint8_t *ne = set + (uint32_t)(2u + i) * ENTRY_BYTES;
        ne[0] = EXFAT_ENTRY_FILENAME;
        for (int k = 0; k < 15 && done < namelen; k++)
        {
            uint16_t ch = name[done++];
            ne[2 + k * 2] = (uint8_t)ch;
            ne[3 + k * 2] = (uint8_t)(ch >> 8);
        }
    }

    uint16_t csum = entry_set_checksum(set, (uint16_t)(1u + scount));
    fe[2] = (uint8_t)csum; fe[3] = (uint8_t)(csum >> 8);

    for (uint8_t i = 0; i < (uint8_t)(1u + scount); i++)
        if (!dir_entry_io(v, dir_cluster, slot + i, set + (uint32_t)i * ENTRY_BYTES, true))
            return -2;
    return 0;
}

/* Split `path` into parent directory + final component, resolving the
 * parent. Returns 1 (parent found and is a directory), 0 (not found / not a
 * directory / bad path), -2 (I/O). `base` receives the final component. */
static int resolve_parent(exfat_volume_t *v, const char *path,
                          file_info_t *parent, char *base, uint32_t base_cap)
{
    uint32_t len = 0;
    while (path[len]) len++;
    while (len > 0 && path[len - 1] == '/') len--;       /* ignore trailing slash */
    if (len == 0) return 0;                              /* root has no parent    */

    uint32_t cut = len;
    while (cut > 0 && path[cut - 1] != '/') cut--;       /* cut = after last '/'  */

    uint32_t bl = len - cut;
    if (bl == 0 || bl >= base_cap) return 0;
    for (uint32_t i = 0; i < bl; i++) base[i] = path[cut + i];
    base[bl] = 0;

    char parent_path[260];
    if (cut == 0) { parent_path[0] = '/'; parent_path[1] = 0; }
    else
    {
        if (cut >= sizeof(parent_path)) return 0;
        for (uint32_t i = 0; i < cut; i++) parent_path[i] = path[i];
        parent_path[cut] = 0;
    }

    int r = resolve(v, parent_path, parent);
    if (r < 0) return -2;
    if (r == 0 || !parent->is_dir) return 0;
    return 1;
}

/* Shared create path for files (is_dir=false) and directories (is_dir=true).
 * Returns 0 on success, -1 invalid/exists, -2 I/O, -4 no space. */
static int exfat_create_common(exfat_volume_t *v, const char *path, bool is_dir)
{
    file_info_t parent;
    char base[EXFAT_NAME_MAX + 1];
    int pr = resolve_parent(v, path, &parent, base, sizeof(base));
    if (pr < 0) return -2;
    if (pr == 0) return -1;

    {
        file_info_t existing;
        int er = resolve(v, path, &existing);
        if (er < 0) return -2;
        if (er == 1) return -1;                 /* name already exists */
    }

    uint16_t u16[EXFAT_NAME_MAX + 1];
    uint8_t  namelen = utf8_to_utf16(base, u16, EXFAT_NAME_MAX);
    if (namelen == 0)                 return -1;
    if (!name_chars_ok(u16, namelen)) return -1;

    bool     parent_is_root = (parent.first_cluster == v->root_cluster);
    uint32_t cluster_bytes  = 1u << v->csz_shift;

    uint32_t first_cluster = 0;
    uint64_t data_len      = 0;
    if (is_dir)
    {
        uint32_t nc = alloc_cluster(v);
        if (nc == 0)                  return -4;
        if (!fat_set(v, nc, FAT_EOC)) return -2;
        if (!cluster_zero(v, nc))     return -2;
        first_cluster = nc;
        data_len      = cluster_bytes;          /* one cluster; dirs: VDL==DL */
    }

    uint8_t  name_entries = (uint8_t)((namelen + 14u) / 15u);
    uint32_t set_count    = 2u + name_entries;  /* File + Stream + FileName(s) */

    uint32_t slot, added;
    if (!dir_find_free(v, parent.first_cluster, set_count, &slot, &added))
    {
        if (is_dir && first_cluster) free_cluster(v, first_cluster);
        return -4;
    }

    /* if a non-root directory grew, update its DataLength in its parent */
    if (added > 0 && !parent_is_root)
    {
        exfat_file_t pf;
        memset(&pf, 0, sizeof(pf));
        pf.first_cluster = parent.first_cluster;
        pf.dir_cluster   = parent.dir_cluster;
        pf.set_offset    = parent.set_index * ENTRY_BYTES;
        pf.contiguous    = false;
        uint32_t total   = (uint32_t)shr64(parent.size + (cluster_bytes - 1u), v->csz_shift)
                           + added;
        pf.size  = shl64((uint64_t)total, v->csz_shift);   /* total * cluster_bytes */
        pf.valid = pf.size;
        (void)entry_meta_commit(v, &pf);                   /* best effort */
    }

    return write_entry_set(v, parent.first_cluster, slot, u16, namelen,
                           is_dir, first_cluster, data_len);
}

/* ===== public API: write path (Phase 2 — stubbed) ===== */

static int exfat_write(exfat_volume_t *v, exfat_file_t *f, const void *buf, uint32_t n)
{
    if (f->is_dir) return -1;
    if (n == 0)    return 0;

    uint32_t cluster_bytes = 1u << v->csz_shift;
    uint32_t spc512        = 1u << (v->csz_shift - 9);
    uint64_t write_end     = f->pos + n;

    /* clusters needed to cover [0, write_end) */
    uint32_t need = (uint32_t)shr64(write_end + (cluster_bytes - 1u), v->csz_shift);
    if (need == 0) need = 1;
    if (ensure_clusters(v, f, need, cluster_bytes) < need) return -4;   /* no space */

    /* A write starting beyond ValidDataLength leaves a gap [valid, pos):
     * exFAT has no sub-valid holes, so it must be real zeros on disk. */
    if (f->pos > f->valid)
    {
        uint64_t gp = f->valid;
        uint8_t  zero[512];
        memset(zero, 0, sizeof(zero));
        while (gp < f->pos)
        {
            uint32_t ci = (uint32_t)shr64(gp, v->csz_shift);
            uint32_t io = (uint32_t)gp & (cluster_bytes - 1u);
            uint32_t cl2, b2;
            if (!cluster_for_index(v, f, ci, &cl2)) return -2;
            if (!cluster_lba512(v, cl2, &b2))       return -2;
            uint32_t s2 = io >> 9, o2 = io & 511u;
            while (s2 < spc512 && gp < f->pos)
            {
                uint32_t chunk = 512u - o2;
                uint64_t rem   = f->pos - gp;
                if ((uint64_t)chunk > rem) chunk = (uint32_t)rem;
                if (o2 != 0 || chunk != 512u)
                {
                    uint8_t t2[512];
                    if (v->rd(v->ctx, b2 + s2, 1, t2) != 0) return -2;
                    memset(t2 + o2, 0, chunk);
                    if (v->wr(v->ctx, b2 + s2, 1, t2) != 0) return -2;
                }
                else if (v->wr(v->ctx, b2 + s2, 1, zero) != 0) return -2;
                gp += chunk; o2 = 0; s2++;
            }
        }
        f->valid = f->pos;
    }

    const uint8_t *src  = (const uint8_t *)buf;
    uint32_t       done = 0;
    while (done < n)
    {
        uint64_t pos    = f->pos + done;
        uint32_t cl_idx = (uint32_t)shr64(pos, v->csz_shift);
        uint32_t in_cl  = (uint32_t)pos & (cluster_bytes - 1u);

        uint32_t cluster, base;
        if (!cluster_for_index(v, f, cl_idx, &cluster)) return done ? (int)done : -2;
        if (!cluster_lba512(v, cluster, &base))         return done ? (int)done : -2;

        uint32_t sec = in_cl >> 9;
        uint32_t off = in_cl & 511u;
        while (sec < spc512 && done < n)
        {
            uint32_t chunk = 512u - off;
            if (chunk > n - done) chunk = n - done;

            uint8_t tmp[512];
            if (off != 0 || chunk != 512u)
            {
                if (v->rd(v->ctx, base + sec, 1, tmp) != 0)
                    return done ? (int)done : -2;
            }
            memcpy(tmp + off, src + done, chunk);
            if (v->wr(v->ctx, base + sec, 1, tmp) != 0)
                return done ? (int)done : -2;

            done += chunk;
            off   = 0;
            sec++;
        }
    }

    uint64_t new_end = f->pos + done;
    if (new_end > f->valid) f->valid = new_end;
    if (new_end > f->size)  f->size  = new_end;
    f->pos  += done;
    f->dirty = true;
    return (int)done;
}

static int exfat_ftrunc(exfat_volume_t *v, exfat_file_t *f, uint64_t length)
{
    if (f->is_dir) return -1;

    uint32_t cluster_bytes = 1u << v->csz_shift;
    uint32_t need = (uint32_t)shr64(length + (cluster_bytes - 1u), v->csz_shift);

    uint32_t have;
    if (f->first_cluster < 2) have = 0;
    else have = (uint32_t)shr64(f->size + (cluster_bytes - 1u), v->csz_shift);
    if (have == 0 && f->first_cluster >= 2) have = 1;

    if (length > f->size)
    {
        /* grow: allocate clusters; new bytes read back as zero because
         * DataLength advances but ValidDataLength does not. */
        if (need > have && ensure_clusters(v, f, need, cluster_bytes) < need)
            return -4;
        f->size  = length;
        f->dirty = true;
        return 0;
    }

    /* shrink (or no size change): free clusters beyond `need`. */
    if (need < have)
    {
        if (need == 0)
        {
            if (f->contiguous)
                for (uint32_t i = 0; i < have; i++) free_cluster(v, f->first_cluster + i);
            else
                free_chain(v, f->first_cluster, have);
            f->first_cluster = 0;
        }
        else
        {
            uint32_t keep_last;
            if (!cluster_for_index(v, f, need - 1u, &keep_last)) return -2;
            if (f->contiguous)
            {
                for (uint32_t i = 0; i < have - need; i++)
                    free_cluster(v, keep_last + 1u + i);
            }
            else
            {
                uint32_t first_free;
                if (!fat_next(v, keep_last, &first_free)) return -2;
                if (!fat_set(v, keep_last, FAT_EOC))      return -2;
                free_chain(v, first_free, have - need);
            }
        }
    }

    f->size = length;
    if (f->valid > length) f->valid = length;
    f->dirty = true;
    return 0;
}

static int exfat_create(exfat_volume_t *v, const char *path)
{
    return exfat_create_common(v, path, false);
}

static int exfat_mkdir(exfat_volume_t *v, const char *path)
{
    return exfat_create_common(v, path, true);
}

/* Mark an entry set as deleted: clear the InUse bit (bit 7) of each entry's
 * TypeCode (0x85->0x05, 0xC0->0x40, 0xC1->0x41). These become "unused" (NOT
 * end-of-directory), so the directory scan continues past them. */
static int dir_set_delete(exfat_volume_t *v, uint32_t dir_cluster,
                          uint32_t set_index, uint8_t set_count)
{
    for (uint8_t i = 0; i < set_count; i++)
    {
        uint8_t ent[ENTRY_BYTES];
        if (!dir_entry_io(v, dir_cluster, set_index + i, ent, false)) return -2;
        ent[0] = (uint8_t)(ent[0] & 0x7Fu);
        if (!dir_entry_io(v, dir_cluster, set_index + i, ent, true))  return -2;
    }
    return 0;
}

/* True if directory `dir_cluster` holds no in-use File entry set. An I/O
 * error is reported as "not empty" so the caller refuses to delete. */
static bool dir_is_empty(exfat_volume_t *v, uint32_t dir_cluster,
                         bool contiguous, uint64_t size)
{
    bool      is_root = (dir_cluster == v->root_cluster);
    dir_cur_t c;
    dir_cur_init(&c, v, dir_cluster,
                 is_root ? false : contiguous,
                 is_root ? false : (size != 0),
                 is_root ? 0 : size);
    file_info_t fi;
    return dir_next_file(&c, &fi) == 0;
}

static int exfat_unlink(exfat_volume_t *v, const char *path)
{
    file_info_t info;
    int r = resolve(v, path, &info);
    if (r < 0) return -2;
    if (r == 0) return -1;
    if (info.dir_cluster < 2) return -1;        /* root / no entry set */

    if (info.is_dir &&
        !dir_is_empty(v, info.first_cluster, info.contiguous, info.size))
        return -5;                              /* directory not empty */

    if (info.first_cluster >= 2)                /* free the allocation */
    {
        uint32_t cluster_bytes = 1u << v->csz_shift;
        uint32_t clusters = (uint32_t)shr64(info.size + (cluster_bytes - 1u), v->csz_shift);
        if (clusters == 0) clusters = 1;
        if (info.contiguous)
            for (uint32_t i = 0; i < clusters; i++) free_cluster(v, info.first_cluster + i);
        else
            free_chain(v, info.first_cluster, clusters);
    }

    return dir_set_delete(v, info.dir_cluster, info.set_index, info.set_count);
}

static int exfat_rename(exfat_volume_t *v, const char *from, const char *to)
{
    file_info_t finfo;
    int r = resolve(v, from, &finfo);
    if (r < 0) return -2;
    if (r == 0) return -1;
    if (finfo.dir_cluster < 2) return -1;       /* can't rename root */

    {
        file_info_t texists;
        int er = resolve(v, to, &texists);
        if (er < 0) return -2;
        if (er == 1) return -5;                 /* target already exists */
    }

    file_info_t tparent;
    char base[EXFAT_NAME_MAX + 1];
    int pr = resolve_parent(v, to, &tparent, base, sizeof(base));
    if (pr < 0) return -2;
    if (pr == 0) return -1;

    uint16_t u16[EXFAT_NAME_MAX + 1];
    uint8_t  namelen = utf8_to_utf16(base, u16, EXFAT_NAME_MAX);
    if (namelen == 0)                 return -1;
    if (!name_chars_ok(u16, namelen)) return -1;

    /* read old File + Stream to preserve attributes, timestamps, NoFatChain,
     * ValidDataLength, FirstCluster and DataLength. */
    uint8_t oldfile[ENTRY_BYTES], oldstream[ENTRY_BYTES];
    if (!dir_entry_io(v, finfo.dir_cluster, finfo.set_index,      oldfile,   false)) return -2;
    if (!dir_entry_io(v, finfo.dir_cluster, finfo.set_index + 1u, oldstream, false)) return -2;
    if (oldfile[0] != EXFAT_ENTRY_FILE || oldstream[0] != EXFAT_ENTRY_STREAM) return -1;

    uint8_t  name_entries = (uint8_t)((namelen + 14u) / 15u);
    uint8_t  scount       = (uint8_t)(1u + name_entries);
    uint16_t hash         = name_hash(u16, namelen);
    uint32_t set_count    = 1u + scount;

    /* build the new set: preserved File (new SecondaryCount) + preserved
     * Stream (new NameLength / NameHash) + new FileName entries. */
    uint8_t set[(1u + MAX_SECONDARY) * ENTRY_BYTES];
    memset(set, 0, sizeof(set));
    memcpy(set, oldfile, ENTRY_BYTES);
    set[1] = scount;
    memcpy(set + ENTRY_BYTES, oldstream, ENTRY_BYTES);
    set[ENTRY_BYTES + 3] = namelen;
    set[ENTRY_BYTES + 4] = (uint8_t)hash;
    set[ENTRY_BYTES + 5] = (uint8_t)(hash >> 8);

    uint16_t done = 0;
    for (uint8_t i = 0; i < name_entries; i++)
    {
        uint8_t *ne = set + (uint32_t)(2u + i) * ENTRY_BYTES;
        ne[0] = EXFAT_ENTRY_FILENAME;
        for (int k = 0; k < 15 && done < namelen; k++)
        {
            uint16_t ch = u16[done++];
            ne[2 + k * 2] = (uint8_t)ch;
            ne[3 + k * 2] = (uint8_t)(ch >> 8);
        }
    }

    uint16_t csum = entry_set_checksum(set, (uint16_t)(1u + scount));
    set[2] = (uint8_t)csum; set[3] = (uint8_t)(csum >> 8);

    /* fast path: same directory and same entry count -> rewrite in place */
    if (finfo.dir_cluster == tparent.first_cluster && finfo.set_count == set_count)
    {
        for (uint8_t i = 0; i < (uint8_t)set_count; i++)
            if (!dir_entry_io(v, finfo.dir_cluster, finfo.set_index + i,
                              set + (uint32_t)i * ENTRY_BYTES, true)) return -2;
        return 0;
    }

    /* general path: write the new set (extending the target dir if needed),
     * then delete the old set. The data clusters are shared, not freed. */
    uint32_t slot, added;
    if (!dir_find_free(v, tparent.first_cluster, set_count, &slot, &added)) return -4;

    if (added > 0 && tparent.first_cluster != v->root_cluster)
    {
        uint32_t cluster_bytes = 1u << v->csz_shift;
        exfat_file_t pf;
        memset(&pf, 0, sizeof(pf));
        pf.first_cluster = tparent.first_cluster;
        pf.dir_cluster   = tparent.dir_cluster;
        pf.set_offset    = tparent.set_index * ENTRY_BYTES;
        pf.contiguous    = false;
        uint32_t total   = (uint32_t)shr64(tparent.size + (cluster_bytes - 1u), v->csz_shift)
                           + added;
        pf.size  = shl64((uint64_t)total, v->csz_shift);
        pf.valid = pf.size;
        (void)entry_meta_commit(v, &pf);
    }

    for (uint8_t i = 0; i < (uint8_t)set_count; i++)
        if (!dir_entry_io(v, tparent.first_cluster, slot + i,
                          set + (uint32_t)i * ENTRY_BYTES, true)) return -2;

    return dir_set_delete(v, finfo.dir_cluster, finfo.set_index, finfo.set_count);
}

/* Commit deferred metadata (size / valid / first_cluster / flags + set
 * checksum) for a written file. A clean file flushes as a no-op. */
static int exfat_flush(exfat_volume_t *v, exfat_file_t *f)
{
    if (!f || !f->dirty) return 0;
    int r = entry_meta_commit(v, f);
    if (r < 0) return r;
    f->dirty = false;
    return 0;
}

/* ===== public API: format (Phase 3 — stubbed) ===== */

/* ===== mkfs helpers (Phase 5) ===== */

/* Boot region checksum (spec Fig. 1): rotate-add over the bytes, skipping
 * the VolumeFlags (106,107) and PercentInUse (112) bytes of sector 0. */
static uint32_t boot_checksum_add(uint32_t sum, const uint8_t *sec,
                                  uint32_t len, bool first)
{
    for (uint32_t i = 0; i < len; i++)
    {
        if (first && (i == 106u || i == 107u || i == 112u)) continue;
        sum = (uint32_t)(((sum & 1u) ? 0x80000000u : 0u) + (sum >> 1) + sec[i]);
    }
    return sum;
}

/* Up-case table checksum (spec Fig. 3). */
static uint32_t upcase_checksum(const uint8_t *t, uint32_t len)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
        sum = (uint32_t)(((sum & 1u) ? 0x80000000u : 0u) + (sum >> 1) + t[i]);
    return sum;
}

/* Write `count` zeroed 512-byte sectors starting at `lba`, in chunks. */
static int wr_zeros(exfat_wrsec_t wr, void *ctx, uint32_t lba, uint32_t count)
{
    static uint8_t zbuf[8 * 512];          /* BSS: zero-initialised by the loader */
    while (count > 0)
    {
        uint32_t chunk = (count > 8u) ? 8u : count;
        if (wr(ctx, lba, chunk, zbuf) != 0) return -2;
        lba   += chunk;
        count -= chunk;
    }
    return 0;
}

/* Write one logical sector (located at logical index `llba`) given that a
 * logical sector spans `s512` physical 512-byte sectors. `buf` holds the
 * logical sector's bytes (s512 * 512). The wr callback is 512-byte based. */
static int wr_lsec(exfat_wrsec_t wr, void *ctx, uint32_t llba, uint32_t s512,
                   const uint8_t *buf)
{
    return wr(ctx, llba * s512, s512, buf);
}

static int exfat_mkfs(exfat_rdsec_t rd, exfat_wrsec_t wr, void *ctx,
                      uint64_t sectors, uint32_t bytes_per_sector,
                      const char *label)
{
    (void)rd;
    if (bytes_per_sector != 512u && bytes_per_sector != 4096u) return -3;
    if (sectors < 2048u) return -4;                /* smaller than 1 MB */

    /* `sectors` is the partition size in 512-byte (physical) sectors.
     * The exFAT logical sector size is `bytes_per_sector` (512 or 4096);
     * a logical sector spans s512 physical sectors. All exFAT on-disk
     * offsets are in logical sectors; the wr callback is 512-byte based. */
    uint32_t bps_shift = (bytes_per_sector == 4096u) ? 12u : 9u;
    uint32_t bps       = bytes_per_sector;
    uint32_t s512      = 1u << (bps_shift - 9u);   /* physical 512-sectors / logical sector */
    uint64_t vol_lsec  = shr64(sectors, bps_shift - 9u);   /* VolumeLength, logical sectors */
    if (vol_lsec < 64u) return -4;

    /* cluster size in bytes by volume size (4 KB / 32 KB / 128 KB), but never
     * smaller than one logical sector. */
    uint32_t clus_shift;
    if      (sectors <= 524288ull)   clus_shift = 12u;     /* <= 256 MB -> 4 KB   */
    else if (sectors <= 67108864ull) clus_shift = 15u;     /* <= 32 GB  -> 32 KB  */
    else                             clus_shift = 17u;     /* else      -> 128 KB */
    if (clus_shift < bps_shift) clus_shift = bps_shift;
    uint32_t spc_shift     = clus_shift - bps_shift;
    uint32_t spc           = 1u << spc_shift;
    uint32_t cluster_bytes = 1u << clus_shift;

    /* layout (logical sectors); FAT estimate overestimates, which is safe */
    uint32_t fat_offset   = 128u;                          /* >= 24 logical sectors */
    uint32_t est_clusters = (uint32_t)shr64(vol_lsec - fat_offset, spc_shift);
    uint64_t fat_bytes    = shl64((uint64_t)est_clusters + 2u, 2);    /* *4 */
    uint32_t fat_len      = (uint32_t)shr64(fat_bytes + (bps - 1u), bps_shift);
    uint32_t heap_offset  = (fat_offset + fat_len + (spc - 1u)) & ~(spc - 1u);
    fat_len               = heap_offset - fat_offset;      /* absorb alignment slack */

    uint32_t cluster_count = (uint32_t)shr64(vol_lsec - heap_offset, spc_shift);
    if (cluster_count < 4u) return -4;

    uint32_t bitmap_bytes   = (cluster_count + 7u) / 8u;
    uint32_t bmp_clusters   = (bitmap_bytes + cluster_bytes - 1u) / cluster_bytes;
    uint32_t bitmap_cluster = 2u;
    uint32_t upcase_cluster = bitmap_cluster + bmp_clusters;
    uint32_t root_cluster   = upcase_cluster + 1u;
    uint32_t used_clusters  = bmp_clusters + 2u;           /* bitmap + upcase + root */
    if (root_cluster >= (bps >> 2)) return -4;             /* chains must fit FAT lsec 0 */

    uint32_t heap_lba_bmp  = heap_offset + (bitmap_cluster - 2u) * spc;
    uint32_t heap_lba_up   = heap_offset + (upcase_cluster - 2u) * spc;
    uint32_t heap_lba_root = heap_offset + (root_cluster   - 2u) * spc;

    uint8_t sec[4096];

    /* up-case table (60 bytes, compressed: identity + a-z->A-Z) + checksum */
    uint8_t up[60];
    memset(up, 0, sizeof(up));
    up[0]=0xFFu; up[1]=0xFFu; up[2]=0x61u; up[3]=0x00u;            /* identity 0000..0060 */
    for (int i = 0; i < 26; i++) { up[4 + i*2] = (uint8_t)(0x41 + i); up[5 + i*2] = 0x00u; }
    up[56]=0xFFu; up[57]=0xFFu; up[58]=0x85u; up[59]=0xFFu;        /* identity 007B..FFFF */
    uint32_t up_csum = upcase_checksum(up, 60u);

    /* ---------- Main Boot Sector (logical sector 0) ---------- */
    memset(sec, 0, bps);
    sec[0]=0xEBu; sec[1]=0x76u; sec[2]=0x90u;                      /* JumpBoot   */
    sec[3]='E'; sec[4]='X'; sec[5]='F'; sec[6]='A'; sec[7]='T';
    sec[8]=' '; sec[9]=' '; sec[10]=' ';                           /* "EXFAT   " */
    { uint64_t v = vol_lsec; for (int i = 0; i < 8; i++) { sec[72+i]=(uint8_t)v; v >>= 8; } }
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
    sec[108]=(uint8_t)bps_shift;                                    /* BytesPerSectorShift    */
    sec[109]=(uint8_t)spc_shift;                                    /* SectorsPerClusterShift */
    sec[110]=1u;                                                    /* NumberOfFats           */
    sec[111]=0x80u;                                                 /* DriveSelect            */
    sec[112]=(uint8_t)((used_clusters * 100u) / cluster_count);     /* PercentInUse           */
    for (int i = 120; i < 510; i++) sec[i] = 0xF4u;                 /* BootCode = halt        */
    sec[510]=0x55u; sec[511]=0xAAu;                                 /* BootSignature          */

    uint32_t bsum = boot_checksum_add(0u, sec, bps, true);
    if (wr_lsec(wr, ctx, 0u,  s512, sec) != 0) return -2;
    if (wr_lsec(wr, ctx, 12u, s512, sec) != 0) return -2;          /* backup boot sector */

    /* ---------- Extended Boot Sectors (1..8) ---------- */
    memset(sec, 0, bps);
    sec[bps-4]=0x00u; sec[bps-3]=0x00u; sec[bps-2]=0x55u; sec[bps-1]=0xAAu; /* ExtendedBootSignature */
    for (uint32_t s = 1u; s <= 8u; s++)
    {
        bsum = boot_checksum_add(bsum, sec, bps, false);
        if (wr_lsec(wr, ctx, s,       s512, sec) != 0) return -2;
        if (wr_lsec(wr, ctx, s + 12u, s512, sec) != 0) return -2;
    }

    /* ---------- OEM Parameters (9) + Reserved (10) ---------- */
    memset(sec, 0, bps);
    for (uint32_t s = 9u; s <= 10u; s++)
    {
        bsum = boot_checksum_add(bsum, sec, bps, false);
        if (wr_lsec(wr, ctx, s,       s512, sec) != 0) return -2;
        if (wr_lsec(wr, ctx, s + 12u, s512, sec) != 0) return -2;
    }

    /* ---------- Boot Checksum (logical sector 11) ---------- */
    memset(sec, 0, bps);
    for (uint32_t i = 0; i < bps; i += 4u)
    {
        sec[i]   = (uint8_t)bsum;         sec[i+1] = (uint8_t)(bsum >> 8);
        sec[i+2] = (uint8_t)(bsum >> 16); sec[i+3] = (uint8_t)(bsum >> 24);
    }
    if (wr_lsec(wr, ctx, 11u, s512, sec) != 0) return -2;
    if (wr_lsec(wr, ctx, 23u, s512, sec) != 0) return -2;

    /* ---------- FAT ---------- */
    memset(sec, 0, bps);
    sec[0]=0xF8u; sec[1]=0xFFu; sec[2]=0xFFu; sec[3]=0xFFu;        /* FatEntry[0] = media   */
    sec[4]=0xFFu; sec[5]=0xFFu; sec[6]=0xFFu; sec[7]=0xFFu;        /* FatEntry[1] = FFFFFFFF */
    for (uint32_t i = 0; i < bmp_clusters; i++)                    /* bitmap chain */
    {
        uint32_t cl   = bitmap_cluster + i;
        uint32_t next = (i + 1u < bmp_clusters) ? (cl + 1u) : FAT_EOC;
        sec[cl*4+0]=(uint8_t)next; sec[cl*4+1]=(uint8_t)(next>>8);
        sec[cl*4+2]=(uint8_t)(next>>16); sec[cl*4+3]=(uint8_t)(next>>24);
    }
    sec[upcase_cluster*4+0]=0xFFu; sec[upcase_cluster*4+1]=0xFFu;  /* upcase: EOC */
    sec[upcase_cluster*4+2]=0xFFu; sec[upcase_cluster*4+3]=0xFFu;
    sec[root_cluster*4+0]=0xFFu;   sec[root_cluster*4+1]=0xFFu;    /* root: EOC */
    sec[root_cluster*4+2]=0xFFu;   sec[root_cluster*4+3]=0xFFu;
    if (wr_lsec(wr, ctx, fat_offset, s512, sec) != 0) return -2;
    if (fat_len > 1u &&
        wr_zeros(wr, ctx, (fat_offset + 1u) * s512, (fat_len - 1u) * s512) != 0) return -2;

    /* ---------- Allocation Bitmap ---------- */
    memset(sec, 0, bps);
    for (uint32_t i = 0; i < used_clusters; i++)                   /* first used_clusters bits */
        sec[i >> 3] = (uint8_t)(sec[i >> 3] | (1u << (i & 7u)));
    if (wr_lsec(wr, ctx, heap_lba_bmp, s512, sec) != 0) return -2;
    {
        uint32_t bmp_lsec = bmp_clusters * spc;
        if (bmp_lsec > 1u &&
            wr_zeros(wr, ctx, (heap_lba_bmp + 1u) * s512, (bmp_lsec - 1u) * s512) != 0)
            return -2;
    }

    /* ---------- Up-case Table ---------- */
    memset(sec, 0, bps);
    memcpy(sec, up, 60u);
    if (wr_lsec(wr, ctx, heap_lba_up, s512, sec) != 0) return -2;
    if (spc > 1u &&
        wr_zeros(wr, ctx, (heap_lba_up + 1u) * s512, (spc - 1u) * s512) != 0) return -2;

    /* ---------- Root Directory ---------- */
    memset(sec, 0, bps);
    sec[0] = 0x83u;                                                /* Volume Label entry */
    {
        uint8_t n = 0;
        if (label && label[0])
        {
            uint16_t lab[11];
            n = utf8_to_utf16(label, lab, 11u);
            for (uint8_t i = 0; i < n; i++)
            {
                sec[2 + i*2] = (uint8_t)lab[i];
                sec[3 + i*2] = (uint8_t)(lab[i] >> 8);
            }
        }
        sec[1] = n;                                                /* CharacterCount */
    }
    {
        uint8_t *b = sec + ENTRY_BYTES;                            /* Allocation Bitmap entry */
        b[0] = 0x81u;
        b[20]=(uint8_t)bitmap_cluster;      b[21]=(uint8_t)(bitmap_cluster>>8);
        b[22]=(uint8_t)(bitmap_cluster>>16);b[23]=(uint8_t)(bitmap_cluster>>24);
        uint64_t dl = bitmap_bytes;
        for (int i = 0; i < 8; i++) { b[24+i]=(uint8_t)dl; dl >>= 8; }
    }
    {
        uint8_t *u = sec + 2u * ENTRY_BYTES;                       /* Up-case Table entry */
        u[0] = 0x82u;
        u[4]=(uint8_t)up_csum;       u[5]=(uint8_t)(up_csum>>8);
        u[6]=(uint8_t)(up_csum>>16); u[7]=(uint8_t)(up_csum>>24);
        u[20]=(uint8_t)upcase_cluster;      u[21]=(uint8_t)(upcase_cluster>>8);
        u[22]=(uint8_t)(upcase_cluster>>16);u[23]=(uint8_t)(upcase_cluster>>24);
        u[24]=60u;                                                 /* DataLength = 60 */
    }
    if (wr_lsec(wr, ctx, heap_lba_root, s512, sec) != 0) return -2;
    if (spc > 1u &&
        wr_zeros(wr, ctx, (heap_lba_root + 1u) * s512, (spc - 1u) * s512) != 0) return -2;

    return 0;
}

/* ===== exports ===== */

__attribute__((visibility("default")))
exfat_api_t __mem_exports =
{
    .mount   = exfat_mount,
    .unmount = exfat_unmount,
    .stat    = exfat_stat,
    .open    = exfat_open,
    .read    = exfat_read,
    .readdir = exfat_readdir,
    .write   = exfat_write,
    .ftrunc  = exfat_ftrunc,
    .create  = exfat_create,
    .mkdir   = exfat_mkdir,
    .unlink  = exfat_unlink,
    .rename  = exfat_rename,
    .flush   = exfat_flush,
    .mkfs    = exfat_mkfs,
};
