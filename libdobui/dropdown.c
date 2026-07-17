/* DobUITools — Dropdown Implementation */

#include "dropdown.h"
#include "scrollbar.h"
#include <DobInterface.h>
#include <string.h>

/* Helpers */

static int dd_max_visible(dob_dropdown_t *dd)
{
    int mv = (dd->count < DOBDD_MAX_VISIBLE) ? dd->count : DOBDD_MAX_VISIBLE;
    return (mv < 1) ? 1 : mv;
}

/* Popup scrollbar geometry. Returns false when the list fits (no bar).
 * On success fills *g (vertical thumb along Y, matching FlushPopup's
 * rendering) and the bar's X span [*sb_x, *sb_x + *sb_w). */
static bool dd_sb_geom(dob_dropdown_t *dd, dob_scroll1d_t *g,
                       int *sb_x, int *sb_w)
{
    int mv = dd_max_visible(dd);
    if (dd->count <= mv) return false;

    int w      = 10;                       /* sb_w in FlushPopup */
    int sy     = dd->y + dd->h + 1;        /* py + 1 */
    int list_h = mv * DOBDD_ITEM_H;

    *sb_x = dd->x + dd->w - w;
    *sb_w = w;
    *g = dob_scroll1d(sy, list_h, dd->count, mv, dd->scroll);
    return g->max_scroll > 0;
}

static int dd_popup_height(dob_dropdown_t *dd)
{
    return dd_max_visible(dd) * DOBDD_ITEM_H + 2;
}

/* Init */

void dobdd_Init(dob_dropdown_t *dd, uint32_t win_id,
                int x, int y, int w, int h,
                const char **items, int count)
{
    memset(dd, 0, sizeof(*dd));
    dd->win_id      = win_id;
    dd->x           = x;
    dd->y           = y;
    dd->w           = w;
    dd->h           = (h > 0) ? h : DOBDD_DEFAULT_H;
    dd->items       = items;
    dd->count       = count;
    dd->selected    = 0;
    dd->visible     = true;
    dd->enabled     = true;
    dd->anchor      = DOB_ANCHOR_CENTER_LEFT;
    dd->col_clear   = DOBUI_TEXT_ALT;

    dd->col_btn_bg   = DOBDD_COL_BTN_BG;
    dd->col_btn_open = DOBDD_COL_BTN_OPEN;
    dd->col_popup_bg = DOBDD_COL_POPUP_BG;
    dd->col_hover_bg = DOBDD_COL_HOVER_BG;
    dd->col_text     = DOBDD_COL_TEXT;
    dd->col_dim      = DOBDD_COL_DIM;
    dd->col_accent   = DOBDD_COL_ACCENT;
    dd->col_border   = DOBDD_COL_BORDER;
    if (dobfocus_auto_register) dobfocus_auto_register(dd, DOB_CTRL_DROPDOWN);
}

/* State */

void dobdd_Open(dob_dropdown_t *dd)
{
    dd->open = true;
    dd->_was_open = true;
    dd->cursor = (dd->selected >= 0) ? dd->selected : 0;

    int mv = dd_max_visible(dd);
    if (dd->cursor >= dd->scroll + mv)
        dd->scroll = dd->cursor - mv + 1;
    if (dd->cursor < dd->scroll)
        dd->scroll = dd->cursor;
}

void dobdd_Close(dob_dropdown_t *dd)
{
    dd->open = false;
}

void dobdd_SetFocus(dob_dropdown_t *dd, bool focused)
{
    dd->focused = focused;
    if (!focused && dd->open)
        dd->open = false;
}

void dobdd_SetSelected(dob_dropdown_t *dd, int index)
{
    if (index >= 0 && index < dd->count)
        dd->selected = index;
}

const char *dobdd_GetSelectedText(dob_dropdown_t *dd)
{
    if (dd->selected >= 0 && dd->selected < dd->count)
        return dd->items[dd->selected];
    return NULL;
}

bool dobdd_HitTest(dob_dropdown_t *dd, int px, int py)
{
    return px >= dd->x && px < dd->x + dd->w
        && py >= dd->y && py < dd->y + dd->h;
}

bool dobdd_PopupHitTest(dob_dropdown_t *dd, int px, int py)
{
    if (!dd->open) return false;
    int py0 = dd->y + dd->h;
    int ph  = dd_popup_height(dd);
    return px >= dd->x && px < dd->x + dd->w
        && py >= py0 && py < py0 + ph;
}

/* Event handling */

