/* libdobfont -- sfnt/glyf backend: metrics + rasterization.
 *
 * Ties sfnt (advances, glyph headers) + outline (glyf decode) + raster
 * (coverage) together. This is the "outline-sfnt" face backend from the
 * modular design; a CFF backend would expose the same two entry points
 * over the 'CFF ' table.
 *
 * Synthetic styling: oblique slant is exact (a shear folded into the
 * device transform). Emboldening here is a first-cut coverage dilation
 * -- it thickens strokes but is not a true outline offset; a designed
 * bold face is always preferable. Both are opt-in via df_raster_req.
 */

#include "df_internal.h"
#include <string.h>
#include <stdlib.h>

#define DF_MAX_BITMAP 4096   /* refuse absurd sizes from bad input */

/* points actually used by each segment kind */
static int seg_pts(df_segkind k)
{
    switch (k) {
        case DF_MOVE: case DF_LINE: return 1;
        case DF_QUAD:               return 2;
        case DF_CUBIC:              return 3;
        default:                    return 0;   /* DF_CLOSE */
    }
}

/* ====================================================================
 *  metrics
 * ==================================================================== */

df_result df_sfnt_glyph_metrics(const df_face *f, uint32_t gid, float px_size,
                           df_gmetrics *out)
{
    if (!f || !out || px_size <= 0.0f) return DF_ERR_INVAL;
    if (gid >= f->num_glyphs)          return DF_ERR_NOGLYPH;

    memset(out, 0, sizeof(*out));
    float scale = px_size / (float)f->units_per_em;

    out->advance = (float)df_sfnt_advance(f, gid) * scale;

    if (f->outline_kind == DF_OUTLINE_GLYF) {
        uint32_t goff, glen;
        if (df_sfnt_glyph_range(f, gid, &goff, &glen)) {   /* glyf header bbox (fast) */
            out->x0 = (float)df_rd_i16(&f->blob, goff + 2) * scale;
            out->y0 = (float)df_rd_i16(&f->blob, goff + 4) * scale;
            out->x1 = (float)df_rd_i16(&f->blob, goff + 6) * scale;
            out->y1 = (float)df_rd_i16(&f->blob, goff + 8) * scale;
        }
    } else if (f->outline_kind == DF_OUTLINE_CFF) {
        df_path path; df_path_init(&path);                 /* CFF: box from the outline */
        if (df_outline_decode(f, gid, &path) == DF_OK && path.count) {
            bool any = false; float minx = 0, maxx = 0, miny = 0, maxy = 0;
            for (uint32_t i = 0; i < path.count; i++) {
                const df_seg *s = &path.seg[i];
                int np = seg_pts(s->k);
                for (int j = 0; j < np; j++) {
                    float x = s->x[j], y = s->y[j];
                    if (!any) { minx = maxx = x; miny = maxy = y; any = true; }
                    else { if (x < minx) minx = x; else if (x > maxx) maxx = x;
                           if (y < miny) miny = y; else if (y > maxy) maxy = y; }
                }
            }
            if (any) { out->x0 = minx*scale; out->y0 = miny*scale;
                       out->x1 = maxx*scale; out->y1 = maxy*scale; }
        }
        df_path_free(&path);
    }
    out->lsb = out->x0;   /* close enough for layout; exact lsb is in hmtx */
    return DF_OK;
}

/* ====================================================================
 *  emboldening (approximate, separable coverage dilation)
 * ==================================================================== */

/* Synthetic emboldening by sub-pixel coverage spreading.
 *
 * The old approach was a separable MAX dilation with an integer radius of
 * ceil(embolden): even the minimum radius 1 grew every stroke by a full pixel
 * on each side in BOTH axes, which bloated stems and -- worse -- closed the
 * counters (the holes in e, a, o), turning bold text into blobs.
 *
 * Instead we spread coverage by a *fractional* amount and bias it horizontally:
 * stems (vertical strokes) get the full weight `ex`, while the vertical spread
 * `ey` is kept small so horizontal bars thicken only a little and the counters
 * stay open. Fractional neighbours contribute weighted coverage, so the result
 * stays anti-aliased rather than turning hard-edged. */
