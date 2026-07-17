/* libdobfont -- glyf outline decoder (TrueType outlines).
 *
 * Turns a glyph's 'glyf' record into a font-unit path (y up). Handles
 * simple glyphs (with the on/off-curve quadratic reconstruction the
 * format requires, including implied on-curve midpoints between two
 * consecutive off-curve control points and contours that begin on an
 * off-curve point) and composite glyphs (recursive components with
 * 2x2 + translation transforms).
 *
 * Covers .ttf and TrueType-flavoured .otf. CFF/Type2 outlines (the
 * other .otf flavour) are a separate backend; a CFF face has no glyf
 * table, so df_has_outlines() is false for it and we are never called.
 */

#include "df_internal.h"
#include <string.h>
#include <stdlib.h>

#define DF_MAX_COMPONENT_DEPTH 8
#define DF_MAX_POINTS          20000   /* sanity cap against malformed fonts */

/* simple-glyph flag bits */
#define GF_ON_CURVE   0x01
#define GF_X_SHORT    0x02
#define GF_Y_SHORT    0x04
#define GF_REPEAT     0x08
#define GF_X_SAME_POS 0x10
#define GF_Y_SAME_POS 0x20

/* composite-glyph flag bits */
#define CF_ARG_WORDS    0x0001
#define CF_ARGS_XY      0x0002
#define CF_HAVE_SCALE   0x0008
#define CF_MORE         0x0020
#define CF_XY_SCALE     0x0040
#define CF_2X2          0x0080

static float f2dot14(int16_t v) { return (float)v / 16384.0f; }

/* ---- transformed segment emitters ---- */

static df_result emit_seg(df_path *p, const df_mat *m, df_segkind k,
                          float x0, float y0, float x1, float y1)
{
    df_seg s;
    s.k = k;
    s.x[0] = m->a * x0 + m->c * y0 + m->e;
    s.y[0] = m->b * x0 + m->d * y0 + m->f;
    s.x[1] = m->a * x1 + m->c * y1 + m->e;
    s.y[1] = m->b * x1 + m->d * y1 + m->f;
    s.x[2] = s.y[2] = 0.0f;
    return df_path_push(p, &s);
}
static df_result emit_move(df_path *p, const df_mat *m, float x, float y)
{ return emit_seg(p, m, DF_MOVE, x, y, 0, 0); }
static df_result emit_line(df_path *p, const df_mat *m, float x, float y)
{ return emit_seg(p, m, DF_LINE, x, y, 0, 0); }
static df_result emit_quad(df_path *p, const df_mat *m,
                           float cx, float cy, float ex, float ey)
{ return emit_seg(p, m, DF_QUAD, cx, cy, ex, ey); }
static df_result emit_close(df_path *p)
{ df_seg s; memset(&s, 0, sizeof(s)); s.k = DF_CLOSE; return df_path_push(p, &s); }

/* Emit one contour (point range [start,end]) with TrueType quadratics. */
static df_result emit_contour(df_path *p, const df_mat *m,
                              const float *X, const float *Y, const uint8_t *ON,
                              int start, int end)
{
    int n = end - start + 1;
    if (n <= 0) return DF_OK;

    /* choose a starting on-curve anchor; synthesize one if none exists */
    int   k0 = -1;
    for (int k = start; k <= end; k++) if (ON[k]) { k0 = k; break; }

    float ax, ay; int first; bool synth;
    if (k0 >= 0) { ax = X[k0]; ay = Y[k0]; first = k0; synth = false; }
    else {
        ax = (X[start] + X[end]) * 0.5f;
        ay = (Y[start] + Y[end]) * 0.5f;
        first = start; synth = true;
    }

    df_result r = emit_move(p, m, ax, ay);
    if (r) return r;

    bool prev_off = false; float cx = 0, cy = 0;
    int begin = synth ? 0 : 1;            /* skip the real anchor itself */
    for (int i = begin; i < n; i++) {
        int   idx = start + (((first - start) + i) % n);
        bool  on  = ON[idx] != 0;
        float x   = X[idx], y = Y[idx];

        if (on) {
            r = prev_off ? emit_quad(p, m, cx, cy, x, y) : emit_line(p, m, x, y);
            if (r) return r;
            prev_off = false;
        } else if (prev_off) {
            float mx = (cx + x) * 0.5f, my = (cy + y) * 0.5f;   /* implied on-curve */
            r = emit_quad(p, m, cx, cy, mx, my);
            if (r) return r;
            cx = x; cy = y;
        } else {
            cx = x; cy = y; prev_off = true;
        }
    }
    if (prev_off) { r = emit_quad(p, m, cx, cy, ax, ay); if (r) return r; }
    return emit_close(p);
}

static df_result decode_glyph(const df_face *f, uint32_t gid,
                              const df_mat *m, df_path *p, int depth);

/* ---- simple glyph ---- */

