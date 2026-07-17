/* dobUI Application Framework — Implementation
 *
 * Single blocking event loop using dob_ipc_receive.
 * The thread sleeps until the kernel wakes it because an event arrived.
 * Weak symbols allow the app to define only the handlers it needs. */

#include "app.h"
#include "focus.h"
#include <unistd.h>
#include <string.h>
#include <DobInterface.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/types.h>

/* GUI event codes (from dobinterface) */
#define GUI_EVT_PANEL_CMD   200
#define GUI_EVT_MOUSE       201
#define GUI_EVT_KEY         202
#define GUI_EVT_SCROLL      203
#define GUI_EVT_CLOSE_REQ   210
#define GUI_EVT_RESIZE      212
#define GUI_EVT_MODCHANGE   213
#define GUI_EVT_DROP        214
#define GUI_EVT_DRAG_END    215
#define GUI_EVT_DRAG_OVER   216

/* Range of message codes that this loop dispatches via the dedicated
 * GUI/timer switch below. Any post outside this range (and outside the
 * timer code 70) is forwarded to event_request so apps can react to
 * service notifications such as DOBFS_UNMOUNT_NOTIFY. */
#define GUI_EVT_FIRST       200
#define GUI_EVT_LAST        216
#define TIMER_EVT           70

#define DROP_MAX_PATHS      256   /* must match DobFiles CLIP_MAX_PATHS */

/* Weak defaults — overridden by the app when it defines the function */

__attribute__((weak)) void event_start(void)                    { }
__attribute__((weak)) void event_key(uint8_t key)               { (void)key; }
__attribute__((weak)) void event_mouseclick(int x, int y, uint8_t b)
                                                                 { (void)x; (void)y; (void)b; }
__attribute__((weak)) void event_rightclick(int x, int y, uint8_t b)
                                                                 { (void)x; (void)y; (void)b; }
__attribute__((weak)) void event_middleclick(int x, int y, uint8_t b)
                                                                 { (void)x; (void)y; (void)b; }
__attribute__((weak)) void event_dblclick(int x, int y, uint8_t b)
                                                                 { (void)x; (void)y; (void)b; }
__attribute__((weak)) void event_mouserelease(int x, int y, uint8_t b)
                                                                 { (void)x; (void)y; (void)b; }
__attribute__((weak)) void event_mousemove(int x, int y, uint8_t b)
                                                                 { (void)x; (void)y; (void)b; }
__attribute__((weak)) void event_modchange(uint8_t mods)         { (void)mods; }
__attribute__((weak)) void event_scroll(int delta)               { (void)delta; }
__attribute__((weak)) void event_resize(int w, int h)            { (void)w; (void)h; }
__attribute__((weak)) void event_panel(int cmd_idx)              { (void)cmd_idx; }
__attribute__((weak)) void event_tick(void)                      { }
__attribute__((weak)) void event_close(void)
{
    extern uint32_t _dobui_win_id;
    if (_dobui_win_id)
        dobui_DestroyWindow(_dobui_win_id);
    _exit(0);
}
__attribute__((weak)) void event_request(dob_msg_t *msg)
{
    dob_msg_t reply;
    memset(&reply, 0, sizeof(reply));
    dob_ipc_reply(msg->sender_tid, &reply);
}
__attribute__((weak)) void event_drop(int lx, int ly,
                                      const char *const *services,
                                      const char *const *paths, int n_paths,
                                      bool is_copy)
{
    (void)lx; (void)ly; (void)services; (void)paths; (void)n_paths; (void)is_copy;
}
__attribute__((weak)) void event_drag_end(int committed)
{
    (void)committed;
}
__attribute__((weak)) void event_drag_over(int lx, int ly)
{
    (void)lx; (void)ly;
}

/* State */

uint32_t _dobui_win_id = 0;
static uint32_t _event_port = 0;
static int _win_w = 0;
static int _win_h = 0;
static uint32_t _tick_ms = 0;
static bool     _tick_armed = false;   /* timer message currently in flight */
static const char *_pending_panel = NULL;

