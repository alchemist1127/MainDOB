/* libdobfont -- internal shared definitions (not a public header).
 *
 * Holds the sfnt byte reader, the glyph-path buffer, the resolved
 * table map, the df_face layout, and the seams between the modules:
 *
 *   sfnt.c         container + cmap + loca + hmtx
 *   outline.c      glyf outline decode -> font-unit path
 *   raster.c       device-space path -> 8-bit coverage
 *   backend_sfnt.c metrics + rasterize (ties the three together)
 *   dobfont.c      open/close, public dispatch
 *
 * Freestanding target: no libm. Tiny float helpers live here.
 */

#ifndef DOBFONT_INTERNAL_H
#define DOBFONT_INTERNAL_H

#include <dobfont/dobfont.h>

/* ---- 4-char big-endian table tag ---- */
#define DF_TAG(a, b, c, d) \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | \
     ((uint32_t)(uint8_t)(c) <<  8) |  (uint32_t)(uint8_t)(d))

/* ---- bounded big-endian reader over the whole font blob ----
 * Out-of-range reads return 0; parse paths validate ranges up front,
 * these guards keep a malformed font from reading past the buffer. */
typedef struct
{
    const uint8_t *base;
    uint32_t       size;
} df_blob;

uint8_t  df_rd_u8 (const df_blob *b, uint32_t off);
uint16_t df_rd_u16(const df_blob *b, uint32_t off);
int16_t  df_rd_i16(const df_blob *b, uint32_t off);
uint32_t df_rd_u32(const df_blob *b, uint32_t off);

/* ---- glyph outline as a list of segments ----
 * Points are absolute. The "current point" is implied (the previous
 * segment's endpoint); DF_MOVE sets it and opens a contour, DF_CLOSE
 * draws a line back to the contour start. */
typedef enum { DF_MOVE, DF_LINE, DF_QUAD, DF_CUBIC, DF_CLOSE } df_segkind;

typedef struct
{
    df_segkind k;
    /* MOVE/LINE: p0 = target. QUAD: p0 = control, p1 = end.
     * CUBIC: p0,p1 = controls, p2 = end. CLOSE: unused. */
    float x[3], y[3];
} df_seg;

typedef struct
{
    df_seg  *seg;
    uint32_t count, cap;
} df_path;

void      df_path_init(df_path *p);
void      df_path_free(df_path *p);
df_result df_path_push(df_path *p, const df_seg *s);
void      df_path_reset(df_path *p);   /* keep capacity, clear contents */

/* ---- 2x3 affine: (x,y) -> (a*x + c*y + e, b*x + d*y + f) ---- */
typedef struct { float a, b, c, d, e, f; } df_mat;

df_mat df_mat_id(void);
/* Combined = outer applied AFTER inner (inner first). */
df_mat df_mat_mul(const df_mat *outer, const df_mat *inner);

/* ---- resolved table location (0,0 = absent) ---- */
typedef struct { uint32_t off, len; } df_table;

/* which outline technology backs this face */
typedef enum { DF_OUTLINE_NONE = 0, DF_OUTLINE_GLYF, DF_OUTLINE_CFF } df_outline_kind;

/* which face backend produces this face's glyphs */
typedef enum { DF_FACE_SFNT = 0, DF_FACE_MONO } df_face_kind;

struct df_cff;   /* CFF backend state, defined in cff.c */
struct df_mono;  /* monobit/bitmap backend state, defined in backend_mono.c */

/* ---- the face ---- */
struct df_face
{
    df_face_kind kind;       /* DF_FACE_SFNT (default) or DF_FACE_MONO */
    struct df_mono *monop;   /* non-NULL when kind == DF_FACE_MONO     */

    df_blob  blob;

    df_table head, maxp, hhea, hmtx, cmap, loca, glyf, cff, kern, os2;

    uint16_t units_per_em;
    uint16_t num_glyphs;
    uint16_t num_h_metrics;
    int16_t  ascent, descent, line_gap;   /* hhea, font units */
    int16_t  index_to_loc;                /* head: 0 short, 1 long */