bool dobdd_OnClick(dob_dropdown_t *dd, int x, int y)
{
    if (!dd->visible || !dd->enabled) return false;

    if (dobdd_HitTest(dd, x, y))
    {
        if (dd->open)
            dd->open = false;
        else
            dobdd_Open(dd);
        return true;
    }

    /* Scrollbar column: a press here drives the bar, never the list.
     * Must run before the item hit-test below, which is width-agnostic
     * and would otherwise select the row behind the thumb. */
    if (dd->open)
    {
        dob_scroll1d_t sg;
        int sb_x, sb_w;
        if (dd_sb_geom(dd, &sg, &sb_x, &sb_w)
            && x >= sb_x && x < sb_x + sb_w
            && dob_scroll1d_hit_track(&sg, y))
        {
            if (dob_scroll1d_hit_thumb(&sg, y))
            {
                dd->sb_drag = true;
                dd->sb_grab = y - sg.thumb_off;
            }
            else
            {
                /* Track click above/below the thumb: page by a screenful
                 * toward the click. */
                int mv = dd_max_visible(dd);
                dd->scroll += (y < sg.thumb_off) ? -mv : mv;
                if (dd->scroll < 0)              dd->scroll = 0;
                if (dd->scroll > sg.max_scroll)  dd->scroll = sg.max_scroll;
            }
            return true;   /* keep the popup open */
        }
    }

    if (dd->open && dobdd_PopupHitTest(dd, x, y))
    {
        int popup_y = dd->y + dd->h + 1;
        int item_idx = (y - popup_y) / DOBDD_ITEM_H + dd->scroll;
        if (item_idx >= 0 && item_idx < dd->count)
        {
            dd->selected = item_idx;
            dd->open = false;
            return true;
        }
    }

    if (dd->open)
    {
        dd->open = false;
        return true;
    }
    return false;
}

bool dobdd_OnKey(dob_dropdown_t *dd, uint8_t key)
{
    if (!dd->visible || !dd->enabled || !dd->focused) return false;

    if (!dd->open)
    {
        if (key == '\n' || key == DOBDD_KEY_DOWN)
        {
            dobdd_Open(dd);
            return true;
        }
        return false;
    }

    if (key == DOBDD_KEY_DOWN)
    {
        if (dd->cursor < dd->count - 1)
        {
            dd->cursor++;
            int mv = dd_max_visible(dd);
            if (dd->cursor >= dd->scroll + mv)
                dd->scroll++;
        }
        return true;
    }
    if (key == DOBDD_KEY_UP)
    {
        if (dd->cursor > 0)
        {
            dd->cursor--;
            if (dd->cursor < dd->scroll)
                dd->scroll--;
        }
        return true;
    }
    if (key == '\n')
    {
        dd->selected = dd->cursor;
        dd->open = false;
        return true;
    }
    if (key == 27)
    {
        dd->open = false;
        return true;
    }
    return false;
}

bool dobdd_OnScroll(dob_dropdown_t *dd, int delta)
{
    if (!dd->visible || !dd->enabled) return false;

    if (dd->open)
    {
        /* Normalize to single steps for smooth scrolling */
        int step = (delta > 0) ? 1 : -1;
        dd->cursor += step;
        if (dd->cursor < 0) dd->cursor = 0;
        if (dd->cursor >= dd->count) dd->cursor = dd->count - 1;

        int mv = dd_max_visible(dd);
        if (dd->cursor >= dd->scroll + mv)
            dd->scroll = dd->cursor - mv + 1;
        if (dd->cursor < dd->scroll)
            dd->scroll = dd->cursor;
        return true;
    }

    if (dd->focused)
    {
        int sel = dd->selected + ((delta > 0) ? 1 : -1);
        if (sel < 0) sel = 0;
        if (sel >= dd->count) sel = dd->count - 1;
        dd->selected = sel;
        return true;
    }
    return false;
}

bool dobdd_OnDrag(dob_dropdown_t *dd, int x, int y)
{
    (void)x;
    if (!dd->sb_drag) return false;
    if (!dd->open) { dd->sb_drag = false; return false; }

    dob_scroll1d_t sg;
    int sb_x, sb_w;
    if (!dd_sb_geom(dd, &sg, &sb_x, &sb_w)) { dd->sb_drag = false; return false; }

    dd->scroll = dob_scroll1d_from_pos(&sg, y, dd->sb_grab);
    return true;
}

void dobdd_OnRelease(dob_dropdown_t *dd)
{
    dd->sb_drag = false;
}

/* Ghost cleanup — call at START of draw cycle */

void dobdd_ClearGhost(dob_dropdown_t *dd)
{
    if (dd->_was_open && !dd->open)
    {
        int py = dd->y + dd->h;
        int ph = dd_popup_height(dd);
        dobui_FillRect(dd->win_id, dd->x, py, dd->w, ph, dd->col_clear);
        dd->_was_open = false;
    }
}

