/* libdoblayout -- persistent, incremental, queryable layout engine.
 *
 * The layout is the bridge between the document model (a continuous,
 * editable byte stream of paragraphs) and the page engine that
 * materializes sheets. It is deliberately PAGE-AGNOSTIC: it lays the
 * document out as one continuous vertical column in "content space"
 * (pixels at a given DPI, x measured from the page's left margin, y from
 * the top of the first paragraph). The page engine slices that column
 * into sheets; pagination does not live here.
 *
 * Three properties make it usable on real hardware:
 *   - Persistent: built once with df_layout_create and kept alive.
 *   - Incremental: df_layout_reflow re-shapes only the paragraph(s) an
 *     edit touched; paragraphs below merely translate (y + byte offsets
 *     shift, no re-shaping). Typing one character does not re-lay the doc.
 *   - Queryable: byte -> geometry (df_layout_locate, caret) and point ->
 *     byte (df_layout_hit, clicks).
 *
 * Ownership: each paragraph owns its line and glyph arrays. A dl_line's
 * `glyphs` and a dl_para's `lines` are valid until that paragraph is next
 * reflowed (or the layout is destroyed).
 */

#ifndef DOBLAYOUT_DOBLAYOUT_H
#define DOBLAYOUT_DOBLAYOUT_H

#include <dobdoc/dobdoc.h>
#include <dobfont/dobfont.h>
#include <dobfont/resolver.h>

/* A positioned glyph. x is content-absolute (px from the page's left
 * margin); the baseline y is the owning line's (y + ascent). */
typedef struct
{
    uint32_t gid;
    float    x;
    float    advance;
    float    px_size;
    float    embolden;
    float    slant;
    uint32_t color;        /* 0xAARRGGBB */
    uint32_t highlight;    /* 0xAARRGGBB text background; alpha 0 = none */
    df_face *face;
    uint32_t byte;         /* source byte offset */
    uint8_t  underline, strike;
} dl_glyph;

/* A laid-out line. Geometry is content-absolute. baseline = y + ascent. */
typedef struct
{
    uint32_t byte_start, byte_len;
    float    x;
    float    y;            /* top of the line */
    float    width;        /* inked width, trailing spaces excluded */
    float    ascent, descent, height;
    const dl_glyph *glyphs;
    uint32_t glyph_count;
} dl_line;

/* A laid-out paragraph: a run of lines stacked in content space. */
typedef struct
{
    uint32_t index;
    uint32_t byte_start, byte_len;
    float    y;            /* top of the paragraph */
    float    height;       /* includes space-before/after */
    const dl_line *lines;
    uint32_t line_count;
} dl_para;

typedef struct { float dpi; } dl_opts;

typedef struct df_layout df_layout;

df_layout *df_layout_create(const df_doc *doc, const df_fontset *fonts, const dl_opts *opts);
void       df_layout_destroy(df_layout *L);

/* What a reflow changed, for the page engine to invalidate. */
typedef struct
{
    uint32_t first_para;        /* first paragraph index re-laid */
    uint32_t para_count_old;    /* paragraphs replaced */
    uint32_t para_count_new;    /* paragraphs produced (differs on split/merge) */
    float    height_delta;      /* y-shift applied to everything below the window */
    float    dirty_y0, dirty_y1;/* content-space band whose pixels were re-shaped */
} dl_update;

/* Re-flow after a document edit: bytes [pos, pos+old_len) were replaced by
 * `new_len` bytes (in the now-current document). Re-shapes only the
 * affected paragraph window; paragraphs below are translated. */
void df_layout_reflow(df_layout *L, uint32_t pos, uint32_t old_len, uint32_t new_len,
                      dl_update *out);

/* Full rebuild from the document (fallback / after wholesale changes). */
void df_layout_rebuild(df_layout *L);

/* Queries */
uint32_t        df_layout_para_count(const df_layout *L);
const dl_para  *df_layout_para(const df_layout *L, uint32_t i);
float           df_layout_content_height(const df_layout *L);
float           df_layout_content_width(const df_layout *L);
uint32_t        df_layout_columns(const df_layout *L);

/* Page geometry (px), so the page engine can paginate the column. */
void df_layout_page_metrics(const df_layout *L,
                            float *page_w, float *page_h,
                            float *margin_left, float *margin_top,
                            float *content_w, float *content_h);

/* ---- sections: pixel geometry consumed by the page engine ---- */
uint32_t df_layout_section_count(const df_layout *L);
uint32_t df_layout_para_section(const df_layout *L, uint32_t para_index);
void     df_layout_section_px(const df_layout *L, uint32_t ordinal,
                              float *page_w, float *page_h, float *content_top,
                              float *content_left, float *content_w, float *content_h);
uint32_t df_layout_section_bg(const df_layout *L, uint32_t ordinal);

/* byte -> caret geometry (content space). Returns false only if empty. */
typedef struct { uint32_t para, line; float x, y_top, y_bottom; } dl_locus;
bool df_layout_locate(const df_layout *L, uint32_t byte, dl_locus *out);

/* point (content space) -> nearest caret byte offset. */
uint32_t df_layout_hit(const df_layout *L, float cx, float cy);

#endif /* DOBLAYOUT_DOBLAYOUT_H */
