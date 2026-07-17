/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * MainDOB Intel GMA X3100 driver — hardware register definitions.
 *
 * TARGET: Intel GMA X3100, chipset Mobile Intel GM965 ("Crestline"),
 * PCI 8086:2A02 (some SKUs 8086:2A03), as found on the Acer Extensa
 * 5220 with an internal LVDS TFT panel. Modeset is done natively by
 * driving the display engine registers directly (NO BIOS VBE, NO GRUB
 * framebuffer) — the same approach as the Mach64 driver, applied to
 * Intel hardware.
 *
 * ===========================================================================
 *  SOURCE OF TRUTH
 * ===========================================================================
 *  PRIMARY: Intel 965 Express Chipset Family and G35 PRM, Volume 3:
 *           Display Registers, Jan 2008 Rev 1.0c (Creative Commons).
 *           Mirror: https://www.x.org/docs/intel/VOL_3_display_registers.pdf
 *           Section numbers below (§2.x) refer to this document.
 *
 *  CROSS-CHECK (to be used when filling bit-level TODOs):
 *           - Linux i915: drivers/gpu/drm/i915/  (i915_reg.h, the
 *             intel_display*.c modeset path, intel_lvds.c, intel_dpll*.c)
 *           - X.Org xf86-video-intel: src/i830_reg.h, src/i830_display.c
 *           - The chipset's own VBIOS register dump at boot (read the
 *             registers the firmware already programmed and reuse the
 *             values — the Mach64 driver did exactly this with
 *             LCD_GEN_CNTL = 0x407524DE).
 *
 * ===========================================================================
 *  HONESTY MARKERS  — read before trusting any constant here
 * ===========================================================================
 *  Three confidence levels are tagged on every definition:
 *
 *    [PRM-IDX]   Offset/name taken from the PRM Volume 3 table of
 *                contents (section titles + register-summary block
 *                ranges). The MMIO block and the register's existence
 *                are confirmed; the EXACT byte offset within its block
 *                still needs a read of the register's PRM page.
 *
 *    [I965-WK]   Absolute offset is the well-known i965/Gen4 value used
 *                consistently by Linux i915 and X.Org. Very likely
 *                correct for GM965, but CONFIRM against the PRM page
 *                before a write that drives the panel.
 *
 *    TODO[PRM §x.y]  A bit-field layout or numeric value that this header
 *                does NOT yet contain because it could not be verified
 *                from the index alone. DO NOT invent it — read §x.y of
 *                the PRM (or cross-check i915) and fill it in. Writing a
 *                guessed value here can drive the real panel out of
 *                range. These are deliberately left as compile-visible
 *                gaps.
 *
 *  Nothing in this file should be written to the hardware until the
 *  TODO for that register's bit layout is resolved. The offsets let us
 *  build the skeleton and the read-only register dump (which is the
 *  safe first step on real hardware); the writes wait on the PRM pages.
 * ===========================================================================
 */

#ifndef MAINDOB_DRIVERS_X3100_HW_H
#define MAINDOB_DRIVERS_X3100_HW_H

#include <dob/types.h>   /* not stdint.h — see mach64_hw.h rationale */

/* ===========================================================================
 *  0. MMIO aperture
 * ===========================================================================
 *  On the i965 the MMIO register block is a separate PCI BAR from the
 *  graphics aperture/framebuffer. Per Gen4 convention (CONFIRM against
 *  the chipset datasheet PCI config section, which the PRM explicitly
 *  excludes):
 *    BAR0 (MMIO_BAR)  : MMIO registers, 512 KB (or 1 MB) — [I965-WK]
 *    BAR2 (GMADR)     : graphics memory aperture (the framebuffer
 *                       window), prefetchable, large (256 MB seen at
 *                       0xE0000000 on a real GM965 dmesg)
 *  All register offsets below are relative to the MMIO BAR base.
 *
 *  The driver maps MMIO_BAR with mmap_phys(dev.bar[0], MMIO_SIZE) and
 *  the aperture with mmap_phys(dev.bar[2], fb_bytes), mirroring how the
 *  mach64 driver mapped its BAR0 and derived g_mmio = g_fb + 0x7FFC00.
 *  (On Intel the two are separate BARs, not one aperture-aliased BAR.) */
