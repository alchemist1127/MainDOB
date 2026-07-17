/* iso9660.mem — ISO9660 filesystem parser, PIC ELF shared object
 * loaded by drivers/cdrom/main.c via dob_mem_load().
 *
 * Locates the Primary Volume Descriptor at LBA 16, walks slash-
 * separated paths through directory extents, exposes stat/open/read/
 * readdir as declared in iso9660_api.h.
 *
 * Out of scope: Joliet / Rock Ridge (8.3 POSIX names suffice for
 * boot media), multi-extent files, El Torito.
 *
 * The only external symbol is the iso_rdsec_t callback the caller
 * hands to mount() — elf_load_shared accepts only R_386_RELATIVE,
 * so any other unresolved reference would fail the load.
 */

#include "iso9660_api.h"

/* ===== libc-free primitives =====
 * Named memcpy/memset (not iso_memcpy/iso_memset) because GCC at -O2
 * may emit implicit calls to them while lowering struct copies. */

void *memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int v, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)v;
    return dst;
}

/* ASCII case-insensitive compare over n bytes. ISO9660 stores names in
 * uppercase, but users type lowercase; we compare both as upper. */
static int icase_ncmp(const char *a, const char *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
    {
        uint8_t ca = (uint8_t)a[i], cb = (uint8_t)b[i];
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return (int)ca - (int)cb;
        if (!ca) return 0;
    }
    return 0;
}

/* ===== On-disk structures =====
 *
 * ISO9660 stores multi-byte fields as both-endian pairs (LE first, BE
 * second). We only ever read the LE half. */

typedef struct __attribute__((packed))
{
    uint8_t  length;             /* record length, 0 marks end-of-sector */
    uint8_t  ext_attr_len;
    uint32_t extent_le;
    uint32_t extent_be;
    uint32_t size_le;
    uint32_t size_be;
    uint8_t  datetime[7];
    uint8_t  flags;              /* bit1 = directory */
    uint8_t  file_unit_size;
    uint8_t  interleave_gap;
    uint16_t vol_seq_le;
    uint16_t vol_seq_be;
    uint8_t  name_len;
    /* name[name_len] follows; padded to even total length */
} iso_dir_rec_t;

#define ISO_FLAG_DIR  0x02

typedef struct __attribute__((packed))
{
    uint8_t  type;               /* 1 = primary */
    char     id[5];              /* "CD001" */
    uint8_t  version;
    uint8_t  unused1;
    char     sys_id[32];
    char     vol_id[32];
    uint8_t  unused2[8];
    uint32_t vol_space_le;
    uint32_t vol_space_be;
    uint8_t  unused3[32];
    uint16_t vol_set_size_le;
    uint16_t vol_set_size_be;
    uint16_t vol_seq_le;
    uint16_t vol_seq_be;
    uint16_t lb_size_le;
    uint16_t lb_size_be;
    uint32_t path_table_size_le;
    uint32_t path_table_size_be;
    uint32_t lpath_table_lba;
    uint32_t lpath_table_opt_lba;
    uint32_t mpath_table_lba;
    uint32_t mpath_table_opt_lba;
    uint8_t  root_dir_rec[34];   /* embedded iso_dir_rec_t */
} iso_pvd_t;

#define ISO_PVD_LBA       16
#define ISO_VD_TYPE_PVD   1
#define ISO_VD_TYPE_END   255

/* ===== Volume handle =====
 *
 * Single static slot — .mem must not depend on the heap, and the cdrom
 * driver is single-mount-per-process anyway. A second mount on the same
 * .mem instance returns NULL. */

struct iso_volume
{
    bool         used;
    iso_rdsec_t  rdsec;
    void        *ctx;
    uint32_t     root_lba;
    uint32_t     root_size;

    /* One-sector cache. Directory walks hit the same sector several
     * times in a row (one read, multiple records); this halves the
     * IPC traffic during lookups. */
    uint8_t      cache[ISO_SECTOR_SIZE];
    uint32_t     cache_lba;
    bool         cache_valid;
};

static struct iso_volume g_volume;

/* Return a pointer to the requested sector inside the volume cache.
 * On hit: zero copies. On miss: one rdsec call into v->cache. The
 * caller MUST copy out anything it needs before calling read_sector
 * again on a different LBA. Returns NULL on I/O error. */
static const uint8_t *read_sector(iso_volume_t *v, uint32_t lba)
{
    if (v->cache_valid && v->cache_lba == lba) return v->cache;
    if (v->rdsec(v->ctx, lba, v->cache) != 0)
    { v->cache_valid = false; return NULL; }
    v->cache_lba   = lba;
    v->cache_valid = true;
    return v->cache;
}

/* Normalize a directory-record name into a user-friendly string:
 *   - "\0" → "."   (current directory marker)
 *   - "\1" → ".."  (parent marker)
 *   - strip ";<version>" suffix
 *   - strip a trailing "." that ISO appends to extension-less files
 * Returns the length written (without the NUL). */
