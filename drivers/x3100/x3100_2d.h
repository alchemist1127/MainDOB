/* x3100_2d.h — i965 (GM965, gen4) 2D blitter command + ring definitions.
 *
 * These are the hardware encodings the 3a CPU-offload path emits.  Every
 * value here is cross-checked against a real source, cited inline, because
 * a single wrong command/ring dword on real hardware is a GPU hang = one
 * wasted CD burn.  Sources:
 *
 *   [i915]  Linux drivers/gpu/drm/i915 — i915_reg.h / gt/intel_engine_regs.h
 *           (ring registers, RING_CTL_SIZE, RING_VALID, HEAD_ADDR; the
 *            G45/gen4 "head won't reset to zero" quirk).
 *   [wld]   michaelforney/wld intel/blt.h — BLT opcodes + BR00 bit layout
 *           (a minimal Intel compositor; the closest match to our use).
 *   [uxa]   xf86-video-intel src/uxa/intel_uxa.c — BR13 depth bits, BR16,
 *           cmd = OPCODE | (len-2), the i830..i965 shared blit path.
 *   [prm]   Intel PRM Vol 2 "Blitter" / Vol 2d "Command Reference: Structures"
 *           — BR field meanings; the classical-vs-3D-BLT split (alpha/stretch
 *            are "3D BLTs", not handled here); BR05/BR06 = mono expansion
 *            background/foreground colors (the glyph path).
 *
 * Scope: classical 2D BLTs only — solid fill, opaque copy, and (next step)
 * monochrome glyph expansion.  Alpha-blended / stretched / rotated blits are
 * "3D BLTs" on this hardware and belong to 3b (render engine); they are NOT
 * defined here.
 */

#ifndef MAINDOB_DRIVERS_X3100_2D_H
#define MAINDOB_DRIVERS_X3100_2D_H

#include <dob/types.h>

/* ==========================================================================
 *  Render ring command-streamer registers   [i915 intel_engine_regs.h]
 *
 *  On gen4 there is a single ring (the render CS); the dedicated blitter ring
 *  is gen6+.  2D BLT commands are submitted on THIS ring.  Offsets are from
 *  RENDER_RING_BASE; access as mmio[reg >> 2].
 * ========================================================================== */

#define X3100_RENDER_RING_BASE   0x02000u

#define X3100_RING_TAIL          (X3100_RENDER_RING_BASE + 0x30u)  /* 0x2030 */
#define X3100_RING_HEAD          (X3100_RENDER_RING_BASE + 0x34u)  /* 0x2034 */
#define X3100_RING_START         (X3100_RENDER_RING_BASE + 0x38u)  /* 0x2038 */
#define X3100_RING_CTL           (X3100_RENDER_RING_BASE + 0x3Cu)  /* 0x203C */
#define X3100_RING_MI_MODE       (X3100_RENDER_RING_BASE + 0x9Cu)  /* 0x209C */

/* RING_HEAD / RING_TAIL: the offset (in bytes) into the ring, masked.
 * HEAD also carries wrap-count bits in the high word. [i915] */
#define X3100_RING_HEAD_ADDR     0x001FFFFCu   /* byte offset field of HEAD   */
#define X3100_RING_TAIL_ADDR     0x001FFFF8u   /* byte offset field of TAIL   */

/* RING_CTL: length field is (ring_size_bytes - PAGE_SIZE) occupying the page
 * count, OR'd with RING_VALID to enable.  [i915: RING_CTL_SIZE(size) =
 * (size) - PAGE_SIZE ; RING_VALID = 0x1] */
#define X3100_RING_VALID         0x00000001u
#define X3100_RING_CTL_SIZE(bytes)  (((uint32_t)(bytes)) - 0x1000u)

/* MI_MODE: clearing STOP_RING lets the CS run.  Masked register: the write
 * sets the mask bit (high 16) to commit the value bit (low 16).  [i915]
 * STOP_RING = bit 8; _MASKED_BIT_DISABLE(x) = (x) << 16. */
#define X3100_MI_MODE_STOP_RING        (1u << 8)
#define X3100_MASKED_DISABLE(bit)      ((uint32_t)(bit) << 16)  /* clear `bit` */
#define X3100_MI_MODE_RUN              X3100_MASKED_DISABLE(X3100_MI_MODE_STOP_RING)

