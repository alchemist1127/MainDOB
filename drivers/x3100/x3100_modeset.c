/* x3100_modeset.c — X3100 (Intel GM965, gen4) display bring-up + scanout.
 *
 * Replaces the Mach64 driver's modeset / cursor / irq modules.  There is no
 * real modeset: the BIOS has the LVDS panel running at its 1280x800 native
 * raster, and we reuse it — we only point display plane B at our framebuffer
 * (the "easy-path scanout swap" validated on the real Acer panel in Fase 2).
 *
 * VRAM is UMA (no dedicated card memory): the "VRAM pool" is dma_alloc'd
 * system RAM whose pages are written into consecutive GTT PTEs so the GRAPHICS
 * address range is contiguous even when the physical pages are not (this also
 * lets us assemble a large pool from smaller physically-contiguous chunks).
 * The CPU reaches the pool through a GMADR aperture window (g_x3100.vram); the
 * 2D engine reaches it through GTT graphics addresses.  Reboot reverts every
 * register we touch.
 */

#include <string.h>
#include <stdio.h>
#include <unistd.h>            /* mmap_phys, dma_alloc (static inline)        */

#include <dob/types.h>
#include <dob/hotplug_driver.h>
#include <dob/video.h>

#include "x3100_state.h"
#include "x3100_hw.h"
#include "x3100_2d.h"

extern void debug_print(const char *s);

/* Native panel mode — BIOS-programmed; we never reprogram the timings. */
#define X3100_NATIVE_W       1280u
#define X3100_NATIVE_H       800u
#define X3100_NATIVE_HZ      60u

/* VRAM pool: 16 MB of UMA RAM.  16 MB is the mmap_phys window cap, so the
 * whole pool maps in ONE GMADR window (CPU can reach every surface).  Built
 * from 4 MB physically-contiguous chunks mapped into consecutive GTT pages.
 * front + back page (4 MB each at 1280x800x4) leave ~8 MB for surfaces, the
 * glyph atlas, layer-cache and the dirty-rect scratch buffer. */
#define X3100_POOL_BYTES        (16u * 1024u * 1024u)
#define X3100_POOL_CHUNK_BYTES  (4u  * 1024u * 1024u)

/* GTT layout (these were local to the Fase-1/2 probe; the production driver
 * owns them here).  The pool starts at a high GTT index, clear of the BIOS's
 * low stolen/VGA range; the ring is placed dynamically right after the pool. */
#define X3100_VRAM_GTT_INDEX    32768u          /* graphics base 0x08000000 */
#define X3100_RING_SIZE_BYTES   (64u * 1024u)   /* 16-page render ring      */

/* Display scan-line register field width (current line, RO). */
#define X3100_DSL_LINE_MASK     0x00001FFFu

/* Hardware cursor — sprite B / pipe B.  An i965 ARGB 64x64 overlay the CRTC
 * composites at scanout, fully independent of the framebuffer compose: moving
 * the pointer is a single CURBPOS write with NO recompose/flip.  Registers per
 * i965 PRM Vol.3 sec 2.10.3 (CURBCNTR 0x700C0 / CURBBASE 0x700C4 / CURBPOS
 * 0x700C8); mode/pos encodings per the i915 gen4 cursor path. */
#define X3100_CURBCNTR          0x700C0u
#define X3100_CURBBASE          0x700C4u
#define X3100_CURBPOS           0x700C8u
#define X3100_CUR_MODE_64_ARGB  0x27u            /* 0x20 (ARGB) | 0x07 (64x64 32bpp) */
#define X3100_CUR_PIPE_B        (1u << 28)       /* MCURSOR_PIPE_SELECT -> pipe B     */
#define X3100_CUR_CNTR_ON       (X3100_CUR_MODE_64_ARGB | X3100_CUR_PIPE_B)
#define X3100_CUR_CNTR_OFF      0x00000000u
#define X3100_CUR_POS_X_SIGN    (1u << 15)
#define X3100_CUR_POS_Y_SIGN    (1u << 31)
#define X3100_CUR_POS_MASK      0x00007FFFu      /* 15-bit magnitude: X[14:0], Y[30:16] */
#define X3100_CUR_DIM           64u              /* fixed 64x64 ARGB sprite */
#define X3100_CUR_BYTES         (X3100_CUR_DIM * X3100_CUR_DIM * 4u)   /* 16384 */