static uint32_t clean_name(const char *raw, uint32_t raw_len,
                           char *out, uint32_t out_max)
{
    if (raw_len == 1 && (raw[0] == 0 || raw[0] == 1))
    {
        if (out_max >= 2) out[0] = '.';
        if (raw[0] == 1 && out_max >= 3)
        { out[1] = '.'; out[2] = 0; return 2; }
        if (out_max >= 2) { out[1] = 0; return 1; }
        return 0;
    }

    uint32_t n = raw_len;
    for (uint32_t i = 0; i < raw_len; i++)
        if (raw[i] == ';') { n = i; break; }
    if (n > 0 && raw[n - 1] == '.') n--;

    if (n >= out_max) n = out_max - 1;
    memcpy(out, raw, n);
    out[n] = 0;
    return n;
}

/* True if the cleaned name is "." or "..". */
static bool is_dot_entry(const char *cleaned, uint32_t len)
{
    if (len == 1 && cleaned[0] == '.') return true;
    if (len == 2 && cleaned[0] == '.' && cleaned[1] == '.') return true;
    return false;
}

/* Iterate every record in the directory extent at (lba, total_size).
 * For each record, clean the name and call visit(); stop when visit
 * returns non-zero. Returns the visit return value, 0 if exhausted,
 * or -2 on I/O error.
 *
 * ISO directories are a sequence of variable-length records that never
 * cross a sector boundary — when there's no room for the next record
 * in the current sector, the rest is zero-padded and the next record
 * starts at sector + 1. A record with length=0 signals end-of-sector,
 * not end-of-directory. */
typedef int (*dir_visit_fn)(const iso_dir_rec_t *r, const char *name,
                            uint32_t name_len, void *ctx);

