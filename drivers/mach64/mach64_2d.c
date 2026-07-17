/* ==========================================================================
 *  mach64_2d.c — hardware 2D draw-engine primitives for the Mach64.
 *
 *  Signatures here MUST match the declarations in mach64_state.h — that
 *  header is the contract the rest of the driver links against.
 *
 *  Where the BGA driver fills/blits with the CPU (sw_fill_rect, sw_blit),
 *  the Mach64 has a real blitter: we program the draw engine and let the
 *  hardware do it.  The desktop path (cmdlist_replay/compose) maps onto
 *  these: FILL_RECT→solid_fill, BLIT→blit, DRAW_LINE→line, glyph atlas
 *  upload→host_blit.
 *
 *  Transcribed from ATI primary source:
 *    PRG-215R3-00-10 "RAGE PRO and Derivatives Programmer's Guide"
 *      ch.5  Engine Initialization  (FIFO discipline, init_engine, tab 5-3)
 *      ch.6  Engine Operations      (rectangle fill, BitBlt, line, host data)
 *  register offsets cross-checked against RRG-S00700-05 and xorg atiregs.h.
 *
 *  FIFO discipline (PRG 5-1) is mandatory: every draw-engine register
 *  write (DWORD offset >= 0x40) goes through a 16-entry FIFO; ensure free
 *  slots before writing or the engine locks.  Reads are never FIFOed.
 * ========================================================================== */

#include <dob/types.h>
#include <stdio.h>
#include "mach64_state.h"
#include "mach64_hw.h"

/* g_mach64 + mach64_mmio_r32/w32 come from mach64_state.h (inline). */

/* ==========================================================================
 *  CPU fallback primitives.
 *
 *  Used when g_mach64.engine_ok is false — i.e. the 2D accelerator isn't
 *  confirmed working on this hardware.  Everything the driver draws into
 *  (the primary and all surfaces/textures) lives in the directly-mapped
 *  VRAM aperture at g_mach64.vram, so the CPU can read/write it without
 *  the engine.  Slow (VRAM access is uncached) but always correct; lets
 *  the desktop render while the hardware path is validated separately.
 *  Each public hw_* primitive below routes here when !engine_ok.
 * ========================================================================== */

static inline volatile uint32_t *vram_at(uint32_t byte_off)
{
    return (volatile uint32_t *)((volatile uint8_t *)g_mach64.vram + byte_off);
}

static void cpu_solid_fill(uint32_t off, uint32_t pitch_px,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                           uint32_t color)
{
    volatile uint32_t *base = vram_at(off);
    for (uint32_t r = 0; r < h; r++)
    {
        volatile uint32_t *row = base + (uint32_t)(y + r) * pitch_px + x;
        for (uint32_t c = 0; c < w; c++) row[c] = color;
    }
}

static void cpu_blit(uint32_t src_off, uint32_t src_pitch, uint32_t sx, uint32_t sy,
                     uint32_t dst_off, uint32_t dst_pitch, uint32_t dx, uint32_t dy,
                     uint32_t w, uint32_t h)
{
    volatile uint32_t *sb = vram_at(src_off);
    volatile uint32_t *db = vram_at(dst_off);
    /* Direction-aware to stay correct when source and destination overlap
     * within the same surface (e.g. scroll). */
    bool backward = (src_off == dst_off) && (dy > sy || (dy == sy && dx > sx));
    if (!backward)
        for (uint32_t r = 0; r < h; r++)
        {
            volatile uint32_t *srow = sb + (uint32_t)(sy + r) * src_pitch + sx;
            volatile uint32_t *drow = db + (uint32_t)(dy + r) * dst_pitch + dx;
            for (uint32_t c = 0; c < w; c++) drow[c] = srow[c];
        }
    else
        for (uint32_t r = h; r-- > 0; )
        {
            volatile uint32_t *srow = sb + (uint32_t)(sy + r) * src_pitch + sx;
            volatile uint32_t *drow = db + (uint32_t)(dy + r) * dst_pitch + dx;
            for (uint32_t c = w; c-- > 0; ) drow[c] = srow[c];
        }
}

