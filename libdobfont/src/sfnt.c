/* libdobfont -- sfnt container: tables, cmap, loca, hmtx.
 *
 * Parses the OpenType/TrueType "sfnt" wrapper that both .ttf and .otf
 * use. We read only the tables the engine needs and validate offsets
 * against the blob size so a malformed font cannot walk off the buffer.
 */

#include "df_internal.h"
#include <string.h>   /* memset */
#include <stdlib.h>   /* malloc/free/realloc */

/* ====================================================================
 *  bounded big-endian readers
 * ==================================================================== */

uint8_t df_rd_u8(const df_blob *b, uint32_t off)
{
    if (off >= b->size) return 0;
    return b->base[off];
}
uint16_t df_rd_u16(const df_blob *b, uint32_t off)
{
    if (off + 2 > b->size) return 0;
    return (uint16_t)((b->base[off] << 8) | b->base[off + 1]);
}
int16_t df_rd_i16(const df_blob *b, uint32_t off)
{
    return (int16_t)df_rd_u16(b, off);
}
uint32_t df_rd_u32(const df_blob *b, uint32_t off)
{
    if (off + 4 > b->size) return 0;
    return ((uint32_t)b->base[off]     << 24) |
           ((uint32_t)b->base[off + 1] << 16) |
           ((uint32_t)b->base[off + 2] <<  8) |
            (uint32_t)b->base[off + 3];
}

/* ====================================================================
 *  path buffer
 * ==================================================================== */

void df_path_init(df_path *p) { p->seg = NULL; p->count = 0; p->cap = 0; }
void df_path_free(df_path *p) { free(p->seg); p->seg = NULL; p->count = p->cap = 0; }
void df_path_reset(df_path *p) { p->count = 0; }

df_result df_path_push(df_path *p, const df_seg *s)
{
    if (p->count == p->cap) {
        uint32_t nc = p->cap ? p->cap * 2 : 64;
        df_seg *ns = (df_seg *)realloc(p->seg, nc * sizeof(df_seg));
        if (!ns) return DF_ERR_NOMEM;
        p->seg = ns; p->cap = nc;
    }
    p->seg[p->count++] = *s;
    return DF_OK;
}

/* ====================================================================
 *  affine
 * ==================================================================== */

df_mat df_mat_id(void)
{
    df_mat m = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
    return m;
}
df_mat df_mat_mul(const df_mat *o, const df_mat *i)
{
    df_mat r;
    r.a = o->a * i->a + o->c * i->b;
    r.b = o->b * i->a + o->d * i->b;
    r.c = o->a * i->c + o->c * i->d;
    r.d = o->b * i->c + o->d * i->d;
    r.e = o->a * i->e + o->c * i->f + o->e;
    r.f = o->b * i->e + o->d * i->f + o->f;
    return r;
}

/* ====================================================================
 *  container open
 * ==================================================================== */

static void store_table(df_face *f, uint32_t tag, uint32_t off, uint32_t len)
{
    df_table t = { off, len };
    switch (tag) {
        case DF_TAG('h','e','a','d'): f->head = t; break;
        case DF_TAG('m','a','x','p'): f->maxp = t; break;
        case DF_TAG('h','h','e','a'): f->hhea = t; break;
        case DF_TAG('h','m','t','x'): f->hmtx = t; break;
        case DF_TAG('c','m','a','p'): f->cmap = t; break;
        case DF_TAG('l','o','c','a'): f->loca = t; break;
        case DF_TAG('g','l','y','f'): f->glyf = t; break;
        case DF_TAG('C','F','F',' '): f->cff  = t; break;
        case DF_TAG('k','e','r','n'): f->kern = t; break;
        case DF_TAG('O','S','/','2'): f->os2  = t; break;
        default: break;
    }
}

/* Rank a cmap (platform,encoding) pair: higher = preferred. */
static int cmap_rank(uint16_t plat, uint16_t enc)
{
    if (plat == 3 && enc == 10) return 6;   /* Windows UCS-4   */
    if (plat == 0 && enc == 6)  return 6;    /* Unicode full    */
    if (plat == 0 && enc == 4)  return 5;    /* Unicode 2.0+    */
    if (plat == 3 && enc == 1)  return 4;    /* Windows BMP     */
    if (plat == 0 && enc == 3)  return 4;    /* Unicode BMP     */
    if (plat == 0)              return 3;    /* other Unicode   */
    if (plat == 3 && enc == 0)  return 1;    /* Windows symbol  */
    return 0;
}

