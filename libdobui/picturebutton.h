/* DobUITools — PictureButton Control
 *
 * Button that displays a bitmap image instead of text.
 *
 * Usage:
 *   static const uint32_t icon[] = { 0x00FF0000, 0x0000FF00, ... };
 *   dob_picturebutton_t pb;
 *   dobpbtn_Init(&pb, win_id, 10, 50, 40, 40);
 *   dobpbtn_SetImage(&pb, icon, 16, 16);
 *   if (dobpbtn_OnClick(&pb, x, y)) { ... pb.clicked = false; }
 *   dobpbtn_Draw(&pb);
 */

#ifndef MAINDOB_DOBUITOOLS_PICTUREBUTTON_H
#define MAINDOB_DOBUITOOLS_PICTUREBUTTON_H

#include <dob/types.h>
#include "dobui_common.h"
#include "dobui_theme.h"

#define DOBPBTN_DEFAULT_SIZE    32
#define DOBPBTN_PAD             4

#define DOBPBTN_COL_BG          DOBUI_INSET
#define DOBPBTN_COL_BG_PRESS    DOBUI_SURFACE
#define DOBPBTN_COL_BG_DIS      DOBUI_INSET
#define DOBPBTN_COL_BORDER      DOBUI_RELIEF
#define DOBPBTN_COL_FOCUS       DOBUI_INPUT

typedef struct
{
    uint32_t        win_id;
    int             x, y;
    int             w, h;

    const uint32_t *pixels;         /* Raw pixel buffer (0x00RRGGBB) */
    int             img_w, img_h;   /* Image dimensions */

    bool            visible;
    bool            enabled;
    bool            focused;
    bool            pressed;
    bool            clicked;

    dob_anchor_t    anchor;

    uint32_t        col_bg;
    uint32_t        col_bg_press;
    uint32_t        col_bg_disabled;
    uint32_t        col_border;
    uint32_t        col_focus;
} dob_picturebutton_t;

void dobpbtn_Init(dob_picturebutton_t *btn, uint32_t win_id,
                  int x, int y, int w, int h);

/* Set the image to display. Caller owns the pixel buffer. */
void dobpbtn_SetImage(dob_picturebutton_t *btn,
                      const uint32_t *pixels, int img_w, int img_h);

bool dobpbtn_OnClick(dob_picturebutton_t *btn, int x, int y);
void dobpbtn_OnRelease(dob_picturebutton_t *btn);
bool dobpbtn_OnKey(dob_picturebutton_t *btn, uint8_t key);
void dobpbtn_Draw(dob_picturebutton_t *btn);
void dobpbtn_SetEnabled(dob_picturebutton_t *btn, bool enabled);
void dobpbtn_SetFocus(dob_picturebutton_t *btn, bool focused);
bool dobpbtn_HitTest(dob_picturebutton_t *btn, int px, int py);

#endif