#define X3100_MMIO_BAR_INDEX        0          /* [I965-WK] confirm in PCI cfg */
#define X3100_GMADR_BAR_INDEX       2          /* [I965-WK] aperture / fb window */
#define X3100_MMIO_SIZE             (512u * 1024u)   /* [I965-WK] 512 KB MMIO */

/* ===========================================================================
 *  1. Display Clock / PLL  — block 0x06000..0x06FFF   (§2.5) [PRM-IDX]
 * ===========================================================================
 *  The DPLL produces the dot clock. Two PLLs: DPLL A (pipe A) and
 *  DPLL B (pipe B). Each has a control register and a pair of FP
 *  (divisor) registers selecting the N/M1/M2 feedback + reference
 *  divide that set the output frequency.
 *
 *  Absolute offsets [I965-WK] (standard i965, confirm vs §2.5.x):
 */
#define X3100_DPLL_A_CTRL           0x06014    /* §2.5.4 DPLLA_CTRL   [I965-WK] */
#define X3100_DPLL_B_CTRL           0x06018    /* §2.5.5 DPLLB_CTRL   [I965-WK] */
#define X3100_DPLL_A_MD             0x0601C    /* §2.5.6 DPLLAMD (SDVO/HDMI mul) [I965-WK] */
#define X3100_DPLL_B_MD             0x06020    /* §2.5.7 DPLLBMD                 [I965-WK] */
#define X3100_FPA0                  0x06040    /* §2.5.8  FPA0 divisor 0 [I965-WK] */
#define X3100_FPA1                  0x06044    /* §2.5.9  FPA1 divisor 1 [I965-WK] */
#define X3100_FPB0                  0x06048    /* §2.5.10 FPB0 divisor 0 [I965-WK] */
#define X3100_FPB1                  0x0604C    /* §2.5.11 FPB1 divisor 1 [I965-WK] */
#define X3100_DPLL_TEST             0x0606C    /* §2.5.12 [I965-WK] */
#define X3100_D_STATE               0x06104    /* §2.5.13 [PRM-IDX] */
#define X3100_DSPCLK_GATE_D         0x06200    /* §2.5.14 [PRM-IDX] */

/* DPLL{A,B}_CTRL bit layout — CONFIRMED against Linux i915 i915_reg.h
 * (gen4/i965). Offsets are facts; the per-mode numeric VALUES of the
 * divider fields are computed/copied at modeset time, not constants. */
#define X3100_DPLL_VCO_ENABLE       0x80000000u  /* 1<<31 enable PLL, then settle-wait */
#define X3100_DPLL_VGA_MODE_DIS     0x10000000u  /* 1<<28 disable legacy VGA clocking  */
#define X3100_DPLL_MODE_MASK        0x0C000000u  /* 27:26 clock mode select            */
#define X3100_DPLLB_MODE_DAC_SERIAL 0x04000000u  /* 1<<26 DAC / SDVO mode              */
#define X3100_DPLLB_MODE_LVDS       0x08000000u  /* 2<<26 LVDS mode (internal panel)   */
#define X3100_DPLL_P2_DIV_MASK      0x03000000u  /* 25:24 P2 post-divider              */
#define X3100_DPLLB_LVDS_P2_DIV_14  0x00000000u  /* 0<<24 LVDS P2=14 (low dot clock)   */
#define X3100_DPLLB_LVDS_P2_DIV_7   0x01000000u  /* 1<<24 LVDS P2=7  (high dot clock)  */
#define X3100_DPLL_P1_POST_DIV_MASK 0x00FF0000u  /* 23:16 P1 field, encoded 1<<(p1-1)  */
/* P1 is a one-hot field: program (1u << (p1 - 1)) into the masked bits.
 * P2 for LVDS is 14 below ~112 MHz dot clock, else 7 (i915 g4x limits). */

