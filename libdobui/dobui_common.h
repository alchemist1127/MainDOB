/* DobUI — Common types shared by all controls */

#ifndef DOBUI_COMMON_H
#define DOBUI_COMMON_H

/*  *  Text anchor — 9-point positioning within a bounding box.
 *  Enum layout: row = anchor/3 (0=top,1=mid,2=bot)
 *               col = anchor%3 (0=left,1=center,2=right)
 */

typedef enum
{
    DOB_ANCHOR_TOP_LEFT = 0,
    DOB_ANCHOR_TOP_CENTER,
    DOB_ANCHOR_TOP_RIGHT,
    DOB_ANCHOR_CENTER_LEFT,
    DOB_ANCHOR_CENTER,
    DOB_ANCHOR_CENTER_RIGHT,
    DOB_ANCHOR_BOTTOM_LEFT,
    DOB_ANCHOR_BOTTOM_CENTER,
    DOB_ANCHOR_BOTTOM_RIGHT
} dob_anchor_t;

/* Compute text position (ox, oy) for content of size (tw x th)
 * inside a box at (bx, by) of size (bw x bh), with padding pad. */
static inline void dob_anchor_pos(dob_anchor_t a,
                                  int bx, int by, int bw, int bh,
                                  int tw, int th, int pad,
                                  int *ox, int *oy)
{
    switch (a % 3)
    {
        default:
        case 0: *ox = bx + pad;                  break;
        case 1: *ox = bx + (bw - tw) / 2;        break;
        case 2: *ox = bx + bw - tw - pad;         break;
    }
    switch (a / 3)
    {
        default:
        case 0: *oy = by + pad;                   break;
        case 1: *oy = by + (bh - th) / 2;         break;
        case 2: *oy = by + bh - th - pad;          break;
    }
}

/*  *  Control type tag — shared so any widget can self-register with
 *  the global focus manager without including focus.h.
 */

typedef enum
{
    DOB_CTRL_NONE = 0,
    DOB_CTRL_BUTTON,
    DOB_CTRL_PICTUREBUTTON,
    DOB_CTRL_TEXTBOX,
    DOB_CTRL_MULTITEXTBOX,
    DOB_CTRL_DROPDOWN,
    DOB_CTRL_CHECKBOX,
    DOB_CTRL_SWITCH,
    DOB_CTRL_SLIDER,
    DOB_CTRL_RADIOGROUP,
    DOB_CTRL_LISTVIEW,
    DOB_CTRL_CHECKED_LISTVIEW,
    DOB_CTRL_TABLE,
} dob_ctrl_type_t;

/* Self-registration hooks into the global focus manager.
 *
 * Declared `weak` so that programs linking a subset of libdobui
 * without focus.o (e.g. a minimal dialog client) still build: the
 * symbol resolves to NULL and the widget _Init calls skip the
 * registration silently. Programs that do link focus.o pick up the
 * real implementation and get automatic focus tracking.
 */
__attribute__((weak)) void dobfocus_auto_register(void *ctrl,
                                                  dob_ctrl_type_t type);
__attribute__((weak)) void dobfocus_auto_unregister(void *ctrl);

#endif /* DOBUI_COMMON_H */
