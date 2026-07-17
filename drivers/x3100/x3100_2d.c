/* x3100_2d.c — i965 (GM965, gen4) 2D engine for the MainDOB X3100 driver.
 *
 * Exposes the SAME public API as mach64_2d.c (x3100_hw_solid_fill / _blit /
 * _blit_overlap / _line / _host_blit / _mono_blit + the engine helpers), so
 * the DV data plane ported verbatim from drivers/mach64/main.c calls these
 * with no changes.  The "off" parameters are BYTE offsets within the VRAM
 * pool and pitches are in PIXELS — exactly mach64's convention — converted
 * here to GTT graphics addresses and byte pitches for the BLT engine.
 *
 * Command/ring encodings are validated on the real Acer panel (Fase 3a:
 * XY_COLOR_BLT + MI_FLUSH + ring bring-up drew EBU bars with the CPU writing
 * zero pixels).  See x3100_2d.h for the cited bit layouts.
 *
 * CPU paths (overlap reverse-copy, diagonal line, host upload, mono fallback)
 * go through the GMADR aperture window (g_x3100.vram) — the CPU->scanned-pages
 * path proven during EBU bring-up — never the raw dma virtual address.  They
 * are off the per-frame hot path (rare/one-time), so their cost is acceptable.
 */

#include <dob/types.h>
#include <unistd.h>           /* clock_ms — wall-clock bound for engine drains */
#include "x3100_state.h"
#include "x3100_2d.h"

/* ==========================================================================
 *  Low-level register / memory access
 * ========================================================================== */

/* MMIO is mapped as uint8_t* (byte offsets), same as mach64 — 32-bit regs are
 * accessed with an explicit cast so a renamed mach64 main.c works verbatim. */
static inline uint32_t r32(uint32_t reg)
{
    return *(volatile uint32_t *)(g_x3100.mmio + reg);
}
static inline void w32(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(g_x3100.mmio + reg) = val;
}

/* GTT graphics address of a byte offset within the VRAM pool. */
static inline uint32_t gaddr(uint32_t off)
{
    return g_x3100.vram_gtt_index * 4096u + off;
}

/* CPU pixel pointer into the pool, through the GMADR aperture window. */
static inline volatile bgra_t *cpu_px(uint32_t off)
{
    return g_x3100.vram + (off >> 2);
}

/* Ring-wait bound.  The old fixed 20M-iteration budget had two problems: its
 * wall-clock length depended on MMIO latency (it could spin for SECONDS — the
 * ~2 s UI freeze, since these waits run synchronously inside the compositor's
 * compose/flip), yet it also had to be huge because giving up the free-space
 * wait early corrupts the ring (see ring_emit_cmd) and hangs the GPU.
 *
 * The fix is to bound by HEAD PROGRESS, not by a count or a flat timer: a
 * healthy-but-busy engine advances RING_HEAD continuously, so we wait as long
 * as HEAD keeps moving (never abandoning real work -> no corruption), and give
 * up only once HEAD has not moved for X3100_RING_STALL_MS — a genuine stall,
 * where a brief hitch is far better than a multi-second freeze.  No single 2D
 * command runs anywhere near this long (a full-screen blit is a few ms), so a
 * frozen HEAD past it unambiguously means the CS is stuck. */
#define X3100_RING_STALL_MS  200u

/* ==========================================================================
 *  Render ring command streamer
 * ========================================================================== */

static void ring_kick(void)
{
    /* Make every ring dword we just wrote globally visible BEFORE the CS sees
     * the new TAIL (the doorbell).  The ring lives in system RAM that the CS
     * reads through the GTT; the TAIL write is an uncached MMIO store, which on
     * x86 can reach the engine before our (write-back) ring stores have drained
     * from the CPU store buffer.  The CS would then fetch stale/garbage dwords
     * past the old TAIL and HANG — the intermittent freeze (the drain below
     * then busy-spins for ~seconds, so the mouse stalls) plus the display chaos
     * that clears once the engine is reset.  An sfence drains the store buffer
     * (and any write-combining) so the doorbell can't overtake the data.  This
     * is the ring-side companion to the posting read the texture-upload path
     * already uses for the same CPU-write -> engine-read hazard. */
    __asm__ volatile ("sfence" ::: "memory");
    w32(X3100_RING_TAIL, g_x3100.ring_tail & X3100_RING_TAIL_ADDR);
}

