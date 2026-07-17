/* DobUITools — Label Implementation */

#include "label.h"
#include <DobInterface.h>
#include <dob/font.h>
#include <string.h>

void doblbl_Init(dob_label_t *lbl, uint32_t win_id,
                 int x, int y, const char *text)
{
    memset(lbl, 0, sizeof(*lbl));
    lbl->win_id  = win_id;
    lbl->x       = x;
    lbl->y       = y;
    lbl->col_text = DOBLBL_COL_TEXT;
    lbl->col_bg   = DOBLBL_COL_BG;
    lbl->has_bg   = false;
    lbl->pad      = DOBLBL_PAD;
    lbl->anchor   = DOB_ANCHOR_CENTER_LEFT;
    lbl->visible  = true;
    lbl->enabled  = true;

    if (text)
    {
        int n = (int)strlen(text);
        if (n > 255) n = 255;
        memcpy(lbl->text, text, (uint32_t)n);
        lbl->text[n] = '\0';
    }
}

void doblbl_InitWithBg(dob_label_t *lbl, uint32_t win_id,
                       int x, int y, const char *text,
                       uint32_t text_color, uint32_t bg_color)
{
    doblbl_Init(lbl, win_id, x, y, text);
    lbl->col_text = text_color;
    lbl->col_bg   = bg_color;
    lbl->has_bg   = true;
}

void doblbl_SetText(dob_label_t *lbl, const char *text)
{
    if (!text) text = "";
    int n = (int)strlen(text);
    if (n > 255) n = 255;
    memcpy(lbl->text, text, (uint32_t)n);
    lbl->text[n] = '\0';
}

int doblbl_GetWidth(dob_label_t *lbl)
{
    if (lbl->w > 0) return lbl->w;
    int tw = dob_text_width(lbl->text, (uint32_t)strlen(lbl->text));
    return lbl->has_bg ? tw + 2 * lbl->pad : tw;
}

int doblbl_GetHeight(dob_label_t *lbl)
{
    if (lbl->h > 0) return lbl->h;
    return lbl->has_bg ? DOBLBL_FONT_H + 2 * lbl->pad : DOBLBL_FONT_H;
}

void doblbl_Draw(dob_label_t *lbl)
{
    if (!lbl->visible) return;

    int rw = doblbl_GetWidth(lbl);
    int rh = doblbl_GetHeight(lbl);
    int tw = dob_text_width(lbl->text, (uint32_t)strlen(lbl->text));
    int tx, ty;

    /* Clear previous extent to avoid garbage when text shrinks */
    int clear_w = (lbl->prev_draw_w > rw) ? lbl->prev_draw_w : rw;

    if (lbl->has_bg)
    {
        dobui_FillRect(lbl->win_id, lbl->x, lbl->y, clear_w, rh, lbl->col_bg);
        dob_anchor_pos(lbl->anchor, lbl->x, lbl->y, rw, rh,
                       tw, DOBLBL_FONT_H, lbl->pad, &tx, &ty);
        dobui_DrawText(lbl->win_id, tx, ty, lbl->text,
                       lbl->col_text, lbl->col_bg);
    }
    else
    {
        dobui_FillRect(lbl->win_id, lbl->x, lbl->y, clear_w, rh, lbl->col_bg);
        dob_anchor_pos(lbl->anchor, lbl->x, lbl->y, rw, rh,
                       tw, DOBLBL_FONT_H, 0, &tx, &ty);
        dobui_DrawText(lbl->win_id, tx, ty, lbl->text,
                       lbl->col_text, lbl->col_bg);
    }

    lbl->prev_draw_w = rw;
}