/* ==========================================================================
 *  Hardware init: map BARs, build the GTT-backed VRAM pool, bring up the ring
 * ========================================================================== */

/* Cursor A register block (pipe A twin of the CURB* set at the top). */
#define X3100_CURACNTR          0x70080u
#define X3100_CURABASE          0x70084u
#define X3100_CURAPOS           0x70088u

/* GM45/CQ62 fallback panel mode if PIPExSRC reads garbage. */
#define X3100_G4X_FALLBACK_W    1366u
#define X3100_G4X_FALLBACK_H    768u

/* Size a memory BAR by the standard write-ones probe.  Only the CPU window
 * is affected for the microseconds the BAR holds 0xFFFFFFF0 — the CRTC
 * fetches scanout through the internal GTT path, not through this BAR —
 * and hw_init is single-threaded with the pool not yet mapped, so nothing
 * else can touch the aperture mid-probe. Returns 0 if unsized. */
static uint32_t pci_bar_size(uint8_t bus, uint8_t slot, uint8_t func,
                             uint8_t bar_off)
{
    uint32_t saved = pci_config_read(bus, slot, func, bar_off);
    uint32_t mask;
    pci_config_write(bus, slot, func, bar_off, 0xFFFFFFFFu);
    mask = pci_config_read(bus, slot, func, bar_off);
    pci_config_write(bus, slot, func, bar_off, saved);
    mask &= 0xFFFFFFF0u;
    if (mask == 0u) return 0u;
    return (~mask) + 1u;
}

