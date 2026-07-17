/* DobInterface stub — cmdbuf-based retained-mode display list.
 *
 * Drawing primitives accumulate into a per-context command buffer
 * instead of writing into a shared framebuffer.  At Invalidate the
 * cmdbuf ships as a single IPC payload to dobinterface, which
 * resets the window's dv_cmdlist and replays each record.
 *
 * Compatibility: the public dobui_* signatures and documented
 * "Begin / N ops / Invalidate" semantics are unchanged.  Apps must
 * re-emit the full window content on every Invalidate — the cmdbuf
 * resets to empty after each flush, so a partial Invalidate after
 * an incremental redraw produces a window with only the redrawn
 * portion present.
 *
 * Windows have no per-window SHM (the compositor walks dv_cmdlist
 * directly).  The shm_id field in the wire protocol is reserved at
 * -1 for ABI stability.  Widgets still allocate a small SHM since
 * the widget tray composes via a CPU rasterizer. */

#include <DobInterface.h>
#include <dob/ipc.h>
#include <dob/reconnect.h>
#include <dob/font.h>
#include <dob/spawn.h>
#include <dob/dobui_cmdbuf.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ============================================================================
 *  IPC plumbing
 * ============================================================================ */

static uint32_t ui_port = 0;
#define UI_CALL(m, r) _dob_call_reconnect(&ui_port, "dobinterface", 5000, m, r)

#define GUI_WIN_CREATE          100
#define GUI_WIN_DESTROY         101
#define GUI_WIN_SET_TITLE       102
#define GUI_WIN_RAISE           103
#define GUI_WIN_HIDE            104
#define GUI_WIN_SET_FLAGS       105   /* arg0=win, arg1=DOBUI_WIN_* bitmask */
#define GUI_WIN_SET_PARENT      106   /* arg0=child, arg1=parent, arg2=flags */
#define GUI_WIN_PARENT_MODAL    0x1   /* arg2 bit: child blocks the parent  */
#define GUI_WIN_INVALIDATE      115
#define GUI_PANEL_SET_CMDS      120
#define GUI_PANEL_CLEAR_CMDS    121
#define GUI_SET_CURSOR          122
#define GUI_SPAWN_DRIVER        130
#define GUI_BEGIN_DRAG          131
#define GUI_CANCEL_DRAG         132

#define GUI_WIDGET_CREATE       140
#define GUI_WIDGET_DESTROY      141
#define GUI_WIDGET_INVALIDATE   142

/* Texture-pool opcodes — used by large BlitBuffer calls. */
#define GUI_WIN_TEX_ALLOC       145   /* arg0=win, arg1=w, arg2=h, reply.arg0=handle */
#define GUI_WIN_TEX_UPDATE      146   /* arg0=win, arg1=handle, payload=pixels */
#define GUI_WIN_SHM_ENSURE      148   /* arg1=w, arg2=h -> reply arg1=shm_id */
#define GUI_WIN_TEX_FREE        147   /* arg0=win, arg1=handle */

/* ============================================================================
 *  Per-context state
 *
 *  Two drawing contexts per process (window, widget); Save/Restore
 *  switches between them.  Each context owns its own cmdbuf and
 *  texture pool (handles are scoped per-window/widget server-side).
 * ============================================================================ */

/* Texture-pool depth.  Every BlitBuffer call lands here now (the
 * inline fast path was retired due to a cross-window scratch
 * texture race, see DOBUI_BLIT_INLINE_THRESHOLD_BYTES = 0).
 *
 * DobFiles, the heaviest user, blits ~10 distinct rasters (8 ribbon
 * icons + 2 grid icons); 16 gives generous headroom for future
 * widgets. Each entry costs `w × h × 4` VRAM on the server only when
 * actually used. Eviction is first-fit-replace.  MUST stay equal to
 * WIN_TEX_POOL_SIZE in dobinterface/main.c -- the server rejects
 * allocations past its own cap. */
#define TEX_POOL_SIZE   16

typedef struct
{
    uint16_t w, h;
    uint32_t handle;
    const uint32_t *last_src;     /* last pixel pointer uploaded -- skip if same */
    uint32_t used_seq;            /* frame sequence this slot was last referenced */
} tex_pool_entry_t;

typedef struct
{
    /* Identity */
    uint32_t            id;            /* window_id or widget_id, 0 = unused */
    int                 fb_w, fb_h;    /* logical dimensions, used for cheap clip */
    int                 shm_id;        /* widget SHM id, or -1 for window contexts */

    /* Pannello SHM del contenuto (solo contesti finestra): buffer
     * condiviso mappato dove l'app disegna direttamente; il record
     * DOBUI_OP_BLIT_SHMPANEL lo fa blittare al bake senza che i pixel
     * attraversino l'IPC. panel_shm_id < 0 = non richiesto/attivo. */
    int                 panel_shm_id;
    uint32_t           *panel_ptr;
    int                 panel_w, panel_h;

    /* Command buffer.  buf == NULL until first append. */
    uint8_t            *cmdbuf;
    uint32_t            cmdbuf_size;   /* bytes written including 8-byte header */
    uint32_t            cmdbuf_cap;
    bool                cmdbuf_overflow;

    /* Server-side texture pool (lazy populated by large BlitBuffer) */
    tex_pool_entry_t    tex[TEX_POOL_SIZE];
    uint32_t            tex_count;
    uint32_t            tex_seq;       /* bumped each frame (at cmdbuf reset) */
} dobui_ctx_t;

/* Window contexts.
 *
 * DobFiles (and any app using a progress popup / secondary tracked
 * window) can have more than one live window at a time. Each window
 * needs its own tex_pool because the server keeps the pool per
 * window_id and resets it on win_free_video (resize, hide,
 * destroy). If we shared a single client-side mirror, clobbering it
 * on CreateWindow #2 would desync the main window's pool — handles
 * still alive on the server side would be marked dead client-side,
 * and the next BlitBuffer would allocate fresh ones, eventually
 * overflowing WIN_TEX_POOL_SIZE on the server (16 slots).
 *
 * Two slots used to be enough (main + progress).  With first-class
 * sub-windows (a process can now own several real windows at once --
 * a manager plus N child windows, dialogs, modals) the table is sized
 * for a realistic per-process fan-out.  Each idle slot costs only its
 * tex_pool array; the cmdbuf stays NULL until the window first draws.
 * MUST be >= the most windows one process keeps open simultaneously;
 * the WM itself caps at MAX_WINDOWS (64) across all processes. */
#define WIN_CTX_SLOTS  16
static dobui_ctx_t  g_win_ctx_slots[WIN_CTX_SLOTS];
static dobui_ctx_t *g_win_ctx = &g_win_ctx_slots[0];  /* legacy alias = primary slot */
static dobui_ctx_t  g_wid_ctx;
static dobui_ctx_t *g_active = &g_win_ctx_slots[0];

