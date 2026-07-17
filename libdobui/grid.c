/* DobUITools — DataGrid Implementation
 *
 * Geometry + state; rendering via the standard dobui primitives that
 * write straight into the window SHM framebuffer (zero IPC per pixel).
 * All event handlers are pure computation on the dob_grid_t struct.
 *
 * Horizontal scrolling is column-granular: hscroll is the index of the
 * first visible NON-frozen column, so the leftmost visible scrolled
 * column always starts on a clean boundary (no mid-column left-edge
 * math). The rightmost visible column is clipped by reducing its
 * drawable width. Scrollbars are indicators only — vertical scroll is
 * mouse-wheel + Up/Down/PgUp/PgDn, horizontal is Left/Right (the cell
 * cursor auto-reveals its column) plus a click on a clipped column.
 * The divider drag follows the Click -> Drag -> Release pattern from
 * dob_table_t / dob_slider_t. */

#include "grid.h"
#include "scrollbar.h"
#include <DobInterface.h>
#include <string.h>

/* Special key codes — must match the input layer (libdob) and app.h.
 * app.h declares KEY_PGUP = 135 and KEY_PGDN = 136; note that
 * dob_table_t's inline comments have these two swapped. We follow
 * app.h, the application-framework contract. If the input daemon
 * actually delivers the opposite, swap the PGUP/PGDN values here (and
 * fix table.c to match). Kept as local names so the widget stays
 * independent of app.h, which is the app framework, not a widget dep. */
#define GRID_KEY_UP     128
#define GRID_KEY_DOWN   129
#define GRID_KEY_LEFT   130
#define GRID_KEY_RIGHT  131
#define GRID_KEY_HOME   132
#define GRID_KEY_END    133
#define GRID_KEY_PGUP   135
#define GRID_KEY_PGDN   136

/* ---- Derived metrics ------------------------------------------------ */

typedef struct
{
    int  content_w;     /* drawing width excluding the vertical scrollbar  */
    int  rows_area_h;   /* height of the row band (below the header)       */
    int  vis;           /* number of rows that fit in rows_area_h          */
    bool vsb;           /* vertical scrollbar needed                       */
    bool hsb;           /* horizontal scrollbar needed                     */
} grid_metrics_t;

static int grid_clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int grid_frozen(const dob_grid_t *g)
{
    return grid_clamp_int(g->frozen_cols, 0, g->ncols);
}

static grid_metrics_t grid_measure(const dob_grid_t *g)
{
    grid_metrics_t m;
    int row_h = (g->row_h > 0) ? g->row_h : DOBGRID_ROW_H;
    int nf    = grid_frozen(g);

    int total_w = 0;
    if (g->cols)
        for (int c = 0; c < g->ncols; c++) total_w += g->cols[c].width;

    /* Decide the horizontal scrollbar from the full width (ignoring the
     * vertical one); the small coupling is not worth a fixed-point
     * iteration — same pragmatic call as dob_table_t. */
    m.hsb = (g->ncols > nf) && (total_w > g->w);

    m.rows_area_h = g->h - g->header_h - (m.hsb ? DOBGRID_SCROLLBAR_W : 0);
    if (m.rows_area_h < 0) m.rows_area_h = 0;

    m.vis = m.rows_area_h / row_h;
    if (m.vis < 1) m.vis = 1;

    m.vsb = (g->nrows > m.vis);
    m.content_w = g->w - (m.vsb ? DOBGRID_SCROLLBAR_W : 0);
    if (m.content_w < 0) m.content_w = 0;
    return m;
}

/* Count non-frozen columns that get at least a sliver of screen from
 * the current hscroll — replicates dobgrid_Draw's column loop so the
 * horizontal thumb's hit-test matches what is painted. */
static int grid_vis_nonfrozen(const dob_grid_t *g, const grid_metrics_t *m)
{
    int nf    = grid_frozen(g);
    int right = g->x + m->content_w;
    int x     = g->x;
    for (int c = 0; c < nf && x < right; c++) x += g->cols[c].width;
    int n = 0;
    for (int c = nf + g->hscroll; c < g->ncols && x < right; c++)
    {
        x += g->cols[c].width;
        n++;
    }
    return (n < 1) ? 1 : n;
}

