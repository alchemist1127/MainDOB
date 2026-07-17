/* libdobdoc -- the DobWrite document model.
 *
 * The data half of the word processor: what the document *is*, separate
 * from how it's laid out or drawn. Headless and testable.
 *
 *   - Text is held in a piece table (original + append buffers) so edits
 *     don't shuffle large runs of bytes and undo is cheap.
 *   - Character formatting is stored as attribute runs over the text
 *     (bold/italic/font/size/colour as spans), paragraph formatting as
 *     runs too (alignment/indents/spacing), both maintained across edits.
 *   - Named styles carry partial formatting and inherit from a parent;
 *     the effective format at a position is the cascade
 *         default  <-  paragraph style chain  <-  direct formatting.
 *   - The file format serialises the MODEL (content + formatting + styles),
 *     not the layout, as a chunked container so unknown chunks are skipped.
 *
 * Units: twips (1/1440 inch) everywhere -- font size, indents, spacing,
 * page geometry. 1 pt = 20 twips. Layout converts twips -> px by DPI.
 *
 * Positions are byte offsets into the logical UTF-8 text. Use
 * df_doc_next_cp / df_doc_prev_cp to keep a caret on code-point
 * boundaries.
 */

#ifndef DOBDOC_DOBDOC_H
#define DOBDOC_DOBDOC_H

#include <dob/types.h>

typedef enum
{
    DD_OK         =  0,
    DD_ERR_INVAL  = -1,
    DD_ERR_NOMEM  = -2,
    DD_ERR_RANGE  = -3,
    DD_ERR_BADFILE= -4,
} dd_result;

/* ---- character formatting ---- */
enum {                              /* CharFmt.mask bits: which fields are set */
    DD_CF_FAMILY    = 1u << 0,
    DD_CF_SIZE      = 1u << 1,
    DD_CF_BOLD      = 1u << 2,
    DD_CF_ITALIC    = 1u << 3,
    DD_CF_UNDERLINE = 1u << 4,
    DD_CF_COLOR     = 1u << 5,
    DD_CF_STRIKE    = 1u << 6,
    DD_CF_HIGHLIGHT = 1u << 7,
};

typedef struct
{
    uint16_t mask;        /* set fields (DD_CF_*) */
    uint16_t family_id;   /* interned family name id (see df_doc_family_*) */
    uint32_t size_twips;  /* font size in twips (1 pt = 20) */
    uint8_t  bold;
    uint8_t  italic;
    uint8_t  underline;
    uint8_t  strike;
    uint32_t color;       /* 0xAARRGGBB */
    uint32_t highlight;   /* 0xAARRGGBB text background; alpha 0 = none */
} CharFmt;

/* ---- paragraph formatting ---- */
typedef enum { DD_ALIGN_LEFT = 0, DD_ALIGN_CENTER, DD_ALIGN_RIGHT, DD_ALIGN_JUSTIFY } dd_align;

enum {                              /* ParaFmt.mask bits */
    DD_PF_ALIGN        = 1u << 0,
    DD_PF_INDENT_LEFT  = 1u << 1,
    DD_PF_INDENT_RIGHT = 1u << 2,
    DD_PF_INDENT_FIRST = 1u << 3,
    DD_PF_SPACE_BEFORE = 1u << 4,
    DD_PF_SPACE_AFTER  = 1u << 5,
    DD_PF_LINE_SPACING = 1u << 6,
    DD_PF_STYLE        = 1u << 7,
};

typedef struct
{
    uint16_t mask;          /* set fields (DD_PF_*) */
    uint8_t  align;         /* dd_align */
    uint8_t  _pad;
    int32_t  indent_left;   /* twips */
    int32_t  indent_right;  /* twips */
    int32_t  indent_first;  /* twips, first-line extra indent */
    uint32_t space_before;  /* twips above the paragraph */
    uint32_t space_after;   /* twips below the paragraph */
    uint32_t line_spacing;  /* twips line height; 0 = single (auto from font) */
    int16_t  style_id;      /* named style for the paragraph, -1 = none */
} ParaFmt;

/* ---- named style (partial fmt + parent for inheritance) ---- */
typedef struct
{
    char    name[48];
    int16_t parent;   /* parent style id, or -1 */
    CharFmt cf;       /* mask = which char fields this style sets */
    ParaFmt pf;       /* mask = which para fields this style sets */
} Style;

/* ---- page setup ---- */
typedef struct
{
    uint32_t width, height;                          /* twips */
    uint32_t margin_left, margin_right;
    uint32_t margin_top, margin_bottom;              /* twips */
    uint32_t bg_color;                               /* paper colour 0xFFRRGGBB; default white */
    uint32_t columns;                                /* newspaper columns 1..6 (0/1 = single) */
    uint32_t column_gap;                             /* gutter between columns, twips         */
} PageSetup;

typedef struct df_doc df_doc;

/* ---- lifecycle ---- */
df_doc   *df_doc_create(void);                        /* empty doc, "Normal" style 0 */
void      df_doc_destroy(df_doc *d);