/* Emit one command (n dwords) onto the ring: handle end-of-ring wrap, pad to
 * qword (2-dword) alignment as the CS requires, chase HEAD for free space, then
 * advance TAIL to kick the engine. */
static void ring_emit_cmd(const uint32_t *dw, uint32_t n)
{
    uint32_t need = (n & 1u) ? (n + 1u) : n;     /* qword-aligned dword count */
    uint32_t need_bytes = need * 4u;

    if (g_x3100.ring == 0 || g_x3100.ring_bytes == 0)
        return;                                  /* ring not up — no-op */

    /* Wrap if the command reaches or crosses the end.  Must be ">=", not ">":
     * TAIL must stay strictly < ring_bytes; a TAIL == ring_bytes wedges the CS. */
    if (g_x3100.ring_tail + need_bytes >= g_x3100.ring_bytes) {
        uint32_t off = g_x3100.ring_tail;
        while (off < g_x3100.ring_bytes) {
            g_x3100.ring[off >> 2] = X3100_MI_NOOP;
            off += 4u;
        }
        g_x3100.ring_tail = 0u;
        ring_kick();
    }

    /* Chase HEAD until enough room.  CRITICAL: never abandon a healthy-but-busy
     * engine here — emitting into space the CS has not yet consumed overwrites
     * pending commands and hangs the GPU.  So give up ONLY once HEAD has not
     * advanced for X3100_RING_STALL_MS (a genuine stall, where the ring is moot
     * and we'll be reset/recovered); while HEAD keeps moving we wait. */
    {
        uint32_t prev  = r32(X3100_RING_HEAD) & X3100_RING_HEAD_ADDR;
        uint32_t t_mov = clock_ms();
        uint32_t i     = 0u;
        for (;;) {
            uint32_t head = r32(X3100_RING_HEAD) & X3100_RING_HEAD_ADDR;
            uint32_t tail = g_x3100.ring_tail;
            uint32_t freeb = (head > tail) ? (head - tail)
                                           : (g_x3100.ring_bytes - (tail - head));
            if (freeb > need_bytes + 8u) break;
            if (((++i) & 0x3FFu) == 0u) {
                if (head != prev) { prev = head; t_mov = clock_ms(); }   /* progress */
                else if ((uint32_t)(clock_ms() - t_mov) >= X3100_RING_STALL_MS)
                    break;                       /* HEAD frozen -> stalled; emit anyway */
            }
        }
    }

    /* Copy the command, pad, advance, kick. */
    {
        uint32_t off = g_x3100.ring_tail;
        uint32_t i;
        for (i = 0; i < n; i++) {
            g_x3100.ring[off >> 2] = dw[i];
            off += 4u;
        }
        if (need != n) {
            g_x3100.ring[off >> 2] = X3100_MI_NOOP;
            off += 4u;
        }
        g_x3100.ring_tail = off;
        ring_kick();
    }
}

