/* libdobfont -- monobit / bitmap face backend.
 *
 * Renders precomputed 1-bit glyphs: no outlines, no curve rasterization,
 * no anti-aliasing.  df_rasterize expands a glyph's bits to a 0/255
 * coverage bitmap, integer-scaled to the requested pixel size, so a
 * bitmap font stays crisp (nearest-neighbour) instead of being smeared.
 * Exposes the same metrics + rasterize entry points as the sfnt backend,
 * so the layout/page pipeline treats a monobit font like any other.
 *
 * File format "DMF1" (Dob Mono Font v1), little-endian:
 *
 *   off  size  field
 *    0    4    magic 'D','M','F','1'
 *    4    2    cell_w        glyph cell width  (px)
 *    6    2    cell_h        glyph cell height (px) == em
 *    8    2    first_cp      first code point covered
 *   10    2    count         number of glyphs (cps first_cp..first_cp+count-1)
 *   12    2    ascent        baseline distance from cell top (px)
 *   14    2    flags         bit0: a per-glyph advance table follows
 *   16    2    fixed_adv     advance when not proportional (0 => cell_w)
 *   18    2    reserved      (0)
 *   20  count  [if flags&1] per-glyph advance bytes
 *    .  ...    glyph bitmaps: count * (stride * cell_h) bytes,
 *              stride = (cell_w + 7) / 8, 1bpp, MSB-first, rows top->bottom
 *
 * Glyph ids: 0 is .notdef (blank); code point cp maps to gid
 * (cp - first_cp) + 1, so the bitmap for gid g is glyph index g-1.
 */

#include "df_internal.h"
#include <string.h>
#include <stdlib.h>

struct df_mono
{
    const uint8_t *data;      /* into the caller-owned blob */
    uint32_t       size;
    uint16_t cell_w, cell_h;
    uint16_t first_cp, count;
    uint16_t ascent;
    uint16_t flags;
    uint16_t fixed_adv;
    uint32_t stride;          /* bytes per glyph row */
    const uint8_t *adv;       /* count bytes or NULL */
    const uint8_t *bitmaps;   /* count * stride * cell_h bytes */
};

static uint16_t rd16(const uint8_t *p, uint32_t o)
{
    return (uint16_t)((uint16_t)p[o] | ((uint16_t)p[o + 1] << 8));
}

/* nearest integer scale so cell_h px maps onto the requested em size */
static int mono_scale(const struct df_mono *m, float px)
{
    int s = (int)(px / (float)m->cell_h + 0.5f);
    if (s < 1) s = 1;
    if (s > 16) s = 16;
    return s;
}

static int mono_advance_px(const struct df_mono *m, uint32_t gid)
{
    int a = m->fixed_adv ? (int)m->fixed_adv : (int)m->cell_w;
    if ((m->flags & 1) && gid >= 1 && gid <= m->count)
    {
        int pg = m->adv[gid - 1];
        if (pg) a = pg;
    }
    return a;
}

df_result df_mono_open(df_face *f, const void *data, uint32_t size)
{
    if (!f || !data || size < 20) return DF_ERR_BADFONT;
    const uint8_t *p = (const uint8_t *)data;
    if (!(p[0] == 'D' && p[1] == 'M' && p[2] == 'F' && p[3] == '1'))
        return DF_ERR_BADFONT;

    struct df_mono *m = (struct df_mono *)calloc(1, sizeof(*m));
    if (!m) return DF_ERR_NOMEM;

    m->data      = p;
    m->size      = size;
    m->cell_w    = rd16(p, 4);
    m->cell_h    = rd16(p, 6);
    m->first_cp  = rd16(p, 8);
    m->count     = rd16(p, 10);
    m->ascent    = rd16(p, 12);
    m->flags     = rd16(p, 14);
    m->fixed_adv = rd16(p, 16);

    if (m->cell_w == 0 || m->cell_h == 0 || m->count == 0 ||
        m->ascent > m->cell_h)
    {
        free(m);
        return DF_ERR_BADFONT;
    }

    m->stride = ((uint32_t)m->cell_w + 7u) / 8u;

    uint32_t off = 20;
    if (m->flags & 1)
    {
        if (off + m->count > size) { free(m); return DF_ERR_BADFONT; }
        m->adv = p + off;
        off += m->count;
    }
    uint32_t bmlen = (uint32_t)m->count * m->stride * m->cell_h;
    if (off + bmlen > size) { free(m); return DF_ERR_BADFONT; }
    m->bitmaps = p + off;

    /* fields the shared (kind-agnostic) code reads */
    f->kind         = DF_FACE_MONO;
    f->monop        = m;
    f->units_per_em = m->cell_h;
    f->num_glyphs   = (uint16_t)(m->count + 1);
    f->ascent       = (int16_t)m->ascent;
    f->descent      = (int16_t)((int)m->ascent - (int)m->cell_h);
    f->line_gap     = 0;
    f->has_outlines = false;
    f->outline_kind = DF_OUTLINE_NONE;
    return DF_OK;
}

