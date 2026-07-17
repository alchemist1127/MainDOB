/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * MainDOB ATI Mach64 driver — internal state.
 *
 * Mirrors bga_state.h's role: shared between main.c (dv_* impls), the
 * transport files (need caller pid + state pointer), and the per-area
 * modules (modeset, 2d, irq, cursor).
 *
 * KEY DIFFERENCE FROM BGA: SINGLE FRAMEBUFFER, NO PAGE FLIP.
 *
 * The Mobility-P / LT Pro variants on the E500 carry 8 MB / 4 MB of
 * SGRAM respectively.  A 1024x768 32-bit primary buffer is 3 MB; a
 * 16-bit one is 1.5 MB.  Reserving a back page would halve usable
 * offscreen VRAM and on the 4 MB LT Pro variant leave ~1 MB free for
 * everything else (surfaces, cursor, glyph atlas, layer cache).
 * Instead we keep a SINGLE primary at VRAM offset 0 and compose
 * directly into it.  dv_page_flip becomes a no-op that returns
 * DV_OK — clients keep working, they just don't get the tear-free
 * swap they would on BGA.  The Mach64 2D engine BitBlts a typical
 * compose frame in <8 ms, so a vblank-aligned dv_compose call gives
 * effectively tear-free output without dedicated double-buffering.
 *
 * Quotas, surface IDs, layer IDs, cmdlist storage layout, etc., all
 * follow BGA's pattern unchanged — these tables are protocol-level
 * concerns, not hardware concerns.
 */

#ifndef MAINDOB_DRIVERS_X3100_STATE_H
#define MAINDOB_DRIVERS_X3100_STATE_H

#include <dob/types.h>
#include <dob/hotplug_driver.h>
#include <dob/video.h>

#include "x3100_hw.h"

/* Resource table caps — same as BGA so clients see identical limits. */
#define MAX_VPROCS              16
#define MAX_SURFACES            128
#define MAX_BUFFERS             64
#define MAX_FENCES              256
#define MAX_LAYERS              64
#define MAX_VTHREADS            32
#define MAX_SUBSCRIBERS         8
#define MAX_MODE_LIST           8
#define MAX_CMDLISTS            128
#define CMDLIST_STORAGE_BYTES   (8 * 1024 * 1024)

typedef uint32_t bgra_t;

typedef struct vram_block
{
    uint32_t offset;
    uint32_t size;
    bool     used;
    struct vram_block *prev, *next;
} vram_block_t;

typedef struct
{
    bool      used;
    pid_t     owner_pid;
    uint64_t  vram_quota_bytes;
    uint64_t  vram_used_bytes;
    uint32_t  vthreads_active;
    uint32_t  fences_in_flight;
} vproc_t;

typedef struct
{
    bool         used;
    dv_vproc_t   owner;
    uint32_t     width, height;
    uint32_t     pitch_words;       /* pitch in pixels (8-aligned by Mach64) */
    dv_format_t  format;
    uint32_t     flags;
    uint32_t     vram_offset;       /* byte offset in VRAM */
    uint32_t     vram_bytes;
    /* Direct handle to the allocator block backing this surface. Stored at
     * creation so destroy frees by pointer instead of recovering the block
     * via vram_find(offset) — that lookup returned NULL (and SILENTLY skipped
     * the free, leaking the block forever) whenever the stored offset didn't
     * exactly match a block boundary, e.g. after alignment padding. NULL only
     * for surfaces with no backing block. */
    vram_block_t *block;
    /* Backing SYSRAM (DV_SURF_FLAG_SYSRAM): pixel in RAM di sistema del
     * driver — malloc alla create, free alla destroy. NULL = surface in
     * VRAM (vram_offset/block validi). Con sys_pixels attivo il motore
     * 2D NON puo' toccare questa surface (legge/scrive solo VRAM): ogni
     * accesso passa dai percorsi CPU (surface_pixels in main.c). E' il
     * backing di backbuf e corpi finestra della dobinterface 1.1: in
     * VRAM resta solo lo scanout. */
    uint8_t      *sys_pixels;
    /* Damage tracking: gen e' l'epoca di scrittura (seme univoco alla
     * create, bump a ogni mutazione dei pixel), dirty_* il box sporco
     * accumulato dalle mutazioni (vuoto se x0>=x1). La compose in
     * modalita' shadow li usa per ricomporre SOLO cio' che e' cambiato
     * dal frame precedente; azzerati dopo ogni compose che li consuma. */
    uint32_t      gen;
    int32_t       dirty_x0, dirty_y0, dirty_x1, dirty_y1;
} surface_t;