/* Source port + window id of the drag being delivered to event_drop.
 * Valid for the duration of the event_drop() callback only. The
 * target uses dobui_drop_source_port() to address a later completion
 * notification — GUI_EVT_DRAG_END fires immediately at drop-commit
 * time, before the target's chunked op runs, so the source can't
 * rely on it alone to refresh after a MOVE. */
static uint32_t _drop_src_port   = 0;
static uint32_t _drop_src_win_id = 0;
uint32_t dobui_drop_source_port(void)   { return _drop_src_port; }
uint32_t dobui_drop_source_window(void) { return _drop_src_win_id; }

/* Accessors */

uint32_t dobui_window(void) { return _dobui_win_id; }
uint32_t dobui_port(void)   { return _event_port; }
int      dobui_width(void)  { return _win_w; }
int      dobui_height(void) { return _win_h; }

/* Set or change the tick interval. Calling with 0 stops the tick.
 *
 * If the tick is currently inactive (no TIMER_EVT in flight) and the
 * new interval is non-zero, we arm the first one-shot here so the
 * caller doesn't have to wait for an unrelated event to wake the loop.
 * Without this, a set_tick called from inside a non-timer event handler
 * (e.g. event_drop starting a chunked op) would never produce a tick:
 * the rearm-on-fire logic in the run loop only fires after a TIMER_EVT,
 * which never arrives if no one ever armed one. */
void dobui_set_tick(uint32_t interval_ms)
{
    _tick_ms = interval_ms;
    if (interval_ms > 0 && !_tick_armed && _event_port != 0)
    {
        timer_set(_event_port, interval_ms, 0);
        _tick_armed = true;
    }
}

void dobui_set_panel(const char *commands)
{
    if (_dobui_win_id)
        dobui_SetPanelCommands(_dobui_win_id, commands);
    else
        _pending_panel = commands;
}

void dobui_quit(void)
{
    if (_dobui_win_id)
        dobui_DestroyWindow(_dobui_win_id);
    _exit(0);
}

/* ============================================================
 *  Multi-window object model
 *
 * Each window a process opens via dobui_win_open/dobui_dialog_open is
 * registered here with its own callback table.  The event loop reads
 * the window id from arg0, finds the object, makes its drawing
 * context active, and dispatches to its callbacks.  Windows with no
 * vtable (and the dobui_run primary, which isn't registered here at
 * all) fall through to the global weak event_* handlers.
 * ============================================================ */

#define DOBUI_WIN_MAX 16

struct dobui_win
{
    uint32_t                id;          /* WM window id, 0 = free slot   */
    uint32_t                parent_id;   /* owner window id, 0 = top-level */
    int                     w, h;
    const dobui_win_vtbl_t *vt;
    void                   *user;
};
static struct dobui_win _wins[DOBUI_WIN_MAX];

/* The dobui_run primary, registered with vt==NULL so its own events
 * still flow to the global weak handlers, but it can be passed as the
 * parent of a dialog/modal opened by a single-window app. */
static struct dobui_win *_primary_obj = NULL;

static struct dobui_win *win_obj_find(uint32_t id)
{
    if (!id) return NULL;
    for (int i = 0; i < DOBUI_WIN_MAX; i++)
        if (_wins[i].id == id) return &_wins[i];
    return NULL;
}

static struct dobui_win *win_obj_alloc(void)
{
    for (int i = 0; i < DOBUI_WIN_MAX; i++)
        if (_wins[i].id == 0) return &_wins[i];
    return NULL;
}

uint32_t dobui_win_id    (dobui_win_t *w) { return w ? w->id : 0; }
int      dobui_win_width (dobui_win_t *w) { return w ? w->w  : 0; }
int      dobui_win_height(dobui_win_t *w) { return w ? w->h  : 0; }
void    *dobui_win_user  (dobui_win_t *w) { return w ? w->user : NULL; }