bool x3100_hw_init(void)
{
    uint32_t bar0 = g_x3100.dev.bar[0] & 0xFFFFF000u;   /* GTTMMADR            */
    uint32_t bar2 = g_x3100.dev.bar[2] & 0xFFFFF000u;   /* GMADR aperture base */
    uint32_t gtt_phys;
    uint32_t pool_pages, ring_index, ring_pages, gtt_map_bytes;

    /* Chipset generation from the PCI device id.  GM965/Crestline (2A02/
     * 2A03) keeps every historical constant; GM45/Cantiga (2A42/2A43) is
     * the same g4x display family with two hard deltas handled below:
     * the GTT lives at BAR0+2MB (i915: i965 @512K, g4x @2M) and the LVDS
     * pipe / native mode must be read from the hardware, not assumed. */
    g_x3100.is_g4x = (g_x3100.dev.device_id == 0x2A42u ||
                      g_x3100.dev.device_id == 0x2A43u);
    gtt_phys = bar0 + (g_x3100.is_g4x ? 0x00200000u : 0x00080000u);

    if (!bar0 || !bar2) {
        debug_print("[x3100] FATAL: BAR0/BAR2 is zero.\n");
        return false;
    }

    /* MMIO — byte addressable (32-bit regs via cast in x3100_mmio_r32/w32). */
    g_x3100.mmio = (volatile uint8_t *)mmap_phys(bar0, 1u * 1024u * 1024u);
    if (!g_x3100.mmio) {
        debug_print("[x3100] FATAL: MMIO (BAR0) map failed.\n");
        return false;
    }
    g_x3100.mmio_phys = bar0;

    /* --- Panel pipe + native mode ------------------------------------
     * GM965 (Extensa 5220): pipe B and 1280x800, both VALIDATED on the
     * real panel — never re-derived, so that machine's behaviour is
     * byte-identical to before this block existed.
     * GM45: the BIOS left the panel running; read which pipe the LVDS
     * port selects (LVDS[30]) and that pipe's PIPESRC for the raster.
     * Sanity-clamp the readback: on garbage fall back to the CQ62's
     * 1366x768 (and pipe A, the common Cantiga wiring). */
    {
        uint32_t pipe_b  = 1u;                       /* GM965 default   */
        uint32_t nw      = X3100_NATIVE_W;
        uint32_t nh      = X3100_NATIVE_H;

        if (g_x3100.is_g4x)
        {
            uint32_t lv  = x3100_mmio_r32(X3100_LVDS);
            pipe_b = (lv & X3100_LVDS_PIPEB_SELECT) ? 1u : 0u;
            {
                /* Panel raster from the PIPE TIMINGS, not from PIPESRC.
                 * PIPESRC is the SOURCE size, and when the firmware left a
                 * scaled mode running (GRUB's VGA text: 720x400 stretched
                 * to the panel by the pfit) it reads the VGA raster —
                 * which passes any sanity check and gave the CQ62 a
                 * quarter-screen desktop once our modeset disabled the
                 * pfit.  HTOTAL/VTOTAL low halves carry (active-1) of the
                 * raster the panel actually scans, pfit or not. */
                uint32_t pbase = pipe_b ? X3100_PIPE_B_BASE
                                        : X3100_PIPE_A_BASE;
                uint32_t ht = x3100_mmio_r32(pbase + X3100_HTOTAL);
                uint32_t vt = x3100_mmio_r32(pbase + X3100_VTOTAL);
                uint32_t w  = (ht & 0xFFFFu) + 1u;
                uint32_t h  = (vt & 0xFFFFu) + 1u;
                if (w >= 320u && w <= 2048u && h >= 200u && h <= 1536u) {
                    nw = w; nh = h;
                } else {
                    nw = X3100_G4X_FALLBACK_W;
                    nh = X3100_G4X_FALLBACK_H;
                    debug_print("[x3100] g4x: pipe timings garbage; "
                                "fallback 1366x768.\n");
                }
            }
            {
                char m[96];
                sprintf(m, "[x3100] g4x (GM45): LVDS on pipe %c, native "
                           "%ux%u.\n", pipe_b ? 'B' : 'A',
                           (unsigned)nw, (unsigned)nh);
                debug_print(m);
            }
        }

        g_x3100.native_w = nw;
        g_x3100.native_h = nh;
        /* Gen4 linear display stride must be a 64-byte multiple.  1280*4
         * already is (Extensa unchanged); 1366*4=5464 is NOT -> pad the
         * scanout pitch to 1376 px.  The pad column is never composed nor
         * shown: the plane scans width px per line and skips the rest. */
        g_x3100.scan_pitch_px = ((nw * 4u + 63u) & ~63u) / 4u;

        if (pipe_b) {
            g_x3100.reg_dsp_cntr   = X3100_DSPB_CNTR;
            g_x3100.reg_dsp_linoff = X3100_DSPB_LINOFF;
            g_x3100.reg_dsp_stride = X3100_DSPB_STRIDE;
            g_x3100.reg_dsp_surf   = X3100_DSPB_SURF;
            g_x3100.reg_pipe_src   = X3100_PIPE_B_BASE + X3100_PIPESRC;
            g_x3100.reg_pipe_dsl   = X3100_PIPEB_DSL;
            g_x3100.reg_cur_cntr   = X3100_CURBCNTR;
            g_x3100.reg_cur_base   = X3100_CURBBASE;
            g_x3100.reg_cur_pos    = X3100_CURBPOS;
            g_x3100.dsp_cntr_enable = X3100_DISP_ENABLE
                                    | X3100_DISP_FORMAT_BGRX888
                                    | X3100_DISP_PIPE_SEL_B;
            g_x3100.cur_cntr_on     = X3100_CUR_MODE_64_ARGB | X3100_CUR_PIPE_B;
        } else {
            g_x3100.reg_dsp_cntr   = X3100_DSPA_CNTR;
            g_x3100.reg_dsp_linoff = X3100_DSPA_LINOFF;
            g_x3100.reg_dsp_stride = X3100_DSPA_STRIDE;
            g_x3100.reg_dsp_surf   = X3100_DSPA_SURF;
            g_x3100.reg_pipe_src   = X3100_PIPE_A_BASE + X3100_PIPESRC;
            g_x3100.reg_pipe_dsl   = X3100_PIPEA_DSL;
            g_x3100.reg_cur_cntr   = X3100_CURACNTR;
            g_x3100.reg_cur_base   = X3100_CURABASE;
            g_x3100.reg_cur_pos    = X3100_CURAPOS;
            g_x3100.dsp_cntr_enable = X3100_DISP_ENABLE
                                    | X3100_DISP_FORMAT_BGRX888;
            g_x3100.cur_cntr_on     = X3100_CUR_MODE_64_ARGB;   /* pipe A = 0 */
        }
    }

    /* Native mode (sizes the primary below). */
    g_x3100.mode.width      = g_x3100.native_w;
    g_x3100.mode.height     = g_x3100.native_h;
    g_x3100.mode.refresh_hz = X3100_NATIVE_HZ;
    g_x3100.mode.format     = DV_FMT_BGRA8888;
    g_x3100.mode.flags      = 0u;

    /* Pool graphics base.  GM965 keeps the validated constant (128 MB,
     * inside its 256 MB aperture).  GM45 apertures vary (128/256/512 MB):
     * size BAR2 and place the pool at half the aperture — above any BIOS
     * stolen range at GTT start, comfortably inside the CPU window. */
    g_x3100.vram_gtt_index = X3100_VRAM_GTT_INDEX;
    if (g_x3100.is_g4x)
    {
        uint32_t ap = pci_bar_size(g_x3100.dev.bus, g_x3100.dev.slot,
                                   g_x3100.dev.func, 0x18);
        if (ap >= 64u * 1024u * 1024u &&
            (ap / 2u) / 4096u != X3100_VRAM_GTT_INDEX)
        {
            g_x3100.vram_gtt_index = (ap / 2u) / 4096u;
        }
        {
            char m[96];
            sprintf(m, "[x3100] g4x: GMADR %u MB, pool @ graphics 0x%08x.\n",
                    (unsigned)(ap >> 20),
                    (unsigned)(g_x3100.vram_gtt_index * 4096u));
            debug_print(m);
        }
    }
    pool_pages   = X3100_POOL_BYTES / 4096u;
    ring_index   = g_x3100.vram_gtt_index + pool_pages;          /* after pool */
    ring_pages   = (X3100_RING_SIZE_BYTES + 4095u) / 4096u;
    gtt_map_bytes = (ring_index + ring_pages + 16u) * 4u;

    /* Map the GTT entry array (covers pool + ring index ranges). */
    g_x3100.gtt = (volatile uint32_t *)mmap_phys(gtt_phys, gtt_map_bytes);
    if (!g_x3100.gtt) {
        debug_print("[x3100] FATAL: GTT map failed.\n");
        return false;
    }
    g_x3100.gtt_entries = 131072u;

    /* Allocate the pool in chunks and map each chunk's pages into consecutive
     * GTT PTEs from vram_gtt_index (graphics-address-contiguous). */
    {
        uint32_t mapped = 0u;
        while (mapped < X3100_POOL_BYTES) {
            uint32_t want = X3100_POOL_BYTES - mapped;
            uint32_t phys = 0u, base_page, n, i;
            void *v;
            if (want > X3100_POOL_CHUNK_BYTES) want = X3100_POOL_CHUNK_BYTES;
            v = dma_alloc(want, &phys);
            if (!v || !phys) {
                debug_print("[x3100] FATAL: pool dma_alloc failed.\n");
                return false;
            }
            if (mapped == 0u) g_x3100.vram_phys = phys;   /* first chunk (info) */
            base_page = g_x3100.vram_gtt_index + (mapped / 4096u);
            n = want / 4096u;
            for (i = 0; i < n; i++)
                g_x3100.gtt[base_page + i] =
                    ((phys & X3100_GTT_PTE_ADDR_MASK) + i * 4096u) | X3100_GTT_PTE_VALID;
            mapped += want;
        }
    }
    g_x3100.vram_bytes = X3100_POOL_BYTES;

    /* CPU window into the (contiguous) pool graphics range, via the GMADR
     * aperture — the CPU->scanned-pages path proven during EBU bring-up. */
    g_x3100.gmadr_phys  = bar2;
    g_x3100.gmadr_bytes = X3100_POOL_BYTES;
    g_x3100.vram = (volatile bgra_t *)mmap_phys(bar2 + g_x3100.vram_gtt_index * 4096u,
                                                X3100_POOL_BYTES);
    if (!g_x3100.vram) {
        debug_print("[x3100] FATAL: GMADR window map failed.\n");
        return false;
    }
    g_x3100.gmadr = (volatile uint8_t *)g_x3100.vram;

    /* Render ring: its own pages.  The CPU writes commands via the dma virtual
     * address; the engine reads via ring_gtt (this split is proven on the
     * panel in Fase 3a). */
    {
        uint32_t ring_phys = 0u, i;
        void *ring_v = dma_alloc(X3100_RING_SIZE_BYTES, &ring_phys);
        if (!ring_v || !ring_phys) {
            debug_print("[x3100] FATAL: ring dma_alloc failed.\n");
            return false;
        }
        for (i = 0; i < ring_pages; i++)
            g_x3100.gtt[ring_index + i] =
                ((ring_phys & X3100_GTT_PTE_ADDR_MASK) + i * 4096u) | X3100_GTT_PTE_VALID;
        g_x3100.ring       = (volatile uint32_t *)ring_v;
        g_x3100.ring_gtt   = ring_index * 4096u;
        g_x3100.ring_bytes = X3100_RING_SIZE_BYTES;
        g_x3100.ring_tail  = 0u;
        for (i = 0; i < X3100_RING_SIZE_BYTES / 4u; i++) g_x3100.ring[i] = 0u;
    }

    /* PTE posting read + fence so the engine/display see our PTEs. */
    (void)g_x3100.gtt[g_x3100.vram_gtt_index];
    (void)g_x3100.gtt[ring_index];
    __asm__ volatile ("lfence" ::: "memory");

    debug_print("[x3100] hw_init: MMIO/GTT/GMADR/pool(16M)/ring up.\n");
    return true;
}