/* Snapshot di un layer come composto nell'ultimo frame in modalita'
 * shadow: la compose lo confronta col layer attuale per calcolare il
 * danno (geometria/z/alpha diversi -> vecchia U nuova area; solo
 * contenuto -> box sporco della sorgente traslato a schermo). */
typedef struct
{
    bool      used;
    bool      is_cmdlist;
    int32_t   z;
    uint8_t   alpha;
    bool      use_pixel_alpha;
    int32_t   surf_slot;      /* slot della surface sorgente, -1 se ignota */
    uint32_t  src_gen;        /* gen della sorgente al momento della compose */
    dv_rect_t src_rect;
    int32_t   dst_x, dst_y;
} composed_layer_t;

typedef struct
{
    bool          used;
    dv_vproc_t    owner;
    uint64_t      bytes;
    uint32_t      vram_offset;
    /* Free by this pointer on teardown, exactly like surface_t (avoids the
     * vram_find(offset) lookup that can miss after alignment padding and
     * silently orphan the block).  NULL until a dv_vram_alloc implementation
     * sets it — the x3100 transport does not currently route dv_vram_alloc,
     * so no buffers are created today; teardown falls back to vram_find. */
    vram_block_t *block;
} buffer_t;

typedef struct
{
    bool        used;
    dv_vproc_t  owner;
    uint64_t    target_value;
    uint64_t    current_value;
} fence_t;

typedef struct
{
    bool         used;
    dv_vproc_t   owner;
    dv_surface_t source;
    int32_t      z;
    uint8_t      alpha;
    bool         visible;
    bool         use_pixel_alpha;
    dv_rect_t    src_rect;
    dv_rect_t    dst_rect;
    dv_cmdlist_t cmdlist;
} layer_t;

typedef struct
{
    bool       used;
    dv_vproc_t owner;
    uint32_t   storage_off;
    uint32_t   capacity;
    uint32_t   bytes_used;
    uint32_t   command_count;
    /* true if this list begins by filling the whole screen with an opaque
     * rect — replaying it overwrites every pixel, so dv_compose can skip the
     * full-screen clear and avoid the per-frame black flash. */
    bool       covers_fullscreen;
} cmdlist_t;

typedef struct
{
    bool       used;
    dv_vproc_t owner;
    uint32_t   worker_tid;
    uint8_t    priority;
} vthread_t;

typedef struct
{
    bool     used;
    uint32_t port;
    uint32_t mask;
} subscriber_t;

/* Variant identification — set at attach from PCI device_id, gates
 * the few code paths in modeset that differ between chips. */
typedef enum
{
    X3100_VARIANT_UNKNOWN     = 0,
    X3100_VARIANT_MOBILITY_P  = 1,   /* 1002:4C4D */
    X3100_VARIANT_LT_PRO      = 2,   /* 1002:4C42 */
} x3100_variant_t;

/* Panel native timing, read from the SHADOW CRTC bank at attach.
 * The BIOS has programmed the shadow set with the LCD panel's exact
 * required timings (we never need to compute them — bigger win than
 * any DAS-based table lookup). */
typedef struct
{
    uint32_t h_tot_disp;          /* CRTC_H_TOTAL_DISP raw value */
    uint32_t h_sync_strt_wid;     /* CRTC_H_SYNC_STRT_WID raw value */
    uint32_t v_tot_disp;          /* CRTC_V_TOTAL_DISP raw value */
    uint32_t v_sync_strt_wid;     /* CRTC_V_SYNC_STRT_WID raw value */
    uint32_t off_pitch;           /* CRTC_OFF_PITCH raw value */
    /* Decoded visible dimensions for convenience. */
    uint16_t width;
    uint16_t height;
} panel_native_t;

