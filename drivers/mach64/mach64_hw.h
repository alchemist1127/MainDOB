/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * MainDOB ATI Mach64 driver — hardware register definitions.
 *
 * SCOPE: only what the v1 driver actually uses.  Anything not touched
 * by exit-VGA / modeset / engine 2D / IRQ has been deleted.  When new
 * registers are needed, add them here with the same source-citation
 * pattern.
 *
 * Target chip is specifically Mach64 LM (3D RAGE Mobility P/M, PCI ID
 * 1002:4C4D) as found on the Compaq Armada E500.  LT-Pro support is
 * deferred — variant-specific bits will be added when we have a
 * device to test on.
 *
 * SOURCES (cross-checked):
 *   - Linux atyfb (drivers/video/fbdev/aty/) — Daniel Mantione + maintainers
 *   - X.Org xf86-video-mach64 — src/atimach64io.h, ati2d.h
 *   - ATI Mach64 Register Reference Guide RRG-S00700-05
 *   - ATI Rage Pro Programmer's Guide PRG-215R3-00-10 (bitsavers)
 *   - OSDev wiki — User:Combuster/Mach64_Hardware
 *   - atyfb 2.4.23 boot log on identical chip+panel
 *     (HP Omnibook 4150B, 0x4c4d rev 0x64, CPT CLAA141XB01):
 *     "atyfb: 3D RAGE Mobility (PCI) [0x4c4d rev 0x64] 8M SDRAM,
 *      29.498928 MHz XTAL, 230 MHz PLL, 83 Mhz MCLK, 125 Mhz XCLK"
 *     "colour active matrix monitor detected: CPT CLAA141XB01
 *      id=10, 1024x768 pixels, 262144 colours (LT mode)"
 *     "LCD CRTC parameters: 15384 167 127 130 0 17 805 767 769 6"
 */

#ifndef MAINDOB_DRIVERS_MACH64_HW_H
#define MAINDOB_DRIVERS_MACH64_HW_H

#include <dob/types.h>   /* int32_t, uint32_t ecc. — NON stdint.h del cross
                          * compiler, che redichiarerebbe gli stessi typedef
                          * come "long int" causando conflicting types
                          * contro libc di MainDOB ("int"). */

/* ==========================================================================
 *  1. CRTC (display controller) — MMIO offsets in BAR2
 * ========================================================================== */

#define M64_CRTC_H_TOTAL_DISP           0x0000  /* RW32 */
#define M64_CRTC_H_SYNC_STRT_WID        0x0004  /* RW32 */
#define M64_CRTC_V_TOTAL_DISP           0x0008  /* RW32 */
#define M64_CRTC_V_SYNC_STRT_WID        0x000C  /* RW32 */
#define M64_CRTC_VLINE_CRNT_VLINE       0x0010  /* RW32 (bits 0:10 vline; 16:26 RO crnt) */
#define M64_CRTC_OFF_PITCH              0x0014  /* RW32 */

/* Hardware cursor registers (official mach64.h dword offsets *4):
 * a 64x64 2bpp sprite the CRTC overlays at scanout. */
#define M64_CUR_CLR0                    0x0060  /* RW32 — cursor colour 0; colour in bits [31:8] (0xRRGGBBxx) */
#define M64_CUR_CLR1                    0x0064  /* RW32 — cursor colour 1; colour in bits [31:8] (0xRRGGBBxx) */
#define M64_CUR_OFFSET                  0x0068  /* RW32 — VRAM offset of cursor image, in qwords */
#define M64_CUR_HORZ_VERT_POSN          0x006C  /* RW32 — (Y<<16)|X on-screen position */
#define M64_CUR_HORZ_VERT_OFF           0x0070  /* RW32 — (Yoff<<16)|Xoff hotspot/clip within sprite */
#define M64_GEN_CUR_EN                  (1u << 7)  /* GEN_TEST_CNTL bit 7: enable HW cursor */
#define M64_CRTC_INT_CNTL               0x0018  /* RW32 (IRQ enables + W1C acks) */
#define M64_CRTC_GEN_CNTL               0x001C  /* RW32 — major mode register */

/* CRTC_GEN_CNTL bit layout — definitive, verified atyfb mach64.h.
 * Bit positions here are the ATI Mach64 PC interpretation.  Some bits
 * have alias names in earlier Mach32-compat documentation; we use the
 * Mach64 names. */