void x3100_hw_shutdown(void)
{
    /* Best effort: drop the engine flag and blank plane B so a detach doesn't
     * leave the panel scanning freed pages. */
    g_x3100.engine_ok = false;
    if (g_x3100.mmio)
        x3100_mmio_w32(g_x3100.reg_dsp_cntr,
                       x3100_mmio_r32(g_x3100.reg_dsp_cntr) & ~X3100_DISP_ENABLE);
}

/* ==========================================================================
 *  Scanout (easy-path) + mode
 * ========================================================================== */

/* Arm plane B onto the current front page — the four validated Fase-2 writes
 * (stride, plane control, surface base, pipe source) plus PFIT off and the
 * legacy VGA plane off.  Reboot reverts them. */
int x3100_modeset(void)
{
    x3100_mmio_w32(g_x3100.reg_dsp_stride, g_x3100.scan_pitch_px * 4u);
    x3100_mmio_w32(g_x3100.reg_dsp_linoff, 0u);
    x3100_mmio_w32(g_x3100.reg_dsp_cntr,   g_x3100.dsp_cntr_enable);
    x3100_mmio_w32(g_x3100.reg_dsp_surf,   x3100_gaddr(g_x3100.front_offset));
    (void)x3100_mmio_r32(g_x3100.reg_dsp_surf);
    x3100_mmio_w32(g_x3100.reg_pipe_src,
                   ((g_x3100.mode.width - 1u) << 16) | (g_x3100.mode.height - 1u));
    x3100_mmio_w32(X3100_PFIT_CONTROL,
                   x3100_mmio_r32(X3100_PFIT_CONTROL) & ~X3100_PFIT_ENABLE);
    x3100_mmio_w32(X3100_VGACNTRL,
                   x3100_mmio_r32(X3100_VGACNTRL) | X3100_VGA_DISP_DISABLE);
    (void)x3100_mmio_r32(X3100_VGACNTRL);
    /* Park the hardware cursor disabled (the BIOS may have left sprite B on
     * with a stale base, which would scan out as garbage).  dobinterface
     * re-enables it via dv_cursor_show after uploading the sprite bitmap. */
    x3100_mmio_w32(g_x3100.reg_cur_cntr, X3100_CUR_CNTR_OFF);
    x3100_mmio_w32(g_x3100.reg_cur_base, 0u);
    __asm__ volatile ("" ::: "memory");
    return 0;
}