int x3100_ring_init(void)
{
    /* Disable, then reset HEAD/TAIL. */
    w32(X3100_RING_CTL, 0u);
    w32(X3100_RING_HEAD, 0u);
    w32(X3100_RING_TAIL, 0u);

    /* gen4/G45 quirk: HEAD may not latch to zero; re-write until it sticks. */
    {
        uint32_t i;
        for (i = 0; i < X3100_RING_HEAD_RESET_RETRIES; i++) {
            if ((r32(X3100_RING_HEAD) & X3100_RING_HEAD_ADDR) == 0u) break;
            w32(X3100_RING_HEAD, 0u);
        }
    }

    /* Program the ring base (page-aligned graphics address) and size+valid. */
    w32(X3100_RING_START, g_x3100.ring_gtt & 0xFFFFF000u);
    w32(X3100_RING_CTL,
        (X3100_RING_CTL_SIZE(g_x3100.ring_bytes) & 0x001FF000u) | X3100_RING_VALID);

    /* Let the command streamer run (clear STOP_RING via the masked write). */
    w32(X3100_RING_MI_MODE, X3100_MI_MODE_RUN);

    g_x3100.ring_tail = 0u;

    /* Verify the ring latched as valid. */
    if (!(r32(X3100_RING_CTL) & X3100_RING_VALID))
        return -1;
    return 0;
}

void x3100_ring_flush_sync(void)
{
    uint32_t cmd[2];

    cmd[0] = X3100_MI_FLUSH;     /* flush render/2D cache to memory */
    cmd[1] = X3100_MI_NOOP;
    ring_emit_cmd(cmd, 2u);

    /* Wait until the CS has drained everything we queued (HEAD == TAIL),
     * bounded by HEAD PROGRESS (see X3100_RING_STALL_MS): the old 20M-iteration
     * spin froze the whole UI for ~2 s on a stall because this runs inside the
     * compositor's synchronous compose/flip.  We keep waiting while HEAD moves
     * and give up only if it freezes mid-ring (genuine stall); giving up here is
     * safe (worst case a partially-drained frame, not ring corruption). */
    uint32_t prev  = r32(X3100_RING_HEAD) & X3100_RING_HEAD_ADDR;
    uint32_t t_mov = clock_ms();
    uint32_t i     = 0u;
    for (;;) {
        uint32_t head = r32(X3100_RING_HEAD) & X3100_RING_HEAD_ADDR;
        if (head == (g_x3100.ring_tail & X3100_RING_HEAD_ADDR)) break;   /* drained */
        if (((++i) & 0x3FFu) == 0u) {
            if (head != prev) { prev = head; t_mov = clock_ms(); }       /* progress */
            else if ((uint32_t)(clock_ms() - t_mov) >= X3100_RING_STALL_MS)
                break;                           /* HEAD frozen -> stalled */
        }
    }
}

/* ==========================================================================
 *  Hardware primitives — mach64_2d.c API, i965 implementation
 * ========================================================================== */

void x3100_hw_solid_fill(uint32_t vram_off, uint32_t pitch_pixels,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         uint32_t color)
{
    uint32_t cmd[X3100_XY_COLOR_BLT_LEN];

    if (!w || !h) return;

    cmd[0] = X3100_CMD_XY_COLOR_BLT;
    cmd[1] = X3100_BR13_DEPTH_32 | X3100_BR13_ROP(X3100_ROP_PATCOPY)
           | X3100_BR13_PITCH(pitch_pixels * 4u);
    cmd[2] = X3100_XY_CORNER(x, y);
    cmd[3] = X3100_XY_CORNER(x + w, y + h);          /* exclusive */
    cmd[4] = gaddr(vram_off);
    cmd[5] = color;
    ring_emit_cmd(cmd, X3100_XY_COLOR_BLT_LEN);
}

void x3100_hw_blit(uint32_t src_off, uint32_t src_pitch,
                   uint32_t src_x, uint32_t src_y,
                   uint32_t dst_off, uint32_t dst_pitch,
                   uint32_t dst_x, uint32_t dst_y,
                   uint32_t w, uint32_t h)
{
    uint32_t cmd[X3100_XY_SRC_COPY_BLT_LEN];

    if (!w || !h) return;

    cmd[0] = X3100_CMD_XY_SRC_COPY_BLT;
    cmd[1] = X3100_BR13_DEPTH_32 | X3100_BR13_ROP(X3100_ROP_SRCCOPY)
           | X3100_BR13_PITCH(dst_pitch * 4u);
    cmd[2] = X3100_XY_CORNER(dst_x, dst_y);
    cmd[3] = X3100_XY_CORNER(dst_x + w, dst_y + h);  /* exclusive */
    cmd[4] = gaddr(dst_off);
    cmd[5] = X3100_XY_CORNER(src_x, src_y);
    cmd[6] = X3100_BR13_PITCH(src_pitch * 4u);       /* BR11: src pitch (bytes) */
    cmd[7] = gaddr(src_off);
    ring_emit_cmd(cmd, X3100_XY_SRC_COPY_BLT_LEN);
}