/* Vertical scrollbar geometry (rows). False when no bar is shown. */
static bool grid_vsb_geom(const dob_grid_t *g, const grid_metrics_t *m,
                          dob_scroll1d_t *sg, int *sb_x)
{
    if (!m->vsb) return false;
    *sb_x = g->x + m->content_w;
    *sg = dob_scroll1d(g->y + g->header_h, m->rows_area_h,
                       g->nrows, m->vis, g->scroll);
    return sg->max_scroll > 0;
}

/* Horizontal scrollbar geometry (non-frozen columns, column-stepped).
 * False when no bar is shown. */
static bool grid_hsb_geom(const dob_grid_t *g, const grid_metrics_t *m,
                          dob_scroll1d_t *sg, int *sb_y)
{
    if (!m->hsb) return false;
    int nf       = grid_frozen(g);
    int total_nf = g->ncols - nf;
    if (total_nf < 1) total_nf = 1;
    int vis_nf   = grid_vis_nonfrozen(g, m);
    *sb_y = g->y + g->header_h + m->rows_area_h;
    *sg = dob_scroll1d(g->x, m->content_w, total_nf, vis_nf, g->hscroll);
    return sg->max_scroll > 0;
}

/* Screen x and drawable width of absolute column `col`. Returns false
 * when the column is not currently visible (scrolled off, or clipped
 * to zero width at the right edge). */
static bool grid_col_screen(const dob_grid_t *g, int col, int *sx, int *dw)
{
    if (!g->cols || col < 0 || col >= g->ncols) return false;

    grid_metrics_t m = grid_measure(g);
    int right = g->x + m.content_w;
    int nf    = grid_frozen(g);
    int x     = g->x;

    if (col < nf)
    {
        for (int c = 0; c < col; c++) x += g->cols[c].width;
        if (x >= right) return false;
        int avail = right - x;
        int cw = g->cols[col].width;
        *sx = x; *dw = (cw < avail) ? cw : avail;
        return (*dw > 0);
    }

    for (int c = 0; c < nf; c++) x += g->cols[c].width;
    int first = nf + g->hscroll;
    if (col < first) return false;
    for (int c = first; c < col; c++)
    {
        x += g->cols[c].width;
        if (x >= right) return false;
    }
    if (x >= right) return false;
    int avail = right - x;
    int cw = g->cols[col].width;
    *sx = x; *dw = (cw < avail) ? cw : avail;
    return (*dw > 0);
}

/* ---- Small helpers -------------------------------------------------- */

static const char *grid_cell(const dob_grid_t *g, int row, int col)
{
    if (!g->cells) return NULL;
    if (row < 0 || row >= g->nrows) return NULL;
    if (col < 0 || col >= g->ncols) return NULL;
    return g->cells[(size_t)row * (size_t)g->ncols + (size_t)col];
}

/* Truncate `src` into `dst` (NUL-terminated) so it fits in max_chars,
 * adding ".." when truncation happens. No allocation. */
static int grid_fit(char *dst, int dst_cap, const char *src, int max_chars)
{
    if (dst_cap <= 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }

    int len = (int)strlen(src);
    if (max_chars < 1) max_chars = 1;

    if (len <= max_chars)
    {
        int n = (len < dst_cap - 1) ? len : dst_cap - 1;
        memcpy(dst, src, (uint32_t)n);
        dst[n] = '\0';
        return n;
    }
    if (max_chars < 3)
    {
        int n = (max_chars < dst_cap - 1) ? max_chars : dst_cap - 1;
        memcpy(dst, src, (uint32_t)n);
        dst[n] = '\0';
        return n;
    }
    int keep = max_chars - 2;
    if (keep > dst_cap - 3) keep = dst_cap - 3;
    memcpy(dst, src, (uint32_t)keep);
    dst[keep]     = '.';
    dst[keep + 1] = '.';
    dst[keep + 2] = '\0';
    return keep + 2;
}

/* Draw aligned, truncated text inside a box (header cell or data cell).
 * Background is assumed already painted by the caller. */
