/* DobUITools — Slider Implementation */

#include "slider.h"
#include <DobInterface.h>
#include <string.h>

void dobsl_Init(dob_slider_t *sl, uint32_t win_id,
                int x, int y, int w, int h)
{
    memset(sl, 0, sizeof(*sl));
    sl->win_id          = win_id;
    sl->x               = x;
    sl->y               = y;
    sl->w               = w;
    sl->h               = (h > 0) ? h : DOBSL_DEFAULT_H;
    sl->max             = 100;
    sl->step            = 1;
    sl->thumb_w         = DOBSL_THUMB_W;
    sl->visible         = true;
    sl->enabled         = true;
    sl->col_track       = DOBSL_COL_TRACK;
    sl->col_fill        = DOBSL_COL_FILL;
    sl->col_thumb       = DOBSL_COL_THUMB;
    sl->col_thumb_disabled = DOBSL_COL_THUMB_DIS;
    sl->col_border      = DOBSL_COL_BORDER;
    sl->col_focus       = DOBSL_COL_FOCUS;
    sl->col_text        = DOBSL_COL_TEXT;
    sl->col_text_bg     = DOBSL_COL_TEXT_BG;
    if (dobfocus_auto_register) dobfocus_auto_register(sl, DOB_CTRL_SLIDER);
}

static void sl_clamp(dob_slider_t *sl)
{
    if (sl->value < sl->min) sl->value = sl->min;
    if (sl->value > sl->max) sl->value = sl->max;
}

void dobsl_SetValue(dob_slider_t *sl, int value)
{
    sl->value = value;
    sl_clamp(sl);
}

void dobsl_SetRange(dob_slider_t *sl, int min, int max)
{
    sl->min = min;
    sl->max = (max > min) ? max : min + 1;
    sl_clamp(sl);
}

void dobsl_SetFocus(dob_slider_t *sl, bool focused)
{
    sl->focused = focused;
}

bool dobsl_HitTest(dob_slider_t *sl, int px, int py)
{
    return px >= sl->x && px < sl->x + sl->w
        && py >= sl->y && py < sl->y + sl->h;
}

static int sl_calc_from_x(dob_slider_t *sl, int x)
{
    int range = sl->max - sl->min;
    if (range <= 0) return sl->min;
    int track_w = sl->w - sl->thumb_w;
    if (track_w <= 0) track_w = 1;
    int rel = x - sl->x - sl->thumb_w / 2;
    if (rel < 0) rel = 0;
    if (rel > track_w) rel = track_w;
    int val = sl->min + rel * range / track_w;
    if (val < sl->min) val = sl->min;
    if (val > sl->max) val = sl->max;
    return val;
}

bool dobsl_OnClick(dob_slider_t *sl, int x, int y)
{
    if (!sl->visible || !sl->enabled) return false;
    if (!dobsl_HitTest(sl, x, y)) return false;
    sl->drag_value = sl_calc_from_x(sl, x);
    sl->grabbed = true;
    return true;
}

bool dobsl_OnDrag(dob_slider_t *sl, int x, int y)
{
    (void)y;
    if (!sl->grabbed) return false;
    sl->drag_value = sl_calc_from_x(sl, x);
    return true;
}

void dobsl_OnRelease(dob_slider_t *sl)
{
    if (sl->grabbed)
    {
        sl->value = sl->drag_value;
        sl->grabbed = false;
    }
}

bool dobsl_OnKey(dob_slider_t *sl, uint8_t key)
{
    if (!sl->visible || !sl->enabled || !sl->focused) return false;

    if (key == 131 || key == 128) /* RIGHT or UP */
    {
        sl->value += sl->step;
        sl_clamp(sl);
        return true;
    }
    if (key == 130 || key == 129) /* LEFT or DOWN */
    {
        sl->value -= sl->step;
        sl_clamp(sl);
        return true;
    }
    if (key == 132) /* HOME */
    {
        sl->value = sl->min;
        return true;
    }
    if (key == 133) /* END */
    {
        sl->value = sl->max;
        return true;
    }
    return false;
}

void dobsl_Draw(dob_slider_t *sl)
{
    if (!sl->visible) return;

    int range = sl->max - sl->min;
    int track_w = sl->w - sl->thumb_w;
    if (track_w < 1) track_w = 1;

    /* Track background */
    dobui_FillRect(sl->win_id, sl->x, sl->y, sl->w, sl->h, sl->col_track);

    /* Fill from left to thumb position */
    int display_val = sl->grabbed ? sl->drag_value : sl->value;
    int thumb_x = 0;
    if (range > 0)
        thumb_x = (display_val - sl->min) * track_w / range;
    if (thumb_x > track_w) thumb_x = track_w;

    if (thumb_x > 0)
        dobui_FillRect(sl->win_id, sl->x, sl->y, thumb_x, sl->h, sl->col_fill);

    /* Thumb */
    uint32_t tc = sl->enabled ? sl->col_thumb : sl->col_thumb_disabled;
    dobui_FillRect(sl->win_id, sl->x + thumb_x, sl->y,
                   sl->thumb_w, sl->h, tc);

    /* Border */
    uint32_t bc = sl->focused ? sl->col_focus : sl->col_border;
    dobui_DrawRect(sl->win_id, sl->x, sl->y, sl->w, sl->h, bc);

    /* Value text */
    if (sl->show_value)
    {
        char buf[16];
        int v = display_val;
        int pos = 0;
        bool neg = false;
        if (v < 0) { neg = true; v = -v; }
        if (v == 0) buf[pos++] = '0';
        else
        {
            char tmp[12];
            int tp = 0;
            while (v > 0) { tmp[tp++] = '0' + (char)(v % 10); v /= 10; }
            if (neg) buf[pos++] = '-';
            for (int i = tp - 1; i >= 0; i--) buf[pos++] = tmp[i];
        }
        buf[pos] = '\0';

        int tx = sl->x + sl->w + 6;
        int ty = sl->y + (sl->h - DOBSL_FONT_H) / 2;

        /* Clear previous value text area to avoid garbage */
        dobui_FillRect(sl->win_id, tx, ty,
                       DOBSL_FONT_W * 8, DOBSL_FONT_H, sl->col_text_bg);

        dobui_DrawText(sl->win_id, tx, ty, buf, sl->col_text, sl->col_text_bg);
    }
}
