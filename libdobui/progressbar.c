/* DobUITools — ProgressBar Implementation */

#include "progressbar.h"
#include <DobInterface.h>
#include <string.h>

void dobpb_Init(dob_progressbar_t *pb, uint32_t win_id,
                int x, int y, int w, int h)
{
    memset(pb, 0, sizeof(*pb));
    pb->win_id      = win_id;
    pb->x           = x;
    pb->y           = y;
    pb->w           = w;
    pb->h           = (h > 0) ? h : DOBPB_DEFAULT_H;
    pb->max         = 100;
    pb->visible     = true;
    pb->enabled     = true;
    pb->show_border = true;
    pb->col_bg      = DOBPB_COL_BG;
    pb->col_fill    = DOBPB_COL_FILL;
    pb->col_border  = DOBPB_COL_BORDER;
    pb->col_text    = DOBPB_COL_TEXT;
    pb->col_text_bg = DOBPB_COL_TEXT_BG;
}

void dobpb_SetValue(dob_progressbar_t *pb, int value)
{
    if (value < 0) value = 0;
    if (value > pb->max) value = pb->max;
    pb->value = value;
}

void dobpb_SetMax(dob_progressbar_t *pb, int max)
{
    if (max < 1) max = 1;
    pb->max = max;
    if (pb->value > max) pb->value = max;
}

int dobpb_GetFillWidth(dob_progressbar_t *pb)
{
    if (pb->max <= 0) return 0;
    int fill = pb->value * pb->w / pb->max;
    if (fill < 0) fill = 0;
    if (fill > pb->w) fill = pb->w;
    return fill;
}

void dobpb_Draw(dob_progressbar_t *pb)
{
    if (!pb->visible) return;

    /* Track background */
    dobui_FillRect(pb->win_id, pb->x, pb->y, pb->w, pb->h, pb->col_bg);

    /* Fill */
    int fill = dobpb_GetFillWidth(pb);
    if (fill > 0)
        dobui_FillRect(pb->win_id, pb->x, pb->y, fill, pb->h, pb->col_fill);

    /* Border */
    if (pb->show_border)
        dobui_DrawRect(pb->win_id, pb->x, pb->y, pb->w, pb->h, pb->col_border);

    /* Percentage text */
    if (pb->show_text)
    {
        int pct = (pb->max > 0) ? pb->value * 100 / pb->max : 0;
        char buf[8];
        int pos = 0;
        if (pct >= 100) buf[pos++] = '1';
        if (pct >= 10)  buf[pos++] = '0' + (pct / 10) % 10;
        buf[pos++] = '0' + pct % 10;
        buf[pos++] = '%';
        buf[pos] = '\0';

        int tx = pb->x + pb->w + 6;
        int ty = pb->y + (pb->h - DOBPB_FONT_H) / 2;
        dobui_DrawText(pb->win_id, tx, ty, buf, pb->col_text, pb->col_text_bg);
    }
}