/* Pick the best supported cmap subtable (format 4 or 12). */
static void select_cmap(df_face *f)
{
    f->cmap_sub = 0; f->cmap_format = 0;
    if (!f->cmap.off) return;

    uint32_t base = f->cmap.off;
    uint16_t n    = df_rd_u16(&f->blob, base + 2);

    int best_rank = -1; uint32_t best_sub = 0; uint8_t best_fmt = 0;
    for (uint16_t i = 0; i < n; i++) {
        uint32_t rec  = base + 4 + (uint32_t)i * 8;
        uint16_t plat = df_rd_u16(&f->blob, rec);
        uint16_t enc  = df_rd_u16(&f->blob, rec + 2);
        uint32_t sub  = base + df_rd_u32(&f->blob, rec + 4);
        uint16_t fmt  = df_rd_u16(&f->blob, sub);
        if (fmt != 4 && fmt != 12) continue;

        int rank = cmap_rank(plat, enc);
        /* prefer wider coverage (format 12) on ties */
        if (rank > best_rank || (rank == best_rank && fmt == 12 && best_fmt != 12)) {
            best_rank = rank; best_sub = sub; best_fmt = (uint8_t)fmt;
        }
    }
    f->cmap_sub = best_sub; f->cmap_format = best_fmt;
}

df_result df_sfnt_open(df_face *f, const void *data, uint32_t size,
                       uint32_t face_index)
{
    memset(f, 0, sizeof(*f));
    f->blob.base = (const uint8_t *)data;
    f->blob.size = size;

    if (size < 12) return DF_ERR_BADFONT;

    /* TrueType Collection: pick the face_index'th offset table. */
    uint32_t sfnt_off = 0;
    if (df_rd_u32(&f->blob, 0) == DF_TAG('t','t','c','f')) {
        uint32_t num = df_rd_u32(&f->blob, 8);
        if (face_index >= num) return DF_ERR_INVAL;
        sfnt_off = df_rd_u32(&f->blob, 12 + face_index * 4);
    }

    uint32_t ver = df_rd_u32(&f->blob, sfnt_off);
    if (ver != 0x00010000u &&
        ver != DF_TAG('t','r','u','e') &&
        ver != DF_TAG('O','T','T','O'))
        return DF_ERR_BADFONT;

    uint16_t num_tables = df_rd_u16(&f->blob, sfnt_off + 4);
    for (uint16_t i = 0; i < num_tables; i++) {
        uint32_t rec = sfnt_off + 12 + (uint32_t)i * 16;
        uint32_t tag = df_rd_u32(&f->blob, rec);
        uint32_t off = df_rd_u32(&f->blob, rec + 8);
        uint32_t len = df_rd_u32(&f->blob, rec + 12);
        if (off > size) continue;                 /* drop bogus tables */
        store_table(f, tag, off, len);
    }

    if (!f->head.off || !f->maxp.off || !f->hhea.off || !f->hmtx.off)
        return DF_ERR_BADFONT;

    f->units_per_em  = df_rd_u16(&f->blob, f->head.off + 18);
    f->index_to_loc  = df_rd_i16(&f->blob, f->head.off + 50);
    f->num_glyphs    = df_rd_u16(&f->blob, f->maxp.off + 4);
    f->ascent        = df_rd_i16(&f->blob, f->hhea.off + 4);
    f->descent       = df_rd_i16(&f->blob, f->hhea.off + 6);
    f->line_gap      = df_rd_i16(&f->blob, f->hhea.off + 8);
    f->num_h_metrics = df_rd_u16(&f->blob, f->hhea.off + 34);

    if (f->units_per_em == 0) return DF_ERR_BADFONT;

    select_cmap(f);

    /* TrueType outlines require glyf + loca. A CFF face is wired up
     * separately by df_open (which calls df_cff_open). */
    if (f->glyf.off != 0 && f->loca.off != 0) {
        f->outline_kind = DF_OUTLINE_GLYF;
        f->has_outlines = true;
    } else {
        f->outline_kind = DF_OUTLINE_NONE;
        f->has_outlines = false;
    }

    return DF_OK;
}

/* ====================================================================
 *  hmtx advance
 * ==================================================================== */

uint32_t df_sfnt_advance(const df_face *f, uint32_t gid)
{
    if (f->num_h_metrics == 0) return 0;
    uint32_t idx = (gid < f->num_h_metrics) ? gid : (uint32_t)(f->num_h_metrics - 1);
    return df_rd_u16(&f->blob, f->hmtx.off + idx * 4);
}

