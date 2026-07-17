/* DobUITools — FileGiver Implementation */

#include "file_giver.h"
#include <DobInterface.h>
#include <string.h>

/* Fold triangle size: 14 px from the top-right corner */
#define FOLD_SZ     14

static int iabs(int x) { return x < 0 ? -x : x; }

void dobfg_Init(dob_file_giver_t *fg, uint32_t win_id, int x, int y)
{
    memset(fg, 0, sizeof(*fg));
    fg->win_id  = win_id;
    fg->x       = x;
    fg->y       = y;
    fg->visible = true;
    fg->enabled = true;
    fg->is_cut  = true;   /* default intent: move */
}

void dobfg_SetEnabled(dob_file_giver_t *fg, bool enabled)
{
    fg->enabled = enabled;
    if (!enabled)
    {
        fg->pressed  = false;
        fg->dragging = false;
    }
}

void dobfg_SetPaths(dob_file_giver_t *fg,
                    const char *const *paths, int n_paths, bool is_cut)
{
    int n = n_paths;
    if (n < 0) n = 0;
    if (n > DOBFG_MAX_PATHS) n = DOBFG_MAX_PATHS;

    fg->n_paths = 0;
    for (int i = 0; i < n; i++)
    {
        const char *p = paths[i] ? paths[i] : "";
        size_t slen = strlen(p);
        if (slen >= DOBFG_MAX_PATH_LEN) slen = DOBFG_MAX_PATH_LEN - 1;
        memcpy(fg->paths[fg->n_paths], p, slen);
        fg->paths[fg->n_paths][slen] = '\0';
        fg->n_paths++;
    }
    fg->is_cut = is_cut;
}

void dobfg_ClearPaths(dob_file_giver_t *fg)
{
    fg->n_paths = 0;
}

bool dobfg_HitTest(dob_file_giver_t *fg, int px, int py)
{
    if (!fg->visible || !fg->enabled) return false;
    return px >= fg->x && px < fg->x + DOBFG_SIZE
        && py >= fg->y && py < fg->y + DOBFG_SIZE;
}

bool dobfg_OnMouseDown(dob_file_giver_t *fg, int x, int y)
{
    if (!dobfg_HitTest(fg, x, y)) return false;
    if (fg->n_paths == 0)
    {
        /* Nothing to drag — consume the click but do not arm. */
        return true;
    }
    fg->pressed  = true;
    fg->dragging = false;
    fg->press_x  = x;
    fg->press_y  = y;
    return true;
}

bool dobfg_OnMouseMove(dob_file_giver_t *fg, int x, int y)
{
    if (!fg->pressed || fg->dragging) return false;
    if (iabs(x - fg->press_x) < DOBFG_DRAG_THRESH
     && iabs(y - fg->press_y) < DOBFG_DRAG_THRESH)
        return false;

    /* Threshold exceeded — arm the drag session. */
    const char *ptrs[DOBFG_MAX_PATHS];
    for (int i = 0; i < fg->n_paths; i++)
        ptrs[i] = fg->paths[i];

    int rc = dobui_BeginDrag(fg->win_id, ptrs, fg->n_paths, fg->is_cut);
    if (rc == 0)
    {
        fg->dragging = true;
    }
    else
    {
        /* WM refused (another drag in progress) — drop the press. */
        fg->pressed = false;
    }
    return true;
}

bool dobfg_OnMouseUp(dob_file_giver_t *fg, int x, int y)
{
    (void)x; (void)y;
    if (!fg->pressed) return false;
    /* If a drag is in progress the WM owns the release and we'll
     * get event_drag_end. If not, this was just a click-without-move
     * and we simply clear the armed state. */
    if (!fg->dragging)
    {
        fg->pressed = false;
        return true;
    }
    return false;
}

void dobfg_OnDragEnd(dob_file_giver_t *fg)
{
    fg->pressed  = false;
    fg->dragging = false;
}

/* Drawing: 48x48 document with folded top-right corner */

void dobfg_Draw(dob_file_giver_t *fg)
{
    if (!fg->visible) return;

    uint32_t id = fg->win_id;
    int x0 = fg->x, y0 = fg->y;

    /* Body fill */
    dobui_FillRect(id, x0, y0, DOBFG_SIZE, DOBFG_SIZE, DOBFG_COL_PAPER);

    /* Fold triangle in the top-right corner.
     * Vertices: (start_x, 0), (47, 0), (47, FOLD_SZ-1).
     * For each row y in [0..FOLD_SZ-1], fill from (x0 + start_x + y)
     * to (x0 + 47), a strip of width (FOLD_SZ - y). */
    int fold_start = DOBFG_SIZE - FOLD_SZ;   /* 34 */
    for (int r = 0; r < FOLD_SZ; r++)
    {
        int sx = x0 + fold_start + r;
        int sw = FOLD_SZ - r;
        dobui_FillRect(id, sx, y0 + r, sw, 1, DOBFG_COL_FOLD);
    }

    /* Crease diagonal: from (fold_start, 0) down-right to
     * (DOBFG_SIZE-1, FOLD_SZ-1). Drawn one pixel per row. */
    for (int r = 0; r < FOLD_SZ; r++)
    {
        int px = x0 + fold_start + r;
        int py = y0 + r;
        dobui_DrawPixel(id, px, py, DOBFG_COL_INK);
    }

    /* Horizontal text lines in the body, skipping the fold area.
     * Lines start at y = 18 and repeat every 6 px. */
    int line_x = x0 + 6;
    int line_w = DOBFG_SIZE - 12;   /* 36 */
    for (int ly = 18; ly < DOBFG_SIZE - 4; ly += 6)
    {
        dobui_FillRect(id, line_x, y0 + ly, line_w, 1, DOBFG_COL_LINE);
    }

    /* Outer border: 4 thin rects. */
    /* Top: only up to the fold start */
    dobui_FillRect(id, x0, y0, fold_start + 1, 1, DOBFG_COL_INK);
    /* Bottom */
    dobui_FillRect(id, x0, y0 + DOBFG_SIZE - 1, DOBFG_SIZE, 1, DOBFG_COL_INK);
    /* Left */
    dobui_FillRect(id, x0, y0, 1, DOBFG_SIZE, DOBFG_COL_INK);
    /* Right: only from the fold bottom down */
    dobui_FillRect(id, x0 + DOBFG_SIZE - 1, y0 + FOLD_SZ - 1,
                   1, DOBFG_SIZE - FOLD_SZ + 1, DOBFG_COL_INK);
    /* Short diagonal edges of the fold:
     *   top edge of fold already implicit in the fill;
     *   right edge of the fold is the rightmost column above the
     *   fold's bottom — already the right border starts at FOLD_SZ-1. */

    /* Disabled overlay — soften the whole icon */
    if (!fg->enabled)
    {
        /* Cheap dim: a semi-opaque fill is not supported; instead we
         * over-fill with a mid-grey hatch (every other row). */
        for (int r = 0; r < DOBFG_SIZE; r += 2)
            dobui_FillRect(id, x0, y0 + r, DOBFG_SIZE, 1, DOBUI_DISABLED);
    }
}
