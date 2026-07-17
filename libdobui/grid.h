/* DobUITools — DataGrid Control (N-column, editing-ready)
 *
 * Generalisation of dob_table_t: where the table widget is a fixed
 * two-column key/value viewer, this is an N-column data grid with a
 * cell cursor, resizable columns, vertical scrolling, column-step
 * horizontal scrolling, and optional frozen leftmost columns (pin a
 * DB ID column with dobgrid_SetFrozenColumns(g, 1)).
 *
 * Like the table, the grid is pure geometry + state: it knows nothing
 * about IPC, services, value types, or how the data was produced. It
 * renders cells as strings and reports two things to the host:
 *
 *   - the "current cell" (cur_row, cur_col), moved by clicks/arrows;
 *   - an ACTIVATE event (Enter or double-click on a cell) via the
 *     on_activate callback.
 *
 * That callback is the EDITING SEAM. The grid does not embed editors:
 * the host looks up the column's type, creates the right widget
 * (textbox / dropdown / checkbox / date editor), positions it over
 * dobgrid_CellRect(), runs the edit, writes the result back into its
 * own model, and redraws the grid. This keeps the grid reusable and
 * lets the same per-type editor code back both the grid and a form
 * view.
 *
 * Memory model: columns and cells are REFERENCED, not copied — same
 * convention as dob_table_t / dob_listview_t. The grid also writes the
 * live width back into cols[i].width when the user drags a divider, so
 * the host can read final widths to persist them.
 *
 * Focus: the grid does NOT register with the focus manager (there is
 * no DOB_CTRL_GRID tag, and registering as DOB_CTRL_TABLE would make
 * the focus dispatcher call the table handlers on a dob_grid_t). Drive
 * its events directly from your handlers — call dobgrid_OnClick /
 * OnDblClick / OnDrag / OnRelease / OnKey / OnScroll, and toggle
 * dobgrid_SetFocus() yourself. (To fold it into the global Tab/panel
 * cycle, add a DOB_CTRL_GRID case to dobui_common.h and focus.c.)
 *
 * Usage:
 *   static dobgrid_col_t cols[] = {
 *       { "ID",   48, DOBGRID_ALIGN_RIGHT },
 *       { "Nome", 0,  DOBGRID_ALIGN_LEFT  },   // 0 width => default
 *       { "Eta'", 0,  DOBGRID_ALIGN_RIGHT },
 *   };
 *   // flat, row-major, stride = ncols:  cell(r,c) = cells[r*3 + c]
 *   static const char *cells[] = {
 *       "1", "Anna",  "31",
 *       "2", "Bruno", "27",
 *   };
 *
 *   dob_grid_t g;
 *   dobgrid_Init(&g, win_id, 10, 40, 460, 300);
 *   dobgrid_SetColumns(&g, cols, 3);
 *   dobgrid_SetRows(&g, cells, 2);
 *   dobgrid_SetFrozenColumns(&g, 1);     // pin the ID column
 *   dobgrid_SetSelectable(&g, true);
 *   dobgrid_SetActivate(&g, on_cell_activate, &my_state);
 *   dobgrid_Draw(&g);
 */

#ifndef MAINDOB_DOBUITOOLS_GRID_H
#define MAINDOB_DOBUITOOLS_GRID_H

#include <dob/types.h>
#include "dobui_common.h"
#include "dobui_theme.h"

/* Layout */
#define DOBGRID_ROW_H         20
#define DOBGRID_HEADER_H      22
#define DOBGRID_PAD            6
#define DOBGRID_FONT_W         8     /* fixed-pitch glyph cell (DrawTextFixed) */
#define DOBGRID_FONT_H        16
#define DOBGRID_SCROLLBAR_W   10
#define DOBGRID_DIVIDER_W      1
#define DOBGRID_DIVIDER_HIT    6     /* grab zone width around a divider       */
#define DOBGRID_MIN_COL_W     30
#define DOBGRID_DEFAULT_COL_W 96

/* Per-column text alignment (cell + header). */
#define DOBGRID_ALIGN_LEFT     0
#define DOBGRID_ALIGN_CENTER   1
#define DOBGRID_ALIGN_RIGHT    2

/* Colors — see dobui_theme.h. Cell text is INPUT (giallo) because a
 * data grid holds user-entered content (style protocol: giallo = roba
 * dell'utente); headers are fixed labels (white on blue). Distinct
 * from dob_table_t, whose values are read-only debug output (cyan). */
#define DOBGRID_COL_BG            DOBUI_INSET     /* black data area          */
#define DOBGRID_COL_HEADER_BG     DOBUI_SURFACE   /* blue 170 header strip    */
#define DOBGRID_COL_HEADER_TEXT   DOBUI_TEXT_ALT  /* white header labels      */
#define DOBGRID_COL_TEXT          DOBUI_INPUT     /* giallo user data         */
#define DOBGRID_COL_SEL_BG        DOBUI_RELIEF    /* blue 255 current cell    */
#define DOBGRID_COL_SEL_TEXT      DOBUI_TEXT_ALT  /* white on selection       */
#define DOBGRID_COL_BORDER        DOBUI_SURFACE
#define DOBGRID_COL_BORDER_FOCUS  DOBUI_INPUT     /* giallo focus (protocol)  */
#define DOBGRID_COL_DIVIDER       DOBUI_DISABLED  /* column separators        */
#define DOBGRID_COL_DIVIDER_GRAB  DOBUI_RELIEF    /* active drag feedback     */
#define DOBGRID_COL_SCROLLBAR     DOBUI_DISABLED