static int dir_iter(iso_volume_t *v, uint32_t lba, uint32_t total_size,
                    dir_visit_fn visit, void *ctx)
{
    uint32_t sectors = (total_size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;

    for (uint32_t s = 0; s < sectors; s++)
    {
        const uint8_t *sector = read_sector(v, lba + s);
        if (!sector) return -2;

        uint32_t off = 0;
        while (off < ISO_SECTOR_SIZE)
        {
            const iso_dir_rec_t *rec = (const iso_dir_rec_t *)(sector + off);
            if (rec->length == 0) break;
            if (off + rec->length > ISO_SECTOR_SIZE) break;

            const char *raw = (const char *)(sector + off + sizeof(iso_dir_rec_t));
            char cleaned[ISO_NAME_MAX];
            uint32_t clen = clean_name(raw, rec->name_len,
                                       cleaned, sizeof(cleaned));

            int rc = visit(rec, cleaned, clen, ctx);
            if (rc) return rc;

            off += rec->length;
        }
    }
    return 0;
}

/* dir_iter visitor for "find named entry". */
typedef struct
{
    const char    *name;
    uint32_t       name_len;
    iso_dir_rec_t *out;
} find_ctx_t;

static int find_visit(const iso_dir_rec_t *r, const char *name,
                      uint32_t len, void *ctx)
{
    find_ctx_t *fc = (find_ctx_t *)ctx;
    if (len != fc->name_len) return 0;
    if (icase_ncmp(name, fc->name, len) != 0) return 0;
    memcpy(fc->out, r, sizeof(*r));
    return 1;   /* found, stop iteration */
}

/* Walk `path` from the root. Fills *out with the matching record on
 * success. Returns 0 on hit, -1 on not-found, -2 on I/O error. */
static int path_walk(iso_volume_t *v, const char *path, iso_dir_rec_t *out)
{
    iso_dir_rec_t cur;
    memset(&cur, 0, sizeof(cur));
    cur.extent_le = v->root_lba;
    cur.size_le   = v->root_size;
    cur.flags     = ISO_FLAG_DIR;

    const char *p = path;
    while (*p == '/') p++;

    while (*p)
    {
        if (!(cur.flags & ISO_FLAG_DIR)) return -1;

        const char *start = p;
        while (*p && *p != '/') p++;
        uint32_t comp_len = (uint32_t)(p - start);
        if (comp_len == 0) break;

        iso_dir_rec_t child;
        find_ctx_t fc = { .name = start, .name_len = comp_len, .out = &child };
        int rc = dir_iter(v, cur.extent_le, cur.size_le, find_visit, &fc);
        if (rc < 0) return rc;
        if (rc == 0) return -1;   /* not found */

        cur = child;
        while (*p == '/') p++;
    }

    memcpy(out, &cur, sizeof(*out));
    return 0;
}

/* ===== Public API ===== */

static iso_volume_t *iso_mount(iso_rdsec_t rdsec, void *ctx)
{
    if (g_volume.used || !rdsec) return 0;

    memset(&g_volume, 0, sizeof(g_volume));
    g_volume.rdsec = rdsec;
    g_volume.ctx   = ctx;

    /* Scan the volume descriptor chain at LBA 16 for the PVD. We read
     * straight into the volume cache and mark it valid on the way. */
    uint8_t *sector = g_volume.cache;
    for (uint32_t i = 0; i < 16; i++)
    {
        uint32_t lba = ISO_PVD_LBA + i;
        if (rdsec(ctx, lba, sector) != 0) return 0;

        if (sector[1] != 'C' || sector[2] != 'D' ||
            sector[3] != '0' || sector[4] != '0' || sector[5] != '1')
            return 0;

        uint8_t type = sector[0];
        if (type == ISO_VD_TYPE_END) return 0;
        if (type != ISO_VD_TYPE_PVD) continue;

        iso_pvd_t *pvd = (iso_pvd_t *)sector;
        iso_dir_rec_t *root = (iso_dir_rec_t *)pvd->root_dir_rec;
        g_volume.root_lba   = root->extent_le;
        g_volume.root_size  = root->size_le;
        g_volume.cache_lba  = lba;
        g_volume.cache_valid = true;
        g_volume.used       = true;
        return &g_volume;
    }
    return 0;
}

static void iso_unmount(iso_volume_t *v)
{
    if (!v || !v->used) return;
    memset(v, 0, sizeof(*v));
}

static int iso_stat(iso_volume_t *v, const char *path, iso_stat_t *out)
{
    if (!v || !v->used || !path || !out) return -1;

    iso_dir_rec_t rec;
    int rc = path_walk(v, path, &rec);
    if (rc != 0) return rc;

    out->lba    = rec.extent_le;
    out->size   = rec.size_le;
    out->is_dir = (rec.flags & ISO_FLAG_DIR) != 0;
    return 0;
}

static int iso_open(iso_volume_t *v, const char *path, iso_file_t *out)
{
    if (!v || !v->used || !path || !out) return -1;

    iso_dir_rec_t rec;
    int rc = path_walk(v, path, &rec);
    if (rc != 0) return rc;
    if (rec.flags & ISO_FLAG_DIR) return -1;

    out->lba  = rec.extent_le;
    out->size = rec.size_le;
    out->pos  = 0;
    return 0;
}

static int iso_read(iso_volume_t *v, iso_file_t *f, void *buf, uint32_t n)
{
    if (!v || !v->used || !f || !buf) return -1;
    if (f->pos >= f->size) return 0;

    uint32_t avail = f->size - f->pos;
    if (n > avail) n = avail;

    uint32_t done = 0;
    uint8_t *dst  = (uint8_t *)buf;

    while (done < n)
    {
        uint32_t sec_off = f->pos / ISO_SECTOR_SIZE;
        uint32_t in_sec  = f->pos % ISO_SECTOR_SIZE;
        uint32_t chunk   = ISO_SECTOR_SIZE - in_sec;
        if (chunk > n - done) chunk = n - done;

        const uint8_t *sector = read_sector(v, f->lba + sec_off);
        if (!sector) return -1;
        memcpy(dst + done, sector + in_sec, chunk);

        done   += chunk;
        f->pos += chunk;
    }
    return (int)done;
}

/* dir_iter visitor for readdir — appends each non-dot entry into the
 * caller's array, stops when the array is full. */
typedef struct
{
    iso_dirent_t *out;
    int           max;
    int           count;
} list_ctx_t;

static int list_visit(const iso_dir_rec_t *r, const char *name,
                      uint32_t len, void *ctx)
{
    list_ctx_t *lc = (list_ctx_t *)ctx;
    if (is_dot_entry(name, len) || len == 0) return 0;
    if (lc->count >= lc->max) return 1;   /* full, stop */

    iso_dirent_t *d = &lc->out[lc->count++];
    memset(d, 0, sizeof(*d));
    uint32_t copy = len < ISO_NAME_MAX - 1 ? len : ISO_NAME_MAX - 1;
    memcpy(d->name, name, copy);
    d->name[copy] = 0;
    d->lba    = r->extent_le;
    d->size   = r->size_le;
    d->is_dir = (r->flags & ISO_FLAG_DIR) != 0;
    return 0;
}

static int iso_readdir(iso_volume_t *v, const char *path,
                       iso_dirent_t *out, int max)
{
    if (!v || !v->used || !path || !out || max <= 0) return -1;

    iso_dir_rec_t rec;
    int rc = path_walk(v, path, &rec);
    if (rc != 0) return rc;
    if (!(rec.flags & ISO_FLAG_DIR)) return -1;

    list_ctx_t lc = { .out = out, .max = max, .count = 0 };
    int it = dir_iter(v, rec.extent_le, rec.size_le, list_visit, &lc);
    if (it < 0) return -1;
    return lc.count;
}

/* ===== Exports =====
 *
 * elf_load_shared finds this by name in the dynamic symbol table and
 * returns its absolute address — the cdrom driver casts it back to
 * (iso9660_api_t *) and starts calling. */

__attribute__((visibility("default")))
iso9660_api_t __mem_exports = {
    .mount    = iso_mount,
    .unmount  = iso_unmount,
    .stat     = iso_stat,
    .open     = iso_open,
    .read     = iso_read,
    .readdir  = iso_readdir,
};