/* FP{A,B}{0,1} divider field layout — i915 FP_*_DIV (non-Pineview).
 * Shifts are universal; mask widths [I965-WK] — eyeball vs i915_reg.h
 * (FP_N_DIV_MASK / FP_M1_DIV_MASK / FP_M2_DIV_MASK) before a write. */
#define X3100_FP_N_SHIFT            16
#define X3100_FP_N_MASK             0x003F0000u  /* bits 21:16 */
#define X3100_FP_M1_SHIFT           8
#define X3100_FP_M1_MASK            0x00003F00u  /* bits 13:8  */
#define X3100_FP_M2_SHIFT           0
#define X3100_FP_M2_MASK            0x0000003Fu  /* bits  5:0  */
/* The dot clock relates to N/M1/M2/P1/P2 by the gen4 PLL formula, but the
 * exact +2/-2 field offsets are gen-specific and MUST be taken from i915
 * g4x_calc_dpll_params() / i9xx_dpll_compute_m() — do NOT hardcode a
 * formula here (a wrong offset yields a wrong pixel clock). For the 5220's
 * NATIVE 1280x800 mode the robust path is to READ the firmware's FPB0 /
 * DPLLB values from the probe dump and reuse them verbatim; the
 * compute-from-scratch path is only needed for non-native modes. */

/* GM965 display PLL reference clock.
 * 96 MHz is the confirmed i965 refclk for DAC/SDVO (OSDev / i915). Mobile
 * LVDS configs can instead use a 100 MHz SSC clock — still CONFIRM on the
 * 5220 by reading the firmware's PLL/refclk selection in the probe dump. */
#define X3100_PLL_REFCLK_KHZ        96000      /* DAC/SDVO; confirm LVDS 96 vs 100 SSC */

/* ===========================================================================
 *  2. Pipe timing  — block 0x60000..0x6FFFF   (§2.7) [PRM-IDX]
 * ===========================================================================
 *  CRTC timing for each pipe. Direct analogue of the mach64 CRTC_*_TOTAL
 *  / SYNC registers. Pipe A at 0x60000, Pipe B at 0x61000. [I965-WK]
 *
 *  Which pipe drives the internal LVDS panel is NOT assumed: on many
 *  Crestline laptops the integrated panel is on pipe B and the analog
 *  CRT on pipe A, but this MUST be read from the hardware on the 5220
 *  (check which pipe the LVDS port register selects). The dump step
 *  will tell us.
 */
#define X3100_PIPE_A_BASE           0x60000    /* [I965-WK] */
#define X3100_PIPE_B_BASE           0x61000    /* [I965-WK] */

/* Per-pipe timing register offsets from the pipe base. [PRM-IDX §2.7.1.x]
 * Each is a 32-bit register packing (total-1)<<16 | (active-1), etc.
 * TODO[PRM §2.7.1]: confirm the exact pack (start/end vs total/active)
 * for each — Intel packs HTOTAL as (htotal-1)<<16 | (hactive-1). */
#define X3100_HTOTAL                0x00       /* HTOTAL_x  §2.7.1.1 */
#define X3100_HBLANK                0x04       /* HBLANK_x  §2.7.1.2 */
#define X3100_HSYNC                 0x08       /* HSYNC_x   §2.7.1.3 */
#define X3100_VTOTAL                0x0C       /* VTOTAL_x  §2.7.1.4 */
#define X3100_VBLANK                0x10       /* VBLANK_x  §2.7.1.5 */
#define X3100_VSYNC                 0x14       /* VSYNC_x   §2.7.1.6 */
#define X3100_PIPESRC               0x1C       /* PIPExSRC  §2.7.1.7 (w-1)<<16|(h-1) */
/* helper: absolute timing register = PIPE_x_BASE + X3100_HTOTAL etc.
 * (offsets above are [PRM-IDX], byte spacing is the well-known i965
 * 0x00/04/08/0C/10/14/1C layout — CONFIRM each on its PRM page.) */

