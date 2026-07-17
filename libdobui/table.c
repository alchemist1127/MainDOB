/* DobUITools — Table Implementation
 *
 * The widget is geometry + state; rendering is done with the standard
 * dobui drawing primitives that write directly into the window's SHM
 * framebuffer (zero IPC per pixel).  All event handlers are pure
 * computation on the dob_table_t struct.
 *
 * The divider drag follows the same Click → Drag → Release pattern as
 * dob_slider_t, so the focus manager can route mouse events to it
 * uniformly with the other interactive controls. */

#include "table.h"
#include "scrollbar.h"
#include <DobInterface.h>
#include <string.h>

/* Layout helpers — derived from the struct so they always stay in
 * sync with whatever the user has reconfigured. */

static inline int tbl_content_w(const dob_table_t *t)
{
    /* Reserve room for the scrollbar only when scrolling is required.
     * VisibleCount needs to know content_w to decide whether to scroll,
     * but here we always reserve when count exceeds capacity assuming
     * NO scrollbar — small flicker of layout is avoided by computing
     * scroll-needed independently. The visible API just exposes the
     * stable post-decision width. */
    int rows_capacity_no_sb = (t->h - t->header_h) / t->row_h;
    bool need_sb = (t->count > rows_capacity_no_sb);
    return need_sb ? t->w - DOBTBL_SCROLLBAR_W : t->w;
}

static inline int tbl_value_col_w(const dob_table_t *t)
{
    int cw = tbl_content_w(t) - t->key_col_w;
    return (cw > 0) ? cw : 0;
}

/* Init / setters */

void dobtbl_Init(dob_table_t *t, uint32_t win_id,
                 int x, int y, int w, int h)
{
    memset(t, 0, sizeof(*t));
    t->win_id            = win_id;
    t->x                 = x;
    t->y                 = y;
    t->w                 = w;
    t->h                 = h;
    t->row_h             = DOBTBL_ROW_H;
    t->header_h          = 0;            /* No headers by default */
    t->key_col_w         = (w * DOBTBL_DEFAULT_KEY_RATIO) / 10;
    t->selected          = -1;
    t->visible           = true;
    t->enabled           = true;
    t->selectable        = false;        /* Pure read-only by default */
    t->col_bg            = DOBTBL_COL_BG;
    t->col_header_bg     = DOBTBL_COL_HEADER_BG;
    t->col_header_text   = DOBTBL_COL_HEADER_TEXT;
    t->col_text          = DOBTBL_COL_TEXT;
    t->col_sel_bg        = DOBTBL_COL_SEL_BG;
    t->col_sel_text      = DOBTBL_COL_SEL_TEXT;
    t->col_border        = DOBTBL_COL_BORDER;
    t->col_border_focus  = DOBTBL_COL_BORDER_FOCUS;
    t->col_divider       = DOBTBL_COL_DIVIDER;
    t->col_divider_grab  = DOBTBL_COL_DIVIDER_GRAB;
    t->col_scrollbar     = DOBTBL_COL_SCROLLBAR;

    if (dobfocus_auto_register)
        dobfocus_auto_register(t, DOB_CTRL_TABLE);
}

void dobtbl_SetHeaders(dob_table_t *t, const char *key, const char *value)
{
    t->header_key   = key;
    t->header_value = value;
    t->header_h     = (key || value) ? DOBTBL_HEADER_H : 0;
}

void dobtbl_SetRows(dob_table_t *t, const char **keys,
                    const char **values, int count)
{
    t->keys     = keys;
    t->values   = values;
    t->count    = (count < 0) ? 0 : count;
    t->selected = -1;
    t->scroll   = 0;
}

void dobtbl_SetKeyColumnWidth(dob_table_t *t, int px)
{
    int max = t->w - DOBTBL_MIN_COL_W - DOBTBL_SCROLLBAR_W;
    if (px < DOBTBL_MIN_COL_W) px = DOBTBL_MIN_COL_W;
    if (px > max)              px = max;
    t->key_col_w = px;
}

void dobtbl_SetSelectable(dob_table_t *t, bool selectable)
{
    t->selectable = selectable;
    if (!selectable) t->selected = -1;
}

void dobtbl_SetSelected(dob_table_t *t, int index)
{
    if (!t->selectable) return;
    if (index >= -1 && index < t->count) t->selected = index;
}

