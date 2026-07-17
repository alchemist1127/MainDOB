/* dobUI Application Framework
 *
 * Pure event-driven model. The app defines handler functions,
 * the framework calls them when events arrive. No loops, no polling.
 *
 * Usage:
 *
 *   void event_key(uint8_t key)
 *   {
 *       // handle keypress
 *   }
 *
 *   void event_mouseclick(int x, int y, uint8_t buttons)
 *   {
 *       // handle click
 *   }
 *
 *   int main(void)
 *   {
 *       dobui_run("My App", 400, 300);
 *   }
 *
 * Available event handlers (define the ones you need, ignore the rest):
 *
 *   void event_start(void)          -- called once after window creation
 *   void event_key(uint8_t key)
 *   void event_mouseclick(int x, int y, uint8_t buttons)  -- left click
 *   void event_rightclick(int x, int y, uint8_t buttons)  -- right click
 *   void event_middleclick(int x, int y, uint8_t buttons) -- middle click
 *   void event_dblclick(int x, int y, uint8_t buttons)
 *   void event_mouserelease(int x, int y, uint8_t buttons)
 *   void event_mousemove(int x, int y, uint8_t buttons) -- drag only
 *   void event_scroll(int delta)
 *   void event_resize(int w, int h)
 *   void event_panel(int cmd_idx)
 *   void event_close(void)
 *   void event_tick(void)          -- periodic, for games only
 *
 * Utility functions:
 *
 *   dobui_run(title, w, h)         -- create window and enter event loop
 *   dobui_set_tick(ms)             -- call before dobui_run for periodic tick
 *   dobui_set_panel(commands)      -- set panel commands (newline-separated)
 *   dobui_window()                 -- returns window id (for drawing)
 *   dobui_width() / dobui_height() -- current window dimensions
 *   dobui_quit()                   -- exit cleanly from any handler
 */

#ifndef DOBUI_APP_H
#define DOBUI_APP_H

#include <dob/types.h>
#include <dob/ipc.h>
#include <DobInterface.h>

/* Special key codes (from input daemon, values 128+) */
#define KEY_UP      128
#define KEY_DOWN    129
#define KEY_LEFT    130
#define KEY_RIGHT   131
#define KEY_HOME    132
#define KEY_END     133
#define KEY_DELETE  134
#define KEY_PGUP    135
#define KEY_PGDN    136

/* Modifier bitmask values delivered to event_modchange.
 * Posted only on edge transitions (press/release of CTRL or SHIFT),
 * never repeated. Apps that care should keep a local cached state and
 * AND it with mouse-click events. */
#define DOBUI_MOD_CTRL  0x01
#define DOBUI_MOD_SHIFT 0x02

/* Event handlers — define the ones you need in your app.
 * The framework provides empty weak defaults for all of them. */
void event_start(void);
void event_key(uint8_t key);
void event_mouseclick(int x, int y, uint8_t buttons);
void event_rightclick(int x, int y, uint8_t buttons);
void event_middleclick(int x, int y, uint8_t buttons);
void event_dblclick(int x, int y, uint8_t buttons);
void event_mouserelease(int x, int y, uint8_t buttons);
void event_mousemove(int x, int y, uint8_t buttons);
void event_modchange(uint8_t mods);
void event_scroll(int delta);
void event_resize(int w, int h);
void event_panel(int cmd_idx);
void event_close(void);
void event_tick(void);

/* Drag & drop events.
 *
 * event_drop is called on the target window when the user releases
 * the mouse over it during a drag session. `services[i]` carries the
 * originating service name for `paths[i]` (empty string = default
 * routing). Both arrays and their strings are valid ONLY for the
 * duration of the handler — copy any data you need to keep.
 *
 * event_drag_end is called on the source window once the WM has
 * released the drag session (whether the drop was delivered to a
 * target or cancelled). 'committed' is 1 if a drop was delivered,
 * 0 otherwise. Use it to reset any "dragging" UI state. */
void event_drop(int lx, int ly,
                const char *const *services,
                const char *const *paths, int n_paths, bool is_copy);
void event_drag_end(int committed);

/* Called continuously on whichever window is under the cursor during
 * a drag session, with the local content-area coordinates (lx, ly).
 * Use it to keep a "drop target" highlight up to date — the WM
 * doesn't deliver normal mouse events during a drag, so this is the
 * only feedback channel. No matching "leave" event today; clear the
 * highlight in event_drop and event_drag_end (both fire at session
 * end). */
void event_drag_over(int lx, int ly);

/* Sync IPC request from another program (e.g., dialog service).
 * Program MUST call dob_ipc_reply itself. Default auto-replies empty. */
void event_request(dob_msg_t *msg);

/* Start the application. Creates window, enters event loop, never returns. */
void dobui_run(const char *title, int width, int height);

/* Set periodic tick interval (ms). Call BEFORE dobui_run.
 * Only for apps that need periodic updates (games). */
void dobui_set_tick(uint32_t interval_ms);

/* Set panel commands. Can be called before or during dobui_run. */
void dobui_set_panel(const char *commands);

/* Accessors — valid after dobui_run starts */
uint32_t dobui_window(void);
uint32_t dobui_port(void);
int      dobui_width(void);
int      dobui_height(void);