/* ===========================================================================
 *  3. Pipe configuration  — block 0x70000   (§2.10) [PRM-IDX]
 * ===========================================================================
 */
#define X3100_PIPEA_CONF            0x70008    /* §2.10.1.3 PIPEACONF [I965-WK] */
#define X3100_PIPEB_CONF            0x71008    /* §2.10.4.3 PIPEBCONF [I965-WK] */
#define X3100_PIPEA_DSL             0x70000    /* §2.10.1.1 scan line (RO) [I965-WK] */
#define X3100_PIPEB_DSL             0x71000    /* §2.10.4.1 [I965-WK] */
#define X3100_PIPEA_STAT            0x70024    /* §2.10.1.7 PIPEASTAT [I965-WK] */
#define X3100_PIPEB_STAT            0x71024    /* §2.10.4.7 PIPEBSTAT [I965-WK] */

/* PIPExCONF bits — CONFIRMED i915 (gen4/i965; "TRANSCONF_*" in newer trees). */
#define X3100_PIPECONF_ENABLE       0x80000000u  /* 1<<31 enable the pipe          */
#define X3100_PIPECONF_STATE_ENABLE 0x40000000u  /* 1<<30 RO: pipe actually running
                                                  * (pre-i965 this bit = double-wide;
                                                  *  the 5220 is i965 -> state bit). */
/* Mode-set order: enable the pipe AFTER plane+port, then poll
 * PIPECONF_STATE_ENABLE (and/or wait one vblank) before enabling the
 * plane. BPC/dither bits are not needed for the 32bpp native path. */

/* PIPExSTAT bits — vblank status/ack, needed only for the vsync data path
 * (Fase 3), not for modeset. Take the exact bits from i915 i915_reg.h
 * (PIPE_VBLANK_INTERRUPT_STATUS / _ENABLE, W1C ack) when wiring vsync;
 * left unfilled here on purpose rather than guessed. */

/* ===========================================================================
 *  4. Display plane (primary)  — block 0x70180 / 0x71180   (§2.10.5/2.10.6)
 * ===========================================================================
 *  The plane reads pixels from the aperture and feeds the pipe. The key
 *  registers for scanning out our framebuffer:
 *    DSPxCNTR  : enable + pixel format (32bpp xRGB)
 *    DSPxSTRIDE: bytes per scanline (the pitch)
 *    DSPxSURF  : surface base address IN THE APERTURE (GMADR offset)
 *    DSPxLINOFF: linear offset (usually 0)
 *  Writing DSPxSURF is what arms a new scanout buffer (the i965 plane
 *  registers are double-buffered; the SURF write latches them at the
 *  next vblank — see §2.2.1 Pipe register double-buffering).
 */
#define X3100_DSPA_CNTR             0x70180    /* §2.10.5.1 DSPACNTR  [I965-WK] */
#define X3100_DSPA_LINOFF           0x70184    /* §2.10.5.2 [I965-WK] */
#define X3100_DSPA_STRIDE           0x70188    /* §2.10.5.3 DSPASTRIDE [I965-WK] */
#define X3100_DSPA_SURF             0x7019C    /* §2.10.5.8 DSPASURF  [I965-WK] */
#define X3100_DSPA_TILEOFF          0x701A4    /* §2.10.5.9 [I965-WK] */

#define X3100_DSPB_CNTR             0x71180    /* §2.10.6.1 DSPBCNTR  [I965-WK] */
#define X3100_DSPB_LINOFF           0x71184    /* §2.10.6.2 [I965-WK] */
#define X3100_DSPB_STRIDE           0x71188    /* §2.10.6.3 DSPBSTRIDE [I965-WK] */
#define X3100_DSPB_SURF             0x7119C    /* §2.10.6.6 DSPBSURF  [I965-WK] */
#define X3100_DSPB_TILEOFF          0x711A4    /* §2.10.6.7 [I965-WK] */

