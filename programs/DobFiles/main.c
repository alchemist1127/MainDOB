/* MainDOB_Files — File Explorer
 *
 * The main file browser for MainDOB. Creates a window via DobInterface,
 * reads directories via VFS (DobFileSystem), and handles file operations
 * through the panel commands.
 *
 * Also serves as the dialog provider: other programs can request
 * OpenFileDialog and SaveFileDialog via IPC (Entry Point: DobFiles).
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <app.h>
#include <DobInterface.h>
#include <DobFileSystem.h>
#include <DobPopup.h>
#include <DobConfig.h>
#include <DobSettings.h>
#include <DobTable.h>
#include <progressbar.h>
#include <picturebutton.h>
#include <dob/spawn.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/types.h>
#include <dob/device_icon.h>

/* Constants */

#define WIN_W           720
#define WIN_H           520
#define WIN_W_MIN       560
#define WIN_H_MIN       360
#define BREADCRUMB_H    28
#define RIBBON_H        64          /* Toolbar under the breadcrumb (button + label) */
#define ITEM_H          24
#define MAX_ENTRIES     128
#define MAX_PATH        512
#define FONT_W          8
#define FONT_H          16

/* Colors — cyberpunk palette
 * - list rows alternate dark blue / dark cyan (zebra)
 * - selection: yellow row background
 * - default text: yellow (high contrast on both blue and cyan rows)
 * - text over a selected (yellow) row: cyan (yellow-on-yellow is
 *   obviously unreadable; cyan is the complementary high-contrast
 *   choice that still fits the palette)
 * - folder marker: magenta so it stands out from the yellow label
 * - breadcrumb / ribbon chrome: near-black blue with cyan-blue buttons
 */
#define COL_BG          0x00001640  /* Even row: dark blue */
#define COL_BG_ALT      0x00003366  /* Odd row: dark cyan */
#define COL_BG_SEL      0x00FFE000  /* Selection: yellow highlight */
#define COL_TEXT         0x00FFE000  /* Body text: yellow */
#define COL_TEXT_SEL     0x00000000  /* Text over yellow selection: black */
#define COL_BREADCRUMB   0x00000820  /* Top bar background: near-black blue */
#define COL_BREAD_TEXT   0x00FFE000  /* Breadcrumb labels: yellow */
#define COL_BREAD_BTN    0x00104070  /* Breadcrumb button bg: cyan-blue */
#define COL_RIBBON_BG    0x00001A50  /* Ribbon strip background */
#define COL_FOLDER       0x00FF00FF  /* Folder [D] marker: magenta */

/* Dialog IPC codes (from other programs → us) */
#define FILES_CMD_OPEN_DIALOG   10
#define FILES_CMD_SAVE_DIALOG   11

/* Switch this DobFiles instance to a different filesystem backend.
 * Payload is a NUL-terminated string in the form "service[:rootpath]"
 * — for example "floppy:/u0". The instance rebinds the dobfs stub to
 * the named service and re-navigates to the supplied root path (or
 * "/" if absent). Used by DAS-spawned satellite instances such as
 * the floppy file manager. */
#define FILES_CMD_MOUNT         20

/* Post-only notification sent by a DobFiles target to a DobFiles
 * source when a drag-and-drop file op has finished. The source's
 * GUI_EVT_DRAG_END fires at drop-commit time — before the target's
 * async chunked copy / delete has run — so a MOVE leaves the source
 * still listing the moved-out file until something else triggers a
 * refresh. This opcode bridges that gap: the source re-reads its
 * directory and redraws on receipt. No payload, no reply. */
#define FILES_CMD_DROP_DONE     21

/* Mount lifecycle opcodes (mirror of dobfs_protocol.h). A satellite
 * instance mounted on a removable-media service (e.g. floppy) calls
 * SUBSCRIBE_UNMOUNT once during boot; the service replies to the call
 * immediately and later, on eject, opens a sync call back with
 * UNMOUNT_NOTIFY. The handler in event_request replies, destroys the
 * window and exits. */
#define DOBFS_SUBSCRIBE_UNMOUNT 16
#define DOBFS_UNMOUNT_NOTIFY    17

/* Bulk-operation lock.
 * The primary DobFiles instance holds a single global lock covering
 * all file manager bulk operations (copy/cut/delete/drop). Any
 * DobFiles instance (primary or satellite) calls FILES_BULK_TRY
 * before starting an op. Primary does the check locally; satellites
 * do it via dob_ipc_call to the primary's registered port.
 *
 * Liveness: if TRY finds the lock busy, the primary does
 * dob_ipc_post(holder_port, FILES_BULK_PROBE). If that returns
 * DOB_ERR_DEAD the holder is gone and the lock is reclaimed.
 * FILES_BULK_PROBE is a no-op message — the live holder doesn't
 * even need to handle it (default event_request replies empty);
 * only the post status matters. */
#define FILES_BULK_TRY      30
#define FILES_BULK_RELEASE  31
#define FILES_BULK_PROBE    32

/* State */

static uint32_t win_id = 0;
static int win_w = WIN_W, win_h = WIN_H;

/* Dialog mode: this process is serving an OpenFile/SaveFile request.
 * The dialog runs in this process's own window (created by dobui_run)
 * -- there is no explorer window to hide. */
static bool dialog_mode = false;
static int dialog_type = 0;          /* 10=open, 11=save */
static tid_t dialog_sender_tid = -1; /* Who to reply to */
static char dialog_extensions[128];  /* Pipe-separated: ".txt|.md|.c" */
static char dialog_save_name[256];   /* Default filename for save */
static char dialog_result_path[MAX_PATH];

/* Dialog-process identity. When this DobFiles binary is spawned with
 * "--dialog <svc>" it is a one-shot file dialog, not the explorer:
 * it owns no persistent window, registers <svc> in place of
 * "DobFiles", and _exit()s the instant the pick completes. The
 * picker stub spawns a fresh instance per dialog, so the user's
 * existing explorer windows are never touched. */
static bool        g_is_dialog  = false;
static const char *g_dialog_svc = NULL;

static char current_path[MAX_PATH] = "/DATA";

typedef struct
{
    char name[256];
    bool is_dir;
    uint32_t size;
} dir_entry_t;

static dir_entry_t entries[MAX_ENTRIES];
static int entry_count = 0;
static int selected = -1;            /* primary cursor — last clicked item */
static int scroll_offset = 0;

/*  *  Multi-selection
 *
 *  A bitmap covering the visible entries. The "selected" int above is
 *  kept as the *primary cursor*: it points to the entry under the most
 *  recent click and is used by single-target operations (Apri, Rinomina,
 *  dialog completion). Multi-target operations (Copia, Taglia, Elimina,
 *  Incolla, drop) iterate the bitmap.
 *
 *  Selection rules:
 *    - Plain click on item    → bitmap = {idx},     selected = idx
 *    - Plain click on empty   → bitmap = {},        selected = -1, start rect
 *    - Ctrl + click on item   → toggle idx,         selected = idx (or -1)
 *    - Ctrl + click on empty  → start rect, additive (do not clear bitmap)
 *    - Rectangle drag         → bitmap = items intersecting the rect
 *    - read_directory()       → bitmap cleared
 */

#define SEL_BITS_BYTES ((MAX_ENTRIES + 7) / 8)
static uint8_t sel_bits[SEL_BITS_BYTES];
static int     sel_count = 0;

static inline bool sel_get(int idx)
{
    if (idx < 0 || idx >= MAX_ENTRIES) return false;
    return (sel_bits[idx >> 3] & (uint8_t)(1u << (idx & 7))) != 0;
}