#define M64_CRTC_DBL_SCAN_EN            (1u <<  0)
#define M64_CRTC_INTERLACE_EN           (1u <<  1)
#define M64_CRTC_HSYNC_DIS              (1u <<  2)   /* DPMS standby */
#define M64_CRTC_VSYNC_DIS              (1u <<  3)   /* DPMS suspend */
#define M64_CRTC_CSYNC_EN               (1u <<  4)
#define M64_CRTC_DISPLAY_DIS            (1u <<  6)   /* DPMS off / blank — 0x40 */
#define M64_CRTC_VGA_XOVERSCAN          (1u <<  7)
#define M64_CRTC_PIX_WIDTH_MASK         (0x7u << 8)
#define M64_CRTC_PIX_WIDTH_SHIFT        8
#define M64_CRTC_BYTE_PIX_ORDER         (1u << 11)
#define M64_CRTC_FIFO_LWM_MASK          (0xFu << 12)
#define M64_CRTC_LOCK_REGS              (1u << 19)
#define M64_CRTC_EXT_DISP_EN            (1u << 24)   /* 0x01000000 — the bootstrap bit */
#define M64_CRTC_EN                     (1u << 25)
#define M64_CRTC_VGA_LINEAR             (1u << 27)   /* 0x08000000 — atyfb sets this always */
#define M64_CRTC_DISP_REQ_EN_B          (1u << 26)

/* PIX_WIDTH values (bits 10:8 of CRTC_GEN_CNTL).
 * The Mach64 docs use these encodings:
 *   1bpp=000, 4bpp=001, 8bpp=010, 15bpp=011, 16bpp=100, 24bpp=101, 32bpp=110.
 * v1 only ever programs 32bpp. */
#define M64_PIX_WIDTH_1BPP              0x0   /* monochrome source (font expansion) */
#define M64_PIX_WIDTH_4BPP              0x1
#define M64_PIX_WIDTH_8BPP              0x2
#define M64_PIX_WIDTH_16BPP             0x4
#define M64_PIX_WIDTH_32BPP             0x6

/* CRTC_INT_CNTL — IRQ enables (bits 1,5,...) and W1C ack bits (bits 2,6,...).
 * v1 uses only vertical-blank. */
#define M64_CRTC_VBLANK                 (1u << 0)   /* RO — currently in vblank */
#define M64_CRTC_VBLANK_BIT             M64_CRTC_VBLANK  /* legacy alias used by mach64_irq.c */
#define M64_CRTC_VBLANK_INT_EN          (1u << 1)
#define M64_CRTC_VBLANK_INT_AK          (1u << 2)   /* W1C */

/* CRTC_OFF_PITCH packing helper.
 *
 * Layout campo Mach64:
 *   bits 0..21 : display start offset in QWORD units (bytes / 8)
 *   bits 22..31: scanline pitch in PIXEL/8
 *
 * FONTE PRIMARIA ATI (RRG-S00700-05 pag 3-41): "Display pitch in
 * pixels-times-8" -> la pitch e' pixel/8, NON bytes/8. Per 1024 px -> 128,
 * qualunque bpp (il chip ricava bytes/pixel da PIX_WIDTH). Va passato il
 * numero di PIXEL per riga (g_fb_w).
 *
 * `byte_off` e' in BYTE e va shiftato di 3 (qword). Allineamento 8-byte. */
#define M64_OFF_PITCH(byte_off, pitch_pixels) \
    (((uint32_t)(byte_off) >> 3) | (((uint32_t)(pitch_pixels) >> 3) << 22))

/* ==========================================================================
 *  2. Bus / extension / chip-identity registers
 * ========================================================================== */

#define M64_BUS_CNTL                    0x00A0  /* RW32 */
#define M64_BUS_APER_REG_DIS            (1u <<  4)  /* 0x00000010: disable reg alias in FB aperture */
#define M64_BUS_EXT_REG_EN              (1u << 27)  /* 0x08000000 */
#define M64_BUS_FIFO_ERR_ACK            (1u << 21)  /* W1C */
#define M64_BUS_HOST_ERR_ACK            (1u << 23)  /* W1C */

#define M64_GEN_TEST_CNTL               0x00D0  /* RW32 */
#define M64_CNFG_CNTL                   0x00DC  /* RW32 — aperture state (PRG 5.2.2) */
#define M64_GUI_ENGINE_ENABLE           (1u << 8)

#define M64_CNFG_CHIP_ID                0x00E0  /* RO32 — low 16 = device ID */
#define M64_CNFG_STAT0                  0x00E4  /* RO32 — memory type strap */

/* ==========================================================================
 *  3. LCD indirect registers (accessed via LCD_INDEX/LCD_DATA at MMIO 0xA4/0xA8)
 *
 *  The v1 driver does NOT write LCD registers — the BIOS programs them
 *  to match the physical panel, and any change would risk panel damage
 *  on a fixed-frequency LVDS panel.  We only READ for snapshot and
 *  diagnostics.
 * ========================================================================== */

#define M64_LCD_INDEX                   0x00A4  /* RW8  — selects indirect register */
#define M64_LCD_DATA                    0x00A8  /* RW32 — data window */

