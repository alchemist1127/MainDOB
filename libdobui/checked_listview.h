/* DobUITools — Checked ListView
 *
 * Scrollable list where each row has its own checkbox. Combines the
 * single-widget ergonomics of a ListView (one control, native
 * scrolling, unlimited rows) with the per-row toggle semantics that
 * would otherwise need one dob_checkbox_t per entry — a setup which
 * stops scaling as soon as the list outgrows the screen or the
 * focus singleton's slot budget.
 *
 * Per-row state lives in a caller-owned bool[] array passed
 * alongside the labels to dobclv_SetItems. The widget toggles
 * entries in place; the caller reads them back when needed.
 *
 * Typical use: install-time program/feature picker, settings whose
 * options outnumber what fits in a flat checkbox column, any UX
 * where the user reviews a list and ticks what to keep.
 *
 * Auto-registers with the focus singleton (same pattern as
 * dob_listview_t and friends), so a single SetFocus call routes
 * keyboard input here. Space / Enter toggles the cursor row;
 * arrows / Home / End / PgUp / PgDn navigate. */

#ifndef MAINDOB_DOBUITOOLS_CHECKED_LISTVIEW_H
#define MAINDOB_DOBUITOOLS_CHECKED_LISTVIEW_H

#include <dob/types.h>
#include "dobui_common.h"
#include "dobui_theme.h"

#define DOBCLV_ITEM_H            20
#define DOBCLV_PAD                6
#define DOBCLV_FONT_W             8
#define DOBCLV_FONT_H            16
#define DOBCLV_SCROLLBAR_W       10
#define DOBCLV_CHECK_SIZE        14
#define DOBCLV_CHECK_GAP          6

#define DOBCLV_COL_BG            DOBUI_INSET
#define DOBCLV_COL_ITEM_BG       DOBUI_INSET
#define DOBCLV_COL_SEL_BG        DOBUI_RELIEF
#define DOBCLV_COL_TEXT          DOBUI_TEXT_ALT
#define DOBCLV_COL_SEL_TEXT      DOBUI_INPUT
#define DOBCLV_COL_BORDER        DOBUI_SURFACE
#define DOBCLV_COL_BORDER_F      DOBUI_TEXT
#define DOBCLV_COL_SCROLL        DOBUI_DISABLED
#define DOBCLV_COL_CHECK_BOX     DOBUI_INSET
#define DOBCLV_COL_CHECK_MARK    DOBUI_INPUT
#define DOBCLV_COL_CHECK_BORDER  DOBUI_SURFACE

typedef struct
{
    uint32_t    win_id;
    int         x, y;
    int         w, h;
    int         item_h;

    const char **items;
    bool       *checked;        /* Caller-owned, parallel to items[]. */
    int         count;

    int         selected;       /* Keyboard cursor (-1 = none) */
    int         scroll;

    bool        visible;
    bool        enabled;
    bool        focused;

    /* Scrollbar thumb drag (armed on press over the thumb). */
    bool        sb_drag;
    int         sb_grab;

    uint32_t    col_bg;
    uint32_t    col_item_bg;
    uint32_t    col_sel_bg;
    uint32_t    col_text;
    uint32_t    col_sel_text;
    uint32_t    col_border;
    uint32_t    col_border_focus;
    uint32_t    col_scrollbar;
    uint32_t    col_check_box;
    uint32_t    col_check_mark;
    uint32_t    col_check_border;
} dob_checked_listview_t;

void dobclv_Init(dob_checked_listview_t *lv, uint32_t win_id,
                 int x, int y, int w, int h);

/* Caller owns both arrays; widget keeps the pointers. selected and
 * scroll persist across calls (clamped if count shrinks) — unlike
 * doblv_SetItems which wipes them. */
void dobclv_SetItems(dob_checked_listview_t *lv,
                     const char **items, bool *checked, int count);

/* Click on a row toggles its check + sets it as the cursor row.
 * Returns true if consumed. */
bool dobclv_OnClick(dob_checked_listview_t *lv, int x, int y);

/* Arrows / PgUp / PgDn / Home / End navigate; Space / Enter toggles
 * the cursor row. Returns true if consumed. */
bool dobclv_OnKey(dob_checked_listview_t *lv, uint8_t key);

bool dobclv_OnScroll(dob_checked_listview_t *lv, int delta);

/* Scrollbar thumb dragging. OnDrag continues an in-flight grab (armed
 * by OnClick over the thumb); OnRelease ends it. Safe no-ops when idle. */
bool dobclv_OnDrag(dob_checked_listview_t *lv, int x, int y);
void dobclv_OnRelease(dob_checked_listview_t *lv);

void dobclv_Draw(dob_checked_listview_t *lv);

void dobclv_SetFocus(dob_checked_listview_t *lv, bool focused);
void dobclv_SetEnabled(dob_checked_listview_t *lv, bool enabled);

int  dobclv_GetSelectedIndex(const dob_checked_listview_t *lv);
int  dobclv_VisibleCount(const dob_checked_listview_t *lv);

#endif /* MAINDOB_DOBUITOOLS_CHECKED_LISTVIEW_H */