/* ====================================================================
 *  loca glyph range
 * ==================================================================== */

bool df_sfnt_glyph_range(const df_face *f, uint32_t gid,
                         uint32_t *off, uint32_t *len)
{
    if (f->outline_kind != DF_OUTLINE_GLYF || gid >= f->num_glyphs) return false;

    uint32_t o0, o1;
    if (f->index_to_loc == 0) {                 /* short: offsets/2 */
        o0 = (uint32_t)df_rd_u16(&f->blob, f->loca.off + gid * 2) * 2;
        o1 = (uint32_t)df_rd_u16(&f->blob, f->loca.off + (gid + 1) * 2) * 2;
    } else {                                     /* long */
        o0 = df_rd_u32(&f->blob, f->loca.off + gid * 4);
        o1 = df_rd_u32(&f->blob, f->loca.off + (gid + 1) * 4);
    }
    if (o1 <= o0) return false;                  /* empty (blank) glyph */

    *off = f->glyf.off + o0;
    *len = o1 - o0;
    return true;
}

/* ====================================================================
 *  cmap lookup (format 4 and 12)
 * ==================================================================== */

static uint32_t cmap4_lookup(const df_face *f, uint32_t base, uint32_t cp)
{
    if (cp > 0xFFFF) return 0;                    /* format 4 is BMP only */

    uint16_t seg2  = df_rd_u16(&f->blob, base + 6);
    uint16_t segc  = seg2 / 2;
    uint32_t endO  = base + 14;
    uint32_t startO = base + 16 + (uint32_t)seg2;       /* after reservedPad */
    uint32_t deltaO = startO + seg2;
    uint32_t rangeO = deltaO + seg2;

    for (uint16_t i = 0; i < segc; i++) {
        uint16_t end = df_rd_u16(&f->blob, endO + i * 2);
        if (cp > end) continue;
        uint16_t start = df_rd_u16(&f->blob, startO + i * 2);
        if (cp < start) return 0;

        int16_t  delta = df_rd_i16(&f->blob, deltaO + i * 2);
        uint16_t range = df_rd_u16(&f->blob, rangeO + i * 2);
        if (range == 0)
            return (uint16_t)(cp + delta);

        uint32_t gi_addr = rangeO + i * 2 + range + (cp - start) * 2;
        uint16_t g = df_rd_u16(&f->blob, gi_addr);
        if (g == 0) return 0;
        return (uint16_t)(g + delta);
    }
    return 0;
}

static uint32_t cmap12_lookup(const df_face *f, uint32_t base, uint32_t cp)
{
    uint32_t ngroups = df_rd_u32(&f->blob, base + 12);
    uint32_t lo = 0, hi = ngroups;               /* binary search over groups */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t g   = base + 16 + mid * 12;
        uint32_t s   = df_rd_u32(&f->blob, g);
        uint32_t e   = df_rd_u32(&f->blob, g + 4);
        if (cp < s)        hi = mid;
        else if (cp > e)   lo = mid + 1;
        else {
            uint32_t sg = df_rd_u32(&f->blob, g + 8);
            return sg + (cp - s);
        }
    }
    return 0;
}

uint32_t df_sfnt_cmap_lookup(const df_face *f, uint32_t cp)
{
    if (!f->cmap_sub) return 0;
    if (f->cmap_format == 12) return cmap12_lookup(f, f->cmap_sub, cp);
    if (f->cmap_format == 4)  return cmap4_lookup(f, f->cmap_sub, cp);
    return 0;
}

/* ====================================================================
 *  style (OS/2 usWeightClass, italic from fsSelection or head.macStyle)
 * ==================================================================== */

uint16_t df_sfnt_weight(const df_face *f)
{
    if (f->os2.off) {
        uint16_t w = df_rd_u16(&f->blob, f->os2.off + 4);   /* usWeightClass */
        if (w) return w;
    }
    return 400;
}

bool df_sfnt_italic(const df_face *f)
{
    if (f->os2.off)
        return (df_rd_u16(&f->blob, f->os2.off + 62) & 0x01) != 0;   /* fsSelection ITALIC */
    if (f->head.off)
        return (df_rd_u16(&f->blob, f->head.off + 44) & 0x02) != 0;  /* macStyle italic    */
    return false;
}