/* Find the slot owning a given window id, or NULL. */
static dobui_ctx_t *win_ctx_find(uint32_t win_id)
{
    if (win_id == 0) return NULL;
    for (int i = 0; i < WIN_CTX_SLOTS; i++)
        if (g_win_ctx_slots[i].id == win_id)
            return &g_win_ctx_slots[i];
    return NULL;
}

/* Find a free slot (id==0) or NULL if all in use. */
static dobui_ctx_t *win_ctx_alloc(void)
{
    for (int i = 0; i < WIN_CTX_SLOTS; i++)
        if (g_win_ctx_slots[i].id == 0)
            return &g_win_ctx_slots[i];
    return NULL;
}

/* Resize / unmap guard.  Drawing during a resize is a no-op; the
 * field is kept as part of the public ABI (dobui_IsResizePending). */
static bool g_resize_pending = false;

/* ============================================================================
 *  cmdbuf helpers
 *
 *  Append-only, little-endian bytes written by hand.  Grow-on-demand
 *  doubling up to DOBUI_CMDBUF_MAX_BYTES; past the cap the overflow
 *  flag is set, subsequent appends are no-ops, and Invalidate drops
 *  the cmdbuf with a debug print.  Runaway redraws degrade to a
 *  blank window rather than corrupted memory.
 * ============================================================================ */

static bool cmdbuf_ensure(dobui_ctx_t *c, uint32_t extra)
{
    if (c->cmdbuf_overflow) return false;

    /* First-time init.  Reserve the header up front so the offset
     * arithmetic below treats every append as "body bytes". */
    if (!c->cmdbuf)
    {
        c->cmdbuf = (uint8_t *)malloc(DOBUI_CMDBUF_INITIAL_BYTES);
        if (!c->cmdbuf) { c->cmdbuf_overflow = true; return false; }
        c->cmdbuf_cap  = DOBUI_CMDBUF_INITIAL_BYTES;
        c->cmdbuf_size = DOBUI_CMDBUF_HDR_SIZE;
        /* Write header lazily here -- we know we'll need it. */
        c->cmdbuf[0] = (uint8_t)( DOBUI_CMDBUF_MAGIC        & 0xFF);
        c->cmdbuf[1] = (uint8_t)((DOBUI_CMDBUF_MAGIC >>  8) & 0xFF);
        c->cmdbuf[2] = (uint8_t)((DOBUI_CMDBUF_MAGIC >> 16) & 0xFF);
        c->cmdbuf[3] = (uint8_t)((DOBUI_CMDBUF_MAGIC >> 24) & 0xFF);
        c->cmdbuf[4] = 0; c->cmdbuf[5] = 0; c->cmdbuf[6] = 0; c->cmdbuf[7] = 0;
    }

    uint64_t need = (uint64_t)c->cmdbuf_size + (uint64_t)extra;
    if (need <= c->cmdbuf_cap) return true;

    if (need > DOBUI_CMDBUF_MAX_BYTES)
    {
        c->cmdbuf_overflow = true;
        debug_print("[dobui] cmdbuf overflow -- frame dropped\n");
        return false;
    }

    uint32_t new_cap = c->cmdbuf_cap;
    while (new_cap < need) new_cap *= 2;
    if (new_cap > DOBUI_CMDBUF_MAX_BYTES) new_cap = DOBUI_CMDBUF_MAX_BYTES;

    uint8_t *nb = (uint8_t *)realloc(c->cmdbuf, new_cap);
    if (!nb) { c->cmdbuf_overflow = true; return false; }
    c->cmdbuf = nb;
    c->cmdbuf_cap = new_cap;
    return true;
}

static inline void cmdbuf_put_u8(dobui_ctx_t *c, uint8_t v)
{
    c->cmdbuf[c->cmdbuf_size++] = v;
}
static inline void cmdbuf_put_u16(dobui_ctx_t *c, uint16_t v)
{
    c->cmdbuf[c->cmdbuf_size++] = (uint8_t)( v       & 0xFF);
    c->cmdbuf[c->cmdbuf_size++] = (uint8_t)((v >> 8) & 0xFF);
}
static inline void cmdbuf_put_i16(dobui_ctx_t *c, int16_t v)
{
    cmdbuf_put_u16(c, (uint16_t)v);
}
static inline void cmdbuf_put_u32(dobui_ctx_t *c, uint32_t v)
{
    c->cmdbuf[c->cmdbuf_size++] = (uint8_t)( v        & 0xFF);
    c->cmdbuf[c->cmdbuf_size++] = (uint8_t)((v >>  8) & 0xFF);
    c->cmdbuf[c->cmdbuf_size++] = (uint8_t)((v >> 16) & 0xFF);
    c->cmdbuf[c->cmdbuf_size++] = (uint8_t)((v >> 24) & 0xFF);
}
static inline void cmdbuf_put_bytes(dobui_ctx_t *c, const void *p, uint32_t n)
{
    memcpy(c->cmdbuf + c->cmdbuf_size, p, n);
    c->cmdbuf_size += n;
}

/* Reset for a fresh frame.  Called after a successful Invalidate
 * flush so the next round of FillRect/DrawText/... starts on an
 * empty buffer.  We KEEP the allocation -- realloc-on-grow has a
 * cost worth amortizing, and typical UI redraws hover around the
 * same byte count frame after frame. */
static void cmdbuf_reset(dobui_ctx_t *c)
{
    c->cmdbuf_size     = c->cmdbuf ? DOBUI_CMDBUF_HDR_SIZE : 0;
    c->cmdbuf_overflow = false;
    c->tex_seq++;      /* frame boundary: entries touched before this are now evictable */
}

static void cmdbuf_free(dobui_ctx_t *c)
{
    if (c->cmdbuf) { free(c->cmdbuf); c->cmdbuf = NULL; }
    c->cmdbuf_size     = 0;
    c->cmdbuf_cap      = 0;
    c->cmdbuf_overflow = false;
}

/* Cheap "is this op trivially outside the window?" test.  Done
 * client-side as a micro-opt: an offscreen FillRect or DrawText
 * still costs 13+ bytes in the cmdbuf and one dv_cmdlist_*
 * server-side; rejecting it here saves both.  Apps that draw
 * scrolled content sometimes emit a lot of these. */
static inline bool trivially_offscreen(int x, int y, int w, int h)
{
    if (!g_active->fb_w || !g_active->fb_h) return false;  /* unknown dims, keep */
    if (x + w <= 0)             return true;
    if (y + h <= 0)             return true;
    if (x >= g_active->fb_w)    return true;
    if (y >= g_active->fb_h)    return true;
    return false;
}