void x3100_hw_blit_overlap(uint32_t src_off, uint32_t pitch,
                           uint32_t sx, uint32_t sy,
                           uint32_t dx, uint32_t dy,
                           uint32_t w, uint32_t h)
{
    bool overlap, needs_reverse;

    if (!w || !h) return;

    /* Same surface, single pitch (mach64's scroll-region use).  The XY copy
     * scans top-left -> bottom-right; if the regions overlap and the dst is
     * below/right of the src, the engine overwrites source rows before reading
     * them. */
    overlap = !(sx + w <= dx || dx + w <= sx || sy + h <= dy || dy + h <= sy);
    needs_reverse = overlap && (dy > sy || (dy == sy && dx > sx));

    if (!needs_reverse) {
        x3100_hw_blit(src_off, pitch, sx, sy, src_off, pitch, dx, dy, w, h);
        return;
    }

    /* Reverse case, FULLY IN-ENGINE via band decomposition (this used to be
     * a CPU reverse copy through the GMADR aperture — the single hottest
     * remaining CPU path: every downward scroll and every down/right window
     * drag paid host-bus bandwidth plus a full ring drain).
     *
     * Idea: split the rectangle into bands no taller (wider) than the
     * displacement delta and emit them in reverse order.  Each individual
     * XY_SRC_COPY then has DISJOINT source and destination row (column)
     * ranges — the engine's top-down scan inside one band can never eat its
     * own source — and the ring executes bands in emission order, so a later
     * band only reads rows the earlier bands have not written:
     *
     *   dy > sy, delta = dy-sy, band k of height bh <= delta at row off:
     *     reads  [sy+off, sy+off+bh)                 (bh <= delta)
     *     writes [dy+off, dy+off+bh) = [sy+off+delta, ...)  -> disjoint;
     *   emitted bottom-up (off descending): every earlier-emitted band
     *   writes rows STRICTLY BELOW every later band's source rows.
     *
     * Worst case (1-px scroll of a full-height region) is h one-row blits:
     * ~8 dwords each on the ring, with ring_emit_cmd's HEAD-chasing back-
     * pressure absorbing any burst — still leagues cheaper than the old
     * 2*w*h aperture accesses, and it leaves the CPU free. */
    if (dy > sy) {
        uint32_t delta     = dy - sy;
        uint32_t remaining = h;
        while (remaining) {
            uint32_t bh  = (remaining < delta) ? remaining : delta;
            uint32_t off = remaining - bh;           /* band's top row in rect */
            x3100_hw_blit(src_off, pitch, sx, sy + off,
                          src_off, pitch, dx, dy + off, w, bh);
            remaining = off;
        }
    } else {                                          /* dy == sy, dx > sx */
        uint32_t delta     = dx - sx;
        uint32_t remaining = w;
        while (remaining) {
            uint32_t bw  = (remaining < delta) ? remaining : delta;
            uint32_t off = remaining - bw;           /* band's left col in rect */
            x3100_hw_blit(src_off, pitch, sx + off, sy,
                          src_off, pitch, dx + off, dy, bw, h);
            remaining = off;
        }
    }
}