/* DSPxCNTR bits — RESOLVED. The enable bit, 32bpp xRGB format code and
 * pipe-select field are CONFIRMED from i915 i9xx_plane_regs.h and defined
 * below as X3100_DISP_ENABLE / X3100_DISP_FORMAT_BGRX888 /
 * X3100_DISP_PIPE_SEL_B (composed into X3100_DSPBCNTR_EBU = 0x99000000).
 * Tiling stays off (linear), so no tiling bit is set. */

/* VGA plane disable — must turn off the legacy VGA plane before/at
 * modeset, or it fights the display plane. */
#define X3100_VGACNTRL              0x71400    /* §2.10.7.1 VGACNTRL [PRM-IDX] */
/* VGA plane disable — CONFIRMED i915 (VGA_DISP_DISABLE) + OSDev (set the
 * VGA Display Disable bit for non-VGA modes, VGA centering = 0). */
#define X3100_VGA_DISP_DISABLE      0x80000000u  /* 1<<31 turn off legacy VGA plane */

/* ===========================================================================
 *  5. LVDS port + panel  — [DevCL] only   (§2.8.10, §2.9) [PRM-IDX]
 * ===========================================================================
 *  THIS IS THE HARD PART (exactly as LCD_GEN_CNTL was on the Mach64).
 *  The internal panel needs: the LVDS port enabled and tied to the
 *  right pipe, the panel power sequenced ON in the correct order with
 *  the correct delays, the panel fitter configured if the mode != panel
 *  native res, and the backlight PWM enabled.
 *
 *  Power sequencing in particular is timing-critical and panel-specific:
 *  PP_ON_DELAYS / PP_OFF_DELAYS / PP_DIVISOR encode delays that, if
 *  wrong, can fail to light the panel or stress it. The SAFE source for
 *  these is the values the VBIOS already programmed — read them before
 *  overwriting. (Same lesson as the Mach64 BIOS dump.)
 */
#define X3100_LVDS                  0x61180    /* §2.8.10 LVDS port ctrl [I965-WK] */

#define X3100_PP_STATUS             0x61200    /* §2.9.1.1 PP_STATUS  [I965-WK] */
#define X3100_PP_CONTROL            0x61204    /* §2.9.1.2 PP_CONTROL [I965-WK] */
#define X3100_PP_ON_DELAYS          0x61208    /* §2.9.1.3 [I965-WK] */
#define X3100_PP_OFF_DELAYS         0x6120C    /* §2.9.1.4 [I965-WK] */
#define X3100_PP_DIVISOR            0x61210    /* §2.9.1.5 [I965-WK] */

#define X3100_PFIT_CONTROL          0x61230    /* §2.9.2.1 PFIT_CONTROL [I965-WK] */
#define X3100_PFIT_PGM_RATIOS       0x61234    /* §2.9.2.2 [I965-WK] */

#define X3100_BLC_PWM_CTL           0x61254    /* §2.9.3.2 BLC_PWM_CTL [I965-WK] */
#define X3100_BLC_PWM_CTL2          0x61250    /* §2.9.3.1 [I965-WK] */