/* ============================================================================
 *  Texture pool (server-side handles cached by the stub)
 *
 *  Large BlitBuffer calls trigger lazy allocation of a server-side
 *  texture.  Same dimensions reuse the handle and only re-upload
 *  when the source pointer changes (cheap aliasing check).  Pool
 *  is small (one or two distinct raster sizes per window in
 *  practice); eviction on overflow is first-fit-replace.
 * ============================================================================ */

static tex_pool_entry_t *tex_pool_get_or_alloc(uint16_t w, uint16_t h,
                                               const uint32_t *src)
{
    if (!g_active->id) return NULL;     /* no live context */

    /* Match on the full (w, h, src) tuple. Keying on (w, h) alone
     * would make distinct rasters of the same size share a single
     * texture handle — the queued BLIT_TEX records all reference the
     * same handle, the cmdlist replay reads whichever upload
     * happened last, and earlier blits in the frame draw the wrong
     * raster. */
    for (uint32_t i = 0; i < g_active->tex_count; i++)
        if (g_active->tex[i].w == w && g_active->tex[i].h == h
            && g_active->tex[i].last_src == src)
        {
            g_active->tex[i].used_seq = g_active->tex_seq;   /* referenced this frame */
            return &g_active->tex[i];
        }

    /* Allocate a slot. When the pool is full, evict the least-recently-used
     * slot that was NOT referenced in the current frame: freeing a handle a
     * BLIT_TEX already queued this frame would leave that blit reading a
     * recycled texture -- exactly the "gears take the games ghost" aliasing
     * seen while scrolling a grid of >16 distinct rasters. Only if every slot
     * is in use this frame (a frame genuinely needing more than TEX_POOL_SIZE
     * distinct rasters) do we fall back to slot 0 and accept the glitch. */
    uint32_t slot;
    bool     fresh = false;
    if (g_active->tex_count < TEX_POOL_SIZE)
    {
        slot = g_active->tex_count++;
        fresh = true;
    }
    else
    {
        slot = TEX_POOL_SIZE;                       /* sentinel: none chosen yet */
        uint32_t oldest = 0;
        for (uint32_t i = 0; i < TEX_POOL_SIZE; i++)
            if (g_active->tex[i].used_seq != g_active->tex_seq
                && (slot == TEX_POOL_SIZE || g_active->tex[i].used_seq < oldest))
            { slot = i; oldest = g_active->tex[i].used_seq; }
        if (slot == TEX_POOL_SIZE) slot = 0;        /* all in use this frame */
        if (g_active->tex[slot].handle != 0)
        {
            dob_msg_t m = {0}, r = {0};
            m.code = GUI_WIN_TEX_FREE;
            m.arg0 = g_active->id;
            m.arg1 = g_active->tex[slot].handle;
            UI_CALL(&m, &r);
        }
    }

    dob_msg_t am = {0}, ar = {0};
    am.code = GUI_WIN_TEX_ALLOC;
    am.arg0 = g_active->id;
    am.arg1 = (uint32_t)w;
    am.arg2 = (uint32_t)h;
    if (UI_CALL(&am, &ar) != DOB_OK || ar.arg0 == 0)
    {
        debug_print("[dobui] tex_alloc failed\n");
        if (fresh) g_active->tex_count--;
        return NULL;
    }

    g_active->tex[slot].w        = w;
    g_active->tex[slot].h        = h;
    g_active->tex[slot].handle   = ar.arg0;
    g_active->tex[slot].last_src = NULL;   /* upload still pending */
    g_active->tex[slot].used_seq = g_active->tex_seq;   /* referenced this frame */
    return &g_active->tex[slot];
}

static void tex_pool_upload(tex_pool_entry_t *t, const uint32_t *src)
{
    /* A bande di righe intere: il buffer IPC del ricevente strippa i
     * payload oltre il suo tetto (vedi dobui_cmdbuf.h), quindi una
     * testura grande (la pagina di un editor) non puo' viaggiare in un
     * messaggio solo. arg2 = riga iniziale, arg3 = numero righe; le
     * testure piccole restano UN messaggio (banda unica). */
    uint32_t row_bytes = (uint32_t)t->w * 4u;
    uint32_t rows_per  = DOBUI_CMDBUF_SEG_BYTES / row_bytes;
    if (rows_per == 0) rows_per = 1;

    for (uint32_t row0 = 0; row0 < (uint32_t)t->h; row0 += rows_per)
    {
        uint32_t nrows = (uint32_t)t->h - row0;
        if (nrows > rows_per) nrows = rows_per;

        dob_msg_t um = {0}, ur = {0};
        um.code         = GUI_WIN_TEX_UPDATE;
        um.arg0         = g_active->id;
        um.arg1         = t->handle;
        um.arg2         = row0;
        um.arg3         = nrows;
        um.payload      = (void *)(src + row0 * (uint32_t)t->w);
        um.payload_size = nrows * row_bytes;
        UI_CALL(&um, &ur);
    }
    t->last_src = src;
}

static void tex_pool_free_all(dobui_ctx_t *c)
{
    for (uint32_t i = 0; i < c->tex_count; i++)
    {
        if (c->tex[i].handle == 0) continue;
        dob_msg_t m = {0}, r = {0};
        m.code = GUI_WIN_TEX_FREE;
        m.arg0 = c->id;
        m.arg1 = c->tex[i].handle;
        UI_CALL(&m, &r);
    }
    c->tex_count = 0;
    memset(c->tex, 0, sizeof(c->tex));
}

/* ============================================================================
 *  Window lifecycle
 * ============================================================================ */

uint32_t dobui_CreateWindow(int width, int height, uint32_t owner_port,
                            const char *title)
{
    /* Pick a free slot up front so we don't bother the server if all
     * slots are full (would leak the server-side window). */
    dobui_ctx_t *slot = win_ctx_alloc();
    if (!slot)
    {
        debug_print("[dobui] CreateWindow: no free window slot\n");
        return 0;
    }

    dob_msg_t msg = {0}, reply = {0};
    msg.code         = GUI_WIN_CREATE;
    msg.arg0         = (uint32_t)width;
    msg.arg1         = (uint32_t)height;
    msg.arg2         = owner_port;
    msg.payload      = (void *)title;
    msg.payload_size = title ? strlen(title) + 1 : 0;

    if (UI_CALL(&msg, &reply) != DOB_OK) return 0;

    uint32_t win_id = reply.arg0;
    if (win_id == 0) return 0;

    /* Populate the allocated slot — do NOT memset over an existing
     * slot's tex_pool: that loses the mirror of the live server-side
     * pool for whatever window owned the slot before. */
    memset(slot, 0, sizeof(*slot));
    slot->id      = win_id;
    slot->fb_w    = width;
    slot->fb_h    = height;
    slot->shm_id  = -1;          /* -1 marks a window context */
    slot->panel_shm_id = -1;
    slot->panel_ptr    = NULL;
    slot->panel_w = slot->panel_h = 0;
    g_active = slot;

    return win_id;
}