static void cpu_host_blit(uint32_t dst_off, uint32_t dst_pitch,
                          uint32_t dx, uint32_t dy, uint32_t w, uint32_t h,
                          const void *pixels, uint32_t pixel_bpp)
{
    volatile uint32_t *db = vram_at(dst_off);
    if (pixel_bpp == 32u)
    {
        const uint32_t *sp = (const uint32_t *)pixels;
        for (uint32_t r = 0; r < h; r++)
        {
            volatile uint32_t *drow = db + (uint32_t)(dy + r) * dst_pitch + dx;
            const uint32_t *srow = sp + (uint32_t)r * w;
            for (uint32_t c = 0; c < w; c++) drow[c] = srow[c];
        }
    }
}

static void cpu_mono_blit(uint32_t mono_off, uint32_t mono_pitch_bytes,
                          uint32_t dst_off, uint32_t dst_pitch_px,
                          uint32_t dx, uint32_t dy, uint32_t w, uint32_t h,
                          uint32_t fg)
{
    volatile uint8_t  *mono = (volatile uint8_t *)g_mach64.vram + mono_off;
    volatile uint32_t *db   = vram_at(dst_off);
    for (uint32_t r = 0; r < h; r++)
    {
        volatile uint8_t  *mrow = mono + (uint32_t)r * mono_pitch_bytes;
        volatile uint32_t *drow = db + (uint32_t)(dy + r) * dst_pitch_px + dx;
        for (uint32_t c = 0; c < w; c++)
        {
            uint8_t bit = (mrow[c >> 3] >> (7u - (c & 7u))) & 1u;  /* MSB-first */
            if (bit) drow[c] = fg;                                /* bit 0 -> leave */
        }
    }
}