/* LVDS register bits — port/dither/border CONFIRMED i915 (gen4/i965). */
#define X3100_LVDS_PORT_EN          0x80000000u  /* 1<<31 enable LVDS port           */
#define X3100_LVDS_PIPEB_SELECT     0x40000000u  /* 1<<30 route LVDS from pipe B
                                                  * (the 5220's panel is on pipe B). */
#define X3100_LVDS_ENABLE_DITHER    0x02000000u  /* 1<<25 dither (965/g4x)           */
#define X3100_LVDS_BORDER_ENABLE    0x00008000u  /* 1<<15 border for unscaled disp.  */
/* PANEL-SPECIFIC, NOT a constant: single vs dual channel and 18 vs 24 bpp
 * MUST match the 5220 panel. Read them from the firmware's LVDS value in
 * the probe dump (and/or the panel EDID over GMBUS); do NOT hardcode. */

/* PP_CONTROL bits — panel power sequencing.
 *   PANEL_POWER_ON = bit 0 (request power on). i915 uses a write-protect
 *   key PANEL_UNLOCK_REGS = 0xabcd<<16 on the PCH PP_CONTROL (0xc7204);
 *   whether the gen4 register here (0x61204) needs it is STILL TO CONFIRM
 *   (PRM §2.1.4). The PP_ON/OFF_DELAYS + PP_DIVISOR are timing-critical
 *   and panel-specific — SEED THEM FROM THE FIRMWARE DUMP, never guess. */
#define X3100_PANEL_POWER_ON        0x00000001u  /* bit 0 request panel power on */

/* PFIT_CONTROL bits.
 *   PFIT_ENABLE = bit 31. At the panel's NATIVE 1280x800 res no scaling is
 *   needed, so the fitter stays disabled; the scaling-mode / pipe-select
 *   fields only matter for non-native modes — take them from i915 (pfit)
 *   when implementing scaling. (i965 analogue of the mach64 HORZ_DIVBY2
 *   scaling trap.) */
#define X3100_PFIT_ENABLE           0x80000000u  /* 1<<31 enable panel fitter */

/* ===========================================================================
 *  6. GMBUS (I2C) for EDID  — block 0x05000..0x05FFF   (§2.4) [PRM-IDX]
 * ===========================================================================
 *  Reading the panel/monitor EDID over GMBUS tells us the native
 *  resolution and timing — which we need to program the pipe correctly
 *  instead of hardcoding 1024x768. Optional for a first hardcoded-mode
 *  bring-up, required for a robust driver.
 */
#define X3100_GMBUS0                0x05100    /* §2.4.6 clock/port select [I965-WK] */
#define X3100_GMBUS1                0x05104    /* §2.4.7 command/status    [I965-WK] */
#define X3100_GMBUS2                0x05108    /* §2.4.8 status            [I965-WK] */
#define X3100_GMBUS3                0x0510C    /* §2.4.9 data buffer       [I965-WK] */
#define X3100_GMBUS4                0x05110    /* §2.4.10 irq mask         [I965-WK] */
#define X3100_GMBUS5                0x05120    /* §2.4.11 2-byte index     [I965-WK] */
/* GMBUS procedure CONFIRMED (OSDev / i915); exact bit positions TO FILL
 * from i915 intel_gmbus.c (GMBUS_SW_RDY, GMBUS_HW_RDY, GMBUS_HW_WAIT_PHASE,
 * cycle WAIT/STOP, direction bit) — left unguessed:
 *   1. GMBUS0: select the panel's DDC pin pair + bus rate (100 kHz).
 *   2. send: write 4 bytes to GMBUS3, then GMBUS1 = addr|count|SW_RDY with
 *      a WAIT cycle (no index/STOP); poll HW_RDY in GMBUS2, repeat GMBUS3
 *      for each further 4 bytes.
 *   3. recv: GMBUS1 = addr|count|SW_RDY|DIR; poll HW_RDY, read GMBUS3 in
 *      4-byte chunks; on completion wait for the wait-phase bit, then issue
 *      STOP via the GMBUS1 cycle-select.
 * EDID is needed for non-native modes / robustness, not for the native
 * 1280x800 bring-up. */

/* ===========================================================================
 *  7. Mode-set sequence scaffold  (§2.2.2)
 * ===========================================================================
 *  Intel specifies the ORDER explicitly in §2.2.2 "Mode Switch
 *  Programming Sequences". This is the backbone the .c modeset follows.
 *  The high-level order (to be confirmed line-by-line against §2.2.2;
 *  this is the consensus i915/X.org sequence for Gen4):
 *
 *    POWER DOWN / DISABLE (if a mode is already up):
 *      1. Disable the display plane (clear DSPxCNTR enable), then write
 *         DSPxSURF to flush the disable (plane regs are double-buffered).
 *      2. For the panel: run the panel power-DOWN sequence via PP_CONTROL
 *         and wait for PP_STATUS, turn off backlight FIRST.
 *      3. Disable the port (LVDS_PORT_EN = 0).
 *      4. Disable the pipe (clear PIPExCONF enable), wait for the pipe
 *         state bit to clear.
 *      5. Disable the DPLL (clear DPLL_VCO_ENABLE).
 *
 *    POWER UP / SET MODE:
 *      1. Program the DPLL divisors (FPx0) for the target dot clock,
 *         then enable the DPLL (DPLL_VCO_ENABLE) and WAIT for it to
 *         settle (a fixed delay; §2.2.2 / i915 use ~150us).
 *      2. Program pipe timing (HTOTAL/HBLANK/HSYNC/VTOTAL/VBLANK/VSYNC,
 *         PIPExSRC).
 *      3. Program the port: LVDS register (pipe select, channels, bpp).
 *      4. Program panel fitter (PFIT_CONTROL) if scaling is needed.
 *      5. Enable the pipe (PIPExCONF enable), wait for the state bit.
 *      6. Program + enable the display plane (DSPxCNTR format+enable,
 *         DSPxSTRIDE = pitch, DSPxLINOFF = 0, DSPxSURF = aperture offset
 *         of the framebuffer — the SURF write latches the plane).
 *      7. Run the panel power-UP sequence (PP_CONTROL on, wait
 *         PP_STATUS), then enable backlight (BLC_PWM_CTL).
 *
 *  Every "wait" above is a real hardware settle; §2.2.2 and the panel
 *  PP delays give the durations. TODO[PRM §2.2.2]: confirm the exact
 *  order and the settle waits before driving the real panel — an
 *  out-of-order enable is the classic black-screen / no-sync failure.
 */

/* ===========================================================================
 *  VERIFIED plane-control bits — from i9xx_plane_regs.h (Linux i915,
 *  SPDX MIT, Intel 2024). These are CONFIRMED values, not TODOs.
 * ===========================================================================
 *    DISP_ENABLE          = bit 31                       (0x80000000)
 *    DISP_FORMAT_MASK     = bits 29:26
 *    DISP_FORMAT_BGRX888  = 6 in that field = 6<<26       (0x18000000)
 *                           -> 32bpp xRGB8888 (B,G,R,pad in memory)
 *    DISP_PIPE_SEL_MASK   = bits 25:24
 *    DISP_PIPE_SEL(pipe)  = pipe<<24  (pipe B = 1 -> 0x01000000)
 *    DSPSURF DISP_ADDR_MASK = bits 31:12 -> framebuffer base MUST be
 *                           4 KB aligned. */
#define X3100_DISP_ENABLE           0x80000000u
#define X3100_DISP_FORMAT_BGRX888   0x18000000u   /* 6<<26, 32bpp xRGB */
#define X3100_DISP_PIPE_SEL_B       0x01000000u   /* 1<<24 */

/* Composed DSPBCNTR value to enable plane B, 32bpp xRGB, on pipe B:
 *   0x80000000 | 0x18000000 | 0x01000000 = 0x99000000  */
#define X3100_DSPBCNTR_EBU          (X3100_DISP_ENABLE | \
                                     X3100_DISP_FORMAT_BGRX888 | \
                                     X3100_DISP_PIPE_SEL_B)