/* PLL snapshot read at attach.  Sufficient to feed compute_dsp() and,
 * if needed in future, to reconstruct the current pixel/memory clocks.
 * v15 reads but never reprograms — x3100_pll_program() exists but is
 * not called from the bring-up path. */
typedef struct
{
    uint8_t  pll_ref_div;
    uint8_t  mclk_fb_div;
    uint8_t  vclk_fb_div;
    uint8_t  vclk_post_div_real;
    uint8_t  xclk_post_div;
    uint8_t  xclk_ref_div;
    uint8_t  mclk_fb_mult;
    uint8_t  active_vclk_idx;     /* 0..3 from CLOCK_CNTL byte 0 */
    /* Memory-controller derived parameters needed by aty_dsp_gt formula. */
    uint32_t fifo_size;
    uint32_t dsp_loop_latency;
    uint32_t xclkpagefaultdelay;
    uint32_t xclkmaxrasdelay;
} dsp_pll_t;

typedef struct
{
    hotplug_device_t  dev;

    /* Hardware base addresses. */
    volatile bgra_t  *vram;          /* mapped LFB (BAR0) */
    uint32_t          vram_phys;
    uint32_t          vram_bytes;    /* probed from CONFIG_STAT0 */
    volatile uint8_t *mmio;          /* mapped MMIO BAR0 (GTTMMADR); byte-offset + cast access */
    uint32_t          mmio_phys;
    uint16_t          io_base;       /* unused on x3100 (kept for mach64 parity) */
    uint8_t           irq_line;

    /* ---- x3100 GTT / aperture / render-ring backing (replaces mach64's
     * LFB + PLL model).  `vram` above is the CPU window into the VRAM pool
     * through the GMADR aperture; `mmio` is BAR0.  The 2D engine writes via
     * GTT graphics addresses; the display scans the same physical pages. ---- */
    volatile uint32_t *gtt;            /* MMIO + 512KB: PTE array            */
    uint32_t           gtt_entries;
    volatile uint8_t  *gmadr;          /* BAR2 aperture base (CPU window)    */
    uint32_t           gmadr_phys;
    uint32_t           gmadr_bytes;
    uint32_t           vram_gtt_index; /* first GTT PTE of the VRAM pool     */
    volatile uint32_t *ring;           /* render command ring (CPU pointer)  */
    uint32_t           ring_gtt;       /* ring graphics address (RING_START) */
    uint32_t           ring_bytes;
    uint32_t           ring_tail;

    x3100_variant_t  variant;
    uint32_t          chip_id;       /* CONFIG_CHIP_ID raw */

    /* ---- Chipset generation + panel-pipe register set (runtime-selected).
     * GM965/Crestline (2A02/2A03, Extensa 5220): validated on pipe/plane/
     * cursor B, GTT at BAR0+512KB — the historical constants.
     * GM45/Cantiga (2A42/2A43, e.g. Compaq CQ62): same g4x-family display
     * block, but GTT at BAR0+2MB and the LVDS pipe is READ from LVDS[30]
     * at init (never assumed).  Every display-side access goes through
     * these fields so one binary serves both machines. ---- */
    bool              is_g4x;         /* true = GM45/Cantiga               */
    uint32_t          native_w, native_h;  /* panel mode adopted at init   */
    uint32_t          scan_pitch_px;   /* scanout pitch in px: width*4
                                        * rounded UP to 64 bytes (gen4
                                        * linear-plane stride rule).
                                        * 1280 -> 1280 (already aligned);
                                        * 1366 -> 1376. Every scanout-page
                                        * consumer (DSPSTRIDE, primary
                                        * surface pitch, shadow present,
                                        * page sizes) uses THIS, never
                                        * mode.width. */
    uint32_t          reg_dsp_cntr, reg_dsp_linoff,
                      reg_dsp_stride, reg_dsp_surf;
    uint32_t          reg_pipe_src;   /* absolute PIPExSRC address         */
    uint32_t          reg_pipe_dsl;   /* absolute PIPExDSL (scan line)     */
    uint32_t          reg_cur_cntr, reg_cur_base, reg_cur_pos;
    uint32_t          dsp_cntr_enable; /* composed DSPxCNTR for our pipe   */
    uint32_t          cur_cntr_on;     /* cursor mode incl. pipe select    */
    uint8_t           chip_rev;
    uint8_t           ram_type;      /* M64_RAM_* from CNFG_STAT0[2:0] */

    /* Current display mode (single buffer, no back page). */
    dv_mode_t         mode;
    dv_mode_t         mode_list_buf[MAX_MODE_LIST];
    uint32_t          mode_list_n;

    /* Primary buffer: VRAM offset 0, sized by current mode. */
    uint32_t          primary_offset; /* always 0 — kept for symmetry with BGA */
    uint32_t          primary_bytes;

    /* Double-buffer (real page-flip). EVEREST on the E500 shows 4136 KiB
     * VRAM free against a 3072 KiB second page, so a full back page fits
     * with ~1 MB to spare. dv_compose draws into the *back* page entirely
     * off-screen (a full recompose is ~9.5 ms = 12x the 784 us vblank, so
     * it can NEVER fit in the blank if drawn into the live primary), then
     * dv_page_flip swaps CRTC_OFF_PITCH at vblank — an atomic register
     * write that always fits. This is what kills the flicker on mouse
     * move / click / drag / resize.
     *
     *   front_offset = page the CRTC is currently scanning out (visible)
     *   back_offset  = page dv_compose draws into (hidden)
     * They swap on every successful flip. If the back page could NOT be
     * allocated (very tight VRAM), back_offset == front_offset == 0 and
     * the driver transparently degrades to the old single-buffer
     * behaviour (compose straight into the visible primary, flip no-op).
     */
    uint32_t          front_offset;   /* visible page (CRTC scanout) */
    uint32_t          back_offset;    /* hidden page (compose target) */
    bool              double_buffered;/* true iff a distinct back page exists */
    /* Shadow frame (SOLO frame singolo, double_buffered==false): la
     * compose piena baka qui, in RAM di sistema, l'intero frame come
     * pixel finali; dv_page_flip lo presenta con UNA copia sequenziale
     * sul front — l'unica scrittura che lo scanout veda mai. Elimina
     * le tempeste di flash della compose diretta nel visibile (sfondo
     * atterrato, finestre non ancora) senza spendere i 3 MB di VRAM
     * della back page: e' il gemello in RAM dello schema del BGA.
     * NULL = shadow non allocata (malloc fallita o doppio frame
     * attivo): compose diretta, corretta ma con strati visibili. */
    uint8_t          *shadow;
    /* Damage tracking della compose shadow: composed[] e' lo snapshot
     * per-slot dei layer come composti nell'ultimo frame
     * (composed_valid=false al boot => primo frame a danno pieno);
     * present_* e' il bbox da presentare, UNIONE dei danni delle
     * compose avvenute dal page_flip precedente (vuoto se x0>=x1).
     * Invariante di correttezza: la presentazione copia SOLO regioni
     * appena ricomposte nella shadow — cio' che sta fuori (es. le
     * patch in place di compose_rect sul front) non viene mai toccato,
     * quindi non puo' regredire. */
    uint32_t          surface_gen_seed;
    composed_layer_t  composed[MAX_LAYERS];
    bool              composed_valid;
    int32_t           present_x0, present_y0, present_x1, present_y1;

    /* Read once at attach, treated as immutable for the driver lifetime
     * (v15 supports only the panel native mode). */
    panel_native_t    panel;
    dsp_pll_t         pll;

    /* BIOS LCD register snapshot — saved at attach so we can restore
     * sensitive bits (LVDS polarity, dither config, etc.) we never
     * compute ourselves. */
    uint32_t          bios_lcd_gen_cntl;
    uint32_t          bios_lcd_config_panel;
    uint32_t          bios_lcd_horz_stretching;
    uint32_t          bios_lcd_vert_stretching;
    uint32_t          bios_lcd_ext_vert_stretch;
    uint32_t          bios_lcd_index;

    /* VRAM free-list (block allocator, same model as BGA). */
    vram_block_t     *blocks_head;

    /* Resource tables. */
    vproc_t           vprocs[MAX_VPROCS];
    surface_t         surfaces[MAX_SURFACES];
    buffer_t          buffers[MAX_BUFFERS];
    fence_t           fences[MAX_FENCES];
    layer_t           layers[MAX_LAYERS];
    vthread_t         vthreads[MAX_VTHREADS];
    subscriber_t      subs[MAX_SUBSCRIBERS];
    cmdlist_t         cmdlists[MAX_CMDLISTS];

    uint8_t           cmdlist_storage[CMDLIST_STORAGE_BYTES];
    uint32_t          cmdlist_storage_used;

    /* Scanout source (set by dobinterface; informational — we always
     * scan out from primary_offset in single-buffer mode). */
    dv_surface_t      scanout_source;

    /* Vsync counter, advanced by IRQ handler.  Used by dv_vsync_wait
     * and as the implicit clock for fence-on-vblank patterns. */
    volatile uint64_t vsync_count;

    /* HW cursor state cache (so we don't reprogram unchanged regs). */
    bool              cursor_visible;
    uint32_t          cursor_vram_offset;
    int32_t           cursor_x, cursor_y;
    uint32_t          cursor_hotspot_x, cursor_hotspot_y;

    /* 2D engine state — set once after engine reset, used by every
     * primitive.  These mirror DP_PIX_WIDTH / DP_WRITE_MASK for the
     * primary surface format and let primitive functions skip the
     * "reprogram pixel width" step on the common path. */
    uint8_t           engine_pix_width;   /* M64_PIX_WIDTH_* */
    bool              engine_ready;
    /* engine_ok: the 2D engine register sequence is validated/working on
     * this hardware -> use hardware blits.  When false, the 2D primitives
     * fall back to CPU (direct writes to the mapped framebuffer/surfaces),
     * which always works but is slow.  Lets the desktop render even before
     * the accelerator is validated on the silicon. */
    bool              engine_ok;

    /* Off-screen scratch buffer for dirty-rect compose (dv_compose_rect,
     * Option B double-buffering).  The dirty region is composed here —
     * background + windows, in z-order, OUTSIDE the vblank where it can't
     * be seen — then a SINGLE hw_blit copies it to the visible primary at
     * the vblank.  The panel therefore never scans a half-composed region
     * (no see-through flash).  Allocated lazily and grown (freed+realloc)
     * only when a larger rect is needed, so the steady state reuses one
     * block.  scratch_w/h are the buffer's pixel capacity; pitch is
     * 8-aligned like every Mach64 surface. */
    vram_block_t     *scratch_block;
    uint32_t          scratch_vram_offset;
    uint32_t          scratch_w, scratch_h;   /* allocated capacity (px) */
    uint32_t          scratch_pitch_words;    /* 8-aligned pitch of buffer */
} x3100_state_t;

