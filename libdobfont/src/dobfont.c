/* libdobfont -- public entry points (open/close, queries, mapping).
 *
 * df_glyph_metrics and df_rasterize live in the sfnt backend; the rest
 * of the public surface is here. With one backend today this file is
 * thin; when a second outline format lands, df_open picks the backend
 * and these dispatch through it.
 */

#include "df_internal.h"
#include <string.h>
#include <stdlib.h>

df_result df_open(const void *data, uint32_t size, uint32_t face_index,
                  df_face **out)
{
    if (!data || !out || size == 0) return DF_ERR_INVAL;

    df_face *f = (df_face *)malloc(sizeof(df_face));
    if (!f) return DF_ERR_NOMEM;
    memset(f, 0, sizeof(*f));               /* kind = DF_FACE_SFNT, monop = NULL */

    /* Monobit (bitmap) font: detected by its 'DMF1' magic. */
    const uint8_t *p = (const uint8_t *)data;
    if (size >= 4 && p[0] == 'D' && p[1] == 'M' && p[2] == 'F' && p[3] == '1')
    {
        df_result r = df_mono_open(f, data, size);
        if (r) { free(f); return r; }
        *out = f;
        return DF_OK;
    }

    df_result r = df_sfnt_open(f, data, size, face_index);
    if (r) { free(f); return r; }
    f->kind = DF_FACE_SFNT;

    /* A PostScript-flavoured .otf has no glyf/loca but a 'CFF ' table;
     * wire up the CFF backend. Failure is non-fatal -- the face still
     * opens for metrics/cmap, just without rasterizable outlines. */
    if (f->outline_kind == DF_OUTLINE_NONE && f->cff.off)
        (void)df_cff_open(f);

    *out = f;
    return DF_OK;
}

void df_close(df_face *f)
{
    if (!f) return;
    if (f->kind == DF_FACE_MONO) df_mono_close(f);  /* frees bitmap-face state */
    else                         df_cff_close(f);   /* frees CFF state if present */
    free(f);                      /* the font blob is owned by the caller */
}

uint16_t df_units_per_em(const df_face *f) { return f ? f->units_per_em : 0; }
uint16_t df_num_glyphs  (const df_face *f) { return f ? f->num_glyphs   : 0; }
bool     df_has_outlines(const df_face *f) { return f ? f->has_outlines : false; }

df_result df_face_vmetrics(const df_face *f, float px_size, df_vmetrics *out)
{
    if (!f || !out)           return DF_ERR_INVAL;
    if (px_size <= 0.0f)      return DF_ERR_INVAL;
    if (f->kind == DF_FACE_MONO) return df_mono_vmetrics(f, px_size, out);

    float scale = px_size / (float)f->units_per_em;
    out->ascent      = (float)f->ascent   * scale;
    out->descent     = (float)f->descent  * scale;
    out->line_gap    = (float)f->line_gap * scale;
    out->line_height = (float)(f->ascent - f->descent + f->line_gap) * scale;
    return DF_OK;
}

uint32_t df_map_codepoint(const df_face *f, uint32_t cp)
{
    if (!f) return 0;
    if (f->kind == DF_FACE_MONO) return df_mono_cmap_lookup(f, cp);
    return df_sfnt_cmap_lookup(f, cp);
}

df_result df_outline_decode(const df_face *f, uint32_t gid, df_path *p)
{
    switch (f->outline_kind) {
        case DF_OUTLINE_GLYF: return df_glyf_decode(f, gid, p);
        case DF_OUTLINE_CFF:  return df_cff_decode(f, gid, p);
        default:              return DF_OK;   /* no outlines -> empty path */
    }
}

uint16_t df_face_weight(const df_face *f)    { return !f ? 400 : (f->kind == DF_FACE_MONO ? 400   : df_sfnt_weight(f)); }
bool     df_face_is_italic(const df_face *f) { return !f ? false : (f->kind == DF_FACE_MONO ? false : df_sfnt_italic(f)); }

void df_free_bitmap(df_bitmap *bm)
{
    if (!bm) return;
    free(bm->cover);
    memset(bm, 0, sizeof(*bm));
}

/* ---- per-glyph metrics + rasterization: dispatch to the face's backend ---- */
df_result df_glyph_metrics(const df_face *f, uint32_t gid, float px_size,
                           df_gmetrics *out)
{
    if (!f || !out) return DF_ERR_INVAL;
    if (f->kind == DF_FACE_MONO) return df_mono_glyph_metrics(f, gid, px_size, out);
    return df_sfnt_glyph_metrics(f, gid, px_size, out);
}

df_result df_rasterize(const df_face *f, uint32_t gid,
                       const df_raster_req *req, df_bitmap *out)
{
    if (!f || !req || !out) return DF_ERR_INVAL;
    if (f->kind == DF_FACE_MONO) return df_mono_rasterize(f, gid, req, out);
    return df_sfnt_rasterize(f, gid, req, out);
}
