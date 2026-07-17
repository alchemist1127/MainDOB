/* DobUITools — Checked ListView Implementation
 *
 * Pure visual + state widget. Each row carries a checkbox glyph on
 * the left followed by the label text. The checked state lives in
 * a caller-supplied bool[] array — the widget never allocates and
 * never owns the array.
 *
 * Click on a row both selects it (cursor moves) and toggles the
 * check. Space / Enter on the focused widget toggles the cursor
 * row without moving it. Arrows / Home / End / PgUp / PgDn just
 * move the cursor.
 *
 * Closely mirrors listview.c so the two stay visually consistent
 * and a future refactor (e.g., parameterising a "checkable" flag
 * on the base listview) can collapse them safely. */

#include "checked_listview.h"
#include "scrollbar.h"
#include <DobInterface.h>
#include <string.h>

/* Scrollbar geometry — false when the list fits. Mirrors dobclv_Draw. */
static bool clv_sb_geom(dob_checked_listview_t *lv, dob_scroll1d_t *g,
                        int *sb_x, int *sb_w)
{
    int vis = (lv->h / lv->item_h);
    if (vis < 1) vis = 1;
    if (lv->count <= vis) return false;

    *sb_x = lv->x + lv->w - DOBCLV_SCROLLBAR_W;
    *sb_w = DOBCLV_SCROLLBAR_W;
    *g = dob_scroll1d(lv->y, lv->h, lv->count, vis, lv->scroll);
    return g->max_scroll > 0;
}

void dobclv_Init(dob_checked_listview_t *lv, uint32_t win_id,
                 int x, int y, int w, int h)
{
    memset(lv, 0, sizeof(*lv));
    lv->win_id           = win_id;
    lv->x                = x;
    lv->y                = y;
    lv->w                = w;
    lv->h                = h;
    lv->item_h           = DOBCLV_ITEM_H;
    lv->selected         = -1;
    lv->visible          = true;
    lv->enabled          = true;
    lv->col_bg           = DOBCLV_COL_BG;
    lv->col_item_bg      = DOBCLV_COL_ITEM_BG;
    lv->col_sel_bg       = DOBCLV_COL_SEL_BG;
    lv->col_text         = DOBCLV_COL_TEXT;
    lv->col_sel_text     = DOBCLV_COL_SEL_TEXT;
    lv->col_border       = DOBCLV_COL_BORDER;
    lv->col_border_focus = DOBCLV_COL_BORDER_F;
    lv->col_scrollbar    = DOBCLV_COL_SCROLL;
    lv->col_check_box    = DOBCLV_COL_CHECK_BOX;
    lv->col_check_mark   = DOBCLV_COL_CHECK_MARK;
    lv->col_check_border = DOBCLV_COL_CHECK_BORDER;
    if (dobfocus_auto_register)
        dobfocus_auto_register(lv, DOB_CTRL_CHECKED_LISTVIEW);
}

void dobclv_SetItems(dob_checked_listview_t *lv,
                     const char **items, bool *checked, int count)
{
    lv->items   = items;
    lv->checked = checked;
    lv->count   = count;
    /* Unlike doblv_SetItems we keep selected and scroll across
     * calls — callers re-set every render with the same logical
     * content, and wiping the cursor every frame is exactly the
     * bug doblv_SetItems forced clients to work around. */
    if (lv->selected >= count) lv->selected = (count > 0) ? count - 1 : -1;
    if (count == 0)            lv->scroll   = 0;
}

void dobclv_SetFocus  (dob_checked_listview_t *lv, bool focused) { lv->focused = focused; }
void dobclv_SetEnabled(dob_checked_listview_t *lv, bool enabled) { lv->enabled = enabled; }

int dobclv_GetSelectedIndex(const dob_checked_listview_t *lv) { return lv->selected; }

int dobclv_VisibleCount(const dob_checked_listview_t *lv)
{
    int n = lv->h / lv->item_h;
    return (n < 1) ? 1 : n;
}