/* dobinterface's scanout-source hint.  We always scan from the front page
 * (set by modeset / page_flip), so this is informational. */
int32_t x3100_internal_scanout_set(dv_surface_t s)
{
    g_x3100.scanout_source = s;
    return DV_OK;
}

/* Only the native mode is supported. */
int32_t x3100_internal_mode_set(const dv_mode_t *m)
{
    if (!m) return DV_ERR_INVAL;
    if (m->width != g_x3100.native_w || m->height != g_x3100.native_h)
        return DV_ERR_NOSUPPORT;
    g_x3100.mode = *m;
    x3100_modeset();
    return DV_OK;
}

void x3100_recompute_mode_list(void)
{
    g_x3100.mode_list_buf[0].width      = g_x3100.native_w;
    g_x3100.mode_list_buf[0].height     = g_x3100.native_h;
    g_x3100.mode_list_buf[0].refresh_hz = X3100_NATIVE_HZ;
    g_x3100.mode_list_buf[0].format     = DV_FMT_BGRA8888;
    g_x3100.mode_list_buf[0].flags      = 0u;
    g_x3100.mode_list_n = 1u;
}

/* ==========================================================================
 *  Vsync — polled on pipe B's scan-line counter (no IRQ in v1)
 * ========================================================================== */

int32_t x3100_vsync_wait(uint32_t timeout_ms)
{
    /* Leave any current vblank, then wait for the next vblank to start.
     * Bounded so a stuck pipe can't hang us.
     * Sleep-until-vblank (same CPU fix as x3100_wait_for_vblank in
     * main.c): de-schedule for slightly less than the time to the next
     * vblank, then finish with the short precise spin. */
    uint32_t budget = (timeout_ms ? timeout_ms : 100u) * 200000u;
    uint32_t h = g_x3100.mode.height;

    {
        uint32_t dsl = x3100_mmio_r32(g_x3100.reg_pipe_dsl) & X3100_DSL_LINE_MASK;
        if (dsl < h)
        {
            uint32_t vtotal_est = h + h / 8u;           /* ~12.5% blanking */
            uint32_t ms = (h - dsl) * 16u / vtotal_est;  /* < time to vblank */
            if (ms > 2u) sleep_ms(ms - 1u);
        }
    }

    while ((x3100_mmio_r32(g_x3100.reg_pipe_dsl) & X3100_DSL_LINE_MASK) >= h && --budget) { }
    while ((x3100_mmio_r32(g_x3100.reg_pipe_dsl) & X3100_DSL_LINE_MASK) <  h && --budget) { }

    g_x3100.vsync_count++;
    return DV_OK;
}

