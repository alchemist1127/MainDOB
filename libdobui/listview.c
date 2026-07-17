/* DobUITools — ListView Implementation */

#include "listview.h"
#include "scrollbar.h"
#include <DobInterface.h>
#include <string.h>

/* Scrollbar geometry — returns false when the list fits (no bar). On
 * success fills *g (vertical thumb) and the bar's X span. Mirrors the
 * arithmetic in doblv_Draw so the grabbable and painted thumbs match. */
static bool lv_sb_geom(dob_listview_t *lv, dob_scroll1d_t *g,
                       int *sb_x, int *sb_w)
{
    int vis = (lv->h / lv->item_h);
    if (vis < 1) vis = 1;
    if (lv->count <= vis) return false;

    *sb_x = lv->x + lv->w - DOBLV_SCROLLBAR_W;
    *sb_w = DOBLV_SCROLLBAR_W;
    *g = dob_scroll1d(lv->y, lv->h, lv->count, vis, lv->scroll);
    return g->max_scroll > 0;
}

void doblv_Init(dob_listview_t *lv, uint32_t win_id,
                int x, int y, int w, int h)
{
    memset(lv, 0, sizeof(*lv));
    lv->win_id          = win_id;
    lv->x               = x;
    lv->y               = y;
    lv->w               = w;
    lv->h               = h;
    lv->item_h          = DOBLV_ITEM_H;
    lv->selected        = -1;
    lv->visible         = true;
    lv->enabled         = true;
    lv->col_bg          = DOBLV_COL_BG;
    lv->col_item_bg     = DOBLV_COL_ITEM_BG;
    lv->col_sel_bg      = DOBLV_COL_SEL_BG;
    lv->col_text        = DOBLV_COL_TEXT;
    lv->col_sel_text    = DOBLV_COL_SEL_TEXT;
    lv->col_border      = DOBLV_COL_BORDER;
    lv->col_border_focus = DOBLV_COL_BORDER_F;
    lv->col_scrollbar   = DOBLV_COL_SCROLL;
    if (dobfocus_auto_register) dobfocus_auto_register(lv, DOB_CTRL_LISTVIEW);
}

void doblv_SetItems(dob_listview_t *lv, const char **items, int count)
{
    lv->items    = items;
    lv->count    = count;
    lv->selected = -1;
    lv->scroll   = 0;
}

void doblv_SetSelected(dob_listview_t *lv, int index)
{
    if (index >= -1 && index < lv->count)
        lv->selected = index;
}

void doblv_SetFocus(dob_listview_t *lv, bool focused) { lv->focused = focused; }
void doblv_SetEnabled(dob_listview_t *lv, bool enabled) { lv->enabled = enabled; }

int doblv_GetSelectedIndex(dob_listview_t *lv)
{
    return lv->selected;
}

const char *doblv_GetSelectedText(dob_listview_t *lv)
{
    if (lv->selected >= 0 && lv->selected < lv->count && lv->items)
        return lv->items[lv->selected];
    return NULL;
}

int doblv_VisibleCount(dob_listview_t *lv)
{
    int n = lv->h / lv->item_h;
    return (n < 1) ? 1 : n;
}

void doblv_EnsureVisible(dob_listview_t *lv)
{
    if (lv->selected < 0) return;
    int vis = doblv_VisibleCount(lv);
    if (lv->selected < lv->scroll)
        lv->scroll = lv->selected;
    else if (lv->selected >= lv->scroll + vis)
        lv->scroll = lv->selected - vis + 1;
    if (lv->scroll < 0) lv->scroll = 0;
}

static void lv_clamp_scroll(dob_listview_t *lv)
{
    int vis = doblv_VisibleCount(lv);
    int max_s = lv->count - vis;
    if (max_s < 0) max_s = 0;
    if (lv->scroll > max_s) lv->scroll = max_s;
    if (lv->scroll < 0) lv->scroll = 0;
}

bool doblv_OnClick(dob_listview_t *lv, int x, int y)
{
    if (!lv->visible || !lv->enabled) return false;
    if (x < lv->x || x >= lv->x + lv->w
        || y < lv->y || y >= lv->y + lv->h)
        return false;

    /* Scrollbar column: drive the bar, not the selection. */
    {
        dob_scroll1d_t sg;
        int sb_x, sb_w;
        if (lv_sb_geom(lv, &sg, &sb_x, &sb_w)
            && x >= sb_x && x < sb_x + sb_w)
        {
            if (dob_scroll1d_hit_thumb(&sg, y))
            {
                lv->sb_drag = true;
                lv->sb_grab = y - sg.thumb_off;
            }
            else
            {
                int vis = doblv_VisibleCount(lv);
                lv->scroll += (y < sg.thumb_off) ? -vis : vis;
                lv_clamp_scroll(lv);
            }
            return true;
        }
    }

    int rel_y = y - lv->y;
    int idx = rel_y / lv->item_h + lv->scroll;
    if (idx >= 0 && idx < lv->count)
        lv->selected = idx;
    return true;
}