/* The single global, defined in main.c. */
extern x3100_state_t g_x3100;

/* Caller pid context — set by transports around each invocation. */
extern pid_t x3100_current_caller_pid;

/* ==========================================================================
 *  Module entry points exported between source files
 * ========================================================================== */

/* x3100_modeset.c */
int   x3100_modeset_init_hw(hotplug_device_t *dev);
bool  x3100_hw_init(void);          /* map BARs, build GTT pool + ring         */
void  x3100_hw_shutdown(void);      /* blank plane B, drop engine flag         */
int   x3100_modeset(void);          /* arm easy-path scanout onto front_offset */
void  x3100_modeset_shutdown(void);
int32_t x3100_internal_mode_set(const dv_mode_t *m);
int32_t x3100_internal_scanout_set(dv_surface_t s);
void  x3100_recompute_mode_list(void);
int32_t x3100_palette_set(const uint32_t *palette_argb, uint32_t count);
int32_t x3100_gamma_get(dv_gamma_ramp_t *out);

/* x3100_2d.c
 *
 * The four functions below changed signature from void to bool/int so
 * timeouts in the FIFO/idle poll, and failures in engine init, can be
 * propagated up.  Hot-path 2D primitives (solid_fill, blit, line, ...)
 * still discard the wait_for_* return — when those run, the engine is
 * either alive (return is true and they proceed) or it's dead and the
 * sticky `engine_ready=false` set by the timed-out wait has already
 * marked it; init paths bail out before the hot path is reachable. */