static void cpu_line(uint32_t off, uint32_t pitch_px,
                     int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color)
{
    volatile uint32_t *base = vram_at(off);
    int32_t dx =  (x1 > x0 ? x1 - x0 : x0 - x1);
    int32_t dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (;;)
    {
        base[(uint32_t)y0 * pitch_px + (uint32_t)x0] = color;
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ==========================================================================
 *  FIFO / engine synchronization — PRG ch.5.2.1
 *  Return bool so callers can detect a wedged engine via timeout and set
 *  the sticky engine_ready=false (state.h contract).
 * ========================================================================== */

bool mach64_wait_for_fifo(uint32_t n)
{
    uint32_t guard = 0;
    if (n > 16u) n = 16u;
    /* ATI idiom (PRG 5-1): wait while (FIFO_STAT & 0xFFFF) > (0x8000>>n). */
    while ((mach64_mmio_r32(M64_FIFO_STAT) & 0xFFFFu) > (uint32_t)(0x8000u >> n))
    {
        if (++guard > 0x01000000u)
        {
            g_mach64.engine_ready = false;
            return false;
        }
    }
    return true;
}

bool mach64_wait_for_idle(void)
{
    uint32_t guard = 0;
    if (!mach64_wait_for_fifo(16u)) return false;
    /* PRG 5-2: idle = all 16 FIFO entries free AND GUI_STAT bit0 clear. */
    while (mach64_mmio_r32(M64_GUI_STAT) & M64_GUI_STAT_ENGINE_BUSY)
    {
        if (++guard > 0x01000000u)
        {
            g_mach64.engine_ready = false;
            return false;
        }
    }
    return true;
}

/* ==========================================================================
 *  Engine reset — PRG ch.5.2.1.2 ResetEngine()
 *  Pulse GEN_TEST_CNTL bit8 (engine enable) off->on, clear FIFO/HOST error
 *  latches in BUS_CNTL (0x00A00000).  Returns 0 on success.
 * ========================================================================== */

int mach64_engine_reset(void)
{
    uint32_t v = mach64_mmio_r32(M64_GEN_TEST_CNTL);
    mach64_mmio_w32(M64_GEN_TEST_CNTL, v & ~0x00000100u);
    mach64_mmio_w32(M64_GEN_TEST_CNTL, v |  0x00000100u);
    mach64_mmio_w32(M64_BUS_CNTL,
                    mach64_mmio_r32(M64_BUS_CNTL) | 0x00A00000u);
    g_mach64.engine_ready = true;
    return mach64_wait_for_idle() ? 0 : -1;
}

/* ==========================================================================
 *  Engine context setup for a pixel format — PRG ch.5.5 (init_engine)
 *
 *  `pix_width` is a M64_PIX_WIDTH_* code (we support 32bpp on the bring-up
 *  path).  `pitch_pixels` is the destination scanline pitch in PIXELS
 *  (engine pitch field = pixels/8, same as CRTC_OFF_PITCH per RRG 3-41).
 *  Sets up the persistent context (scissor, colors, mix, src, depth) so
 *  subsequent primitives only set color + trajectory.  Returns 0 on OK.
 * ========================================================================== */

int mach64_engine_setup_for_format(uint8_t pix_width, uint32_t pitch_pixels)
{
    if (mach64_engine_reset() != 0) return -1;

    if (!mach64_wait_for_fifo(1u)) return -1;
    mach64_mmio_w32(M64_CONTEXT_MASK, 0xFFFFFFFFu);

    /* Host/pattern/scissor/colors/mix/src — PRG 5-10/5-11.
     * Scissor to the full surface: SC_RIGHT=pitch-1, SC_BOTTOM = large
     * (we clip per-op via width/height; height-bound comes from the op). */
    if (!mach64_wait_for_fifo(13u)) return -1;
    mach64_mmio_w32(M64_HOST_CNTL,     0u);
    mach64_mmio_w32(M64_PAT_REG0,      0u);
    mach64_mmio_w32(M64_PAT_REG1,      0u);
    mach64_mmio_w32(M64_PAT_CNTL,      0u);
    /* Scissor to the full screen.  PRG 6.4.2: scissors are inclusive and
     * signed (SC_RIGHT 13-bit, SC_BOTTOM 15-bit); to include the whole
     * screen set left/top = 0 and right/bottom = (xres-1)/(yres-1).  A
     * saturated value like 0x3FFF is OUTSIDE the rule and makes the engine
     * treat every draw as below the bottom scissor (GUI_STAT clip flags),
     * so it completes but writes nothing. */
    {
        uint32_t xres = pitch_pixels ? pitch_pixels : g_mach64.mode.width;
        uint32_t yres = g_mach64.mode.height ? g_mach64.mode.height : 768u;
        mach64_mmio_w32(M64_SC_LEFT,   0u);
        mach64_mmio_w32(M64_SC_TOP,    0u);
        mach64_mmio_w32(M64_SC_RIGHT,  xres - 1u);
        mach64_mmio_w32(M64_SC_BOTTOM, yres - 1u);
    }
    mach64_mmio_w32(M64_DP_BKGD_CLR,   0u);
    mach64_mmio_w32(M64_DP_FRGD_CLR,   0xFFFFFFFFu);
    mach64_mmio_w32(M64_DP_WRITE_MASK, 0xFFFFFFFFu);
    mach64_mmio_w32(M64_DP_MIX,        M64_DP_MIX_SOLID);
    mach64_mmio_w32(M64_DP_SRC,        M64_DP_SRC_SOLID_FILL);

    if (!mach64_wait_for_fifo(3u)) return -1;
    mach64_mmio_w32(M64_CLR_CMP_CLR,   0u);
    mach64_mmio_w32(M64_CLR_CMP_MASK,  0xFFFFFFFFu);
    mach64_mmio_w32(M64_CLR_CMP_CNTL,  0u);

    /* Pixel depth on all three channels.  For 32bpp: DP_CHAIN_MASK 0x8080
     * (PRG tab 5-3).  We only special-case 32bpp here. */
    if (!mach64_wait_for_fifo(2u)) return -1;
    {
        uint32_t pw = ((uint32_t)pix_width)
                    | ((uint32_t)pix_width << M64_DP_SRC_PIX_WIDTH_SHIFT)
                    | ((uint32_t)pix_width << M64_DP_HOST_PIX_WIDTH_SHIFT);
        mach64_mmio_w32(M64_DP_PIX_WIDTH,  pw);
        mach64_mmio_w32(M64_DP_CHAIN_MASK, M64_DP_CHAIN_MASK_32BPP);
    }

    if (!mach64_wait_for_fifo(1u)) return -1;
    mach64_mmio_w32(M64_DST_OFF_PITCH, M64_OFF_PITCH(0, pitch_pixels));

    g_mach64.engine_pix_width = pix_width;
    g_mach64.engine_ready     = mach64_wait_for_idle();
    return g_mach64.engine_ready ? 0 : -1;
}

/* ==========================================================================
 *  Solid rectangle fill — PRG ch.6.3.1.2
 *  Final write to DST_HEIGHT_WIDTH triggers the op.  Engine is async;
 *  callers reading pixels back must mach64_wait_for_idle() themselves.
 * ========================================================================== */

/* Reject any engine operation whose addressed VRAM range would fall outside
 * physical VRAM. Feeding the 2D engine an out-of-range DST/SRC offset+pitch
 * makes it scribble past the framebuffer and wedge — observed on the real
 * Rage Mobility as a frozen UI with drifting colour "fog". That is a HARD
 * GPU hang, not a recoverable software error, so the only safe thing is to
 * never issue such an op: validate the byte extent the blitter will touch
 * (rows [0, y+h) at the given pitch, 32bpp) against the probed VRAM size and
 * skip + log if it overflows. A missing blit leaves a cosmetic glitch; an
 * out-of-range blit takes down the whole display. This is a backstop: it
 * catches a bad offset/pitch from ANY upstream cause (stale handle, offset
 * miscomputed under VRAM pressure) without needing to know which. */
static bool mach64_range_ok(uint32_t vram_off, uint32_t pitch_pixels,
                            uint32_t y, uint32_t h, const char *who)
{
    uint32_t vram = g_mach64.vram_bytes;
    if (vram == 0u) return true;                 /* size unknown -> don't block */
    /* bytes from vram_off to the end of the last touched row */
    uint64_t end_rows = (uint64_t)y + (uint64_t)h;
    uint64_t extent   = end_rows * (uint64_t)pitch_pixels * 4ull;
    uint64_t last     = (uint64_t)vram_off + extent;
    if (last > (uint64_t)vram)
    {
        char line[96];
        sprintf(line, "[mach64] BLOCKED %s: off=%u pitch=%u y=%u h=%u "
                      "end=%llu > vram=%u\n",
                who ? who : "op", vram_off, pitch_pixels, y, h,
                (unsigned long long)last, vram);
        debug_print(line);
        return false;
    }
    return true;
}

void mach64_hw_solid_fill(uint32_t vram_off, uint32_t pitch_pixels,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint32_t color)
{
    if (w == 0u || h == 0u) return;
    if (!mach64_range_ok(vram_off, pitch_pixels, y, h, "solid_fill")) return;
    if (!g_mach64.engine_ok) { cpu_solid_fill(vram_off, pitch_pixels, x, y, w, h, color); return; }

    /* Mirror the PRG ch.6 solid-fill sample using the PACKED composites.
     * (Writing the separate DST_X/Y/H/W registers left the geometry at zero
     * on this silicon — the composites DST_Y_X / DST_HEIGHT_WIDTH are the
     * ones the engine latches, and DST_HEIGHT_WIDTH triggers the fill.) */
    if (!mach64_wait_for_fifo(5u)) return;
    mach64_mmio_w32(M64_DP_FRGD_CLR,   color);
    mach64_mmio_w32(M64_DP_SRC,        M64_DP_SRC_SOLID_FILL);   /* 0x00000100 */
    mach64_mmio_w32(M64_DP_MIX,        M64_DP_MIX_SOLID);        /* frgd:paint */
    mach64_mmio_w32(M64_CLR_CMP_CNTL,  0u);                      /* compare off */
    mach64_mmio_w32(M64_GUI_TRAJ_CNTL, M64_GUI_TRAJ_DST_LTR_TTB);/* L->R, T->B */

    if (!mach64_wait_for_fifo(3u)) return;
    mach64_mmio_w32(M64_DST_OFF_PITCH,    M64_OFF_PITCH(vram_off, pitch_pixels));
    mach64_mmio_w32(M64_DST_Y_X,          (x << 16) | (y & 0xFFFFu));   /* X<<16 | Y (RRG 3-55) */
    mach64_mmio_w32(M64_DST_HEIGHT_WIDTH, (w << 16) | (h & 0xFFFFu));   /* WIDTH<<16 | HEIGHT (RRG 3-49); TRIGGER */
}

/* ==========================================================================
 *  Screen-to-screen BitBlt, simple 1:1, NON-overlapping — PRG ch.6.3.2.3
 *  For the general (possibly cross-surface) case.  Always L->R, T->B.
 *  Use mach64_hw_blit_overlap for same-surface moves that may overlap.
 * ========================================================================== */

void mach64_hw_blit(uint32_t src_off, uint32_t src_pitch,
                    uint32_t src_x,   uint32_t src_y,
                    uint32_t dst_off, uint32_t dst_pitch,
                    uint32_t dst_x,   uint32_t dst_y,
                    uint32_t w,       uint32_t h)
{
    if (w == 0u || h == 0u) return;
    if (!mach64_range_ok(src_off, src_pitch, src_y, h, "blit.src")) return;
    if (!mach64_range_ok(dst_off, dst_pitch, dst_y, h, "blit.dst")) return;
    if (!g_mach64.engine_ok) { cpu_blit(src_off, src_pitch, src_x, src_y, dst_off, dst_pitch, dst_x, dst_y, w, h); return; }

    if (!mach64_wait_for_fifo(1u)) return;
    mach64_mmio_w32(M64_DP_SRC, M64_DP_SRC_BLIT_FILL);

    /* Source trajectory: write SRC_OFF_PITCH + SRC_CNTL first (PRG). */
    if (!mach64_wait_for_fifo(4u)) return;
    mach64_mmio_w32(M64_SRC_OFF_PITCH, M64_OFF_PITCH(src_off, src_pitch));
    mach64_mmio_w32(M64_SRC_CNTL,      0u);                 /* unbounded Y (simple 1:1) */
    mach64_mmio_w32(M64_SRC_Y_X,       (src_x << 16) | (src_y & 0xFFFFu));
    mach64_mmio_w32(M64_SRC_WIDTH1,    w);

    if (!mach64_wait_for_fifo(4u)) return;
    mach64_mmio_w32(M64_DST_OFF_PITCH,    M64_OFF_PITCH(dst_off, dst_pitch));
    mach64_mmio_w32(M64_DST_CNTL,         M64_DST_X_DIR | M64_DST_Y_DIR);
    mach64_mmio_w32(M64_DST_Y_X,          (dst_x << 16) | (dst_y & 0xFFFFu));
    mach64_mmio_w32(M64_DST_HEIGHT_WIDTH, (w << 16) | (h & 0xFFFFu));   /* PRG: W<<16 | H; TRIGGER */
}

/* ==========================================================================
 *  Same-surface BitBlt with overlap handling (scroll) — PRG ch.6.2.3
 *  trajectories: "source trajectory direction always tracks destination".
 *  Choose direction so a pixel is read before it is overwritten.  When a
 *  direction is reversed, start the trajectory at the far corner.
 * ========================================================================== */

void mach64_hw_blit_overlap(uint32_t src_off, uint32_t pitch,
                            uint32_t sx, uint32_t sy,
                            uint32_t dx, uint32_t dy,
                            uint32_t w,  uint32_t h)
{
    if (w == 0u || h == 0u) return;
    {
        uint32_t ymax = sy > dy ? sy : dy;
        if (!mach64_range_ok(src_off, pitch, ymax, h, "blit_overlap")) return;
    }
    if (!g_mach64.engine_ok) { cpu_blit(src_off, pitch, sx, sy, src_off, pitch, dx, dy, w, h); return; }

    uint32_t x_dir = M64_DST_X_DIR;   /* L->R */
    uint32_t y_dir = M64_DST_Y_DIR;   /* T->B */
    uint32_t sx_start = sx, sy_start = sy;
    uint32_t dx_start = dx, dy_start = dy;

    if (dy > sy)                      /* moving down: copy bottom row first */
    {
        y_dir = 0u;
        sy_start = sy + h - 1u;
        dy_start = dy + h - 1u;
    }
    if (dx > sx)                      /* moving right: copy rightmost col first */
    {
        x_dir = 0u;
        sx_start = sx + w - 1u;
        dx_start = dx + w - 1u;
    }

    if (!mach64_wait_for_fifo(1u)) return;
    mach64_mmio_w32(M64_DP_SRC, M64_DP_SRC_BLIT_FILL);

    if (!mach64_wait_for_fifo(4u)) return;
    mach64_mmio_w32(M64_SRC_OFF_PITCH, M64_OFF_PITCH(src_off, pitch));
    mach64_mmio_w32(M64_SRC_CNTL,      0u);
    mach64_mmio_w32(M64_SRC_Y_X,       (sx_start << 16) | (sy_start & 0xFFFFu));
    mach64_mmio_w32(M64_SRC_WIDTH1,    w);

    if (!mach64_wait_for_fifo(4u)) return;
    mach64_mmio_w32(M64_DST_OFF_PITCH,    M64_OFF_PITCH(src_off, pitch));
    mach64_mmio_w32(M64_DST_CNTL,         x_dir | y_dir);
    mach64_mmio_w32(M64_DST_Y_X,          (dx_start << 16) | (dy_start & 0xFFFFu));
    mach64_mmio_w32(M64_DST_HEIGHT_WIDTH, (w << 16) | (h & 0xFFFFu));   /* PRG: W<<16 | H; TRIGGER */
}

/* ==========================================================================
 *  Line draw (18-bit Bresenham) — PRG ch.6.3.1.1
 *  Octant from dx/dy sign + which axis is major; Bresenham params per the
 *  ATI formula.  Final write to DST_BRES_LNTH triggers the draw.
 *  Note: line draw is NOT supported in packed-24bpp (we run 32bpp).
 * ========================================================================== */

void mach64_hw_line(uint32_t vram_off, uint32_t pitch_pixels,
                    int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                    uint32_t color)
{
    if (!g_mach64.engine_ok) { cpu_line(vram_off, pitch_pixels, x0, y0, x1, y1, color); return; }

    int32_t dx = x1 - x0; if (dx < 0) dx = -dx;
    int32_t dy = y1 - y0; if (dy < 0) dy = -dy;
    int32_t minD = (dx < dy) ? dx : dy;
    int32_t maxD = (dx < dy) ? dy : dx;

    uint32_t cntl = 0u;
    if (x0 < x1) cntl |= M64_DST_X_DIR;                 /* left-to-right */
    if (y0 < y1) cntl |= M64_DST_Y_DIR;                 /* top-to-bottom */
    if (dx < dy) cntl |= M64_DST_Y_MAJOR;               /* Y is the major axis */
    cntl |= M64_DST_LAST_PEL;                           /* draw the final pixel */

    if (!mach64_wait_for_fifo(2u)) return;
    mach64_mmio_w32(M64_DP_FRGD_CLR, color);
    mach64_mmio_w32(M64_DP_SRC,      M64_DP_SRC_SOLID_FILL);

    /* DST_CNTL read is not FIFOed; we write a known full value (PRG note
     * about avoiding a stale DST_CNTL in the FIFO is satisfied because we
     * set every direction bit explicitly rather than read-modify-write). */
    if (!mach64_wait_for_fifo(6u)) return;
    mach64_mmio_w32(M64_DST_OFF_PITCH, M64_OFF_PITCH(vram_off, pitch_pixels));
    mach64_mmio_w32(M64_DST_CNTL,      cntl);
    mach64_mmio_w32(M64_DST_Y_X,       ((uint32_t)x0 << 16) | ((uint32_t)y0 & 0xFFFFu));
    mach64_mmio_w32(M64_DST_BRES_ERR,  (uint32_t)(2 * minD - maxD));
    mach64_mmio_w32(M64_DST_BRES_INC,  (uint32_t)(2 * minD));
    mach64_mmio_w32(M64_DST_BRES_DEC,  (uint32_t)(2 * (minD - maxD)));
    if (!mach64_wait_for_fifo(1u)) return;
    mach64_mmio_w32(M64_DST_BRES_LNTH, (uint32_t)(maxD + 1));               /* TRIGGER */
}

/* ==========================================================================
 *  Host-data rectangle (CPU -> VRAM pixel upload) — PRG ch.6.3.1.2
 *  Used to upload a glyph atlas / texture region into VRAM through the
 *  engine.  mono:always'1', frgd:host; we push width*height pixels as
 *  dwords into HOST_DATA0, respecting FIFO discipline (one dword per slot;
 *  the ATI sample bursts 16 at a time — we keep it simple but FIFO-safe).
 *  pixel_bpp is informational; the 32bpp path packs one pixel per dword.
 * ========================================================================== */

void mach64_hw_host_blit(uint32_t dst_off, uint32_t dst_pitch,
                         uint32_t dst_x, uint32_t dst_y,
                         uint32_t w, uint32_t h,
                         const void *pixels, uint32_t pixel_bpp)
{
    if (w == 0u || h == 0u || pixels == NULL) return;
    if (!g_mach64.engine_ok) { cpu_host_blit(dst_off, dst_pitch, dst_x, dst_y, w, h, pixels, pixel_bpp); return; }
    (void)pixel_bpp;   /* 32bpp path: one pixel per dword */

    const uint32_t *src = (const uint32_t *)pixels;
    uint32_t total = w * h;

    /* foreground source = host data (PRG 6-29); mono:always_'1' (field 0). */
    if (!mach64_wait_for_fifo(2u)) return;
    mach64_mmio_w32(M64_DP_SRC,
        (uint32_t)M64_DP_SRC_HOST << M64_DP_FRGD_SRC_SHIFT);
    mach64_mmio_w32(M64_HOST_CNTL, 0u);

    /* destination trajectory + initiate; engine now consumes host data. */
    if (!mach64_wait_for_fifo(4u)) return;
    mach64_mmio_w32(M64_DST_OFF_PITCH,    M64_OFF_PITCH(dst_off, dst_pitch));
    mach64_mmio_w32(M64_DST_CNTL,         M64_DST_X_DIR | M64_DST_Y_DIR);
    mach64_mmio_w32(M64_DST_Y_X,          (dst_x << 16) | (dst_y & 0xFFFFu));
    mach64_mmio_w32(M64_DST_HEIGHT_WIDTH, (w << 16) | (h & 0xFFFFu));   /* PRG: W<<16 | H; TRIGGER */

    /* feed pixels.  One dword per FIFO slot; wait for a slot each time.
     * (Correctness over speed; a 16-dword burst is a later optimization.) */
    for (uint32_t i = 0; i < total; i++)
    {
        if (!mach64_wait_for_fifo(1u)) return;
        mach64_mmio_w32(M64_HOST_DATA0, src[i]);
    }

    /* restore default source for subsequent solid ops. */
    if (!mach64_wait_for_fifo(1u)) return;
    mach64_mmio_w32(M64_DP_SRC, M64_DP_SRC_SOLID_FILL);
}

/* ==========================================================================
 *  Monochrome-expansion blit — PRG ch.6.3.3.1 ("Monochrome Expansion",
 *  the documented font-caching path).
 *
 *  Source is a 1bpp mask packed linearly in VRAM (off-screen).  The engine
 *  expands it: bit=1 -> DP_FRGD_CLR (the requested text colour), bit=0 ->
 *  bkgd:leave_alone (destination untouched).  No destination read, no 3D
 *  texture path -> fast and free of the LM texture-alpha defect.
 *
 *  Register sequence (verbatim from the ATI example, adapted to 32bpp dst):
 *    DP_PIX_WIDTH = SRC:1bpp | DST:32bpp | HOST:32bpp
 *    DP_MIX       = 0x00070003   (frgd:paint, bkgd:leave_alone)
 *    DP_SRC       = 0x00030100   (mono:blit, frgd:DP_FRGD_CLR)
 *    SRC_CNTL     = LINEAR       (mask is linear, not 2D)
 *  mono_pitch_bytes is the mask's row stride in bytes; the SRC pitch field
 *  is pixels/8 = (mono_pitch_bytes*8)/8 = mono_pitch_bytes. */

void mach64_hw_mono_blit(uint32_t mono_off, uint32_t mono_pitch_bytes,
                         uint32_t dst_off, uint32_t dst_pitch_px,
                         uint32_t dst_x, uint32_t dst_y,
                         uint32_t w, uint32_t h, uint32_t fg_color)
{
    if (w == 0u || h == 0u) return;
    if (!g_mach64.engine_ok) { cpu_mono_blit(mono_off, mono_pitch_bytes, dst_off, dst_pitch_px, dst_x, dst_y, w, h, fg_color); return; }

    /* Context for mono expansion. */
    if (!mach64_wait_for_fifo(5u)) return;
    mach64_mmio_w32(M64_DP_FRGD_CLR,  fg_color);
    mach64_mmio_w32(M64_DP_WRITE_MASK, 0xFFFFFFFFu);
    /* DP_PIX_WIDTH: dst=32bpp (nibble0), src=1bpp (nibble2=0), host=32bpp. */
    mach64_mmio_w32(M64_DP_PIX_WIDTH,
        ((uint32_t)M64_PIX_WIDTH_32BPP)
      | ((uint32_t)M64_PIX_WIDTH_1BPP  << M64_DP_SRC_PIX_WIDTH_SHIFT)
      | ((uint32_t)M64_PIX_WIDTH_32BPP << M64_DP_HOST_PIX_WIDTH_SHIFT));
    mach64_mmio_w32(M64_DP_MIX, M64_DP_MIX_SOLID);   /* frgd:paint, bkgd:leave_alone */
    /* DP_SRC = mono:blit (field 3 @ bits16), frgd:frgd_clr (field 1 @ bits8). */
    mach64_mmio_w32(M64_DP_SRC, M64_DP_SRC_MONO_BLIT);

    /* Source trajectory: LINEAR mono stream, exactly as the PRG font example
     * (6.3.3.1).  The mask is packed CONTINUOUSLY (no per-row byte padding,
     * see atlas_mono_build), so the engine wraps every SRC_WIDTH1 bits with
     * no gap.  Pitch uses the screen pitch as in the ATI sample. */
    if (!mach64_wait_for_fifo(4u)) return;
    mach64_mmio_w32(M64_SRC_OFF_PITCH, M64_OFF_PITCH(mono_off, dst_pitch_px));
    mach64_mmio_w32(M64_SRC_CNTL,      M64_SRC_LINEAR_EN);       /* 0x04 = linear */
    mach64_mmio_w32(M64_SRC_Y_X,       0u);                      /* mask starts at its origin */
    mach64_mmio_w32(M64_SRC_WIDTH1,    w);                       /* wrap after w bits */

    /* Destination trajectory + initiate. */
    if (!mach64_wait_for_fifo(4u)) return;
    mach64_mmio_w32(M64_DST_OFF_PITCH,    M64_OFF_PITCH(dst_off, dst_pitch_px));
    mach64_mmio_w32(M64_DST_CNTL,         M64_DST_X_DIR | M64_DST_Y_DIR);
    mach64_mmio_w32(M64_DST_Y_X,          (dst_x << 16) | (dst_y & 0xFFFFu));
    mach64_mmio_w32(M64_DST_HEIGHT_WIDTH, (w << 16) | (h & 0xFFFFu));   /* PRG: W<<16 | H; TRIGGER */

    /* Restore default solid source for subsequent fills. */
    if (!mach64_wait_for_fifo(2u)) return;
    mach64_mmio_w32(M64_DP_PIX_WIDTH, M64_DP_PIX_WIDTH_32BPP);
    mach64_mmio_w32(M64_DP_SRC,       M64_DP_SRC_SOLID_FILL);
}
