/* DobUITools — Checkbox Implementation */

#include "checkbox.h"
#include <DobInterface.h>
#include <string.h>

void dobcb_Init(dob_checkbox_t *cb, uint32_t win_id,
                int x, int y, int box_size, const char *text)
{
    memset(cb, 0, sizeof(*cb));
    cb->win_id           = win_id;
    cb->x                = x;
    cb->y                = y;
    cb->box_size         = (box_size > 0) ? box_size : DOBCB_DEFAULT_SIZE;
    cb->text_gap         = DOBCB_TEXT_GAP;
    cb->visible          = true;
    cb->enabled          = true;
    cb->col_box          = DOBCB_COL_BOX;
    cb->col_box_bg       = DOBCB_COL_BOX_BG;
    cb->col_check        = DOBCB_COL_CHECK;
    cb->col_box_disabled = DOBCB_COL_BOX_DIS;
    cb->col_check_disabled = DOBCB_COL_CHECK_DIS;
    cb->col_text         = DOBCB_COL_TEXT;
    cb->col_bg           = DOBCB_COL_BG;
    cb->col_focus        = DOBCB_COL_FOCUS;

    if (text)
    {
        int n = (int)strlen(text);
        if (n > 127) n = 127;
        memcpy(cb->text, text, (uint32_t)n);
        cb->text[n] = '\0';
    }
    if (dobfocus_auto_register) dobfocus_auto_register(cb, DOB_CTRL_CHECKBOX);
}

void dobcb_SetText(dob_checkbox_t *cb, const char *text)
{
    if (!text) text = "";
    int n = (int)strlen(text);
    if (n > 127) n = 127;
    memcpy(cb->text, text, (uint32_t)n);
    cb->text[n] = '\0';
}

void dobcb_SetChecked(dob_checkbox_t *cb, bool checked)  { cb->checked = checked; }
void dobcb_SetEnabled(dob_checkbox_t *cb, bool enabled)   { cb->enabled = enabled; }
void dobcb_SetFocus(dob_checkbox_t *cb, bool focused)     { cb->focused = focused; }

int dobcb_GetWidth(dob_checkbox_t *cb)
{
    int tw = (int)strlen(cb->text) * DOBCB_FONT_W;
    if (tw > 0)
        return cb->box_size + cb->text_gap + tw;
    return cb->box_size;
}

bool dobcb_HitTest(dob_checkbox_t *cb, int px, int py)
{
    int total_w = dobcb_GetWidth(cb);
    int total_h = cb->box_size;
    if (DOBCB_FONT_H > total_h) total_h = DOBCB_FONT_H;
    return px >= cb->x && px < cb->x + total_w
        && py >= cb->y && py < cb->y + total_h;
}

bool dobcb_OnClick(dob_checkbox_t *cb, int x, int y)
{
    if (!cb->visible || !cb->enabled) return false;
    if (dobcb_HitTest(cb, x, y))
    {
        cb->checked = !cb->checked;
        return true;
    }
    return false;
}

bool dobcb_OnKey(dob_checkbox_t *cb, uint8_t key)
{
    if (!cb->visible || !cb->enabled || !cb->focused) return false;
    if (key == ' ' || key == '\n')
    {
        cb->checked = !cb->checked;
        return true;
    }
    return false;
}

void dobcb_Draw(dob_checkbox_t *cb)
{
    if (!cb->visible) return;

    int bs = cb->box_size;
    uint32_t border_col, inner_col;

    if (!cb->enabled)
    {
        border_col = cb->col_box_disabled;
        inner_col  = cb->checked ? cb->col_check_disabled : cb->col_box_bg;
    }
    else
    {
        border_col = cb->focused ? cb->col_focus : cb->col_box;
        inner_col  = cb->checked ? cb->col_check : cb->col_box_bg;
    }

    /* Box background */
    dobui_FillRect(cb->win_id, cb->x, cb->y, bs, bs, inner_col);

    /* Box border */
    dobui_DrawRect(cb->win_id, cb->x, cb->y, bs, bs, border_col);

    /* Check mark: filled inner square */
    if (cb->checked)
    {
        int p = DOBCB_CHECK_PAD;
        int inner = bs - 2 * p;
        if (inner > 0)
            dobui_FillRect(cb->win_id, cb->x + p, cb->y + p,
                           inner, inner, inner_col);
    }

    /* Text */
    if (cb->text[0] != '\0')
    {
        int tx = cb->x + bs + cb->text_gap;
        int ty = cb->y + (bs - DOBCB_FONT_H) / 2;
        uint32_t fg = cb->enabled ? cb->col_text : cb->col_box_disabled;
        int tw = (int)strlen(cb->text) * DOBCB_FONT_W;
        int clear_w = (cb->prev_text_w > tw) ? cb->prev_text_w : tw;
        dobui_FillRect(cb->win_id, tx, ty, clear_w, DOBCB_FONT_H, cb->col_bg);
        dobui_DrawText(cb->win_id, tx, ty, cb->text, fg, cb->col_bg);
        cb->prev_text_w = tw;
    }
}