static void grid_cell_text(dob_grid_t *g, int bx, int by, int bw, int bh,
                           const char *text, uint8_t align,
                           uint32_t fg, uint32_t bg)
{
    int cap = (bw - DOBGRID_PAD * 2) / DOBGRID_FONT_W;
    if (cap < 1) cap = 1;

    char buf[256];
    int n   = grid_fit(buf, (int)sizeof(buf), text, cap);
    int tpx = n * DOBGRID_FONT_W;

    int ox;
    switch (align)
    {
        case DOBGRID_ALIGN_CENTER: ox = bx + (bw - tpx) / 2;            break;
        case DOBGRID_ALIGN_RIGHT:  ox = bx + bw - DOBGRID_PAD - tpx;    break;
        default:                   ox = bx + DOBGRID_PAD;               break;
    }
    if (ox < bx + DOBGRID_PAD) ox = bx + DOBGRID_PAD;

    int oy = by + (bh - DOBGRID_FONT_H) / 2;
    dobui_DrawTextFixed(g->win_id, ox, oy, buf, fg, bg);
}

/* ---- Clamp / ensure-visible ----------------------------------------- */

static void grid_clamp(dob_grid_t *g)
{
    grid_metrics_t m = grid_measure(g);
    int nf = grid_frozen(g);

    int max_s = g->nrows - m.vis;
    if (max_s < 0) max_s = 0;
    g->scroll = grid_clamp_int(g->scroll, 0, max_s);

    int max_h = (g->ncols - nf) - 1;
    if (max_h < 0) max_h = 0;
    g->hscroll = grid_clamp_int(g->hscroll, 0, max_h);

    if (g->cur_row >= g->nrows) g->cur_row = g->nrows - 1;
    if (g->cur_row < -1)        g->cur_row = -1;
    if (g->cur_col >= g->ncols) g->cur_col = g->ncols - 1;
    if (g->cur_col < -1)        g->cur_col = -1;
}

static void grid_ensure_row_visible(dob_grid_t *g)
{
    if (g->cur_row < 0) return;
    grid_metrics_t m = grid_measure(g);
    if (g->cur_row < g->scroll)
        g->scroll = g->cur_row;
    else if (g->cur_row >= g->scroll + m.vis)
        g->scroll = g->cur_row - m.vis + 1;
    if (g->scroll < 0) g->scroll = 0;
}

static void grid_ensure_col_visible(dob_grid_t *g)
{
    if (g->cur_col < 0) return;
    int nf = grid_frozen(g);
    if (g->cur_col < nf) return;                 /* frozen: always shown */

    if (g->cur_col < nf + g->hscroll)            /* off the left          */
    {
        g->hscroll = g->cur_col - nf;
        if (g->hscroll < 0) g->hscroll = 0;
        return;
    }
    /* Scroll right until the column is fully visible (bounded loop). */
    int guard = g->ncols + 1;
    while (guard-- > 0 && g->hscroll < g->cur_col - nf)
    {
        int sx, dw;
        if (grid_col_screen(g, g->cur_col, &sx, &dw)
            && dw >= g->cols[g->cur_col].width)
            break;
        g->hscroll++;
    }
}

/* ---- Hit testing ---------------------------------------------------- */

/* Absolute column index under screen x, or -1. */
static int grid_col_at(const dob_grid_t *g, int px)
{
    grid_metrics_t m = grid_measure(g);
    int right = g->x + m.content_w;
    int nf    = grid_frozen(g);
    int x     = g->x;

    for (int c = 0; c < nf && x < right; c++)
    {
        int cw = g->cols[c].width;
        int rx = x + cw; if (rx > right) rx = right;
        if (px >= x && px < rx) return c;
        x += cw;
    }
    int first = nf + g->hscroll;
    for (int c = first; c < g->ncols && x < right; c++)
    {
        int cw = g->cols[c].width;
        int rx = x + cw; if (rx > right) rx = right;
        if (px >= x && px < rx) return c;
        x += cw;
    }
    return -1;
}

/* Column whose right divider is under (px,py), or -1. */
static int grid_divider_at(const dob_grid_t *g, int px, int py)
{
    if (py < g->y || py >= g->y + g->h) return -1;

    grid_metrics_t m = grid_measure(g);
    int right = g->x + m.content_w;
    int nf    = grid_frozen(g);
    int half  = DOBGRID_DIVIDER_HIT / 2;
    int x     = g->x;

    for (int c = 0; c < nf && x < right; c++)
    {
        x += g->cols[c].width;
        if (x <= right && px >= x - half && px < x + half) return c;
    }
    int first = nf + g->hscroll;
    for (int c = first; c < g->ncols && x < right; c++)
    {
        x += g->cols[c].width;
        if (x <= right && px >= x - half && px < x + half) return c;
    }
    return -1;
}

/* ---- Public API ----------------------------------------------------- */

