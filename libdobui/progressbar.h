/* DobUITools — ProgressBar Control
 *
 * Horizontal progress bar with optional percentage text.
 *
 * Usage:
 *   dob_progressbar_t pb;
 *   dobpb_Init(&pb, win_id, 10, 80, 200, 0);
 *   dobpb_SetValue(&pb, 65);
 *   dobpb_Draw(&pb);
 */

#ifndef MAINDOB_DOBUITOOLS_PROGRESSBAR_H
#define MAINDOB_DOBUITOOLS_PROGRESSBAR_H

#include <dob/types.h>
#include "dobui_common.h"
#include "dobui_theme.h"

#define DOBPB_DEFAULT_H     12
#define DOBPB_FONT_W        8
#define DOBPB_FONT_H        16

#define DOBPB_COL_BG        DOBUI_INSET
#define DOBPB_COL_FILL      DOBUI_SUCCESS
#define DOBPB_COL_BORDER    DOBUI_SURFACE
#define DOBPB_COL_TEXT      DOBUI_TEXT
#define DOBPB_COL_TEXT_BG   DOBUI_INSET

typedef struct
{
    uint32_t    win_id;
    int         x, y;
    int         w, h;

    int         value;          /* Current (0..max) */
    int         max;            /* Maximum value */

    bool        visible;
    bool        enabled;
    bool        show_text;      /* Draw percentage text to the right */
    bool        show_border;

    uint32_t    col_bg;
    uint32_t    col_fill;
    uint32_t    col_border;
    uint32_t    col_text;
    uint32_t    col_text_bg;    /* Background for percentage text cells */
} dob_progressbar_t;

/* Init at (x, y) with size (w, h). h=0 for default. */
void dobpb_Init(dob_progressbar_t *pb, uint32_t win_id,
                int x, int y, int w, int h);

void dobpb_SetValue(dob_progressbar_t *pb, int value);
void dobpb_SetMax(dob_progressbar_t *pb, int max);
void dobpb_Draw(dob_progressbar_t *pb);

/* Get fill width in pixels (for custom drawing alongside). */
int  dobpb_GetFillWidth(dob_progressbar_t *pb);

#endif
