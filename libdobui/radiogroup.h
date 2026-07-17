/* DobUITools — RadioGroup Control
 *
 * Mutually exclusive selection from a vertical list of options.
 *
 * Usage:
 *   dob_radiogroup_t rg;
 *   dobrg_Init(&rg, win_id, 10, 50, 0);
 *   dobrg_AddItem(&rg, "Opzione 1");
 *   dobrg_AddItem(&rg, "Opzione 2");
 *   dobrg_AddItem(&rg, "Opzione 3");
 *   rg.selected = 0;
 *   dobrg_Draw(&rg);
 */

#ifndef MAINDOB_DOBUITOOLS_RADIOGROUP_H
#define MAINDOB_DOBUITOOLS_RADIOGROUP_H

#include <dob/types.h>
#include "dobui_common.h"
#include "dobui_theme.h"

#define DOBRG_MAX_ITEMS     16
#define DOBRG_LABEL_LEN     64
#define DOBRG_DEFAULT_SIZE  14
#define DOBRG_TEXT_GAP      6
#define DOBRG_ITEM_GAP      4
#define DOBRG_DOT_PAD       4     /* Padding for inner dot */
#define DOBRG_FONT_W        8
#define DOBRG_FONT_H        16

#define DOBRG_COL_BOX       DOBUI_SURFACE
#define DOBRG_COL_BOX_BG    DOBUI_INSET
#define DOBRG_COL_DOT       DOBUI_INPUT
#define DOBRG_COL_DOT_DIS   DOBUI_DISABLED
#define DOBRG_COL_TEXT      DOBUI_TEXT
#define DOBRG_COL_BG        DOBUI_SURFACE
#define DOBRG_COL_FOCUS     DOBUI_TEXT

typedef struct
{
    uint32_t    win_id;
    int         x, y;
    int         box_size;
    int         text_gap;
    int         item_gap;

    char        items[DOBRG_MAX_ITEMS][DOBRG_LABEL_LEN];
    int         count;
    int         selected;       /* Currently selected index (-1 = none) */

    bool        visible;
    bool        enabled;
    bool        focused;
    int         focus_idx;      /* Keyboard cursor within group */

    uint32_t    col_box;
    uint32_t    col_box_bg;
    uint32_t    col_dot;
    uint32_t    col_dot_disabled;
    uint32_t    col_text;
    uint32_t    col_bg;
    uint32_t    col_focus;
} dob_radiogroup_t;

void dobrg_Init(dob_radiogroup_t *rg, uint32_t win_id,
                int x, int y, int box_size);

/* Add item. Returns index or -1 if full. */
int  dobrg_AddItem(dob_radiogroup_t *rg, const char *label);

/* Clear all items. */
void dobrg_Clear(dob_radiogroup_t *rg);

bool dobrg_OnClick(dob_radiogroup_t *rg, int x, int y);
bool dobrg_OnKey(dob_radiogroup_t *rg, uint8_t key);
void dobrg_Draw(dob_radiogroup_t *rg);

void dobrg_SetSelected(dob_radiogroup_t *rg, int index);
void dobrg_SetEnabled(dob_radiogroup_t *rg, bool enabled);
void dobrg_SetFocus(dob_radiogroup_t *rg, bool focused);

/* Total height of the group. */
int  dobrg_GetHeight(dob_radiogroup_t *rg);

#endif