void dobgrid_Init(dob_grid_t *g, uint32_t win_id, int x, int y, int w, int h)
{
    memset(g, 0, sizeof(*g));
    g->win_id           = win_id;
    g->x                = x;
    g->y                = y;
    g->w                = w;
    g->h                = h;
    g->row_h            = DOBGRID_ROW_H;
    g->header_h         = 0;
    g->frozen_cols      = 0;
    g->cur_row          = -1;
    g->cur_col          = -1;
    g->grabbed_col      = -1;
    g->visible          = true;
    g->enabled          = true;
    g->selectable       = false;
    g->col_bg           = DOBGRID_COL_BG;
    g->col_bg_alt       = DOBGRID_COL_BG;   /* uguale a col_bg => niente zebra di default */
    g->col_header_rule  = DOBGRID_COL_HEADER_BG; /* invisibile finche non impostato */
    g->col_header_bg    = DOBGRID_COL_HEADER_BG;
    g->col_header_text  = DOBGRID_COL_HEADER_TEXT;
    g->col_text         = DOBGRID_COL_TEXT;
    g->col_sel_bg       = DOBGRID_COL_SEL_BG;
    g->col_sel_text     = DOBGRID_COL_SEL_TEXT;
    g->col_border       = DOBGRID_COL_BORDER;
    g->col_border_focus = DOBGRID_COL_BORDER_FOCUS;
    g->col_divider      = DOBGRID_COL_DIVIDER;
    g->col_divider_grab = DOBGRID_COL_DIVIDER_GRAB;
    g->col_scrollbar    = DOBGRID_COL_SCROLLBAR;
    /* Deliberately NOT auto-registered with the focus manager — see
     * the header. The host drives dobgrid_On* directly. */
}

void dobgrid_SetColumns(dob_grid_t *g, dobgrid_col_t *cols, int ncols)
{
    g->cols  = cols;
    g->ncols = (ncols < 0) ? 0 : ncols;
    if (g->cols)
        for (int c = 0; c < g->ncols; c++)
            if (g->cols[c].width <= 0) g->cols[c].width = DOBGRID_DEFAULT_COL_W;
    g->header_h = (g->ncols > 0) ? DOBGRID_HEADER_H : 0;
    if (g->frozen_cols > g->ncols) g->frozen_cols = g->ncols;
    grid_clamp(g);
}

void dobgrid_SetRows(dob_grid_t *g, const char *const *cells, int nrows)
{
    g->cells = cells;
    g->nrows = (nrows < 0) ? 0 : nrows;
    grid_clamp(g);
}

void dobgrid_SetFrozenColumns(dob_grid_t *g, int n)
{
    g->frozen_cols = grid_clamp_int(n, 0, g->ncols);
    grid_clamp(g);
}

void dobgrid_SetSelectable(dob_grid_t *g, bool selectable)
{
    g->selectable = selectable;
    if (!selectable) { g->cur_row = -1; g->cur_col = -1; }
}

void dobgrid_SetEnabled(dob_grid_t *g, bool enabled) { g->enabled = enabled; }
void dobgrid_SetFocus(dob_grid_t *g, bool focused)   { g->focused = focused; }

void dobgrid_SetActivate(dob_grid_t *g, dobgrid_activate_fn cb, void *user)
{
    g->on_activate = cb;
    g->user        = user;
}

void dobgrid_SetCurrent(dob_grid_t *g, int row, int col)
{
    if (!g->selectable) return;
    if (row >= -1 && row < g->nrows) g->cur_row = row;
    if (col >= -1 && col < g->ncols) g->cur_col = col;
    grid_ensure_row_visible(g);
    grid_ensure_col_visible(g);
}

int dobgrid_CurRow(const dob_grid_t *g) { return g->cur_row; }
int dobgrid_CurCol(const dob_grid_t *g) { return g->cur_col; }

const char *dobgrid_CellAt(const dob_grid_t *g, int row, int col)
{
    return grid_cell(g, row, col);
}

int dobgrid_VisibleRows(const dob_grid_t *g)
{
    grid_metrics_t m = grid_measure(g);
    return m.vis;
}

