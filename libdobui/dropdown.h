/* DobUITools — Dropdown Control
 *
 * Deferred popup design: draw button inline, flush popup last.
 *
 * Usage:
 *   static const char *items[] = { "A", "B", "C" };
 *   dob_dropdown_t dd;
 *   dobdd_Init(&dd, win_id, 10, 50, 160, 0, items, 3);
 *   dd.col_clear = MY_WINDOW_BG;   // required for ghost cleanup
 *
 *   // Draw cycle:
 *   dobdd_ClearGhost(&dd);          // FIRST — clears old popup area
 *   // ... draw other controls ...
 *   dobdd_Draw(&dd);                // draws button
 *   // ... draw more controls ...
 *   dobdd_FlushPopup(&dd);          // LAST — draws popup on top
 *   dobui_Invalidate(win_id);
 */

#ifndef MAINDOB_DOBUITOOLS_DROPDOWN_H
#define MAINDOB_DOBUITOOLS_DROPDOWN_H

#include <dob/types.h>
#include "dobui_common.h"
#include "dobui_theme.h"

#define DOBDD_FONT_W        8
#define DOBDD_FONT_H        16
#define DOBDD_ITEM_H        20
#define DOBDD_DEFAULT_H     22
#define DOBDD_PAD           4
#define DOBDD_MAX_VISIBLE   8
#define DOBDD_ARROW_W       16

#define DOBDD_COL_BTN_BG    DOBUI_INSET
#define DOBDD_COL_BTN_OPEN  DOBUI_SURFACE
#define DOBDD_COL_POPUP_BG  DOBUI_INSET
#define DOBDD_COL_HOVER_BG  DOBUI_SURFACE
#define DOBDD_COL_TEXT      DOBUI_TEXT_ALT
#define DOBDD_COL_DIM       DOBUI_DISABLED
#define DOBDD_COL_ACCENT    DOBUI_TEXT
#define DOBDD_COL_BORDER    DOBUI_SURFACE

#define DOBDD_KEY_UP        128
#define DOBDD_KEY_DOWN      129

typedef struct
{
    uint32_t    win_id;
    int         x, y;
    int         w, h;           /* h = button height (0 in Init = default) */

    const char **items;
    int         count;

    int         selected;
    bool        open;
    int         cursor;
    int         scroll;

    bool        visible;
    bool        enabled;
    bool        focused;

    /* Scrollbar thumb drag: armed on press over the thumb, tracked
     * through OnDrag, cleared on OnRelease. sb_grab is the cursor's
     * offset within the thumb at grab time so the grab point stays
     * pinned under the cursor. */
    bool        sb_drag;
    int         sb_grab;

    /* Ghost cleanup: set col_clear to your window background color */
    bool        _was_open;
    uint32_t    col_clear;

    dob_anchor_t anchor;        /* Text anchor within button */

    uint32_t    col_btn_bg;
    uint32_t    col_btn_open;
    uint32_t    col_popup_bg;
    uint32_t    col_hover_bg;
    uint32_t    col_text;
    uint32_t    col_dim;
    uint32_t    col_accent;
    uint32_t    col_border;
} dob_dropdown_t;

void dobdd_Init(dob_dropdown_t *dd, uint32_t win_id,
                int x, int y, int w, int h,
                const char **items, int count);

bool dobdd_OnClick(dob_dropdown_t *dd, int x, int y);
bool dobdd_OnKey(dob_dropdown_t *dd, uint8_t key);
bool dobdd_OnScroll(dob_dropdown_t *dd, int delta);

/* Scrollbar thumb dragging. OnDrag continues an in-flight grab (armed
 * by OnClick over the thumb) and returns true while it owns the drag;
 * OnRelease ends it. Harmless no-ops when no grab is active, so hosts
 * can call them unconditionally on every mouse move / button release. */
bool dobdd_OnDrag(dob_dropdown_t *dd, int x, int y);
void dobdd_OnRelease(dob_dropdown_t *dd);

/* Call at the START of your draw cycle to clear ghost popup area. */
void dobdd_ClearGhost(dob_dropdown_t *dd);

/* Draw the button (normal draw order). */
void dobdd_Draw(dob_dropdown_t *dd);

/* Draw popup ON TOP. Call LAST before Invalidate. */
void dobdd_FlushPopup(dob_dropdown_t *dd);

void dobdd_Open(dob_dropdown_t *dd);
void dobdd_Close(dob_dropdown_t *dd);
void dobdd_SetFocus(dob_dropdown_t *dd, bool focused);
void dobdd_SetSelected(dob_dropdown_t *dd, int index);
const char *dobdd_GetSelectedText(dob_dropdown_t *dd);
bool dobdd_HitTest(dob_dropdown_t *dd, int px, int py);
bool dobdd_PopupHitTest(dob_dropdown_t *dd, int px, int py);

#endif