void dobui_DestroyWindow(uint32_t win_id)
{
    /* Locate the slot that owns this window. Falls back to the
     * primary slot to preserve the legacy behaviour where callers
     * destroy "the" window without passing the matching id (rare
     * but possible). */
    dobui_ctx_t *slot = win_ctx_find(win_id);
    if (!slot) slot = g_win_ctx;

    tex_pool_free_all(slot);
    cmdbuf_free(slot);

    dob_msg_t msg = {0}, reply = {0};
    msg.code = GUI_WIN_DESTROY;
    msg.arg0 = win_id;
    UI_CALL(&msg, &reply);

    /* Mark the slot free. If it was the active one, fall back to
     * any other live window slot, or the widget context, or the
     * primary slot as a last resort. */
    bool was_active = (g_active == slot);
    memset(slot, 0, sizeof(*slot));
    if (was_active)
    {
        g_active = NULL;
        for (int i = 0; i < WIN_CTX_SLOTS; i++)
            if (g_win_ctx_slots[i].id)
            {
                g_active = &g_win_ctx_slots[i];
                break;
            }
        if (!g_active) g_active = g_wid_ctx.id ? &g_wid_ctx : g_win_ctx;
    }
}

void dobui_SetTitle(uint32_t win_id, const char *title)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code         = GUI_WIN_SET_TITLE;
    msg.arg0         = win_id;
    msg.payload      = (void *)title;
    msg.payload_size = title ? strlen(title) + 1 : 0;
    UI_CALL(&msg, &reply);
}

void dobui_SetWindowFlags(uint32_t win_id, uint32_t flags)
{
    /* Synchronous: by the time this returns the WM has applied the
     * flags, so resize/maximize are gated before the window draws. */
    dob_msg_t msg = {0}, reply = {0};
    msg.code = GUI_WIN_SET_FLAGS;
    msg.arg0 = win_id;
    msg.arg1 = flags;
    UI_CALL(&msg, &reply);
}

void dobui_SetParent(uint32_t child_id, uint32_t parent_id, bool modal)
{
    /* Establish (or, with parent_id == 0, drop) the owner relationship
     * for a sub-window/dialog/popup.  Call right after CreateWindow,
     * before the first Invalidate -- same timing rule as
     * dobui_SetWindowFlags, so a modal is blocking and a child is
     * stacked above its parent before either is ever drawn.
     * Synchronous: the relationship is in force by the time this
     * returns.  parent_id may belong to another process; it's a window
     * id, not a pid. */
    dob_msg_t msg = {0}, reply = {0};
    msg.code = GUI_WIN_SET_PARENT;
    msg.arg0 = child_id;
    msg.arg1 = parent_id;
    msg.arg2 = modal ? GUI_WIN_PARENT_MODAL : 0;
    UI_CALL(&msg, &reply);
}

void dobui_Raise(uint32_t win_id)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = GUI_WIN_RAISE;
    msg.arg0 = win_id;
    UI_CALL(&msg, &reply);
}

void dobui_Hide(uint32_t win_id)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = GUI_WIN_HIDE;
    msg.arg0 = win_id;
    UI_CALL(&msg, &reply);
}

bool dobui_IsResizePending(void)
{
    return g_resize_pending;
}

void dobui_HandleResize(int new_w, int new_h, int new_shm_id)
{
    /* Windows have no SHM; the next Invalidate will emit a fresh
     * cmdbuf at the new dimensions.  new_shm_id is reserved at -1
     * in the wire protocol and ignored — kept for ABI stability.
     *
     * libdobui calls us only for the main window (the one dobui_run
     * created), and at this point the active slot is that same
     * main window (event dispatch hasn't moved g_active). Resetting
     * the active slot's pool is the right scope. */
    (void)new_shm_id;
    g_resize_pending = true;
    g_active->fb_w = new_w;
    g_active->fb_h = new_h;

    /* The WM destroys every texture in the window's pool on resize
     * (win_apply_geometry -> win_free_video). Without dropping our
     * mirror cache here, the next BlitBuffer would call
     * tex_pool_get_or_alloc, find a matching (w, h, src) entry,
     * issue BLIT_TEX with the now-dangling handle, and the WM would
     * paint nothing (or a blank square). Reset the count so the
     * next blit re-allocates and re-uploads. We do NOT send
     * GUI_WIN_TEX_FREE for the dead handles — the WM has already
     * freed them and would just reject the calls. */
    g_active->tex_count = 0;
    for (uint32_t i = 0; i < TEX_POOL_SIZE; i++)
    {
        g_active->tex[i].handle   = 0;
        g_active->tex[i].last_src = NULL;
    }

    g_resize_pending = false;
}

/* ============================================================================
 *  Active drawing context
 * ============================================================================ */

/* Make `win_id`'s context the active one for subsequent draw calls.
 * Each window the process opens has its own slot (cmdbuf + texture
 * pool); the app framework calls this before delivering an event to a
 * window, so handlers draw into their own window with no manual
 * bookkeeping.  No-op if the id isn't one of this process's window
 * slots (the active context is left untouched). */
void dobui_SetActiveWindow(uint32_t win_id)
{
    dobui_ctx_t *slot = win_ctx_find(win_id);
    if (slot) g_active = slot;
}

/* ============================================================================
 *  Drawing API — each function appends one record to g_active->cmdbuf.
 *  No pixel written client-side.  Clipping is the server's job; we do
 *  a cheap trivial-reject here to avoid bloating the cmdbuf with
 *  fully-offscreen ops.
 * ============================================================================ */

void dobui_FillRect(uint32_t win_id, int x, int y, int w, int h, uint32_t color)
{
    (void)win_id;     /* matches g_active->id; multi-window-per-process not supported */
    if (g_resize_pending || w <= 0 || h <= 0) return;
    if (trivially_offscreen(x, y, w, h)) return;
    if (!cmdbuf_ensure(g_active, DOBUI_REC_FILL_RECT_SZ)) return;

    cmdbuf_put_u8 (g_active, (uint8_t)DOBUI_OP_FILL_RECT);
    cmdbuf_put_i16(g_active, (int16_t)x);
    cmdbuf_put_i16(g_active, (int16_t)y);
    cmdbuf_put_u16(g_active, (uint16_t)w);
    cmdbuf_put_u16(g_active, (uint16_t)h);
    cmdbuf_put_u32(g_active, color);
}