void dobtbl_SetEnabled(dob_table_t *t, bool enabled) { t->enabled = enabled; }
void dobtbl_SetFocus(dob_table_t *t, bool focused)   { t->focused = focused; }

int dobtbl_GetSelectedIndex(const dob_table_t *t) { return t->selected; }

const char *dobtbl_GetSelectedKey(const dob_table_t *t)
{
    if (t->selected >= 0 && t->selected < t->count && t->keys)
        return t->keys[t->selected];
    return NULL;
}

const char *dobtbl_GetSelectedValue(const dob_table_t *t)
{
    if (t->selected >= 0 && t->selected < t->count && t->values)
        return t->values[t->selected];
    return NULL;
}

int dobtbl_VisibleCount(const dob_table_t *t)
{
    int n = (t->h - t->header_h) / t->row_h;
    return (n < 1) ? 1 : n;
}

/* Hit tests */

bool dobtbl_HitTest(const dob_table_t *t, int x, int y)
{
    return x >= t->x && x < t->x + t->w
        && y >= t->y && y < t->y + t->h;
}

bool dobtbl_HitTestDivider(const dob_table_t *t, int x, int y)
{
    if (!t->visible || !t->enabled) return false;
    if (!dobtbl_HitTest(t, x, y))   return false;

    /* Divider screen-x = t->x + key_col_w. Hit zone is centered on it
     * and DOBTBL_DIVIDER_HIT pixels wide so the user can grab without
     * pixel-perfect aim. */
    int dx = t->x + t->key_col_w;
    int half = DOBTBL_DIVIDER_HIT / 2;
    return x >= dx - half && x < dx + half;
}

/* Vertical scrollbar geometry — false when all rows fit. Mirrors the
 * arithmetic in dobtbl_Draw so the grabbable and painted thumbs match. */
static bool tbl_sb_geom(const dob_table_t *t, dob_scroll1d_t *g,
                        int *sb_x, int *sb_w)
{
    int vis = dobtbl_VisibleCount(t);
    if (t->count <= vis) return false;

    int sb_y = t->y + t->header_h;
    int sb_h = t->h - t->header_h;

    *sb_x = t->x + t->w - DOBTBL_SCROLLBAR_W;
    *sb_w = DOBTBL_SCROLLBAR_W;
    *g = dob_scroll1d(sb_y, sb_h, t->count, vis, t->scroll);
    return g->max_scroll > 0;
}

/* Scroll clamp — recomputed on demand because count, scroll, and h
 * can change independently between calls. */

static void tbl_clamp_scroll(dob_table_t *t)
{
    int vis = dobtbl_VisibleCount(t);
    int max_s = t->count - vis;
    if (max_s < 0) max_s = 0;
    if (t->scroll > max_s) t->scroll = max_s;
    if (t->scroll < 0)     t->scroll = 0;
}

static void tbl_ensure_visible(dob_table_t *t)
{
    if (t->selected < 0) return;
    int vis = dobtbl_VisibleCount(t);
    if (t->selected < t->scroll)
        t->scroll = t->selected;
    else if (t->selected >= t->scroll + vis)
        t->scroll = t->selected - vis + 1;
    if (t->scroll < 0) t->scroll = 0;
}

/* Event handlers */

bool dobtbl_OnClick(dob_table_t *t, int x, int y)
{
    if (!t->visible || !t->enabled)        return false;
    if (!dobtbl_HitTest(t, x, y))          return false;

    /* Divider takes priority — it overlaps the row hit areas. */
    if (dobtbl_HitTestDivider(t, x, y))
    {
        t->divider_grabbed = true;
        /* Switch to the horizontal split cursor for the duration of
         * the drag.  Per-window override: only shows while inside
         * this window's body.  Cleared in dobtbl_OnRelease. */
        dobui_SetCursor(t->win_id, CURSOR_HSPLIT);
        return true;
    }

    /* Vertical scrollbar column: drive the bar, not a row. */
    {
        dob_scroll1d_t sg;
        int sb_x, sb_w;
        if (tbl_sb_geom(t, &sg, &sb_x, &sb_w)
            && x >= sb_x && x < sb_x + sb_w
            && dob_scroll1d_hit_track(&sg, y))
        {
            if (dob_scroll1d_hit_thumb(&sg, y))
            {
                t->sb_drag = true;
                t->sb_grab = y - sg.thumb_off;
            }
            else
            {
                int vis = dobtbl_VisibleCount(t);
                t->scroll += (y < sg.thumb_off) ? -vis : vis;
                tbl_clamp_scroll(t);
            }
            return true;
        }
    }

    /* Header row: never selectable, but consume the click so it
     * doesn't fall through to whatever is behind. */
    if (t->header_h > 0 && y < t->y + t->header_h)
        return true;

    if (!t->selectable) return true;       /* Consume but don't act */

    int rel_y = y - t->y - t->header_h;
    int idx = rel_y / t->row_h + t->scroll;
    if (idx >= 0 && idx < t->count)
        t->selected = idx;
    return true;
}

