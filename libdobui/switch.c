/* DobUITools — Switch Implementation */

#include "switch.h"
#include <DobInterface.h>
#include <string.h>

void dobsw_Init(dob_switch_t *sw, uint32_t win_id,
                int x, int y, int sw_w, const char *text)
{
    memset(sw, 0, sizeof(*sw));
    sw->win_id            = win_id;
    sw->x                 = x;
    sw->y                 = y;
    sw->sw_w              = (sw_w > 0) ? sw_w : DOBSW_DEFAULT_W;
    sw->sw_h              = DOBSW_DEFAULT_H;
    sw->text_gap          = DOBSW_TEXT_GAP;
    sw->visible           = true;
    sw->enabled           = true;
    sw->col_off           = DOBSW_COL_OFF;
    sw->col_on            = DOBSW_COL_ON;
    sw->col_knob          = DOBSW_COL_KNOB;
    sw->col_disabled_bg   = DOBSW_COL_DIS_BG;
    sw->col_disabled_knob = DOBSW_COL_DIS_KNOB;
    sw->col_border        = DOBSW_COL_BORDER;
    sw->col_focus         = DOBSW_COL_FOCUS;
    sw->col_text          = DOBSW_COL_TEXT;
    sw->col_bg            = DOBSW_COL_BG;

    if (text)
    {
        int n = (int)strlen(text);
        if (n > 127) n = 127;
        memcpy(sw->text, text, (uint32_t)n);
        sw->text[n] = '\0';
    }
    if (dobfocus_auto_register) dobfocus_auto_register(sw, DOB_CTRL_SWITCH);
}

void dobsw_SetText(dob_switch_t *sw, const char *text)
{
    if (!text) text = "";
    int n = (int)strlen(text);
    if (n > 127) n = 127;
    memcpy(sw->text, text, (uint32_t)n);
    sw->text[n] = '\0';
}

void dobsw_SetOn(dob_switch_t *sw, bool on)       { sw->on = on; }
void dobsw_SetEnabled(dob_switch_t *sw, bool en)   { sw->enabled = en; }
void dobsw_SetFocus(dob_switch_t *sw, bool focused) { sw->focused = focused; }

int dobsw_GetWidth(dob_switch_t *sw)
{
    int tw = (int)strlen(sw->text) * DOBSW_FONT_W;
    if (tw > 0)
        return sw->sw_w + sw->text_gap + tw;
    return sw->sw_w;
}

bool dobsw_HitTest(dob_switch_t *sw, int px, int py)
{
    int total_w = dobsw_GetWidth(sw);
    int total_h = sw->sw_h;
    if (DOBSW_FONT_H > total_h) total_h = DOBSW_FONT_H;
    return px >= sw->x && px < sw->x + total_w
        && py >= sw->y && py < sw->y + total_h;
}

bool dobsw_OnClick(dob_switch_t *sw, int x, int y)
{
    if (!sw->visible || !sw->enabled) return false;
    if (dobsw_HitTest(sw, x, y))
    {
        sw->on = !sw->on;
        return true;
    }
    return false;
}

bool dobsw_OnKey(dob_switch_t *sw, uint8_t key)
{
    if (!sw->visible || !sw->enabled || !sw->focused) return false;
    if (key == ' ' || key == '\n')
    {
        sw->on = !sw->on;
        return true;
    }
    return false;
}

void dobsw_Draw(dob_switch_t *sw)
{
    if (!sw->visible) return;

    int tw = sw->sw_w;
    int th = sw->sw_h;
    int pad = DOBSW_KNOB_PAD;
    int knob_size = th - 2 * pad;

    uint32_t bg_col, knob_col, border_col;

    if (!sw->enabled)
    {
        bg_col     = sw->col_disabled_bg;
        knob_col   = sw->col_disabled_knob;
        border_col = sw->col_disabled_bg;
    }
    else
    {
        bg_col     = sw->on ? sw->col_on : sw->col_off;
        knob_col   = sw->col_knob;
        border_col = sw->focused ? sw->col_focus : sw->col_border;
    }

    /* Track background */
    dobui_FillRect(sw->win_id, sw->x, sw->y, tw, th, bg_col);

    /* Track border */
    dobui_DrawRect(sw->win_id, sw->x, sw->y, tw, th, border_col);

    /* Knob position: left when OFF, right when ON */
    int knob_x;
    if (sw->on)
        knob_x = sw->x + tw - pad - knob_size;
    else
        knob_x = sw->x + pad;

    int knob_y = sw->y + pad;

    dobui_FillRect(sw->win_id, knob_x, knob_y, knob_size, knob_size, knob_col);

    /* Text label */
    if (sw->text[0] != '\0')
    {
        int tx = sw->x + tw + sw->text_gap;
        int ty = sw->y + (th - DOBSW_FONT_H) / 2;
        uint32_t fg = sw->enabled ? sw->col_text : sw->col_disabled_bg;
        int text_w = (int)strlen(sw->text) * DOBSW_FONT_W;
        int clear_w = (sw->prev_text_w > text_w) ? sw->prev_text_w : text_w;
        dobui_FillRect(sw->win_id, tx, ty, clear_w, DOBSW_FONT_H, sw->col_bg);
        dobui_DrawText(sw->win_id, tx, ty, sw->text, fg, sw->col_bg);
        sw->prev_text_w = text_w;
    }
}