static void clv_ensure_visible(dob_checked_listview_t *lv)
{
    if (lv->selected < 0) return;
    int vis = dobclv_VisibleCount(lv);
    if (lv->selected < lv->scroll)
        lv->scroll = lv->selected;
    else if (lv->selected >= lv->scroll + vis)
        lv->scroll = lv->selected - vis + 1;
    if (lv->scroll < 0) lv->scroll = 0;
}

static void clv_clamp_scroll(dob_checked_listview_t *lv)
{
    int vis = dobclv_VisibleCount(lv);
    int max_s = lv->count - vis;
    if (max_s < 0) max_s = 0;
    if (lv->scroll > max_s) lv->scroll = max_s;
    if (lv->scroll < 0) lv->scroll = 0;
}

bool dobclv_OnClick(dob_checked_listview_t *lv, int x, int y)
{
    if (!lv->visible || !lv->enabled) return false;
    if (x < lv->x || x >= lv->x + lv->w ||
        y < lv->y || y >= lv->y + lv->h)
        return false;

    /* Scrollbar column: drive the bar, not the row toggle. */
    {
        dob_scroll1d_t sg;
        int sb_x, sb_w;
        if (clv_sb_geom(lv, &sg, &sb_x, &sb_w)
            && x >= sb_x && x < sb_x + sb_w)
        {
            if (dob_scroll1d_hit_thumb(&sg, y))
            {
                lv->sb_drag = true;
                lv->sb_grab = y - sg.thumb_off;
            }
            else
            {
                int vis = dobclv_VisibleCount(lv);
                lv->scroll += (y < sg.thumb_off) ? -vis : vis;
                clv_clamp_scroll(lv);
            }
            return true;
        }
    }

    int rel_y = y - lv->y;
    int idx = rel_y / lv->item_h + lv->scroll;
    if (idx >= 0 && idx < lv->count)
    {
        lv->selected = idx;
        if (lv->checked)
            lv->checked[idx] = !lv->checked[idx];
    }
    return true;
}

bool dobclv_OnKey(dob_checked_listview_t *lv, uint8_t key)
{
    if (!lv->visible || !lv->enabled || !lv->focused || lv->count == 0)
        return false;

    if (key == 129) /* DOWN */
    {
        if (lv->selected < lv->count - 1) lv->selected++;
        else if (lv->selected < 0)        lv->selected = 0;
        clv_ensure_visible(lv);
        return true;
    }
    if (key == 128) /* UP */
    {
        if (lv->selected > 0)             lv->selected--;
        else if (lv->selected < 0)        lv->selected = 0;
        clv_ensure_visible(lv);
        return true;
    }
    if (key == 135) /* PGDN */
    {
        int vis = dobclv_VisibleCount(lv);
        lv->selected += vis;
        if (lv->selected >= lv->count) lv->selected = lv->count - 1;
        clv_ensure_visible(lv);
        return true;
    }
    if (key == 136) /* PGUP */
    {
        int vis = dobclv_VisibleCount(lv);
        lv->selected -= vis;
        if (lv->selected < 0) lv->selected = 0;
        clv_ensure_visible(lv);
        return true;
    }
    if (key == 132) /* HOME */
    {
        lv->selected = 0;
        clv_ensure_visible(lv);
        return true;
    }
    if (key == 133) /* END */
    {
        lv->selected = lv->count - 1;
        clv_ensure_visible(lv);
        return true;
    }
    if (key == ' ' || key == '\n' || key == '\r')
    {
        if (lv->selected >= 0 && lv->selected < lv->count && lv->checked)
            lv->checked[lv->selected] = !lv->checked[lv->selected];
        return true;
    }
    return false;
}

bool dobclv_OnScroll(dob_checked_listview_t *lv, int delta)
{
    if (!lv->visible || !lv->enabled) return false;
    lv->scroll += delta;
    clv_clamp_scroll(lv);
    return true;
}