void x3100_hw_line(uint32_t vram_off, uint32_t pitch_pixels,
                   int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                   uint32_t color)
{
    if (x0 == x1) {                                  /* vertical -> thin fill */
        int32_t  y = (y0 < y1) ? y0 : y1;
        uint32_t h = (uint32_t)(((y0 < y1) ? y1 : y0) - y + 1);
        x3100_hw_solid_fill(vram_off, pitch_pixels, (uint32_t)x0, (uint32_t)y,
                            1u, h, color);
        return;
    }
    if (y0 == y1) {                                  /* horizontal -> thin fill */
        int32_t  x = (x0 < x1) ? x0 : x1;
        uint32_t w = (uint32_t)(((x0 < x1) ? x1 : x0) - x + 1);
        x3100_hw_solid_fill(vram_off, pitch_pixels, (uint32_t)x, (uint32_t)y0,
                            w, 1u, color);
        return;
    }

    /* Diagonal: rare; CPU Bresenham via GMADR (drain first to draw on current
     * pixels). */
    x3100_ring_flush_sync();
    {
        volatile bgra_t *base = cpu_px(vram_off);
        int32_t dx  = x1 - x0, dy = y1 - y0;
        int32_t adx = (dx < 0) ? -dx : dx;
        int32_t ady = (dy < 0) ? -dy : dy;
        int32_t sx  = (dx < 0) ? -1 : 1;
        int32_t sy  = (dy < 0) ? -1 : 1;
        int32_t err = ((adx > ady) ? adx : -ady) / 2;
        int32_t e2, x = x0, y = y0;
        for (;;) {
            base[(uint32_t)y * pitch_pixels + (uint32_t)x] = (bgra_t)color;
            if (x == x1 && y == y1) break;
            e2 = err;
            if (e2 > -adx) { err -= ady; x += sx; }
            if (e2 <  ady) { err += adx; y += sy; }
        }
    }
}

void x3100_hw_host_blit(uint32_t dst_off, uint32_t dst_pitch,
                        uint32_t dst_x, uint32_t dst_y,
                        uint32_t w, uint32_t h,
                        const void *pixels, uint32_t pixel_bpp)
{
    const uint32_t *src = (const uint32_t *)pixels;
    uint32_t row, col;

    if (!w || !h || !pixels) return;
    (void)pixel_bpp;                                 /* DV uploads BGRA8888 */

    /* CPU upload into the pool via GMADR.  Drain so we don't race in-flight
     * blits to this surface. */
    x3100_ring_flush_sync();
    {
        volatile bgra_t *dbase = cpu_px(dst_off);
        for (row = 0; row < h; row++) {
            volatile bgra_t *d = dbase + (dst_y + row) * dst_pitch + dst_x;
            const uint32_t *s = src + (uint32_t)row * w;
            for (col = 0; col < w; col++) d[col] = (bgra_t)s[col];
        }
    }
}

/* ----- Engine glyph path: XY_SETUP_BLT + XY_TEXT_BLT (gen4, per wld) -----
 * Set up once per glyph run, then a text blit per glyph.  Transparent mono
 * expansion (BR01 bit 29): foreground where the source bit is 1, destination
 * untouched where it is 0 — glyphs composite over the chrome already blitted
 * beneath them, with no destination read and no CPU writes. */
void x3100_hw_text_setup(uint32_t dst_off, uint32_t dst_pitch_px,
                         uint32_t fg_color,
                         uint32_t clip_w, uint32_t clip_h)
{
    uint32_t cmd[X3100_XY_SETUP_BLT_LEN];
    cmd[0] = X3100_CMD_XY_SETUP_BLT;
    cmd[1] = X3100_BLT_CLIP_ENABLE                   /* clip glyphs to surface */
           | X3100_BLT_MONO_SRC_TRANSPARENT          /* bit 29: leave bg alone */
           | X3100_BR13_DEPTH_32
           | X3100_BR13_ROP(X3100_ROP_SRCCOPY)
           | X3100_BR13_PITCH(dst_pitch_px * 4u);
    cmd[2] = 0u;                                      /* BR24: clip TL (0,0) incl */
    cmd[3] = X3100_XY_CORNER(clip_w, clip_h);         /* BR25: clip BR excl       */
    cmd[4] = gaddr(dst_off);                          /* BR09: dst base         */
    cmd[5] = 0u;                                      /* BR05: bg (ignored)     */
    cmd[6] = fg_color;                                /* BR06: fg               */
    cmd[7] = 0u;                                      /* BR07: pattern addr     */
    ring_emit_cmd(cmd, X3100_XY_SETUP_BLT_LEN);
}