/* ---- family name interning ---- */
uint16_t  df_doc_family_intern(df_doc *d, const char *name);   /* name -> id */
const char *df_doc_family_name(const df_doc *d, uint16_t id);  /* id -> name ("" if none) */

/* ---- defaults & page ---- */
void      df_doc_get_default_char(const df_doc *d, CharFmt *out);  /* fully-set */
void      df_doc_set_default_char(df_doc *d, const CharFmt *cf);   /* fully-set */
void      df_doc_get_page(const df_doc *d, PageSetup *out);
void      df_doc_set_page(df_doc *d, const PageSetup *p);

/* ---- sections ----
 * The document is split into contiguous sections, each with its own page
 * geometry.  A fresh document has exactly one section spanning everything,
 * so code that ignores sections keeps working (get_page returns section 0,
 * set_page applies to every section).  Sections track edits automatically. */
uint32_t  df_doc_section_count(const df_doc *d);
/* Ordinal (0-based, document order) of the section containing byte `pos`. */
uint32_t  df_doc_section_at(const df_doc *d, uint32_t pos);
/* Byte range of the section with the given ordinal. */
dd_result df_doc_section_range(const df_doc *d, uint32_t ordinal,
                               uint32_t *start, uint32_t *len);
/* Page geometry of a section, by ordinal or by a contained byte position. */
dd_result df_doc_section_page(const df_doc *d, uint32_t ordinal, PageSetup *out);
void      df_doc_section_page_at(const df_doc *d, uint32_t pos, PageSetup *out);
/* Set the geometry of just the section containing `pos` (undoable). */
dd_result df_doc_set_section_page_at(df_doc *d, uint32_t pos, const PageSetup *p);
/* Start a new section at `pos` (a paragraph boundary).  The new section
 * inherits the geometry of the one it splits; no-op if `pos` is already a
 * section boundary.  Undoable. */
dd_result df_doc_insert_section_break(df_doc *d, uint32_t pos);
/* Remove the section boundary at `pos`, merging it into the previous section.
 * No-op if `pos` is not a boundary or is the document start.  Undoable. */
dd_result df_doc_remove_section_break(df_doc *d, uint32_t pos);

/* ---- styles ---- */
int       df_doc_style_define(df_doc *d, const char *name, int parent,
                              const CharFmt *cf, const ParaFmt *pf);  /* -> style id */
int       df_doc_style_count(const df_doc *d);
dd_result df_doc_style_get(const df_doc *d, int id, Style *out);

/* ---- editing (each is one undo step) ---- */
dd_result df_doc_insert(df_doc *d, uint32_t pos, const char *utf8, uint32_t n);
dd_result df_doc_insert_fmt(df_doc *d, uint32_t pos, const char *utf8, uint32_t n,
                            const CharFmt *overrides);     /* inserted text gets overrides */
dd_result df_doc_delete(df_doc *d, uint32_t pos, uint32_t n);
dd_result df_doc_apply_char_fmt(df_doc *d, uint32_t pos, uint32_t n, const CharFmt *cf);
dd_result df_doc_set_para_fmt(df_doc *d, uint32_t pos, uint32_t n, const ParaFmt *pf);
dd_result df_doc_set_para_style(df_doc *d, uint32_t pos, uint32_t n, int style_id);

/* ---- queries ---- */
uint32_t  df_doc_length(const df_doc *d);                         /* bytes */
uint32_t  df_doc_get_text(const df_doc *d, uint32_t pos, uint32_t n,
                          char *buf, uint32_t cap);                /* -> bytes copied */
void      df_doc_char_fmt_at(const df_doc *d, uint32_t pos, CharFmt *resolved);
void      df_doc_char_fmt_at_para(const df_doc *d, uint32_t para_start, uint32_t pos, CharFmt *resolved);

uint32_t  df_doc_para_count(const df_doc *d);
uint32_t  df_doc_para_at(const df_doc *d, uint32_t pos);          /* -> paragraph index */
dd_result df_doc_para_range(const df_doc *d, uint32_t index,
                            uint32_t *start, uint32_t *len);
void      df_doc_para_fmt_resolved(const df_doc *d, uint32_t index, ParaFmt *resolved);

/* code-point boundary helpers (UTF-8) */
uint32_t  df_doc_next_cp(const df_doc *d, uint32_t pos);
uint32_t  df_doc_prev_cp(const df_doc *d, uint32_t pos);

/* ---- undo / redo ---- */
bool      df_doc_can_undo(const df_doc *d);
bool      df_doc_can_redo(const df_doc *d);
dd_result df_doc_undo(df_doc *d);
dd_result df_doc_redo(df_doc *d);

/* ---- file format (serialise the model to/from a byte buffer) ---- */
/* On success *out points to a malloc'd buffer of *size bytes; caller frees. */
dd_result df_doc_serialize(const df_doc *d, uint8_t **out, uint32_t *size);
dd_result df_doc_load(const uint8_t *data, uint32_t size, df_doc **out);

#endif /* DOBDOC_DOBDOC_H */
