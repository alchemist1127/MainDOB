/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * mach64_cursor.c — hardware cursor for the ATI Mach64 (Rage Mobility).
 *
 * The CRTC overlays a 64x64 2-bit sprite at scanout, so moving the cursor
 * is just a register write — no recomposition of the framebuffer.  This is
 * what lets dobinterface move the pointer without calling dv_compose (which
 * on a single-buffer path would briefly show the half-drawn frame).
 *
 * Cursor pixel format (PRG 6.4.3): 2 bits/pixel, Intel order (first pixel in
 * the low 2 bits of the first byte), pitch ALWAYS 64px = 16 bytes/line, 64
 * lines => 1024 bytes.  Pixel values: 00=CUR_CLR0, 01=CUR_CLR1,
 * 10=transparent, 11=complement.  We map an incoming RGBA cursor to a simple
 * 2-colour + transparency sprite: opaque pixels -> colour1 (foreground),
 * everything else -> transparent.  (A 1-bit shape is exactly what UI pointers
 * need; this also sidesteps the Rage's alpha-texture defect.)
 */
#include <dob/video.h>
#include <string.h>

#include "mach64_state.h"
#include "mach64_hw.h"

#define CUR_DIM        64u           /* hardware sprite is fixed 64x64        */
#define CUR_PITCH_B    16u           /* 64 px * 2 bits = 16 bytes per line     */
#define CUR_IMG_BYTES  (CUR_PITCH_B * CUR_DIM)   /* 1024 bytes               */

/* Program the two cursor colour registers. The Mach64 sprite is a genuine
 * 2-colour-plus-transparency overlay (codes per pixel: 00=CLR0, 01=CLR1,
 * 10=transparent, 11=complement), so a black-border/white-body pointer maps
 * exactly: CLR0 = border colour, CLR1 = body colour.
 *
 * Byte layout: the CRTC reads the colour from bits [31:8] of the register
 * (0xRRGGBBxx), NOT the low 24 bits. Passing 0x00RRGGBB put 0x00 in the red
 * byte the chip actually samples, so the white body (0x00FFFFFF) came out
 * with R=0 = CYAN. We take a normal 0x00RRGGBB argument and shift it up 8
 * bits into the field the hardware reads. (Black is all-zero either way, so
 * the border was unaffected — which is why only the body looked wrong.) */
static void cursor_program_colors(uint32_t clr0_rgb, uint32_t clr1_rgb)
{
    mach64_mmio_w32(M64_CUR_CLR0, (clr0_rgb & 0x00FFFFFFu) << 8);
    mach64_mmio_w32(M64_CUR_CLR1, (clr1_rgb & 0x00FFFFFFu) << 8);
}

int32_t mach64_cursor_set_bitmap(const dv_cursor_desc_t *d)
{
    if (!d || !d->pixels) return DV_ERR_INVAL;
    if (d->width == 0u || d->height == 0u) return DV_ERR_INVAL;
    if (d->width > CUR_DIM || d->height > CUR_DIM) return DV_ERR_INVAL;

    /* Allocate the 1KB cursor image in VRAM once, lazily. */
    if (g_mach64.cursor_vram_offset == 0u)
    {
        vram_block_t *b = vram_alloc(CUR_IMG_BYTES, 256u); /* CUR_OFFSET is in qwords */
        if (!b) return DV_ERR_NOMEM;
        g_mach64.cursor_vram_offset = b->offset;
    }

    volatile uint8_t *img = (volatile uint8_t *)g_mach64.vram
                          + g_mach64.cursor_vram_offset;

    /* Start fully transparent (all pixels = 10b => 0xAA per byte). */
    for (uint32_t i = 0; i < CUR_IMG_BYTES; i++) img[i] = 0xAAu;

    /* Two-colour conversion. The compositor hands us a fully-rendered BGRA
     * pointer (transparent / opaque-black border / opaque-white body), so we
     * key each opaque pixel by brightness rather than collapsing everything
     * to one colour (the old code took CLR1 from the FIRST opaque pixel —
     * the arrow's black border — and drew the whole sprite black):
     *   transparent (a<128)        -> 10b  (leave as initialised)
     *   opaque, dark  (luma < 128) -> 00b  CLR0  (border)
     *   opaque, light (luma >=128) -> 01b  CLR1  (body)
     * CLR0/CLR1 are fixed black/white, so the result is independent of scan
     * order and of which pixel happens to come first. */
    const uint8_t *src = (const uint8_t *)d->pixels;   /* 4 bytes/pixel */

    for (uint32_t y = 0; y < d->height; y++)
    {
        volatile uint8_t *row = img + y * CUR_PITCH_B;
        for (uint32_t x = 0; x < d->width; x++)
        {
            const uint8_t *px = src + (y * d->width + x) * 4u;
            uint8_t r, g, b, a;
            if (d->format == DV_FMT_BGRA8888) { b = px[0]; g = px[1]; r = px[2]; a = px[3]; }
            else                              { r = px[0]; g = px[1]; b = px[2]; a = px[3]; } /* RGBA */

            uint8_t code;
            if (a < 128u)
                code = 0x2u;                           /* transparent (10b)  */
            else
            {
                /* Cheap luma (sum/4 ~ average); body=white -> high, border=
                 * black -> low. Threshold at mid-grey. */
                uint32_t luma = ((uint32_t)r + g + b) / 3u;
                code = (luma >= 128u) ? 0x1u            /* CLR1 = white body  */
                                      : 0x0u;           /* CLR0 = black border*/
            }
            /* pack 2 bits at pixel x, Intel order (low pixel = low bits) */
            uint32_t shift = (x & 3u) * 2u;
            uint8_t *cell = (uint8_t *)&row[x >> 2];
            *cell = (uint8_t)((*cell & ~(0x3u << shift)) | (code << shift));
        }
    }

    cursor_program_colors(0x00000000u, 0x00FFFFFFu);   /* CLR0 black, CLR1 white */

    g_mach64.cursor_hotspot_x = d->hotspot_x;
    g_mach64.cursor_hotspot_y = d->hotspot_y;

    /* CUR_OFFSET is the cursor image address in QWORDS. */
    mach64_mmio_w32(M64_CUR_OFFSET, g_mach64.cursor_vram_offset >> 3);
    return DV_OK;
}

int32_t mach64_cursor_set_position(int32_t x, int32_t y)
{
    /* Apply hotspot: the on-screen top-left corner is (x - hotspot). */
    int32_t px = x - (int32_t)g_mach64.cursor_hotspot_x;
    int32_t py = y - (int32_t)g_mach64.cursor_hotspot_y;

    /* The Mach64 hides the cursor entirely if POSN goes negative.  Handle it
     * per PRG 6.4.3: saturate POSN to 0 and push the difference into the
     * in-sprite offset (and CUR_OFFSET for the vertical case). */
    uint32_t off_x = 0u, off_y = 0u;
    uint32_t cur_off_qw = g_mach64.cursor_vram_offset >> 3;

    if (px < 0) { off_x = (uint32_t)(-px); if (off_x > 63u) off_x = 63u; px = 0; }
    if (py < 0) {
        off_y = (uint32_t)(-py); if (off_y > 63u) off_y = 63u;
        cur_off_qw += off_y * (CUR_PITCH_B >> 3);   /* skip clipped top lines */
        py = 0;
    }

    g_mach64.cursor_x = x;
    g_mach64.cursor_y = y;

    mach64_mmio_w32(M64_CUR_OFFSET,         cur_off_qw);
    mach64_mmio_w32(M64_CUR_HORZ_VERT_OFF,  (off_y << 16) | (off_x & 0xFFFFu));
    mach64_mmio_w32(M64_CUR_HORZ_VERT_POSN, ((uint32_t)py << 16) | ((uint32_t)px & 0xFFFFu));
    return DV_OK;
}

int32_t mach64_cursor_show(void)
{
    uint32_t g = mach64_mmio_r32(M64_GEN_TEST_CNTL);
    mach64_mmio_w32(M64_GEN_TEST_CNTL, g | M64_GEN_CUR_EN);
    g_mach64.cursor_visible = true;
    return DV_OK;
}

int32_t mach64_cursor_hide(void)
{
    uint32_t g = mach64_mmio_r32(M64_GEN_TEST_CNTL);
    mach64_mmio_w32(M64_GEN_TEST_CNTL, g & ~M64_GEN_CUR_EN);
    g_mach64.cursor_visible = false;
    return DV_OK;
}
