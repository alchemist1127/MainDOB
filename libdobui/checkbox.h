/* DobUITools — Checkbox Control
 *
 * Square toggle box with optional label text.
 *
 * Usage:
 *   dob_checkbox_t cb;
 *   dobcb_Init(&cb, win_id, 10, 50, 0, "Attiva notifiche");
 *   if (dobcb_OnClick(&cb, x, y)) redraw();
 *   dobcb_Draw(&cb);
 */

#ifndef MAINDOB_DOBUITOOLS_CHECKBOX_H
#define MAINDOB_DOBUITOOLS_CHECKBOX_H

#include <dob/types.h>
#include "dobui_common.h"
#include "dobui_theme.h"

#define DOBCB_DEFAULT_SIZE  14
#define DOBCB_TEXT_GAP      6
#define DOBCB_FONT_W        8
#define DOBCB_FONT_H        16
#define DOBCB_CHECK_PAD     3     /* Inner padding for check fill */

#define DOBCB_COL_BOX       DOBUI_SURFACE
#define DOBCB_COL_BOX_BG    DOBUI_INSET
#define DOBCB_COL_CHECK     DOBUI_INPUT
#define DOBCB_COL_BOX_DIS   DOBUI_DISABLED
#define DOBCB_COL_CHECK_DIS DOBUI_DISABLED
#define DOBCB_COL_TEXT      DOBUI_TEXT
#define DOBCB_COL_BG        DOBUI_SURFACE
#define DOBCB_COL_FOCUS     DOBUI_TEXT

typedef struct
{
    uint32_t    win_id;
    int         x, y;
    int         box_size;       /* Side length of the box */
    char        text[128];
    int         text_gap;

    bool        checked;
    bool        visible;
    bool        enabled;
    bool        focused;

    uint32_t    col_box;
    uint32_t    col_box_bg;     /* Box interior unchecked */
    uint32_t    col_check;      /* Box interior checked */
    uint32_t    col_box_disabled;
    uint32_t    col_check_disabled;
    uint32_t    col_text;
    uint32_t    col_bg;         /* Text cell background */
    uint32_t    col_focus;
    int         prev_text_w;    /* Previous text width for garbage cleanup */
} dob_checkbox_t;

/* Init at (x, y), box_size 0 = default. */
void dobcb_Init(dob_checkbox_t *cb, uint32_t win_id,
                int x, int y, int box_size, const char *text);

void dobcb_SetText(dob_checkbox_t *cb, const char *text);
void dobcb_SetChecked(dob_checkbox_t *cb, bool checked);
void dobcb_SetEnabled(dob_checkbox_t *cb, bool enabled);
void dobcb_SetFocus(dob_checkbox_t *cb, bool focused);

/* Returns true if hit. Toggles checked state. */
bool dobcb_OnClick(dob_checkbox_t *cb, int x, int y);

/* Space/Enter toggles if focused. */
bool dobcb_OnKey(dob_checkbox_t *cb, uint8_t key);

void dobcb_Draw(dob_checkbox_t *cb);

/* Hit test — box + text area. */
bool dobcb_HitTest(dob_checkbox_t *cb, int px, int py);

/* Total width (box + gap + text). */
int  dobcb_GetWidth(dob_checkbox_t *cb);

#endif