/* LCD_INDEX is a 32-bit register; only bits 0-5 hold the index, and the
 * upper bits carry status flags (panel-on, etc.) that must be preserved
 * on every read-modify-write.  Atyfb's pattern:
 *     temp = ld_le32(LCD_INDEX)
 *     st_le32(LCD_INDEX, (temp & ~LCD_INDEX_MASK) | index)
 * Earlier MainDOB versions wrote LCD_INDEX as an 8-bit value, clobbering
 * bits 8-31 — works at boot but leaves the chip in an indeterminate
 * state after any LCD access.  v12 uses the atyfb pattern. */
#define M64_LCD_INDEX_MASK              0x0000003Fu

/* LCD indirect register indices (from Linux include/video/mach64.h).
 * Discovery: the earlier driver had M64_LCD_GRAPHIC_*_TIMING aliased to
 * 0x10/0x11, but in the standard Mach64 LCD register map those indices
 * are ICON2_CLR1 / ICON2_OFFSET (second hardware cursor) — NOT panel
 * timing.  Panel timing on the Mobility-P lives in the BIOS-programmed
 * accelerator CRTC + the HW stretcher; the LCD indirect registers
 * configure the LCD output path itself. */
#define M64_LCD_CONFIG_PANEL            0x00   /* RW — panel ID + flags */
#define M64_LCD_GEN_CNTL                0x01   /* RW — LCD_ON, CRT_ON, SHADOW_EN, ... */
#define M64_LCD_DSTN_CONTROL            0x02   /* dual-scan STN (unused on TFT) */
#define M64_LCD_HFB_PITCH_ADDR          0x03
#define M64_LCD_HORZ_STRETCHING         0x04   /* HW horizontal scaler (BIOS-set) */
#define M64_LCD_VERT_STRETCHING         0x05   /* HW vertical scaler (BIOS-set) */
#define M64_LCD_EXT_VERT_STRETCH        0x06
#define M64_LCD_LT_GIO                  0x07   /* backlight + GPIO */
#define M64_LCD_POWER_MANAGEMENT        0x08
#define M64_LCD_MISC_CNTL               0x14

/* LCD_GEN_CNTL bit layout (from Linux include/video/mach64.h, atyfb).
 *
 * IMPORTANT: v14 had SHADOW_EN/SHADOW_RW_EN at bits 6/7.  That is WRONG.
 * In the real atyfb register definition, those bits are at 30/31.
 * Bit 6 is actually DONT_SHADOW_VPAR.  When v14 thought it was
 * "clearing SHADOW_EN" to switch from shadow CRTC to live CRTC, it was
 * really clearing DONT_SHADOW_VPAR (a different feature) — so writes
 * to CRTC could still end up in the wrong bank.  v15 fixes this. */
#define M64_LCD_GEN_CNTL_CRT_ON              (1u <<  1)   /* 0x00000002 */
#define M64_LCD_GEN_CNTL_LOCK_8DOT           (1u <<  4)   /* 0x00000010 */
#define M64_LCD_GEN_CNTL_ICON_ENABLE         (1u <<  5)   /* 0x00000020 */
#define M64_LCD_GEN_CNTL_DONT_SHADOW_VPAR    (1u <<  6)   /* 0x00000040 */
#define M64_LCD_GEN_CNTL_V2CLK_PM_EN         (1u <<  7)   /* 0x00000080 */
#define M64_LCD_GEN_CNTL_DIS_HOR_CRT_DIVBY2  (1u << 10)   /* 0x00000400 */
#define M64_LCD_GEN_CNTL_HORZ_DIVBY2_EN      (1u << 14)   /* 0x00004000 */
#define M64_LCD_GEN_CNTL_TVCLK_PM_EN         (1u << 16)   /* 0x00010000 */
#define M64_LCD_GEN_CNTL_VCLK_DAC_PM_EN      (1u << 17)   /* 0x00020000 */
#define M64_LCD_GEN_CNTL_VCLK_LCD_OFF        (1u << 18)   /* 0x00040000 */
#define M64_LCD_GEN_CNTL_LVDS_EN             (1u << 21)   /* 0x00200000 */
#define M64_LCD_GEN_CNTL_LCD_ON              (1u << 22)   /* 0x00400000 — panel enable */
#define M64_LCD_GEN_CNTL_CRTC_RW_SELECT      (1u << 27)   /* 0x08000000 — 0=graphics, 1=VGA */
#define M64_LCD_GEN_CNTL_USE_SHADOWED_VEND   (1u << 28)   /* 0x10000000 */
#define M64_LCD_GEN_CNTL_USE_SHADOWED_ROWCUR (1u << 29)   /* 0x20000000 */
#define M64_LCD_GEN_CNTL_SHADOW_EN           (1u << 30)   /* 0x40000000 */
#define M64_LCD_GEN_CNTL_SHADOW_RW_EN        (1u << 31)   /* 0x80000000 */

/* CNFG_PANEL has a "magic" bit 14 that atyfb always sets when writing
 * the panel config back.  Comment in atyfb: "lcd_config_panel | 0x4000". */
