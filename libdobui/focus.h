/* DobUITools — Focus Manager
 *
 * Tracks the focused control and routes keyboard / mouse / panel
 * events to it. Exactly one control focused at a time.
 *
 * Contextual panel: dobfocus_AttachPanel hands the window's panel to
 * the focus manager. Mute widgets show the app's base commands; a
 * focused textbox/multitextbox swaps in clipboard commands (Incolla,
 * Pulisci, Copia tutto + Copia/Taglia when a selection is live).
 * Panel clicks via dobfocus_OnPanel return true when consumed by the
 * focus manager so the app skips them.
 *
 * Usage:
 *   dob_focus_t fm;
 *   dobfocus_Init(&fm);
 *   dobfocus_Register(&fm, &textbox, DOB_CTRL_TEXTBOX);
 *   dobfocus_AttachPanel(&fm, dobui_window(), "Apri\nSalva");
 *
 *   dobfocus_OnClick(&fm, x, y);
 *   dobfocus_OnDrag(&fm, x, y);
 *   dobfocus_OnRelease(&fm);
 *   dobfocus_OnDblClick(&fm, x, y);
 *   if (dobfocus_OnPanel(&fm, idx)) { redraw(); return; }
 */

#ifndef MAINDOB_DOBUITOOLS_FOCUS_H
#define MAINDOB_DOBUITOOLS_FOCUS_H

#include <dob/types.h>
#include "dobui_common.h"

#define DOBFOCUS_MAX     64

typedef struct
{
    void            *ctrl;
    dob_ctrl_type_t  type;
} dob_focus_entry_t;

typedef struct
{
    dob_focus_entry_t entries[DOBFOCUS_MAX];
    int               count;
    int               focused;      /* -1 = none */

    /* Panel management — attached lazily via dobfocus_AttachPanel. */
    uint32_t          panel_win_id;
    const char       *panel_base;
    bool              panel_attached;
    uint32_t          panel_sig;    /* hash of last published panel */
} dob_focus_t;

void dobfocus_Init(dob_focus_t *fm);

/* Register a control. Returns its index, or -1 if full. */
int  dobfocus_Register(dob_focus_t *fm, void *ctrl, dob_ctrl_type_t type);

/* Set focus to a registered control. Clears previous focus.
 * Refreshes the panel if one is attached. */
void dobfocus_SetFocus(dob_focus_t *fm, void *ctrl);

/* Clear all focus. Refreshes the panel to the base. */
void dobfocus_ClearFocus(dob_focus_t *fm);

/* Route a key event to the focused control. Returns true if consumed. */
bool dobfocus_OnKey(dob_focus_t *fm, uint8_t key);

/* Route a scroll event. Returns true if consumed. */
bool dobfocus_OnScroll(dob_focus_t *fm, int delta);

/* Hit-test all controls. If hit: set focus, call OnClick, return
 * pointer. Returns NULL if nothing was clicked. */
void *dobfocus_OnClick(dob_focus_t *fm, int x, int y);

/* Double-click routing for text widgets. Returns the widget that
 * consumed the event, or NULL. */
void *dobfocus_OnDblClick(dob_focus_t *fm, int x, int y);

/* Finish any in-flight press or drag. For sliders, returns the
 * slider whose value just committed; otherwise returns NULL. */
void *dobfocus_OnRelease(dob_focus_t *fm);

/* Route a mouse drag to controls that track it (slider, textbox,
 * multitextbox). Returns true if a drag is active. */
bool dobfocus_OnDrag(dob_focus_t *fm, int x, int y);

/* Attach the window's command panel. From this call on the focus
 * manager owns it: focus transitions overwrite the published
 * commands as needed. `base_cmds` is a '\n'-separated list of the
 * app's default commands, shown when no contextual widget has focus. */
void dobfocus_AttachPanel(dob_focus_t *fm, uint32_t win_id,
                          const char *base_cmds);

/* Replace the base panel (e.g. when the app's own mode changes). */
void dobfocus_SetBasePanel(dob_focus_t *fm, const char *base_cmds);

/* Recompute and publish the panel if it has changed. Called
 * automatically by the event routing above — apps rarely need to
 * invoke it directly. */
void dobfocus_RefreshPanel(dob_focus_t *fm);

/* Handle a panel-command index. Returns true when the focus manager
 * consumed the command (contextual clipboard op); false when the app
 * should handle it as one of its base commands. */
bool dobfocus_OnPanel(dob_focus_t *fm, int cmd_idx);

/* Get pointer / type of focused control. */
void            *dobfocus_GetFocused(dob_focus_t *fm);
dob_ctrl_type_t  dobfocus_GetFocusedType(dob_focus_t *fm);

/*  *  Implicit global focus manager
 *
 *  Every widget that supports focus self-registers into a library-
 *  owned singleton at _Init time. Apps can then drive the focus
 *  manager without ever declaring their own dob_focus_t: the calls
 *  below operate on the singleton.
 *
 *  Apps that already use the explicit API continue to work as
 *  before — passing a non-NULL `fm` to the dobfocus_On*() family
 *  dispatches to that instance instead of the singleton.
 */

/* Access the singleton directly (rarely needed). */
dob_focus_t *dobfocus_global(void);

/* Thin wrappers around the explicit API, always targeting the
 * singleton. Pair one-to-one with dobfocus_On*() below. */
void             dobfocus_set_focus(void *ctrl);
void             dobfocus_clear_focus(void);
void            *dobfocus_get_focused(void);
dob_ctrl_type_t  dobfocus_get_focused_type(void);
bool             dobfocus_key(uint8_t key);
void            *dobfocus_click(int x, int y);
void            *dobfocus_dblclick(int x, int y);
void            *dobfocus_release(void);
bool             dobfocus_drag(int x, int y);
bool             dobfocus_scroll(int delta);
bool             dobfocus_panel(int cmd_idx);
void             dobfocus_attach_panel(uint32_t win_id, const char *base);
void             dobfocus_set_base_panel(const char *base);
void             dobfocus_refresh_panel(void);

#endif