void x3100_hw_text_blit(uint32_t mono_off,
                        int32_t dst_x, int32_t dst_y,
                        uint32_t w, uint32_t h)
{
    uint32_t cmd[X3100_XY_TEXT_BLT_LEN];
    if (!w || !h) return;
    cmd[0] = X3100_CMD_XY_TEXT_BLT;
    cmd[1] = X3100_XY_CORNER(dst_x, dst_y);                       /* BR22 (signed) */
    cmd[2] = X3100_XY_CORNER(dst_x + (int32_t)w, dst_y + (int32_t)h); /* BR23 excl */
    cmd[3] = gaddr(mono_off);                         /* BR12: 1bpp src (DWORD) */
    ring_emit_cmd(cmd, X3100_XY_TEXT_BLT_LEN);
}

void x3100_hw_mono_blit(uint32_t mono_off, uint32_t mono_pitch_bytes,
                        uint32_t dst_off, uint32_t dst_pitch_px,
                        uint32_t dst_x, uint32_t dst_y,
                        uint32_t w, uint32_t h, uint32_t fg_color)
{
    if (!w || !h) return;

#if X3100_MONO_ENGINE
    /* Engine path: XY_MONO_SRC_COPY_BLT.  Mask must be MSB-first, byte-packed
     * rows, in UC memory (our pool qualifies).  Transparency is unvalidated
     * (see x3100_2d.h) — bg colour set to 0 as the transparent intent. */
    {
        uint32_t cmd[X3100_XY_MONO_SRC_COPY_BLT_LEN];
        cmd[0] = X3100_CMD_XY_MONO_SRC_COPY_BLT;
        cmd[1] = X3100_BR13_DEPTH_32 | X3100_BR13_ROP(X3100_ROP_SRCCOPY)
               | X3100_BR13_PITCH(dst_pitch_px * 4u);
        cmd[2] = X3100_XY_CORNER(dst_x, dst_y);
        cmd[3] = X3100_XY_CORNER(dst_x + w, dst_y + h);
        cmd[4] = gaddr(dst_off);
        cmd[5] = gaddr(mono_off);                    /* BR12: mono source */
        cmd[6] = 0x00000000u;                        /* background colour */
        cmd[7] = fg_color;                           /* foreground colour */
        ring_emit_cmd(cmd, X3100_XY_MONO_SRC_COPY_BLT_LEN);
        (void)mono_pitch_bytes;                      /* pitch implied by mask layout */
    }
#else
    /* Transparent CPU fallback (default, correct): write fg only where the
     * 1bpp mask bit is set, leaving the destination untouched elsewhere so
     * glyphs composite over existing content.  atlas_mono_build packs each
     * glyph as a CONTINUOUS MSB-first bit stream of width `w` (= gw) with NO
     * per-row byte padding — mono_pitch_bytes is bytes-per-GLYPH, not per-row —
     * so the bit index is row*w + col, read straight from mono_off.
     *
     * NOTE: this CPU path no longer drains the ring itself.  The CALLER must
     * drain once (x3100_wait_for_idle) before a run of mono blits, so engine
     * writes to the destination (e.g. the window-chrome fill beneath the text)
     * have landed before the CPU overwrites those pixels with glyphs.  The
     * drain was hoisted out because draining per glyph flushed the entire
     * pipeline once per character — dozens to hundreds of stalls per compose,
     * ~60x/s during a drag — which was the dominant cost that made window
     * motion unusable.  replay_draw_glyphs (the hot caller) drains once per
     * glyph run; the only other caller is the one-shot boot self-test. */
    {
        const volatile uint8_t *mask = (const volatile uint8_t *)g_x3100.vram + mono_off;
        volatile bgra_t *dbase = cpu_px(dst_off);
        uint32_t row, col;
        (void)mono_pitch_bytes;                  /* per-glyph stride, unused here */
        for (row = 0; row < h; row++) {
            volatile bgra_t *d = dbase + (dst_y + row) * dst_pitch_px + dst_x;
            uint32_t row_bit = row * w;           /* continuous: no per-row padding */
            for (col = 0; col < w; col++) {
                uint32_t bit = row_bit + col;
                if (mask[bit >> 3] & (uint8_t)(0x80u >> (bit & 7u)))
                    d[col] = (bgra_t)fg_color;
            }
        }
    }
#endif
}

