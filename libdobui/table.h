/* DobUITools — Table Control
 *
 * Two-column scrollable key/value table.  Pure UI widget: knows nothing
 * about IPC, services, or how the data was produced.  Any program that
 * links libdobui can embed one in its own window — and the standalone
 * DobTable.mdl program is just one consumer of this widget that
 * happens to expose its API over IPC.
 *
 * Layout:
 *
 *   ┌────────────────┬──────────────────────────┐ ▲
 *   │ Header L       │ Header R                 │ │ header_h (optional)
 *   ├────────────────┼──────────────────────────┤
 *   │ key 0          │ value 0                  │
 *   │ key 1          │ value 1                  │ ░ scrollbar (auto)
 *   │ key 2          │ value 2                  │ ░
 *   │ ...            │ ...                      │
 *   └────────────────┴──────────────────────────┘
 *                    ↑
 *             draggable divider (cursor → ↔)
 *
 * Memory model: rows are referenced, not copied.  Caller owns the key
 * and value string arrays and must keep them alive as long as the
 * table is displayed.  Same convention as dob_listview_t.
 *
 * Usage:
 *   static const char *keys[]   = { "Nome", "Dimensione", "Tipo" };
 *   static const char *values[] = { "report.pdf", "412 KB", "PDF" };
 *
 *   dob_table_t t;
 *   dobtbl_Init(&t, win_id, 10, 40, 400, 300);
 *   dobtbl_SetHeaders(&t, "Proprietà", "Valore");
 *   dobtbl_SetRows(&t, keys, values, 3);
 *   dobtbl_Draw(&t);
 *
 *   // The widget's OnClick/OnRelease install and clear the
 *   // CURSOR_HSPLIT override automatically — no host-program code
 *   // is needed for the ←→ cursor on the divider.
 */

#ifndef MAINDOB_DOBUITOOLS_TABLE_H
#define MAINDOB_DOBUITOOLS_TABLE_H

#include <dob/types.h>
#include "dobui_common.h"
#include "dobui_theme.h"

/* Layout */
#define DOBTBL_ROW_H        20
#define DOBTBL_HEADER_H     22
#define DOBTBL_PAD          6
#define DOBTBL_FONT_W       8
#define DOBTBL_FONT_H       16
#define DOBTBL_SCROLLBAR_W  10

/* Divider: visual line is 1 px, but the hit zone is wider so the user
 * can grab it without pixel-perfect aim. */
#define DOBTBL_DIVIDER_W    1
#define DOBTBL_DIVIDER_HIT  6

/* Minimum width of either column. Prevents the user from collapsing
 * one column to nothing by dragging the divider all the way over. */
#define DOBTBL_MIN_COL_W    30

/* Default ratio of key column width to total content width, in tenths.
 * 4 = 40 % keys / 60 % values.  Override per-instance with
 * dobtbl_SetKeyColumnWidth(). */
#define DOBTBL_DEFAULT_KEY_RATIO    4

/* Colors */
#define DOBTBL_COL_BG               DOBUI_INSET
#define DOBTBL_COL_HEADER_BG        DOBUI_SURFACE
#define DOBTBL_COL_HEADER_TEXT      DOBUI_TEXT_ALT
#define DOBTBL_COL_TEXT             DOBUI_TEXT
#define DOBTBL_COL_SEL_BG           DOBUI_RELIEF
#define DOBTBL_COL_SEL_TEXT         DOBUI_TEXT_ALT
#define DOBTBL_COL_BORDER           DOBUI_SURFACE
#define DOBTBL_COL_BORDER_FOCUS     DOBUI_TEXT
#define DOBTBL_COL_DIVIDER          DOBUI_DISABLED
#define DOBTBL_COL_DIVIDER_GRAB     DOBUI_RELIEF
#define DOBTBL_COL_SCROLLBAR        DOBUI_DISABLED

typedef struct
{
    uint32_t    win_id;
    int         x, y;
    int         w, h;

    /* Row data — caller-owned. Two parallel arrays, both `count`
     * entries long. Either array may be NULL only if count == 0. */
    const char **keys;
    const char **values;
    int         count;

    /* Optional headers. Set to NULL to hide the header row entirely;
     * the header_h slot is then absent from the layout. */
    const char *header_key;
    const char *header_value;

    /* Layout state */
    int         key_col_w;       /* Pixels reserved for the key column. */
    int         row_h;
    int         header_h;        /* 0 when no headers */

    /* Selection (only used when selectable=true) */
    int         selected;        /* -1 = none */
    int         scroll;          /* First visible row index */

    /* Behavior flags */
    bool        visible;
    bool        enabled;
    bool        focused;
    bool        selectable;      /* If false, clicks don't select rows */
    bool        divider_grabbed; /* Active drag of the column splitter */
    bool        sb_drag;         /* Active drag of the vertical scrollbar */
    int         sb_grab;         /* Cursor offset within the thumb at grab */

    /* Theme */
    uint32_t    col_bg;
    uint32_t    col_header_bg;
    uint32_t    col_header_text;
    uint32_t    col_text;
    uint32_t    col_sel_bg;
    uint32_t    col_sel_text;
    uint32_t    col_border;
    uint32_t    col_border_focus;
    uint32_t    col_divider;
    uint32_t    col_divider_grab;
    uint32_t    col_scrollbar;
} dob_table_t;

/* Lifecycle */
void dobtbl_Init(dob_table_t *t, uint32_t win_id,
                 int x, int y, int w, int h);

/* Content. SetRows takes two parallel arrays; the table references
 * them directly, so the caller must keep both alive until the table
 * is destroyed or SetRows is called again. */
void dobtbl_SetHeaders(dob_table_t *t, const char *key, const char *value);
void dobtbl_SetRows(dob_table_t *t, const char **keys,
                    const char **values, int count);

/* Layout overrides */
void dobtbl_SetKeyColumnWidth(dob_table_t *t, int px);

/* Behavior */
void dobtbl_SetSelectable(dob_table_t *t, bool selectable);
void dobtbl_SetSelected(dob_table_t *t, int index);
void dobtbl_SetEnabled(dob_table_t *t, bool enabled);
void dobtbl_SetFocus(dob_table_t *t, bool focused);

/* Queries */
int  dobtbl_GetSelectedIndex(const dob_table_t *t);
const char *dobtbl_GetSelectedKey(const dob_table_t *t);
const char *dobtbl_GetSelectedValue(const dob_table_t *t);
int  dobtbl_VisibleCount(const dob_table_t *t);

/* Hit-tests */
bool dobtbl_HitTest(const dob_table_t *t, int x, int y);
bool dobtbl_HitTestDivider(const dob_table_t *t, int x, int y);

/* Event handlers — return true when the event was consumed.
 *
 *   OnClick     starts a divider drag if the click landed on the
 *               splitter; otherwise selects a row (when selectable).
 *   OnDrag      while the divider is grabbed, updates key_col_w from
 *               the cursor x. No-op otherwise.
 *   OnRelease   ends the divider drag.
 *   OnKey       Up/Down/PgUp/PgDn/Home/End move the selection (when
 *               selectable and focused).
 *   OnScroll    mouse wheel; positive delta scrolls down. */
bool dobtbl_OnClick(dob_table_t *t, int x, int y);
bool dobtbl_OnDrag(dob_table_t *t, int x, int y);
void dobtbl_OnRelease(dob_table_t *t);
bool dobtbl_OnKey(dob_table_t *t, uint8_t key);
bool dobtbl_OnScroll(dob_table_t *t, int delta);

/* Render. Call after any state change. */
void dobtbl_Draw(dob_table_t *t);

#endif