/* Handle for the dobui_run primary window, or NULL if dobui_run isn't
 * running.  Lets a single-window app parent a dialog/modal on its
 * main window: dobui_dialog_open(dobui_primary(), ...). */
dobui_win_t *dobui_primary(void) { return _primary_obj; }

/* Lazily create the shared event port the first time any window is
 * opened (covers the case where a window is opened before dobui_run,
 * or without dobui_run at all). */
static void ensure_event_port(void)
{
    if (_event_port == 0)
    {
        _event_port = (uint32_t)port_create();
        dob_registry_wait("dobinterface", 5000);
    }
}

static struct dobui_win *win_open_common(const char *title, int w, int h,
                                         uint32_t parent_id,
                                         const dobui_win_vtbl_t *vt, void *user,
                                         uint32_t flags)
{
    ensure_event_port();
    struct dobui_win *obj = win_obj_alloc();
    if (!obj) return NULL;

    /* All of a process's windows deliver to the same port; the WM tags
     * each event with the window id in arg0, which is how the loop
     * demuxes.  CreateWindow also allocates this window's stub drawing
     * context (its own cmdbuf + texture pool). */
    uint32_t id = dobui_CreateWindow(w, h, _event_port, title);
    if (id == 0) return NULL;

    /* Apply window policy flags (DOBUI_WIN_NORESIZE / NOMAXIMIZE)
     * before the first Invalidate -- the WM gates them while the
     * window is still undrawn.  Enforcement is per-window, so it
     * applies to sub-windows and dialogs exactly as to top-levels. */
    if (flags)
        dobui_SetWindowFlags(id, flags);

    obj->id        = id;
    obj->parent_id = parent_id;
    obj->w         = w;
    obj->h         = h;
    obj->vt        = vt;
    obj->user      = user;
    return obj;
}

dobui_win_t *dobui_win_open(const char *title, int width, int height,
                            const dobui_win_vtbl_t *vt, void *user,
                            uint32_t flags)
{
    struct dobui_win *obj = win_open_common(title, width, height, 0,
                                            vt, user, flags);
    if (obj)
    {
        dobui_SetActiveWindow(obj->id);
        if (vt && vt->on_start) vt->on_start(obj);
        dobui_Invalidate(obj->id);
    }
    return obj;
}

dobui_win_t *dobui_dialog_open(dobui_win_t *parent,
                               const char *title, int width, int height,
                               const dobui_win_vtbl_t *vt, void *user,
                               bool modal, uint32_t flags)
{
    uint32_t pid = parent ? parent->id : 0;
    struct dobui_win *obj = win_open_common(title, width, height, pid,
                                            vt, user, flags);
    if (obj)
    {
        /* Establish ownership BEFORE the first draw -- same timing rule
         * as the window flags.  For a modal this begins blocking the
         * parent immediately. */
        dobui_SetParent(obj->id, pid, modal);
        dobui_SetActiveWindow(obj->id);
        if (vt && vt->on_start) vt->on_start(obj);
        dobui_Invalidate(obj->id);
    }
    return obj;
}

void dobui_win_close(dobui_win_t *w)
{
    if (!w || !w->id) return;
    uint32_t id = w->id;

    /* Mirror the WM cascade in our object table: free child objects
     * too, so their slots don't linger with dead ids after the WM
     * destroys the windows underneath them. */
    for (int i = 0; i < DOBUI_WIN_MAX; i++)
        if (_wins[i].id && _wins[i].parent_id == id)
            dobui_win_close(&_wins[i]);

    w->id = 0; w->parent_id = 0; w->vt = NULL; w->user = NULL;
    dobui_DestroyWindow(id);   /* WM destroys the window (and its children) */
}

/* Forward decl: the shared event loop, defined as the tail of
 * dobui_run below and reused by dobui_loop. */
static void dobui_event_loop(void);

void dobui_loop(void)
{
    ensure_event_port();
    dobui_event_loop();
}

/* Event loop — the only loop in the entire app */

