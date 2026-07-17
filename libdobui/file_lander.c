/* DobUITools — FileLander Implementation */

#include "file_lander.h"
#include <DobInterface.h>
#include <string.h>

#define DOBFL_FONT_W  8
#define DOBFL_FONT_H  16

void dobfl_Init(dob_file_lander_t *fl, uint32_t win_id,
                int x, int y, int w, int h)
{
    memset(fl, 0, sizeof(*fl));
    fl->win_id     = win_id;
    fl->x          = x;
    fl->y          = y;
    fl->w          = w;
    fl->h          = h;
    fl->visible    = true;
    fl->enabled    = true;
    fl->col_bg     = DOBFL_COL_BG;
    fl->col_border = DOBFL_COL_BORDER;
    fl->col_text   = DOBFL_COL_TEXT;
}

void dobfl_SetLabel(dob_file_lander_t *fl, const char *label)
{
    if (!label) label = "";
    size_t n = strlen(label);
    if (n >= sizeof(fl->label)) n = sizeof(fl->label) - 1;
    memcpy(fl->label, label, n);
    fl->label[n] = '\0';
}

void dobfl_SetEnabled(dob_file_lander_t *fl, bool enabled)
{
    fl->enabled = enabled;
}

bool dobfl_HitTest(dob_file_lander_t *fl, int px, int py)
{
    if (!fl->visible || !fl->enabled) return false;
    return px >= fl->x && px < fl->x + fl->w
        && py >= fl->y && py < fl->y + fl->h;
}

bool dobfl_OnDrop(dob_file_lander_t *fl, int lx, int ly,
                  const char *const *paths, int n_paths, bool is_copy)
{
    if (!dobfl_HitTest(fl, lx, ly)) return false;
    if (!paths || n_paths <= 0) return false;

    int n = n_paths;
    if (n > DOBFL_MAX_PATHS) n = DOBFL_MAX_PATHS;

    fl->drop_n = 0;
    for (int i = 0; i < n; i++)
    {
        const char *p = paths[i] ? paths[i] : "";
        size_t slen = strlen(p);
        if (slen >= DOBFL_MAX_PATH_LEN) slen = DOBFL_MAX_PATH_LEN - 1;
        memcpy(fl->drop_paths[fl->drop_n], p, slen);
        fl->drop_paths[fl->drop_n][slen] = '\0';
        fl->drop_n++;
    }
    fl->drop_is_copy = is_copy;
    fl->drop_ready   = true;
    return true;
}

void dobfl_Draw(dob_file_lander_t *fl)
{
    if (!fl->visible) return;

    uint32_t bg     = fl->enabled ? fl->col_bg     : DOBFL_COL_BG_DIS;
    uint32_t border = fl->enabled ? fl->col_border : DOBFL_COL_BORDER_DIS;

    /* Fill background */
    dobui_FillRect(fl->win_id, fl->x, fl->y, fl->w, fl->h, bg);

    /* 1px border (4 rects) */
    dobui_FillRect(fl->win_id, fl->x, fl->y, fl->w, 1, border);
    dobui_FillRect(fl->win_id, fl->x, fl->y + fl->h - 1, fl->w, 1, border);
    dobui_FillRect(fl->win_id, fl->x, fl->y, 1, fl->h, border);
    dobui_FillRect(fl->win_id, fl->x + fl->w - 1, fl->y, 1, fl->h, border);

    /* Dashed inner border to suggest "drop zone" — 4px dashes with 4px gaps */
    int inset = 4;
    int ix = fl->x + inset, iy = fl->y + inset;
    int iw = fl->w - 2 * inset, ih = fl->h - 2 * inset;
    if (iw > 0 && ih > 0)
    {
        for (int k = 0; k < iw; k += 8)
        {
            int seg = (k + 4 <= iw) ? 4 : (iw - k);
            dobui_FillRect(fl->win_id, ix + k, iy,             seg, 1, border);
            dobui_FillRect(fl->win_id, ix + k, iy + ih - 1,    seg, 1, border);
        }
        for (int k = 0; k < ih; k += 8)
        {
            int seg = (k + 4 <= ih) ? 4 : (ih - k);
            dobui_FillRect(fl->win_id, ix,             iy + k, 1, seg, border);
            dobui_FillRect(fl->win_id, ix + iw - 1,    iy + k, 1, seg, border);
        }
    }

    /* Centered label */
    if (fl->label[0])
    {
        int tw = (int)strlen(fl->label) * DOBFL_FONT_W;
        int tx = fl->x + (fl->w - tw) / 2;
        int ty = fl->y + (fl->h - DOBFL_FONT_H) / 2;
        dobui_DrawText(fl->win_id, tx, ty, fl->label, fl->col_text, bg);
    }
}