#define M64_LCD_CONFIG_PANEL_MAGIC_4000      (1u << 14)

/* HORZ_STRETCHING bit layout (atyfb mach64.h).
 * Bit 31 = master STRETCH_EN, bit 30 = STRETCH_MODE (blend vs nearest).
 * Bits 15:0 = HORZ_STRETCH_RATIO (fixed-point ratio), bits 18:16 = LOOP. */
#define M64_LCD_HORZ_STRETCH_RATIO       0x0000FFFFul
#define M64_LCD_HORZ_STRETCH_BLEND       0x00000FFFul
#define M64_LCD_HORZ_STRETCH_LOOP        0x00070000ul
#define M64_LCD_HORZ_PANEL_SIZE          0x0FF00000ul   /* XC/XL only */
#define M64_LCD_AUTO_HORZ_RATIO          (1u << 29)
#define M64_LCD_HORZ_STRETCH_MODE        (1u << 30)
#define M64_LCD_HORZ_STRETCH_EN          (1u << 31)

/* VERT_STRETCHING bit layout (atyfb mach64.h). */
#define M64_LCD_VERT_STRETCH_RATIO0      0x000003FFul
#define M64_LCD_VERT_STRETCH_RATIO1      0x000FFC00ul
#define M64_LCD_VERT_STRETCH_RATIO2      0x3FF00000ul
#define M64_LCD_VERT_STRETCH_USE0        (1u << 30)
#define M64_LCD_VERT_STRETCH_EN          (1u << 31)

/* DAC control — required for true-color modes.  When the chip is in
 * 32bpp mode the DAC must be in 8-bit-per-channel (palette bypass) mode,
 * otherwise the lower 2 bits of each component are masked.  Bit 8 is
 * DAC_8BIT_EN; the BIOS sets it for any extended graphics mode but it
 * costs nothing to assert it explicitly. */
#define M64_DAC_CNTL                    0x00C4  /* RW32 */
#define M64_DAC_8BIT_EN                 (1u <<  8)

/* ==========================================================================
 *  4. PLL indirect registers (accessed via IO port BAR1 + CLOCK_CNTL_ADDR/DATA)
 *
 *  v1 SNAPSHOTS the PLL state at attach for diagnostics, but never
 *  reprograms.  The BIOS programs the PLL for the LCD panel's required
 *  pixel clock (65 MHz for 1024x768@60 on this panel); the chip
 *  stretches VGA text mode up to 1024x768 in hardware via the LCD
 *  controller, so the PLL is at 65 MHz even when DOS text mode is
 *  displayed.  Reprogramming risks losing sync with the LCD panel.
 *
 *  Reference clock (XTAL) on chip 0x4C4D is 29.498928 MHz (Mantione
 *  log, identical chip).  Different from the Mach64 LN (0x4C4E) which
 *  uses 14.31818 MHz — that distinction caught me earlier; don't
 *  conflate them.
 * ========================================================================== */

#define M64_IO_CLOCK_CNTL_ADDR          0x91   /* IO offset from io_base */
#define M64_IO_CLOCK_CNTL_DATA          0x92   /* IO offset from io_base */

/* DAC palette registers via relocatable I/O (PCI Mach64).  The DAC_REGS
 * block lives at MM offset 0x30, byte offset 0xC0.  4 sub-registers occupy
 * the 4 bytes.  Per ATI Programmer's Guide Table 5-1 + section 2.2.3.3
 * (relocatable I/O = MM<<2 + io_base). */
#define M64_IO_DAC_W_INDEX              0xC0   /* W: select entry to write */
#define M64_IO_DAC_DATA                 0xC1   /* RW: R, G, B in succession */
#define M64_IO_DAC_MASK                 0xC2   /* RW: pixel mask, set 0xFF */
#define M64_IO_DAC_R_INDEX              0xC3   /* W: select entry to read */

/* PLL register file (64 byte-addressable registers).
 *
 * IMPORTANT: v14 had PLL_EXT_CNTL=0x10, DLL_CNTL=0x12, VFC_CNTL=0x1B.
 * Those are all WRONG.  The correct atyfb values (verified against
 * Linux include/video/mach64.h master) are:
 *
 *   PLL_EXT_CNTL = 0x0B
 *   DLL_CNTL     = 0x0C
 *   VFC_CNTL     = 0x0D
 *
 * v14's value 0x10 maps to LVDS_CNTL0 (LVDS panel control) — writing
 * "xclk_post_div" data there does Bad Things to the LVDS interface,
 * which is exactly what drives the panel.  v15 corrects this. */