bool dobgrid_CellRect(const dob_grid_t *g, int row, int col,
                      int *out_x, int *out_y, int *out_w, int *out_h)
{
    if (row < 0 || row >= g->nrows) return false;

    grid_metrics_t m = grid_measure(g);
    int rel = row - g->scroll;
    if (rel < 0 || rel >= m.vis) return false;

    int sx, dw;
    if (!grid_col_screen(g, col, &sx, &dw)) return false;

    int row_h = (g->row_h > 0) ? g->row_h : DOBGRID_ROW_H;
    *out_x = sx;
    *out_y = g->y + g->header_h + rel * row_h;
    *out_w = dw;
    *out_h = row_h;
    return true;
}

bool dobgrid_HitTest(const dob_grid_t *g, int x, int y)
{
    return x >= g->x && x < g->x + g->w
        && y >= g->y && y < g->y + g->h;
}

bool dobgrid_OnClick(dob_grid_t *g, int x, int y)
{
    if (!g->visible || !g->enabled)   return false;
    if (!dobgrid_HitTest(g, x, y))    return false;

    /* Divider takes priority — it overlaps cell hit areas. */
    int dcol = grid_divider_at(g, x, y);
    if (dcol >= 0)
    {
        g->grabbed_col = dcol;
        dobui_SetCursor(g->win_id, CURSOR_HSPLIT);
        return true;
    }

    /* Scrollbars: a press on either bar drives it, never the cells. */
    {
        grid_metrics_t m = grid_measure(g);
        dob_scroll1d_t sg;
        int sb_x, sb_y;

        if (grid_vsb_geom(g, &m, &sg, &sb_x)
            && x >= sb_x && x < sb_x + DOBGRID_SCROLLBAR_W
            && dob_scroll1d_hit_track(&sg, y))
        {
            if (dob_scroll1d_hit_thumb(&sg, y))
            {
                g->sb_vdrag = true;
                g->sb_grab  = y - sg.thumb_off;
            }
            else
            {
                g->scroll += (y < sg.thumb_off) ? -m.vis : m.vis;
                grid_clamp(g);
            }
            return true;
        }

        if (grid_hsb_geom(g, &m, &sg, &sb_y)
            && y >= sb_y && y < sb_y + DOBGRID_SCROLLBAR_W
            && dob_scroll1d_hit_track(&sg, x))
        {
            if (dob_scroll1d_hit_thumb(&sg, x))
            {
                g->sb_hdrag = true;
                g->sb_grab  = x - sg.thumb_off;
            }
            else
            {
                int vis_nf = grid_vis_nonfrozen(g, &m);
                g->hscroll += (x < sg.thumb_off) ? -vis_nf : vis_nf;
                grid_clamp(g);
            }
            return true;
        }
    }

    /* Header row: consume, never selects. */
    if (g->header_h > 0 && y < g->y + g->header_h) return true;
    if (!g->selectable) return true;

    int col   = grid_col_at(g, x);
    int row_h = (g->row_h > 0) ? g->row_h : DOBGRID_ROW_H;
    int rel_y = y - g->y - g->header_h;
    int idx   = (rel_y >= 0) ? (rel_y / row_h + g->scroll) : -1;

    if (col >= 0 && idx >= 0 && idx < g->nrows)
    {
        g->cur_col = col;
        g->cur_row = idx;
        grid_ensure_col_visible(g);   /* reveal a clipped column on click */
    }
    return true;
}

bool dobgrid_OnDblClick(dob_grid_t *g, int x, int y)
{
    if (!g->visible || !g->enabled)   return false;
    if (!dobgrid_HitTest(g, x, y))    return false;
    if (g->header_h > 0 && y < g->y + g->header_h) return true;
    if (!g->selectable) return true;

    int col   = grid_col_at(g, x);
    int row_h = (g->row_h > 0) ? g->row_h : DOBGRID_ROW_H;
    int rel_y = y - g->y - g->header_h;
    int idx   = (rel_y >= 0) ? (rel_y / row_h + g->scroll) : -1;

    if (col >= 0 && idx >= 0 && idx < g->nrows)
    {
        g->cur_col = col;
        g->cur_row = idx;
        grid_ensure_col_visible(g);
        if (g->on_activate) g->on_activate(g, g->cur_row, g->cur_col);
    }
    return true;
}

