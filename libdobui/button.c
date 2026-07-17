/* DobUITools — Button Implementation */

#include "button.h"
#include <DobInterface.h>
#include <dob/font.h>
#include <string.h>

void dobbtn_Init(dob_button_t *btn, uint32_t win_id,
                 int x, int y, int w, int h,
                 const char *label)
{
    memset(btn, 0, sizeof(*btn));
    btn->win_id  = win_id;
    btn->x       = x;
    btn->y       = y;
    btn->h       = (h > 0) ? h : DOBBTN_DEFAULT_H;
    btn->visible = true;
    btn->enabled = true;
    btn->anchor  = DOB_ANCHOR_CENTER;

    if (label)
    {
        int n = (int)strlen(label);
        if (n > 63) n = 63;
        memcpy(btn->label, label, (uint32_t)n);
        btn->label[n] = '\0';
    }

    btn->w = (w > 0) ? w
             : (int)strlen(btn->label) * DOBBTN_FONT_W + 2 * DOBBTN_PAD;

    btn->col_bg          = DOBBTN_COL_BG;
    btn->col_bg_press    = DOBBTN_COL_BG_PRESS;
    btn->col_bg_disabled = DOBBTN_COL_BG_DIS;
    btn->col_fg          = DOBBTN_COL_FG;
    btn->col_fg_disabled = DOBBTN_COL_FG_DIS;
    btn->col_border      = DOBBTN_COL_BORDER;
    btn->col_focus       = DOBBTN_COL_FOCUS;
    if (dobfocus_auto_register) dobfocus_auto_register(btn, DOB_CTRL_BUTTON);
}

void dobbtn_SetLabel(dob_button_t *btn, const char *label)
{
    if (!label) label = "";
    int n = (int)strlen(label);
    if (n > 63) n = 63;
    memcpy(btn->label, label, (uint32_t)n);
    btn->label[n] = '\0';
}

void dobbtn_SetEnabled(dob_button_t *btn, bool enabled)
{
    btn->enabled = enabled;
    if (!enabled)
        btn->pressed = false;
}

void dobbtn_SetFocus(dob_button_t *btn, bool focused)
{
    btn->focused = focused;
}

bool dobbtn_HitTest(dob_button_t *btn, int px, int py)
{
    return px >= btn->x && px < btn->x + btn->w
        && py >= btn->y && py < btn->y + btn->h;
}

bool dobbtn_OnClick(dob_button_t *btn, int x, int y)
{
    if (!btn->visible || !btn->enabled) return false;
    if (dobbtn_HitTest(btn, x, y))
    {
        /* Do NOT latch `pressed`: it darkens the button (col_bg_press) and is
         * only cleared by dobbtn_OnRelease, which needs a mouse-release event
         * to reach the widget's window. Dialogs don't always get that release
         * (WM pointer-capture guards), so a latched press would stay stuck
         * dark. Registering the click alone is enough -- the click is atomic
         * from the caller's view -- so the stuck-dark button class of bug is
         * structurally impossible. */
        btn->clicked = true;
        return true;
    }
    return false;
}

void dobbtn_OnRelease(dob_button_t *btn)
{
    btn->pressed = false;
}

bool dobbtn_OnKey(dob_button_t *btn, uint8_t key)
{
    if (!btn->visible || !btn->enabled || !btn->focused) return false;
    if (key == '\n' || key == ' ')
    {
        btn->clicked = true;
        return true;
    }
    return false;
}

void dobbtn_Draw(dob_button_t *btn)
{
    if (!btn->visible) return;

    uint32_t bg, fg, border;

    if (!btn->enabled)
    {
        bg     = btn->col_bg_disabled;
        fg     = btn->col_fg_disabled;
        border = btn->col_border;
    }
    else if (btn->pressed)
    {
        bg     = btn->col_bg_press;
        fg     = btn->col_fg;
        border = btn->col_border;
    }
    else
    {
        bg     = btn->col_bg;
        /* in focus testo E bordo diventano col_focus (giallo) -> "riquadro = testo" */
        fg     = btn->focused ? btn->col_focus : btn->col_fg;
        border = btn->focused ? btn->col_focus : btn->col_border;
    }

    dobui_FillRect(btn->win_id, btn->x, btn->y, btn->w, btn->h, bg);
    dobui_DrawRect(btn->win_id, btn->x, btn->y, btn->w, btn->h, border);

    int tw = dob_text_width(btn->label, (uint32_t)strlen(btn->label));
    int tx, ty;
    dob_anchor_pos(btn->anchor, btn->x, btn->y, btn->w, btn->h,
                   tw, DOBBTN_FONT_H, DOBBTN_PAD, &tx, &ty);
    dobui_DrawText(btn->win_id, tx, ty, btn->label, fg, bg);
}

/* Button Row */

void dobbtn_RowInit(dob_button_row_t *row, uint32_t win_id,
                    int center_x, int y)
{
    memset(row, 0, sizeof(*row));
    row->win_id   = win_id;
    row->center_x = center_x;
    row->y        = y;
}

dob_button_t *dobbtn_RowAdd(dob_button_row_t *row, const char *label)
{
    if (row->count >= DOBBTN_ROW_MAX) return NULL;
    dob_button_t *btn = &row->buttons[row->count];
    dobbtn_Init(btn, row->win_id, 0, row->y, 0, DOBBTN_DEFAULT_H, label);
    row->count++;
    return btn;
}

void dobbtn_RowLayout(dob_button_row_t *row)
{
    if (row->count == 0) return;

    int total_w = 0;
    for (int i = 0; i < row->count; i++)
        total_w += row->buttons[i].w;
    total_w += (row->count - 1) * DOBBTN_ROW_GAP;

    int x = row->center_x - total_w / 2;
    for (int i = 0; i < row->count; i++)
    {
        row->buttons[i].x = x;
        row->buttons[i].y = row->y;
        x += row->buttons[i].w + DOBBTN_ROW_GAP;
    }
}

int dobbtn_RowOnClick(dob_button_row_t *row, int x, int y)
{
    for (int i = 0; i < row->count; i++)
    {
        if (dobbtn_OnClick(&row->buttons[i], x, y))
        {
            row->buttons[i].clicked = false;
            return i;
        }
    }
    return -1;
}

int dobbtn_RowOnKey(dob_button_row_t *row, uint8_t key)
{
    for (int i = 0; i < row->count; i++)
    {
        if (dobbtn_OnKey(&row->buttons[i], key))
        {
            row->buttons[i].clicked = false;
            return i;
        }
    }
    return -1;
}

void dobbtn_RowDraw(dob_button_row_t *row)
{
    for (int i = 0; i < row->count; i++)
        dobbtn_Draw(&row->buttons[i]);
}