#define M64_PLL_REF_DIV                 0x02
#define M64_PLL_GEN_CNTL                0x03
#define M64_PLL_MCLK_FB_DIV             0x04   /* memory clock feedback divider */
#define M64_PLL_VCLK_CNTL               0x05
#define M64_PLL_VCLK_POST_DIV           0x06
#define M64_PLL_VCLK0_FB_DIV            0x07   /* +clock = +0..+3 for VCLK0..VCLK3 */
#define M64_PLL_VCLK1_FB_DIV            0x08
#define M64_PLL_VCLK2_FB_DIV            0x09
#define M64_PLL_VCLK3_FB_DIV            0x0A
#define M64_PLL_PLL_EXT_CNTL            0x0B   /* xclk_post_div, MFB times-4, etc. */
#define M64_PLL_DLL_CNTL                0x0C   /* memory DLL — XL_DLL needs 0x80 */
#define M64_PLL_VFC_CNTL                0x0D   /* memory data fetch tuning — 0x1B */
#define M64_PLL_LVDS_CNTL0              0x10
#define M64_PLL_LVDS_CNTL1              0x11
#define M64_PLL_SCLK_FB_DIV             0x15
#define M64_PLL_SPLL_CNTL2              0x17

/* PLL_GEN_CNTL bits. */
#define M64_PLL_OVERRIDE                (1u << 0)
#define M64_PLL_MCLK_RST                (1u << 1)
#define M64_PLL_OSC_EN                  (1u << 2)
#define M64_PLL_EXT_CLK_EN              (1u << 3)
#define M64_PLL_DLL_PWDN                (1u << 7)

/* PLL_VCLK_CNTL bits. */
#define M64_PLL_VCLK_SRC_SEL            0x03
#define M64_PLL_VCLK_RST                (1u << 2)

/* CLOCK_CNTL byte 0 layout. */
#define M64_CLOCK_SEL_MASK              0x03   /* selects VCLK0..VCLK3 */
#define M64_CLOCK_STROBE                0x40
#define M64_PLL_WR_EN                   0x02   /* CLOCK_CNTL_ADDR bit */

/* PLL_EXT_CNTL bits. */
#define M64_PLL_MFB_TIMES_4_2B          (1u << 3)   /* mclk_fb_mult = 4 instead of 2 */

/* CLOCK_CNTL byte 0 holds the active VCLK index (bits 0-1).
 * Offset is 0x004C in the Block-0 MMIO aliasing (dword 0x13) that this
 * driver uses (BAR0 + 0x7FFC00).  The 0x0090 value seen elsewhere is the
 * sbus/Ultra linear-aperture mapping — wrong for us.  The working CRTC
 * bring-up programs the PLL through 0x004C, so this is authoritative. */
#define M64_CLOCK_CNTL                  0x004C  /* RW8 — byte 0 of CLOCK_CNTL (Block-0) */

/* Memory controller — used to derive xclkpagefaultdelay / xclkmaxrasdelay
 * for the DSP formula.  Bit fields (from atyfb mach64_ct.c):
 *   bits  8-9   = TRP  (RAS-to-precharge)
 *   bits 10-11  = page fault delay component (×1)
 *   bit  12     = +1 page fault delay
 *   bits 16-18  = max RAS delay component
 * The exact field semantics are encoded in compute_dsp_params(). */
#define M64_MEM_CNTL                    0x00B0  /* RW32 */

/* Display FIFO / DSP — the controller of the display fetch pipeline.
 *
 * On modern Mach64 (GTB DSP and later, including Mobility) the chip does
 * NOT auto-derive when to refill its display FIFO from memory; the driver
 * computes DSP_CONFIG and DSP_ON_OFF based on pixel clock, memory clock,
 * fifo size, ram type, and bpp.  Leaving these at BIOS-set values (which
 * were calibrated for VGA 640x400x4bpp) when we switch to 1024x768x32bpp
 * causes FIFO underrun mid-line, observed as vertical noise bands —
 * cf. Risto Suominen's 2009 LKML PowerMac patch (same symptom). */
#define M64_DSP_CONFIG                  0x0020  /* RW32 */
#define M64_DSP_ON_OFF                  0x0024  /* RW32 */
#define M64_VGA_DSP_CONFIG              0x0028  /* RW32 — BIOS-set; do not touch */
#define M64_VGA_DSP_ON_OFF              0x002C  /* RW32 */

/* DSP_CONFIG bit layout (atyfb mach64.h):
 *   DSP_XCLKS_PER_QW = bits 0-13 (0x00003FFF)
 *   DSP_LOOP_LATENCY = bits 16-19 (0x000F0000)
 *   DSP_PRECISION    = bits 20-22 (0x00700000)
 *
 * DSP_ON_OFF bit layout: dsp_off in bits 0-10, dsp_on in bits 16-26.
 * v14 had DSP_OFF_MASK = 0xFFFF (16-bit) — actually 11-bit. */
#define M64_DSP_XCLKS_PER_QW            0x00003FFFu   /* bits  0-13 */
#define M64_DSP_LOOP_LATENCY            0x000F0000u   /* bits 16-19 */
#define M64_DSP_PRECISION               0x00700000u   /* bits 20-22 */