void df_mono_close(df_face *f)
{
    if (!f || !f->monop) return;
    free(f->monop);
    f->monop = NULL;
}

uint32_t df_mono_cmap_lookup(const df_face *f, uint32_t cp)
{
    const struct df_mono *m = f ? f->monop : NULL;
    if (!m) return 0;
    if (cp >= m->first_cp && cp < (uint32_t)m->first_cp + m->count)
        return cp - m->first_cp + 1;            /* gid 0 reserved = .notdef */
    return 0;
}

df_result df_mono_vmetrics(const df_face *f, float px_size, df_vmetrics *out)
{
    const struct df_mono *m = f ? f->monop : NULL;
    if (!m || !out || px_size <= 0.0f) return DF_ERR_INVAL;
    int s = mono_scale(m, px_size);
    out->ascent      = (float)((int)m->ascent * s);
    out->descent     = (float)(((int)m->ascent - (int)m->cell_h) * s);
    out->line_gap    = 0.0f;
    out->line_height = (float)((int)m->cell_h * s);
    return DF_OK;
}

df_result df_mono_glyph_metrics(const df_face *f, uint32_t gid, float px_size,
                                df_gmetrics *out)
{
    const struct df_mono *m = f ? f->monop : NULL;
    if (!m || !out || px_size <= 0.0f) return DF_ERR_INVAL;
    int s = mono_scale(m, px_size);
    out->advance = (float)(mono_advance_px(m, gid) * s);
    out->lsb     = 0.0f;
    if (gid == 0 || gid > m->count)         /* .notdef / out of range: blank */
    {
        out->x0 = out->y0 = out->x1 = out->y1 = 0.0f;
        return DF_OK;
    }
    out->x0 = 0.0f;
    out->x1 = (float)((int)m->cell_w * s);
    out->y1 = (float)((int)m->ascent * s);                       /* top, y up   */
    out->y0 = (float)(((int)m->ascent - (int)m->cell_h) * s);    /* bottom (<=0) */
    return DF_OK;
}

df_result df_mono_rasterize(const df_face *f, uint32_t gid,
                            const df_raster_req *req, df_bitmap *out)
{
    const struct df_mono *m = f ? f->monop : NULL;
    if (!m || !req || !out || req->px_size <= 0.0f) return DF_ERR_INVAL;

    int s = mono_scale(m, req->px_size);
    int adv = mono_advance_px(m, gid) * s;

    memset(out, 0, sizeof(*out));
    out->advance = (float)adv;

    if (gid == 0 || gid > m->count) return DF_OK;   /* blank glyph */

    int W = (int)m->cell_w * s, H = (int)m->cell_h * s;
    uint8_t *cov = (uint8_t *)calloc((size_t)W * H, 1);
    if (!cov) return DF_ERR_NOMEM;

    const uint8_t *gb = m->bitmaps + (size_t)(gid - 1) * m->stride * m->cell_h;
    for (int sy = 0; sy < (int)m->cell_h; sy++)
    {
        const uint8_t *row = gb + (size_t)sy * m->stride;
        for (int sx = 0; sx < (int)m->cell_w; sx++)
        {
            if (!((row[sx >> 3] >> (7 - (sx & 7))) & 1)) continue;
            for (int dy = 0; dy < s; dy++)
            {
                uint8_t *d = cov + (size_t)(sy * s + dy) * W + (size_t)sx * s;
                for (int dx = 0; dx < s; dx++) d[dx] = 255;
            }
        }
    }

    /* synthetic bold: OR the glyph onto itself shifted right by s px */
    if (req->embolden >= 0.5f)
    {
        for (int y = 0; y < H; y++)
        {
            uint8_t *r = cov + (size_t)y * W;
            for (int x = W - 1; x >= s; x--)
                if (r[x - s]) r[x] = 255;
        }
    }

    out->cover   = cov;
    out->w       = W;
    out->h       = H;
    out->left    = 0;                          /* left bearing 0 (v1) */
    out->top     = (int)m->ascent * s;         /* baseline is ascent px below top */
    out->advance = (float)adv;
    return DF_OK;
}