static df_result decode_simple(const df_face *f, uint32_t gpos, int nc,
                               const df_mat *m, df_path *p)
{
    const df_blob *b = &f->blob;

    /* end points of contours -> point count */
    uint32_t epos = gpos + 10;
    int num_pts = (int)df_rd_u16(b, epos + (uint32_t)(nc - 1) * 2) + 1;
    if (num_pts <= 0 || num_pts > DF_MAX_POINTS) return DF_ERR_RANGE;

    uint32_t instr_len = df_rd_u16(b, epos + (uint32_t)nc * 2);
    uint32_t cur = epos + (uint32_t)nc * 2 + 2 + instr_len;   /* flags start */

    uint8_t *flags = (uint8_t *)malloc((size_t)num_pts);
    float   *X     = (float   *)malloc((size_t)num_pts * sizeof(float));
    float   *Y     = (float   *)malloc((size_t)num_pts * sizeof(float));
    if (!flags || !X || !Y) { free(flags); free(X); free(Y); return DF_ERR_NOMEM; }

    /* flags (with repeat) */
    for (int i = 0; i < num_pts; ) {
        uint8_t fl = df_rd_u8(b, cur++);
        flags[i++] = fl;
        if (fl & GF_REPEAT) {
            int rep = df_rd_u8(b, cur++);
            while (rep-- > 0 && i < num_pts) flags[i++] = fl;
        }
    }

    /* x coordinates (running) */
    int acc = 0;
    for (int i = 0; i < num_pts; i++) {
        uint8_t fl = flags[i];
        if (fl & GF_X_SHORT) {
            int d = df_rd_u8(b, cur++);
            acc += (fl & GF_X_SAME_POS) ? d : -d;
        } else if (!(fl & GF_X_SAME_POS)) {
            acc += df_rd_i16(b, cur); cur += 2;
        }
        X[i] = (float)acc;
    }
    /* y coordinates (running) */
    acc = 0;
    for (int i = 0; i < num_pts; i++) {
        uint8_t fl = flags[i];
        if (fl & GF_Y_SHORT) {
            int d = df_rd_u8(b, cur++);
            acc += (fl & GF_Y_SAME_POS) ? d : -d;
        } else if (!(fl & GF_Y_SAME_POS)) {
            acc += df_rd_i16(b, cur); cur += 2;
        }
        Y[i] = (float)acc;
    }

    /* on-curve flags packed in place */
    for (int i = 0; i < num_pts; i++) flags[i] = (uint8_t)(flags[i] & GF_ON_CURVE);

    df_result r = DF_OK;
    int cstart = 0;
    for (int c = 0; c < nc && !r; c++) {
        int cend = (int)df_rd_u16(b, epos + (uint32_t)c * 2);
        if (cend >= num_pts) cend = num_pts - 1;
        r = emit_contour(p, m, X, Y, flags, cstart, cend);
        cstart = cend + 1;
    }

    free(flags); free(X); free(Y);
    return r;
}

/* ---- composite glyph ---- */

static df_result decode_composite(const df_face *f, uint32_t gpos,
                                  const df_mat *m, df_path *p, int depth)
{
    const df_blob *b = &f->blob;
    uint32_t cur = gpos + 10;
    uint16_t flags;

    do {
        flags             = df_rd_u16(b, cur);
        uint16_t comp_gid = df_rd_u16(b, cur + 2);
        cur += 4;

        int arg1, arg2;
        if (flags & CF_ARG_WORDS) {
            arg1 = df_rd_i16(b, cur); arg2 = df_rd_i16(b, cur + 2); cur += 4;
        } else {
            arg1 = (int8_t)df_rd_u8(b, cur); arg2 = (int8_t)df_rd_u8(b, cur + 1); cur += 2;
        }

        float dx = 0, dy = 0;
        if (flags & CF_ARGS_XY) { dx = (float)arg1; dy = (float)arg2; }
        /* point-matching (flag clear) is rare in practice; treated as no offset */

        float a = 1, bb = 0, cc = 0, d = 1;
        if (flags & CF_HAVE_SCALE) {
            a = d = f2dot14(df_rd_i16(b, cur)); cur += 2;
        } else if (flags & CF_XY_SCALE) {
            a = f2dot14(df_rd_i16(b, cur)); d = f2dot14(df_rd_i16(b, cur + 2)); cur += 4;
        } else if (flags & CF_2X2) {
            a  = f2dot14(df_rd_i16(b, cur));
            bb = f2dot14(df_rd_i16(b, cur + 2));
            cc = f2dot14(df_rd_i16(b, cur + 4));
            d  = f2dot14(df_rd_i16(b, cur + 6));
            cur += 8;
        }

        df_mat local = { a, bb, cc, d, dx, dy };
        df_mat child = df_mat_mul(m, &local);
        df_result r = decode_glyph(f, comp_gid, &child, p, depth + 1);
        if (r) return r;
    } while (flags & CF_MORE);

    return DF_OK;
}

/* ---- dispatch ---- */

static df_result decode_glyph(const df_face *f, uint32_t gid,
                              const df_mat *m, df_path *p, int depth)
{
    if (depth > DF_MAX_COMPONENT_DEPTH) return DF_OK;

    uint32_t goff, glen;
    if (!df_sfnt_glyph_range(f, gid, &goff, &glen)) return DF_OK;  /* blank */

    int nc = df_rd_i16(&f->blob, goff);
    if (nc >= 0) return decode_simple(f, goff, nc, m, p);
    return decode_composite(f, goff, m, p, depth);
}

df_result df_glyf_decode(const df_face *f, uint32_t gid, df_path *p)
{
    df_mat id = df_mat_id();
    return decode_glyph(f, gid, &id, p, 0);
}