#define M64_DSP_OFF_MASK                0x000007FFu   /* bits  0-10 */
#define M64_DSP_ON_MASK                 0x07FF0000u   /* bits 16-26 */
#define M64_DSP_ON_SHIFT                16

/* CNFG_STAT0 — memory type strap, bits 0:2.  Used to compute DSP
 * loop_latency and xclkpagefaultdelay correctly for each RAM type.
 * For dynamic adaptation: read at attach, never hardcode SGRAM. */
#define M64_CFG_MEM_TYPE_xT             0x00000007u
#define M64_RAM_DRAM                    1
#define M64_RAM_EDO                     2
#define M64_RAM_PSEUDO_EDO              3
#define M64_RAM_SDRAM                   4
#define M64_RAM_SGRAM                   5
#define M64_RAM_WRAM                    6
#define M64_RAM_SDRAM32                 6

/* ==========================================================================
 *  5. 2D Engine — FIFO-based pipeline
 *
 *  FIFO depth = 16.  Programming pattern (per atyfb mach64_accel.c):
 *    1. wait_for_fifo(N) where N = number of register writes to follow
 *    2. write setup regs (OFF_PITCH, color, mix, mask, scissor)
 *    3. write trigger reg (DST_HEIGHT_WIDTH for blit/fill, DST_BRES_LNTH
 *       for line) — fires the operation
 *    4. engine works asynchronously; CPU may queue next op
 *
 *  CRITICAL idiom for FIFO occupancy (atyfb-confirmed): treat FIFO_STAT
 *  as a left-shifted bitmask.  "Slots free >= N" is tested as
 *  `(FIFO_STAT & 0xFFFF) <= (0x8000 >> N)`.  The naive `FIFO_CNT` read
 *  from GUI_STAT bits [20:16] gives the wrong polarity on some
 *  silicon — DO NOT USE.
 * ========================================================================== */

/* Drawing operation control */
#define M64_DST_OFF_PITCH               0x0100  /* RW32 — same packing as CRTC */
#define M64_DST_X                       0x0104  /* RW32 */
#define M64_DST_Y                       0x0108  /* RW32 */
#define M64_DST_Y_X                     0x010C  /* RW32 — packed Y<<16 | X */
#define M64_DST_WIDTH                   0x0110
#define M64_DST_HEIGHT                  0x0114
#define M64_DST_HEIGHT_WIDTH            0x0118  /* RW32 — packed H<<16 | W; TRIGGERS the op */
#define M64_DST_X_WIDTH                 0x011C
#define M64_DST_BRES_LNTH               0x0120  /* triggers line draw */
#define M64_DST_BRES_ERR                0x0124
#define M64_DST_BRES_INC                0x0128
#define M64_DST_BRES_DEC                0x012C
#define M64_DST_CNTL                    0x0130
#define M64_DST_X_DIR                   (1u <<  0)  /* 1 = left-to-right */
#define M64_DST_Y_DIR                   (1u <<  1)  /* 1 = top-to-bottom */
/* GUI_TRAJ_CNTL (0x0330 per official mach64.h): composite of
 * DST_CNTL/SRC_CNTL/PAT_CNTL.  (0x0148 — used before — is Z_OFF_PITCH, a 3D
 * register; writing the trajectory there did nothing.) */
#define M64_GUI_TRAJ_CNTL               0x0330
#define M64_GUI_TRAJ_DST_LTR_TTB        0x00000003u
#define M64_DST_Y_MAJOR                 (1u <<  2)  /* 1 = Y major (line) */
#define M64_DST_X_TILE                  (1u <<  3)
#define M64_DST_Y_TILE                  (1u <<  4)
#define M64_DST_LAST_PEL                (1u <<  5)
#define M64_DST_POLYGON_EN              (1u <<  6)
#define M64_DST_24_ROT_EN               (1u <<  7)

/* Source */
#define M64_SRC_OFF_PITCH               0x0180  /* RW32 */
#define M64_SRC_X                       0x0184
#define M64_SRC_Y                       0x0188
#define M64_SRC_Y_X                     0x018C  /* packed */
#define M64_SRC_WIDTH1                  0x0190
#define M64_SRC_HEIGHT1                 0x0194
#define M64_SRC_HEIGHT1_WIDTH1          0x0198
#define M64_SRC_X_START                 0x019C
#define M64_SRC_Y_START                 0x01A0
#define M64_SRC_Y_X_START               0x01A4
#define M64_SRC_WIDTH2                  0x01A8
#define M64_SRC_HEIGHT2                 0x01AC
#define M64_SRC_HEIGHT2_WIDTH2          0x01B0
#define M64_SRC_CNTL                    0x01B4

#define M64_SRC_PATT_EN                 (1u <<  0)
#define M64_SRC_PATT_ROT_EN             (1u <<  1)
#define M64_SRC_LINEAR_EN               (1u <<  2)
#define M64_SRC_BYTE_ALIGN              (1u <<  3)
#define M64_SRC_LINE_X_DIR              (1u <<  4)