/* gen4/G45 quirk: after writing RING_HEAD=0 the read-back may not be zero;
 * re-write until it sticks (bounded retry) before programming CTL, or the
 * CS resumes from a garbage HEAD and hangs.  [i915 init_render_ring comment:
 * "G45 ring initialization fails to reset head to zero"] */
#define X3100_RING_HEAD_RESET_RETRIES  16u

/* ==========================================================================
 *  MI (Memory Interface) commands                              [prm / i915]
 *  MI client = 0 (bits 31:29); opcode in bits 28:23.
 * ========================================================================== */

#define X3100_MI_NOOP            0x00000000u
#define X3100_MI_FLUSH           (0x04u << 23)   /* 0x02000000 — flush caches  */
/* MI_FLUSH low-bit flush selectors (verify exact bits on HW before relying on
 * a subset; plain MI_FLUSH flushes the render/2D cache, which is what scanout
 * coherency needs after a blit). */
#define X3100_MI_FLUSH_WRITE_DIRTY     (1u << 2)
#define X3100_MI_BATCH_BUFFER_START    (0x31u << 23)

/* ==========================================================================
 *  BLT command header — BR00                                         [wld]
 *    31:29 CLIENT (= 2 for the BLT engine)
 *    28:22 OPCODE
 *    21    write ALPHA channel (set for 32bpp targets)
 *    20    write RGB channels  (set for 32bpp targets)
 *    16    PACKING
 *    15    SRC tiling enable
 *    11    DST tiling enable
 *    7:0   DWORD LENGTH (total dwords - 2)
 * ========================================================================== */

#define X3100_BLT_CLIENT         (0x2u << 29)

#define X3100_BLT_OP_XY_SETUP            (0x01u << 22)
#define X3100_BLT_OP_XY_TEXT             (0x26u << 22)
#define X3100_BLT_OP_XY_TEXT_IMMEDIATE   (0x31u << 22)
#define X3100_BLT_OP_XY_COLOR            (0x50u << 22)
#define X3100_BLT_OP_XY_SRC_COPY         (0x53u << 22)
#define X3100_BLT_OP_XY_MONO_SRC_COPY    (0x54u << 22)

#define X3100_BLT_WRITE_ALPHA    (1u << 21)
#define X3100_BLT_WRITE_RGB      (1u << 20)
#define X3100_BLT_WRITE_RGBA     (X3100_BLT_WRITE_ALPHA | X3100_BLT_WRITE_RGB)
#define X3100_BLT_SRC_TILED      (1u << 15)
#define X3100_BLT_DST_TILED      (1u << 11)

/* ==========================================================================
 *  BR13 — raster op + color depth + destination pitch               [uxa]
 *    25:24 color depth: 00=8bpp, 01=16bpp(=1<<24), 11=32bpp(=3<<24)
 *    23:16 raster op (ROP3)
 *    15:0  destination pitch in BYTES (linear surfaces; tiled is in dwords)
 * ========================================================================== */

#define X3100_BR13_DEPTH_8       (0u << 24)
#define X3100_BR13_DEPTH_16      (1u << 24)
#define X3100_BR13_DEPTH_32      (3u << 24)
#define X3100_BR13_ROP(rop3)     (((uint32_t)(rop3) & 0xFFu) << 16)
#define X3100_BR13_PITCH(bytes)  ((uint32_t)(bytes) & 0xFFFFu)

/* ROP3 codes we use. */
#define X3100_ROP_PATCOPY        0xF0u   /* dst = pattern (solid fill color)   */
#define X3100_ROP_SRCCOPY        0xCCu   /* dst = src   (opaque copy / glyph)  */

/* Dword length of each command on gen4 (linear, untiled). */
#define X3100_XY_COLOR_BLT_LEN     6u
#define X3100_XY_SRC_COPY_BLT_LEN  8u

/* ----- BR00 builders for the two opaque primitives (32bpp targets) ----- */