/* Source identity of the drag whose drop is being delivered. Valid
 * only inside an event_drop() handler; both return 0 outside.
 * Targets use these to send a completion notification IPC back to
 * the source after the async file op finishes — GUI_EVT_DRAG_END
 * fires at drop-commit time, too early for MOVE semantics. */
uint32_t dobui_drop_source_port(void);
uint32_t dobui_drop_source_window(void);

/* Exit cleanly. Call from any handler to terminate. */
void dobui_quit(void);

/* ============================================================
 *  Multi-window applications
 *
 * dobui_run drives ONE window through the global weak event_*
 * handlers above.  An app that needs several real windows from a
 * single process -- a manager with child windows, dialogs, modal
 * popups -- opens each one here with its own callback table.  The
 * event loop routes every event to the right window by id and makes
 * that window's drawing context active before calling back, so a
 * handler just draws into its own window: no SaveContext /
 * RestoreContext, no faking sub-windows inside one surface.
 *
 * The two styles compose: an app may keep dobui_run for its primary
 * window (weak handlers) and still open extra windows with vtables.
 * ============================================================ */

typedef struct dobui_win dobui_win_t;

/* Per-window callbacks.  Define only the ones you need; leave the
 * rest NULL.  `w` is the window the event targets; its user pointer
 * (dobui_win_user) carries your per-window state. */
typedef struct
{
    void (*on_start)      (dobui_win_t *w);
    void (*on_key)        (dobui_win_t *w, uint8_t key);
    void (*on_click)      (dobui_win_t *w, int x, int y, uint8_t buttons);
    void (*on_rightclick) (dobui_win_t *w, int x, int y, uint8_t buttons);
    void (*on_middleclick)(dobui_win_t *w, int x, int y, uint8_t buttons);
    void (*on_dblclick)   (dobui_win_t *w, int x, int y, uint8_t buttons);
    void (*on_release)    (dobui_win_t *w, int x, int y, uint8_t buttons);
    void (*on_mousemove)  (dobui_win_t *w, int x, int y, uint8_t buttons);
    void (*on_scroll)     (dobui_win_t *w, int delta);
    void (*on_modchange)  (dobui_win_t *w, uint8_t mods);
    void (*on_resize)     (dobui_win_t *w, int new_w, int new_h);
    void (*on_panel)      (dobui_win_t *w, int cmd_idx);
    void (*on_close)      (dobui_win_t *w);   /* NULL => close the window */
} dobui_win_vtbl_t;

/* Open a top-level window owned by this process.  Returns NULL on
 * failure (WM unreachable, or the window/slot tables are full).  May
 * be called before or after dobui_run; the first call lazily creates
 * the shared event port.  vt may be NULL (events ignored for this
 * window); user is stashed verbatim.  flags is an OR of DOBUI_WIN_*
 * (NORESIZE / NOMAXIMIZE), or 0 for a normal resizable window. */
dobui_win_t *dobui_win_open(const char *title, int width, int height,
                            const dobui_win_vtbl_t *vt, void *user,
                            uint32_t flags);

/* Open a sub-window owned by `parent`: always stacked above it and
 * destroyed with it.  If modal, the parent (and the chain above it)
 * is input-blocked until this window closes -- a click on the blocked
 * parent rings the attention sound instead.  parent may be NULL to
 * open an unowned top-level (same as dobui_win_open).  flags is an OR
 * of DOBUI_WIN_* (dialogs are typically NORESIZE | NOMAXIMIZE). */
dobui_win_t *dobui_dialog_open(dobui_win_t *parent,
                               const char *title, int width, int height,
                               const dobui_win_vtbl_t *vt, void *user,
                               bool modal, uint32_t flags);

/* Destroy a window opened above and free its slot.  Child windows are
 * destroyed too (matches the WM cascade).  Safe to call from the
 * window's own on_close. */
void dobui_win_close(dobui_win_t *w);

/* Run the event loop for an app built purely on dobui_win_open (i.e.
 * one that never calls dobui_run).  Never returns.  Apps that use
 * dobui_run don't need this -- dobui_run runs the same loop. */
void dobui_loop(void);

/* Dispatch a single already-received event through the normal app
 * handling (event_request routing, per-window vtable demux, the full
 * GUI/timer switch).  Both dobui_run's loop and any nested modal loop
 * (e.g. a dobpopup dialog) call it: the nested loop handles its own
 * window's events and delegates everything else here so ticks and
 * service requests keep flowing while the dialog is up. */
void dobui_dispatch_event(dob_msg_t *msg);

/* Per-window accessors (valid for the window's lifetime). */
uint32_t dobui_win_id    (dobui_win_t *w);
int      dobui_win_width (dobui_win_t *w);
int      dobui_win_height(dobui_win_t *w);
void    *dobui_win_user  (dobui_win_t *w);

/* Handle for the dobui_run primary window (NULL if dobui_run isn't
 * running).  Use it to parent a dialog/modal on the main window of a
 * single-window app: dobui_dialog_open(dobui_primary(), ...). */
dobui_win_t *dobui_primary(void);

#endif
