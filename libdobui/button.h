/* DobUITools — Button Control
 *
 * Clickable button rendered via DobInterface primitives.
 *
 * Usage:
 *   dob_button_t btn;
 *   dobbtn_Init(&btn, win_id, 10, 50, 80, 24, "OK");
 *   if (dobbtn_OnClick(&btn, x, y)) { ... btn.clicked = false; }
 *   dobbtn_Draw(&btn);
 */

#ifndef MAINDOB_DOBUITOOLS_BUTTON_H
#define MAINDOB_DOBUITOOLS_BUTTON_H

#include <dob/types.h>
#include "dobui_common.h"
#include "dobui_theme.h"

#define DOBBTN_FONT_W       8
#define DOBBTN_FONT_H       16
#define DOBBTN_DEFAULT_H    24
#define DOBBTN_PAD          8

/* bottone = riquadro + testo dello STESSO colore.
 * Default neutro = bianco; focus = giallo; fondo = inserto nero.
 * Per un bottone ciano/giallo/rosso: imposta col_fg E col_border
 * (sull'istanza) allo stesso token. */
#define DOBBTN_COL_BG       DOBUI_INSET      /* nero              */
#define DOBBTN_COL_BG_PRESS DOBUI_SURFACE    /* blu (feedback)    */
#define DOBBTN_COL_BG_DIS   DOBUI_INSET      /* nero              */
#define DOBBTN_COL_FG       DOBUI_TEXT_ALT   /* bianco (= bordo)  */
#define DOBBTN_COL_FG_DIS   DOBUI_DISABLED
#define DOBBTN_COL_BORDER   DOBUI_TEXT_ALT   /* bianco (= testo)  */
#define DOBBTN_COL_FOCUS    DOBUI_INPUT      /* giallo            */

typedef struct
{
    uint32_t    win_id;
    int         x, y;
    int         w, h;
    char        label[64];

    bool        visible;
    bool        enabled;
    bool        focused;
    bool        pressed;
    bool        clicked;

    dob_anchor_t anchor;

    uint32_t    col_bg;
    uint32_t    col_bg_press;
    uint32_t    col_bg_disabled;
    uint32_t    col_fg;
    uint32_t    col_fg_disabled;
    uint32_t    col_border;
    uint32_t    col_focus;
} dob_button_t;

void dobbtn_Init(dob_button_t *btn, uint32_t win_id,
                 int x, int y, int w, int h,
                 const char *label);

void dobbtn_SetLabel(dob_button_t *btn, const char *label);
bool dobbtn_OnClick(dob_button_t *btn, int x, int y);
void dobbtn_OnRelease(dob_button_t *btn);
bool dobbtn_OnKey(dob_button_t *btn, uint8_t key);
void dobbtn_Draw(dob_button_t *btn);
void dobbtn_SetEnabled(dob_button_t *btn, bool enabled);
void dobbtn_SetFocus(dob_button_t *btn, bool focused);
bool dobbtn_HitTest(dob_button_t *btn, int px, int py);

/* Button Row — helper for laying out multiple buttons */

#define DOBBTN_ROW_MAX  8
#define DOBBTN_ROW_GAP  8

typedef struct
{
    dob_button_t    buttons[DOBBTN_ROW_MAX];
    int             count;
    uint32_t        win_id;
    int             center_x;
    int             y;
} dob_button_row_t;

void dobbtn_RowInit(dob_button_row_t *row, uint32_t win_id,
                    int center_x, int y);
dob_button_t *dobbtn_RowAdd(dob_button_row_t *row, const char *label);
void dobbtn_RowLayout(dob_button_row_t *row);
int  dobbtn_RowOnClick(dob_button_row_t *row, int x, int y);
int  dobbtn_RowOnKey(dob_button_row_t *row, uint8_t key);
void dobbtn_RowDraw(dob_button_row_t *row);

#endif
