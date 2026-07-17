#ifndef MAINDOB_STUBS_DOBINTERFACE_H
#define MAINDOB_STUBS_DOBINTERFACE_H

/* DobInterface Entry Point — GUI API
 *
 * Usage:
 *   #include <DobInterface.h>
 *
 *   uint32_t win = dobui_CreateWindow(400, 300, my_port, "My App");
 *   dobui_FillRect(win, 0, 0, 400, 300, 0x00FFFFFF);
 *   dobui_DrawText(win, 10, 10, "Hello", 0x00000000, 0x00FFFFFF);
 *   dobui_SetPanelCommands(win, "Save\nOpen\nClose");
 *   dobui_Invalidate(win);
 */

#include <dob/types.h>

/* Create a window. Returns window ID (>0) or 0 on error. */
uint32_t dobui_CreateWindow(int width, int height, uint32_t owner_port, const char *title);

/* Window policy flags for dobui_SetWindowFlags(). Independent bits. */
#define DOBUI_WIN_NORESIZE    0x1   /* user cannot resize the window   */
#define DOBUI_WIN_NOMAXIMIZE  0x2   /* user cannot maximize the window */

/* Set window policy flags (a DOBUI_WIN_* bitmask, OR-combined).
 * Call once right after dobui_CreateWindow. Synchronous: the flags
 * are in effect by the time this returns, so chrome interactions are
 * gated before the window is ever drawn. Each call replaces the full
 * flag set; pass 0 to clear all policy restrictions. */
void dobui_SetWindowFlags(uint32_t win_id, uint32_t flags);

/* Establish an owner relationship between two windows of any process.
 * child_id becomes a sub-window of parent_id and is always stacked
 * above it; when the parent is destroyed the child is destroyed too.
 * If modal is true, the parent (and the chain above it) is input-
 * blocked while the child is alive: a click on a blocked window rings
 * the attention sound and pulls the child to the front instead.
 *
 * Pass parent_id == 0 to detach (back to a top-level, non-modal
 * window).  parent_id is a window id and may belong to a different
 * process (e.g. a separate-process popup made modal over your window).
 *
 * Call once right after dobui_CreateWindow, before the first
 * dobui_Invalidate -- same timing rule as dobui_SetWindowFlags. */
void dobui_SetParent(uint32_t child_id, uint32_t parent_id, bool modal);

/* Destroy a window */
void dobui_DestroyWindow(uint32_t win_id);

/* Change window title */
void dobui_SetTitle(uint32_t win_id, const char *title);

/* Bring window to front and focus it */
void dobui_Raise(uint32_t win_id);

/* Hide window (stays alive, can be shown again) */
void dobui_Hide(uint32_t win_id);

/* Drawing primitives (into window backing buffer) */
void dobui_FillRect(uint32_t win_id, int x, int y, int w, int h, uint32_t color);
void dobui_DrawText(uint32_t win_id, int x, int y, const char *text, uint32_t fg, uint32_t bg);
/* Monospace (fixed 8px pitch) variant of dobui_DrawText. dobui_DrawText is
 * proportional; use this for editable text fields and anywhere a fixed cell
 * grid is required (the compositor advances one DOB_FONT_W cell per glyph). */
void dobui_DrawTextFixed(uint32_t win_id, int x, int y, const char *text, uint32_t fg, uint32_t bg);
void dobui_DrawRect(uint32_t win_id, int x, int y, int w, int h, uint32_t color);
void dobui_DrawPixel(uint32_t win_id, int x, int y, uint32_t color);

/* Blit a raw pixel buffer (0x00RRGGBB) into the window at (x,y).
 * src: pixel array, row-major, src_w * src_h entries.
 * Pixels with value 0xFF000000 are treated as transparent (skipped). */
void dobui_BlitBuffer(uint32_t win_id, int x, int y,
                      const uint32_t *src, int src_w, int src_h);

/* Same as dobui_BlitBuffer, but for buffers whose pixels change between
 * redraws (editor pages, live previews). dobui_BlitBuffer caches the texture
 * by source pointer and skips the upload when the pointer is unchanged, which
 * freezes a buffer mutated in place; this variant always re-uploads. Use only
 * for genuinely dynamic buffers (one GPU upload per call). */
void dobui_BlitBufferDynamic(uint32_t win_id, int x, int y,
                             const uint32_t *src, int src_w, int src_h);