/* Drawing — Button */

void dobdd_Draw(dob_dropdown_t *dd)
{
    if (!dd->visible) return;

    uint32_t bg = dd->open ? dd->col_btn_open : dd->col_btn_bg;

    dobui_FillRect(dd->win_id, dd->x, dd->y, dd->w, dd->h, bg);

    /* Selected text */
    const char *cur = "(nessuno)";
    if (dd->selected >= 0 && dd->selected < dd->count)
        cur = dd->items[dd->selected];

    int text_max_w = dd->w - DOBDD_ARROW_W - DOBDD_PAD;
    int max_chars  = text_max_w / DOBDD_FONT_W;
    if (max_chars > 63) max_chars = 63;

    char display[64];
    int clen = (int)strlen(cur);
    if (clen > max_chars) clen = max_chars;
    memcpy(display, cur, (uint32_t)clen);
    display[clen] = '\0';

    int tw = clen * DOBDD_FONT_W;
    int tx, ty;
    dob_anchor_pos(dd->anchor, dd->x, dd->y,
                   dd->w - DOBDD_ARROW_W, dd->h,
                   tw, DOBDD_FONT_H, DOBDD_PAD, &tx, &ty);
    dobui_DrawText(dd->win_id, tx, ty, display, dd->col_text, bg);

    /* Arrow */
    const char *arrow = dd->open ? "^" : "v";
    int ay = dd->y + (dd->h - DOBDD_FONT_H) / 2;
    dobui_DrawText(dd->win_id, dd->x + dd->w - DOBDD_ARROW_W + 2, ay,
                   arrow, dd->col_accent, bg);

    if (dd->focused)
        dobui_DrawRect(dd->win_id, dd->x, dd->y, dd->w, dd->h, dd->col_accent);
}

/* Drawing — Popup (deferred, draw LAST) */

void dobdd_FlushPopup(dob_dropdown_t *dd)
{
    if (!dd->open) return;

    int px = dd->x;
    int py = dd->y + dd->h;
    int pw = dd->w;
    int mv = dd_max_visible(dd);
    bool need_scroll = (dd->count > mv);

    int sb_w      = need_scroll ? 10 : 0;
    int content_w = pw - 2 - sb_w;
    int list_h    = mv * DOBDD_ITEM_H;
    int ph        = list_h + 2;

    int max_scroll = dd->count - mv;
    if (max_scroll < 0) max_scroll = 0;
    if (dd->scroll > max_scroll) dd->scroll = max_scroll;
    if (dd->scroll < 0) dd->scroll = 0;

    dobui_FillRect(dd->win_id, px, py, pw, ph, dd->col_popup_bg);
    dobui_DrawRect(dd->win_id, px, py, pw, ph, dd->col_border);

    for (int vi = 0; vi < mv; vi++)
    {
        int idx = vi + dd->scroll;
        if (idx >= dd->count) break;

        bool is_cursor = (idx == dd->cursor);
        uint32_t item_bg = is_cursor ? dd->col_hover_bg : dd->col_popup_bg;
        uint32_t item_fg = is_cursor ? DOBUI_INPUT : dd->col_text;
        int iy = py + 1 + vi * DOBDD_ITEM_H;

        dobui_FillRect(dd->win_id, px + 1, iy, content_w, DOBDD_ITEM_H, item_bg);

        const char *label = dd->items[idx];
        int mc = (content_w - DOBDD_PAD * 2) / DOBDD_FONT_W;
        if (mc > 63) mc = 63;
        char buf[64];
        int ll = (int)strlen(label);
        if (ll > mc) ll = mc;
        memcpy(buf, label, (uint32_t)ll);
        buf[ll] = '\0';

        int ty = iy + (DOBDD_ITEM_H - DOBDD_FONT_H) / 2;
        dobui_DrawText(dd->win_id, px + 1 + DOBDD_PAD, ty, buf,
                       item_fg, item_bg);
    }

    if (need_scroll && max_scroll > 0)
    {
        int sb_x = px + pw - sb_w;
        int sb_y = py + 1;

        dobui_FillRect(dd->win_id, sb_x, sb_y, sb_w - 1, list_h,
                       dd->col_popup_bg);

        /* Same geometry the drag hit-test uses (dd_sb_geom), so the
         * grabbable thumb and the painted thumb are pixel-identical. */
        dob_scroll1d_t sg = dob_scroll1d(sb_y, list_h,
                                         dd->count, mv, dd->scroll);
        dobui_FillRect(dd->win_id, sb_x + 1, sg.thumb_off,
                       sb_w - 3, sg.thumb_len, dd->col_accent);
    }
}