#define X3100_CMD_XY_COLOR_BLT \
    (X3100_BLT_CLIENT | X3100_BLT_OP_XY_COLOR    | X3100_BLT_WRITE_RGBA | \
     (X3100_XY_COLOR_BLT_LEN - 2u))

#define X3100_CMD_XY_SRC_COPY_BLT \
    (X3100_BLT_CLIENT | X3100_BLT_OP_XY_SRC_COPY | X3100_BLT_WRITE_RGBA | \
     (X3100_XY_SRC_COPY_BLT_LEN - 2u))

/* A rectangle corner packs as (y << 16) | x; the second corner is exclusive.
 * BR09 (dst) and BR12 (src) carry GTT graphics addresses; BR11 the src pitch
 * in bytes; BR16 the fill color (XY_COLOR_BLT).  [prm BR09/BR11/BR12/BR16] */
#define X3100_XY_CORNER(x, y)    (((uint32_t)(y) << 16) | ((uint32_t)(x) & 0xFFFFu))

/* ==========================================================================
 *  XY_SETUP_BLT (0x01) + XY_TEXT_BLT (0x26) — glyph rendering on the engine.
 *
 *  Source: wld/intel/blt.h (MIT, M. Forney), gen4 path; cross-checked against
 *  xf86-video-intel UXA for the BR13 depth bits.  This is the VALIDATED answer
 *  to the transparency question the XY_MONO_SRC_COPY note below could not pin
 *  down: transparent monochrome expansion is the SINGLE bit 29
 *  (MONO_SRC_TRANSPARENT) of the setup/BR13 dword — not a 3-bit selector.  With
 *  it set, expansion writes the foreground only where the 1bpp source bit is 1
 *  and leaves the destination untouched elsewhere (glyphs composite over
 *  existing pixels); the background colour is then ignored.
 *
 *  Use: emit ONE XY_SETUP_BLT (fg, transparency, ROP=SRCCOPY, 32bpp, dst
 *  base+pitch) immediately before a CONTIGUOUS run of glyphs, then ONE
 *  XY_TEXT_BLT per glyph (dst rect + mono source address).  XY_TEXT_BLT derives
 *  the source dimensions from the destination rect and reads the 1bpp source as
 *  a CONTINUOUS bit stream (packing = bit, BR00 bit 16 = 0) of that width —
 *  matching atlas_mono_build's continuous packing.  The source address (BR12)
 *  must be DWORD aligned (atlas_mono_build dword-aligns each glyph start).
 *
 *  XY_SETUP_BLT (8 dw): BR00, BR01(transp|depth|rop|dst pitch), BR24/BR25 clip,
 *  BR09 dst addr, BR05 bg, BR06 fg, BR07 pattern addr(0).  BR01 shares BR13's
 *  field layout (depth 25:24, rop 23:16, pitch 15:0, transp bit 29).
 *  XY_TEXT_BLT  (4 dw): BR00, BR22(dst y1,x1), BR23(dst y2,x2 excl), BR12 src.
 *
 *  CLIPPING: assert CLIP_ENABLE (BR01 bit 30) in the setup and load the clip
 *  rect (BR24 top-left INCLUSIVE, BR25 bottom-right EXCLUSIVE).  The BLT engine
 *  then clips XY_TEXT_BLT writes to that rect [PRM: XY_ commands clip against
 *  the rectangle when Clip Enable is set; top/left inclusive, bottom/right
 *  exclusive; negative destination coordinates are supported].  Emitting each
 *  glyph at its FULL cell with the clip set to the surface bounds lets text
 *  that overflows a window be clipped by hardware while the 1bpp source is
 *  still read at its native gw-bit row stride — so an over-wide glyph is
 *  clipped, not sheared.  The opaque blits (XY_COLOR/XY_SRC_COPY) leave Clip
 *  Enable clear, so this setup-time clip never affects them. */
#define X3100_BLT_MONO_SRC_TRANSPARENT  (1u << 29)   /* BR01/BR13 bit 29 */
#define X3100_BLT_CLIP_ENABLE           (1u << 30)   /* BR01/BR13 bit 30 */