/* Host data (for CPU→GPU pixel uploads via the host blit FIFO) */
#define M64_HOST_DATA0                  0x0200
#define M64_HOST_CNTL                   0x0240

/* Pattern / mask / clip */
#define M64_PAT_REG0                    0x0280
#define M64_PAT_REG1                    0x0284
#define M64_PAT_CNTL                    0x0288

#define M64_SC_LEFT                     0x02A0
#define M64_SC_RIGHT                    0x02A4
#define M64_SC_LEFT_RIGHT               0x02A8  /* packed: RIGHT<<16 | LEFT */
#define M64_SC_TOP                      0x02AC
#define M64_SC_BOTTOM                   0x02B0
#define M64_SC_TOP_BOTTOM               0x02B4  /* packed: BOTTOM<<16 | TOP */

/* Color / mix / write-mask */
#define M64_DP_BKGD_CLR                 0x02C0
#define M64_DP_FRGD_CLR                 0x02C4
#define M64_DP_WRITE_MASK               0x02C8
#define M64_DP_CHAIN_MASK               0x02CC
#define M64_DP_PIX_WIDTH                0x02D0  /* same encoding as CRTC PIX_WIDTH per channel */
#define M64_DP_MIX                      0x02D4
#define M64_DP_SRC                      0x02D8

/* DP_PIX_WIDTH packing — encodes pixel width separately for dst/src/host */
#define M64_DP_DST_PIX_WIDTH_SHIFT      0
#define M64_DP_SRC_PIX_WIDTH_SHIFT      8
#define M64_DP_HOST_PIX_WIDTH_SHIFT     16
/* Convenient packed values for 32bpp paths */
#define M64_DP_PIX_WIDTH_32BPP \
    ((uint32_t)M64_PIX_WIDTH_32BPP        \
   | ((uint32_t)M64_PIX_WIDTH_32BPP <<  8)\
   | ((uint32_t)M64_PIX_WIDTH_32BPP << 16))

/* DP_MIX: low 16 bits = BKGD mix function, high 16 = FRGD mix function.
 * Mix function codes (PRG-215R3 cap. 6 / atyfb):
 *   0x03 = leave_alone (D, destination unchanged)
 *   0x07 = paint/overpaint (S, take the source)
 * Per un fill/blit solido la doc ATI (pag 6-28) usa:
 *   DP_MIX = 0x00070003  → frgd:paint, bkgd:leave_alone.
 * bkgd:leave_alone è ANCHE una delle condizioni per abilitare il
 * block-write (fill molto più veloce), quindi è il valore giusto —
 * NON 0x00070007. */
#define M64_MIX_FN_LEAVE_ALONE          0x0003   /* D */
#define M64_MIX_FN_PAINT                0x0007   /* S */
#define M64_DP_MIX_FRGD_SHIFT           16
#define M64_DP_MIX_SOLID \
    (((uint32_t)M64_MIX_FN_PAINT << 16) | (uint32_t)M64_MIX_FN_LEAVE_ALONE)

/* DP_SRC: which source the engine pulls pixels from per channel.
 * VERIFIED against ATI RRG-S00700-05 p.3-40 and the PRG ch.6 fill sample:
 *   [2:0]  = BKGD_SRC, [10:8] = FRGD_SRC, [18:16] = MONO_SRC
 *   colour-source codes: 0=bkgd_clr, 1=frgd_clr, 2=host, 3=blit, 4=pattern
 *   MONO_SRC codes:      0=ALWAYS_ONE, 1=pattern, 2=host, 3=blit
 * The PRG solid-fill sample writes DP_SRC = 0x00000100 (mono:always_'1' = 0,
 * frgd:frgd_clr = 1<<8, bkgd:0); the blit sample writes 0x00000300
 * (frgd:blit = 3<<8).  Note MONO_SRC "always one" is the field VALUE 0, so
 * it contributes nothing to the word — earlier code set bit 0 (BKGD_SRC=1)
 * by mistake, yielding 0x101 instead of 0x100. */
#define M64_DP_SRC_BKGD_CLR             0
#define M64_DP_SRC_FRGD_CLR             1
#define M64_DP_SRC_HOST                 2
#define M64_DP_SRC_BLIT                 3
#define M64_DP_BKGD_SRC_SHIFT           0
#define M64_DP_FRGD_SRC_SHIFT           8
#define M64_DP_MONO_SRC_SHIFT           16
#define M64_DP_MONO_SRC_ALWAYS_ONE      0u   /* field value 0 = always '1' */
#define M64_DP_MONO_SRC_BLIT            3u
/* solid fill: bkgd:0, frgd:frgd_clr, mono:always_'1'(=0) -> 0x00000100 */
#define M64_DP_SRC_SOLID_FILL \
    ((uint32_t)M64_DP_SRC_FRGD_CLR << M64_DP_FRGD_SRC_SHIFT)