static void embolden_cov(uint8_t *cov, int w, int h, float ex, float ey)
{
    if (ex <= 0.0f && ey <= 0.0f) return;
    uint8_t *tmp = (uint8_t *)malloc((size_t)w * (size_t)h);
    if (!tmp) return;

    int rx = (int)ex; float fx = ex - (float)rx;          /* horizontal: full stems */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int m = cov[(size_t)y * w + x];
            for (int d = 1; d <= rx; d++) {
                int a = x - d, b = x + d;
                if (a >= 0 && cov[(size_t)y * w + a] > m) m = cov[(size_t)y * w + a];
                if (b <  w && cov[(size_t)y * w + b] > m) m = cov[(size_t)y * w + b];
            }
            if (fx > 0.0f) {
                int a = x - rx - 1, b = x + rx + 1;
                int va = (a >= 0) ? (int)(cov[(size_t)y * w + a] * fx) : 0;
                int vb = (b <  w) ? (int)(cov[(size_t)y * w + b] * fx) : 0;
                if (va > m) m = va;
                if (vb > m) m = vb;
            }
            tmp[(size_t)y * w + x] = (uint8_t)m;
        }

    int ry = (int)ey; float fy = ey - (float)ry;          /* vertical: gentle, keep counters open */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int m = tmp[(size_t)y * w + x];
            for (int d = 1; d <= ry; d++) {
                int a = y - d, b = y + d;
                if (a >= 0 && tmp[(size_t)a * w + x] > m) m = tmp[(size_t)a * w + x];
                if (b <  h && tmp[(size_t)b * w + x] > m) m = tmp[(size_t)b * w + x];
            }
            if (fy > 0.0f) {
                int a = y - ry - 1, b = y + ry + 1;
                int va = (a >= 0) ? (int)(tmp[(size_t)a * w + x] * fy) : 0;
                int vb = (b <  h) ? (int)(tmp[(size_t)b * w + x] * fy) : 0;
                if (va > m) m = va;
                if (vb > m) m = vb;
            }
            cov[(size_t)y * w + x] = (uint8_t)m;
        }

    /* Mid-tone intensity lift. Synthetic bold isn't only about thickening: the
     * anti-aliased edge pixels carry partial coverage (grey), and lifting those
     * toward opaque makes the stroke read dense and crisp without spreading the
     * ink further (which would swallow the counters). The mainDOB kernel ships
     * an FPU, so we spend it here: a parabola u = t + s*t*(1-t) that fixes 0 and
     * 1, peaks at half-coverage, and uses only multiplies -- no libm. */
    {
        const float s = 0.50f;
        size_t n = (size_t)w * (size_t)h;
        for (size_t i = 0; i < n; i++) {
            float t = cov[i] / 255.0f;
            float u = t + s * t * (1.0f - t);
            if (u > 1.0f) u = 1.0f;
            cov[i] = (uint8_t)(u * 255.0f + 0.5f);
        }
    }
    free(tmp);
}

/* ====================================================================
 *  rasterization
 * ==================================================================== */

df_result df_sfnt_rasterize(const df_face *f, uint32_t gid,
                       const df_raster_req *req, df_bitmap *out)
{
    if (!f || !req || !out)            return DF_ERR_INVAL;
    if (req->px_size <= 0.0f)          return DF_ERR_INVAL;
    if (gid >= f->num_glyphs)          return DF_ERR_NOGLYPH;
    if (!f->has_outlines)              return DF_ERR_NOOUTLINE;

    memset(out, 0, sizeof(*out));
    float scale = req->px_size / (float)f->units_per_em;
    float slant = req->slant;
    out->advance = (float)df_sfnt_advance(f, gid) * scale;

    /* decode glyph outline in font units (y up) */
    df_path path; df_path_init(&path);
    df_result r = df_outline_decode(f, gid, &path);
    if (r) { df_path_free(&path); return r; }

    if (path.count == 0) { df_path_free(&path); return DF_OK; }   /* blank */

    /* device-space extents (apply shear + scale; controls included -> a
     * conservative box that always contains the ink) */
    bool any = false;
    float minx = 0, maxx = 0, miny = 0, maxy = 0;
    for (uint32_t i = 0; i < path.count; i++) {
        const df_seg *s = &path.seg[i];
        int np = seg_pts(s->k);
        for (int j = 0; j < np; j++) {
            float fx = (s->x[j] + slant * s->y[j]) * scale;
            float fy = s->y[j] * scale;
            if (!any) { minx = maxx = fx; miny = maxy = fy; any = true; }
            else {
                if (fx < minx) minx = fx; else if (fx > maxx) maxx = fx;
                if (fy < miny) miny = fy; else if (fy > maxy) maxy = fy;
            }
        }
    }
    if (!any) { df_path_free(&path); return DF_OK; }

    int pad   = 1 + ((req->embolden > 0.0f) ? (int)df_ceilf(req->embolden) : 0);
    int left  = (int)df_floorf(minx) - pad;
    int right = (int)df_ceilf (maxx) + pad;
    int top   = (int)df_ceilf (maxy) + pad;    /* pixels above baseline */
    int bot   = (int)df_floorf(miny) - pad;
    int w = right - left;
    int h = top - bot;

    if (w <= 0 || h <= 0) { df_path_free(&path); return DF_OK; }      /* degenerate -> blank */
    if (w > DF_MAX_BITMAP || h > DF_MAX_BITMAP) { df_path_free(&path); return DF_ERR_RANGE; }

    /* map the path into device pixels (y down): row 0 = `top` px above baseline */
    df_path dev; df_path_init(&dev);
    for (uint32_t i = 0; i < path.count && !r; i++) {
        const df_seg *s = &path.seg[i];
        df_seg d; d.k = s->k; d.x[0]=d.y[0]=d.x[1]=d.y[1]=d.x[2]=d.y[2]=0.0f;
        int np = seg_pts(s->k);
        for (int j = 0; j < np; j++) {
            d.x[j] = (s->x[j] + slant * s->y[j]) * scale - (float)left + req->shift_x;
            d.y[j] = (float)top - s->y[j] * scale;
        }
        r = df_path_push(&dev, &d);
    }
    df_path_free(&path);
    if (r) { df_path_free(&dev); return r; }

    uint8_t *cover = (uint8_t *)malloc((size_t)w * (size_t)h);
    if (!cover) { df_path_free(&dev); return DF_ERR_NOMEM; }

    r = df_raster_fill(&dev, w, h, cover);
    df_path_free(&dev);
    if (r) { free(cover); return r; }

    if (req->embolden > 0.0f) embolden_cov(cover, w, h, req->embolden, req->embolden * 0.35f);

    out->cover = cover;
    out->w = w; out->h = h;
    out->left = left;
    out->top  = top;
    return DF_OK;
}