#define X3100_XY_SETUP_BLT_LEN  8u
#define X3100_CMD_XY_SETUP_BLT \
    (X3100_BLT_CLIENT | X3100_BLT_OP_XY_SETUP | X3100_BLT_WRITE_RGBA | \
     (X3100_XY_SETUP_BLT_LEN - 2u))

#define X3100_XY_TEXT_BLT_LEN   4u
#define X3100_CMD_XY_TEXT_BLT \
    (X3100_BLT_CLIENT | X3100_BLT_OP_XY_TEXT | \
     (X3100_XY_TEXT_BLT_LEN - 2u))      /* packing = bit (BR00 bit 16 = 0) */

/* ==========================================================================
 *  XY_MONO_SRC_COPY_BLT — monochrome 1bpp source expanded to fg/bg   [prm 14.x]
 *
 *  Classic gen4 layout (8 dwords): BR00, BR13, BR22(dst y1,x1),
 *  BR23(dst y2,x2 excl.), BR09(dst addr), BR12(mono src addr), bg color,
 *  fg color.  The 1bpp source MUST live in UC (non-cacheable) memory — the
 *  PRM forbids monochrome sources in cacheable memory; our GTT pool PTEs
 *  (phys|1, no cache bits) are UC on gen4, so the cached glyph mask qualifies.
 *
 *  TRANSPARENCY CAVEAT: a plain XY_MONO_SRC_COPY_BLT writes BOTH fg (bit=1)
 *  and bg (bit=0) — i.e. OPAQUE, painting a bg-coloured box behind each glyph.
 *  Transparent text (leave the destination untouched where bit=0, so glyphs
 *  composite over whatever is already there) needs the "destination
 *  transparency mode" field — per the PRM a 3-bit per-pixel write-mask
 *  selector — whose exact bit position on gen4 we could NOT confirm from the
 *  available sources.  The engine mono path in x3100_2d.c is therefore GATED
 *  OFF by default; the correct transparent CPU fallback ships instead, until
 *  the engine encoding is validated on the panel. */
#define X3100_XY_MONO_SRC_COPY_BLT_LEN  8u
#define X3100_CMD_XY_MONO_SRC_COPY_BLT \
    (X3100_BLT_CLIENT | X3100_BLT_OP_XY_MONO_SRC_COPY | X3100_BLT_WRITE_RGBA | \
     (X3100_XY_MONO_SRC_COPY_BLT_LEN - 2u))

/* Glyph path selector.  0 = transparent CPU fallback (correct, ships a working
 * desktop with glyphs not yet on the engine); 1 = engine XY_MONO_SRC_COPY_BLT
 * (CPU-free, but its transparency encoding is unvalidated — flip on a probe
 * burn to test). */
#ifndef X3100_MONO_ENGINE
#define X3100_MONO_ENGINE  0
#endif

/* ==========================================================================
 *  Engine API
 *
 *  The hardware primitives (x3100_hw_solid_fill / _blit / _blit_overlap /
 *  _line / _host_blit / _mono_blit) and the engine helpers (x3100_engine_reset,
 *  x3100_engine_setup_for_format, x3100_wait_for_fifo, x3100_wait_for_idle,
 *  x3100_hw_alpha_blit_full, x3100_hw_stretched_blit, x3100_3d_invalidate) are
 *  declared in x3100_state.h (matching the mach64 signatures the ported DV
 *  data plane calls) and implemented in x3100_2d.c via the i965 BLT engine.
 *
 *  The functions below are x3100-specific ring management, not part of the
 *  mach64-shared engine API.
 * ========================================================================== */

/* Bring up the render ring (allocates the ring in GTT-mapped memory, applies
 * the gen4 head-reset quirk, enables the CS).  Returns 0 on success. */
int  x3100_ring_init(void);

/* Flush the 2D/render cache and block until the CS has drained (HEAD==TAIL),
 * so the panel scans finished pixels.  Bounded wait.  x3100_wait_for_idle()
 * is a thin wrapper over this. */
void x3100_ring_flush_sync(void);

/* Optional standalone validation: paint EBU bars into the scanout buffer via
 * the ENGINE (not the CPU).  Not used by the driver; kept for probe builds. */
void x3100_2d_smoketest(void);

#endif /* MAINDOB_DRIVERS_X3100_2D_H */