bool doblv_OnKey(dob_listview_t *lv, uint8_t key)
{
    if (!lv->visible || !lv->enabled || !lv->focused || lv->count == 0)
        return false;

    if (key == 129) /* DOWN */
    {
        if (lv->selected < lv->count - 1)
            lv->selected++;
        else if (lv->selected < 0)
            lv->selected = 0;
        doblv_EnsureVisible(lv);
        return true;
    }
    if (key == 128) /* UP */
    {
        if (lv->selected > 0)
            lv->selected--;
        else if (lv->selected < 0)
            lv->selected = 0;
        doblv_EnsureVisible(lv);
        return true;
    }
    if (key == 135) /* PGDN */
    {
        int vis = doblv_VisibleCount(lv);
        lv->selected += vis;
        if (lv->selected >= lv->count) lv->selected = lv->count - 1;
        doblv_EnsureVisible(lv);
        return true;
    }
    if (key == 136) /* PGUP */
    {
        int vis = doblv_VisibleCount(lv);
        lv->selected -= vis;
        if (lv->selected < 0) lv->selected = 0;
        doblv_EnsureVisible(lv);
        return true;
    }
    if (key == 132) /* HOME */
    {
        lv->selected = 0;
        doblv_EnsureVisible(lv);
        return true;
    }
    if (key == 133) /* END */
    {
        lv->selected = lv->count - 1;
        doblv_EnsureVisible(lv);
        return true;
    }
    return false;
}

bool doblv_OnScroll(dob_listview_t *lv, int delta)
{
    if (!lv->visible || !lv->enabled) return false;
    lv->scroll += delta;
    lv_clamp_scroll(lv);
    return true;
}

bool doblv_OnDrag(dob_listview_t *lv, int x, int y)
{
    (void)x;
    if (!lv->sb_drag) return false;

    dob_scroll1d_t sg;
    int sb_x, sb_w;
    if (!lv_sb_geom(lv, &sg, &sb_x, &sb_w)) { lv->sb_drag = false; return false; }

    lv->scroll = dob_scroll1d_from_pos(&sg, y, lv->sb_grab);
    return true;
}

void doblv_OnRelease(dob_listview_t *lv)
{
    lv->sb_drag = false;
}

void doblv_Draw(dob_listview_t *lv)
{
    if (!lv->visible) return;

    int vis = doblv_VisibleCount(lv);
    bool need_scroll = (lv->count > vis);
    int content_w = need_scroll ? lv->w - DOBLV_SCROLLBAR_W : lv->w;

    lv_clamp_scroll(lv);

    /* Background */
    dobui_FillRect(lv->win_id, lv->x, lv->y, lv->w, lv->h, lv->col_bg);

    /* Items */
    int max_chars = (content_w - DOBLV_PAD * 2) / DOBLV_FONT_W;
    if (max_chars > 127) max_chars = 127;

    for (int vi = 0; vi < vis; vi++)
    {
        int idx = vi + lv->scroll;
        if (idx >= lv->count) break;

        bool is_sel = (idx == lv->selected);
        uint32_t ibg = is_sel ? lv->col_sel_bg : lv->col_item_bg;
        uint32_t ifg = is_sel ? lv->col_sel_text : lv->col_text;

        int iy = lv->y + vi * lv->item_h;
        dobui_FillRect(lv->win_id, lv->x, iy, content_w, lv->item_h, ibg);

        const char *label = lv->items[idx];
        char buf[128];
        int ll = (int)strlen(label);
        if (ll > max_chars) ll = max_chars;
        memcpy(buf, label, (uint32_t)ll);
        buf[ll] = '\0';

        int ty = iy + (lv->item_h - DOBLV_FONT_H) / 2;
        dobui_DrawText(lv->win_id, lv->x + DOBLV_PAD, ty, buf, ifg, ibg);
    }

    /* Scrollbar */
    if (need_scroll)
    {
        int sb_x = lv->x + lv->w - DOBLV_SCROLLBAR_W;
        dobui_FillRect(lv->win_id, sb_x, lv->y,
                       DOBLV_SCROLLBAR_W, lv->h, lv->col_bg);

        dob_scroll1d_t sg = dob_scroll1d(lv->y, lv->h,
                                         lv->count, vis, lv->scroll);
        dobui_FillRect(lv->win_id, sb_x + 1, sg.thumb_off,
                       DOBLV_SCROLLBAR_W - 2, sg.thumb_len, lv->col_scrollbar);
    }

    /* Border */
    uint32_t bc = lv->focused ? lv->col_border_focus : lv->col_border;
    dobui_DrawRect(lv->win_id, lv->x, lv->y, lv->w, lv->h, bc);
}