int   x3100_engine_reset(void);
int   x3100_engine_setup_for_format(uint8_t pix_width, uint32_t pitch_pixels);
bool  x3100_wait_for_fifo(uint32_t n);
bool  x3100_wait_for_idle(void);
void  x3100_hw_solid_fill(uint32_t vram_off, uint32_t pitch_pixels,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                           uint32_t color);
void  x3100_hw_blit(uint32_t src_off, uint32_t src_pitch,
                     uint32_t src_x,   uint32_t src_y,
                     uint32_t dst_off, uint32_t dst_pitch,
                     uint32_t dst_x,   uint32_t dst_y,
                     uint32_t w,       uint32_t h);
void  x3100_hw_blit_overlap(uint32_t src_off, uint32_t pitch,
                             uint32_t sx, uint32_t sy,
                             uint32_t dx, uint32_t dy,
                             uint32_t w,  uint32_t h);
void  x3100_hw_line(uint32_t vram_off, uint32_t pitch_pixels,
                     int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     uint32_t color);
void  x3100_hw_host_blit(uint32_t dst_off, uint32_t dst_pitch,
                          uint32_t dst_x, uint32_t dst_y,
                          uint32_t w, uint32_t h,
                          const void *pixels, uint32_t pixel_bpp);

/* Monochrome-expansion blit (PRG 6.3.3.1, "font caching").  Expands a
 * 1bpp linear source mask in VRAM to fg_color where the bit is 1, and
 * leaves the destination untouched where the bit is 0 (bkgd:leave_alone)
 * — a masked write with NO destination read.  This is the hardware glyph
 * path: it uses only the 2D engine, sidestepping the LM texture-alpha
 * hardware defect.  `mono_off`/`mono_pitch_bytes` describe the packed
 * 1bpp mask; (dst_x,dst_y,w,h) the destination rectangle. */