bool dobtbl_OnDrag(dob_table_t *t, int x, int y)
{
    if (t->sb_drag)
    {
        dob_scroll1d_t sg;
        int sb_x, sb_w;
        if (!tbl_sb_geom(t, &sg, &sb_x, &sb_w)) { t->sb_drag = false; return false; }
        t->scroll = dob_scroll1d_from_pos(&sg, y, t->sb_grab);
        return true;
    }

    if (!t->divider_grabbed) return false;

    int new_w = x - t->x;
    /* Reuse the public setter so the same min/max clamps apply. */
    dobtbl_SetKeyColumnWidth(t, new_w);
    return true;
}

void dobtbl_OnRelease(dob_table_t *t)
{
    t->sb_drag = false;
    if (t->divider_grabbed)
    {
        t->divider_grabbed = false;
        /* Drop the HSPLIT cursor override; the WM's baseline takes
         * over again (CURSOR_ARROW elsewhere, CURSOR_RESIZE on
         * window edges). */
        dobui_SetCursor(t->win_id, CURSOR_DEFAULT);
    }
}

bool dobtbl_OnKey(dob_table_t *t, uint8_t key)
{
    if (!t->visible || !t->enabled || !t->focused) return false;
    if (!t->selectable || t->count == 0)            return false;

    switch (key)
    {
        case 129:  /* DOWN */
            if (t->selected < t->count - 1) t->selected++;
            else if (t->selected < 0)       t->selected = 0;
            tbl_ensure_visible(t);
            return true;

        case 128:  /* UP */
            if (t->selected > 0)            t->selected--;
            else if (t->selected < 0)       t->selected = 0;
            tbl_ensure_visible(t);
            return true;

        case 135:  /* PGDN */
        {
            int vis = dobtbl_VisibleCount(t);
            t->selected += vis;
            if (t->selected >= t->count) t->selected = t->count - 1;
            tbl_ensure_visible(t);
            return true;
        }

        case 136:  /* PGUP */
        {
            int vis = dobtbl_VisibleCount(t);
            t->selected -= vis;
            if (t->selected < 0) t->selected = 0;
            tbl_ensure_visible(t);
            return true;
        }

        case 132:  /* HOME */
            t->selected = 0;
            tbl_ensure_visible(t);
            return true;

        case 133:  /* END */
            t->selected = t->count - 1;
            tbl_ensure_visible(t);
            return true;

        default:
            return false;
    }
}

bool dobtbl_OnScroll(dob_table_t *t, int delta)
{
    if (!t->visible || !t->enabled) return false;
    t->scroll += delta;
    tbl_clamp_scroll(t);
    return true;
}

/* Render */

/* Truncate `src` so it fits in `max_chars` and copy into `dst`
 * (capacity dst_cap, NUL-terminated). Adds an ASCII ellipsis ".."
 * when truncation actually happens. Cheap, no allocation. */
static int tbl_fit(char *dst, int dst_cap, const char *src, int max_chars)
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

    /* Need at least 1 char for content + 2 for "..". If max_chars is
     * tiny, drop the ellipsis. */
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