/* screen->screen blit: frgd:blit, mono:always_'1'(=0) -> 0x00000300 */
#define M64_DP_SRC_BLIT_FILL \
    ((uint32_t)M64_DP_SRC_BLIT << M64_DP_FRGD_SRC_SHIFT)
/* mono expansion (glyphs): frgd:frgd_clr, mono:BLIT source -> 0x00030100 */
#define M64_DP_SRC_MONO_BLIT \
    (((uint32_t)M64_DP_SRC_FRGD_CLR << M64_DP_FRGD_SRC_SHIFT) \
   | ((uint32_t)M64_DP_MONO_SRC_BLIT << M64_DP_MONO_SRC_SHIFT))

/* DP_CHAIN_MASK per 32bpp (PRG tab 5-3). */
#define M64_DP_CHAIN_MASK_32BPP         0x8080u

/* Engine status */
#define M64_FIFO_STAT                   0x0310  /* RO — bitmask of occupied slots (dword 0xC4) */
#define M64_GUI_STAT                    0x0338  /* RO — bit 0 = ENGINE_BUSY (dword 0xCE) */
#define M64_GUI_STAT_ENGINE_BUSY        (1u << 0)

/* Context control + color-compare (offsets = xorg atiregs.h BlockIOTag×4,
 * cross-checked: FIFO_STAT 0xC4→0x310 and GUI_STAT 0xCE→0x338 match above). */
#define M64_CONTEXT_MASK                0x0320  /* dword 0xC8 */
#define M64_CLR_CMP_CLR                 0x0300  /* dword 0xC0 */
#define M64_CLR_CMP_MASK                0x0304  /* dword 0xC1 */
#define M64_CLR_CMP_CNTL                0x0308  /* dword 0xC2 */

/* ==========================================================================
 *  6. PCI device IDs we recognise
 * ========================================================================== */

#define M64_PCI_VENDOR_ATI              0x1002
#define M64_PCI_DEV_MOBILITY_P_AGP      0x4C4D   /* "LM" */
#define M64_PCI_DEV_LT_PRO              0x4C42   /* "LB" — not on E500 but kept for symmetry */

/* ==========================================================================
 *  7. Chip-specific constants (from Mantione log, chip 0x4C4D + CLAA141XB01)
 * ========================================================================== */

#define M64_LM_XTAL_KHZ                 29499   /* 29.498928 MHz reference clock */
#define M64_LM_PLL_VCO_MAX_KHZ          230000  /* 230 MHz */
#define M64_LM_MCLK_KHZ                 83000   /* 83 MHz memory clock target */
#define M64_LM_XCLK_KHZ                 125000  /* 125 MHz chip clock target */
#define M64_LM_VRAM_BYTES               (8u * 1024u * 1024u)

/* Native panel timing for CPT CLAA141XB01 — exact values from the
 * atyfb 2.4.23 log on the identical chip+panel combination.  These are
 * what the BIOS already programmed into the LCD controller; we
 * program the CRTC to match so the LCD controller's expected timings
 * arrive on time. */
#define M64_PANEL_W                     1024
#define M64_PANEL_H                     768
#define M64_PANEL_REFRESH_HZ            60
#define M64_PANEL_PIXCLOCK_PS           15384   /* 1/65 MHz */

/* CRTC register values, derived once from XGA timings and the Mantione
 * log:
 *
 *   pixclock = 15384 ps  → 65.0 MHz
 *   h_total = 1344 → encoded (1344/8)-1 = 167
 *   h_disp  = 1024 → encoded (1024/8)-1 = 127
 *   h_sync_strt = 1048 → encoded (1048/8)-1 = 130
 *   h_sync_dly  = 0
 *   h_sync_wid  = 136 px → 17 chars
 *   v_total = 806 → encoded 806-1 = 805
 *   v_disp  = 768 → encoded 768-1 = 767
 *   v_sync_strt = 770 → encoded 770-1 = 769
 *   v_sync_wid  = 6 lines
 *
 * Polarity for XGA-compatible 1024x768@60 is NHSYNC NVSYNC (both
 * negative).  In Mach64 CRTC_*_SYNC_STRT_WID the polarity bit is
 * bit 21 (atyfb mach64.h: CRTC_H_SYNC_NEG / CRTC_V_SYNC_NEG). */
#define M64_PANEL_CRTC_H_TOTAL_DISP     ((127u << 16) | 167u)
#define M64_PANEL_CRTC_H_SYNC_STRT_WID  ((1u << 21) | (17u << 16) | 130u)
#define M64_PANEL_CRTC_V_TOTAL_DISP     ((767u << 16) | 805u)
#define M64_PANEL_CRTC_V_SYNC_STRT_WID  ((1u << 21) | (6u << 16) | 769u)

#endif /* MAINDOB_DRIVERS_MACH64_HW_H */
