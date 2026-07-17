/* colorpick.h -- reusable modeless RGB colour picker for DobWrite.
 *
 * A secondary (non-blocking) dialog with three RGB trackbars, each paired
 * with a 0..255 value box, a live preview swatch showing the hex code, a
 * listbox of the standard HTML/CSS colour names, and OK / Annulla.
 *
 * Modeless: colorpick_open() returns immediately and the caller keeps
 * running.  On OK the chosen colour is delivered to on_pick as 0x00RRGGBB;
 * on Annulla / window-close nothing is delivered.  Only one picker exists
 * at a time -- a second colorpick_open() while one is up is ignored.
 *
 * Parent it on dobui_primary() so it stacks above the main window and is
 * destroyed with it.
 */
#ifndef DOBWRITE_COLORPICK_H
#define DOBWRITE_COLORPICK_H

#include <dob/types.h>
#include <app.h>

typedef void (*colorpick_cb)(uint32_t rgb, void *user);

void colorpick_open(dobui_win_t *parent, const char *title,
                    uint32_t initial_rgb, colorpick_cb on_pick, void *user);

#endif /* DOBWRITE_COLORPICK_H */