void dobtbl_Draw(dob_table_t *t)
{
    if (!t->visible) return;

    tbl_clamp_scroll(t);

    int vis = dobtbl_VisibleCount(t);
    int content_w = tbl_content_w(t);
    bool need_sb  = (t->count > vis);

    /* Background — fill the whole bounding box first; subsequent
     * passes overwrite the parts they own. Avoids striping when the
     * row count shrinks below what was on screen previously. */
    dobui_FillRect(t->win_id, t->x, t->y, t->w, t->h, t->col_bg);

    /* Header row */
    if (t->header_h > 0)
    {
        dobui_FillRect(t->win_id, t->x, t->y,
                       content_w, t->header_h, t->col_header_bg);

        int key_chars = (t->key_col_w - DOBTBL_PAD * 2) / DOBTBL_FONT_W;
        int val_chars = (tbl_value_col_w(t) - DOBTBL_PAD * 2) / DOBTBL_FONT_W;
        char buf[160];
        int hy = t->y + (t->header_h - DOBTBL_FONT_H) / 2;

        if (t->header_key && key_chars > 0)
        {
            tbl_fit(buf, sizeof(buf), t->header_key, key_chars);
            dobui_DrawText(t->win_id, t->x + DOBTBL_PAD, hy,
                           buf, t->col_header_text, t->col_header_bg);
        }
        if (t->header_value && val_chars > 0)
        {
            tbl_fit(buf, sizeof(buf), t->header_value, val_chars);
            dobui_DrawText(t->win_id,
                           t->x + t->key_col_w + DOBTBL_PAD, hy,
                           buf, t->col_header_text, t->col_header_bg);
        }
    }

    /* Data rows */
    int key_chars = (t->key_col_w - DOBTBL_PAD * 2) / DOBTBL_FONT_W;
    int val_chars = (tbl_value_col_w(t) - DOBTBL_PAD * 2) / DOBTBL_FONT_W;
    if (key_chars < 1) key_chars = 1;
    if (val_chars < 1) val_chars = 1;

    char buf[160];
    int rows_y0 = t->y + t->header_h;

    for (int vi = 0; vi < vis; vi++)
    {
        int idx = vi + t->scroll;
        if (idx >= t->count) break;

        bool is_sel = (t->selectable && idx == t->selected);
        uint32_t row_bg = is_sel ? t->col_sel_bg   : t->col_bg;
        uint32_t row_fg = is_sel ? t->col_sel_text : t->col_text;

        int ry = rows_y0 + vi * t->row_h;
        if (is_sel)
            dobui_FillRect(t->win_id, t->x, ry,
                           content_w, t->row_h, row_bg);

        int ty = ry + (t->row_h - DOBTBL_FONT_H) / 2;

        const char *k = t->keys   ? t->keys[idx]   : NULL;
        const char *v = t->values ? t->values[idx] : NULL;

        if (k)
        {
            tbl_fit(buf, sizeof(buf), k, key_chars);
            dobui_DrawText(t->win_id, t->x + DOBTBL_PAD, ty,
                           buf, row_fg, row_bg);
        }
        if (v)
        {
            tbl_fit(buf, sizeof(buf), v, val_chars);
            dobui_DrawText(t->win_id,
                           t->x + t->key_col_w + DOBTBL_PAD, ty,
                           buf, row_fg, row_bg);
        }
    }

    /* Column divider — drawn over the rows so it survives selection
     * highlights. Thicker / colored when grabbed for visual feedback. */
    int dx = t->x + t->key_col_w;
    uint32_t dcol = t->divider_grabbed ? t->col_divider_grab : t->col_divider;
    int dw = t->divider_grabbed ? 2 : DOBTBL_DIVIDER_W;
    dobui_FillRect(t->win_id, dx - dw / 2, t->y, dw, t->h, dcol);

    /* Scrollbar */
    if (need_sb)
    {
        int sb_x = t->x + t->w - DOBTBL_SCROLLBAR_W;
        int sb_y = rows_y0;
        int sb_h = t->h - t->header_h;
        dobui_FillRect(t->win_id, sb_x, sb_y,
                       DOBTBL_SCROLLBAR_W, sb_h, t->col_bg);

        dob_scroll1d_t sg = dob_scroll1d(sb_y, sb_h, t->count, vis, t->scroll);
        dobui_FillRect(t->win_id, sb_x + 1, sg.thumb_off,
                       DOBTBL_SCROLLBAR_W - 2, sg.thumb_len, t->col_scrollbar);
    }

    /* Border */
    uint32_t bc = t->focused ? t->col_border_focus : t->col_border;
    dobui_DrawRect(t->win_id, t->x, t->y, t->w, t->h, bc);
}