bool dobgrid_OnDrag(dob_grid_t *g, int x, int y)
{
    if (g->sb_vdrag)
    {
        grid_metrics_t m = grid_measure(g);
        dob_scroll1d_t sg;
        int sb_x;
        if (!grid_vsb_geom(g, &m, &sg, &sb_x)) { g->sb_vdrag = false; return false; }
        g->scroll = dob_scroll1d_from_pos(&sg, y, g->sb_grab);
        return true;
    }

    if (g->sb_hdrag)
    {
        grid_metrics_t m = grid_measure(g);
        dob_scroll1d_t sg;
        int sb_y;
        if (!grid_hsb_geom(g, &m, &sg, &sb_y)) { g->sb_hdrag = false; return false; }
        g->hscroll = dob_scroll1d_from_pos(&sg, x, g->sb_grab);
        return true;
    }

    if (g->grabbed_col < 0) return false;

    int sx, dw;
    if (grid_col_screen(g, g->grabbed_col, &sx, &dw))
    {
        int nw = x - sx;
        if (nw < DOBGRID_MIN_COL_W) nw = DOBGRID_MIN_COL_W;
        g->cols[g->grabbed_col].width = nw;
    }
    return true;
}

void dobgrid_OnRelease(dob_grid_t *g)
{
    g->sb_vdrag = false;
    g->sb_hdrag = false;
    if (g->grabbed_col >= 0)
    {
        g->grabbed_col = -1;
        dobui_SetCursor(g->win_id, CURSOR_DEFAULT);
    }
}

bool dobgrid_OnKey(dob_grid_t *g, uint8_t key)
{
    if (!g->visible || !g->enabled || !g->focused) return false;
    if (!g->selectable || g->ncols == 0)           return false;

    if (g->cur_row < 0 && g->nrows > 0) g->cur_row = 0;
    if (g->cur_col < 0)                 g->cur_col = 0;

    grid_metrics_t m = grid_measure(g);

    switch (key)
    {
        case GRID_KEY_DOWN:
            if (g->cur_row < g->nrows - 1) g->cur_row++;
            grid_ensure_row_visible(g);
            return true;

        case GRID_KEY_UP:
            if (g->cur_row > 0) g->cur_row--;
            grid_ensure_row_visible(g);
            return true;

        case GRID_KEY_RIGHT:
            if (g->cur_col < g->ncols - 1) g->cur_col++;
            grid_ensure_col_visible(g);
            return true;

        case GRID_KEY_LEFT:
            if (g->cur_col > 0) g->cur_col--;
            grid_ensure_col_visible(g);
            return true;

        case GRID_KEY_PGDN:
            g->cur_row += m.vis;
            if (g->cur_row >= g->nrows) g->cur_row = g->nrows - 1;
            grid_ensure_row_visible(g);
            return true;

        case GRID_KEY_PGUP:
            g->cur_row -= m.vis;
            if (g->cur_row < 0) g->cur_row = 0;
            grid_ensure_row_visible(g);
            return true;

        case GRID_KEY_HOME:   /* first column */
            g->cur_col = 0;
            grid_ensure_col_visible(g);
            return true;

        case GRID_KEY_END:    /* last column */
            g->cur_col = g->ncols - 1;
            grid_ensure_col_visible(g);
            return true;

        case '\n':
        case '\r':
            if (g->on_activate && g->cur_row >= 0 && g->cur_row < g->nrows)
                g->on_activate(g, g->cur_row, g->cur_col);
            return true;

        default:
            return false;
    }
}

bool dobgrid_OnScroll(dob_grid_t *g, int delta)
{
    if (!g->visible || !g->enabled) return false;
    g->scroll += delta;
    grid_clamp(g);
    return true;
}

/* ---- Render --------------------------------------------------------- */

static void grid_draw_column(dob_grid_t *g, const grid_metrics_t *m,
                             int col, int sx, int draw_w)
{
    uint8_t align = g->cols[col].align;

    /* Header cell (strip background already filled by dobgrid_Draw). */
    if (g->header_h > 0 && g->cols[col].title)
        grid_cell_text(g, sx, g->y, draw_w, g->header_h,
                       g->cols[col].title, align,
                       g->col_header_text, g->col_header_bg);

    /* Data rows. */
    int rows_y0 = g->y + g->header_h;
    int row_h   = (g->row_h > 0) ? g->row_h : DOBGRID_ROW_H;

    for (int vi = 0; vi < m->vis; vi++)
    {
        int idx = vi + g->scroll;
        if (idx >= g->nrows) break;

        int ry = rows_y0 + vi * row_h;
        bool cur = (g->selectable && idx == g->cur_row && col == g->cur_col);
        uint32_t bg = cur ? g->col_sel_bg
                          : ((idx & 1) ? g->col_bg_alt : g->col_bg);
        uint32_t fg = cur ? g->col_sel_text : g->col_text;

        dobui_FillRect(g->win_id, sx, ry, draw_w, row_h, bg);

        const char *s = grid_cell(g, idx, col);
        if (s)
            grid_cell_text(g, sx, ry, draw_w, row_h, s, align, fg, bg);
    }

    /* Right separator / resize divider, spanning header + rows. Thicker
     * and highlighted while its drag is active. */
    int span_h    = g->header_h + m->rows_area_h;
    bool grabbed  = (g->grabbed_col == col);
    uint32_t dcol = grabbed ? g->col_divider_grab : g->col_divider;
    int dw        = grabbed ? 2 : DOBGRID_DIVIDER_W;
    int dx        = sx + draw_w - dw;
    if (dx < sx) dx = sx;
    dobui_FillRect(g->win_id, dx, g->y, dw, span_h, dcol);
}