/* One column. Caller-owned array; the grid references it and WRITES
 * BACK `width` on a divider drag. `title` is referenced too — keep it
 * alive while the grid is displayed. Pass width <= 0 to SetColumns to
 * have it defaulted to DOBGRID_DEFAULT_COL_W. */
typedef struct
{
    const char *title;   /* header label (NULL = empty header cell) */
    int         width;   /* pixels; <= 0 in SetColumns => default    */
    uint8_t     align;   /* DOBGRID_ALIGN_*                          */
} dobgrid_col_t;

struct dob_grid;

/* Fired on Enter / double-click on a cell. The host opens the right
 * editor for that column's type (see header comment). */
typedef void (*dobgrid_activate_fn)(struct dob_grid *g, int row, int col);

typedef struct dob_grid
{
    uint32_t win_id;
    int      x, y, w, h;

    /* Columns — caller-owned (see dobgrid_col_t). */
    dobgrid_col_t *cols;
    int            ncols;

    /* Cells — caller-owned flat array, row-major, stride = ncols:
     * cell(r,c) = cells[r * ncols + c]. NULL entry => empty cell. */
    const char *const *cells;
    int                nrows;

    int  row_h;
    int  header_h;       /* 0 until SetColumns; then DOBGRID_HEADER_H */
    int  frozen_cols;    /* leftmost N columns never scroll horizontally */

    /* The "current cell" (distinct from a row selection). -1 = none.
     * Only meaningful when selectable. */
    int  cur_row, cur_col;

    int  scroll;         /* first visible row  (vertical)                */
    int  hscroll;        /* first visible NON-frozen column (column-step) */

    bool visible, enabled, focused, selectable;
    int  grabbed_col;    /* column whose right divider is dragging, -1 idle */

    /* Scrollbar thumb drag. sb_vdrag / sb_hdrag mark which bar (if any)
     * is grabbed; sb_grab is the cursor offset within the thumb. */
    bool sb_vdrag, sb_hdrag;
    int  sb_grab;

    /* Editing seam. */
    dobgrid_activate_fn on_activate;
    void               *user;

    /* Theme (init from DOBGRID_COL_*; override per instance). */
    uint32_t col_bg, col_bg_alt, col_header_bg, col_header_text, col_text;
    uint32_t col_header_rule;
    uint32_t col_sel_bg, col_sel_text, col_border, col_border_focus;
    uint32_t col_divider, col_divider_grab, col_scrollbar;
} dob_grid_t;

/* Lifecycle */
void dobgrid_Init(dob_grid_t *g, uint32_t win_id, int x, int y, int w, int h);

/* Content. SetColumns defaults any width <= 0 and turns the header row
 * on. SetRows takes the flat cell array. Both reference caller memory;
 * neither resets the cursor or scroll beyond clamping to new bounds,
 * so a live grid can be refreshed without jumping to the top. */
void dobgrid_SetColumns(dob_grid_t *g, dobgrid_col_t *cols, int ncols);
void dobgrid_SetRows(dob_grid_t *g, const char *const *cells, int nrows);
void dobgrid_SetFrozenColumns(dob_grid_t *g, int n);

/* Behavior */
void dobgrid_SetSelectable(dob_grid_t *g, bool selectable);
void dobgrid_SetEnabled(dob_grid_t *g, bool enabled);
void dobgrid_SetFocus(dob_grid_t *g, bool focused);
void dobgrid_SetActivate(dob_grid_t *g, dobgrid_activate_fn cb, void *user);
void dobgrid_SetCurrent(dob_grid_t *g, int row, int col);

/* Queries */
int  dobgrid_CurRow(const dob_grid_t *g);
int  dobgrid_CurCol(const dob_grid_t *g);
const char *dobgrid_CellAt(const dob_grid_t *g, int row, int col);
int  dobgrid_VisibleRows(const dob_grid_t *g);

/* Pixel rect of a cell, for placing an inline editor over it. Returns
 * false when the cell is not currently visible (scrolled away, or its
 * column is off-screen / clipped). */
bool dobgrid_CellRect(const dob_grid_t *g, int row, int col,
                      int *out_x, int *out_y, int *out_w, int *out_h);

/* Hit test (whole bounding box). */
bool dobgrid_HitTest(const dob_grid_t *g, int x, int y);

/* Events — return true when consumed (mirror dob_table_t semantics).
 *   OnClick     start a divider drag, else move the cell cursor
 *               (revealing a clipped column).
 *   OnDblClick  move the cursor to the cell, then fire on_activate.
 *   OnDrag      resize the grabbed column.
 *   OnRelease   end a divider drag.
 *   OnKey       Left/Right move columns, Up/Down/PgUp/PgDn move rows,
 *               Home/End jump to first/last column; Enter activates.
 *   OnScroll    mouse wheel = vertical scroll. */
bool dobgrid_OnClick(dob_grid_t *g, int x, int y);
bool dobgrid_OnDblClick(dob_grid_t *g, int x, int y);
bool dobgrid_OnDrag(dob_grid_t *g, int x, int y);
void dobgrid_OnRelease(dob_grid_t *g);
bool dobgrid_OnKey(dob_grid_t *g, uint8_t key);
bool dobgrid_OnScroll(dob_grid_t *g, int delta);

/* Render. Call after any state change. */
void dobgrid_Draw(dob_grid_t *g);

#endif /* MAINDOB_DOBUITOOLS_GRID_H */