void dobui_DrawRect(uint32_t win_id, int x, int y, int w, int h, uint32_t color)
{
    (void)win_id;
    if (g_resize_pending || w <= 0 || h <= 0) return;
    if (trivially_offscreen(x, y, w, h)) return;
    if (!cmdbuf_ensure(g_active, DOBUI_REC_DRAW_RECT_SZ)) return;

    cmdbuf_put_u8 (g_active, (uint8_t)DOBUI_OP_DRAW_RECT);
    cmdbuf_put_i16(g_active, (int16_t)x);
    cmdbuf_put_i16(g_active, (int16_t)y);
    cmdbuf_put_u16(g_active, (uint16_t)w);
    cmdbuf_put_u16(g_active, (uint16_t)h);
    cmdbuf_put_u32(g_active, color);
}

void dobui_DrawPixel(uint32_t win_id, int x, int y, uint32_t color)
{
    (void)win_id;
    if (g_resize_pending) return;
    if (trivially_offscreen(x, y, 1, 1)) return;
    if (!cmdbuf_ensure(g_active, DOBUI_REC_DRAW_PIXEL_SZ)) return;

    cmdbuf_put_u8 (g_active, (uint8_t)DOBUI_OP_DRAW_PIXEL);
    cmdbuf_put_i16(g_active, (int16_t)x);
    cmdbuf_put_i16(g_active, (int16_t)y);
    cmdbuf_put_u32(g_active, color);
}

void dobui_DrawText(uint32_t win_id, int x, int y, const char *text,
                    uint32_t fg, uint32_t bg)
{
    (void)win_id;
    if (g_resize_pending || !text) return;

    uint32_t len = (uint32_t)strlen(text);
    if (len == 0) return;
    if (len > 0xFFFFu) len = 0xFFFFu;   /* wire-format cap (u16 length field) */

    /* Trivial reject: the text spans len*FONT_W pixels horizontally. */
    if (trivially_offscreen(x, y, (int)len * DOB_FONT_W, DOB_FONT_H)) return;

    uint32_t need = DOBUI_REC_DRAW_TEXT_HDR + len;
    if (!cmdbuf_ensure(g_active, need)) return;

    cmdbuf_put_u8   (g_active, (uint8_t)DOBUI_OP_DRAW_TEXT);
    cmdbuf_put_i16  (g_active, (int16_t)x);
    cmdbuf_put_i16  (g_active, (int16_t)y);
    cmdbuf_put_u32  (g_active, fg);
    cmdbuf_put_u32  (g_active, bg);
    cmdbuf_put_u16  (g_active, (uint16_t)len);
    cmdbuf_put_bytes(g_active, text, len);
}

/* Like dobui_DrawText, but asks the compositor to lay the run out
 * monospace (fixed 8px pitch). Editable text fields use this so their
 * cell-based cursor/selection geometry keeps matching what is drawn. */
void dobui_DrawTextFixed(uint32_t win_id, int x, int y, const char *text,
                         uint32_t fg, uint32_t bg)
{
    (void)win_id;
    if (g_resize_pending || !text) return;

    uint32_t len = (uint32_t)strlen(text);
    if (len == 0) return;
    if (len > 0xFFFFu) len = 0xFFFFu;

    if (trivially_offscreen(x, y, (int)len * DOB_FONT_W, DOB_FONT_H)) return;

    uint32_t need = DOBUI_REC_DRAW_TEXT_HDR + len;
    if (!cmdbuf_ensure(g_active, need)) return;

    cmdbuf_put_u8   (g_active, (uint8_t)DOBUI_OP_DRAW_TEXT_FIXED);
    cmdbuf_put_i16  (g_active, (int16_t)x);
    cmdbuf_put_i16  (g_active, (int16_t)y);
    cmdbuf_put_u32  (g_active, fg);
    cmdbuf_put_u32  (g_active, bg);
    cmdbuf_put_u16  (g_active, (uint16_t)len);
    cmdbuf_put_bytes(g_active, text, len);
}

void dobui_BlitBuffer(uint32_t win_id, int x, int y,
                      const uint32_t *src, int src_w, int src_h)
{
    (void)win_id;
    if (g_resize_pending || !src || src_w <= 0 || src_h <= 0) return;
    if (src_w > 0xFFFF || src_h > 0xFFFF) return;
    if (trivially_offscreen(x, y, src_w, src_h)) return;

    uint64_t bytes = (uint64_t)src_w * (uint64_t)src_h * 4ull;

    if (bytes <= DOBUI_BLIT_INLINE_THRESHOLD_BYTES)
    {
        /* Inline path: pixels travel inside the cmdbuf record. */
        uint32_t need = DOBUI_REC_BLIT_INLINE_HDR + (uint32_t)bytes;
        if (!cmdbuf_ensure(g_active, need)) return;

        cmdbuf_put_u8   (g_active, (uint8_t)DOBUI_OP_BLIT_INLINE);
        cmdbuf_put_i16  (g_active, (int16_t)x);
        cmdbuf_put_i16  (g_active, (int16_t)y);
        cmdbuf_put_u16  (g_active, (uint16_t)src_w);
        cmdbuf_put_u16  (g_active, (uint16_t)src_h);
        cmdbuf_put_bytes(g_active, src, (uint32_t)bytes);
        return;
    }

    /* Texture-handle path: pool entry keyed by (w, h, src).  Different
     * rasters of the same size are stored in separate handles so they
     * don't fight; identical (w, h, src) hits the cached entry with
     * no re-upload (icon/preview blitted unchanged every redraw). */
    tex_pool_entry_t *t = tex_pool_get_or_alloc((uint16_t)src_w, (uint16_t)src_h, src);
    if (!t) return;                /* alloc failed; silently drop */

    if (t->last_src != src) tex_pool_upload(t, src);

    if (!cmdbuf_ensure(g_active, DOBUI_REC_BLIT_TEX_SZ)) return;
    cmdbuf_put_u8 (g_active, (uint8_t)DOBUI_OP_BLIT_TEX);
    cmdbuf_put_i16(g_active, (int16_t)x);
    cmdbuf_put_i16(g_active, (int16_t)y);
    cmdbuf_put_u32(g_active, t->handle);
    cmdbuf_put_u16(g_active, (uint16_t)src_w);
    cmdbuf_put_u16(g_active, (uint16_t)src_h);
}

/* ----------------------------------------------------------------------------
 *  dobui_BlitBufferDynamic — like dobui_BlitBuffer, but for buffers whose
 *  PIXELS CHANGE between redraws (an editor page, a live preview, a video
 *  frame). The pooled-texture path keys on (w, h, src) and skips the GPU
 *  upload when the source pointer is unchanged — correct and fast for static
 *  icons, but it freezes the texture at first content for a buffer that the
 *  app keeps mutating in place. This variant resolves the same pool entry but
 *  ALWAYS re-uploads, so the on-screen result tracks the current pixels.
 *  Purely additive: dobui_BlitBuffer is unchanged. Cost is one texture upload
 *  per call, so callers should use it only for genuinely dynamic buffers.
 * -------------------------------------------------------------------------- */
