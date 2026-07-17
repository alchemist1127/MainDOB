/* DobUITools — TextBox Controls
 *
 *   dob_textbox_t      — Single-line text input
 *   dob_multitextbox_t — Multi-line text editor
 *
 * Both controls support:
 *   - Drag-to-select with the mouse (OnClick -> OnDrag -> OnRelease)
 *   - Multi-double-click expansion: word -> line -> whole text
 *     (the single-line box skips straight to whole text on the
 *      second double-click, since "line" and "whole" coincide)
 *   - Clipboard operations: Copy, Cut, Paste, SelectAll, Clear,
 *     CopyAll — via both direct calls and the focus manager's
 *     contextual panel. Keyboard shortcuts Ctrl+A/C/X/V arrive
 *     as ASCII control bytes (0x01/0x03/0x18/0x16) through OnKey.
 *
 * The multi-line widget owns a heap-allocated buffer that grows on
 * demand; call dobmt_Free to release it explicitly. Single-line uses
 * a fixed inline buffer of DOBTB_MAX_TEXT bytes.
 */

#ifndef MAINDOB_DOBUITOOLS_TEXTBOX_H
#define MAINDOB_DOBUITOOLS_TEXTBOX_H

#include <dob/types.h>
#include "dobui_common.h"
#include "dobui_theme.h"

/* Special key codes */
#define DOBTB_KEY_UP      128
#define DOBTB_KEY_DOWN    129
#define DOBTB_KEY_LEFT    130
#define DOBTB_KEY_RIGHT   131
#define DOBTB_KEY_HOME    132
#define DOBTB_KEY_END     133
#define DOBTB_KEY_DELETE  134

/* True for a byte that should be inserted as a text character:
 * printable ASCII (32..126) plus printable Latin-1/-9 (160..255),
 * the latter being the accented letters and symbols the keyboard
 * layouts emit. Excludes DEL (127) and the 128..159 band -- that is
 * exactly where the DOBTB_KEY_* navigation codes live, so an accented
 * byte can never be confused with an arrow key. Those navigation
 * codes are matched explicitly before this test is reached anyway. */
static inline bool dobtb_is_text_char(uint8_t key)
{
    return (key >= 32 && key < 127) || key >= 0xA0;
}

/* Color defaults */
#define DOBTB_COL_BG           DOBUI_INSET
#define DOBTB_COL_TEXT         DOBUI_INPUT
#define DOBTB_COL_CURSOR       DOBUI_INPUT
#define DOBTB_COL_BORDER       DOBUI_SURFACE
#define DOBTB_COL_BORDER_FOCUS DOBUI_TEXT
#define DOBTB_COL_LINENO_BG    DOBUI_INSET
#define DOBTB_COL_LINENO_FG    DOBUI_TEXT
#define DOBTB_COL_SEL_BG       DOBUI_RELIEF
#define DOBTB_COL_SEL_TEXT     DOBUI_INPUT

/* Multi-double-click window: 800 ms is comfortable for casual users;
 * 4 px of slack tolerates a relaxed hand. */
#define DOBTB_MULTICLICK_MS   800
#define DOBTB_MULTICLICK_SLOP 4

/* Multi-click tracking state, shared between single and multi. */
typedef struct
{
    uint32_t last_ms;
    int      last_x, last_y;
    int      count;
} dob_multiclick_t;

/* Single-Line TextBox */

#define DOBTB_MAX_TEXT      512
#define DOBTB_FONT_W        8
#define DOBTB_FONT_H        16
#define DOBTB_DEFAULT_H     22
#define DOBTB_PAD           4