void  x3100_hw_mono_blit(uint32_t mono_off, uint32_t mono_pitch_bytes,
                          uint32_t dst_off, uint32_t dst_pitch_px,
                          uint32_t dst_x, uint32_t dst_y,
                          uint32_t w, uint32_t h, uint32_t fg_color);

/* Engine glyph path (XY_SETUP_BLT + XY_TEXT_BLT, validated gen4 encoding).
 * x3100_hw_text_setup establishes the foreground colour, transparent
 * monochrome expansion, SRCCOPY ROP, 32bpp and the destination base/pitch for
 * a run; emit it ONCE immediately before a contiguous run of glyphs, then one
 * x3100_hw_text_blit per glyph.  No destination read, no CPU pixel writes, no
 * drain — the whole glyph run executes on the engine in ring order. */
void  x3100_hw_text_setup(uint32_t dst_off, uint32_t dst_pitch_px,
                          uint32_t fg_color,
                          uint32_t clip_w, uint32_t clip_h);
void  x3100_hw_text_blit(uint32_t mono_off,
                          int32_t dst_x, int32_t dst_y,
                          uint32_t w, uint32_t h);

/* x3100_cursor.c */
int32_t x3100_cursor_set_bitmap(const dv_cursor_desc_t *d);
int32_t x3100_cursor_set_position(int32_t x, int32_t y);
int32_t x3100_cursor_show(void);
int32_t x3100_cursor_hide(void);

/* x3100_irq.c */
int   x3100_irq_init(uint32_t my_port);
void  x3100_irq_dispatch(uint32_t notification);  /* called by main loop */
int32_t x3100_vsync_wait(uint32_t timeout_ms);