void dobui_BlitBufferDynamic(uint32_t win_id, int x, int y,
                             const uint32_t *src, int src_w, int src_h)
{
    (void)win_id;
    if (g_resize_pending || !src || src_w <= 0 || src_h <= 0) return;
    if (src_w > 0xFFFF || src_h > 0xFFFF) return;
    if (trivially_offscreen(x, y, src_w, src_h)) return;

    tex_pool_entry_t *t = tex_pool_get_or_alloc((uint16_t)src_w, (uint16_t)src_h, src);
    if (!t) return;
    tex_pool_upload(t, src);          /* unconditional: the buffer is dynamic */

    if (!cmdbuf_ensure(g_active, DOBUI_REC_BLIT_TEX_SZ)) return;
    cmdbuf_put_u8 (g_active, (uint8_t)DOBUI_OP_BLIT_TEX);
    cmdbuf_put_i16(g_active, (int16_t)x);
    cmdbuf_put_i16(g_active, (int16_t)y);
    cmdbuf_put_u32(g_active, t->handle);
    cmdbuf_put_u16(g_active, (uint16_t)src_w);
    cmdbuf_put_u16(g_active, (uint16_t)src_h);
}

/* Upload di una banda di righe [row0, row0+nrows) — stesso protocollo
 * riga-based di tex_pool_upload (GUI_WIN_TEX_UPDATE gia' porta
 * arg2=riga, arg3=numero righe): zero cambi lato server. */
static void tex_pool_upload_rows(tex_pool_entry_t *t, const uint32_t *src,
                                 uint32_t row0, uint32_t nrows)
{
    uint32_t row_bytes = (uint32_t)t->w * 4u;
    uint32_t rows_per  = DOBUI_CMDBUF_SEG_BYTES / row_bytes;
    if (rows_per == 0) rows_per = 1;

    uint32_t end = row0 + nrows;
    if (end > (uint32_t)t->h) end = (uint32_t)t->h;

    for (uint32_t r0 = row0; r0 < end; r0 += rows_per)
    {
        uint32_t nr = end - r0;
        if (nr > rows_per) nr = rows_per;

        dob_msg_t um = {0}, ur = {0};
        um.code         = GUI_WIN_TEX_UPDATE;
        um.arg0         = g_active->id;
        um.arg1         = t->handle;
        um.arg2         = r0;
        um.arg3         = nr;
        um.payload      = (void *)(src + r0 * (uint32_t)t->w);
        um.payload_size = nr * row_bytes;
        UI_CALL(&um, &ur);
    }
    t->last_src = src;
}

/* ----------------------------------------------------------------------------
 *  dobui_BlitBufferDynamicRows — Dynamic con upload a banda: per buffer
 *  di cui il chiamante SA quali righe sono cambiate (l'editor dopo un
 *  reflow incrementale). Taglia l'upload da tutto il buffer alle sole
 *  righe sporche — la parte piu' costosa del percorso Dynamic sui
 *  buffer grandi. Entry nuova o puntatore cambiato => upload integrale
 *  (lato server le righe fuori banda non esistono ancora).
 * -------------------------------------------------------------------------- */
void dobui_BlitBufferDynamicRows(uint32_t win_id, int x, int y,
                                 const uint32_t *src, int src_w, int src_h,
                                 int dirty_row0, int dirty_rows)
{
    (void)win_id;
    if (g_resize_pending || !src || src_w <= 0 || src_h <= 0) return;
    if (src_w > 0xFFFF || src_h > 0xFFFF) return;
    if (trivially_offscreen(x, y, src_w, src_h)) return;

    tex_pool_entry_t *t = tex_pool_get_or_alloc((uint16_t)src_w, (uint16_t)src_h, src);
    if (!t) return;

    if (t->last_src != src || dirty_row0 < 0 || dirty_rows <= 0
        || dirty_row0 >= src_h)
        tex_pool_upload(t, src);                 /* integrale */
    else
        tex_pool_upload_rows(t, src, (uint32_t)dirty_row0,
                             (uint32_t)dirty_rows);

    if (!cmdbuf_ensure(g_active, DOBUI_REC_BLIT_TEX_SZ)) return;
    cmdbuf_put_u8 (g_active, (uint8_t)DOBUI_OP_BLIT_TEX);
    cmdbuf_put_i16(g_active, (int16_t)x);
    cmdbuf_put_i16(g_active, (int16_t)y);
    cmdbuf_put_u32(g_active, t->handle);
    cmdbuf_put_u16(g_active, (uint16_t)src_w);
    cmdbuf_put_u16(g_active, (uint16_t)src_h);
}

/* ----------------------------------------------------------------------------
 *  Pannello SHM del contenuto — il percorso a copia singola
 *
 *  dobui_ShmPanelEnsure chiede al server un framebuffer w x h in
 *  memoria condivisa legato alla finestra e lo mappa; l'app vi disegna
 *  direttamente e dobui_ShmPanelBlit accoda il record che al bake lo
 *  copia nel corpo. I pixel non attraversano MAI l'IPC: rispetto a
 *  BlitBufferDynamic spariscono l'upload a segmenti e la texture di
 *  appoggio. Misura diversa => il server rialloca e l'id cambia: qui
 *  si rimappa in trasparenza. NULL su fallimento (il chiamante puo'
 *  ripiegare sul percorso Dynamic, sempre corretto).
 * -------------------------------------------------------------------------- */
uint32_t *dobui_ShmPanelEnsure(uint32_t win_id, int w, int h)
{
    dobui_ctx_t *c = g_active;
    if (!c || c->shm_id >= 0) return NULL;      /* solo contesti finestra */
    if (w <= 0 || h <= 0 || w > 0xFFFF || h > 0xFFFF) return NULL;
    if (c->panel_ptr && c->panel_w == w && c->panel_h == h)
        return c->panel_ptr;                    /* gia' pronta, stessa misura */

    if (!ui_port) ui_port = dob_registry_find("dobinterface");
    if (!ui_port) return NULL;

    dob_msg_t m = {0}, r = {0};
    m.code = GUI_WIN_SHM_ENSURE;
    m.arg0 = win_id;
    m.arg1 = (uint32_t)w;
    m.arg2 = (uint32_t)h;
    if (UI_CALL(&m, &r) != 0 || r.arg0 != 0) return NULL;

    int new_id = (int)r.arg1;
    if (new_id != c->panel_shm_id)
    {
        if (c->panel_shm_id >= 0) shm_unmap(c->panel_shm_id);
        uint32_t vaddr = 0;           /* la libc passa il vaddr come intero */
        if (shm_map(new_id, &vaddr) != 0 || !vaddr)
        {
            c->panel_shm_id = -1; c->panel_ptr = NULL;
            c->panel_w = c->panel_h = 0;
            return NULL;
        }
        c->panel_shm_id = new_id;
        c->panel_ptr    = (uint32_t *)(uintptr_t)vaddr;
    }
    c->panel_w = w; c->panel_h = h;
    return c->panel_ptr;
}