typedef struct
{
    uint32_t     win_id;
    int          x, y;
    int          w, h;

    char         text[DOBTB_MAX_TEXT];
    int          len;
    int          cursor;
    int          scroll_x;

    /* Selection: sel_anchor < 0 means no selection; otherwise the
     * selected range is [min(sel_anchor,cursor), max(sel_anchor,cursor)).
     * `selecting` is true between OnClick and OnRelease — the widget
     * is the target of the current drag. Without it, a widget that
     * merely holds a stale selection would keep reacting to every
     * global OnDrag the focus manager dispatches. */
    int          sel_anchor;
    bool         selecting;

    dob_multiclick_t mc;

    bool         visible;
    bool         enabled;
    bool         focused;
    bool         modified;
    /* Modalita' password: il Draw stampa un pallino per carattere al
     * posto del testo (il buffer resta quello vero), e Copy / Cut /
     * CopyAll rifiutano -- la clipboard non deve mai ricevere un
     * segreto. Paste resta ammesso (comodita' d'inserimento). Default
     * false da dobtb_Init. */
    bool         masked;
    dob_anchor_t anchor;

    uint32_t     col_bg;
    uint32_t     col_text;
    uint32_t     col_cursor;
    uint32_t     col_border;
    uint32_t     col_border_focus;
    uint32_t     col_sel_bg;
    uint32_t     col_sel_text;
} dob_textbox_t;

void        dobtb_Init(dob_textbox_t *tb, uint32_t win_id,
                        int x, int y, int w, int h);
void        dobtb_SetText(dob_textbox_t *tb, const char *text);
const char *dobtb_GetText(dob_textbox_t *tb);
bool        dobtb_OnKey(dob_textbox_t *tb, uint8_t key);
bool        dobtb_OnClick(dob_textbox_t *tb, int x, int y);
bool        dobtb_OnDrag(dob_textbox_t *tb, int x, int y);
bool        dobtb_OnRelease(dob_textbox_t *tb);
bool        dobtb_OnDblClick(dob_textbox_t *tb, int x, int y);
void        dobtb_Draw(dob_textbox_t *tb);
void        dobtb_SetFocus(dob_textbox_t *tb, bool focused);
void        dobtb_ClearModified(dob_textbox_t *tb);

/* Selection / clipboard. Return true if text or selection changed. */
bool        dobtb_HasSelection(const dob_textbox_t *tb);
void        dobtb_GetSelection(const dob_textbox_t *tb, int *a, int *b);
void        dobtb_ClearSelection(dob_textbox_t *tb);
bool        dobtb_SelectAll(dob_textbox_t *tb);
bool        dobtb_Copy(dob_textbox_t *tb);
bool        dobtb_Cut(dob_textbox_t *tb);
bool        dobtb_Paste(dob_textbox_t *tb);
bool        dobtb_Clear(dob_textbox_t *tb);
bool        dobtb_CopyAll(dob_textbox_t *tb);

/* Ctrl+C/X/V dispatcher. Defined in textbox_clip.c; programs that
 * don't link the clipboard add-on can ignore it. */
bool        dobtb_OnKeyClip(dob_textbox_t *tb, uint8_t key);

/*  *  Multi-Line TextBox
 *
 *  The text buffer is heap-allocated and grows on demand. Call
 *  dobmt_Free when the widget is no longer needed. Programs that
 *  run until process exit can skip the Free call — the kernel
 *  reclaims the heap on _exit().
 */

#define DOBMT_INITIAL_CAP   1024u
#define DOBMT_LINE_H        18
#define DOBMT_LEFT_PAD      8
#define DOBMT_LINENO_W      32
#define DOBMT_SCROLLBAR_W   10
#define DOBMT_COL_SCROLL    DOBUI_DISABLED

