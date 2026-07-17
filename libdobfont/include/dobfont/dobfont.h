/* libdobfont -- MainDOB scalable font engine.
 *
 * Parses real TrueType (.ttf) and OpenType (.otf) files "as they are"
 * (sfnt container) and rasterizes glyph outlines to 8-bit coverage
 * (R8), ready to upload into a dv DV_FMT_R8 glyph atlas and composite
 * with dv_draw_glyphs.
 *
 * The engine is the device-independent half of the font subsystem:
 * it takes a font blob + a size and yields metrics and coverage. It
 * does NOT talk to dv, the window server, or the filesystem -- the
 * caller owns I/O and the atlas. This keeps the engine a pure,
 * headless, testable library (see programs/fonttest).
 *
 * Modular backends sit behind df_face: the TrueType/glyf outline
 * backend is implemented now (covers .ttf and TrueType-flavoured
 * .otf). CFF/Type2 outlines (the other .otf flavour) plug in as a
 * second backend without changing this API -- df_has_outlines()
 * reports false for a face whose outline format is not yet supported.
 */

#ifndef DOBFONT_DOBFONT_H
#define DOBFONT_DOBFONT_H

#include <dob/types.h>   /* uintN_t / int16_t / size_t / bool / dob_status_t */

/* ---- result codes (0 ok, negative error; mirrors dv / dob style) ---- */
typedef enum
{
    DF_OK            =  0,
    DF_ERR_INVAL     = -1,   /* malformed arguments                          */
    DF_ERR_NOMEM     = -2,   /* allocation failed                            */
    DF_ERR_BADFONT   = -3,   /* not a valid sfnt / unsupported container     */
    DF_ERR_NOGLYPH   = -4,   /* glyph id out of range                        */
    DF_ERR_NOOUTLINE = -5,   /* face outline format not supported (e.g. CFF) */
    DF_ERR_RANGE     = -6,   /* coordinate / size out of sane range          */
} df_result;

/* Vertical (line) metrics, scaled to a pixel size. All in pixels. */
typedef struct
{
    float ascent;       /* + above baseline                      */
    float descent;      /* usually negative (below baseline)     */
    float line_gap;     /* extra leading between lines           */
    float line_height;  /* ascent - descent + line_gap (handy)   */
} df_vmetrics;

/* Per-glyph metrics at a pixel size. Ink box is relative to the pen
 * origin on the baseline, y up. A blank glyph (e.g. space) has a zero
 * ink box but a non-zero advance. */
typedef struct
{
    float advance;          /* pen advance, pixels             */
    float lsb;              /* left side bearing, pixels       */
    float x0, y0, x1, y1;   /* ink box, pixels, y up           */
} df_gmetrics;

/* A rasterized coverage bitmap (R8). Origin is the top-left of the ink
 * box. To place it: in device space (y down), with the pen at
 * (pen_x, baseline_y), blit at (pen_x + left, baseline_y - top). */
typedef struct
{
    uint8_t *cover;    /* w*h bytes, row-major, pitch == w (malloc'd) */
    int      w, h;     /* size in pixels; 0/0 + cover==NULL = blank   */
    int      left;     /* x offset from pen origin to bitmap left     */
    int      top;      /* y offset from baseline UP to bitmap top     */
    float    advance;  /* pen advance, pixels (convenience)           */
} df_bitmap;

/* A rasterization request. Synthetic styling (embolden / slant) is
 * applied at raster time for faces that do not ship a designed bold or
 * italic; pass 0 to disable. shift_x lets a glyph cache rasterize
 * sub-pixel phases of the same glyph. */
typedef struct
{
    float px_size;   /* em size in pixels (REQUIRED, > 0)            */
    float embolden;  /* extra stroke weight in px (approx), 0 = none */
    float slant;     /* oblique shear, tan(angle), e.g. 0.21 ~ 12deg */
    float shift_x;   /* sub-pixel x phase 0..1, 0 = none             */
} df_raster_req;

/* Opaque face. The font blob passed to df_open must stay valid for the
 * face's lifetime (df keeps a pointer, it does not copy the bytes). */
typedef struct df_face df_face;

/* ---- lifecycle ---- */

/* Open a face from an in-memory sfnt blob (.ttf / .otf / .ttc).
 * face_index selects a face inside a TrueType Collection (.ttc); use 0
 * for a normal single-face file. */
df_result df_open(const void *data, uint32_t size, uint32_t face_index,
                  df_face **out);
void      df_close(df_face *f);

/* ---- queries ---- */

uint16_t  df_units_per_em(const df_face *f);
uint16_t  df_num_glyphs  (const df_face *f);
bool      df_has_outlines(const df_face *f);   /* false until CFF backend lands */
df_result df_face_vmetrics    (const df_face *f, float px_size, df_vmetrics *out);

/* Style read from the face (OS/2 usWeightClass + italic bit). Handy for a
 * font resolver. weight is 100..900 (400 normal, 700 bold). */
uint16_t  df_face_weight   (const df_face *f);
bool      df_face_is_italic(const df_face *f);

/* Map a Unicode code point to a glyph id; returns 0 (.notdef) if the
 * face has no glyph for it. */
uint32_t  df_map_codepoint(const df_face *f, uint32_t cp);

/* Glyph metrics at a size, no rasterization. */
df_result df_glyph_metrics(const df_face *f, uint32_t gid, float px_size,
                           df_gmetrics *out);

/* ---- rasterization ---- */

/* Rasterize glyph gid into an 8-bit coverage bitmap per req. The caller
 * frees out->cover via df_free_bitmap (or free()). A blank glyph yields
 * w==h==0, cover==NULL, and DF_OK. */
df_result df_rasterize(const df_face *f, uint32_t gid,
                       const df_raster_req *req, df_bitmap *out);
void      df_free_bitmap(df_bitmap *bm);

#endif /* DOBFONT_DOBFONT_H */