/* ===========================================================================
 *  GGTT / GMADR — gen4 layout, cross-checked against the live chip.
 * ===========================================================================
 *  The GTTMMADR BAR (BAR0) on the i965/GM965 is a 1 MB window: the first
 *  512 KB is the MMIO register block, the second 512 KB aliases the GTT
 *  entry array. So the GTT lives at MMIO_base + 512 KB — NOT + 2 MB. (The
 *  4 MB / first-2MB-MMIO + second-2MB-GTT split is the gen6+ layout; see
 *  the PGTBL_CTL note below, which already disclaims +2MB for gen4.)
 *  AGPGART/Anholt: on G965 the GTT is 512 KB (= 131072 4-byte PTEs)
 *  against a 256 MB aperture. The driver's verified EBU path uses the
 *  +512 KB alias.
 *
 *  NOTE TO RECONCILE ON HARDWARE: the probe logged MMIO base 0xFC100000
 *  (1 MB-aligned, consistent with a 1 MB BAR). The EBU path's
 *  (bar0 & 0xFFC00000) snaps that to a 4 MB boundary (0xFC000000) — only
 *  correct if GTTMMADR is actually 4 MB. Confirm the BAR size on the 5220
 *  and drop the 4 MB mask if it is 1 MB (GTT would then be @ 0xFC180000).
 *
 *  Each i965 (gen4) GGTT PTE is 4 BYTES: physical 4 KB page in the high
 *  bits + low flag bits. The live chip showed e.g. GTT[0]=0x01004401
 *  -> phys 0x01004000, low bits 0x401 (bit0=Valid set); bwidawsk's GGTT
 *  writeup notes the valid bit is always set for GGTT PTEs. So our PTE
 *  encoding for a present page is:
 *      (phys & 0xFFFFF000) | X3100_GTT_PTE_VALID
 *  (We replicate the firmware's encoding rather than invent flags.)
 *
 *  GMADR (BAR2) is the CPU-side aperture into graphics memory; the probe
 *  read it as 0xD0000000 (64-bit prefetchable). Writing pixels at
 *  (GMADR + ggtt_offset) lands them in the page the GTT maps at that
 *  offset. The aperture is 256 MB (0xD0000000..0xE0000000), so the CPU
 *  can only reach GTT byte offsets < 256 MB this way, i.e. GTT INDICES
 *  < 65536. The scanout side sees the full 512 MB GTT, but the EBU paint
 *  must keep its index below 65536 (see EBU_GTT_START_INDEX). */