void dobui_run(const char *title, int width, int height)
{
    _win_w = width;
    _win_h = height;

    /* Create event port */
    _event_port = (uint32_t)port_create();

    /* Wait for dobinterface */
    dob_registry_wait("dobinterface", 5000);

    /* Create window */
    _dobui_win_id = dobui_CreateWindow(width, height, _event_port, title);
    if (_dobui_win_id == 0)
        _exit(1);

    /* Register the primary in the object table with vt==NULL: its own
     * events keep flowing to the global weak handlers (use_vt stays
     * false), but it now has a dobui_win_t the app can pass as the
     * parent of a dialog/modal via dobui_primary(). */
    _primary_obj = win_obj_alloc();
    if (_primary_obj)
    {
        _primary_obj->id        = _dobui_win_id;
        _primary_obj->parent_id = 0;
        _primary_obj->w         = width;
        _primary_obj->h         = height;
        _primary_obj->vt        = NULL;
        _primary_obj->user      = NULL;
    }

    /* Apply buffered panel commands */
    if (_pending_panel)
    {
        dobui_SetPanelCommands(_dobui_win_id, _pending_panel);
        _pending_panel = NULL;
    }

    /* First tick: one-shot timer. Re-armed after each event_tick.
     * Never more than 1 timer message in the system. */
    if (_tick_ms > 0)
    {
        timer_set(_event_port, _tick_ms, 0);
        _tick_armed = true;
    }

    /* App-specific init (initial draw, etc.) */
    event_start();

    dobui_event_loop();
}

/* The shared blocking event loop.  Reached from dobui_run (after it
 * creates the primary window) and from dobui_loop (multi-window apps
 * with no primary).  Demuxes each event to the window named in arg0:
 * a registered window with a vtable gets its callbacks, everything
 * else falls through to the global weak event_* handlers. */
/* Dispatch a single already-received event through the normal app
 * handling: event_request routing, per-window vtable demux, active-
 * context switch, and the full GUI/timer switch.  Factored out of
 * dobui_event_loop so a nested modal loop (e.g. a dobpopup dialog)
 * can intercept its own window's events and delegate everything else
 * here, keeping ticks / service requests / other windows alive. */