void dobui_ShmPanelBlit(uint32_t win_id, int x, int y, int w, int h,
                        int band_y0, int band_rows)
{
    (void)win_id;
    dobui_ctx_t *c = g_active;
    if (!c || c->panel_shm_id < 0) return;
    if (!cmdbuf_ensure(c, DOBUI_REC_BLIT_SHMPANEL_SZ)) return;
    uint16_t by0, brs;
    if (band_y0 == (int)DOBUI_SHMPANEL_UNCHANGED
        && band_rows == (int)DOBUI_SHMPANEL_UNCHANGED)
    { by0 = DOBUI_SHMPANEL_UNCHANGED; brs = DOBUI_SHMPANEL_UNCHANGED; }
    else if (band_rows <= 0 || band_y0 < 0 || band_y0 >= h)
    { by0 = 0; brs = 0; }             /* fuori misura: copia integrale */
    else
    { by0 = (uint16_t)band_y0; brs = (uint16_t)band_rows; }
    cmdbuf_put_u8 (c, (uint8_t)DOBUI_OP_BLIT_SHMPANEL);
    cmdbuf_put_i16(c, (int16_t)x);
    cmdbuf_put_i16(c, (int16_t)y);
    cmdbuf_put_u16(c, (uint16_t)w);
    cmdbuf_put_u16(c, (uint16_t)h);
    cmdbuf_put_u16(c, by0);
    cmdbuf_put_u16(c, brs);
}

/* ============================================================================
 *  Invalidate — flush the cmdbuf to dobinterface
 *
 *  Async post (fire-and-forget).  Overflow handling: if the cmdbuf
 *  overflowed the per-frame cap, send a header-only payload — the
 *  server resets the cmdlist and renders an empty window for this
 *  frame, and the next redraw starts fresh.
 * ============================================================================ */

static void invalidate_flush(uint32_t id, uint32_t code, dobui_ctx_t *c)
{
    if (!ui_port) ui_port = dob_registry_find("dobinterface");
    if (!ui_port) return;

    if (g_resize_pending) return;     /* skip flush during transient resize */

    /* If nothing has been drawn this frame, still send a header-only
     * cmdbuf so the server resets the cmdlist (= empty window).
     * Apps that call Invalidate without prior draws are expressing
     * "clear me" intent. */
    uint8_t  empty_hdr[DOBUI_CMDBUF_HDR_SIZE];
    uint8_t *payload = c->cmdbuf;
    uint32_t size    = c->cmdbuf_size;

    if (!payload || size < DOBUI_CMDBUF_HDR_SIZE || c->cmdbuf_overflow)
    {
        empty_hdr[0] = (uint8_t)( DOBUI_CMDBUF_MAGIC        & 0xFF);
        empty_hdr[1] = (uint8_t)((DOBUI_CMDBUF_MAGIC >>  8) & 0xFF);
        empty_hdr[2] = (uint8_t)((DOBUI_CMDBUF_MAGIC >> 16) & 0xFF);
        empty_hdr[3] = (uint8_t)((DOBUI_CMDBUF_MAGIC >> 24) & 0xFF);
        empty_hdr[4] = 0; empty_hdr[5] = 0; empty_hdr[6] = 0; empty_hdr[7] = 0;
        payload = empty_hdr;
        size    = DOBUI_CMDBUF_HDR_SIZE;
    }

    dob_msg_t m = {0};
    m.code = code;
    m.arg0 = id;

    /* Spedizione a segmenti (vedi dobui_cmdbuf.h): il buffer IPC del
     * ricevente strippa in silenzio i payload oltre il suo tetto, e un
     * cmdbuf cresce col contenuto della finestra. Ogni segmento sta
     * largamente sotto il limite; il caso comune (frame piccolo) resta
     * UN solo messaggio. */
    uint32_t total_segs = (size + DOBUI_CMDBUF_SEG_BYTES - 1u)
                        / DOBUI_CMDBUF_SEG_BYTES;
    if (total_segs == 0) total_segs = 1;

    for (uint32_t seq = 0; seq < total_segs; seq++)
    {
        uint32_t off   = seq * DOBUI_CMDBUF_SEG_BYTES;
        uint32_t chunk = size - off;
        if (chunk > DOBUI_CMDBUF_SEG_BYTES) chunk = DOBUI_CMDBUF_SEG_BYTES;

        m.arg1         = seq;
        m.arg2         = total_segs;
        m.arg3         = size;
        m.payload      = payload + off;
        m.payload_size = chunk;
        dob_ipc_post(ui_port, &m);
    }

    cmdbuf_reset(c);
}

void dobui_Invalidate(uint32_t win_id)
{
    /* Honor whatever context is active; if SaveContext was used to
     * temporarily switch to a different context, the caller is the
     * one who knows which window-id matches.  We trust win_id. */
    dobui_ctx_t *c = win_ctx_find(win_id);
    if (!c) c = (win_id == g_wid_ctx.id) ? &g_wid_ctx : g_active;
    invalidate_flush(win_id, GUI_WIN_INVALIDATE, c);
}

void dobui_WidgetInvalidate(uint32_t widget_id)
{
    dobui_ctx_t *c;
    if (widget_id == g_wid_ctx.id)       c = &g_wid_ctx;
    else                                  c = win_ctx_find(widget_id);
    if (!c) c = g_active;
    invalidate_flush(widget_id, GUI_WIDGET_INVALIDATE, c);
}

void dobui_SetPanelCommands(uint32_t win_id, const char *commands)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code         = GUI_PANEL_SET_CMDS;
    msg.arg0         = win_id;
    msg.payload      = (void *)commands;
    msg.payload_size = commands ? strlen(commands) + 1 : 0;
    UI_CALL(&msg, &reply);
}

void dobui_ClearPanelCommands(uint32_t win_id)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = GUI_PANEL_CLEAR_CMDS;
    msg.arg0 = win_id;
    UI_CALL(&msg, &reply);
}