    uint32_t cmap_sub;       /* absolute offset of chosen cmap subtable, 0 = none */
    uint8_t  cmap_format;    /* 4 or 12, else 0 */

    bool             has_outlines;   /* an outline backend can render this face */
    df_outline_kind  outline_kind;
    struct df_cff   *cffp;           /* non-NULL when outline_kind == DF_OUTLINE_CFF */
};

/* ---- sfnt.c ---- */
df_result df_sfnt_open(df_face *f, const void *data, uint32_t size,
                       uint32_t face_index);
uint32_t  df_sfnt_advance(const df_face *f, uint32_t gid);     /* font units */
uint32_t  df_sfnt_cmap_lookup(const df_face *f, uint32_t cp);  /* gid, 0 = none */
/* Resolve the glyf byte range for gid. Returns false for an out-of-range
 * or empty (zero-length) glyph -- the latter is a valid blank glyph. */
bool      df_sfnt_glyph_range(const df_face *f, uint32_t gid,
                              uint32_t *off, uint32_t *len);

/* ---- outline.c (glyf backend) ----
 * Decode glyph gid into p (font-unit segments, y up). Empty glyphs leave
 * p empty and still return DF_OK. */
df_result df_glyf_decode(const df_face *f, uint32_t gid, df_path *p);

/* ---- cff.c (CFF/Type2 backend) ---- */
df_result df_cff_open(df_face *f);
void      df_cff_close(df_face *f);
df_result df_cff_decode(const df_face *f, uint32_t gid, df_path *p);

/* route to whichever outline backend the face uses */
df_result df_outline_decode(const df_face *f, uint32_t gid, df_path *p);

/* ---- backend_sfnt.c: per-glyph metrics + rasterization (sfnt faces) ---- */
df_result df_sfnt_glyph_metrics(const df_face *f, uint32_t gid, float px_size,
                                df_gmetrics *out);
df_result df_sfnt_rasterize(const df_face *f, uint32_t gid,
                            const df_raster_req *req, df_bitmap *out);

/* ---- backend_mono.c: bitmap (monobit) faces ----
 * A monobit face renders precomputed 1-bit glyphs (no outlines, no
 * anti-aliasing): df_rasterize just expands a glyph's bits to 0/255
 * coverage, integer-scaled to the requested size. cmap is a single
 * contiguous code-point range. Same two render entry points as sfnt,
 * so the layout/page pipeline is unchanged. */
df_result df_mono_open(df_face *f, const void *data, uint32_t size);
void      df_mono_close(df_face *f);
uint32_t  df_mono_cmap_lookup(const df_face *f, uint32_t cp);   /* gid, 0 = none */
df_result df_mono_vmetrics(const df_face *f, float px_size, df_vmetrics *out);
df_result df_mono_glyph_metrics(const df_face *f, uint32_t gid, float px_size,
                                df_gmetrics *out);
df_result df_mono_rasterize(const df_face *f, uint32_t gid,
                            const df_raster_req *req, df_bitmap *out);

/* style read from OS/2 / head (used by the resolver) */
uint16_t  df_sfnt_weight(const df_face *f);   /* usWeightClass, 400 if absent */
bool      df_sfnt_italic(const df_face *f);

/* ---- raster.c ----
 * Fill a path already mapped into DEVICE space (pixels, y down) into a
 * w*h 8-bit coverage buffer. Nonzero winding, 4x vertical supersample +
 * exact horizontal coverage. cover is zeroed internally. */
df_result df_raster_fill(const df_path *dev, int w, int h, uint8_t *cover);

/* ---- tiny freestanding float helpers (no libm) ---- */
static inline float df_fabsf(float v)  { return v < 0.0f ? -v : v; }
static inline float df_floorf(float v)
{
    int i = (int)v;
    return (float)((v < (float)i) ? i - 1 : i);
}
static inline float df_ceilf(float v)
{
    int i = (int)v;
    return (float)((v > (float)i) ? i + 1 : i);
}
static inline float df_minf(float a, float b) { return a < b ? a : b; }
static inline float df_maxf(float a, float b) { return a > b ? a : b; }

#endif /* DOBFONT_INTERNAL_H */
