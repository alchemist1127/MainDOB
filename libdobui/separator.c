/* DobUITools — Separator Implementation */

#include "separator.h"
#include <DobInterface.h>
#include <string.h>

void dobsep_Init(dob_separator_t *sep, uint32_t win_id,
                 int x, int y, int length, bool vertical)
{
    memset(sep, 0, sizeof(*sep));
    sep->win_id    = win_id;
    sep->x         = x;
    sep->y         = y;
    sep->length    = length;
    sep->thickness = 1;
    sep->vertical  = vertical;
    sep->visible   = true;
    sep->col       = DOBSEP_COL_DEFAULT;
}

void dobsep_Draw(dob_separator_t *sep)
{
    if (!sep->visible) return;

    if (sep->vertical)
        dobui_FillRect(sep->win_id, sep->x, sep->y,
                       sep->thickness, sep->length, sep->col);
    else
        dobui_FillRect(sep->win_id, sep->x, sep->y,
                       sep->length, sep->thickness, sep->col);
}