#define X3100_GTT_WINDOW_OFFSET     (512u * 1024u)  /* GTT @ MMIO+512KB (gen4 alias) */
#define X3100_GTT_PTE_VALID         0x00000001u
#define X3100_GTT_PTE_ADDR_MASK     0xFFFFF000u

/* ===========================================================================
 *  PGTBL_CTL — page-table control. On gen4 (GM965) this register gives
 *  the PHYSICAL base address of the GGTT directly, and the GTT lives in
 *  stolen memory (NOT at GTTMMADR+2MB, which is the gen6+ layout).
 *  Verified from intel_reg.h (xorg-intel-gpu-tools) + i915 stolen code.
 * ===========================================================================
 *    PGETBL_CTL          = MMIO 0x02020
 *    PGTBL_ADDRESS_LO    = bits 31:12   (0xFFFFF000)
 *    PGTBL_ADDRESS_HI    = bits 7:4 -> phys bits 35:32 on gen4 (<<28)
 *    PGTBL_ENABLED       = bit 0
 *    PGETBL_SIZE_MASK    = bits 3:1 (0x0000000E):
 *        0 -> 512KB, 1<<1 -> 256KB, 2<<1 -> 128KB, 3<<1 -> 1MB, 4<<1 -> 2MB
 *    GTT size in bytes / 4 = number of PTE entries. */
#define X3100_PGETBL_CTL            0x02020
#define X3100_PGTBL_ADDR_LO_MASK    0xFFFFF000u
#define X3100_PGTBL_ADDR_HI_MASK    0x000000F0u
#define X3100_PGTBL_ENABLED         0x00000001u
#define X3100_PGTBL_SIZE_MASK       0x0000000Eu

#endif /* MAINDOB_DRIVERS_X3100_HW_H */