/* ==========================================================================
 *  Engine helpers expected by the ported DV data plane
 *
 *  The X3100 has no FIFO / pixel-width setup like the Mach64: each BLT carries
 *  its own depth and pitch, and ordering is via the ring + MI_FLUSH.  The
 *  wait/setup helpers therefore map onto ring operations or no-ops, and the
 *  3D (alpha/stretch) primitives return false so compose falls back to the
 *  software-over path (Fase 3b will provide the render-engine implementation).
 * ========================================================================== */

bool x3100_wait_for_idle(void)
{
    x3100_ring_flush_sync();
    return true;
}

bool x3100_wait_for_fifo(uint32_t n)
{
    (void)n;
    return true;
}

int x3100_engine_reset(void)
{
    return x3100_ring_init();
}

int x3100_engine_setup_for_format(uint8_t pix_width, uint32_t pitch_pixels)
{
    (void)pix_width;
    (void)pitch_pixels;
    return 0;
}

void x3100_3d_invalidate(void)
{
    /* no 3D engine state cached yet */
}

bool x3100_hw_alpha_blit_full(uint32_t src_off, uint32_t src_pitch,
                              uint32_t sx, uint32_t sy,
                              uint32_t dst_off, uint32_t dst_pitch,
                              uint32_t dx, uint32_t dy,
                              uint32_t w, uint32_t h,
                              uint8_t const_alpha, bool use_pixel_alpha)
{
    (void)src_off; (void)src_pitch; (void)sx; (void)sy;
    (void)dst_off; (void)dst_pitch; (void)dx; (void)dy;
    (void)w; (void)h; (void)const_alpha; (void)use_pixel_alpha;
    return false;   /* -> software-over fallback in compose */
}

bool x3100_hw_stretched_blit(uint32_t src_off, uint32_t src_pitch,
                             uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh,
                             uint32_t dst_off, uint32_t dst_pitch,
                             uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh)
{
    (void)src_off; (void)src_pitch; (void)sx; (void)sy; (void)sw; (void)sh;
    (void)dst_off; (void)dst_pitch; (void)dx; (void)dy; (void)dw; (void)dh;
    return false;   /* -> software (or skip) in compose */
}

/* ==========================================================================
 *  Optional standalone validation (not used by the driver)
 * ========================================================================== */

void x3100_2d_smoketest(void)
{
    static const uint32_t bars[8] = {
        0x00FFFFFFu, 0x00FFFF00u, 0x0000FFFFu, 0x0000FF00u,
        0x00FF00FFu, 0x00FF0000u, 0x000000FFu, 0x00000000u,
    };
    uint32_t W = g_x3100.mode.width;
    uint32_t H = g_x3100.mode.height;
    uint32_t bw, i;

    if (!W || !H) return;
    bw = W / 8u;
    for (i = 0; i < 8u; i++) {
        uint32_t x = i * bw;
        uint32_t w = (i == 7u) ? (W - x) : bw;
        x3100_hw_solid_fill(g_x3100.front_offset, W, x, 0u, w, H, bars[i]);
    }
    x3100_ring_flush_sync();
}