bool dobclv_OnDrag(dob_checked_listview_t *lv, int x, int y)
{
    (void)x;
    if (!lv->sb_drag) return false;

    dob_scroll1d_t sg;
    int sb_x, sb_w;
    if (!clv_sb_geom(lv, &sg, &sb_x, &sb_w)) { lv->sb_drag = false; return false; }

    lv->scroll = dob_scroll1d_from_pos(&sg, y, lv->sb_grab);
    return true;
}

void dobclv_OnRelease(dob_checked_listview_t *lv)
{
    lv->sb_drag = false;
}

static void
clv_draw_check(dob_checked_listview_t *lv, int box_x, int box_y, bool on)
{
    int s = DOBCLV_CHECK_SIZE;
    dobui_FillRect(lv->win_id, box_x, box_y, s, s, lv->col_check_box);
    dobui_DrawRect(lv->win_id, box_x, box_y, s, s, lv->col_check_border);
    if (on)
    {
        /* Filled inner square as the check mark — simple and
         * font-free. Could be upgraded to a proper "V" later. */
        dobui_FillRect(lv->win_id,
                       box_x + 3, box_y + 3, s - 6, s - 6,
                       lv->col_check_mark);
    }
}

void dobclv_Draw(dob_checked_listview_t *lv)
{
    if (!lv->visible) return;

    int vis = dobclv_VisibleCount(lv);
    bool need_scroll = (lv->count > vis);
    int content_w = need_scroll ? lv->w - DOBCLV_SCROLLBAR_W : lv->w;

    clv_clamp_scroll(lv);

    /* Background */
    dobui_FillRect(lv->win_id, lv->x, lv->y, lv->w, lv->h, lv->col_bg);

    int text_x = lv->x + DOBCLV_PAD + DOBCLV_CHECK_SIZE + DOBCLV_CHECK_GAP;
    int text_w = content_w - (text_x - lv->x) - DOBCLV_PAD;
    int max_chars = text_w / DOBCLV_FONT_W;
    if (max_chars > 127) max_chars = 127;
    if (max_chars < 1)   max_chars = 1;

    for (int vi = 0; vi < vis; vi++)
    {
        int idx = vi + lv->scroll;
        if (idx >= lv->count) break;

        bool is_sel = (idx == lv->selected);
        uint32_t ibg = is_sel ? lv->col_sel_bg   : lv->col_item_bg;
        uint32_t ifg = is_sel ? lv->col_sel_text : lv->col_text;

        int iy = lv->y + vi * lv->item_h;
        dobui_FillRect(lv->win_id, lv->x, iy, content_w, lv->item_h, ibg);

        /* Checkbox glyph */
        int box_y = iy + (lv->item_h - DOBCLV_CHECK_SIZE) / 2;
        int box_x = lv->x + DOBCLV_PAD;
        bool on = lv->checked ? lv->checked[idx] : false;
        clv_draw_check(lv, box_x, box_y, on);

        /* Label */
        const char *label = lv->items[idx];
        char buf[128];
        int  ll = (int)strlen(label);
        if (ll > max_chars) ll = max_chars;
        memcpy(buf, label, (uint32_t)ll);
        buf[ll] = '\0';

        int ty = iy + (lv->item_h - DOBCLV_FONT_H) / 2;
        dobui_DrawText(lv->win_id, text_x, ty, buf, ifg, ibg);
    }

    /* Scrollbar */
    if (need_scroll)
    {
        int sb_x = lv->x + lv->w - DOBCLV_SCROLLBAR_W;
        dobui_FillRect(lv->win_id, sb_x, lv->y,
                       DOBCLV_SCROLLBAR_W, lv->h, lv->col_bg);

        dob_scroll1d_t sg = dob_scroll1d(lv->y, lv->h,
                                         lv->count, vis, lv->scroll);
        dobui_FillRect(lv->win_id, sb_x + 1, sg.thumb_off,
                       DOBCLV_SCROLLBAR_W - 2, sg.thumb_len, lv->col_scrollbar);
    }

    /* Border */
    uint32_t bc = lv->focused ? lv->col_border_focus : lv->col_border;
    dobui_DrawRect(lv->win_id, lv->x, lv->y, lv->w, lv->h, bc);
}
