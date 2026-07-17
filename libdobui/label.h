/* DobUITools — Label Control
 *
 * Static text display, optionally with a colored background rectangle.
 * When has_bg is false, col_bg is used as the character cell background
 * by DrawText — set it to match your window color. */

#ifndef MAINDOB_DOBUITOOLS_LABEL_H
#define MAINDOB_DOBUITOOLS_LABEL_H

#include <dob/types.h>
#include "dobui_common.h"
#include "dobui_theme.h"

#define DOBLBL_FONT_W       8
#define DOBLBL_FONT_H       16
#define DOBLBL_PAD          4

#define DOBLBL_COL_TEXT     DOBUI_TEXT
#define DOBLBL_COL_BG       DOBUI_SURFACE

typedef struct
{
    uint32_t        win_id;
    int             x, y;
    int             w, h;           /* 0 = auto from text */
    char            text[256];

    uint32_t        col_text;
    uint32_t        col_bg;
    bool            has_bg;
    int             pad;
    dob_anchor_t    anchor;
    bool            visible;
    bool            enabled;
    int             prev_draw_w;    /* Previous drawn width (for garbage cleanup) */
} dob_label_t;

void doblbl_Init(dob_label_t *lbl, uint32_t win_id,
                 int x, int y, const char *text);

void doblbl_InitWithBg(dob_label_t *lbl, uint32_t win_id,
                       int x, int y, const char *text,
                       uint32_t text_color, uint32_t bg_color);

void doblbl_SetText(dob_label_t *lbl, const char *text);
void doblbl_Draw(dob_label_t *lbl);
int  doblbl_GetWidth(dob_label_t *lbl);
int  doblbl_GetHeight(dob_label_t *lbl);

#endif