void dobgrid_Draw(dob_grid_t *g)
{
    if (!g->visible) return;

    grid_clamp(g);
    grid_metrics_t m = grid_measure(g);

    /* Whole-widget background first — subsequent passes overwrite what
     * they own; avoids striping when row/column counts shrink. */
    dobui_FillRect(g->win_id, g->x, g->y, g->w, g->h, g->col_bg);

    /* Header strip background across the content width. */
    if (g->header_h > 0)
        dobui_FillRect(g->win_id, g->x, g->y, m.content_w, g->header_h,
                       g->col_header_bg);

    int right = g->x + m.content_w;
    int nf    = grid_frozen(g);

    /* Frozen columns first (never horizontally scrolled). */
    int x = g->x;
    for (int c = 0; c < nf && x < right; c++)
    {
        int cw = g->cols[c].width;
        int dw = (cw < right - x) ? cw : (right - x);
        grid_draw_column(g, &m, c, x, dw);
        x += cw;
    }

    /* Scrolled columns, from the first visible non-frozen one. */
    int first = nf + g->hscroll;
    int n_vis_nonfrozen = 0;
    for (int c = first; c < g->ncols && x < right; c++)
    {
        int cw = g->cols[c].width;
        int dw = (cw < right - x) ? cw : (right - x);
        grid_draw_column(g, &m, c, x, dw);
        x += cw;
        n_vis_nonfrozen++;
    }

    int rows_y0 = g->y + g->header_h;

    /* Vertical scrollbar (now grab-draggable; wheel/keys still work). */
    if (m.vsb)
    {
        int sb_x = g->x + m.content_w;
        int sb_h = m.rows_area_h;
        dobui_FillRect(g->win_id, sb_x, rows_y0,
                       DOBGRID_SCROLLBAR_W, sb_h, g->col_bg);

        dob_scroll1d_t sg = dob_scroll1d(rows_y0, sb_h,
                                         g->nrows, m.vis, g->scroll);
        dobui_FillRect(g->win_id, sb_x + 1, sg.thumb_off,
                       DOBGRID_SCROLLBAR_W - 2, sg.thumb_len, g->col_scrollbar);
    }

    /* Horizontal scrollbar (now grab-draggable; Left/Right still work). */
    if (m.hsb)
    {
        int total_nf = g->ncols - nf;
        if (total_nf < 1) total_nf = 1;
        int vis_nf = (n_vis_nonfrozen < 1) ? 1 : n_vis_nonfrozen;

        int sb_y = g->y + g->header_h + m.rows_area_h;
        dobui_FillRect(g->win_id, g->x, sb_y,
                       m.content_w, DOBGRID_SCROLLBAR_W, g->col_bg);

        dob_scroll1d_t sg = dob_scroll1d(g->x, m.content_w,
                                         total_nf, vis_nf, g->hscroll);
        dobui_FillRect(g->win_id, sg.thumb_off, sb_y + 1,
                       sg.thumb_len, DOBGRID_SCROLLBAR_W - 2, g->col_scrollbar);
    }

    /* Riga separatrice sotto i titoli di colonna (utile con le righe alterne). */
    if (g->header_h > 0)
        dobui_FillRect(g->win_id, g->x, g->y + g->header_h - 2,
                       m.content_w, 2, g->col_header_rule);

    /* Border — focus color per the style protocol. */
    uint32_t bc = g->focused ? g->col_border_focus : g->col_border;
    dobui_DrawRect(g->win_id, g->x, g->y, g->w, g->h, bc);
}
