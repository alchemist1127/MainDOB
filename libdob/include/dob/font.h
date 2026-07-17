/* MainDOB shared font -- 8x16 bitmap, full 256-glyph table.
 *
 * Exposed as a single shared symbol so each program that links libdobui
 * does not carry its own copy of the glyph table.
 *
 * The table is indexed directly by the raw character byte (0..255):
 * ASCII 32..126 are populated, plus the Latin-1/-9 codepoints the
 * keyboard layouts emit (accented letters, degree/section/pound/euro).
 * Unused slots are zero (rendered as blank). */

#ifndef MAINDOB_LIBDOBUI_DOB_FONT_H
#define MAINDOB_LIBDOBUI_DOB_FONT_H

#include <dob/types.h>

#define DOB_FONT_W       8
#define DOB_FONT_H       16

/* The table now spans the whole byte range. FIRST/LAST are kept for
 * source compatibility with code that iterates the glyph set; they no
 * longer denote a printable sub-range. */
#define DOB_FONT_FIRST   0
#define DOB_FONT_LAST    255
#define DOB_FONT_COUNT   256

extern const uint8_t dob_font_data[DOB_FONT_COUNT][DOB_FONT_H];

/* Return row bits for char ch at y (0..15). The byte cast covers the
 * whole 0..255 range, so the lookup is a single indexed load with no
 * remap branch. Inline so the hot font_draw_char loop keeps inlining. */
static inline uint8_t dob_font_row(char ch, int row)
{
    if (row < 0 || row >= DOB_FONT_H) return 0;
    return dob_font_data[(uint8_t)ch][row];
}

/* Decode one display glyph starting at s[i] (caller guarantees i < len).
 * Returns the number of input bytes consumed (>= 1) and writes the glyph
 * index (0..255) into *glyph.
 *
 * The font and the keyboard layouts are Latin-1/-9 (one byte per glyph),
 * but C source string literals are saved as UTF-8 by virtually every
 * editor, so an accented letter in a label reaches us as a 2-byte UTF-8
 * sequence (e.g. 'e-grave' = C3 A8) instead of the single Latin-1 byte
 * (E8). We bridge the two here so both render correctly:
 *
 *   - ASCII (< 0x80) passes through and consumes 1 byte.
 *   - A byte that begins a *valid, complete* UTF-8 multibyte sequence is
 *     decoded to a codepoint cp:
 *         cp <= 0xFF                  -> glyph = cp           (Latin-1)
 *         common typographic chars    -> folded to a look-alike (table)
 *         anything else               -> '?'                 (visible)
 *   - Any byte that does NOT begin a valid sequence (a stray, truncated,
 *     or bad-continuation byte) is treated as raw Latin-1 and consumes 1
 *     byte. This is what keeps already-Latin-1 input — such as the bytes
 *     a textbox gets from the keyboard — rendering as before.
 *
 * The only realistic ambiguity is three+ consecutive Latin-1 bytes that
 * happen to form a valid UTF-8 sequence; that combination is not
 * reachable from the IT/US layouts, so it is accepted as a known limit. */
static inline uint32_t dob_font_decode(const uint8_t *s, uint32_t len,
                                       uint32_t i, uint8_t *glyph)
{
    uint8_t b0 = s[i];
    if (b0 < 0x80) { *glyph = b0; return 1; }

    uint32_t cp, need;
    if      ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1Fu; need = 1; }
    else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0Fu; need = 2; }
    else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07u; need = 3; }
    else { *glyph = b0; return 1; }              /* stray byte -> Latin-1 */

    if (i + need >= len) { *glyph = b0; return 1; }   /* truncated -> Latin-1 */
    for (uint32_t k = 1; k <= need; k++) {
        uint8_t bk = s[i + k];
        if ((bk & 0xC0) != 0x80) { *glyph = b0; return 1; } /* bad cont -> L1 */
        cp = (cp << 6) | (bk & 0x3Fu);
    }

    if (cp <= 0xFF) { *glyph = (uint8_t)cp; }
    else switch (cp) {
        case 0x2013: case 0x2014: *glyph = '-';  break; /* - en/em dash    */
        case 0x2018: case 0x2019: *glyph = '\''; break; /* ' curly quotes  */
        case 0x201C: case 0x201D: *glyph = '"';  break; /* " curly dquotes */
        case 0x2026:              *glyph = '.';  break; /* . ellipsis      */
        case 0x2192:              *glyph = '>';  break; /* > rightarrow    */
        default:                  *glyph = '?';  break;
    }
    return 1 + need;
}

/* ---- proportional (adaptive) spacing ----------------------------------
 * Per-glyph ink metrics, generated alongside dob_font_data: dob_glyph_l is
 * the first inked column of a glyph and dob_glyph_w its inked width
 * (0 = blank, e.g. space). To lay text out proportionally a renderer draws
 * each glyph shifted left by dob_glyph_l (so its ink starts at the pen) and
 * then advances the pen by dob_font_advance(): the inked width plus a small
 * constant tracking, or a fixed width for blanks. This is the "read the
 * first and last inked column" scheme, precomputed once at build time. */
extern const uint8_t dob_glyph_l[DOB_FONT_COUNT];
extern const uint8_t dob_glyph_w[DOB_FONT_COUNT];

#define DOB_FONT_TRACKING  1   /* pixels of gap after each inked glyph */
#define DOB_FONT_SPACE_ADV 3   /* advance for a blank glyph (space)    */

static inline int dob_font_left(uint8_t ch) { return dob_glyph_l[ch]; }

static inline int dob_font_advance(uint8_t ch)
{
    int w = dob_glyph_w[ch];
    return w ? w + DOB_FONT_TRACKING : DOB_FONT_SPACE_ADV;
}

/* Total proportional pixel width of a UTF-8 string of byte-length len.
 * Equals the pen position after drawing it, so it also gives the x of a
 * cursor placed just past the last glyph. Decodes UTF-8 exactly as the
 * renderers do, so measurement and drawing always agree. */
static inline int dob_text_width(const char *s, uint32_t len)
{
    uint32_t i = 0;
    int x = 0;
    while (i < len) {
        uint8_t g;
        i += dob_font_decode((const uint8_t *)s, len, i, &g);
        x += dob_font_advance(g);
    }
    return x;
}

#endif
