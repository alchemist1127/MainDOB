/* DobUITools — Separator Control
 *
 * Horizontal or vertical line to divide UI sections.
 *
 * Usage:
 *   dob_separator_t sep;
 *   dobsep_Init(&sep, win_id, 10, 100, 200, false);
 *   dobsep_Draw(&sep);
 */

#ifndef MAINDOB_DOBUITOOLS_SEPARATOR_H
#define MAINDOB_DOBUITOOLS_SEPARATOR_H

#include <dob/types.h>
#include "dobui_theme.h"

#define DOBSEP_COL_DEFAULT  DOBUI_SURFACE

typedef struct
{
    uint32_t    win_id;
    int         x, y;
    int         length;
    int         thickness;      /* Default 1 */
    bool        vertical;
    bool        visible;
    uint32_t    col;
} dob_separator_t;

/* Init at (x, y), length in pixels, vertical=false for horizontal. */
void dobsep_Init(dob_separator_t *sep, uint32_t win_id,
                 int x, int y, int length, bool vertical);

void dobsep_Draw(dob_separator_t *sep);

#endif