typedef struct
{
    uint32_t     win_id;
    int          x, y;
    int          w, h;
    bool         fill_mode;

    /* Heap-allocated text buffer, owned by the widget. `text[len]`
     * is always '\0'; before the first write both `text` and `cap`
     * may be zero/NULL. */
    char        *text;
    int          len;
    int          cap;
    int          cursor;

    int          scroll_line;
    bool         show_line_numbers;

    int          sel_anchor;
    bool         selecting;
    dob_multiclick_t mc;

    /* Line-offset cache. `line_offsets[i]` is the buffer offset of
     * the first character of logical line i. Rebuilt lazily when
     * `lines_dirty` is true. */
    int         *line_offsets;
    int          line_cap;
    int          total_lines;
    bool         lines_dirty;

    /* Word wrap.  When on, long logical lines are folded onto multiple
     * visual rows at the widget's right edge.  `wrap_offsets[i]` is
     * the buffer offset of visual row i (equal to line_offsets[] when
     * wrap is off).  Rebuilt lazily when wrap_dirty is true or when
     * the text width changed since the last rebuild. */
    bool         word_wrap;
    int         *wrap_offsets;
    int          wrap_cap;
    int          total_wrap_lines;
    bool         wrap_dirty;
    int          wrap_built_for_w;

    bool         visible;
    bool         enabled;
    bool         focused;
    bool         modified;

    /* Vertical scrollbar thumb drag (armed on press over the thumb). */
    bool         sb_drag;
    int          sb_grab;

    uint32_t     col_bg;
    uint32_t     col_text;
    uint32_t     col_cursor;
    uint32_t     col_border;
    uint32_t     col_border_focus;
    uint32_t     col_lineno_bg;
    uint32_t     col_lineno_fg;
    uint32_t     col_sel_bg;
    uint32_t     col_sel_text;
    uint32_t     col_scrollbar;
} dob_multitextbox_t;

void        dobmt_Init(dob_multitextbox_t *mt, uint32_t win_id,
                        int x, int y, int w, int h);
void        dobmt_InitFill(dob_multitextbox_t *mt, uint32_t win_id,
                            int x, int y);
void        dobmt_Free(dob_multitextbox_t *mt);
void        dobmt_SetSize(dob_multitextbox_t *mt, int win_w, int win_h);
void        dobmt_SetText(dob_multitextbox_t *mt, const char *text, int len);
const char *dobmt_GetText(dob_multitextbox_t *mt, int *out_len);
bool        dobmt_OnKey(dob_multitextbox_t *mt, uint8_t key);
bool        dobmt_OnClick(dob_multitextbox_t *mt, int x, int y);
bool        dobmt_OnDrag(dob_multitextbox_t *mt, int x, int y);
bool        dobmt_OnRelease(dob_multitextbox_t *mt);
bool        dobmt_OnDblClick(dob_multitextbox_t *mt, int x, int y);
bool        dobmt_OnScroll(dob_multitextbox_t *mt, int delta);
void        dobmt_Draw(dob_multitextbox_t *mt);
void        dobmt_SetFocus(dob_multitextbox_t *mt, bool focused);
int         dobmt_CursorLine(dob_multitextbox_t *mt);
int         dobmt_CursorColumn(dob_multitextbox_t *mt);

bool        dobmt_HasSelection(const dob_multitextbox_t *mt);
void        dobmt_GetSelection(const dob_multitextbox_t *mt, int *a, int *b);
void        dobmt_ClearSelection(dob_multitextbox_t *mt);
bool        dobmt_SelectAll(dob_multitextbox_t *mt);
bool        dobmt_Copy(dob_multitextbox_t *mt);
bool        dobmt_Cut(dob_multitextbox_t *mt);
bool        dobmt_Paste(dob_multitextbox_t *mt);
bool        dobmt_Clear(dob_multitextbox_t *mt);
bool        dobmt_CopyAll(dob_multitextbox_t *mt);

/* Toggle word wrap.  When ON, logical lines longer than the visible
 * text width fold onto multiple visual rows at the widget's right
 * edge.  Navigation (↑/↓, click, scroll) is by visual row;
 * dobmt_CursorLine/Column still report logical position. */
void        dobmt_SetWordWrap(dob_multitextbox_t *mt, bool on);
bool        dobmt_GetWordWrap(const dob_multitextbox_t *mt);

/* Ctrl+C/X/V dispatcher. Defined in textbox_clip.c. */
bool        dobmt_OnKeyClip(dob_multitextbox_t *mt, uint8_t key);

#endif
