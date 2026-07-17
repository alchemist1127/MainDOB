/* DobUITools — FileLander Control
 *
 * Passive drop zone widget. Mirrors the dobbtn_* / dobpb_* idiom:
 * state + query functions, no callbacks, no observers.
 *
 * Usage:
 *   dob_file_lander_t fl;
 *   dobfl_Init(&fl, win_id, 20, 40, 200, 80);
 *   dobfl_Draw(&fl);
 *
 *   // inside event_drop:
 *   if (dobfl_OnDrop(&fl, lx, ly, paths, n, is_copy))
 *   {
 *       // fl->drop_ready is now true; consume:
 *       for (int i = 0; i < fl.drop_n; i++) process(fl.drop_paths[i]);
 *       fl.drop_ready = false;
 *   }
 *
 * The paths copied into the widget are valid until the next
 * dobfl_OnDrop call on this instance or until the app clears them.
 */

#ifndef MAINDOB_DOBUITOOLS_FILE_LANDER_H
#define MAINDOB_DOBUITOOLS_FILE_LANDER_H

#include <dob/types.h>
#include "dobui_common.h"
#include "dobui_theme.h"

#define DOBFL_MAX_PATHS     64    /* per widget drop */
#define DOBFL_MAX_PATH_LEN  256

#define DOBFL_COL_BG        DOBUI_SURFACE  /* pale blue */
#define DOBFL_COL_BORDER    DOBUI_RELIEF
#define DOBFL_COL_TEXT      DOBUI_TEXT_ALT
#define DOBFL_COL_BG_DIS    DOBUI_DISABLED
#define DOBFL_COL_BORDER_DIS DOBUI_DISABLED

typedef struct
{
    uint32_t    win_id;
    int         x, y;
    int         w, h;

    bool        visible;
    bool        enabled;

    /* A drop just landed; app should consume drop_paths and clear. */
    bool        drop_ready;
    /* Drop payload, copied from the dispatch into widget-owned storage
     * so it survives past the event_drop handler. */
    char        drop_paths[DOBFL_MAX_PATHS][DOBFL_MAX_PATH_LEN];
    int         drop_n;
    bool        drop_is_copy;

    /* Optional label drawn centred inside the zone. */
    char        label[64];

    uint32_t    col_bg;
    uint32_t    col_border;
    uint32_t    col_text;
} dob_file_lander_t;

void dobfl_Init(dob_file_lander_t *fl, uint32_t win_id,
                int x, int y, int w, int h);

void dobfl_SetLabel(dob_file_lander_t *fl, const char *label);
void dobfl_SetEnabled(dob_file_lander_t *fl, bool enabled);

bool dobfl_HitTest(dob_file_lander_t *fl, int px, int py);

/* Called by the app from its event_drop handler. If (lx, ly) falls
 * within the widget, copies the payload into the widget's storage,
 * sets drop_ready, and returns true. The caller is then expected
 * to consume the drop and clear drop_ready. */
bool dobfl_OnDrop(dob_file_lander_t *fl, int lx, int ly,
                  const char *const *paths, int n_paths, bool is_copy);

void dobfl_Draw(dob_file_lander_t *fl);

#endif