/* Variante a banda: identica a Dynamic ma ricarica SOLO le righe
 * [dirty_row0, dirty_row0+dirty_rows) del buffer — per chi ridisegna
 * una porzione nota (l'editor che riscrive un paragrafo). Se la entry
 * di pool e' nuova o il puntatore e' cambiato, l'upload e' comunque
 * integrale (le altre righe non esistono ancora lato server). */
void dobui_BlitBufferDynamicRows(uint32_t win_id, int x, int y,
                                 const uint32_t *src, int src_w, int src_h,
                                 int dirty_row0, int dirty_rows);
/* Pannello SHM del contenuto: framebuffer condiviso app<->server per i
 * contenuti grandi e vivi (la pagina di un editor). Ensure crea/mappa
 * (o rialloca su misura diversa) e restituisce il puntatore dove
 * disegnare; Blit accoda il record che al bake copia [0,0,w,h) del
 * pannello alle coordinate (x,y) del corpo. Zero pixel via IPC. */
uint32_t *dobui_ShmPanelEnsure(uint32_t win_id, int w, int h);
/* band_y0/band_rows: banda sporca in coordinate pannello (righe che il
 * bake deve ricopiare nel corpo). band_rows<=0 = copia integrale;
 * entrambi = 0xFFFF (DOBUI_SHMPANEL_UNCHANGED) = contenuto invariato,
 * zero copia. Il server promuove comunque a integrale finche' corpo e
 * pannello non sono sincronizzati (primo bake, resize). */
void      dobui_ShmPanelBlit(uint32_t win_id, int x, int y, int w, int h,
                             int band_y0, int band_rows);

/* Force repaint of the window on screen */
void dobui_Invalidate(uint32_t win_id);

/* Make win_id's drawing context active for subsequent draw calls.
 * Each window a process opens has its own context; the app framework
 * calls this before delivering an event to a window, so each window's
 * handlers draw into their own window with no manual bookkeeping.
 * No-op for an unknown id. */
void dobui_SetActiveWindow(uint32_t win_id);

/* Handle resize event — call when GUI_EVT_RESIZE is received.
 * Remaps the shared framebuffer if needed (arg3 = new shm_id).
 * new_w, new_h: the new dimensions from arg1, arg2.
 * new_shm_id: from arg3 (>= 0 = remap needed, -1 = no remap). */
void dobui_HandleResize(int new_w, int new_h, int new_shm_id);

/* Returns true if a resize remap is in progress.
 * Drawing is automatically skipped during this time, but callers
 * can also check this to avoid queuing a redraw that would be lost. */
bool dobui_IsResizePending(void);

/* Set context commands shown in the panel when this window is focused.
 * commands: newline-separated labels, e.g. "Apri\nCopia\nElimina" */
void dobui_SetPanelCommands(uint32_t win_id, const char *commands);

/* Clear all context commands for this window */
void dobui_ClearPanelCommands(uint32_t win_id);

/* Spawn a .mdl and promote it to driver.
 * DobInterface performs the spawn and make_driver on behalf of the caller.
 * Returns PID on success, -1 on failure. */
pid_t dobui_SpawnDriver(const char *path, const char *const argv[]);

/* ================================================================
 *  CURSOR — per-window override
 *
 * A window can ask for a non-default cursor shape while the mouse is
 * inside its body (header bar and resize edges keep the WM-managed
 * defaults).  The override is stored on the window and goes away
 * automatically when the window is destroyed — no cleanup needed,
 * even on a process crash mid-drag.
 *
 * Event-driven: typical pattern is one call on press to install the
 * shape, one call on release to clear it.  Two IPCs per drag, none
 * per frame.
 *
 *     // start of column-divider drag
 *     dobui_SetCursor(win_id, CURSOR_HSPLIT);
 *     ...
 *     // end of drag
 *     dobui_SetCursor(win_id, CURSOR_DEFAULT);
 *
 * Pass CURSOR_DEFAULT to drop the override and fall back to the
 * WM-computed shape (CURSOR_ARROW for normal hovering, CURSOR_RESIZE
 * over a window edge, etc.). */

