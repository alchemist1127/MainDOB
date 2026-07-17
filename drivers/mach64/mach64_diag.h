/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * mach64_diag.h — engine diagnostic opcode + payload, shared between the
 * mach64 driver (data-plane dispatch) and the enginediag tool.
 *
 * This is a DATA-PLANE opcode issued via the int 0x85 boomerang
 * (dv_call_pl), in a private range that does not collide with any dobVideo
 * DV_* opcode.  The driver performs a hardware solid-fill probe and fills
 * the struct below with engine status registers read immediately after, so
 * the 2D engine can be diagnosed on real hardware with no serial logs.
 *
 * Both sides must agree on the opcode value and the struct layout.
 */
#ifndef MACH64_DIAG_H
#define MACH64_DIAG_H

#include <dob/types.h>

/* Private diagnostic opcode (well outside the dobVideo DV_* opcode map). */
#define MACH64_DV_ENGINE_DIAG   0x7E570001u   /* "TEST" */

typedef struct {
    /* identity / engine-enable state */
    uint32_t chip_id;          /* CONFIG_CHIP_ID  (MMIO aliasing sanity)   */
    uint32_t gen_test_cntl;    /* bit 8 GEN_GUI_EN must be set (enabled)   */
    uint32_t bus_cntl;         /* FIFO/HOST error bits live here           */

    /* engine status around the test fill */
    uint32_t gui_stat_before;  /* bit 0 = ENGINE_BUSY                      */
    uint32_t fifo_stat_before; /* occupied-slot bitmask                    */
    uint32_t gui_stat_during;  /* read right AFTER triggering a big fill:  */
                               /*   bit0 set -> engine is actually drawing */
                               /*   bit0 clear -> not executing the draw    */
    uint32_t gui_stat_after;   /* still busy after the fill? -> wedged      */
    uint32_t fifo_stat_after;

    /* read-back of the setup registers (written by engine_setup):
     * if a value read back != what we wrote, that register does not hold
     * -> MMIO/aliasing problem, which would explain "draws nothing". */
    uint32_t dst_off_pitch_rb;
    uint32_t sc_right_rb;
    uint32_t sc_bottom_rb;
    uint32_t dp_mix_rb;
    uint32_t dp_write_mask_rb;
    uint32_t dp_src_rb;
    uint32_t dp_pix_width_rb;
    uint32_t dp_frgd_clr_rb;

    /* the decisive probe: CPU read-back of the framebuffer pixel that the
     * HARDWARE fill should have written.  Equal to fill_color -> the
     * blitter wrote it (hardware works).  Equal to the sentinel (0) ->
     * the blitter wrote nothing. */
    uint32_t fill_color;       /* what we asked the engine to write        */
    uint32_t fb_pixel_after;   /* what is actually in VRAM after the fill  */
    uint32_t engine_reset_rc;  /* 0 if engine_reset()/setup reported idle  */

    /* aperture / memory state (PRG 5.2.2 calls CONFIG_CNTL "very important"
     * for the aperture configuration the engine and CPU share). */
    uint32_t config_cntl;
    uint32_t mem_cntl;
    uint32_t cnfg_stat0;       /* memory-type strap: bits[2:0] 4=SDRAM,5=SGRAM
                                * (Rage Mobility: 4=SDRAM,5=SGRAM,6/7=2:1) */
    /* what the engine actually latched for the test fill's geometry: shows
     * whether X/Y or W/H are transposed (GUI_STAT scissor-clip flags). */
    uint32_t dst_y_x_rb;
    uint32_t dst_height_width_rb;

    /* --- Display timing (added for the flash investigation) ---
     * All in microseconds, measured on real hardware via SYS_CLOCK_US.
     * These tell us whether "wait for vblank, then update inside the
     * blanking" can work: if a full compose (compose_full_us) is much
     * larger than the blanking window (vblank_us), it can't — the update
     * overruns blanking and the panel scans a half-drawn frame (the flash).
     * A single region blit (blit_region_us) is the cheap operation we'd
     * want to fit inside the blank instead. frame_interval_us is the time
     * between two vblanks (~16667 at 60 Hz) — a sanity check that the
     * vblank polling actually tracks the panel. */
    uint32_t vblank_us;          /* duration of one vertical-blank window  */
    uint32_t frame_interval_us;  /* vblank-to-vblank period (~16667 @60Hz)  */
    uint32_t compose_full_us;    /* time for one full-screen solid fill     */
    uint32_t blit_region_us;     /* time for one ~300x200 region blit       */
    uint32_t timing_valid;       /* 1 if the timing fields were measured    */

    /* VRAM budget — to decide if a 3 MB second buffer (page-flip) fits. */
    uint32_t vram_total_kib;     /* total VRAM                              */
    uint32_t vram_free_kib;      /* free VRAM right now                     */
    uint32_t second_buf_kib;     /* size a 2nd framebuffer would need       */

    /* Runtime state of the page-flip, as actually configured by the driver
     * at init — NOT the "does it fit" prediction above. On a physical
     * machine there are no logs, so this is the only way to confirm from
     * screen whether real double-buffering came up (1) or the driver fell
     * back to the single-buffer path because the back page couldn't be
     * allocated (0). */
    uint32_t double_buffered;    /* 1 = real page-flip active, 0 = fallback  */

    /* === Compose layer-order snapshot ==================================
     * Captured by dv_compose on its most recent run: the visible layers in
     * the exact paint order the driver used (after the z-sort), so the
     * z-order can be inspected on the metal with no logs. This is how we
     * tell whether windows actually reach the driver above the desktop
     * icons (icons live in the backbuf layer at z=0; windows should be
     * z>=10 and therefore appear LATER in this list = painted on top).
     * If a window shows up with z < the icon/backbuf layer, or is absent
     * entirely, the bug is upstream; if the order here is correct but the
     * screen still shows icons on top, the bug is in the paint itself. */
    uint32_t compose_layer_count;        /* how many entries below are valid */
    int32_t  compose_layer_z[16];        /* z of each layer, in paint order  */
    uint32_t compose_layer_is_cmdlist[16];/* 1=cmdlist layer, 0=surface layer */
    uint32_t compose_layer_x[16];        /* dst_rect.x (helps identify which) */
    uint32_t compose_layer_w[16];        /* dst_rect.w                        */
} mach64_engine_diag_t;

#endif /* MACH64_DIAG_H */
