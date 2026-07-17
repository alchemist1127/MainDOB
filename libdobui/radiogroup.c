/* DobUITools — RadioGroup Implementation */

#include "radiogroup.h"
#include <DobInterface.h>
#include <string.h>

/* Height of a single item row */
static int rg_row_h(dob_radiogroup_t *rg)
{
    return (rg->box_size > DOBRG_FONT_H) ? rg->box_size : DOBRG_FONT_H;
}

void dobrg_Init(dob_radiogroup_t *rg, uint32_t win_id,
                int x, int y, int box_size)
{
    memset(rg, 0, sizeof(*rg));
    rg->win_id          = win_id;
    rg->x               = x;
    rg->y               = y;
    rg->box_size        = (box_size > 0) ? box_size : DOBRG_DEFAULT_SIZE;
    rg->text_gap        = DOBRG_TEXT_GAP;
    rg->item_gap        = DOBRG_ITEM_GAP;
    rg->selected        = -1;
    rg->visible         = true;
    rg->enabled         = true;
    rg->col_box         = DOBRG_COL_BOX;
    rg->col_box_bg      = DOBRG_COL_BOX_BG;
    rg->col_dot         = DOBRG_COL_DOT;
    rg->col_dot_disabled = DOBRG_COL_DOT_DIS;
    rg->col_text        = DOBRG_COL_TEXT;
    rg->col_bg          = DOBRG_COL_BG;
    rg->col_focus       = DOBRG_COL_FOCUS;
    if (dobfocus_auto_register) dobfocus_auto_register(rg, DOB_CTRL_RADIOGROUP);
}

int dobrg_AddItem(dob_radiogroup_t *rg, const char *label)
{
    if (rg->count >= DOBRG_MAX_ITEMS) return -1;
    int idx = rg->count;
    if (label)
    {
        int n = (int)strlen(label);
        if (n > DOBRG_LABEL_LEN - 1) n = DOBRG_LABEL_LEN - 1;
        memcpy(rg->items[idx], label, (uint32_t)n);
        rg->items[idx][n] = '\0';
    }
    rg->count++;
    return idx;
}

void dobrg_Clear(dob_radiogroup_t *rg)
{
    rg->count = 0;
    rg->selected = -1;
    rg->focus_idx = 0;
}

void dobrg_SetSelected(dob_radiogroup_t *rg, int index)
{
    if (index >= -1 && index < rg->count)
        rg->selected = index;
}

void dobrg_SetEnabled(dob_radiogroup_t *rg, bool enabled) { rg->enabled = enabled; }
void dobrg_SetFocus(dob_radiogroup_t *rg, bool focused)   { rg->focused = focused; }

int dobrg_GetHeight(dob_radiogroup_t *rg)
{
    if (rg->count <= 0) return 0;
    int rh = rg_row_h(rg);
    return rg->count * rh + (rg->count - 1) * rg->item_gap;
}

bool dobrg_OnClick(dob_radiogroup_t *rg, int x, int y)
{
    if (!rg->visible || !rg->enabled || rg->count == 0) return false;

    int rh = rg_row_h(rg);
    int stride = rh + rg->item_gap;

    if (x < rg->x || y < rg->y) return false;

    int rel_y = y - rg->y;
    int idx = rel_y / stride;
    if (idx < 0 || idx >= rg->count) return false;

    /* Check the click is within the row, not in the gap */
    if (rel_y - idx * stride >= rh) return false;

    rg->selected = idx;
    rg->focus_idx = idx;
    return true;
}

bool dobrg_OnKey(dob_radiogroup_t *rg, uint8_t key)
{
    if (!rg->visible || !rg->enabled || !rg->focused || rg->count == 0)
        return false;

    if (key == 129) /* DOWN */
    {
        if (rg->focus_idx < rg->count - 1) rg->focus_idx++;
        rg->selected = rg->focus_idx;
        return true;
    }
    if (key == 128) /* UP */
    {
        if (rg->focus_idx > 0) rg->focus_idx--;
        rg->selected = rg->focus_idx;
        return true;
    }
    if (key == ' ' || key == '\n')
    {
        rg->selected = rg->focus_idx;
        return true;
    }
    return false;
}

void dobrg_Draw(dob_radiogroup_t *rg)
{
    if (!rg->visible) return;

    int bs = rg->box_size;
    int rh = rg_row_h(rg);
    int stride = rh + rg->item_gap;

    for (int i = 0; i < rg->count; i++)
    {
        int iy = rg->y + i * stride;
        int box_y = iy + (rh - bs) / 2;
        bool is_sel = (i == rg->selected);
        bool is_focus = (rg->focused && i == rg->focus_idx);

        /* Box border */
        uint32_t bc = is_focus ? rg->col_focus : rg->col_box;
        if (!rg->enabled) bc = rg->col_dot_disabled;

        dobui_FillRect(rg->win_id, rg->x, box_y, bs, bs, rg->col_box_bg);
        dobui_DrawRect(rg->win_id, rg->x, box_y, bs, bs, bc);

        /* Dot (smaller centered square) */
        if (is_sel)
        {
            int p = DOBRG_DOT_PAD;
            int ds = bs - 2 * p;
            if (ds > 0)
            {
                uint32_t dc = rg->enabled ? rg->col_dot : rg->col_dot_disabled;
                dobui_FillRect(rg->win_id, rg->x + p, box_y + p, ds, ds, dc);
            }
        }

        /* Label */
        if (rg->items[i][0] != '\0')
        {
            int tx = rg->x + bs + rg->text_gap;
            int ty = iy + (rh - DOBRG_FONT_H) / 2;
            uint32_t fg = rg->enabled ? rg->col_text : rg->col_dot_disabled;
            dobui_DrawText(rg->win_id, tx, ty, rg->items[i], fg, rg->col_bg);
        }
    }
}