void dobui_dispatch_event(dob_msg_t *msg)
{
    /* Route to event_request: every sync request, plus async posts
     * whose code falls outside the GUI/timer range. Service-level
     * notifications (DOBFS_UNMOUNT_NOTIFY, future similar) arrive
     * as fire-and-forget posts and need to reach the app; GUI and
     * timer codes have their own dispatchers below. */
    bool is_gui_or_timer = (msg->code >= GUI_EVT_FIRST && msg->code <= GUI_EVT_LAST)
                        || msg->code == 70   /* MSG_TIMER */;

    /* Events not addressed to a specific window -- service requests
     * (event_request) and timer ticks (event_tick) -- default their
     * draw context to the main window before dispatch.  Drawing
     * primitives target the active context, not the window id they're
     * passed, so without this a redraw triggered from one of these
     * handlers (e.g. an async worker result repainting a progress bar)
     * would land on whichever window happened to be active last -- such
     * as an open dialog -- painting stray content into it for a frame.
     * GUI events set the per-window context below and are left alone.
     * No-op for apps with their own loop (no dobui_run primary). */
    bool is_gui_event = (msg->type != 1)
                     && (msg->code >= GUI_EVT_FIRST && msg->code <= GUI_EVT_LAST);
    if (!is_gui_event && _dobui_win_id)
        dobui_SetActiveWindow(_dobui_win_id);

    if (msg->type == 1 || !is_gui_or_timer)
    {
        event_request(msg);
    }

    /* Per-window demux.  GUI events carry the target window id in
     * arg0.  If it names a registered window with a vtable, route
     * to that window's callbacks; otherwise fall through to the
     * global weak handlers (the dobui_run primary and any vtable-
     * less window).  Either way, switch the drawing context to the
     * target window so its handler draws into the right surface
     * with no SaveContext/RestoreContext bookkeeping. */
    struct dobui_win *wo = win_obj_find(msg->arg0);
    bool use_vt = (wo && wo->vt);
    if (is_gui_or_timer && msg->arg0 != 0)
        dobui_SetActiveWindow(msg->arg0);

    /* Dispatch to the appropriate handler */
    switch (msg->code)
    {
        case GUI_EVT_KEY:
            if (use_vt) { if (wo->vt->on_key) wo->vt->on_key(wo, (uint8_t)msg->arg1); }
            else        event_key((uint8_t)msg->arg1);
            break;

        case GUI_EVT_MOUSE:
        {
            /* Sign-extend the 16-bit packed coordinates so that
             * negative positions (cursor dragged above/left of the
             * window during a drag) survive the round-trip through
             * the unsigned 32-bit IPC field. Without the cast,
             * a y of -10 would arrive as 65526. */
            int lx = (int)(int16_t)(msg->arg1 & 0xFFFF);
            int ly = (int)(int16_t)((msg->arg1 >> 16) & 0xFFFF);
            uint8_t buttons = (uint8_t)msg->arg2;
            uint32_t etype = msg->arg3;

            if (use_vt)
            {
                const dobui_win_vtbl_t *vt = wo->vt;
                if      (etype == 1) { if (vt->on_click)       vt->on_click(wo, lx, ly, buttons); }
                else if (etype == 2) { if (vt->on_release)     vt->on_release(wo, lx, ly, buttons); }
                else if (etype == 3) { if (vt->on_dblclick)    vt->on_dblclick(wo, lx, ly, buttons); }
                else if (etype == 4) { if (vt->on_rightclick)  vt->on_rightclick(wo, lx, ly, buttons); }
                else if (etype == 5) { if (vt->on_middleclick) vt->on_middleclick(wo, lx, ly, buttons); }
                else if (etype == 6) { if (vt->on_mousemove)   vt->on_mousemove(wo, lx, ly, buttons); }
                /* etype 7 (chrome click): the library still clears
                 * the global focus manager as a convenience; a
                 * window using its own focus manager is unaffected. */
                else if (etype == 7) dobfocus_clear_focus();
                break;
            }

            if (etype == 1)
                event_mouseclick(lx, ly, buttons);
            else if (etype == 2)
                event_mouserelease(lx, ly, buttons);
            else if (etype == 3)
                event_dblclick(lx, ly, buttons);
            else if (etype == 4)
                event_rightclick(lx, ly, buttons);
            else if (etype == 5)
                event_middleclick(lx, ly, buttons);
            else if (etype == 6)
                event_mousemove(lx, ly, buttons);
            else if (etype == 7)
            {
                /* Click on window chrome (titlebar/borders).  The
                 * compositor signals it so apps can drop input
                 * focus on internal widgets — making the side
                 * panel return to its base commands.  Handled
                 * library-side so every app benefits without
                 * needing custom code. */
                dobfocus_clear_focus();
            }
            break;
        }

        case GUI_EVT_SCROLL:
            if (use_vt) { if (wo->vt->on_scroll) wo->vt->on_scroll(wo, (int)(int32_t)msg->arg1); }
            else        event_scroll((int)(int32_t)msg->arg1);
            break;

        case GUI_EVT_MODCHANGE:
            if (use_vt) { if (wo->vt->on_modchange) wo->vt->on_modchange(wo, (uint8_t)msg->arg1); }
            else        event_modchange((uint8_t)msg->arg1);
            break;

        case GUI_EVT_DROP:
        {
            /* Payload wire format:
             *   u8  is_cut (original intent, ignored here)
             *   u16 n_paths
             *   n × ( service\0  path\0 )
             *
             * is_copy is authoritative and comes from arg2 (the WM
             * has already applied the Space modifier). All strings
             * point directly into msg->payload and are valid only
             * for the duration of this handler call. */
            int lx = (int)(int16_t)(msg->arg1 & 0xFFFF);
            int ly = (int)(int16_t)((msg->arg1 >> 16) & 0xFFFF);
            bool is_copy = (msg->arg2 != 0);

            if (!msg->payload || msg->payload_size < 3)
            {
                _drop_src_port = msg->arg3;
                event_drop(lx, ly, NULL, NULL, 0, is_copy);
                _drop_src_port = 0;
                break;
            }

            const uint8_t *buf = (const uint8_t *)msg->payload;
            uint32_t len = msg->payload_size;
            uint16_t n = (uint16_t)(buf[1] | (buf[2] << 8));
            if (n > DROP_MAX_PATHS) n = DROP_MAX_PATHS;

            const char *services[DROP_MAX_PATHS];
            const char *paths   [DROP_MAX_PATHS];
            uint32_t off = 3;
            int parsed = 0;
            for (int i = 0; i < n; i++)
            {
                if (off >= len) break;
                services[i] = (const char *)(buf + off);
                while (off < len && buf[off] != '\0') off++;
                if (off >= len) break;
                off++;
                if (off >= len) break;
                paths[i] = (const char *)(buf + off);
                while (off < len && buf[off] != '\0') off++;
                if (off >= len) break;
                off++;
                parsed++;
            }

            _drop_src_port   = msg->arg3;
            _drop_src_win_id = 0; /* not transported yet; only port matters */
            event_drop(lx, ly, services, paths, parsed, is_copy);
            _drop_src_port   = 0;
            _drop_src_win_id = 0;
            break;
        }

        case GUI_EVT_DRAG_END:
            event_drag_end((int)msg->arg1);
            break;

        case GUI_EVT_DRAG_OVER:
        {
            int lx = (int)(int16_t)(msg->arg1 & 0xFFFF);
            int ly = (int)(int16_t)((msg->arg1 >> 16) & 0xFFFF);
            event_drag_over(lx, ly);
            break;
        }

        case GUI_EVT_PANEL_CMD:
            if (use_vt) { if (wo->vt->on_panel) wo->vt->on_panel(wo, (int)msg->arg1); }
            else        event_panel((int)msg->arg1);
            break;

        case GUI_EVT_RESIZE:
            /* arg0: window id, arg1/arg2: new w/h.  A registered
             * multi-window updates its object dims and relayouts;
             * the dobui_run primary updates the globals and calls
             * event_resize.  (SHM remap is dead -- the WM sends
             * arg3 = -1 -- so a window just redraws at the new
             * size via its cmdbuf.) */
            if (use_vt)
            {
                wo->w = (int)msg->arg1;
                wo->h = (int)msg->arg2;
                if (wo->vt->on_resize) wo->vt->on_resize(wo, wo->w, wo->h);
                dobui_Invalidate(wo->id);
            }
            else if (msg->arg0 == _dobui_win_id)
            {
                _win_w = (int)msg->arg1;
                _win_h = (int)msg->arg2;
                dobui_HandleResize(_win_w, _win_h, (int)msg->arg3);
                event_resize(_win_w, _win_h);
            }
            break;

        case GUI_EVT_CLOSE_REQ:
            if (use_vt)
            {
                if (wo->vt->on_close) wo->vt->on_close(wo);
                else                  dobui_win_close(wo);
            }
            else event_close();
            break;

        case TIMER_EVT:
            /* Mark armed=false BEFORE invoking event_tick: if the
             * tick handler calls dobui_set_tick(N>0) we want it to
             * see _tick_armed=false and arm the next one itself.
             * After event_tick returns, if we're still supposed to
             * tick AND nobody else armed it (e.g. the handler kept
             * the same interval and didn't touch set_tick), we arm
             * the next one-shot here. */
            _tick_armed = false;
            event_tick();
            if (_tick_ms > 0 && !_tick_armed)
            {
                timer_set(_event_port, _tick_ms, 0);
                _tick_armed = true;
            }
            break;
    }
}

static void dobui_event_loop(void)
{
    /* Block forever. Wake only when an event arrives. */
    for (;;)
    {
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        dob_ipc_receive(_event_port, &msg);
        dobui_dispatch_event(&msg);
    }
}