#define CURSOR_ARROW       0
#define CURSOR_RESIZE      1     /* Diagonal ↘↖ — window resize edges */
#define CURSOR_FILE_DRAG   2     /* WM-internal, used during file-drag DnD */
#define CURSOR_HSPLIT      3     /* Horizontal ←→ — column splitters etc */
#define CURSOR_VSPLIT      4     /* Vertical ↕ — row / margin splitters    */
#define CURSOR_DEFAULT   (-1)    /* Sentinel: drop any override          */

/* Set or clear the cursor override for `win_id`.  The override is in
 * effect only while the mouse is inside the window's body.  Returns 0
 * on success, -1 if the WM is unreachable.
 *
 * cursor_type: a CURSOR_* constant, or CURSOR_DEFAULT (-1) to drop
 * the override.  Signed because CURSOR_DEFAULT is negative — a
 * uint8_t parameter would silently truncate -1 to 0xFF and the WM
 * would reject the call as "out of range". */
int dobui_SetCursor(uint32_t win_id, int cursor_type);

/*  *  WIDGETS — mini-windows composited in the widget tray
 *
 * Usage:
 *   uint32_t wid = dobui_CreateWidget(120, 40, my_port);
 *   // Draw into widget framebuffer (same drawing API, use wid)
 *   dobui_FillRect(wid, 0, 0, 120, 40, COLOR_BG);
 *   dobui_DrawText(wid, 4, 12, "Rete: Attiva", fg, bg);
 *   dobui_WidgetInvalidate(wid);
 *
 *   // In event loop, handle GUI_EVT_WIDGET_CLICK:
 *   //   arg0 = widget_id, arg1 = mouse_x (relative), arg2 = mouse_y (relative)
 *
 * Widgets are displayed when the user clicks '<' in the panel footer.
 * They are stacked vertically and dismissed on any outside click.
 * The process owns the framebuffer and draws whatever it wants.
 */

/* Create a widget. Returns widget_id (>0) or 0 on error.
 * width/height: pixel dimensions of the widget surface.
 * owner_port: IPC port to receive GUI_EVT_WIDGET_CLICK events. */
uint32_t dobui_CreateWidget(int width, int height, uint32_t owner_port);

/* Destroy a widget and free its resources. */
void dobui_DestroyWidget(uint32_t widget_id);

/* Notify the compositor that widget content has changed. */
void dobui_WidgetInvalidate(uint32_t widget_id);

/* Widget framebuffer context — for processes with both windows and widgets.
 * Save/restore around CreateWidget to keep the window fb active. */
typedef struct
{
    uint32_t *fb;
    int       fb_w, fb_h;
    int       shm_id;
} dobui_widget_fb_ctx_t;

void dobui_WidgetSaveContext(dobui_widget_fb_ctx_t *ctx);
void dobui_WidgetRestoreContext(const dobui_widget_fb_ctx_t *ctx);

/* ================================================================
 *  DRAG & DROP
 *
 * File-drag session mediated by the WM. The source calls BeginDrag
 * once, passing the list of source paths as a packed payload. The WM
 * takes ownership of a copy, shows the file-drag cursor, tracks the
 * Space key as a copy-mode modifier, and on mouse release forwards a
 * GUI_EVT_DROP event to the window under the cursor (including the
 * packed payload). Source receives GUI_EVT_DRAG_END on session end.
 *
 * Payload wire format (packed little-endian):
 *     u8   is_cut          (1 = move, 0 = copy)
 *     u16  n_paths
 *     n × null-terminated path string
 *
 * Paths are unpacked by libdobui before dispatch to the app via the
 * weak event_drop() handler. Applications never touch the wire. */

/* Begin a drag session. The paths array must contain n_paths entries,
 * each at most 512 bytes. is_cut selects default semantics (can be
 * overridden at drop time by Space held). Returns 0 on success. */
int  dobui_BeginDrag(uint32_t src_win_id,
                     const char *const *paths, int n_paths, bool is_cut);

/* Same, but each path carries an originating service name. The drop
 * target receives the (service, path) pairs so it can route source
 * Open/Stat to the right backend (cross-service drag from CD or
 * floppy onto the boot-disk window, etc). Pass NULL services or "" per
 * entry to mean "default routing on the receiver". */
int  dobui_BeginDragOn(uint32_t src_win_id,
                       const char *const *services,
                       const char *const *paths, int n_paths, bool is_cut);

/* Abort the current drag session. No-op if not the owner. */
void dobui_CancelDrag(void);

#endif /* MAINDOB_STUBS_DOBINTERFACE_H */
