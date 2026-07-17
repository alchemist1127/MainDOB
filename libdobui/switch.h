/* DobUITools — Switch Control
 *
 * Horizontal toggle switch with ON/OFF visual state.
 * Green (left) = OFF, Red (right) = ON.
 *
 * Usage:
 *   dob_switch_t sw;
 *   dobsw_Init(&sw, win_id, 10, 50, 0, "Abilita suoni");
 *   if (dobsw_OnClick(&sw, x, y)) redraw();
 *   dobsw_Draw(&sw);
 */

#ifndef MAINDOB_DOBUITOOLS_SWITCH_H
#define MAINDOB_DOBUITOOLS_SWITCH_H

#include <dob/types.h>
#include "dobui_common.h"
#include "dobui_theme.h"

#define DOBSW_DEFAULT_W     36
#define DOBSW_DEFAULT_H     18
#define DOBSW_KNOB_PAD      2
#define DOBSW_TEXT_GAP      6
#define DOBSW_FONT_W        8
#define DOBSW_FONT_H        16

#define DOBSW_COL_OFF       DOBUI_SUCCESS      /* Green — OFF (left) */
#define DOBSW_COL_ON        DOBUI_DANGER      /* Red — ON (right) */
#define DOBSW_COL_KNOB      DOBUI_TEXT_ALT
#define DOBSW_COL_DIS_BG    DOBUI_DISABLED
#define DOBSW_COL_DIS_KNOB  DOBUI_DISABLED
#define DOBSW_COL_BORDER    DOBUI_SURFACE
#define DOBSW_COL_FOCUS     DOBUI_TEXT
#define DOBSW_COL_TEXT      DOBUI_TEXT
#define DOBSW_COL_BG        DOBUI_SURFACE

typedef struct
{
    uint32_t    win_id;
    int         x, y;
    int         sw_w, sw_h;         /* Switch track dimensions */
    char        text[128];
    int         text_gap;

    bool        on;
    bool        visible;
    bool        enabled;
    bool        focused;

    uint32_t    col_off;
    uint32_t    col_on;
    uint32_t    col_knob;
    uint32_t    col_disabled_bg;
    uint32_t    col_disabled_knob;
    uint32_t    col_border;
    uint32_t    col_focus;
    uint32_t    col_text;
    uint32_t    col_bg;             /* Text cell background */
    int         prev_text_w;        /* Previous text width for garbage cleanup */
} dob_switch_t;

/* Init at (x, y). sw_w/sw_h 0 = defaults. */
void dobsw_Init(dob_switch_t *sw, uint32_t win_id,
                int x, int y, int sw_w, const char *text);

void dobsw_SetText(dob_switch_t *sw, const char *text);
void dobsw_SetOn(dob_switch_t *sw, bool on);
void dobsw_SetEnabled(dob_switch_t *sw, bool enabled);
void dobsw_SetFocus(dob_switch_t *sw, bool focused);

/* Returns true if hit. Toggles on state. */
bool dobsw_OnClick(dob_switch_t *sw, int x, int y);

/* Space/Enter toggles if focused. */
bool dobsw_OnKey(dob_switch_t *sw, uint8_t key);

void dobsw_Draw(dob_switch_t *sw);

/* Hit test — switch track + text area. */
bool dobsw_HitTest(dob_switch_t *sw, int px, int py);

/* Total width (track + gap + text). */
int  dobsw_GetWidth(dob_switch_t *sw);

#endif