/* ==========================================================================
 *  Hardware cursor — i965 ARGB sprite B on pipe B (register block at top).
 *
 *  The CRTC overlays the 64x64 ARGB sprite at scanout, so pointer motion is
 *  one CURBPOS write and NEVER triggers a framebuffer recompose/flip — which
 *  is what flooded the panel with artifacts on the software-cursor path (every
 *  mouse-move forced a full dv_compose + page-flip).  The sprite lives in a
 *  16 KB page-aligned pool block: the CPU uploads it through the GMADR
 *  aperture (g_x3100.vram), the CRTC reads it via its graphics address
 *  (CURBBASE = x3100_gaddr(offset)).  Stays double-buffer-friendly: the sprite
 *  is an independent scanout overlay, orthogonal to the content pages.
 * ========================================================================== */

int32_t x3100_cursor_set_bitmap(const dv_cursor_desc_t *d)
{
    if (!d || !d->pixels || d->width == 0u || d->height == 0u) return DV_ERR_INVAL;
    if (d->width > X3100_CUR_DIM || d->height > X3100_CUR_DIM)  return DV_ERR_NOSUPPORT;

    /* Allocate the fixed 64x64 ARGB sprite once, page-aligned so CURBBASE is a
     * clean graphics page address.  Offset 0 is the primary front page, so a
     * zero cursor offset reliably means "not yet allocated". */
    if (g_x3100.cursor_vram_offset == 0u)
    {
        vram_block_t *cb = vram_alloc(X3100_CUR_BYTES, 4096u);
        if (!cb) return DV_ERR_OOM_VRAM;
        g_x3100.cursor_vram_offset = cb->offset;
    }

    /* Intel ARGB cursor memory is little-endian [B,G,R,A] == BGRA8888, so the
     * incoming bitmap copies verbatim.  Clear the sprite fully transparent,
     * then drop the bitmap into the top-left. */
    {
        volatile uint32_t *spr = (volatile uint32_t *)(g_x3100.vram)
                               + (g_x3100.cursor_vram_offset >> 2);
        const uint32_t    *src = (const uint32_t *)d->pixels;
        uint32_t i, row, col;
        for (i = 0; i < X3100_CUR_DIM * X3100_CUR_DIM; i++) spr[i] = 0u;
        for (row = 0; row < d->height; row++)
            for (col = 0; col < d->width; col++)
                spr[row * X3100_CUR_DIM + col] = src[row * d->width + col];
    }

    g_x3100.cursor_hotspot_x = d->hotspot_x;
    g_x3100.cursor_hotspot_y = d->hotspot_y;

    /* (Re)point the sprite base — latches the new pixels at the next vblank —
     * and keep it shown if it already was (shape change while visible). */
    x3100_mmio_w32(g_x3100.reg_cur_base, x3100_gaddr(g_x3100.cursor_vram_offset));
    if (g_x3100.cursor_visible)
        x3100_mmio_w32(g_x3100.reg_cur_cntr, g_x3100.cur_cntr_on);
    return DV_OK;
}