int dobui_SetCursor(uint32_t win_id, int cursor_type)
{
    if (!ui_port) ui_port = dob_registry_find("dobinterface");
    if (!ui_port) return -1;

    dob_msg_t m = {0};
    m.code = GUI_SET_CURSOR;
    m.arg0 = win_id;
    m.arg1 = (uint32_t)cursor_type;     /* sign-extends CURSOR_DEFAULT=-1 to 0xFFFFFFFF */
    dob_ipc_post(ui_port, &m);
    return 0;
}

pid_t dobui_SpawnDriver(const char *path, const char *const argv[])
{
    if (!path) return -1;

    void    *blob = NULL;
    uint32_t blob_size = 0;
    if (_spawn_build_blob(argv, &blob, &blob_size) < 0) return -1;

    uint32_t path_len = strlen(path) + 1;
    uint32_t total    = path_len + blob_size;

    char *payload = (char *)malloc(total);
    if (!payload) { free(blob); return -1; }
    memcpy(payload, path, path_len);
    if (blob_size > 0) memcpy(payload + path_len, blob, blob_size);

    dob_msg_t msg = {0}, reply = {0};
    msg.code         = GUI_SPAWN_DRIVER;
    msg.payload      = payload;
    msg.payload_size = total;
    msg.arg0         = (blob_size > 0) ? path_len : 0;

    dob_status_t rc = UI_CALL(&msg, &reply);
    free(payload);
    free(blob);

    return (rc != DOB_OK) ? -1 : (pid_t)(int32_t)reply.arg0;
}

/* ============================================================================
 *  Widget API
 * ============================================================================ */

uint32_t dobui_CreateWidget(int width, int height, uint32_t owner_port)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = GUI_WIDGET_CREATE;
    msg.arg0 = (uint32_t)width;
    msg.arg1 = (uint32_t)height;
    msg.arg2 = owner_port;

    if (UI_CALL(&msg, &reply) != DOB_OK) return 0;

    uint32_t wid    = reply.arg0;
    int      shm_id = (int)reply.arg1;
    if (wid == 0) return 0;

    /* Widget SHM is real and active: the server-side widget tray
     * composes via a CPU rasterizer that writes into this buffer
     * (widget_replay_cmdbuf_to_shm) and then uploads it to a texture.
     * Map it now and keep it for the widget's lifetime. */
    if (shm_id >= 0)
    {
        uint32_t dummy = 0;
        if (shm_map(shm_id, &dummy) != 0)
            debug_print("[dobui] widget SHM map failed\n");
    }

    memset(&g_wid_ctx, 0, sizeof(g_wid_ctx));
    g_wid_ctx.id     = wid;
    g_wid_ctx.fb_w   = width;
    g_wid_ctx.fb_h   = height;
    g_wid_ctx.shm_id = shm_id;
    g_active = &g_wid_ctx;
    return wid;
}

void dobui_DestroyWidget(uint32_t widget_id)
{
    tex_pool_free_all(&g_wid_ctx);
    cmdbuf_free(&g_wid_ctx);

    if (g_wid_ctx.shm_id >= 0) shm_unmap(g_wid_ctx.shm_id);

    dob_msg_t msg = {0}, reply = {0};
    msg.code = GUI_WIDGET_DESTROY;
    msg.arg0 = widget_id;
    UI_CALL(&msg, &reply);

    memset(&g_wid_ctx, 0, sizeof(g_wid_ctx));
    if (g_active == &g_wid_ctx)
    {
        g_active = NULL;
        for (int i = 0; i < WIN_CTX_SLOTS; i++)
            if (g_win_ctx_slots[i].id)
            {
                g_active = &g_win_ctx_slots[i];
                break;
            }
        if (!g_active) g_active = g_win_ctx;
    }
}

void dobui_WidgetSaveContext(dobui_widget_fb_ctx_t *ctx)
{
    /* Widget contexts use `fb` for its original purpose: pointer to
     * the widget's SHM-mapped framebuffer. Keep the legacy contents
     * — disambiguating between widget contexts uses shm_id, which is
     * unique per widget. */
    ctx->fb     = NULL;
    ctx->fb_w   = g_active->fb_w;
    ctx->fb_h   = g_active->fb_h;
    ctx->shm_id = g_active->shm_id;
}

void dobui_WidgetRestoreContext(const dobui_widget_fb_ctx_t *ctx)
{
    if (ctx->shm_id == g_wid_ctx.shm_id && g_wid_ctx.id) g_active = &g_wid_ctx;
    else if (ctx->shm_id == g_win_ctx->shm_id && g_win_ctx->id) g_active = g_win_ctx;
}

static void *drag_pack_paths(const char *const *services,
                             const char *const *paths, int n, bool is_cut,
                             uint32_t *out_size)
{
    if (n < 0) n = 0;
    if (n > 65535) n = 65535;

    uint32_t total = 1 + 2;
    for (int i = 0; i < n; i++)
    {
        const char *svc = services ? services[i] : NULL;
        total += (uint32_t)(svc ? strlen(svc) : 0) + 1u;
        total += (uint32_t)(paths[i] ? strlen(paths[i]) : 0) + 1u;
    }

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return NULL;

    uint32_t off = 0;
    buf[off++] = is_cut ? 1 : 0;
    buf[off++] = (uint8_t)(n & 0xFF);
    buf[off++] = (uint8_t)((n >> 8) & 0xFF);
    for (int i = 0; i < n; i++)
    {
        const char *svc = services ? services[i] : NULL;
        const char *p   = paths[i] ? paths[i] : "";
        uint32_t    sl  = (uint32_t)(svc ? strlen(svc) : 0);
        uint32_t    pl  = (uint32_t)strlen(p);
        if (svc && sl) memcpy(buf + off, svc, sl);
        off += sl; buf[off++] = '\0';
        memcpy(buf + off, p, pl);
        off += pl; buf[off++] = '\0';
    }

    *out_size = total;
    return buf;
}

int dobui_BeginDragOn(uint32_t src_win_id,
                      const char *const *services,
                      const char *const *paths, int n_paths, bool is_cut)
{
    if (!paths || n_paths <= 0) return -1;

    uint32_t size = 0;
    void *buf = drag_pack_paths(services, paths, n_paths, is_cut, &size);
    if (!buf) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code         = GUI_BEGIN_DRAG;
    msg.arg0         = src_win_id;
    msg.payload      = buf;
    msg.payload_size = size;

    dob_status_t st = UI_CALL(&msg, &reply);
    free(buf);
    return (st != DOB_OK) ? -1 : (int)reply.arg0;
}

int dobui_BeginDrag(uint32_t src_win_id,
                    const char *const *paths, int n_paths, bool is_cut)
{
    return dobui_BeginDragOn(src_win_id, NULL, paths, n_paths, is_cut);
}

void dobui_CancelDrag(void)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = GUI_CANCEL_DRAG;
    UI_CALL(&msg, &reply);
}
