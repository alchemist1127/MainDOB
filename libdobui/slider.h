/* DobUITools — Slider Control
 *
 * Horizontal slider for numeric value selection.
 *
 * Usage:
 *   dob_slider_t sl;
 *   dobsl_Init(&sl, win_id, 10, 80, 200, 0);
 *   sl.min = 0; sl.max = 100; sl.value = 50;
 *   if (dobsl_OnClick(&sl, x, y)) redraw();
 *   dobsl_Draw(&sl);
 */

#ifndef MAINDOB_DOBUITOOLS_SLIDER_H
#define MAINDOB_DOBUITOOLS_SLIDER_H

#include <dob/types.h>
#include "dobui_common.h"
#include "dobui_theme.h"

#define DOBSL_DEFAULT_H     16
#define DOBSL_THUMB_W       10
#define DOBSL_FONT_W        8
#define DOBSL_FONT_H        16

#define DOBSL_COL_TRACK     DOBUI_INSET
#define DOBSL_COL_FILL      DOBUI_SUCCESS
#define DOBSL_COL_THUMB     DOBUI_TEXT_ALT
#define DOBSL_COL_THUMB_DIS DOBUI_DISABLED
#define DOBSL_COL_BORDER    DOBUI_SURFACE
#define DOBSL_COL_FOCUS     DOBUI_TEXT
#define DOBSL_COL_TEXT      DOBUI_TEXT
#define DOBSL_COL_TEXT_BG   DOBUI_INSET

typedef struct
{
    uint32_t    win_id;
    int         x, y;
    int         w, h;

    int         value;
    int         min, max;
    int         step;           /* Keyboard step (default 1) */
    int         drag_value;     /* Preview value during drag (committed on release) */
    int         thumb_w;

    bool        visible;
    bool        enabled;
    bool        focused;
    bool        grabbed;        /* Thumb is being dragged */
    bool        show_value;     /* Show numeric value to the right */

    uint32_t    col_track;
    uint32_t    col_fill;
    uint32_t    col_thumb;
    uint32_t    col_thumb_disabled;
    uint32_t    col_border;
    uint32_t    col_focus;
    uint32_t    col_text;
    uint32_t    col_text_bg;
} dob_slider_t;

void dobsl_Init(dob_slider_t *sl, uint32_t win_id,
                int x, int y, int w, int h);

void dobsl_SetValue(dob_slider_t *sl, int value);
void dobsl_SetRange(dob_slider_t *sl, int min, int max);
void dobsl_SetFocus(dob_slider_t *sl, bool focused);

/* Click positions the thumb. Returns true if hit. */
bool dobsl_OnClick(dob_slider_t *sl, int x, int y);

/* Drag updates thumb while grabbed. Returns true if active. */
bool dobsl_OnDrag(dob_slider_t *sl, int x, int y);

/* Release ends drag. */
void dobsl_OnRelease(dob_slider_t *sl);

/* Arrows adjust value by step. Returns true if consumed. */
bool dobsl_OnKey(dob_slider_t *sl, uint8_t key);

void dobsl_Draw(dob_slider_t *sl);

bool dobsl_HitTest(dob_slider_t *sl, int px, int py);

#endif
