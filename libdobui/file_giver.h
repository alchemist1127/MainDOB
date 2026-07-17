/* DobUITools — FileGiver Control
 *
 * Active drag source widget. Shows a 48x48 "document with folded
 * corner" icon. When the user presses on it and drags past a small
 * threshold, internally calls dobui_BeginDrag with the path list
 * that the app has populated via dobfg_SetPaths.
 *
 * Usage:
 *   dob_file_giver_t fg;
 *   dobfg_Init(&fg, win_id, 20, 40);
 *   dobfg_SetPaths(&fg, my_paths, n, true);  // true = cut semantics
 *   dobfg_Draw(&fg);
 *
 *   // inside event_mouseclick (left press):
 *   dobfg_OnMouseDown(&fg, x, y);
 *   // inside event_mousemove:
 *   dobfg_OnMouseMove(&fg, x, y);
 *   // inside event_mouserelease:
 *   dobfg_OnMouseUp(&fg, x, y);
 *   // inside event_drag_end:
 *   dobfg_OnDragEnd(&fg);
 *
 * The app is responsible for keeping the paths list up to date
 * (typically on selection change). The widget never calls back
 * into the app — the only query is SetPaths going in.
 */

#ifndef MAINDOB_DOBUITOOLS_FILE_GIVER_H
#define MAINDOB_DOBUITOOLS_FILE_GIVER_H

#include <dob/types.h>
#include "dobui_common.h"
#include "dobui_theme.h"

#define DOBFG_SIZE          48   /* icon is 48x48 */
#define DOBFG_MAX_PATHS     64
#define DOBFG_MAX_PATH_LEN  256
#define DOBFG_DRAG_THRESH   4    /* pixels moved before drag starts */

#define DOBFG_COL_PAPER     DOBUI_TEXT_ALT
#define DOBFG_COL_INK       DOBUI_INSET
#define DOBFG_COL_FOLD      DOBUI_DISABLED
#define DOBFG_COL_LINE      DOBUI_DISABLED

typedef struct
{
    uint32_t    win_id;
    int         x, y;            /* top-left of 48x48 icon */

    bool        visible;
    bool        enabled;

    /* Internal drag state machine */
    bool        pressed;         /* left button down on icon */
    int         press_x, press_y;
    bool        dragging;        /* BeginDrag has been fired */

    /* Payload to hand to the WM when drag begins */
    char        paths[DOBFG_MAX_PATHS][DOBFG_MAX_PATH_LEN];
    int         n_paths;
    bool        is_cut;
} dob_file_giver_t;

void dobfg_Init(dob_file_giver_t *fg, uint32_t win_id, int x, int y);
void dobfg_SetEnabled(dob_file_giver_t *fg, bool enabled);

/* Replace the drag payload. Call whenever the app's selection changes
 * so the giver is always ready to spawn a drag on demand. */
void dobfg_SetPaths(dob_file_giver_t *fg,
                    const char *const *paths, int n_paths, bool is_cut);

void dobfg_ClearPaths(dob_file_giver_t *fg);

bool dobfg_HitTest(dob_file_giver_t *fg, int px, int py);

/* Mouse hooks — call from the corresponding event_* handlers.
 * Return true if the event was consumed by the widget. */
bool dobfg_OnMouseDown(dob_file_giver_t *fg, int x, int y);
bool dobfg_OnMouseMove(dob_file_giver_t *fg, int x, int y);
bool dobfg_OnMouseUp  (dob_file_giver_t *fg, int x, int y);

/* Call from event_drag_end to reset internal state. */
void dobfg_OnDragEnd  (dob_file_giver_t *fg);

void dobfg_Draw(dob_file_giver_t *fg);

#endif