/* x3100_3d.c — SCALE_3D engine path */
void  x3100_3d_invalidate(void);
bool  x3100_hw_alpha_blit_full(uint32_t src_off, uint32_t src_pitch,
                                uint32_t sx, uint32_t sy,
                                uint32_t dst_off, uint32_t dst_pitch,
                                uint32_t dx, uint32_t dy,
                                uint32_t w,  uint32_t h,
                                uint8_t  const_alpha,
                                bool     use_pixel_alpha);
bool  x3100_hw_stretched_blit(uint32_t src_off, uint32_t src_pitch,
                               uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh,
                               uint32_t dst_off, uint32_t dst_pitch,
                               uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh);

/* x3100_overlay.c — single HW overlay plane */
int32_t x3100_overlay_create(dv_vproc_t v, const dv_overlay_desc_t *d);
int32_t x3100_overlay_destroy(void);
int32_t x3100_overlay_update(const dv_overlay_update_t *u);
int32_t x3100_overlay_set_visible(bool v);
int32_t x3100_blit_yuv_to_rgb_oneshot(dv_buffer_t y_plane,
                                       uint32_t sw, uint32_t sh, dv_format_t sf,
                                       dv_surface_t dst, dv_rect_t dr);

/* main.c — internals used by submodules */
void  x3100_notify_subscribers(uint32_t code, uint32_t display_id,
                                uint32_t arg0, uint32_t arg1);
int   x3100_subscribe(uint32_t port, uint32_t mask);
void  x3100_gpu_reset_full(void);

/* MMIO accessors — defined inline here for hot-path use across all
 * modules.  We use a generic memory-mapped pointer rather than the
 * kernel's iomem helpers because the LFB and MMIO are user-mapped at
 * driver attach via the standard hotplug BAR-map path. */
static inline uint32_t x3100_mmio_r32(uint32_t off)
{
    return *(volatile uint32_t *)(g_x3100.mmio + off);
}
static inline void x3100_mmio_w32(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(g_x3100.mmio + off) = val;
}
static inline uint16_t x3100_mmio_r16(uint32_t off)
{
    return *(volatile uint16_t *)(g_x3100.mmio + off);
}
static inline void x3100_mmio_w16(uint32_t off, uint16_t val)
{
    *(volatile uint16_t *)(g_x3100.mmio + off) = val;
}
static inline uint8_t x3100_mmio_r8(uint32_t off)
{
    return *(volatile uint8_t *)(g_x3100.mmio + off);
}
static inline void x3100_mmio_w8(uint32_t off, uint8_t val)
{
    *(volatile uint8_t *)(g_x3100.mmio + off) = val;
}

/* IO BAR byte access — used only for PLL.  Helper indirect-PLL pair
 * defined in x3100_modeset.c. */
uint8_t x3100_pll_read(uint8_t pll_offset);
void    x3100_pll_write(uint8_t pll_offset, uint8_t val);

/* LCD indirect register helpers (MMIO LCD_INDEX/LCD_DATA). */
uint32_t x3100_lcd_read(uint8_t lcd_offset);
void     x3100_lcd_write(uint8_t lcd_offset, uint32_t val);

/* VRAM allocator (defined in main.c, mirrors BGA pattern). */
void          vram_init_pool(uint32_t base, uint32_t size);
vram_block_t *vram_alloc(uint32_t bytes, uint32_t align);
void          vram_free(vram_block_t *b);

/* ---- graphics-address helper (X3100) -------------------------------------
 * Maps a VRAM-pool byte offset to its GTT graphics address.  (The MMIO
 * accessors x3100_mmio_r32/w32 are already defined above; x3100_2d.c keeps
 * file-local equivalents for the hot path.) */
static inline uint32_t x3100_gaddr(uint32_t pool_byte_offset)
{
    return g_x3100.vram_gtt_index * 4096u + pool_byte_offset;
}

#endif /* MAINDOB_DRIVERS_X3100_STATE_H */