static inline void sel_set(int idx, bool on)
{
    if (idx < 0 || idx >= MAX_ENTRIES) return;
    bool was = sel_get(idx);
    if (on && !was)
    {
        sel_bits[idx >> 3] |= (uint8_t)(1u << (idx & 7));
        sel_count++;
    }
    else if (!on && was)
    {
        sel_bits[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
        sel_count--;
    }
}

static inline void sel_toggle(int idx)
{
    sel_set(idx, !sel_get(idx));
}

static void sel_clear(void)
{
    memset(sel_bits, 0, sizeof(sel_bits));
    sel_count = 0;
}

/* Index of the first selected entry, or -1 if none. */
static int sel_first(void)
{
    for (int i = 0; i < entry_count; i++)
        if (sel_get(i)) return i;
    return -1;
}

/* Modifier state — kept in sync via event_modchange. Bit 0 = CTRL. */
static uint8_t mod_state = 0;

/* Rectangle selection state. While rect_active is true, mouse moves
 * extend the rectangle and recompute the selection bitmap. The drag
 * starts on a click landing in empty list area and ends on release. */
static bool rect_active   = false;
static bool rect_additive = false;     /* set if drag started with Ctrl */
static int  rect_x0, rect_y0;          /* anchor (in window coords) */
static int  rect_x1, rect_y1;          /* current cursor */
static uint8_t rect_base_bits[SEL_BITS_BYTES]; /* snapshot for additive mode */
static int     rect_base_count = 0;

/*  Clipboard — SHM-backed, adaptive size.
 *
 *  A shared-memory region created on demand; its id is published in
 *  the config server under the key "clipboard". Any DobFiles instance
 *  reads the id and maps the region. clipboard_set_multi unmaps the
 *  old region and creates a fresh one of the needed size; older
 *  regions are reclaimed once the last mapper unmaps (kernel SHM is
 *  refcounted).
 *
 *  Layout (header is uint32 × 5 = 20 bytes, then payload):
 *      uint32 magic     ('DCLP')
 *      uint32 mode      (0 = copy, 1 = cut)
 *      uint32 count     (entries in payload)
 *      uint32 used      (bytes used in payload)
 *      uint32 capacity  (bytes available)
 *      payload[]: 'count' entries, each as
 *                 service\0path\0
 *      Empty service means "let the receiver route by path prefix".
 */

#define CLIP_MAGIC        0x44434C50u   /* 'DCLP' */
#define CLIP_HEADER_SIZE  20u
#define CLIP_MIN_PAYLOAD  64u
#define CLIP_MAX_PATHS    256

static int      clip_local_id   = -1;
static uint8_t *clip_local_addr = NULL;

static void clip_unmap_local(void)
{
    if (clip_local_id >= 0)
    {
        shm_unmap(clip_local_id);
        clip_local_id   = -1;
        clip_local_addr = NULL;
    }
}

/* Fetch the published id from dobconfig. If it differs from the one
 * we currently have mapped (or we have nothing), unmap the old and
 * map the new. Returns true on success with clip_local_addr valid.
 * Validates the magic header — a stale dangling id is treated as
 * "no clipboard". */
static bool clip_attach_published(void)
{
    char buf[16];
    if (dobconfig_Get("clipboard", buf, sizeof(buf)) != 0) return false;
    if (buf[0] == '\0' || buf[0] == '-') return false;

    int id = atoi(buf);
    if (id < 0) return false;

    if (id == clip_local_id && clip_local_addr) return true;

    clip_unmap_local();

    uint32_t va = 0;
    if (shm_map(id, &va) != 0) return false;

    uint32_t *hdr = (uint32_t *)va;
    if (hdr[0] != CLIP_MAGIC)
    {
        shm_unmap(id);
        return false;
    }

    clip_local_id   = id;
    clip_local_addr = (uint8_t *)va;
    return true;
}

static bool clipboard_active(void)
{
    return clip_attach_published();
}

/* Replace clipboard contents with `n` (service, path) pairs. An empty
 * service means "boot disk / default routing". Returns 0 on success. */
static int clipboard_set_multi(const char *const *services,
                               const char *const *paths,
                               int n, bool is_cut)
{
    if (n <= 0) return -1;

    uint32_t payload_bytes = 0;
    for (int i = 0; i < n; i++)
    {
        const char *svc = services ? services[i] : NULL;
        payload_bytes += (uint32_t)(svc ? strlen(svc) : 0) + 1u;
        payload_bytes += (uint32_t)strlen(paths[i]) + 1u;
    }
    if (payload_bytes < CLIP_MIN_PAYLOAD) payload_bytes = CLIP_MIN_PAYLOAD;

    uint32_t total = CLIP_HEADER_SIZE + payload_bytes;

    /* Drop our reference on the previous region first; if we are the
     * sole holder it will be freed by the kernel when refcount hits 0. */
    clip_unmap_local();

    uint32_t va = 0;
    int id = shm_create(total, &va);
    if (id < 0) return -1;

    uint32_t *hdr = (uint32_t *)va;
    hdr[0] = CLIP_MAGIC;
    hdr[1] = is_cut ? 1u : 0u;
    hdr[2] = (uint32_t)n;
    hdr[3] = payload_bytes;
    hdr[4] = payload_bytes;

    char *p = (char *)(va + CLIP_HEADER_SIZE);
    for (int i = 0; i < n; i++)
    {
        const char *svc  = services ? services[i] : NULL;
        uint32_t    sl   = (uint32_t)(svc ? strlen(svc) : 0);
        uint32_t    pl   = (uint32_t)strlen(paths[i]);
        if (svc && sl) memcpy(p, svc, sl);
        p[sl] = '\0'; p += sl + 1;
        memcpy(p, paths[i], pl);
        p[pl] = '\0'; p += pl + 1;
    }

    clip_local_id   = id;
    clip_local_addr = (uint8_t *)va;

    char idbuf[16];
    snprintf(idbuf, sizeof(idbuf), "%d", id);
    dobconfig_Set("clipboard", idbuf);
    return 0;
}

/* Read clipboard. Fills `services[i]` and `paths[i]` for each entry,
 * sets `*out_cut`, returns the entry count or -1 if no clipboard. */
static int clipboard_get_multi(char services[][64], char paths[][MAX_PATH],
                               int max_n, bool *out_cut)
{
    if (!clip_attach_published()) return -1;

    uint32_t *hdr = (uint32_t *)clip_local_addr;
    if (out_cut) *out_cut = (hdr[1] != 0);

    int count = (int)hdr[2];
    if (count < 0) return -1;

    const char *p   = (const char *)(clip_local_addr + CLIP_HEADER_SIZE);
    const char *end = (const char *)(clip_local_addr + CLIP_HEADER_SIZE + hdr[3]);
    int got = 0;
    for (int i = 0; i < count && p < end && got < max_n; i++)
    {
        size_t sl = 0;
        while (p + sl < end && p[sl] != '\0' && sl < 63) sl++;
        memcpy(services[got], p, sl);
        services[got][sl] = '\0';
        p += strlen(p) + 1;
        if (p >= end) break;

        size_t pl = 0;
        while (p + pl < end && p[pl] != '\0' && pl < (size_t)(MAX_PATH - 1)) pl++;
        memcpy(paths[got], p, pl);
        paths[got][pl] = '\0';
        p += strlen(p) + 1;
        got++;
    }
    return got;
}

/*  *  Bulk-operation lock
 *
 *  The primary DobFiles instance owns the single global lock.
 *  All instances (primary and satellites) acquire it via
 *  bulk_try_begin() before starting any copy/cut/delete/drop.
 *
 *  In-process fast path: the primary calls bulk_local_* directly,
 *  no IPC. Satellites send FILES_BULK_TRY/FILES_BULK_RELEASE to the
 *  primary's registered port.
 *
 *  Liveness: when a new TRY arrives and the lock is held, the
 *  primary posts FILES_BULK_PROBE to the holder. If that fails with
 *  DOB_ERR_DEAD the holder has crashed and the lock is reclaimed.
 */

static bool     g_is_primary     = false;    /* set in event_start */
static uint32_t g_primary_port   = 0;        /* cached; only used by satellites */

/* Mount handoff from main(argv) to event_start(). Populated only when
 * the process was spawned as "--mount <service> <root>"; empty (NULL
 * pointers) means primary mode. */
static const char *g_mount_service = NULL;
static const char *g_mount_root    = NULL;

/* Primary-side lock state. Touched ONLY by the primary. */
static bool     bulk_busy          = false;
static uint32_t bulk_holder_port   = 0;
static char     bulk_holder_label[64];

static bool bulk_local_try(const char *label, uint32_t holder_port)
{
    if (bulk_busy)
    {
        /* Liveness probe. Any post status other than DOB_ERR_DEAD
         * is treated as "still alive" (the holder may be busy in
         * an ipc call and the probe will just sit in its queue). */
        if (bulk_holder_port != 0)
        {
            dob_msg_t probe = {0};
            probe.code = FILES_BULK_PROBE;
            dob_status_t st = dob_ipc_post(bulk_holder_port, &probe);
            if (st != DOB_ERR_DEAD)
                return false;
        }
        /* Reclaim stale lock. */
    }

    bulk_busy        = true;
    bulk_holder_port = holder_port;
    if (label)
    {
        size_t n = strlen(label);
        if (n >= sizeof(bulk_holder_label)) n = sizeof(bulk_holder_label) - 1;
        memcpy(bulk_holder_label, label, n);
        bulk_holder_label[n] = '\0';
    }
    else
    {
        bulk_holder_label[0] = '\0';
    }
    return true;
}

static void bulk_local_release(uint32_t holder_port)
{
    if (bulk_busy && bulk_holder_port == holder_port)
    {
        bulk_busy        = false;
        bulk_holder_port = 0;
        bulk_holder_label[0] = '\0';
    }
}

/* Resolve the primary port for satellite calls. Cached. */
static uint32_t bulk_resolve_primary_port(void)
{
    if (g_primary_port) return g_primary_port;
    g_primary_port = dob_registry_find("DobFiles");
    return g_primary_port;
}

/* Public helper: try to acquire the lock. Returns true on success.
 * On failure the caller should show an error popup with out_label. */
static bool bulk_try_begin(const char *label, char *out_label, size_t cap)
{
    if (g_is_primary)
    {
        bool ok = bulk_local_try(label, dobui_port());
        if (!ok && out_label && cap > 0)
        {
            strncpy(out_label, bulk_holder_label, cap - 1);
            out_label[cap - 1] = '\0';
        }
        return ok;
    }

    uint32_t pp = bulk_resolve_primary_port();
    if (pp == 0) return false;   /* no primary — should not happen */

    dob_msg_t m = {0}, r = {0};
    m.code = FILES_BULK_TRY;
    m.arg0 = dobui_port();
    m.payload = (void *)label;
    m.payload_size = label ? strlen(label) + 1 : 0;
    if (dob_ipc_call(pp, &m, &r) != DOB_OK) return false;

    if (r.arg0 == 0) return true;   /* granted */

    if (out_label && cap > 0)
    {
        const char *bl = r.payload ? (const char *)r.payload : "operazione";
        strncpy(out_label, bl, cap - 1);
        out_label[cap - 1] = '\0';
    }
    return false;
}

static void bulk_release(void)
{
    if (g_is_primary)
    {
        bulk_local_release(dobui_port());
        return;
    }
    uint32_t pp = bulk_resolve_primary_port();
    if (pp == 0) return;
    dob_msg_t m = {0}, r = {0};
    m.code = FILES_BULK_RELEASE;
    m.arg0 = dobui_port();
    dob_ipc_call(pp, &m, &r);
}

/*  *  Chunked file operation state machine
 *
 *  Replaces the old synchronous do_paste / do_delete loops. The
 *  operation is driven one chunk per event_tick(1ms), so the event
 *  loop keeps pumping redraws, progress window updates, and other
 *  events (including drop events on this instance from other drags).
 *
 *  PASTE_COPY  — loops Read/Write chunks until each file is done,
 *                then moves to the next.
 *  PASTE_MOVE  — one dobfs_Rename per file, instantaneous per file.
 *  OP_DELETE   — one dobfs_Unlink per file.
 *
 *  Progress granularity is bytes, computed up-front via dobfs_Stat
 *  on every source path, so the progress bar never jumps proportional
 *  to file count. Speed is byte/s instantaneous (over ~250ms window)
 *  and byte/s average (since op start).
 */

#define OP_NONE         0
#define OP_PASTE_COPY   1
#define OP_PASTE_MOVE   2
#define OP_DELETE       3

#define OP_CHUNK_BYTES  32768
#define OP_REDRAW_MS    200
#define OP_IO_RETRIES   6      /* re-attempts for a chunk that read short
                                * because of a transient USB error, before
                                * declaring the copy failed */

static struct {
    int        kind;
    char       paths[CLIP_MAX_PATHS][MAX_PATH];
    char       svcs [CLIP_MAX_PATHS][64];   /* "" means default routing */
    int        n_paths;
    int        cur_idx;
    int        cur_rfd;
    int        cur_wfd;
    char       cur_name[256];       /* displayed on progress window */
    char       cur_dest[MAX_PATH];  /* for active copy */
    char       dest_dir[MAX_PATH];  /* where files land (for drop, = target dir) */
    uint64_t   total_bytes;
    uint64_t   done_bytes;
    uint64_t   file_size;
    uint64_t   file_done;
    uint32_t   start_ms;
    uint32_t   last_redraw_ms;
    uint64_t   last_sample_bytes;
    uint32_t   last_sample_ms;
    uint32_t   inst_speed;          /* bytes/s instantaneous */
    uint32_t   avg_speed;           /* bytes/s average */
    int        errors;
    bool       active;
    /* Progress window: a real modal child of the main window. */
    dobui_win_t      *pw_win;
    uint32_t          pw_id;
    dob_progressbar_t pw_bar;
    /* If this op was started by an incoming drop, this is the source
     * window's IPC port: op_finish() posts FILES_CMD_DROP_DONE so
     * the source can refresh its listing after a MOVE took files
     * from it. 0 for non-drop ops (paste/delete from the panel). */
    uint32_t   drop_notify_port;
    /* First REJECTED reason string seen during this op, if any. When
     * non-empty at op_finish time, takes precedence over the generic
     * "completata con N errori" popup — the driver's reason is
     * vastly more useful ("Disco pieno", "CD di sola lettura", ...).
     * Captured at the first refusal because that one usually tells
     * the whole story (further refusals will likely be the same
     * reason); subsequent rejections are silent rather than spam
     * the user with N near-identical popups. */
    char       rejection_msg[192];
} op;

/* Forward decls used by the state machine. */
static void op_progress_open(const char *title);
static void op_progress_close(void);
static void op_progress_redraw(void);
static void redraw(void);
static void update_panel(void);
static void read_directory(void);

/* Drag source state */

static bool drag_armed       = false;   /* press landed on a selected item */
static bool drag_in_progress = false;   /* BeginDrag has been called */
static int  drag_press_x     = 0;
static int  drag_press_y     = 0;
#define DRAG_THRESHOLD 4

/* Drag-target hover state.
 *
 * When a drag session is in flight (from this window or any other),
 * the WM keeps delivering etype=6 "drag move" events to whichever
 * window the cursor is currently over.  In DobFiles those arrive
 * through event_mousemove as ordinary moves with `buttons != 0`.
 * If the cursor is hovering a folder entry we want to highlight it
 * — that's the universal "drop here, into this folder" affordance.
 *
 * The state is *target-side*: it tracks where on THIS window's
 * listing a hypothetical drop would land. -1 means "no folder under
 * cursor, drop would land in the current directory".  Updated by
 * event_mousemove during drag-over, and consulted by event_drop to
 * decide the destination path.
 *
 * Cleared on drag-end / drop / explicit cancel so a stale highlight
 * doesn't linger after the session. */
static int  drag_hover_idx = -1;

/*  *  Ribbon toolbar
 *
 *  A row of picturebutton icons under the breadcrumb. Mirrors the
 *  panel-side commands (so users can drive copy/paste/etc. from either
 *  the in-window ribbon or the external panel — they share the same
 *  do_*() backends and the same enable rules).
 *
 *  Layout: 8 buttons, 28x28 each, 4px padding, drawn left-aligned with
 *  a 6px left margin starting at y = BREADCRUMB_H + 2.  The strip is
 *  RIBBON_H tall.
 *
 *  Visibility: in dialog mode the ribbon is hidden (file dialogs only
 *  need the picker, not bulk-edit tools).  ribbon_h_visible() centralises
 *  this so the list-area math has a single source of truth.
 */

#define RIB_COUNT         8
#define RIB_BTN_W         40
#define RIB_BTN_H         40
/* Inter-button padding: sized so the widest label ("Proprietà", 9
 * chars × 8 px = 72 px) overflows the 40 px button by 16 px per side
 * without spilling onto the neighbour.  Padding/2 = 20 > 16 ✓. */
#define RIB_BTN_PAD       40
#define RIB_LEFT_MARGIN   6
#define RIB_BTN_Y_OFFSET  2          /* button top from BREADCRUMB_H */
#define RIB_LABEL_GAP     2          /* gap between button bottom and label top */
#define RIB_ICON_SRC_W    16        /* source pixel-art dimension */
#define RIB_ICON_SRC_H    16
#define RIB_ICON_W        32        /* on-screen icon (2x upscaled) */
#define RIB_ICON_H        32

/* Slot ordering (matches the order requested in the design doc). */
#define RIB_COPY      0
#define RIB_CUT       1
#define RIB_PASTE     2
#define RIB_RENAME    3
#define RIB_MULTI     4
#define RIB_PROPS     5
#define RIB_VIEW      6   /* stub — view-style toggle, disabled for now */
#define RIB_MOUNT     7   /* stub — mount-other-drive, disabled for now */

static dob_picturebutton_t rib_btn[RIB_COUNT];
static bool                rib_ready = false;

/* Per-button labels rendered under each icon.  Order matches the
 * RIB_* slot enum.  Kept short enough that the widest ("Proprietà",
 * 9 chars × 8 px = 72 px) fits within the slot footprint
 * (button 40 px + half-padding 20 on each side = 80 px), see the
 * RIB_BTN_PAD comment for the spacing arithmetic. */
static const char *rib_labels[RIB_COUNT] = {
    "Copia",      /* RIB_COPY   */
    "Taglia",     /* RIB_CUT    */
    "Incolla",    /* RIB_PASTE  */
    "Rinomina",   /* RIB_RENAME */
    "Multi",      /* RIB_MULTI  — latched state shown by button press */
    "Proprietà",  /* RIB_PROPS  */
    "Vista",      /* RIB_VIEW   — stub */
    "Monta",      /* RIB_MOUNT  — stub */
};

/* Sticky multi-select toggle. When ON, every list click toggles the
 * item's selection bit as if Ctrl were held (so the user doesn't have
 * to keep a modifier pressed). When OFF, behaviour is the historical
 * Ctrl/Shift-driven model. Double-click and rect-drag are unaffected
 * either way. The RIB_MULTI button is rendered "pressed" while ON. */
static bool multi_sticky_mode = false;

/* View style — driven by the RIB_VIEW ribbon toggle.
 *   VIEW_LIST  (0): one row per entry, [D] marker + name (historical)
 *   VIEW_ICONS (1): 80x80 grid cell per entry, 32x32 icon + label
 * Toggling resets scroll_offset since the unit changes (rows vs.
 * grid-rows). Selection state is preserved across the flip. */
#define VIEW_LIST   0
#define VIEW_ICONS  1
static int view_style = VIEW_LIST;

/* Data source: what's currently being listed in the window.
 *   SRC_DIR    — a filesystem directory (entries[] = files/dirs)
 *   SRC_MOUNTS — the desktop DAS roster (mount_entries[] = devices)
 *
 * The view-style (LIST vs ICONS) is orthogonal: it only controls the
 * layout for SRC_DIR. SRC_MOUNTS always renders as an icon grid with
 * its own cell geometry (MOUNT_GRID_*).
 *
 * The flip between sources is owned by the Monta ribbon button:
 *   do_mount_view() flips into SRC_MOUNTS and populates mount_entries
 *   do_enter_mount(idx) posts ICON_ACTIVATED to hotplug for entry idx
 *   The driver eventually calls dobfiles_OpenMount() which IPC-hijacks
 *   this very window back into SRC_DIR via FILES_CMD_MOUNT. */
#define SRC_DIR     0
#define SRC_MOUNTS  1
static int data_source = SRC_DIR;

/* SRC_MOUNTS view geometry. The DAS bitmap is 48x48 at natural size
 * (1bpp mask, rasterized to BGRA in read_mounts()). Cell sized to fit
 * the icon with vertical room for a label below; 6 columns fit in the
 * default 720 px window. */
#define MOUNT_GRID_W       112
#define MOUNT_GRID_H       96
#define MOUNT_ICON_MAX_W   48
#define MOUNT_ICON_MAX_H   48
#define MOUNT_ICON_TOP     6
#define MOUNT_LABEL_GAP    4
#define MOUNT_LABEL_MAXC   13
#define MAX_MOUNTS         DEV_LIST_MAX_ENTRIES

typedef struct
{
    uint32_t device_id;
    uint32_t kind;
    char     label[32];
    char     service_name[32];
    /* Pre-rasterized BGRA at natural bitmap size. Pixels outside
     * bm_w/bm_h within the MOUNT_ICON_MAX_* backing array are
     * untouched (unused). Pixel-alpha convention: top byte 0 =
     * transparent, 0xFF | RGB = opaque using the DAS-declared
     * fg color. */
    uint16_t bm_w;
    uint16_t bm_h;
    uint32_t pixels[MOUNT_ICON_MAX_W * MOUNT_ICON_MAX_H];
} mount_entry_t;

static mount_entry_t mount_entries[MAX_MOUNTS];
static int           mount_entry_count = 0;

/* Icon-view grid geometry. Each entry occupies an 80x80 slot; the
 * 16x16 source icon is 2x-upscaled to 32x32 on screen, centered
 * horizontally with the name centered below.  Slot width chosen so
 * 9 columns fit in the default 720 px window with no gap. */
#define GRID_W           80
#define GRID_H           80
#define GRID_ICON_SRC_W  16
#define GRID_ICON_SRC_H  16
#define GRID_ICON_W      32
#define GRID_ICON_H      32
#define GRID_ICON_TOP    8           /* icon top from slot top */
#define GRID_LABEL_GAP   4
#define GRID_LABEL_MAXC  10          /* truncate names longer than this */

/* Upscaled storage for the two grid icons. Same scheme as the ribbon
 * icons; populated once in ribbon_init() so we don't bother with a
 * separate grid_init step. */
static uint32_t grid_icon_folder_x2[GRID_ICON_W * GRID_ICON_H];
static uint32_t grid_icon_file_x2  [GRID_ICON_W * GRID_ICON_H];

/* Pixel-art icons, 16x16, 0x00RRGGBB.  Palette legend used below:
 *   K = 0xFF000000  black outline (opaque black)
 *   R = 0xFFFF2050  red
 *   Y = 0xFFFFE000  yellow (matches selection)
 *   C = 0xFF00FFFF  cyan accent
 *   M = 0xFFFF00FF  magenta accent
 *   W = 0xFFFFFFFF  white highlight
 *   _ = 0x00000000  transparent (button bg shows through)
 *
 * Alpha convention: every BlitBuffer call goes through the texture-
 * pool path now (the old inline fast path was retired due to a
 * cross-window scratch race).  The driver's pixel-alpha rule is
 *   top byte == 0  →  skip       (transparent)
 *   top byte != 0  →  write s_p  (opaque, color is the low 24 bits)
 * so opaque colors carry an explicit 0xFF in the alpha slot, and
 * 0x00000000 acts as the transparent sentinel.  This is the inverse
 * of the legacy inline-blit convention (where 0xFF000000 meant
 * transparent); under the new pipeline that would be opaque black,
 * which is what produced the "icons are black squares" symptom on
 * earlier iterations. */
#define ICK 0xFFFFFFFFu
#define ICR 0xFFFF2050u
#define ICY 0xFFFFE000u
#define ICC 0xFF00FFFFu
#define ICM 0xFFFF00FFu
#define ICW 0xFFFFFFFFu
#define IC_ 0x00000000u

/* Copy: two overlapping sheets */
static const uint32_t ico_copy[16*16] = {
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,ICK,ICW,ICW,ICW,ICW,ICW,ICW,ICK,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,ICK,ICW,ICW,ICW,ICW,ICW,ICW,ICK,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,ICK,ICW,ICW,ICW,ICW,ICW,ICW,ICK,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,ICK,ICW,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,IC_,IC_,
    IC_,IC_,ICK,ICW,ICK,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICK,IC_,IC_,
    IC_,IC_,ICK,ICW,ICK,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICK,IC_,IC_,
    IC_,IC_,ICK,ICK,ICK,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICK,IC_,IC_,
    IC_,IC_,IC_,IC_,ICK,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICK,IC_,IC_,
    IC_,IC_,IC_,IC_,ICK,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICK,IC_,IC_,
    IC_,IC_,IC_,IC_,ICK,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICK,IC_,IC_,
    IC_,IC_,IC_,IC_,ICK,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICK,IC_,IC_,
    IC_,IC_,IC_,IC_,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
};

/* Cut: scissors silhouette */
static const uint32_t ico_cut[16*16] = {
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,ICK,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,ICK,ICK,IC_,IC_,
    ICK,ICR,ICR,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,ICK,ICR,ICR,ICK,IC_,
    ICK,ICR,ICR,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,ICK,ICR,ICR,ICK,IC_,
    IC_,ICK,ICK,IC_,ICK,IC_,IC_,IC_,IC_,IC_,ICK,IC_,ICK,ICK,IC_,IC_,
    IC_,IC_,IC_,IC_,ICK,ICK,IC_,IC_,IC_,ICK,ICK,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,ICK,ICK,IC_,ICK,ICK,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,ICK,ICK,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,ICK,ICK,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,ICK,ICK,IC_,ICK,ICK,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,ICK,ICK,IC_,IC_,IC_,ICK,ICK,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,ICK,ICK,IC_,IC_,IC_,IC_,IC_,ICK,ICK,IC_,IC_,IC_,IC_,
    IC_,IC_,ICK,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,ICK,ICK,IC_,IC_,IC_,
    IC_,ICK,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,ICK,ICK,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
};

/* Paste: clipboard with paper sticking out */
static const uint32_t ico_paste[16*16] = {
    IC_,IC_,IC_,IC_,IC_,ICK,ICK,ICK,ICK,ICK,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,ICK,ICW,ICW,ICW,ICW,ICW,ICK,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,ICK,ICK,ICW,ICW,ICW,ICW,ICW,ICK,ICK,IC_,IC_,IC_,IC_,
    IC_,IC_,ICK,ICY,ICY,ICK,ICK,ICK,ICK,ICK,ICY,ICY,ICK,IC_,IC_,IC_,
    IC_,IC_,ICK,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICK,IC_,IC_,IC_,
    IC_,IC_,ICK,ICY,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICY,ICK,IC_,IC_,IC_,
    IC_,IC_,ICK,ICY,ICW,ICK,ICK,ICK,ICK,ICK,ICW,ICY,ICK,IC_,IC_,IC_,
    IC_,IC_,ICK,ICY,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICY,ICK,IC_,IC_,IC_,
    IC_,IC_,ICK,ICY,ICW,ICK,ICK,ICK,ICK,ICK,ICW,ICY,ICK,IC_,IC_,IC_,
    IC_,IC_,ICK,ICY,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICY,ICK,IC_,IC_,IC_,
    IC_,IC_,ICK,ICY,ICW,ICK,ICK,ICK,ICK,ICK,ICW,ICY,ICK,IC_,IC_,IC_,
    IC_,IC_,ICK,ICY,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICY,ICK,IC_,IC_,IC_,
    IC_,IC_,ICK,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICK,IC_,IC_,IC_,
    IC_,IC_,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
};

/* Rename: pencil */
static const uint32_t ico_rename[16*16] = {
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,ICK,ICK,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,ICK,ICY,ICY,ICK,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,ICK,ICY,ICY,ICY,ICY,ICK,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,ICK,ICY,ICY,ICY,ICY,ICK,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,ICK,ICY,ICY,ICY,ICY,ICK,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,ICK,ICY,ICY,ICY,ICY,ICK,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,ICK,ICY,ICY,ICY,ICY,ICK,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,ICK,ICY,ICY,ICY,ICY,ICK,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,ICK,ICY,ICY,ICY,ICY,ICK,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,ICK,ICY,ICY,ICY,ICY,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,ICK,ICW,ICW,ICY,ICY,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,ICK,ICW,ICW,ICW,ICW,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,ICK,ICK,ICW,ICW,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,ICK,ICK,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
};

/* Multi-select toggle: two stacked checkboxes, one ticked */
static const uint32_t ico_multi[16*16] = {
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,ICK,ICK,ICK,ICK,ICK,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,ICK,IC_,IC_,IC_,IC_,ICK,IC_,IC_,IC_,IC_,IC_,IC_,ICY,IC_,IC_,
    IC_,ICK,IC_,IC_,IC_,IC_,ICK,IC_,IC_,IC_,IC_,IC_,ICY,IC_,IC_,IC_,
    IC_,ICK,IC_,IC_,IC_,IC_,ICK,IC_,IC_,IC_,IC_,ICY,IC_,IC_,IC_,IC_,
    IC_,ICK,IC_,IC_,IC_,IC_,ICK,IC_,IC_,ICY,ICY,IC_,IC_,IC_,IC_,IC_,
    IC_,ICK,ICK,ICK,ICK,ICK,ICK,IC_,ICY,ICY,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,ICK,ICK,ICK,ICK,ICK,ICK,IC_,IC_,IC_,IC_,IC_,IC_,ICY,IC_,IC_,
    IC_,ICK,IC_,IC_,IC_,IC_,ICK,IC_,IC_,IC_,IC_,IC_,ICY,IC_,IC_,IC_,
    IC_,ICK,IC_,IC_,IC_,IC_,ICK,IC_,IC_,IC_,IC_,ICY,IC_,IC_,IC_,IC_,
    IC_,ICK,IC_,IC_,IC_,IC_,ICK,IC_,IC_,ICY,ICY,IC_,IC_,IC_,IC_,IC_,
    IC_,ICK,IC_,IC_,IC_,IC_,ICK,IC_,ICY,ICY,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,ICK,ICK,ICK,ICK,ICK,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
};

/* Properties: circled "i" */
static const uint32_t ico_props[16*16] = {
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,ICK,ICK,ICK,ICK,ICK,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,ICK,ICK,IC_,IC_,IC_,IC_,IC_,ICK,ICK,IC_,IC_,IC_,IC_,
    IC_,IC_,ICK,IC_,IC_,IC_,ICC,ICC,IC_,IC_,IC_,IC_,ICK,IC_,IC_,IC_,
    IC_,ICK,IC_,IC_,IC_,IC_,ICC,ICC,IC_,IC_,IC_,IC_,IC_,ICK,IC_,IC_,
    IC_,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,ICK,IC_,IC_,
    ICK,IC_,IC_,IC_,IC_,IC_,ICC,ICC,IC_,IC_,IC_,IC_,IC_,IC_,ICK,IC_,
    ICK,IC_,IC_,IC_,IC_,IC_,ICC,ICC,IC_,IC_,IC_,IC_,IC_,IC_,ICK,IC_,
    ICK,IC_,IC_,IC_,IC_,IC_,ICC,ICC,IC_,IC_,IC_,IC_,IC_,IC_,ICK,IC_,
    ICK,IC_,IC_,IC_,IC_,IC_,ICC,ICC,IC_,IC_,IC_,IC_,IC_,IC_,ICK,IC_,
    IC_,ICK,IC_,IC_,IC_,IC_,ICC,ICC,IC_,IC_,IC_,IC_,IC_,ICK,IC_,IC_,
    IC_,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,ICK,IC_,IC_,
    IC_,IC_,ICK,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,ICK,IC_,IC_,IC_,
    IC_,IC_,IC_,ICK,ICK,IC_,IC_,IC_,IC_,IC_,ICK,ICK,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,ICK,ICK,ICK,ICK,ICK,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
};

/* View: grid of 2x2 cells (stub icon — feature TBD) */
static const uint32_t ico_view[16*16] = {
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,IC_,IC_,
    IC_,ICK,ICC,ICC,ICC,ICC,ICK,ICC,ICC,ICC,ICC,ICC,ICC,ICK,IC_,IC_,
    IC_,ICK,ICC,ICC,ICC,ICC,ICK,ICC,ICC,ICC,ICC,ICC,ICC,ICK,IC_,IC_,
    IC_,ICK,ICC,ICC,ICC,ICC,ICK,ICC,ICC,ICC,ICC,ICC,ICC,ICK,IC_,IC_,
    IC_,ICK,ICC,ICC,ICC,ICC,ICK,ICC,ICC,ICC,ICC,ICC,ICC,ICK,IC_,IC_,
    IC_,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,IC_,IC_,
    IC_,ICK,ICC,ICC,ICC,ICC,ICK,ICC,ICC,ICC,ICC,ICC,ICC,ICK,IC_,IC_,
    IC_,ICK,ICC,ICC,ICC,ICC,ICK,ICC,ICC,ICC,ICC,ICC,ICC,ICK,IC_,IC_,
    IC_,ICK,ICC,ICC,ICC,ICC,ICK,ICC,ICC,ICC,ICC,ICC,ICC,ICK,IC_,IC_,
    IC_,ICK,ICC,ICC,ICC,ICC,ICK,ICC,ICC,ICC,ICC,ICC,ICC,ICK,IC_,IC_,
    IC_,ICK,ICC,ICC,ICC,ICC,ICK,ICC,ICC,ICC,ICC,ICC,ICC,ICK,IC_,IC_,
    IC_,ICK,ICC,ICC,ICC,ICC,ICK,ICC,ICC,ICC,ICC,ICC,ICC,ICK,IC_,IC_,
    IC_,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
};

/* Mount: disk with an upward arrow (mounting an external device) */
static const uint32_t ico_mount[16*16] = {
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,ICM,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,ICM,ICM,ICM,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,ICM,ICM,ICM,ICM,ICM,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,ICM,ICM,ICM,ICM,ICM,ICM,ICM,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,ICM,ICM,ICM,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,ICM,ICM,ICM,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,ICM,ICM,ICM,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,IC_,IC_,
    IC_,ICK,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICK,IC_,IC_,
    IC_,ICK,ICC,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICC,ICK,IC_,IC_,
    IC_,ICK,ICC,ICK,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICK,ICC,ICK,IC_,IC_,
    IC_,ICK,ICC,ICK,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICK,ICC,ICK,IC_,IC_,
    IC_,ICK,ICC,ICK,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICK,ICC,ICK,IC_,IC_,
    IC_,ICK,ICC,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICC,ICK,IC_,IC_,
    IC_,ICK,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICK,IC_,IC_,
    IC_,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,ICK,IC_,IC_,
};

/*  *  Folder/file icons used by the icon-view grid (RIB_VIEW toggle).
 *
 *  Both 16x16 and 2x-upscaled at runtime to 32x32 on screen, same
 *  pipeline as the ribbon icons.  Folder is the classic manila tab
 *  shape; file is a sheet with the top-right corner folded over.
 */

/* Folder icon: cyan tab + body outline, yellow fill.  Outline in
 * cyan rather than black so it stays visible against the dark-blue
 * list background. */
static const uint32_t ico_grid_folder[16*16] = {
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,ICC,ICC,ICC,ICC,ICC,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,ICC,ICY,ICY,ICY,ICY,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,IC_,IC_,
    IC_,ICC,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICC,IC_,IC_,
    IC_,ICC,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICC,IC_,IC_,
    IC_,ICC,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICC,IC_,IC_,
    IC_,ICC,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICC,IC_,IC_,
    IC_,ICC,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICC,IC_,IC_,
    IC_,ICC,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICC,IC_,IC_,
    IC_,ICC,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICC,IC_,IC_,
    IC_,ICC,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICC,IC_,IC_,
    IC_,ICC,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICY,ICC,IC_,IC_,
    IC_,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
};

/* File icon: cyan outline + white sheet, with a prominent folded
 * top-right corner.  The fold is a stair of 3 cyan pixels stepping
 * diagonally (1px right + 1px down per step) so the corner reads
 * as a clear triangle even at 32×32 on screen.  Three horizontal
 * cyan stripes inside the page suggest text content. */
static const uint32_t ico_grid_file[16*16] = {
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,ICC,ICW,ICW,ICW,ICW,ICW,ICW,ICC,ICC,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,ICC,ICW,ICW,ICW,ICW,ICW,ICW,ICC,ICC,ICC,IC_,IC_,IC_,
    IC_,IC_,IC_,ICC,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICC,IC_,IC_,IC_,
    IC_,IC_,IC_,ICC,ICW,ICC,ICC,ICC,ICC,ICC,ICW,ICW,ICC,IC_,IC_,IC_,
    IC_,IC_,IC_,ICC,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICC,IC_,IC_,IC_,
    IC_,IC_,IC_,ICC,ICW,ICC,ICC,ICC,ICC,ICC,ICC,ICW,ICC,IC_,IC_,IC_,
    IC_,IC_,IC_,ICC,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICC,IC_,IC_,IC_,
    IC_,IC_,IC_,ICC,ICW,ICC,ICC,ICC,ICC,ICC,ICC,ICW,ICC,IC_,IC_,IC_,
    IC_,IC_,IC_,ICC,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICC,IC_,IC_,IC_,
    IC_,IC_,IC_,ICC,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICW,ICC,IC_,IC_,IC_,
    IC_,IC_,IC_,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,ICC,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
    IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,IC_,
};

#undef ICK
#undef ICR
#undef ICY
#undef ICC
#undef ICM
#undef ICW
#undef IC_

/* Whether the ribbon strip is currently part of the layout.  The list
 * top is BREADCRUMB_H + this value; all hit-testing and drawing use
 * this single accessor to stay consistent. */
/* Whether the ribbon strip is currently part of the layout. The list
 * top is BREADCRUMB_H + this value; all hit-testing and drawing use
 * this single accessor to stay consistent.
 *
 * The ribbon is always present, even in dialog (Open/Save) mode: the
 * file picker is exactly when the user wants Vista (list/icons) and
 * Monta (jump to floppy/CD). The other ribbon buttons are
 * force-disabled and visually skipped in dialog mode so the ribbon
 * stays minimal. */
static int ribbon_h_visible(void)
{
    return RIBBON_H;
}

/* Backing storage for the 2x-upscaled icons handed to the
 * picturebutton widget. Sized at startup from the 16x16 sources in
 * ribbon_init(); kept static so the pointers stay valid for the
 * widget's lifetime (dobpbtn_SetImage stores the pointer, doesn't
 * copy). 8 * 32*32 * 4B = 32 KB — modest. */
static uint32_t rib_icon_x2[RIB_COUNT][RIB_ICON_W * RIB_ICON_H];

/* Nearest-neighbour 2x upscale. The pixel-art sources are 16x16 and
 * BlitBuffer doesn't scale, so we'd otherwise have a tiny icon
 * lost in the centre of a 40x40 button. */
static void rib_upscale_2x(const uint32_t *src, uint32_t *dst)
{
    for (int y = 0; y < RIB_ICON_SRC_H; y++)
    {
        for (int x = 0; x < RIB_ICON_SRC_W; x++)
        {
            uint32_t c = src[y * RIB_ICON_SRC_W + x];
            int dx = x * 2, dy = y * 2;
            dst[(dy    ) * RIB_ICON_W + dx    ] = c;
            dst[(dy    ) * RIB_ICON_W + dx + 1] = c;
            dst[(dy + 1) * RIB_ICON_W + dx    ] = c;
            dst[(dy + 1) * RIB_ICON_W + dx + 1] = c;
        }
    }
}

static void ribbon_init(void)
{
    if (rib_ready) return;

    int x = RIB_LEFT_MARGIN;
    /* Top-aligned now that the label sits under each button — the
     * old vertical centering would have pushed the icon down into
     * the label row. */
    int y = BREADCRUMB_H + RIB_BTN_Y_OFFSET;

    for (int i = 0; i < RIB_COUNT; i++)
    {
        dobpbtn_Init(&rib_btn[i], win_id, x, y, RIB_BTN_W, RIB_BTN_H);
        /* Don't anchor: ribbon lives in the top-left and shouldn't
         * follow window resize.  The list area below is the only
         * thing that needs to grow. */
        x += RIB_BTN_W + RIB_BTN_PAD;
    }

    /* Order matches the RIB_* slot enum so the lookup-by-index below
     * stays straightforward. */
    const uint32_t *sources[RIB_COUNT] = {
        ico_copy,   /* RIB_COPY   */
        ico_cut,    /* RIB_CUT    */
        ico_paste,  /* RIB_PASTE  */
        ico_rename, /* RIB_RENAME */
        ico_multi,  /* RIB_MULTI  */
        ico_props,  /* RIB_PROPS  */
        ico_view,   /* RIB_VIEW   */
        ico_mount,  /* RIB_MOUNT  */
    };
    for (int i = 0; i < RIB_COUNT; i++)
    {
        rib_upscale_2x(sources[i], rib_icon_x2[i]);
        dobpbtn_SetImage(&rib_btn[i], rib_icon_x2[i],
                         RIB_ICON_W, RIB_ICON_H);
    }

    /* Upscale the icon-view grid icons too. They use the same 16→32
     * pixel-art scheme so the nearest-neighbour upscaler works
     * unchanged. */
    rib_upscale_2x(ico_grid_folder, grid_icon_folder_x2);
    rib_upscale_2x(ico_grid_file,   grid_icon_file_x2);

    rib_ready = true;
}

/* Refresh the enabled-state of each ribbon button based on current
 * selection and clipboard.  Called from update_panel() so both UIs
 * (ribbon + external panel) stay coherent. */
static void ribbon_update_state(void)
{
    if (!rib_ready) return;

    bool has_clip = clipboard_active();
    bool has_sel  = sel_count >= 1;
    bool one_sel  = sel_count == 1;
    bool in_mount = (data_source == SRC_MOUNTS);
    /* "minimal ribbon" contexts: only Vista + Monta are useful.
     * Dialog mode is a file picker — no bulk edit / clipboard ops
     * make sense. SRC_MOUNTS picker is the same logic. */
    bool minimal  = dialog_mode || in_mount;

    dobpbtn_SetEnabled(&rib_btn[RIB_COPY],   has_sel && !minimal);
    dobpbtn_SetEnabled(&rib_btn[RIB_CUT],    has_sel && !minimal);
    dobpbtn_SetEnabled(&rib_btn[RIB_PASTE],  has_clip && !minimal);
    dobpbtn_SetEnabled(&rib_btn[RIB_RENAME], one_sel && !minimal);
    dobpbtn_SetEnabled(&rib_btn[RIB_MULTI],  !minimal);
    dobpbtn_SetEnabled(&rib_btn[RIB_PROPS],  one_sel && !minimal);
    dobpbtn_SetEnabled(&rib_btn[RIB_VIEW],   true);
    dobpbtn_SetEnabled(&rib_btn[RIB_MOUNT],  true);

    rib_btn[RIB_MULTI].pressed = multi_sticky_mode && !minimal;
    rib_btn[RIB_VIEW].pressed  = (view_style == VIEW_ICONS);
    rib_btn[RIB_MOUNT].pressed = in_mount;

    /* Reflow the button x-coordinates so that in minimal mode (only
     * Vista + Monta visible) those two sit on the left edge of the
     * ribbon instead of where they'd land at the end of the full
     * 8-button row. Without this the picker shows an empty ribbon
     * with the two buttons floating far right — looks broken. */
    int x = RIB_LEFT_MARGIN;
    for (int i = 0; i < RIB_COUNT; i++)
    {
        if (minimal && i != RIB_VIEW && i != RIB_MOUNT) continue;
        rib_btn[i].x = x;
        x += RIB_BTN_W + RIB_BTN_PAD;
    }
}

static void ribbon_draw(void)
{
    if (!rib_ready) return;

    /* Strip background — covers from below the breadcrumb to the top
     * of the list area, full window width. */
    dobui_FillRect(win_id, 0, BREADCRUMB_H, win_w, RIBBON_H, COL_RIBBON_BG);

    /* Minimal-ribbon contexts (dialog picker, SRC_MOUNTS): only
     * Vista and Monta render. The others would be grey-disabled
     * forever — better to hide them entirely so the picker stays
     * clean. */
    bool minimal = dialog_mode || (data_source == SRC_MOUNTS);

    for (int i = 0; i < RIB_COUNT; i++)
    {
        if (minimal && i != RIB_VIEW && i != RIB_MOUNT) continue;
        dobpbtn_Draw(&rib_btn[i]);
    }

    /* Per-button labels, centered under each button.  Color tracks
     * the button's enabled state so a greyed-out icon doesn't sit
     * under a fully-bright label — visually inconsistent.  Disabled
     * dim colour is a desaturated khaki of the regular yellow text
     * so the palette still reads as one. */
    int label_y = BREADCRUMB_H + RIB_BTN_Y_OFFSET + RIB_BTN_H + RIB_LABEL_GAP;
    for (int i = 0; i < RIB_COUNT; i++)
    {
        if (minimal && i != RIB_VIEW && i != RIB_MOUNT) continue;
        const char *txt = rib_labels[i];
        int text_w = (int)strlen(txt) * FONT_W;
        int text_x = rib_btn[i].x + (RIB_BTN_W - text_w) / 2;
        uint32_t fg = rib_btn[i].enabled ? COL_TEXT : DOBUI_DISABLED;
        dobui_DrawText(win_id, text_x, label_y, txt, fg, COL_RIBBON_BG);
    }
}

/* Forward decls so the ribbon click handler can reach the do_*() ops.
 * The functions themselves are defined further down. */
static void do_copy(bool cut);
static void do_paste(void);
static void do_rename(void);
static void do_properties(void);
static void do_mount_view(void);
static void do_enter_mount(int idx);

/* Returns true if the click was consumed by a ribbon button. */
static bool ribbon_handle_click(int lx, int ly)
{
    if (!rib_ready)  return false;

    /* Quick reject: outside the strip. */
    if (ly < BREADCRUMB_H || ly >= BREADCRUMB_H + RIBBON_H)
        return false;

    /* Match the hide rule in ribbon_draw so a click in the dead
     * space of a hidden button doesn't accidentally trigger it. */
    bool minimal = dialog_mode || (data_source == SRC_MOUNTS);

    for (int i = 0; i < RIB_COUNT; i++)
    {
        if (minimal && i != RIB_VIEW && i != RIB_MOUNT) continue;
        if (!dobpbtn_OnClick(&rib_btn[i], lx, ly))
            continue;
        if (!rib_btn[i].clicked) continue;
        rib_btn[i].clicked = false;
        dobpbtn_OnRelease(&rib_btn[i]);

        /* Disabled buttons swallow the click silently. */
        if (!rib_btn[i].enabled) return true;

        switch (i)
        {
            case RIB_COPY:   do_copy(false); break;
            case RIB_CUT:    do_copy(true);  break;
            case RIB_PASTE:  do_paste();     break;
            case RIB_RENAME: do_rename();    break;
            case RIB_MULTI:
                multi_sticky_mode = !multi_sticky_mode;
                update_panel();   /* refresh ribbon press-state */
                redraw();
                break;
            case RIB_PROPS:  do_properties(); break;
            case RIB_VIEW:
                view_style = (view_style == VIEW_LIST) ? VIEW_ICONS : VIEW_LIST;
                /* User-initiated change: persist it so the choice
                 * survives a restart.  settingsd stores the ENUM option
                 * ("Elenco"/"Icone") declared at startup.  The ribbon is
                 * only present in the real browser (not the file
                 * dialog), so view.style is always declared here. */
                writeSetting("view.style",
                             view_style == VIEW_ICONS ? "Icone" : "Elenco");
                /* Reset scroll: the unit changes (rows in list mode,
                 * grid-rows in icon mode) so an old offset would
                 * point at a meaningless position. */
                scroll_offset = 0;
                update_panel();   /* refresh view-button press state */
                redraw();
                break;
            case RIB_MOUNT:
                do_mount_view();
                break;
        }
        return true;
    }
    return false;
}

/* Directory reading */

static void read_directory(void)
{
    entry_count = 0;
    selected = -1;
    scroll_offset = 0;
    sel_clear();
    rect_active = false;

    dobfs_dirent_t raw[MAX_ENTRIES];
    uint32_t count = 0;

    if (dobfs_List(current_path, raw, MAX_ENTRIES, &count) < 0 || count == 0)
        return;

    for (uint32_t i = 0; i < count; i++)
    {
        strncpy(entries[entry_count].name, raw[i].name, 255);
        entries[entry_count].is_dir = (raw[i].type == FS_TYPE_DIR);
        entries[entry_count].size = raw[i].size;
        entry_count++;
    }

    /* Sort: directories first, then alphabetical (insertion sort) */
    for (int i = 1; i < entry_count; i++)
    {
        dir_entry_t tmp = entries[i];
        int j = i - 1;
        while (j >= 0)
        {
            bool should_move = false;
            if (tmp.is_dir && !entries[j].is_dir)
                should_move = true;
            else if (tmp.is_dir == entries[j].is_dir &&
                     strcmp(tmp.name, entries[j].name) < 0)
                should_move = true;

            if (!should_move) break;
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = tmp;
    }
}

/* WM IPC port helper. Used by read_mounts() to query GUI_LIST_DEVICES
 * and (via do_enter_mount) to *post* ICON_ACTIVATED at hotplug. Note:
 * find() is non-blocking; wait(0) would block 30s per kernel cap. */
static uint32_t dobui_server_port(void)
{
    return dob_registry_find("dobinterface");
}

/* SRC_MOUNTS: fetch the desktop device roster from the WM.
 *
 * One synchronous IPC: GUI_LIST_DEVICES → reply is gui_device_list_t
 * header + N entries. For each entry we copy the metadata and
 * rasterize the 1bpp mask into BGRA so the draw path can blit via
 * the same texture-pool route as the file/folder icons (alpha
 * convention: top byte 0 = transparent, 0xFF | RGB = opaque).
 *
 * Returns 0 on success (mount_entry_count populated, possibly to 0),
 * -1 on IPC failure. */
static int read_mounts(void)
{
    mount_entry_count = 0;

    uint32_t wm_port = dobui_server_port();
    if (!wm_port) return -1;

    dob_msg_t req = {0}, rep = {0};
    req.code = GUI_LIST_DEVICES;
    if (dob_ipc_call(wm_port, &req, &rep) != DOB_OK) return -1;
    if (!rep.payload || rep.payload_size < sizeof(gui_device_list_t))
        return 0;

    const gui_device_list_t *hdr = (const gui_device_list_t *)rep.payload;
    uint32_t n = hdr->count;
    if (n > MAX_MOUNTS) n = MAX_MOUNTS;

    const gui_device_list_entry_t *src =
        (const gui_device_list_entry_t *)(hdr + 1);

    /* Defensive: trust the header but cap to what's actually in the
     * payload buffer. */
    uint32_t bytes_for_n = (uint32_t)(sizeof(*hdr) + (size_t)n * sizeof(*src));
    if (bytes_for_n > rep.payload_size)
        n = (uint32_t)((rep.payload_size - sizeof(*hdr)) / sizeof(*src));

    for (uint32_t i = 0; i < n; i++)
    {
        const gui_device_list_entry_t *s = &src[i];
        mount_entry_t *d = &mount_entries[mount_entry_count++];
        d->device_id = s->device_id;
        d->kind      = s->kind;
        memcpy(d->label,        s->label,        sizeof(d->label));
        memcpy(d->service_name, s->service_name, sizeof(d->service_name));
        d->label[sizeof(d->label) - 1] = '\0';
        d->service_name[sizeof(d->service_name) - 1] = '\0';

        uint16_t bw = s->bitmap.width;
        uint16_t bh = s->bitmap.height;
        if (bw > MOUNT_ICON_MAX_W) bw = MOUNT_ICON_MAX_W;
        if (bh > MOUNT_ICON_MAX_H) bh = MOUNT_ICON_MAX_H;
        d->bm_w = bw;
        d->bm_h = bh;

        uint32_t fg = 0xFF000000u
                    | ((uint32_t)s->bitmap.fg_r << 16)
                    | ((uint32_t)s->bitmap.fg_g <<  8)
                    | ((uint32_t)s->bitmap.fg_b      );
        uint16_t stride = (uint16_t)((s->bitmap.width + 7) / 8);

        for (uint16_t y = 0; y < bh; y++)
        {
            for (uint16_t x = 0; x < bw; x++)
            {
                uint8_t byte = s->bitmap.data[y * stride + (x >> 3)];
                uint8_t bit  = (byte >> (7 - (x & 7))) & 1;
                d->pixels[y * MOUNT_ICON_MAX_W + x] = bit ? fg : 0;
            }
        }
    }
    return 0;
}

/* Drawing */

/* Breadcrumb hit zones */
#define MAX_BREAD_ITEMS 16
static struct { int x, w; char path[MAX_PATH]; } bread_items[MAX_BREAD_ITEMS];
static int bread_count = 0;

static void draw_breadcrumb(void)
{
    bread_count = 0;
    dobui_FillRect(win_id, 0, 0, win_w, BREADCRUMB_H, COL_BREADCRUMB);

    int x = 4;
    const char *p = current_path;

    /* Root "/" button */
    dobui_FillRect(win_id, x, 3, 20, BREADCRUMB_H - 6, COL_BREAD_BTN);
    dobui_DrawText(win_id, x + 6, 6, "/", COL_BREAD_TEXT, COL_BREAD_BTN);
    bread_items[0].x = x;
    bread_items[0].w = 20;
    strncpy(bread_items[0].path, "/", MAX_PATH);
    bread_count = 1;
    x += 24;

    if (*p == '/') p++;

    /* Build cumulative path for each component */
    char cumul[MAX_PATH];
    cumul[0] = '\0';

    while (*p && bread_count < MAX_BREAD_ITEMS)
    {
        const char *slash = p;
        while (*slash && *slash != '/') slash++;

        int clen = (int)(slash - p);
        if (clen > 0 && clen < 64)
        {
            char comp[64];
            memcpy(comp, p, (uint32_t)clen);
            comp[clen] = '\0';

            /* Build cumulative path: /DATA/Documents/... */
            int cl = (int)strlen(cumul);
            cumul[cl] = '/';
            memcpy(cumul + cl + 1, comp, (uint32_t)clen);
            cumul[cl + 1 + clen] = '\0';

            int bw = clen * FONT_W + 8;
            if (x + bw > win_w - 10) break;  /* No room */

            dobui_FillRect(win_id, x, 3, bw, BREADCRUMB_H - 6, COL_BREAD_BTN);
            dobui_DrawText(win_id, x + 4, 6, comp, COL_BREAD_TEXT, COL_BREAD_BTN);

            bread_items[bread_count].x = x;
            bread_items[bread_count].w = bw;
            strncpy(bread_items[bread_count].path, cumul, MAX_PATH);
            bread_count++;
            x += bw + 4;
        }

        p = slash;
        if (*p == '/') p++;
    }
}

/*  *  View geometry helpers
 *
 *  Both modes treat the listing area as a uniform array of rectangles.
 *  view_entry_rect() returns the (x, y, w, h) of the i-th entry in
 *  window coordinates, *or* returns false if the entry is scrolled
 *  off-screen. view_idx_at() does the inverse: given window-local
 *  coords inside the listing area, returns the entry index or -1.
 *
 *  Splitting these out of draw / hit-test means rect_recompute_selection
 *  and friends are mode-agnostic — they ask the helper for rects and
 *  intersect them with the drag box. */

static int view_list_top(void)
{
    return BREADCRUMB_H + ribbon_h_visible();
}

/* Item count for the active data source. Centralised so all the
 * view helpers reach into the right array. */
static int view_item_count(void)
{
    return (data_source == SRC_MOUNTS) ? mount_entry_count : entry_count;
}

/* Columns per row in icon mode. Fixed-width slots; the rightmost
 * partial column (window width not a multiple of GRID_W) is unused
 * since hit-testing into a clipped slot would be confusing. */
static int view_cols(void)
{
    int slot_w = (data_source == SRC_MOUNTS) ? MOUNT_GRID_W : GRID_W;
    int c = win_w / slot_w;
    return c < 1 ? 1 : c;
}

/* Returns true and fills *x/*y/*w/*h if entry idx is currently on
 * screen. SRC_MOUNTS is always icon-style with its own cell geometry;
 * SRC_DIR follows view_style. */
static bool view_entry_rect(int idx, int *x, int *y, int *w, int *h)
{
    int count = view_item_count();
    if (idx < 0 || idx >= count) return false;
    int list_top = view_list_top();
    int list_h   = win_h - list_top;

    if (data_source == SRC_MOUNTS)
    {
        int cols = view_cols();
        int row  = idx / cols;
        int col  = idx % cols;
        int local_row = row - scroll_offset;
        int rows_visible = list_h / MOUNT_GRID_H;
        if (local_row < 0 || local_row >= rows_visible) return false;
        *x = col * MOUNT_GRID_W;
        *y = list_top + local_row * MOUNT_GRID_H;
        *w = MOUNT_GRID_W;
        *h = MOUNT_GRID_H;
        return true;
    }

    if (view_style == VIEW_ICONS)
    {
        int cols = view_cols();
        int row  = idx / cols;
        int col  = idx % cols;
        int local_row = row - scroll_offset;
        int rows_visible = list_h / GRID_H;
        if (local_row < 0 || local_row >= rows_visible) return false;
        *x = col * GRID_W;
        *y = list_top + local_row * GRID_H;
        *w = GRID_W;
        *h = GRID_H;
    }
    else
    {
        int local_row = idx - scroll_offset;
        int rows_visible = list_h / ITEM_H;
        if (local_row < 0 || local_row >= rows_visible) return false;
        *x = 0;
        *y = list_top + local_row * ITEM_H;
        *w = win_w;
        *h = ITEM_H;
    }
    return true;
}

/* Given window coords (lx, ly) inside the listing area, return the
 * entry index under the cursor, or -1. Callers must already have
 * verified ly >= view_list_top() (which means the click is not in
 * breadcrumb / ribbon). */
static int view_idx_at(int lx, int ly)
{
    int list_top = view_list_top();
    if (ly < list_top) return -1;
    int local_y = ly - list_top;
    int count = view_item_count();

    if (data_source == SRC_MOUNTS)
    {
        int cols = view_cols();
        int col  = lx / MOUNT_GRID_W;
        if (col < 0 || col >= cols) return -1;
        int row  = local_y / MOUNT_GRID_H + scroll_offset;
        int idx  = row * cols + col;
        if (idx < 0 || idx >= count) return -1;
        return idx;
    }

    if (view_style == VIEW_ICONS)
    {
        int cols = view_cols();
        int col  = lx / GRID_W;
        if (col < 0 || col >= cols) return -1;
        int row  = local_y / GRID_H + scroll_offset;
        int idx  = row * cols + col;
        if (idx < 0 || idx >= count) return -1;
        return idx;
    }
    else
    {
        int idx = local_y / ITEM_H + scroll_offset;
        if (idx < 0 || idx >= count) return -1;
        return idx;
    }
}

/* Maximum useful scroll_offset for the current view & entry count. */
static int view_max_scroll(void)
{
    int list_top = view_list_top();
    int list_h   = win_h - list_top;
    int count = view_item_count();

    if (data_source == SRC_MOUNTS)
    {
        int cols = view_cols();
        int rows_total = (count + cols - 1) / cols;
        int rows_vis   = list_h / MOUNT_GRID_H;
        int m = rows_total - rows_vis;
        return m < 0 ? 0 : m;
    }
    if (view_style == VIEW_ICONS)
    {
        int cols = view_cols();
        int rows_total = (count + cols - 1) / cols;
        int rows_vis   = list_h / GRID_H;
        int m = rows_total - rows_vis;
        return m < 0 ? 0 : m;
    }
    else
    {
        int rows_vis = list_h / ITEM_H;
        int m = count - rows_vis;
        return m < 0 ? 0 : m;
    }
}

/* Truncate a filename so it fits inside a grid label slot. Names
 * up to GRID_LABEL_MAXC characters render verbatim; longer ones are
 * cut to (MAXC - 3) characters with a trailing "..." so the user
 * sees the prefix and knows there's more. Plain ASCII ellipsis to
 * stay portable to whatever Latin-1 vs UTF-8 the font happens to
 * handle; "…" would risk a fallback glyph. */
static void grid_display_name(const char *src, char *dst, size_t cap)
{
    size_t n = strlen(src);
    if (n <= GRID_LABEL_MAXC)
    {
        size_t copy = (n < cap - 1) ? n : (cap - 1);
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    else
    {
        size_t keep = GRID_LABEL_MAXC - 3;
        if (keep + 4 > cap) keep = (cap >= 4) ? cap - 4 : 0;
        memcpy(dst, src, keep);
        dst[keep    ] = '.';
        dst[keep + 1] = '.';
        dst[keep + 2] = '.';
        dst[keep + 3] = '\0';
    }
}

/*  *  Drawing modes  */

static void draw_view_list(void)
{
    int list_y = view_list_top();
    int list_h = win_h - list_y;
    int visible = list_h / ITEM_H;

    dobui_FillRect(win_id, 0, list_y, win_w, list_h, COL_BG);

    if (entry_count == 0)
    {
        dobui_DrawText(win_id, 20, list_y + 20, "(vuoto)", COL_TEXT, COL_BG);
    }

    for (int i = 0; i < visible && (i + scroll_offset) < entry_count; i++)
    {
        int idx = i + scroll_offset;
        dir_entry_t *e = &entries[idx];
        int y = list_y + i * ITEM_H;

        /* Background — drop-hover wins over selection wins over zebra.
         * Hover uses cyan (COL_BG_ALT's complement, max contrast on
         * blue) so it's unambiguous even when the target is also
         * selected. */
        uint32_t bg;
        if (idx == drag_hover_idx)
            bg = 0xFF00FFFFu;        /* cyan drop highlight */
        else if (sel_get(idx))
            bg = COL_BG_SEL;
        else
            bg = (idx % 2 == 0) ? COL_BG : COL_BG_ALT;

        uint32_t fg = (idx == drag_hover_idx) ? COL_TEXT_SEL
                    : (sel_get(idx) ? COL_TEXT_SEL : COL_TEXT);

        dobui_FillRect(win_id, 0, y, win_w, ITEM_H, bg);

        /* Folder/file icon indicator */
        if (e->is_dir)
            dobui_DrawText(win_id, 4, y + 4, "[D]", COL_FOLDER, bg);
        else
            dobui_DrawText(win_id, 4, y + 4, "   ", fg, bg);

        /* Name */
        dobui_DrawText(win_id, 32, y + 4, e->name, fg, bg);
    }
}

static void draw_view_icons(void)
{
    int list_y = view_list_top();
    int list_h = win_h - list_y;
    int cols   = view_cols();
    int rows_visible = list_h / GRID_H;

    dobui_FillRect(win_id, 0, list_y, win_w, list_h, COL_BG);

    if (entry_count == 0)
    {
        dobui_DrawText(win_id, 20, list_y + 20, "(vuoto)", COL_TEXT, COL_BG);
        return;
    }

    for (int row_off = 0; row_off < rows_visible; row_off++)
    {
        int row = row_off + scroll_offset;
        for (int col = 0; col < cols; col++)
        {
            int idx = row * cols + col;
            if (idx >= entry_count) goto done_grid;     /* nothing past last entry */

            dir_entry_t *e = &entries[idx];
            int sx = col * GRID_W;
            int sy = list_y + row_off * GRID_H;

            /* Same hover-over-selection rule as the list view. */
            uint32_t bg;
            if (idx == drag_hover_idx)
                bg = 0xFF00FFFFu;     /* cyan drop highlight */
            else if (sel_get(idx))
                bg = COL_BG_SEL;
            else
                bg = COL_BG;

            uint32_t fg = (idx == drag_hover_idx) ? COL_TEXT_SEL
                        : (sel_get(idx) ? COL_TEXT_SEL : COL_TEXT);

            dobui_FillRect(win_id, sx, sy, GRID_W, GRID_H, bg);

            const uint32_t *icon = e->is_dir
                ? grid_icon_folder_x2 : grid_icon_file_x2;
            int icon_x = sx + (GRID_W - GRID_ICON_W) / 2;
            int icon_y = sy + GRID_ICON_TOP;
            dobui_BlitBuffer(win_id, icon_x, icon_y, icon,
                             GRID_ICON_W, GRID_ICON_H);

            char disp[GRID_LABEL_MAXC + 4];
            grid_display_name(e->name, disp, sizeof(disp));
            int tw = (int)strlen(disp) * FONT_W;
            int tx = sx + (GRID_W - tw) / 2;
            int ty = sy + GRID_ICON_TOP + GRID_ICON_H + GRID_LABEL_GAP;
            dobui_DrawText(win_id, tx, ty, disp, fg, bg);
        }
    }
done_grid:

    /* Sole-selection overlay: when exactly one entry is highlighted,
     * spill its full (untruncated) name into the neighbour cells'
     * label strips — same convention as Windows / macOS icon views,
     * which lets you read long filenames without resorting to
     * tooltips.  Skipped if the name fits in the cell anyway. */
    if (sel_count == 1)
    {
        int sidx = sel_first();
        int ex, ey, ew, eh;
        if (sidx >= 0 && view_entry_rect(sidx, &ex, &ey, &ew, &eh))
        {
            const char *full = entries[sidx].name;
            int text_w = (int)strlen(full) * FONT_W;
            if (text_w > ew - 4)        /* only spill if it actually overflows */
            {
                int text_x = ex + (ew - text_w) / 2;
                int text_y = ey + GRID_ICON_TOP + GRID_ICON_H + GRID_LABEL_GAP;
                /* Yellow background bar matching the cell's selection
                 * highlight, 2 px padding on each side so the text
                 * doesn't graze the next neighbour's icon. */
                int bar_x = text_x - 2;
                int bar_w = text_w + 4;
                if (bar_x < 0) { bar_w += bar_x; bar_x = 0; }
                if (bar_x + bar_w > win_w) bar_w = win_w - bar_x;
                dobui_FillRect(win_id, bar_x, text_y, bar_w, FONT_H, COL_BG_SEL);
                dobui_DrawText(win_id, text_x, text_y, full,
                               COL_TEXT_SEL, COL_BG_SEL);
            }
        }
    }
}

/* Mode-agnostic selection-rectangle overlay. Drawn after either
 * view renderer so it floats on top. */
static void draw_rect_overlay(void)
{
    if (!rect_active) return;
    int list_top = view_list_top();
    int rx0 = rect_x0 < rect_x1 ? rect_x0 : rect_x1;
    int rx1 = rect_x0 < rect_x1 ? rect_x1 : rect_x0;
    int ry0 = rect_y0 < rect_y1 ? rect_y0 : rect_y1;
    int ry1 = rect_y0 < rect_y1 ? rect_y1 : rect_y0;
    if (ry0 < list_top) ry0 = list_top;
    int rw = rx1 - rx0;
    int rh = ry1 - ry0;
    if (rw <= 0 || rh <= 0) return;

    dobui_FillRect(win_id, rx0,         ry0,         rw, 1,  COL_BG_SEL);
    dobui_FillRect(win_id, rx0,         ry1 - 1,     rw, 1,  COL_BG_SEL);
    dobui_FillRect(win_id, rx0,         ry0,         1,  rh, COL_BG_SEL);
    dobui_FillRect(win_id, rx1 - 1,     ry0,         1,  rh, COL_BG_SEL);
}

/* SRC_MOUNTS renderer. Mountable devices use a chunkier 112x96 cell
 * to fit the natural 48x48 DAS bitmap with room for a wider label
 * below. Icons come from mount_entry_t.pixels[] which read_mounts()
 * rasterized from the 1bpp DAS mask. */
static void draw_view_mounts(void)
{
    int list_y = view_list_top();
    int list_h = win_h - list_y;
    int cols   = view_cols();
    int rows_visible = list_h / MOUNT_GRID_H;

    dobui_FillRect(win_id, 0, list_y, win_w, list_h, COL_BG);

    if (mount_entry_count == 0)
    {
        dobui_DrawText(win_id, 20, list_y + 20,
                       "Nessun dispositivo disponibile.",
                       COL_TEXT, COL_BG);
        return;
    }

    for (int row_off = 0; row_off < rows_visible; row_off++)
    {
        int row = row_off + scroll_offset;
        for (int col = 0; col < cols; col++)
        {
            int idx = row * cols + col;
            if (idx >= mount_entry_count) goto done;

            mount_entry_t *m = &mount_entries[idx];
            int sx = col * MOUNT_GRID_W;
            int sy = list_y + row_off * MOUNT_GRID_H;

            uint32_t bg = sel_get(idx) ? COL_BG_SEL : COL_BG;
            uint32_t fg = sel_get(idx) ? COL_TEXT_SEL : COL_TEXT;

            dobui_FillRect(win_id, sx, sy, MOUNT_GRID_W, MOUNT_GRID_H, bg);

            /* Icon at natural size, centred in cell. The pixels[]
             * array is MOUNT_ICON_MAX_W wide in memory; if bm_w is
             * smaller we compact to a tightly-packed scratch buffer
             * before blitting so the stride matches src_w (which
             * BlitBuffer assumes). */
            int icon_x = sx + (MOUNT_GRID_W - m->bm_w) / 2;
            int icon_y = sy + MOUNT_ICON_TOP;
            if (m->bm_w == MOUNT_ICON_MAX_W)
            {
                dobui_BlitBuffer(win_id, icon_x, icon_y, m->pixels,
                                 m->bm_w, m->bm_h);
            }
            else
            {
                static uint32_t scratch[MOUNT_ICON_MAX_W * MOUNT_ICON_MAX_H];
                for (int y = 0; y < m->bm_h; y++)
                    memcpy(&scratch[y * m->bm_w],
                           &m->pixels[y * MOUNT_ICON_MAX_W],
                           (size_t)m->bm_w * 4u);
                dobui_BlitBuffer(win_id, icon_x, icon_y, scratch,
                                 m->bm_w, m->bm_h);
            }

            /* Label, truncated to fit. */
            char disp[MOUNT_LABEL_MAXC + 4];
            size_t n = strlen(m->label);
            if (n <= MOUNT_LABEL_MAXC)
            {
                memcpy(disp, m->label, n);
                disp[n] = '\0';
            }
            else
            {
                size_t keep = MOUNT_LABEL_MAXC - 3;
                memcpy(disp, m->label, keep);
                disp[keep] = '.'; disp[keep+1] = '.'; disp[keep+2] = '.';
                disp[keep+3] = '\0';
            }
            int tw = (int)strlen(disp) * FONT_W;
            int tx = sx + (MOUNT_GRID_W - tw) / 2;
            int ty = sy + MOUNT_ICON_TOP + MOUNT_ICON_MAX_H + MOUNT_LABEL_GAP;
            dobui_DrawText(win_id, tx, ty, disp, fg, bg);
        }
    }
done:
    ;
}

static void draw_file_list(void)
{
    if (data_source == SRC_MOUNTS)        draw_view_mounts();
    else if (view_style == VIEW_ICONS)    draw_view_icons();
    else                                  draw_view_list();
    /* No rect-select overlay in SRC_MOUNTS — no multi-select there. */
    if (data_source != SRC_MOUNTS) draw_rect_overlay();
}

static void redraw(void)
{
    draw_breadcrumb();
    ribbon_draw();
    draw_file_list();
    dobui_Invalidate(win_id);
}

/* Panel commands */

static void update_panel(void)
{
    /* SRC_MOUNTS: no panel commands make sense (you can't copy a
     * device, etc.). The only meaningful action — "back to the
     * filesystem" — is already on the ribbon (RIB_MOUNT toggle). */
    if (data_source == SRC_MOUNTS)
    {
        dobui_SetPanelCommands(win_id, "");
        ribbon_update_state();
        return;
    }

    /* In dialog mode, use dialog-specific panel commands */
    if (dialog_mode)
    {
        if (dialog_type == 10) /* Open */
        {
            if (selected >= 0 && !entries[selected].is_dir)
                dobui_SetPanelCommands(win_id, "Seleziona\nAnnulla");
            else
                dobui_SetPanelCommands(win_id, "Annulla");
        }
        else /* Save */
        {
            dobui_SetPanelCommands(win_id, "Salva qui\nAnnulla");
        }
        return;
    }

    /* Compose the panel command list by appending into a single
     * buffer. Keeps the new "Proprietà" / "Multi On/Off" entries in a
     * single decision tree instead of cascading overrides.
     *
     * Rules:
     *   - Single-target items (Apri / Rinomina / Proprietà) appear
     *     only when exactly one entry is selected.
     *   - Bulk items (Copia / Taglia / Elimina) need >=1.
     *   - Incolla appears whenever the clipboard is populated.
     *   - Create-in-folder items show only when nothing is selected.
     *   - Multi toggle is always available; its label reflects state. */
    bool has_clip = clipboard_active();
    bool has_sel  = sel_count >= 1;
    bool one_sel  = sel_count == 1;

    char menu[256];
    int  n = 0;
    #define APPEND(s) do { int _l = (int)strlen(s); \
        if (n + _l + 1 < (int)sizeof(menu)) { \
            if (n) menu[n++] = '\n'; \
            memcpy(menu + n, s, _l); n += _l; menu[n] = '\0'; } } while (0)

    if (one_sel)
    {
        APPEND("Apri");
        APPEND("Rinomina");
        APPEND("Proprietà");
    }
    if (has_sel)
    {
        APPEND("Copia");
        APPEND("Taglia");
        APPEND("Elimina");
    }
    if (has_clip)
        APPEND("Incolla");
    if (!has_sel)
    {
        APPEND("Crea file");
        APPEND("Crea cartella");
    }
    APPEND(multi_sticky_mode ? "Multi Off" : "Multi On");

    #undef APPEND
    dobui_SetPanelCommands(win_id, menu);

    /* Sync the ribbon's enable/press state in lockstep. */
    ribbon_update_state();
}

/* Path helpers */

static void build_full_path(char *out, const char *name)
{
    int plen = (int)strlen(current_path);
    memcpy(out, current_path, (uint32_t)plen);
    if (plen > 0 && current_path[plen - 1] != '/')
        out[plen++] = '/';
    int nlen = (int)strlen(name);
    memcpy(out + plen, name, (uint32_t)nlen);
    out[plen + nlen] = '\0';
}

/* Build the dialog's reply path. On a mounted/removable volume whose
 * plain "/file" paths a *default* caller's dobfs stub cannot reach —
 * anything other than the boot FS ("DobFileSystem") and the floppy
 * (/u<n>) route, i.e. a partition or USB stick mounted as dobfs_<token>,
 * or a CD — qualify it as "service:/path" so the calling process opens
 * it on the correct backend. The boot FS and floppy resolve correctly
 * from a plain path in every process, so they stay unqualified. */
static void dialog_build_result(const char *name)
{
    const char *svc = dobfs_get_service();
    bool qualify = svc && *svc
        && strcmp(svc, "DobFileSystem") != 0
        && strcmp(svc, "floppy") != 0;

    if (qualify)
    {
        char tmp[MAX_PATH];
        build_full_path(tmp, name);
        snprintf(dialog_result_path, sizeof(dialog_result_path),
                 "%s:%s", svc, tmp);
    }
    else
    {
        build_full_path(dialog_result_path, name);
    }
}

static void navigate_to(const char *path)
{
    strncpy(current_path, path, MAX_PATH - 1);
    current_path[MAX_PATH - 1] = '\0';
    read_directory();
    update_panel();
    redraw();
}

static void navigate_into(const char *name)
{
    char full[MAX_PATH];
    build_full_path(full, name);
    navigate_to(full);
}

/* Get file extension (pointer into name, not a copy) */
static const char *get_extension(const char *name)
{
    const char *dot = NULL;
    while (*name)
    {
        if (*name == '.') dot = name;
        name++;
    }
    return dot;
}

/* File operations */

/* Forward declarations — dialog functions defined later */
static void dialog_complete_open(void);
static void dialog_complete_save(void);
static void dialog_cancel(void);
static void dialog_update_panel(void);

/* Check if a name exists in the current directory */
static bool name_exists(const char *name)
{
    dobfs_stat_t st;
    char full[MAX_PATH];
    build_full_path(full, name);
    return (dobfs_Stat(full, &st) == 0);
}

/* Ask user for a name, looping until unique or cancelled */
static int ask_unique_name(const char *title, const char *prompt,
                           const char *default_name, char *out, uint32_t out_size)
{
    char buf[256];
    strncpy(buf, default_name, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    for (;;)
    {
        int result = dobpopup_InputBox(title, prompt, buf, out, out_size);
        if (result != 0 || out[0] == '\0')
            return -1;

        if (!name_exists(out))
            return 0;

        dobpopup_Warning("Nome duplicato",
            "Esiste gia' un elemento con questo nome.");
        strncpy(buf, out, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
    }
}

/* Check if the current directory contains a manifest marking modules as drivers.
 * Looks for manifest.dob or *.manifest and checks for driver=true or type=driver. */
static bool check_dir_is_driver(const char *dir)
{
    /* Try manifest.dob first */
    char mpath[MAX_PATH];
    snprintf(mpath, sizeof(mpath), "%s/manifest.dob", dir);

    int fd = dobfs_Open(mpath, FS_READ);
    if (fd < 0)
    {
        /* Scan for *.manifest */
        dobfs_dirent_t ents[32];
        uint32_t cnt = 0;
        if (dobfs_List(dir, ents, 32, &cnt) < 0) return false;
        bool found = false;
        for (uint32_t i = 0; i < cnt; i++)
        {
            int nlen = (int)strlen(ents[i].name);
            if (nlen > 9 && strcmp(ents[i].name + nlen - 9, ".manifest") == 0)
            {
                snprintf(mpath, sizeof(mpath), "%s/%s", dir, ents[i].name);
                found = true;
                break;
            }
        }
        if (!found) return false;
        fd = dobfs_Open(mpath, FS_READ);
        if (fd < 0) return false;
    }

    char buf[512];
    int rd = dobfs_Read(fd, buf, sizeof(buf) - 1);
    dobfs_Close(fd);
    if (rd <= 0) return false;
    buf[rd] = '\0';

    /* Parse key=value lines for driver=true or type=driver */
    char *p = buf;
    while (*p)
    {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '\0') break;

        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol;
        *eol = '\0';

        char *eq = strchr(p, '=');
        if (eq)
        {
            *eq = '\0';
            char *key = p;
            char *val = eq + 1;
            if ((strcmp(key, "driver") == 0 &&
                 (strcmp(val, "true") == 0 || strcmp(val, "1") == 0)) ||
                (strcmp(key, "type") == 0 && strcmp(val, "driver") == 0))
            {
                return true;
            }
        }

        if (saved == '\0') break;
        p = eol + 1;
    }
    return false;
}

/* Toggle SRC_DIR ⇄ SRC_MOUNTS. Same button (RIB_MOUNT) acts both
 * ways — visually communicated by the latched press-state in
 * ribbon_update_state. Entering refreshes the mount roster; leaving
 * re-lists whatever directory we were on before. */
static void do_mount_view(void)
{
    if (data_source == SRC_MOUNTS)
    {
        data_source = SRC_DIR;
        scroll_offset = 0;
        sel_clear();
        selected = -1;
        read_directory();
        update_panel();
        redraw();
        return;
    }

    read_mounts();
    data_source = SRC_MOUNTS;
    scroll_offset = 0;
    sel_clear();
    selected = -1;
    rect_active = false;
    update_panel();
    redraw();
}

/* User clicked a device in SRC_MOUNTS. Post ICON_ACTIVATED to hotplug
 * — same message the WM sends after a desktop double-click. Hotplug
 * runs the matched DAS action {} block (spawn driver + probe). When
 * the driver succeeds, it calls dobfiles_OpenMount() which in turn
 * IPC-calls our FILES_CMD_MOUNT handler, which hijacks this very
 * window onto the new mount.
 *
 * No gate, no timeout, no awaiting state. The FILES_CMD_MOUNT
 * handler accepts unconditionally (it always has — that's the v60
 * unified "dirotto puro" model), so whenever OpenMount happens to
 * fire, the hijack just lands. If the DAS does something other than
 * mount (popup, format wizard, ...) the OpenMount never fires and
 * the window stays in SRC_MOUNTS — perfectly fine, the user can
 * click again or leave the view. */
static void do_enter_mount(int idx)
{
    if (idx < 0 || idx >= mount_entry_count) return;
    mount_entry_t *m = &mount_entries[idx];

    uint32_t hotplug_port = dob_registry_find("hotplug");
    if (!hotplug_port)
    {
        dobpopup_Error("Monta",
                       "Impossibile contattare il gestore dispositivi.");
        return;
    }

    /* Forward our own dobui_port() inside the icon_activated_t. It
     * rides the request through hotplug → DAS → driver, and the
     * driver eventually feeds it to dobfiles_OpenMount() which uses
     * it to route the FILES_CMD_MOUNT call directly back to us
     * instead of guessing. Desktop double-clicks on the same icon
     * leave the field at 0 (set explicitly by send_icon_activated
     * in the WM) and so always spawn a fresh window. */
    icon_activated_t req = {
        .device_id          = m->device_id,
        .hijack_target_port = dobui_port(),
    };
    dob_msg_t msg = {0};
    msg.code = ICON_ACTIVATED;
    msg.payload = &req;
    msg.payload_size = sizeof(req);
    dob_ipc_post(hotplug_port, &msg);
}

static void do_open(void)
{
    if (selected < 0) return;
    dir_entry_t *e = &entries[selected];

    if (e->is_dir)
    {
        navigate_into(e->name);
        if (dialog_mode) dialog_update_panel();
        return;
    }

    /* In dialog mode, double-click a file = complete the dialog */
    if (dialog_mode)
    {
        if (dialog_type == 10)
            dialog_complete_open();
        else
            dialog_complete_save();
        return;
    }

    const char *ext = get_extension(e->name);

    /* .mdl is MainDOB's native executable format.
     * Handled directly — no association lookup needed.
     * This is OS behavior, like double-clicking an .exe on Windows.
     * If the directory's manifest marks it as a driver, ask DobInterface
     * to spawn and promote it. */
    if (ext && (strcmp(ext, ".mdl") == 0 || strcmp(ext, ".MDL") == 0))
    {
        char full_path[MAX_PATH];
        build_full_path(full_path, e->name);
        if (check_dir_is_driver(current_path))
            dobui_SpawnDriver(full_path, NULL);
        else
            spawn_file(full_path, NULL);
        return;
    }

    if (!ext)
    {
        dobpopup_Info("Apri", "Tipo di file sconosciuto.");
        return;
    }

    /* Look up file association for this extension. Extensions are
     * registered lowercase in the config DB, but filenames coming
     * from FAT12 (floppy) are uppercase, and even on FAT32 users may
     * create files with mixed case. Normalise before lookup. */
    char ext_lc[32];
    {
        uint32_t i = 0;
        for (; ext[i] && i < sizeof(ext_lc) - 1; i++)
        {
            char c = ext[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            ext_lc[i] = c;
        }
        ext_lc[i] = '\0';
    }

    char prog[256];
    if (dobconfig_GetAssoc(ext_lc, prog, sizeof(prog)) == 0)
    {
        char full[MAX_PATH];
        build_full_path(full, e->name);

        /* Mount propagation: if this DobFiles window is a satellite
         * attached to a removable backend (archive, floppy, ...),
         * publish that backend's service name so the program we're
         * about to spawn inherits the same view. Without this, the
         * spawned program's SVC_DEFAULT stays on the boot disk and
         * any relative path in `full` resolves to nothing.
         *
         * The primary DobFiles (attached to the boot disk) does NOT
         * set the key — primary spawns must route to DobFileSystem
         * as usual. */
        const char *cur = dobfs_get_service();
        if (cur && strcmp(cur, "DobFileSystem") != 0)
            dobconfig_Set("spawn_default_service", cur);
        else
            dobconfig_Set("spawn_default_service", "");

        /* If the associated program's bubble declares driver=true (or
         * type=driver) in its manifest, promote at spawn time — this is
         * how privileged tools like DobInstaller get the clearance they
         * need to write under /SYSTEM/. Without this, sandbox_check
         * denies every write and the user sees "Estrazione fallita". */
        char prog_dir[256];
        strncpy(prog_dir, prog, sizeof(prog_dir) - 1);
        prog_dir[sizeof(prog_dir) - 1] = '\0';
        char *last_slash = NULL;
        for (char *p = prog_dir; *p; p++)
            if (*p == '/') last_slash = p;
        if (last_slash) *last_slash = '\0';

        const char *argv[] = { full, NULL };
        if (prog_dir[0] && check_dir_is_driver(prog_dir))
            dobui_SpawnDriver(prog, argv);
        else
            spawn_file(prog, argv);
        return;
    }

    dobpopup_Warning("Apri", "Nessun programma associato a questo tipo di file.");
}

static void do_copy(bool cut)
{
    if (sel_count == 0) return;

    /* Collect all selected entries as full paths */
    static char paths_buf[CLIP_MAX_PATHS][MAX_PATH];
    const char *ptrs[CLIP_MAX_PATHS];
    const char *svcs[CLIP_MAX_PATHS];
    const char *svc = dobfs_get_service();
    /* Boot disk is the routable default — leave service blank so any
     * receiver still routes by path prefix. */
    bool blank_svc = (!svc || strcmp(svc, "DobFileSystem") == 0);

    int n = 0;
    for (int i = 0; i < entry_count && n < CLIP_MAX_PATHS; i++)
    {
        if (!sel_get(i)) continue;
        build_full_path(paths_buf[n], entries[i].name);
        ptrs[n] = paths_buf[n];
        svcs[n] = blank_svc ? "" : svc;
        n++;
    }
    if (n == 0) return;

    if (clipboard_set_multi(svcs, ptrs, n, cut) != 0)
    {
        dobpopup_Error("Clipboard", "Impossibile aggiornare la clipboard.");
        return;
    }

    /* Cut clears the selection so the user gets visual feedback that
     * the items have been "consumed" into the clipboard. We also
     * re-list the directory in case anything changed externally. */
    if (cut)
    {
        read_directory();
    }
    update_panel();
    redraw();
}

/*  *  Progress window
 *
 *  A secondary, non-interactive window shown alongside the main
 *  browser for the duration of a bulk op. The user can still see
 *  and even click on the main window (the lock prevents new ops).
 *  We flip g_fb contexts via Save/Restore to draw on either.
 */

#define PW_W    320
#define PW_H    120

#define PW_COL_BG       DOBUI_INSET
#define PW_COL_TEXT     DOBUI_TEXT
#define PW_COL_FAINT    DOBUI_DISABLED

static void op_progress_format_speed(char *buf, size_t cap, uint32_t bps)
{
    if (bps >= 1024 * 1024)
        snprintf(buf, cap, "%u.%u MB/s",
                 bps / (1024 * 1024),
                 (bps % (1024 * 1024)) * 10 / (1024 * 1024));
    else if (bps >= 1024)
        snprintf(buf, cap, "%u.%u KB/s",
                 bps / 1024, (bps % 1024) * 10 / 1024);
    else
        snprintf(buf, cap, "%u B/s", bps);
}

/* Width reserved on the right of the bar for the "100%" percentage
 * label drawn by dobpb_Draw.  4 chars × 8 px + 6 px gap + a little
 * breathing room. */
#define PW_PCT_LABEL_W  44

/* Repaint the static chrome of the progress window — background,
 * border, and the progress bar.  Called from the window's on_start
 * and on every op_progress_redraw (the cmdlist resets each Invalidate,
 * so the chrome must be re-emitted every frame). */
static void op_progress_paint_chrome(int w, int h)
{
    dobui_FillRect(op.pw_id, 0, 0, w, h, PW_COL_BG);
    /* 4-side thin border */
    dobui_FillRect(op.pw_id, 0,     0,     w, 1, PW_COL_FAINT);
    dobui_FillRect(op.pw_id, 0,     h - 1, w, 1, PW_COL_FAINT);
    dobui_FillRect(op.pw_id, 0,     0,     1, h, PW_COL_FAINT);
    dobui_FillRect(op.pw_id, w - 1, 0,     1, h, PW_COL_FAINT);

    /* Progress bar: 12 px margin on the left, a reserved label strip
     * on the right so dobpb's "100%" text fits fully on screen.  The
     * bar itself occupies what's left. */
    int bar_w = w - 24 - PW_PCT_LABEL_W;
    if (bar_w < 40) bar_w = 40;
    dobpb_Init(&op.pw_bar, op.pw_id, 12, 56, bar_w, 14);
    dobpb_SetMax(&op.pw_bar, 10000);
    op.pw_bar.show_text = true;
    dobpb_Draw(&op.pw_bar);
}

/* The progress window is created modal and fixed-size; its only
 * callbacks are on_start (paint the chrome) and on_close (ignored --
 * the op owns the window's lifetime, the user can't dismiss it). */
static void op_pw_on_start(dobui_win_t *w)
{
    op.pw_id = dobui_win_id(w);
    op_progress_paint_chrome(PW_W, PW_H);   /* Invalidate done by dialog_open */
}

static void op_pw_on_close(dobui_win_t *w) { (void)w; }   /* ignore */

static const dobui_win_vtbl_t op_pw_vtbl = {
    .on_start = op_pw_on_start,
    .on_close = op_pw_on_close,
};

static void op_progress_open(const char *title)
{
    /* Real modal child of the main window: the WM blocks the browser
     * for the duration of the op and rings the attention sound on a
     * click into it.  Fixed size -> NORESIZE | NOMAXIMIZE.  on_start
     * paints the chrome; op_progress_redraw (from event_tick) fills in
     * the live progress.  If creation fails the op still runs headless
     * (pw_id stays 0 and every redraw is a no-op). */
    op.pw_win = dobui_dialog_open(dobui_primary(), title, PW_W, PW_H,
                                  &op_pw_vtbl, NULL, /*modal=*/true,
                                  DOBUI_WIN_NORESIZE | DOBUI_WIN_NOMAXIMIZE);
    op.pw_id  = dobui_win_id(op.pw_win);
}

static void op_progress_redraw(void)
{
    if (op.pw_id == 0) return;

    /* Direct the draws at the progress window (we're usually called
     * from event_tick, where the active context isn't ours). */
    dobui_SetActiveWindow(op.pw_id);

    int cur_w = PW_W, cur_h = PW_H;

    /* Repaint chrome every frame — cmdlist resets on each Invalidate,
     * so border/bar would disappear from the second frame onwards. */
    op_progress_paint_chrome(cur_w, cur_h);

    /* Clear the dynamic lines (filename + speeds) then redraw. */
    dobui_FillRect(op.pw_id, 12, 14,  cur_w - 24, 18, PW_COL_BG);
    dobui_FillRect(op.pw_id, 12, 78,  cur_w - 24, 18, PW_COL_BG);
    dobui_FillRect(op.pw_id, 12, 96,  cur_w - 24, 18, PW_COL_BG);

    /* Filename — truncate with ellipsis if it can't fit the line in
     * the current width (8px/char, 24px horizontal margin). */
    int max_chars = (cur_w - 24) / 8;
    if (max_chars < 8) max_chars = 8;
    if (max_chars > 60) max_chars = 60;
    char line[64];
    const char *nm = op.cur_name;
    size_t nl = strlen(nm);
    if ((int)nl > max_chars)
    {
        int keep = max_chars - 3;
        if (keep < 1) keep = 1;
        snprintf(line, sizeof(line), "...%s", nm + (nl - keep));
    }
    else
    {
        snprintf(line, sizeof(line), "%s", nm);
    }
    dobui_DrawText(op.pw_id, 12, 14, line, PW_COL_TEXT, PW_COL_BG);

    /* Progress bar value */
    int pct = 0;
    if (op.total_bytes > 0)
    {
        /* 32-bit safe percentage on a 0..10000 scale.
         * done32 * 10000 overflows past ~420 KB, so two branches. */
        uint32_t done32  = (uint32_t)op.done_bytes;
        uint32_t total32 = (uint32_t)op.total_bytes;
        if (done32 <= 0xFFFFFFFFu / 10000u)
            pct = (int)((done32 * 10000u) / total32);
        else
            pct = (int)(done32 / (total32 / 10000u + 1u));
        if (pct > 10000) pct = 10000;
    }
    dobpb_SetValue(&op.pw_bar, pct);
    dobpb_Draw(&op.pw_bar);

    /* Speed lines */
    char sbuf[32];
    char lbuf[64];
    op_progress_format_speed(sbuf, sizeof(sbuf), op.inst_speed);
    snprintf(lbuf, sizeof(lbuf), "Velocita': %s", sbuf);
    dobui_DrawText(op.pw_id, 12, 78, lbuf, PW_COL_TEXT, PW_COL_BG);

    op_progress_format_speed(sbuf, sizeof(sbuf), op.avg_speed);
    snprintf(lbuf, sizeof(lbuf), "Media:     %s", sbuf);
    dobui_DrawText(op.pw_id, 12, 96, lbuf, PW_COL_TEXT, PW_COL_BG);

    dobui_Invalidate(op.pw_id);
}

static void op_progress_close(void)
{
    if (!op.pw_win) { op.pw_id = 0; return; }
    dobui_win_t *w = op.pw_win;
    op.pw_win = NULL;
    op.pw_id  = 0;
    /* Destroying the window unblocks the browser; the WM refocuses it. */
    dobui_win_close(w);
}

/* Chunked op core */

static void op_close_current_files(void)
{
    if (op.cur_rfd >= 0) { dobfs_Close(op.cur_rfd); op.cur_rfd = -1; }
    if (op.cur_wfd >= 0) { dobfs_Close(op.cur_wfd); op.cur_wfd = -1; }
}

static void op_finish(void)
{
    op_close_current_files();
    op_progress_close();

    bulk_release();
    dobui_set_tick(0);   /* stop the chunk timer */

    int errors = op.errors;
    uint32_t notify_port = op.drop_notify_port;
    char rejection_msg[sizeof(op.rejection_msg)];
    memcpy(rejection_msg, op.rejection_msg, sizeof(rejection_msg));

    /* Reset state */
    memset(&op, 0, sizeof(op));
    op.cur_rfd = -1;
    op.cur_wfd = -1;

    /* If this op started as a drop from another DobFiles window,
     * tell it we're done so it can re-read its directory now that
     * MOVE deletes are committed. Post-only; failure is harmless. */
    if (notify_port)
    {
        dob_msg_t m = {0};
        m.code = FILES_CMD_DROP_DONE;
        dob_ipc_post(notify_port, &m);
    }

    /* The progress window is gone; point draws back at the browser
     * before refreshing its listing. */
    dobui_SetActiveWindow(win_id);
    read_directory();
    update_panel();
    redraw();

    if (errors > 0)
    {
        if (rejection_msg[0] != '\0')
        {
            /* The driver gave us a specific reason — surface it as a
             * modal error popup. Far more useful than the generic
             * count, which is shown only when no REJECTED arrived. */
            dobpopup_Error("File", rejection_msg);
        }
        else
        {
            char m[128];
            snprintf(m, sizeof(m), "Operazione completata con %d errori.", errors);
            dobpopup_Warning("File", m);
        }
    }
}

/* Extract filename from a full path. Returns pointer inside src. */
static const char *op_basename(const char *src)
{
    const char *nm = src;
    for (const char *p = src; *p; p++)
        if (*p == '/') nm = p + 1;
    return nm;
}

/* Build destination path in op.dest_dir from a source path. */
static void op_build_dest(const char *src_full, char *out, size_t cap)
{
    const char *nm = op_basename(src_full);
    size_t dl = strlen(op.dest_dir);
    bool need_slash = (dl > 0 && op.dest_dir[dl - 1] != '/');
    snprintf(out, cap, "%s%s%s", op.dest_dir, need_slash ? "/" : "", nm);
}

/* Stat/Open the source path of entry `i`, routed by its origin service
 * if known. An empty op.svcs[i] means "use default routing" — same
 * behaviour as before, fine for boot-disk sources. */
static int op_src_stat(int i, dobfs_stat_t *out)
{
    const char *svc = op.svcs[i];
    return (svc && *svc) ? dobfs_StatOn(svc, op.paths[i], out)
                         : dobfs_Stat(op.paths[i], out);
}

static int op_src_open(int i, uint32_t flags)
{
    const char *svc = op.svcs[i];
    return (svc && *svc) ? dobfs_OpenOn(svc, op.paths[i], flags)
                         : dobfs_Open(op.paths[i], flags);
}

/* Record one failed sub-operation. Captures the driver's REJECTED
 * reason on the first occurrence — subsequent rejections in the
 * same op are still counted as errors but their reason is not
 * stored (avoids overwriting the first message; further rejections
 * are usually the same root cause). */
static void op_note_error(void)
{
    op.errors++;
    if (op.rejection_msg[0] != '\0') return;
    const char *why = dobfs_last_rejection();
    if (!why || !*why) return;
    strncpy(op.rejection_msg, why, sizeof(op.rejection_msg) - 1);
    op.rejection_msg[sizeof(op.rejection_msg) - 1] = '\0';
}

/* Start the next file in the op. Returns 1 if started, 0 if op done. */
static int op_advance_to_next_file(void)
{
    while (op.cur_idx < op.n_paths)
    {
        const char *src = op.paths[op.cur_idx];
        strncpy(op.cur_name, op_basename(src), sizeof(op.cur_name) - 1);
        op.cur_name[sizeof(op.cur_name) - 1] = '\0';

        if (op.kind == OP_PASTE_COPY)
        {
            dobfs_stat_t st;
            if (op_src_stat(op.cur_idx, &st) < 0)
            {
                op_note_error();
                op.cur_idx++;
                continue;
            }
            op.file_size = st.size;
            op.file_done = 0;

            op.cur_rfd = op_src_open(op.cur_idx, FS_READ);
            if (op.cur_rfd < 0) { op_note_error(); op.cur_idx++; continue; }

            op_build_dest(src, op.cur_dest, sizeof(op.cur_dest));

            /* Dedupe via " (copia)" suffix — walk the basename. Probe
             * always against the local destination service (default
             * routing), never the source service. */
            char dname[256];
            strncpy(dname, op_basename(op.cur_dest), sizeof(dname) - 1);
            dname[sizeof(dname) - 1] = '\0';
            char probe[MAX_PATH];
            snprintf(probe, sizeof(probe), "%s/%s", op.dest_dir, dname);
            dobfs_stat_t pst;
            if (dobfs_Stat(probe, &pst) == 0)
            {
                /* Exists — generate "(copia N)" suffix */
                char base[256], ext[64];
                base[0] = ext[0] = '\0';
                const char *ddot = NULL;
                for (const char *q = dname; *q; q++) if (*q == '.') ddot = q;
                if (ddot && ddot != dname)
                {
                    size_t bl = (size_t)(ddot - dname);
                    if (bl >= sizeof(base)) bl = sizeof(base) - 1;
                    memcpy(base, dname, bl);
                    base[bl] = '\0';
                    strncpy(ext, ddot, sizeof(ext) - 1);
                    ext[sizeof(ext) - 1] = '\0';
                }
                else
                {
                    strncpy(base, dname, sizeof(base) - 1);
                    base[sizeof(base) - 1] = '\0';
                }
                int k = 1;
                while (k < 1000)
                {
                    if (k == 1)
                        snprintf(dname, sizeof(dname), "%s (copia)%s", base, ext);
                    else
                        snprintf(dname, sizeof(dname), "%s (copia %d)%s", base, k, ext);
                    snprintf(probe, sizeof(probe), "%s/%s", op.dest_dir, dname);
                    if (dobfs_Stat(probe, &pst) < 0) break;
                    k++;
                }
                snprintf(op.cur_dest, sizeof(op.cur_dest), "%s/%s",
                         op.dest_dir, dname);
            }

            op.cur_wfd = dobfs_Open(op.cur_dest,
                                    FS_WRITE | FS_CREATE | FS_TRUNC);
            if (op.cur_wfd < 0)
            {
                dobfs_Close(op.cur_rfd);
                op.cur_rfd = -1;
                op_note_error();
                op.cur_idx++;
                continue;
            }
            return 1;
        }
        else if (op.kind == OP_PASTE_MOVE)
        {
            /* MOVE only meaningful within the same service. Cross-service
             * "cut+paste" was clamped to "copy" by do_paste before we
             * even get here. */
            dobfs_stat_t st;
            if (op_src_stat(op.cur_idx, &st) == 0)
                op.file_size = st.size;
            else
                op.file_size = 0;
            op.file_done = 0;

            char dest[MAX_PATH];
            op_build_dest(src, dest, sizeof(dest));

            if (dobfs_Rename(src, dest) != 0) op_note_error();

            op.done_bytes += op.file_size;
            op.cur_idx++;
            return (op.cur_idx < op.n_paths) ? 1 : 2;
        }
        else /* OP_DELETE */
        {
            dobfs_stat_t st;
            if (op_src_stat(op.cur_idx, &st) == 0)
                op.file_size = st.size;
            else
                op.file_size = 0;
            op.file_done = 0;

            if (dobfs_Unlink(src) != 0) op_note_error();

            op.done_bytes += op.file_size;
            op.cur_idx++;
            return (op.cur_idx < op.n_paths) ? 1 : 2;
        }
    }
    return 0;   /* no more files */
}

/* Do one chunk of work. Called from event_tick. */
static void op_do_chunk(void)
{
    if (!op.active) return;

    /* For COPY we transfer OP_CHUNK_BYTES. For MOVE/DELETE each file
     * is a single-shot op — advance_to_next_file does the work and we
     * progress whole-file at a time. */
    if (op.kind == OP_PASTE_COPY)
    {
        if (op.cur_rfd < 0)
        {
            int rc = op_advance_to_next_file();
            if (rc == 0)
            {
                op_finish();
                return;
            }
        }

        /* Read + write one chunk. Buffer is static: 32 KB on the stack
         * would exhaust the 128 KB user stack (see kernel/proc/elf.c —
         * user stack is 0xBFFE0000..0xBFFFFFFF) once combined with the
         * event loop and FS stub frames already above us. A page fault
         * at ~0xbffdf52c confirmed the overflow. Static is safe because
         * op_do_chunk is never re-entered: the chunked state machine
         * runs strictly one chunk per event_tick. */
        static char op_chunk_buf[OP_CHUNK_BYTES];
        int rd = dobfs_Read(op.cur_rfd, op_chunk_buf, OP_CHUNK_BYTES);

        /* rd == 0 usually means EOF, but the FS returns 0 ALSO when a disk
         * read fails mid-file (handle_read does not signal the error). Tell
         * the two apart by progress: short of the source size, this 0 is a
         * masked read error, not EOF. Retry — the usual cause is a transient
         * USB hiccup that clears, and the driver's reset-recovery realigns
         * the data toggle on the failed attempt. A failed fill does NOT
         * advance the read offset, so a retry re-reads the same spot. */
        for (int rt = 0; rd == 0 && op.file_done < op.file_size
                         && rt < OP_IO_RETRIES; rt++)
            rd = dobfs_Read(op.cur_rfd, op_chunk_buf, OP_CHUNK_BYTES);

        if (rd > 0)
        {
            int wr = dobfs_Write(op.cur_wfd, op_chunk_buf, (uint32_t)rd);
            if (wr != rd)
            {
                /* Write failed or fell short (the FS now reports disk write
                 * errors instead of masking them as a short write). Fail this
                 * file visibly and delete the half-written destination — never
                 * leave a silently truncated file behind. */
                op_note_error();
                op_close_current_files();
                dobfs_Unlink(op.cur_dest);
                op.cur_idx++;
            }
            else
            {
                op.file_done += (uint64_t)rd;
                op.done_bytes += (uint64_t)rd;
            }
        }
        else if (op.file_done < op.file_size)
        {
            /* rd <= 0 but we never reached the source size: the read ended
             * early (a masked disk error, still failing after the retries).
             * Same treatment — visible error, drop the partial destination. */
            op_note_error();
            op_close_current_files();
            dobfs_Unlink(op.cur_dest);
            op.cur_idx++;
        }
        else
        {
            /* Genuine EOF: the whole file came through. */
            op_close_current_files();
            op.cur_idx++;
        }
    }
    else
    {
        /* MOVE or DELETE: each file in one shot */
        int rc = op_advance_to_next_file();
        if (rc == 0)
        {
            op_finish();
            return;
        }
    }

    /* Update speed stats and redraw the progress window.
     *
     * Redraw is UNCONDITIONAL (once per chunk) so the user sees the
     * progress moving even when individual chunks take noticeably less
     * than OP_REDRAW_MS. The speed SAMPLING window still honours
     * OP_REDRAW_MS — inst/avg speed need a non-zero elapsed interval to
     * avoid divide-by-zero and to smooth the reading. So: sample speed
     * every OP_REDRAW_MS, but always repaint (filename, percentage, bar
     * fill, latest speed values). */
    uint32_t now = clock_ms();
    uint32_t elapsed = now - op.last_sample_ms;
    if (elapsed >= OP_REDRAW_MS)
    {
        /* Speed calculations must avoid 64-bit division (no libgcc on
         * i686 freestanding). Since the sample window is ~200ms the
         * delta bytes always fits in 32 bits; same for total bytes
         * given MainDOB's 255MB RAM ceiling. Compute bytes/ms first
         * (32÷32), then scale to bytes/s. */
        uint32_t ddb = (uint32_t)(op.done_bytes - op.last_sample_bytes);
        if (elapsed > 0)
            op.inst_speed = (ddb / elapsed) * 1000u;
        op.last_sample_bytes = op.done_bytes;
        op.last_sample_ms    = now;

        uint32_t total_elapsed = now - op.start_ms;
        if (total_elapsed > 0)
        {
            uint32_t done32 = (uint32_t)op.done_bytes;
            op.avg_speed = (done32 / total_elapsed) * 1000u;
        }
    }

    op_progress_redraw();

    /* Check for normal termination */
    if (op.cur_idx >= op.n_paths && op.cur_rfd < 0)
    {
        op_finish();
    }
}

/* Return the index of the first source path whose tree would
 * contain the destination — i.e. dest_dir is the path itself or
 * any descendant. Copying / moving such a directory into its own
 * subtree would recurse forever (the chunked copy enumerates dest
 * dir live; new entries it just wrote keep showing up in scans).
 *
 * Works as a pure prefix test on normalised path strings:
 *   dest == src                          → recursive (level 0)
 *   dest starts with (src + "/")         → recursive (descendant)
 * Anything else → safe. The same predicate is correct for files
 * too (a file is never the ancestor of a directory) so we don't
 * special-case directories vs files — no extra IPC stat needed.
 *
 * Services must also match for the check to apply: a "Foo" in the
 * primary filesystem is unrelated to a "Foo" on a floppy. Each src
 * carries its service in services[i] (empty == default routing,
 * mapped via dobfs_get_service / "DobFileSystem" pair). Returns
 * -1 if no path is recursive. */
static int op_first_recursive_src(const char *const *services,
                                  const char *const *paths, int n,
                                  const char *dest_dir)
{
    const char *dest_svc  = dobfs_get_service();
    bool dest_default = (!dest_svc || strcmp(dest_svc, "DobFileSystem") == 0);
    size_t dest_len = strlen(dest_dir);

    for (int i = 0; i < n; i++)
    {
        const char *sp = paths[i];
        const char *ss = (services && services[i]) ? services[i] : "";
        bool src_default_i = (ss[0] == '\0');
        bool same_svc = (src_default_i && dest_default) ||
                        (!src_default_i && !dest_default &&
                         strcmp(ss, dest_svc) == 0);
        if (!same_svc) continue;     /* cross-volume can't be recursive */

        size_t sl = strlen(sp);
        if (dest_len < sl) continue; /* dest shorter than src can't be inside */
        if (memcmp(sp, dest_dir, sl) != 0) continue;
        /* dest_dir == sp                          -> recursive */
        /* dest_dir[sl] == '/' (with sl < dest_len) -> descendant */
        if (dest_len == sl)               return i;
        if (dest_dir[sl] == '/')          return i;
    }
    return -1;
}

/* Kick off a copy-or-move op: copies/moves an array of source paths
 * into op.dest_dir. `services` is parallel to `paths`; entries with
 * empty service use default routing. NULL services array = all blank.
 * Pre-req: bulk lock already acquired by caller. */
static void op_start_from_paths(int kind,
                                const char *const *services,
                                const char *const *paths, int n,
                                const char *dest_dir, const char *title)
{
    memset(&op, 0, sizeof(op));
    op.cur_rfd = -1;
    op.cur_wfd = -1;

    op.kind = kind;
    op.n_paths = (n > CLIP_MAX_PATHS) ? CLIP_MAX_PATHS : n;
    for (int i = 0; i < op.n_paths; i++)
    {
        strncpy(op.paths[i], paths[i], MAX_PATH - 1);
        op.paths[i][MAX_PATH - 1] = '\0';

        if (services && services[i])
        {
            strncpy(op.svcs[i], services[i], sizeof(op.svcs[i]) - 1);
            op.svcs[i][sizeof(op.svcs[i]) - 1] = '\0';
        }

        dobfs_stat_t st;
        if (op_src_stat(i, &st) == 0)
            op.total_bytes += st.size;
    }
    strncpy(op.dest_dir, dest_dir, sizeof(op.dest_dir) - 1);
    op.dest_dir[sizeof(op.dest_dir) - 1] = '\0';

    op.start_ms       = clock_ms();
    op.last_sample_ms = op.start_ms;
    op.last_redraw_ms = op.start_ms;
    op.active         = true;

    op_progress_open(title);
    dobui_set_tick(1);   /* 1 ms — pump chunks as fast as the loop allows */
}

/* Kick off a delete op on the current selection. Sources always live
 * on this window's bound service (hence svcs[] left blank — default
 * routing through dobfs_set_service). */
static void op_start_delete_selection(void)
{
    memset(&op, 0, sizeof(op));
    op.cur_rfd = -1;
    op.cur_wfd = -1;
    op.kind = OP_DELETE;

    for (int i = 0; i < entry_count && op.n_paths < CLIP_MAX_PATHS; i++)
    {
        if (!sel_get(i)) continue;
        build_full_path(op.paths[op.n_paths], entries[i].name);
        dobfs_stat_t st;
        if (dobfs_Stat(op.paths[op.n_paths], &st) == 0)
            op.total_bytes += st.size;
        op.n_paths++;
    }

    op.start_ms       = clock_ms();
    op.last_sample_ms = op.start_ms;
    op.last_redraw_ms = op.start_ms;
    op.active         = true;

    op_progress_open("Eliminazione");
    dobui_set_tick(1);
}

static void do_paste(void)
{
    if (op.active) return;   /* an op is already running */

    static char src_svcs [CLIP_MAX_PATHS][64];
    static char src_paths[CLIP_MAX_PATHS][MAX_PATH];
    bool is_cut = false;
    int n = clipboard_get_multi(src_svcs, src_paths, CLIP_MAX_PATHS, &is_cut);
    if (n <= 0) return;

    /* Cut across services has no sane semantics for us — Rename only
     * works within one backend. Demote to copy when any source carries
     * a non-blank service that differs from this window's own. */
    if (is_cut)
    {
        const char *here = dobfs_get_service();
        bool here_default = (!here || strcmp(here, "DobFileSystem") == 0);
        for (int i = 0; i < n; i++)
        {
            bool src_default = (src_svcs[i][0] == '\0');
            if (src_default && here_default) continue;
            if (!src_default && !here_default && strcmp(src_svcs[i], here) == 0) continue;
            is_cut = false;   /* cross-service: do a copy instead */
            break;
        }
    }

    char busy_label[64];
    if (!bulk_try_begin(is_cut ? "Spostamento" : "Copia", busy_label, sizeof(busy_label)))
    {
        char m[160];
        snprintf(m, sizeof(m),
                 "Impossibile effettuare questa operazione,\n"
                 "un'altra operazione e' in corso: %s", busy_label);
        dobpopup_Error("File", m);
        return;
    }

    const char *ptrs[CLIP_MAX_PATHS];
    const char *svcs[CLIP_MAX_PATHS];
    for (int i = 0; i < n; i++) { ptrs[i] = src_paths[i]; svcs[i] = src_svcs[i]; }

    int kind = is_cut ? OP_PASTE_MOVE : OP_PASTE_COPY;
    const char *title = is_cut ? "Spostamento" : "Copia";

    /* Reject self-recursive paste: copying/moving a folder into
     * itself or one of its descendants would recurse forever. */
    if (op_first_recursive_src(svcs, ptrs, n, current_path) >= 0)
    {
        bulk_release();
        dobpopup_Error("File",
            "Impossibile copiare una cartella dentro se' stessa.");
        return;
    }

    op_start_from_paths(kind, svcs, ptrs, n, current_path, title);
}

static void do_delete(void)
{
    if (op.active) return;
    if (sel_count == 0) return;

    char msg_buf[160];
    if (sel_count == 1)
    {
        int idx = sel_first();
        snprintf(msg_buf, sizeof(msg_buf), "Eliminare '%s'?", entries[idx].name);
    }
    else
    {
        snprintf(msg_buf, sizeof(msg_buf), "Eliminare %d elementi?", sel_count);
    }
    int choice = dobpopup_YesNo("Elimina", msg_buf);
    if (choice != 0) return;

    char busy_label[64];
    if (!bulk_try_begin("Eliminazione", busy_label, sizeof(busy_label)))
    {
        char m[160];
        snprintf(m, sizeof(m),
                 "Impossibile effettuare questa operazione,\n"
                 "un'altra operazione e' in corso: %s", busy_label);
        dobpopup_Error("File", m);
        return;
    }

    op_start_delete_selection();
}

/* Properties: pop up a DobTable populated with this entry's metadata.
 *
 * Same pattern as videotest's dobVideo introspection panel and the
 * standalone valueParser demo — DobTable is the reusable key/value
 * widget, callers supply their own pairs.  Nothing in DobTable cares
 * about file semantics; we just construct strings and ship them. */
static void do_properties(void)
{
    if (sel_count != 1) return;
    int idx = sel_first();
    if (idx < 0 || idx >= entry_count) return;

    dir_entry_t *e = &entries[idx];
    char full[MAX_PATH];
    build_full_path(full, e->name);

    /* Tipo */
    const char *type_label = e->is_dir ? "Cartella" : "File";

    /* Estensione (pointer into e->name, or "(nessuna)") */
    const char *ext_raw = get_extension(e->name);
    const char *ext = (ext_raw && *ext_raw) ? ext_raw : "(nessuna)";

    /* Dimensione — human + raw byte count.  Dirs report "—" since the
     * dirent size field is the file count for directories on FAT32
     * and would be misleading. */
    char size_buf[64];
    if (e->is_dir)
    {
        snprintf(size_buf, sizeof(size_buf), "—");
    }
    else if (e->size < 1024u)
    {
        snprintf(size_buf, sizeof(size_buf), "%u byte", (unsigned)e->size);
    }
    else if (e->size < 1024u * 1024u)
    {
        unsigned whole = e->size / 1024u;
        unsigned frac  = ((e->size % 1024u) * 10u) / 1024u;
        snprintf(size_buf, sizeof(size_buf),
                 "%u.%u KB (%u byte)", whole, frac, (unsigned)e->size);
    }
    else
    {
        unsigned whole = e->size / (1024u * 1024u);
        unsigned frac  = ((e->size % (1024u * 1024u)) * 10u) / (1024u * 1024u);
        snprintf(size_buf, sizeof(size_buf),
                 "%u.%u MB (%u byte)", whole, frac, (unsigned)e->size);
    }

    /* Filesystem backend that's currently serving this listing.  An
     * empty/NULL return from dobfs_get_service() means we're on the
     * default DobFileSystem router (system disk). */
    const char *fs_svc = dobfs_get_service();
    if (!fs_svc || !*fs_svc) fs_svc = "DobFileSystem";

    /* Build the rows. Order is deliberate: identity first (Nome/Tipo),
     * then static attributes (Estensione), then dynamic (Dimensione),
     * then location (Path/Filesystem).  Mirrors how Windows Explorer
     * orders its own property sheet so the placement feels natural. */
    const char *keys[6];
    const char *vals[6];
    int n = 0;

    keys[n] = "Nome";          vals[n] = e->name;   n++;
    keys[n] = "Tipo";          vals[n] = type_label; n++;
    if (!e->is_dir)
    {
        keys[n] = "Estensione"; vals[n] = ext;       n++;
    }
    keys[n] = "Dimensione";    vals[n] = size_buf;  n++;
    keys[n] = "Path completo"; vals[n] = full;      n++;
    keys[n] = "Filesystem";    vals[n] = fs_svc;    n++;

    /* Title carries the basename so multiple Properties windows can
     * coexist and be told apart at a glance from the WM's task list. */
    char title[128];
    snprintf(title, sizeof(title), "Proprietà — %s", e->name);

    char svc[32];
    if (dobtable_Spawn(svc, sizeof(svc)) != 0)
    {
        dobpopup_Error("Proprietà",
                       "Impossibile avviare DobTable.\n"
                       "Controlla che DobTable.mdl sia installato.");
        return;
    }
    if (dobtable_SetTitle(svc, title) != 0)            goto fail;
    if (dobtable_SetHeaders(svc, "Proprietà", "Valore") != 0) goto fail;
    if (dobtable_AddRows(svc, keys, vals, n) != 0)     goto fail;
    if (dobtable_Show(svc) != 0)                       goto fail;
    return;

fail:
    dobpopup_Error("Proprietà",
                   "Errore nel popolare la tabella delle proprietà.");
}

static void do_create_file(void)
{
    char name[256];
    if (ask_unique_name("Crea file", "Nome del file:", "", name, sizeof(name)) != 0)
        return;

    char full[MAX_PATH];
    build_full_path(full, name);
    int fd = dobfs_Open(full, FS_WRITE | FS_CREATE);
    if (fd >= 0) dobfs_Close(fd);

    read_directory();
    update_panel();
    redraw();
}

static void do_create_dir(void)
{
    char name[256];
    if (ask_unique_name("Crea cartella", "Nome della cartella:", "", name, sizeof(name)) != 0)
        return;

    char full[MAX_PATH];
    build_full_path(full, name);
    if (dobfs_Mkdir(full) != 0)
    {
        const char *why = dobfs_last_rejection();
        if (why && *why) dobpopup_Error("File", why);
    }

    read_directory();
    update_panel();
    redraw();
}

static void do_rename(void)
{
    if (selected < 0) return;

    char name[256];
    if (ask_unique_name("Rinomina", "Nuovo nome:",
                        entries[selected].name, name, sizeof(name)) != 0)
        return;

    char old_path[MAX_PATH], new_path[MAX_PATH];
    build_full_path(old_path, entries[selected].name);
    build_full_path(new_path, name);
    if (dobfs_Rename(old_path, new_path) != 0)
    {
        const char *why = dobfs_last_rejection();
        if (why && *why) dobpopup_Error("File", why);
    }

    read_directory();
    update_panel();
    redraw();
}

/* Dialog mode — OpenFile / SaveFile API for other programs */

/* Check if a filename matches the extension filter.
 * Empty filter = accept all. Filter format: ".txt|.md|.c" */
static bool ext_matches(const char *name, const char *filter)
{
    if (!filter || filter[0] == '\0') return true;

    const char *dot = NULL;
    const char *p = name;
    while (*p) { if (*p == '.') dot = p; p++; }
    if (!dot) return false;

    /* Walk pipe-separated extensions */
    const char *f = filter;
    while (*f)
    {
        const char *fend = f;
        while (*fend && *fend != '|') fend++;

        int flen = (int)(fend - f);
        int dlen = (int)strlen(dot);
        if (flen == dlen)
        {
            bool match = true;
            for (int i = 0; i < flen; i++)
            {
                char a = f[i], b = dot[i];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { match = false; break; }
            }
            if (match) return true;
        }

        f = fend;
        if (*f == '|') f++;
    }
    return false;
}

static void dialog_update_panel(void)
{
    update_panel();
}

/* End the one-shot dialog process: tear down its window and exit.
 * The reply to the picker stub must already have been sent. */
static void dialog_finish(void)
{
    dobui_DestroyWindow(win_id);
    _exit(0);
}

static void dialog_complete_open(void)
{
    if (selected < 0 || entries[selected].is_dir) return;

    /* Validate extension filter */
    if (dialog_extensions[0] &&
        !ext_matches(entries[selected].name, dialog_extensions))
    {
        dobpopup_Warning("Tipo non valido",
            "Il file selezionato non corrisponde ai tipi richiesti.");
        return;
    }

    /* Build full path (service-qualified when on a mounted volume) */
    dialog_build_result(entries[selected].name);

    /* Reply with path only — caller reads the file itself */
    dob_msg_t reply = {0};
    reply.arg0 = 0;  /* success */
    reply.payload = dialog_result_path;
    reply.payload_size = strlen(dialog_result_path) + 1;
    dob_ipc_reply(dialog_sender_tid, &reply);

    dialog_mode = false;
    dialog_finish();
}

static void dialog_complete_save(void)
{
    /* Ask filename via InputBox */
    char name[256];
    int result = dobpopup_InputBox("Salva file", "Nome del file:",
                                    dialog_save_name, name, sizeof(name));
    if (result != 0 || name[0] == '\0') return;

    /* Build full path (service-qualified when on a mounted volume) */
    dialog_build_result(name);

    /* Reply with path only — caller writes the file itself */
    dob_msg_t reply = {0};
    reply.arg0 = 0;  /* success */
    reply.payload = dialog_result_path;
    reply.payload_size = strlen(dialog_result_path) + 1;
    dob_ipc_reply(dialog_sender_tid, &reply);

    dialog_mode = false;
    dialog_finish();
}

static void dialog_cancel(void)
{
    dob_msg_t reply = {0};
    reply.arg0 = (uint32_t)(-1);
    dob_ipc_reply(dialog_sender_tid, &reply);

    dialog_mode = false;
    dialog_finish();
}

static void handle_dialog_request(dob_msg_t *msg)
{
    if (!msg->payload || msg->payload_size < 2)
    {
        dob_msg_t reply = {0};
        reply.arg0 = (uint32_t)(-1);
        dob_ipc_reply(msg->sender_tid, &reply);
        return;
    }

    dialog_mode = true;
    dialog_type = (int)msg->code;
    dialog_sender_tid = msg->sender_tid;

    if (dialog_type == 10) /* OPEN */
    {
        /* Payload: extensions\0default_path */
        const char *ext = (const char *)msg->payload;
        strncpy(dialog_extensions, ext, sizeof(dialog_extensions) - 1);
        dialog_extensions[sizeof(dialog_extensions) - 1] = '\0';
        uint32_t elen = strlen(ext);
        const char *path = (elen + 1 < msg->payload_size) ? ext + elen + 1 : "/DATA";
        navigate_to(path);
    }
    else /* SAVE (11) */
    {
        /* Payload: default_name\0extensions\0default_path\0 */
        const char *dname = (const char *)msg->payload;
        uint32_t nlen = strlen(dname);
        strncpy(dialog_save_name, dname, sizeof(dialog_save_name) - 1);
        dialog_save_name[sizeof(dialog_save_name) - 1] = '\0';

        const char *ext = (nlen + 1 < msg->payload_size) ? dname + nlen + 1 : "";
        strncpy(dialog_extensions, ext, sizeof(dialog_extensions) - 1);
        dialog_extensions[sizeof(dialog_extensions) - 1] = '\0';
        uint32_t elen = strlen(ext);

        const char *path = (nlen + 1 + elen + 1 < msg->payload_size)
                           ? ext + elen + 1 : "/DATA";

        navigate_to(path);
    }

    dialog_update_panel();

    /* This process IS the dialog. Re-title the window dobui_run
     * already created (no second window, nothing to hide). The
     * picker stays a normal window — resizable and maximizable like
     * any other; event_resize() already handles re-layout. */
    const char *dtitle = (dialog_type == 10) ? "Apri file" : "Salva file";
    dobui_SetTitle(win_id, dtitle);

    redraw();
}

/* Event handling */

static void handle_panel_cmd(int cmd_idx)
{
    /* Dialog mode has its own panel commands.  The index meaning is
     * driven by what update_panel composed:
     *   Open with selection      -> [0]=Seleziona, [1]=Annulla
     *   Open without selection   -> [0]=Annulla         <-- only one
     *   Save                     -> [0]=Salva qui, [1]=Annulla
     *
     * If we naively branched on (cmd_idx == 0 -> complete, else
     * cancel), the "Open without selection" case would never cancel
     * — the lone Annulla button is index 0 there. Mirror the panel-
     * composition predicates so the action matches the label. */
    if (dialog_mode)
    {
        if (dialog_type == 10) /* Open */
        {
            bool has_seleziona = (selected >= 0
                                  && data_source == SRC_DIR
                                  && !entries[selected].is_dir);
            if (has_seleziona)
            {
                if (cmd_idx == 0) dialog_complete_open();
                else              dialog_cancel();
            }
            else
            {
                /* Lone Annulla button. */
                dialog_cancel();
            }
        }
        else /* Save */
        {
            /* update_panel exposes Annulla as the lone command when
             * we're in SRC_MOUNTS (no save target there); otherwise
             * two-button layout. */
            bool has_salva = (data_source == SRC_DIR);
            if (has_salva)
            {
                if (cmd_idx == 0) dialog_complete_save();
                else              dialog_cancel();
            }
            else
            {
                dialog_cancel();
            }
        }
        return;
    }

    /* Rebuild the index ↔ command table in the EXACT same order as
     * update_panel() composes it.  Both functions are driven from the
     * same predicates so adding a new command means editing both in
     * lock-step.
     *
     * Sequence (skipping a slot when its predicate is false):
     *   0..2  Apri / Rinomina / Proprietà   (one_sel)
     *   3..5  Copia / Taglia / Elimina      (has_sel)
     *   6     Incolla                       (has_clip)
     *   7..8  Crea file / Crea cartella     (!has_sel)
     *   9     Multi On|Off                  (always)
     */
    enum {
        CMD_OPEN, CMD_RENAME, CMD_PROPS,
        CMD_COPY, CMD_CUT, CMD_DELETE,
        CMD_PASTE,
        CMD_NEW_FILE, CMD_NEW_DIR,
        CMD_MULTI,
        CMD__MAX
    };

    bool has_clip = clipboard_active();
    bool has_sel  = sel_count >= 1;
    bool one_sel  = sel_count == 1;

    int order[CMD__MAX];
    int n = 0;

    if (one_sel)  { order[n++] = CMD_OPEN;  order[n++] = CMD_RENAME; order[n++] = CMD_PROPS; }
    if (has_sel)  { order[n++] = CMD_COPY;  order[n++] = CMD_CUT;    order[n++] = CMD_DELETE; }
    if (has_clip) { order[n++] = CMD_PASTE; }
    if (!has_sel) { order[n++] = CMD_NEW_FILE; order[n++] = CMD_NEW_DIR; }
    order[n++] = CMD_MULTI;

    if (cmd_idx < 0 || cmd_idx >= n) return;

    switch (order[cmd_idx])
    {
        case CMD_OPEN:     do_open();        break;
        case CMD_RENAME:   do_rename();      break;
        case CMD_PROPS:    do_properties();  break;
        case CMD_COPY:     do_copy(false);   break;
        case CMD_CUT:      do_copy(true);    break;
        case CMD_DELETE:   do_delete();      break;
        case CMD_PASTE:    do_paste();       break;
        case CMD_NEW_FILE: do_create_file(); break;
        case CMD_NEW_DIR:  do_create_dir();  break;
        case CMD_MULTI:
            multi_sticky_mode = !multi_sticky_mode;
            update_panel();
            redraw();
            break;
    }
}

static void handle_mouse_click(int lx, int ly, uint8_t buttons)
{
    if (buttons & 2) return;

    /* Cancel any pending drag arm from a previous click. */
    drag_armed = false;

    /* Breadcrumb strip — leftmost layer. */
    if (ly < BREADCRUMB_H)
    {
        for (int i = bread_count - 1; i >= 0; i--)
        {
            if (lx >= bread_items[i].x && lx < bread_items[i].x + bread_items[i].w)
            {
                navigate_to(bread_items[i].path);
                return;
            }
        }
        return;
    }

    /* Ribbon strip — second layer, only when visible (not in dialog). */
    int list_top = BREADCRUMB_H + ribbon_h_visible();
    if (ly < list_top)
    {
        ribbon_handle_click(lx, ly);
        return;
    }

    /* SRC_MOUNTS click: single-click selects + activates. No drag,
     * no multi-select, no rect-select (the picker is one-of-N). */
    if (data_source == SRC_MOUNTS)
    {
        int midx = view_idx_at(lx, ly);
        if (midx >= 0)
        {
            sel_clear();
            sel_set(midx, true);
            selected = midx;
            redraw();
            do_enter_mount(midx);
        }
        return;
    }

    /* Sticky multi-select acts as a held Ctrl: every list click toggles
     * the item without changing the rest of the selection.  The user
     * keeps explicit control over what's in the multi-set without
     * having to keep a modifier pressed. */
    bool ctrl = (mod_state & DOBUI_MOD_CTRL) != 0 || multi_sticky_mode;

    int idx = view_idx_at(lx, ly);

    if (idx >= 0)
    {
        if (ctrl)
        {
            /* Ctrl-click (or sticky-multi): toggle selection, never
             * start a drag. */
            sel_toggle(idx);
            selected = sel_get(idx) ? idx : sel_first();
        }
        else if (sel_get(idx) && sel_count >= 1)
        {
            /* Clicked on an already-selected item without Ctrl:
             * arm a potential drag. Do NOT change selection yet —
             * if the user moves > threshold we start a drag with the
             * whole current selection; if they release without moving
             * we narrow the selection to this single item (handled
             * in event_mouserelease). */
            drag_armed   = true;
            drag_press_x = lx;
            drag_press_y = ly;
            /* Don't touch selection or redraw here. */
            return;
        }
        else
        {
            /* Clicked on an unselected item without Ctrl:
             * single-select AND arm a potential drag from this item.
             * If the user releases without moving past threshold this
             * stays a normal click (handled in event_mouserelease).
             * If they move past threshold it becomes a drag of this
             * single freshly-selected item. Matches the expected
             * "click and drag immediately" UX of every other OS. */
            sel_clear();
            sel_set(idx, true);
            selected = idx;
            drag_armed   = true;
            drag_press_x = lx;
            drag_press_y = ly;
        }
        update_panel();
        redraw();
    }
    else
    {
        /* Click on empty list area → start a rectangle drag.
         * Ctrl (or sticky-multi) makes the drag additive: the existing
         * selection is preserved as a baseline and the rect adds to it. */
        rect_active   = true;
        rect_additive = ctrl;
        rect_x0 = rect_x1 = lx;
        rect_y0 = rect_y1 = ly;
        if (rect_additive)
        {
            memcpy(rect_base_bits, sel_bits, sizeof(sel_bits));
            rect_base_count = sel_count;
        }
        else
        {
            sel_clear();
            selected = -1;
        }
        update_panel();
        redraw();
    }
}

static void handle_mouse_dblclick(int lx, int ly)
{
    int list_top = BREADCRUMB_H + ribbon_h_visible();
    if (ly < list_top) return;

    /* SRC_MOUNTS: single-click already triggered do_enter_mount;
     * swallow the dblclick so we don't fire twice or, worse, land on
     * a stale index after a successful hijack changed entries[]. */
    if (data_source == SRC_MOUNTS) return;

    int idx = view_idx_at(lx, ly);
    if (idx >= 0)
    {
        sel_clear();
        sel_set(idx, true);
        selected = idx;
        do_open();
    }
}

/* Scroll the view so entry `idx` is visible.  In list mode the unit
 * is "entries"; in icon mode it's "grid rows".  Used by the keyboard
 * cursor — after every arrow / Home / End / PgUp / PgDn the new
 * selection must come into view or the user loses their place. */
static void scroll_to_visible(int idx)
{
    if (idx < 0 || idx >= entry_count) return;
    int list_top = view_list_top();
    int list_h   = win_h - list_top;

    if (view_style == VIEW_ICONS)
    {
        int cols = view_cols();
        int rows_vis = list_h / GRID_H;
        if (rows_vis < 1) rows_vis = 1;
        int row = idx / cols;
        if (row < scroll_offset) scroll_offset = row;
        else if (row >= scroll_offset + rows_vis)
            scroll_offset = row - rows_vis + 1;
    }
    else
    {
        int rows_vis = list_h / ITEM_H;
        if (rows_vis < 1) rows_vis = 1;
        if (idx < scroll_offset) scroll_offset = idx;
        else if (idx >= scroll_offset + rows_vis)
            scroll_offset = idx - rows_vis + 1;
    }
    if (scroll_offset < 0) scroll_offset = 0;
    int max_off = view_max_scroll();
    if (scroll_offset > max_off) scroll_offset = max_off;
}

/* Replace selection with a single new entry and bring it on screen.
 * Used by every keyboard navigation handler so they all share the
 * same "single-cursor" model. Shift-extending isn't implemented yet;
 * the user can still build a multi-set with sticky-Multi + arrows
 * (toggle behaves like Ctrl) if desired. */
static void cursor_set(int new_idx)
{
    if (entry_count == 0) return;
    if (new_idx < 0) new_idx = 0;
    if (new_idx >= entry_count) new_idx = entry_count - 1;
    sel_clear();
    sel_set(new_idx, true);
    selected = new_idx;
    scroll_to_visible(new_idx);
    update_panel();
    redraw();
}

/* Translate (dx, dy) in "cells" into a new index in the current
 * layout.  List mode ignores dx (one column).  Icon mode uses both. */
static void cursor_move(int dx, int dy)
{
    if (entry_count == 0) return;

    int cur = (selected >= 0) ? selected : 0;
    int new_idx;

    if (view_style == VIEW_ICONS)
    {
        int cols = view_cols();
        int row  = cur / cols;
        int col  = cur % cols;
        col += dx;
        row += dy;
        if (col < 0) col = 0;
        if (col >= cols) col = cols - 1;
        if (row < 0) row = 0;
        new_idx = row * cols + col;
        /* If we landed past the last entry on the bottom row, clamp
         * back to the last actual entry. */
        if (new_idx >= entry_count) new_idx = entry_count - 1;
    }
    else
    {
        /* List mode: vertical only. dx silently ignored. */
        (void)dx;
        new_idx = cur + dy;
    }

    cursor_set(new_idx);
}

/* Page jump: one screenful in the active mode. */
static void cursor_page(int direction)
{
    if (entry_count == 0) return;
    int list_top = view_list_top();
    int list_h   = win_h - list_top;
    int step;
    if (view_style == VIEW_ICONS)
    {
        int cols = view_cols();
        int rows_vis = list_h / GRID_H;
        step = rows_vis * cols;
    }
    else
    {
        step = list_h / ITEM_H;
    }
    if (step < 1) step = 1;
    int cur = (selected >= 0) ? selected : 0;
    cursor_set(cur + direction * step);
}

static void handle_key(uint8_t key)
{
    /* Dialog mode owns its own keyboard semantics (Enter = confirm,
     * Esc = cancel, etc.) handled elsewhere; skip the explorer
     * shortcuts entirely so we don't fight the dialog. */
    if (dialog_mode) return;

    /* Ctrl shortcuts.  inputd delivers Ctrl+letter as the matching
     * control character (Ctrl+A=0x01, C=0x03, V=0x16, X=0x18), same
     * convention used by libdobui's textbox clipboard handlers. */
    if (key == 0x01)                  /* Ctrl+A — select all */
    {
        if (entry_count == 0) return;
        for (int i = 0; i < entry_count; i++) sel_set(i, true);
        selected = sel_first();
        update_panel();
        redraw();
        return;
    }
    if (key == 0x03) { do_copy(false); return; }   /* Ctrl+C */
    if (key == 0x18) { do_copy(true);  return; }   /* Ctrl+X */
    if (key == 0x16) { do_paste();     return; }   /* Ctrl+V */

    /* Navigation. KEY_* codes come from libdobui/app.h. */
    switch (key)
    {
        case KEY_UP:     cursor_move(0, -1); return;
        case KEY_DOWN:   cursor_move(0, +1); return;
        case KEY_LEFT:   cursor_move(-1, 0); return;
        case KEY_RIGHT:  cursor_move(+1, 0); return;
        case KEY_HOME:   cursor_set(0); return;
        case KEY_END:    cursor_set(entry_count - 1); return;
        case KEY_PGUP:   cursor_page(-1); return;
        case KEY_PGDN:   cursor_page(+1); return;
        case KEY_DELETE: do_delete(); return;
    }

    /* Enter / Return on the selected entry opens it — keyboard
     * equivalent of double-click.  Only the single-selection case;
     * Enter on a multi-selection is ambiguous (open which?) so we
     * silently ignore it. */
    if ((key == '\n' || key == '\r') && sel_count == 1)
    {
        do_open();
        return;
    }
}

/* Event loop */

/* Event handlers — framework event-driven model */

void event_mouseclick(int x, int y, uint8_t buttons)
{
    handle_mouse_click(x, y, buttons);
}

void event_dblclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    handle_mouse_dblclick(x, y);
}

/* Recompute the multiselection bitmap from the current rectangle.
 * In additive mode (drag started with Ctrl) the saved baseline is
 * restored first; otherwise the bitmap is cleared. Then every entry
 * whose row strip vertically intersects the rectangle is added. */
static void rect_recompute_selection(void)
{
    int list_top = view_list_top();
    int rx0 = rect_x0 < rect_x1 ? rect_x0 : rect_x1;
    int rx1 = rect_x0 < rect_x1 ? rect_x1 : rect_x0;
    int ry0 = rect_y0 < rect_y1 ? rect_y0 : rect_y1;
    int ry1 = rect_y0 < rect_y1 ? rect_y1 : rect_y0;
    if (ry0 < list_top) ry0 = list_top;

    if (rect_additive)
    {
        memcpy(sel_bits, rect_base_bits, sizeof(sel_bits));
        sel_count = rect_base_count;
    }
    else
    {
        sel_clear();
    }

    /* Mode-agnostic: ask the geometry helper for each visible entry's
     * rect and test for 2D intersection with the drag box. Off-screen
     * entries (view_entry_rect returns false) are skipped — they
     * couldn't be under the rect anyway. */
    for (int idx = 0; idx < entry_count; idx++)
    {
        int ex, ey, ew, eh;
        if (!view_entry_rect(idx, &ex, &ey, &ew, &eh)) continue;
        if (ex + ew <= rx0 || ex >= rx1) continue;
        if (ey + eh <= ry0 || ey >= ry1) continue;
        sel_set(idx, true);
    }

    selected = sel_first();
}

void event_mousemove(int x, int y, uint8_t buttons)
{
    /* Drag source detection: if the user moves past threshold while
     * drag_armed, build the payload and call BeginDrag. Once the WM
     * takes over, no further mouse events arrive until the session
     * ends (handled in event_drag_end). */
    if (drag_armed && !drag_in_progress)
    {
        int dx = x - drag_press_x;
        int dy = y - drag_press_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;

        if (dx + dy >= DRAG_THRESHOLD)
        {
            /* Build path list from current selection, attaching this
             * window's bound service so the drop target can route the
             * source open back through the right backend. */
            static char dpaths[CLIP_MAX_PATHS][MAX_PATH];
            const char *ptrs[CLIP_MAX_PATHS];
            const char *svcs[CLIP_MAX_PATHS];
            const char *here = dobfs_get_service();
            bool here_default = (!here || strcmp(here, "DobFileSystem") == 0);
            int n = 0;
            for (int i = 0; i < entry_count && n < CLIP_MAX_PATHS; i++)
            {
                if (!sel_get(i)) continue;
                build_full_path(dpaths[n], entries[i].name);
                ptrs[n] = dpaths[n];
                svcs[n] = here_default ? "" : here;
                n++;
            }
            if (n > 0)
            {
                /* Default intent: move (is_cut = true). Space toggles
                 * copy mode inside the WM during the drag session. */
                int rc = dobui_BeginDragOn(win_id, svcs, ptrs, n, true);
                if (rc == 0)
                {
                    drag_in_progress = true;
                    /* WM has taken over — no more mouse events until
                     * event_drag_end. */
                    return;
                }
            }
            /* Failed to start drag — fall through to normal. */
            drag_armed = false;
        }
        return;
    }

    /* Drag-over from any window (including from us-as-source after
     * BeginDrag — but that path never reaches here because the WM
     * stops delivering us mouse events while we're the source).
     *
     * Heuristic: any mousemove with buttons held that is NOT our own
     * pre-BeginDrag press (which would have set drag_armed above) is
     * a drag-over.  This catches:
     *   - drops being aimed at us from another DobFiles window
     *   - the rare case where the user starts dragging in this window
     *     and then drags back into it (the WM still routes etype=6 to
     *     us in that case; we treat it the same way)
     *
     * Exclude rect-select: a rectangle drag started on empty space
     * is purely a selection operation, not a drop target. Letting
     * it set drag_hover_idx paints a cyan drop-highlight on whatever
     * folder happens to be under the cursor, masking the yellow
     * rect-selection on that one item. */
    if (buttons != 0 && !drag_armed && !drag_in_progress && !rect_active)
    {
        int idx = view_idx_at(x, y);
        int new_hover = (idx >= 0 && entries[idx].is_dir) ? idx : -1;
        if (new_hover != drag_hover_idx)
        {
            drag_hover_idx = new_hover;
            redraw();
        }
        return;
    }

    if (!rect_active) return;

    rect_x1 = x;
    rect_y1 = y;
    rect_recompute_selection();
    redraw();
}

void event_mouserelease(int x, int y, uint8_t buttons)
{
    (void)buttons;

    /* Belt-and-suspenders: clear any stale drag-over highlight. The
     * normal cleanup happens in event_drop / event_drag_end, but this
     * handler catches edge cases like a release on the title bar
     * (where neither of those fires for us). */
    if (drag_hover_idx >= 0)
    {
        drag_hover_idx = -1;
        redraw();
    }

    /* If drag was armed but never reached threshold, treat this as a
     * normal click: narrow selection to the single item under the
     * original press position. */
    if (drag_armed && !drag_in_progress)
    {
        drag_armed = false;
        int idx = view_idx_at(drag_press_x, drag_press_y);
        if (idx >= 0)
        {
            sel_clear();
            sel_set(idx, true);
            selected = idx;
            update_panel();
            redraw();
        }
        return;
    }

    if (!rect_active) return;

    rect_x1 = x;
    rect_y1 = y;
    rect_recompute_selection();
    rect_active = false;
    update_panel();
    redraw();
}

void event_modchange(uint8_t mods)
{
    mod_state = mods;
}

void event_key(uint8_t key)
{
    handle_key(key);
}

void event_scroll(int delta)
{
    /* Per-mode scroll granularity:
     *   - list:  3 entries per wheel tick (historical behaviour)
     *   - icons: 1 grid-row per wheel tick (= cols entries jumped)
     *
     * The scroll_offset unit is the same as the row index used by the
     * draw / hit-test paths in each mode, so we don't have to convert
     * between modes — just clamp against view_max_scroll(). */
    int step = (view_style == VIEW_ICONS) ? 1 : 3;
    scroll_offset += delta * step;
    if (scroll_offset < 0) scroll_offset = 0;
    int max_off = view_max_scroll();
    if (scroll_offset > max_off) scroll_offset = max_off;
    redraw();
}

void event_panel(int cmd_idx)
{
    handle_panel_cmd(cmd_idx);
}

void event_resize(int w, int h)
{
    win_w = w;
    win_h = h;

    /* Refresh panel + ribbon state. ribbon_update_state's relayout
     * pass needs to run after a resize too, otherwise minimal-mode
     * buttons stay at their full-layout x-coords from init time. */
    update_panel();
    redraw();
}

void event_close(void)
{
    if (dialog_mode)
        dialog_cancel();
    else
    {
        dobui_DestroyWindow(win_id);
        _exit(0);
    }
}

void event_request(dob_msg_t *msg)
{
    if (msg->code == DOBFS_UNMOUNT_NOTIFY)
    {
        /* The mount we were serving is gone (eject, or remount on the
         * same drive). Tear down the window and exit — a satellite has
         * no other reason to live. Always arrives as a fire-and-forget
         * post, so no reply is owed. */
        dobui_DestroyWindow(win_id);
        _exit(0);
    }

    if (msg->code == FILES_CMD_OPEN_DIALOG
     || msg->code == FILES_CMD_SAVE_DIALOG)
    {
        if (g_is_dialog && !dialog_mode)
        {
            handle_dialog_request(msg);
            /* Do NOT reply — dialog_complete/cancel will reply later */
        }
        else
        {
            /* Only a spawned --dialog instance serves dialogs; an
             * explorer instance must never be hijacked into one.
             * Reply a clean cancel so a stray caller does not hang. */
            dob_msg_t reply = {0};
            reply.arg0 = (uint32_t)(-1);
            dob_ipc_reply(msg->sender_tid, &reply);
        }
    }
    else if (msg->code == FILES_CMD_MOUNT && msg->payload && msg->payload_size > 0)
    {
        /* Switch this DobFiles instance to a different filesystem
         * backend. Payload format: service\0root_path\0 — packed by
         * dobfiles_OpenMount() in the EPS stub.
         *
         * Accept unconditionally — the OpenMount stub now routes the
         * call to a specific port only when the originator (a
         * DobFiles in its Monta view) explicitly asked for it. If
         * the message arrives here, by definition someone wanted us
         * to take this mount. */
        const char *p = (const char *)msg->payload;
        uint32_t avail = msg->payload_size;

        /* Defensive: ensure both strings are NUL-terminated within
         * the payload before we touch them. */
        uint32_t slen = 0;
        while (slen < avail && p[slen] != '\0') slen++;
        if (slen >= avail) { goto mount_fail; }

        const char *service = p;
        const char *root    = p + slen + 1;
        uint32_t rlen = 0;
        while ((slen + 1 + rlen) < avail && root[rlen] != '\0') rlen++;
        if ((slen + 1 + rlen) >= avail) { goto mount_fail; }

        dobfs_set_service(service);
        strncpy(current_path, (rlen > 0) ? root : "/", sizeof(current_path) - 1);
        current_path[sizeof(current_path) - 1] = '\0';

        /* If the Monta view was up, drop it: the mount is the answer
         * to that view's pending question, and the user expects the
         * window to flip back to a filesystem listing — not stay on
         * the picker. (Without this the new mount is silently
         * installed but the user still sees the device grid until
         * they toggle Monta off manually.) */
        data_source = SRC_DIR;
        scroll_offset = 0;
        sel_clear();
        selected = -1;

        /* Reply BEFORE read_directory (v1.0.1.1 deadlock fix).
         *
         * read_directory() below issues synchronous dobfs calls
         * (READDIR, STAT) back into the mounted service. If the
         * caller IS that service — which is exactly what happens
         * when a removable-media driver such as floppy surfaces
         * itself via dobfiles_OpenMount() from inside its own
         * probe_media handler — keeping the caller blocked here
         * would deadlock: the caller can't serve our callback
         * because it's still waiting for our reply. Replying
         * first releases it so it returns to its dispatch loop
         * in time to answer us. Any non-self caller is unaffected
         * — for them the reply simply arrives a few microseconds
         * earlier. */
        dob_msg_t reply = {0};
        reply.arg0 = 0;
        dob_ipc_reply(msg->sender_tid, &reply);

        read_directory();
        update_panel();
        redraw();
        return;

    mount_fail:
        {
            dob_msg_t reply = {0};
            reply.arg0 = (uint32_t)(-1);
            dob_ipc_reply(msg->sender_tid, &reply);
        }
    }
    else if (msg->code == FILES_CMD_DROP_DONE)
    {
        /* A drag we originated has finished on its target. Refresh
         * to reflect any files that were moved out from under us.
         * Skip if a popup-input is active (e.g. rename) so we don't
         * yank entries out from beneath the user. Skip in dialog
         * mode and SRC_MOUNTS — neither is a normal directory view. */
        if (!dialog_mode && data_source == SRC_DIR)
        {
            read_directory();
            update_panel();
            redraw();
        }
        /* Post-only: no reply. */
    }
    else if (msg->code == FILES_BULK_TRY && g_is_primary)
    {
        /* A DobFiles instance (this one or a satellite) wants the
         * exclusive bulk-operation lock. arg0 = caller's port.
         * Payload = label string (for error display). */
        uint32_t caller_port = msg->arg0;
        const char *label = msg->payload ? (const char *)msg->payload : "operazione";

        dob_msg_t reply = {0};
        if (bulk_local_try(label, caller_port))
        {
            reply.arg0 = 0;  /* granted */
        }
        else
        {
            reply.arg0 = 1;  /* busy */
            reply.payload      = bulk_holder_label;
            reply.payload_size = strlen(bulk_holder_label) + 1;
        }
        dob_ipc_reply(msg->sender_tid, &reply);
    }
    else if (msg->code == FILES_BULK_RELEASE && g_is_primary)
    {
        uint32_t caller_port = msg->arg0;
        bulk_local_release(caller_port);
        dob_msg_t reply = {0};
        reply.arg0 = 0;
        dob_ipc_reply(msg->sender_tid, &reply);
    }
    else
    {
        dob_msg_t reply = {0};
        reply.arg0 = (uint32_t)(-1);
        dob_ipc_reply(msg->sender_tid, &reply);
    }
}

/* Chunked operation timer */

void event_tick(void)
{
    if (op.active)
        op_do_chunk();
}

/* Drag & drop target */

void event_drop(int lx, int ly,
                const char *const *services,
                const char *const *paths, int n_paths, bool is_copy)
{
    (void)lx; (void)ly;
    if (!paths || n_paths <= 0) return;
    if (op.active) return;   /* already busy */

    /* Resolve the destination directory.
     *
     *   - drag_hover_idx >= 0 and points at a folder  -> drop INTO that
     *     folder (universal "drop onto folder" semantics).
     *   - otherwise drop into the directory currently being shown.
     *
     * We capture and clear drag_hover_idx now so the highlight goes
     * away no matter which path we take next (success, failure, or
     * early return).  The redraw at the end of the routine actually
     * paints the cleared state. */
    int hover_at_drop = drag_hover_idx;
    drag_hover_idx = -1;

    char dest_path[MAX_PATH];
    if (hover_at_drop >= 0 && hover_at_drop < entry_count
        && entries[hover_at_drop].is_dir)
    {
        build_full_path(dest_path, entries[hover_at_drop].name);
    }
    else
    {
        size_t cp_len = strlen(current_path);
        if (cp_len + 1 >= sizeof(dest_path))
        {
            redraw();
            return;
        }
        memcpy(dest_path, current_path, cp_len + 1);
    }

    /* No-op detection: drop landed in the same (service, directory)
     * the files already live in. Happens whenever the user drags
     * files within their own window and releases on empty space (or
     * on a non-folder item) — nothing to do, just clear the hover
     * highlight. Compared per-path to handle mixed payloads.
     *
     * The dest service is whatever this window is currently bound
     * to; the source service for each path comes in `services[]`
     * (empty string == default service, matching the same here->
     * default convention used below for cross-volume detection). */
    {
        const char *dest_svc = dobfs_get_service();
        bool dest_default = (!dest_svc || strcmp(dest_svc, "DobFileSystem") == 0);
        bool all_in_place = true;
        for (int i = 0; i < n_paths; i++)
        {
            const char *sp = paths[i];
            const char *ss = (services && services[i]) ? services[i] : "";
            bool src_default_i = (ss[0] == '\0');
            bool same_svc = (src_default_i && dest_default) ||
                            (!src_default_i && !dest_default && strcmp(ss, dest_svc) == 0);
            if (!same_svc) { all_in_place = false; break; }

            /* Compare the path's parent dir to dest_path. The parent
             * is the substring up to the last '/'; root paths have
             * parent "/". No allocation — just an offset compare. */
            const char *last_slash = NULL;
            for (const char *q = sp; *q; q++)
                if (*q == '/') last_slash = q;
            const char *parent_end = last_slash ? last_slash : sp;
            size_t parent_len = (size_t)(parent_end - sp);
            if (parent_len == 0) parent_len = 1;  /* "/x" -> parent "/" */
            if (parent_len != strlen(dest_path)) { all_in_place = false; break; }
            if (memcmp(sp, dest_path, parent_len) != 0) { all_in_place = false; break; }
        }
        if (all_in_place)
        {
            redraw();   /* clear stale hover */
            return;
        }
    }

    /* Move-vs-copy autodetect.
     *
     * The is_copy flag from the WM reflects the Space modifier at drop
     * time — not the final intent. A drag is cross-volume when the
     * source service differs from this window's binding. With explicit
     * service info per path we can decide without comparing paths.
     *
     * Truth table:
     *   same-vol,  Space=0  -> move
     *   same-vol,  Space=1  -> copy
     *   cross-vol, *        -> copy   (Rename refuses cross-FS) */
    const char *here = dobfs_get_service();
    bool here_default = (!here || strcmp(here, "DobFileSystem") == 0);
    const char *src_svc = (services && services[0]) ? services[0] : "";
    bool src_default = (src_svc[0] == '\0');
    bool cross_vol = !((src_default && here_default) ||
                       (!src_default && !here_default && strcmp(src_svc, here) == 0));

    bool effective_copy = cross_vol ? true : is_copy;

    const char *kind_label = effective_copy ? "Copia" : "Spostamento";
    char busy_label[64];
    if (!bulk_try_begin(kind_label, busy_label, sizeof(busy_label)))
    {
        char m[160];
        snprintf(m, sizeof(m),
                 "Impossibile effettuare questa operazione,\n"
                 "un'altra operazione e' in corso: %s", busy_label);
        dobpopup_Error("File", m);
        redraw();   /* clear stale hover */
        return;
    }

    int kind = effective_copy ? OP_PASTE_COPY : OP_PASTE_MOVE;

    /* Reject self-recursive drop: copying/moving a folder into
     * itself or one of its descendants would loop forever. Has
     * to come after bulk_try_begin (otherwise we'd skip the busy
     * check) but before op_start_from_paths (otherwise we'd start
     * the op and have to abort mid-way). */
    if (op_first_recursive_src(services, paths, n_paths, dest_path) >= 0)
    {
        bulk_release();
        dobpopup_Error("File",
            "Impossibile copiare una cartella dentro se' stessa.");
        redraw();   /* clear stale hover */
        return;
    }

    /* Remember the source so op_finish() can ping it once the chunked
     * op finishes. Captured here because dobui_drop_source_port() is
     * only valid inside event_drop. */
    uint32_t src_port = dobui_drop_source_port();
    op_start_from_paths(kind, services, paths, n_paths, dest_path, kind_label);
    op.drop_notify_port = src_port;
    redraw();   /* clear stale hover */
}

/* Drag source end notification */

void event_drag_end(int committed)
{
    drag_armed       = false;
    drag_in_progress = false;
    /* The hover highlight is target-side state; if the user dragged
     * back over this window without dropping, clear it so the cell
     * doesn't stay lit. */
    drag_hover_idx   = -1;

    /* If the drop was committed by the WM, the target window has performed
     * (or queued) the file operation and our own listing is potentially
     * stale.  Re-read the current directory so any moved-out entries
     * disappear from view.
     *
     * For pure copies this is a visual no-op (sources still exist), but
     * it costs only one filesystem listing and avoids tracking
     * move-vs-copy intent across the drag session.  We do NOT re-read on
     * cancelled drops because nothing on disk has changed. */
    if (committed)
        read_directory();

    redraw();
}

/* Continuous hover notification from the WM during a drag session.
 * The WM posts this for every mouse move while a drag is in flight,
 * including over the source window itself. Mirror the existing
 * drop-hover highlight logic: light up a folder under the cursor
 * (cyan in the listing), clear it on any non-folder hit. SRC_MOUNTS
 * and dialog mode have no drop semantics — ignore drag-over there. */
void event_drag_over(int lx, int ly)
{
    if (dialog_mode) return;
    if (data_source == SRC_MOUNTS) return;

    int new_hover = -1;
    /* Cursor inside the listing area? Map to an entry. */
    if (ly >= view_list_top())
    {
        int idx = view_idx_at(lx, ly);
        if (idx >= 0 && idx < entry_count && entries[idx].is_dir)
            new_hover = idx;
    }
    if (new_hover != drag_hover_idx)
    {
        drag_hover_idx = new_hover;
        redraw();
    }
}

/* Main */

void event_start(void)
{
    win_id = dobui_window();
    win_w = dobui_width();
    win_h = dobui_height();

    /* Show loading message while waiting for filesystem */
    dobui_FillRect(win_id, 0, 0, win_w, win_h, COL_BG);
    dobui_DrawText(win_id, 16, win_h / 2 - 8, "Caricamento...",
                   COL_TEXT, COL_BG);
    dobui_Invalidate(win_id);

    /* One-shot dialog process: register the unique service the picker
     * stub is waiting on, then idle. The OPEN/SAVE request lands in
     * event_request -> handle_dialog_request, which drives the rest.
     * No explorer init, and no "DobFiles" registry slot.
     *
     * We DO init the ribbon though: in dialog mode the picker exposes
     * Vista (list/icons) and Monta (jump to floppy/CD) on the ribbon
     * — the rest of the buttons are hidden by ribbon_draw's minimal
     * filter. Without this init the dialog window would have a blank
     * ribbon strip. */
    if (g_is_dialog)
    {
        ribbon_init();
        dob_registry_register(g_dialog_svc, dobui_port());
        return;
    }

    /* Mount handoff. If main() parsed "--mount <service> <root>"
     * from argv, this process was spawned by a removable-media driver
     * through dobfiles_OpenMount() and must attach itself to that
     * service rather than behaving as the primary filesystem browser.
     * Using argv rather than shared DobConfig keys means this handoff
     * is per-spawn — no leakage across processes, no race window if a
     * second OpenMount runs while we're still initialising. Satellite
     * instances do NOT register as "DobFiles" in the registry —
     * dialog services go to the primary instance only. */
    bool is_satellite = false;
    if (g_mount_service && g_mount_service[0])
    {
        const char *root = (g_mount_root && g_mount_root[0]) ? g_mount_root : "/";

        dobfs_set_service(g_mount_service);
        strncpy(current_path, root, sizeof(current_path) - 1);
        current_path[sizeof(current_path) - 1] = '\0';
        is_satellite = true;

        /* Unit number for the unmount subscription is taken from the
         * first digit of the root path (floppy "/u0", "/u1"). When
         * the root carries no digit (cdrom passes "/") the driver
         * ignores the field anyway. */
        uint8_t unit = 0;
        for (int i = 0; root[i]; i++)
        {
            if (root[i] >= '0' && root[i] <= '9')
            {
                unit = (uint8_t)(root[i] - '0');
                break;
            }
        }

        uint32_t svc_port = dob_registry_wait(g_mount_service, 1000);
        if (svc_port)
        {
            dob_msg_t sm = {0}, sr = {0};
            sm.code = DOBFS_SUBSCRIBE_UNMOUNT;
            sm.arg0 = dobui_port();
            sm.arg1 = unit;
            (void)dob_ipc_call(svc_port, &sm, &sr);
        }
    }

    /* Only the primary instance owns the "DobFiles" registry slot
     * (used for file-open/save dialog requests from other programs).
     * Satellite windows stay off the registry. */
    g_is_primary = !is_satellite;
    if (!is_satellite)
        dob_registry_register("DobFiles", dobui_port());

    /* Build the ribbon now that we have a real win_id. The dialog
     * fast-exit above does its own ribbon_init for the minimal
     * Vista+Monta strip; primary explorer / satellite mount instances
     * land here for the full toolbar. */
    ribbon_init();

    read_directory();
    update_panel();
    redraw();
}

int main(int argc, char **argv)
{
    /* Parse "--mount <service> <root>" out of argv. All other forms
     * (empty argv, unrelated flags) mean "primary mode": the globals
     * stay NULL and event_start runs as the primary file browser.
     * Doing the parse here rather than in event_start keeps the argv
     * window narrow — once dobui_run takes over, argv is no longer
     * referenced. */
    for (int i = 0; i + 2 < argc; i++)
    {
        if (argv[i] && strcmp(argv[i], "--mount") == 0)
        {
            g_mount_service = argv[i + 1];
            g_mount_root    = argv[i + 2];
            break;
        }
    }

    /* Parse "--dialog <service>": spawned by the file-picker stub.
     * This instance becomes a one-shot OpenFile/SaveFile dialog and
     * registers <service> (a name unique to that picker call) rather
     * than "DobFiles". */
    for (int i = 0; i + 1 < argc; i++)
    {
        if (argv[i] && strcmp(argv[i], "--dialog") == 0)
        {
            g_is_dialog  = true;
            g_dialog_svc = argv[i + 1];
            break;
        }
    }

    /* Declare the view-style setting (Elenco / Icone) and read it to
     * seed the initial view, so the choice survives a restart. The user
     * can change it either in DobSettings OR with the ribbon's Vista
     * toggle: the toggle now persists the choice via writeSetting (its
     * own file, user-initiated), so the two stay in sync. Skip for the
     * one-shot file dialog, which has no ribbon. */
    if (!g_is_dialog)
    {
        static const char *const VIEW_OPTS[] = { "Elenco", "Icone", 0 };
        declareSetting("view.style", SETTING_ENUM,
                       "Vista predefinita", "Elenco", VIEW_OPTS);
        const char *v = getSetting("view.style");
        if (v && strcmp(v, "Icone") == 0)
            view_style = VIEW_ICONS;
        else
            view_style = VIEW_LIST;
    }

    dobui_run(g_is_dialog ? "Dialogo file" : "File", WIN_W, WIN_H);
    return 0;
}
