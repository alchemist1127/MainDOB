/* DobUITools — ListView Control
 *
 * Scrollable list of selectable items.
 *
 * Usage:
 *   static const char *items[] = { "File 1", "File 2", "File 3" };
 *   dob_listview_t lv;
 *   doblv_Init(&lv, win_id, 10, 40, 200, 300);
 *   doblv_SetItems(&lv, items, 3);
 *   doblv_Draw(&lv);
 */

#ifndef MAINDOB_DOBUITOOLS_LISTVIEW_H
#define MAINDOB_DOBUITOOLS_LISTVIEW_H

#include <dob/types.h>
#include "dobui_common.h"
#include "dobui_theme.h"

#define DOBLV_ITEM_H        20
#define DOBLV_PAD           6
#define DOBLV_FONT_W        8
#define DOBLV_FONT_H        16
#define DOBLV_SCROLLBAR_W   10

#define DOBLV_COL_BG        DOBUI_INSET
#define DOBLV_COL_ITEM_BG   DOBUI_INSET
#define DOBLV_COL_SEL_BG    DOBUI_RELIEF
#define DOBLV_COL_TEXT      DOBUI_TEXT_ALT
#define DOBLV_COL_SEL_TEXT  DOBUI_INPUT
#define DOBLV_COL_BORDER    DOBUI_SURFACE
#define DOBLV_COL_BORDER_F  DOBUI_TEXT
#define DOBLV_COL_SCROLL    DOBUI_DISABLED

typedef struct
{
    uint32_t    win_id;
    int         x, y;
    int         w, h;
    int         item_h;

    const char **items;
    int         count;
    int         selected;       /* -1 = none */
    int         scroll;         /* First visible item index */

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
} dob_listview_t;

void doblv_Init(dob_listview_t *lv, uint32_t win_id,
                int x, int y, int w, int h);

/* Set items (caller owns the strings). */
void doblv_SetItems(dob_listview_t *lv, const char **items, int count);

/* Returns true if consumed. Sets selected on click. */
bool doblv_OnClick(dob_listview_t *lv, int x, int y);

/* Arrows navigate, Enter confirms. Returns true if consumed. */
bool doblv_OnKey(dob_listview_t *lv, uint8_t key);

/* Mouse wheel. Returns true if consumed. */
bool doblv_OnScroll(dob_listview_t *lv, int delta);

/* Scrollbar thumb dragging. OnDrag continues an in-flight grab (armed
 * by OnClick over the thumb); OnRelease ends it. Safe no-ops when idle. */
bool doblv_OnDrag(dob_listview_t *lv, int x, int y);
void doblv_OnRelease(dob_listview_t *lv);

void doblv_Draw(dob_listview_t *lv);

void doblv_SetSelected(dob_listview_t *lv, int index);
void doblv_SetEnabled(dob_listview_t *lv, bool enabled);
void doblv_SetFocus(dob_listview_t *lv, bool focused);

/* Get selected index (-1 if none). */
int  doblv_GetSelectedIndex(dob_listview_t *lv);

/* Get selected item text, or NULL if none. */
const char *doblv_GetSelectedText(dob_listview_t *lv);

/* Ensure selected item is visible. */
void doblv_EnsureVisible(dob_listview_t *lv);

/* Visible item count. */
int  doblv_VisibleCount(dob_listview_t *lv);

#endif
