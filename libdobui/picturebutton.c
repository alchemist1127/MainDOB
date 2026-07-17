/* DobUITools — PictureButton Implementation */

#include "picturebutton.h"
#include <DobInterface.h>
#include <string.h>

void dobpbtn_Init(dob_picturebutton_t *btn, uint32_t win_id,
                  int x, int y, int w, int h)
{
    memset(btn, 0, sizeof(*btn));
    btn->win_id          = win_id;
    btn->x               = x;
    btn->y               = y;
    btn->w               = (w > 0) ? w : DOBPBTN_DEFAULT_SIZE;
    btn->h               = (h > 0) ? h : DOBPBTN_DEFAULT_SIZE;
    btn->visible         = true;
    btn->enabled         = true;
    btn->anchor          = DOB_ANCHOR_CENTER;
    btn->col_bg          = DOBPBTN_COL_BG;
    btn->col_bg_press    = DOBPBTN_COL_BG_PRESS;
    btn->col_bg_disabled = DOBPBTN_COL_BG_DIS;
    btn->col_border      = DOBPBTN_COL_BORDER;
    btn->col_focus       = DOBPBTN_COL_FOCUS;
    if (dobfocus_auto_register) dobfocus_auto_register(btn, DOB_CTRL_PICTUREBUTTON);
}

void dobpbtn_SetImage(dob_picturebutton_t *btn,
                      const uint32_t *pixels, int img_w, int img_h)
{
    btn->pixels = pixels;
    btn->img_w  = img_w;
    btn->img_h  = img_h;
}

void dobpbtn_SetEnabled(dob_picturebutton_t *btn, bool enabled)
{
    btn->enabled = enabled;
    if (!enabled)
        btn->pressed = false;
}

void dobpbtn_SetFocus(dob_picturebutton_t *btn, bool focused)
{
    btn->focused = focused;
}

bool dobpbtn_HitTest(dob_picturebutton_t *btn, int px, int py)
{
    return px >= btn->x && px < btn->x + btn->w
        && py >= btn->y && py < btn->y + btn->h;
}

bool dobpbtn_OnClick(dob_picturebutton_t *btn, int x, int y)
{
    if (!btn->visible || !btn->enabled) return false;
    if (dobpbtn_HitTest(btn, x, y))
    {
        /* No sticky `pressed` latch -- see dobbtn_OnClick. */
        btn->clicked = true;
        return true;
    }
    return false;
}

void dobpbtn_OnRelease(dob_picturebutton_t *btn)
{
    btn->pressed = false;
}

bool dobpbtn_OnKey(dob_picturebutton_t *btn, uint8_t key)
{
    if (!btn->visible || !btn->enabled || !btn->focused) return false;
    if (key == '\n' || key == ' ')
    {
        btn->clicked = true;
        return true;
    }
    return false;
}

void dobpbtn_Draw(dob_picturebutton_t *btn)
{
    if (!btn->visible) return;

    uint32_t bg, border;

    if (!btn->enabled)
    {
        bg     = btn->col_bg_disabled;
        border = btn->col_border;
    }
    else if (btn->pressed)
    {
        bg     = btn->col_bg_press;
        border = btn->col_border;
    }
    else
    {
        bg     = btn->col_bg;
        border = btn->focused ? btn->col_focus : btn->col_border;
    }

    /* Background */
    dobui_FillRect(btn->win_id, btn->x, btn->y, btn->w, btn->h, bg);

    /* Border */
    dobui_DrawRect(btn->win_id, btn->x, btn->y, btn->w, btn->h, border);

    /* Image centered in button */
    if (btn->pixels && btn->img_w > 0 && btn->img_h > 0)
    {
        int ix, iy;
        dob_anchor_pos(btn->anchor, btn->x, btn->y, btn->w, btn->h,
                       btn->img_w, btn->img_h, DOBPBTN_PAD, &ix, &iy);
        dobui_BlitBuffer(btn->win_id, ix, iy,
                         btn->pixels, btn->img_w, btn->img_h);
    }
}