int32_t x3100_cursor_set_position(int32_t x, int32_t y)
{
    /* Apply the hotspot, then encode magnitude + sign.  The sign bits let the
     * sprite slide partly off the top/left edge instead of wrapping. */
    int32_t  cx = x - (int32_t)g_x3100.cursor_hotspot_x;
    int32_t  cy = y - (int32_t)g_x3100.cursor_hotspot_y;
    uint32_t pos = 0u;
    if (cx < 0) pos |= X3100_CUR_POS_X_SIGN |  ((uint32_t)(-cx) & X3100_CUR_POS_MASK);
    else        pos |=                          ((uint32_t)cx  & X3100_CUR_POS_MASK);
    if (cy < 0) pos |= X3100_CUR_POS_Y_SIGN | (((uint32_t)(-cy) & X3100_CUR_POS_MASK) << 16);
    else        pos |=                        (((uint32_t)cy  & X3100_CUR_POS_MASK) << 16);
    x3100_mmio_w32(g_x3100.reg_cur_pos, pos);
    g_x3100.cursor_x = x;
    g_x3100.cursor_y = y;
    return DV_OK;
}

int32_t x3100_cursor_show(void)
{
    if (g_x3100.cursor_vram_offset == 0u) return DV_ERR_NOTREADY;   /* no bitmap yet */
    x3100_mmio_w32(g_x3100.reg_cur_cntr, g_x3100.cur_cntr_on);
    x3100_mmio_w32(g_x3100.reg_cur_base, x3100_gaddr(g_x3100.cursor_vram_offset));
    g_x3100.cursor_visible = true;
    return DV_OK;
}

int32_t x3100_cursor_hide(void)
{
    x3100_mmio_w32(g_x3100.reg_cur_cntr, X3100_CUR_CNTR_OFF);
    x3100_mmio_w32(g_x3100.reg_cur_base, 0u);
    g_x3100.cursor_visible = false;
    return DV_OK;
}

/* ==========================================================================
 *  Unsupported in v1 — palette/gamma and IRQ.  These report cleanly so the
 *  data plane links.
 * ========================================================================== */

int32_t x3100_palette_set(const uint32_t *palette_argb, uint32_t count)
{
    (void)palette_argb; (void)count;
    return DV_ERR_NOSUPPORT;
}
int32_t x3100_gamma_get(dv_gamma_ramp_t *out) { (void)out; return DV_ERR_NOSUPPORT; }

int  x3100_irq_init(uint32_t my_port)        { (void)my_port; return 0; }   /* no IRQ */
void x3100_irq_dispatch(uint32_t notification){ (void)notification; }

/* Declared by the mach64-derived state header; the easy-path bring-up lives in
 * x3100_hw_init(), so these are thin shims. */
int  x3100_modeset_init_hw(hotplug_device_t *dev) { (void)dev; return 0; }
void x3100_modeset_shutdown(void)                 { x3100_hw_shutdown(); }
