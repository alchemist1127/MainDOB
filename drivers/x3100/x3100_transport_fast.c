/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * MainDOB x3100 driver — fast-transport C dispatcher (data plane).
 *
 * The kernel int 0x85 boomerang lands in x3100_fast_entry (asm shim in
 * x3100_fast_entry.asm), which marshals the 5 register args into a cdecl
 * call to us.  Argument layout per the boomerang ABI:
 *
 *   opcode = EAX, a0..a4 = EBX/ECX/EDX/ESI/EDI, caller_pid = EBP
 *   a3 = pointer to a kernel-VA payload buffer (or 0); a4 = its size.
 *
 * The kernel copies the caller's payload into a kernel buffer before the
 * CR3 switch and copies it back after we return, so we read/write *payload
 * directly (OUT handles land back in the caller's buffer).
 *
 * IO struct layouts MUST byte-match libdob/src/video.c.  These mirror the
 * BGA reference's definitions (already aligned to libdob) so the two
 * drivers stay wire-compatible at the protocol level.
 *
 * This driver implements the subset of the data plane that dobinterface
 * uses (vproc / vram_info / surface / texture / cmdlist / layer / compose /
 * page_flip, plus the direct surface draws — fill_rect / blit / blit_alpha /
 * blit_stretched / draw_glyphs — that the 1.1 baked-surface pipeline relies
 * on).  Every other opcode returns DV_ERR_NOSUPPORT, making the caller fall
 * back to the IPC transport rather than mis-handling it.
 */

#include <dob/video.h>
#include <string.h>

#include "x3100_state.h"

/* ---- packing helper (matches the protocol's BGRA byte order) ---- */
static inline dv_color_t unpack_color(uint32_t pix)
{
    dv_color_t c;
    c.b = (uint8_t)( pix        & 0xFF);
    c.g = (uint8_t)((pix >> 8)  & 0xFF);
    c.r = (uint8_t)((pix >> 16) & 0xFF);
    c.a = (uint8_t)((pix >> 24) & 0xFF);
    return c;
}

/* ---- IO struct definitions (must match libdob/src/video.c) ---- */

typedef struct { dv_vproc_attach_desc_t desc; dv_vproc_t  out; } dv_vproc_attach_io_t;
typedef struct { dv_surface_desc_t      desc; dv_surface_t out; } dv_surface_create_io_t;
typedef struct { dv_texture_desc_t      desc; dv_texture_t out; } dv_texture_create_io_t;
typedef struct { dv_layer_desc_t        desc; dv_layer_t   out; } dv_layer_create_io_t;
typedef struct { dv_rect_t r; /* uint32_t pixels[r.w*r.h] follows */ } dv_texture_update_region_io_t;

/* Draw diretti (pipeline dobinterface 1.1) — layout identici a
 * libdob/src/video.c e al riferimento BGA, byte per byte. */
typedef struct { dv_rect_t r; } dv_fill_rect_io_t;
typedef struct {
    dv_rect_t    sr;
    dv_point_t   dp;
    dv_surface_t src;
    dv_surface_t dst;
} dv_blit_io_t;
typedef struct {
    dv_rect_t    sr;
    dv_rect_t    dr;
    dv_surface_t src;
    dv_surface_t dst;
} dv_blit_stretched_io_t;
typedef struct {
    dv_color_t   c;
    dv_surface_t dst;
    dv_texture_t atlas;
    uint32_t     count;
    /* dv_glyph_t glyphs[count] follows */
} dv_draw_glyphs_io_t;

int32_t x3100_fast_dispatch(uint32_t opcode,
                             uint32_t a0, uint32_t a1,
                             uint32_t a2, uint32_t a3,
                             uint32_t a4, uint32_t caller_pid)
{
    void    *payload      = (void *)(uintptr_t)a3;
    uint32_t payload_size = a4;

    /* dv_vproc_attach stamps owner_pid from this; set it first so a fresh
     * vproc slot is attributed to the real caller (needed for
     * detach-on-death cleanup), not the previous IPC caller. */
    x3100_current_caller_pid = (pid_t)caller_pid;

    switch (opcode)
    {
    /* ---------------- scalar opcodes (no payload) ---------------- */
    case DV_VPROC_DETACH:
        return dv_vproc_detach((dv_vproc_t)a0);

    case DV_VPROC_SET_QUOTA:
        return dv_vproc_set_quota((dv_vproc_t)a0,
                                  ((uint64_t)a2 << 32) | (uint64_t)a1);

    case DV_VRAM_INFO:
        if (!payload || payload_size < sizeof(dv_vram_info_t)) return DV_ERR_INVAL;
        return dv_vram_info((dv_vproc_t)a0, (dv_vram_info_t *)payload);

    case DV_VPROC_INFO:
        if (!payload || payload_size < sizeof(dv_vproc_info_t)) return DV_ERR_INVAL;
        return dv_vproc_info((dv_vproc_t)a0, (dv_vproc_info_t *)payload);

    case DV_SURFACE_DESTROY:
        return dv_surface_destroy((dv_surface_t)a0);

    case DV_SURFACE_INFO:
        if (!payload || payload_size < sizeof(dv_surface_info_t)) return DV_ERR_INVAL;
        return dv_surface_info((dv_surface_t)a0, (dv_surface_info_t *)payload);

    case DV_SURFACE_CLEAR:
        /* a0 = surface, a1 = packed bgra colour */
        return dv_surface_clear((dv_surface_t)a0, unpack_color(a1));

    case DV_TEXTURE_DESTROY:
        return dv_texture_destroy((dv_texture_t)a0);

    case DV_COMPOSE:
        return dv_compose(a0, (dv_fence_t)a1);

    case DV_COMPOSE_RECT:
    {
        /* a0 = (rx<<16)|ry, a1 = (rw<<16)|rh, a2 = fence, a3 = display_id.
         * rx/ry are signed 16-bit; sign-extend them. */
        int32_t rx = (int32_t)(int16_t)((uint32_t)a0 >> 16);
        int32_t ry = (int32_t)(int16_t)((uint32_t)a0 & 0xFFFFu);
        uint32_t rw = ((uint32_t)a1 >> 16) & 0xFFFFu;
        uint32_t rh = (uint32_t)a1 & 0xFFFFu;
        return dv_compose_rect((uint32_t)a3, rx, ry, rw, rh, (dv_fence_t)a2);
    }

    case DV_PAGE_FLIP:
        return dv_page_flip(a0, a1, (dv_fence_t)a2);

    /* ---------------- payload-bearing opcodes ---------------- */
    case DV_VPROC_ATTACH:
    {
        if (!payload || payload_size < sizeof(dv_vproc_attach_io_t)) return DV_ERR_INVAL;
        dv_vproc_attach_io_t *io = (dv_vproc_attach_io_t *)payload;
        dv_vproc_t v;
        int32_t rc = dv_vproc_attach(&io->desc, &v);
        if (rc == DV_OK) io->out = v;
        return rc;
    }

    case DV_SURFACE_CREATE:
    {
        if (!payload || payload_size < sizeof(dv_surface_create_io_t)) return DV_ERR_INVAL;
        dv_surface_create_io_t *io = (dv_surface_create_io_t *)payload;
        dv_surface_t s;
        int32_t rc = dv_surface_create((dv_vproc_t)a0, &io->desc, &s);
        if (rc == DV_OK) io->out = s;
        return rc;
    }

    case DV_TEXTURE_CREATE:
    {
        if (!payload || payload_size < sizeof(dv_texture_create_io_t)) return DV_ERR_INVAL;
        dv_texture_create_io_t *io = (dv_texture_create_io_t *)payload;
        dv_texture_t t;
        int32_t rc = dv_texture_create((dv_vproc_t)a0, &io->desc, &t);
        if (rc == DV_OK) io->out = t;
        return rc;
    }

    case DV_TEXTURE_UPDATE_REGION:
    {
        if (!payload || payload_size < sizeof(dv_texture_update_region_io_t))
            return DV_ERR_INVAL;
        const dv_texture_update_region_io_t *io =
            (const dv_texture_update_region_io_t *)payload;
        uint32_t need = sizeof(*io) + io->r.w * io->r.h * 4u;
        if (payload_size < need) return DV_ERR_INVAL;
        const void *pixels = (const void *)((const uint8_t *)io + sizeof(*io));
        /* libdob packs rows tightly -> src_pitch = r.w * 4 */
        return dv_texture_update_region((dv_texture_t)a0, io->r, pixels, io->r.w * 4u);
    }

    case DV_LAYER_CREATE:
    {
        if (!payload || payload_size < sizeof(dv_layer_create_io_t)) return DV_ERR_INVAL;
        dv_layer_create_io_t *io = (dv_layer_create_io_t *)payload;
        dv_layer_t l;
        int32_t rc = dv_layer_create((dv_vproc_t)a0, &io->desc, &l);
        if (rc == DV_OK) io->out = l;
        return rc;
    }

    case DV_LAYER_DESTROY:
        return dv_layer_destroy((dv_layer_t)a0);

    case DV_LAYER_UPDATE:
    {
        if (!payload || payload_size < sizeof(dv_layer_create_io_t)) return DV_ERR_INVAL;
        const dv_layer_create_io_t *io = (const dv_layer_create_io_t *)payload;
        return dv_layer_update((dv_layer_t)a0, &io->desc);
    }

    case DV_LAYER_SET_VISIBLE:
        return dv_layer_set_visible((dv_layer_t)a0, a1 != 0u);

    /* ---------------- cmdlist (retained-mode) opcodes ---------------- */
    case DV_CMDLIST_CREATE:
    {
        /* a0 = vproc, a1 = capacity_bytes, payload = out dv_cmdlist_t */
        if (!payload || payload_size < sizeof(dv_cmdlist_t)) return DV_ERR_INVAL;
        dv_cmdlist_t cl;
        int32_t rc = dv_cmdlist_create((dv_vproc_t)a0, a1, &cl);
        if (rc == DV_OK) *(dv_cmdlist_t *)payload = cl;
        return rc;
    }

    case DV_CMDLIST_DESTROY:
        return dv_cmdlist_destroy((dv_cmdlist_t)a0);

    case DV_CMDLIST_RESET:
        return dv_cmdlist_reset((dv_cmdlist_t)a0);

    case DV_CMDLIST_FILL_RECT:
    {
        /* a0 = cmdlist, a1 = packed bgra, payload = dv_rect_t */
        if (!payload || payload_size < sizeof(dv_rect_t)) return DV_ERR_INVAL;
        const dv_rect_t *r = (const dv_rect_t *)payload;
        return dv_cmdlist_fill_rect((dv_cmdlist_t)a0, *r, unpack_color(a1));
    }

    case DV_CMDLIST_BLIT:
    {
        struct { dv_surface_t src; dv_rect_t sr; dv_point_t dp; } io;
        if (!payload || payload_size < sizeof(io)) return DV_ERR_INVAL;
        __builtin_memcpy(&io, payload, sizeof(io));
        return dv_cmdlist_blit((dv_cmdlist_t)a0, io.src, io.sr, io.dp);
    }

    case DV_CMDLIST_BLIT_ALPHA:
    {
        /* a1 = (alpha << 8) | use_pixel_alpha */
        struct { dv_surface_t src; dv_rect_t sr; dv_point_t dp; } io;
        if (!payload || payload_size < sizeof(io)) return DV_ERR_INVAL;
        __builtin_memcpy(&io, payload, sizeof(io));
        uint8_t alpha = (uint8_t)(a1 >> 8);
        bool    upa   = (a1 & 1u) != 0u;
        return dv_cmdlist_blit_alpha((dv_cmdlist_t)a0, io.src, io.sr, io.dp, alpha, upa);
    }

    case DV_CMDLIST_DRAW_GLYPHS:
    {
        /* a1 = packed bgra; payload = { dv_texture_t atlas; uint32_t count;
         * dv_glyph_t glyphs[count] } */
        if (!payload || payload_size < sizeof(uint32_t) * 2u) return DV_ERR_INVAL;
        const uint8_t *p = (const uint8_t *)payload;
        dv_texture_t atlas; uint32_t count;
        __builtin_memcpy(&atlas, p,     4);
        __builtin_memcpy(&count, p + 4, 4);
        uint32_t need = 8u + count * (uint32_t)sizeof(dv_glyph_t);
        if (payload_size < need) return DV_ERR_INVAL;
        const dv_glyph_t *glyphs = (const dv_glyph_t *)(p + 8);
        return dv_cmdlist_draw_glyphs((dv_cmdlist_t)a0, atlas, glyphs,
                                      count, unpack_color(a1));
    }

    case DV_CMDLIST_DRAW_LINE:
    {
        /* a1 = packed bgra, a2 = thickness; payload = { dv_point_t a, b } */
        struct { dv_point_t a; dv_point_t b; } io;
        if (!payload || payload_size < sizeof(io)) return DV_ERR_INVAL;
        __builtin_memcpy(&io, payload, sizeof(io));
        return dv_cmdlist_draw_line((dv_cmdlist_t)a0, io.a, io.b, a2, unpack_color(a1));
    }

    case DV_CMDLIST_INFO:
        if (!payload || payload_size < sizeof(dv_cmdlist_info_t)) return DV_ERR_INVAL;
        return dv_cmdlist_info((dv_cmdlist_t)a0, (dv_cmdlist_info_t *)payload);

    /* Capability query (boomerang/fast path; IO struct carries the mask). */
    case DV_CAP_QUERY: {
        if (!payload || payload_size < 8u) return DV_ERR_INVAL;
        uint64_t caps = 0; int32_t rc = dv_cap_query(&caps);
        uint32_t *io = (uint32_t *)payload; io[0] = (uint32_t)caps; io[1] = (uint32_t)(caps >> 32);
        return rc;
    }
    case DV_CAP_QUERY_LIMIT: {
        if (!payload || payload_size < 8u) return DV_ERR_INVAL;
        uint64_t v = 0; int32_t rc = dv_cap_query_limit((dv_limit_t)a0, &v);
        uint32_t *io = (uint32_t *)payload; io[0] = (uint32_t)v; io[1] = (uint32_t)(v >> 32);
        return rc;
    }
    case DV_CAP_QUERY_FORMAT: {
        if (!payload || payload_size < 4u) return DV_ERR_INVAL;
        uint32_t f = 0; int32_t rc = dv_cap_query_format((dv_format_t)a0, &f);
        *(uint32_t *)payload = f;
        return rc;
    }

    /* Hardware cursor (opcodes 0x0A00..). */
    case DV_CURSOR_SET_BITMAP: {
        if (!payload || payload_size < sizeof(dv_cursor_desc_t)) return DV_ERR_INVAL;
        /* Wire layout: [desc][pixels].  Point desc.pixels at the trailing
         * bytes (kernel already copied the whole payload into our AS). */
        dv_cursor_desc_t d = *(const dv_cursor_desc_t *)payload;
        d.pixels = (const uint8_t *)payload + sizeof(dv_cursor_desc_t);
        return dv_cursor_set_bitmap(a0, &d);
    }
    case DV_CURSOR_SET_POSITION:
        return dv_cursor_set_position(a0, (int32_t)a1, (int32_t)a2);
    case DV_CURSOR_SHOW:
        return dv_cursor_show(a0);
    case DV_CURSOR_HIDE:
        return dv_cursor_hide(a0);

    /* ---- draw diretti su surface (blocco #2c in main.c) ----
     * La dobinterface 1.1 baka backbuf e corpi finestra con questi;
     * prima cadevano nel default DV_ERR_NOSUPPORT ed essendo i draw
     * lato client fire-and-forget lo schermo restava sfondo + finestre
     * nere: la compose girava, il contenuto non entrava mai. */
    case DV_FILL_RECT:
    {
        if (!payload || payload_size < sizeof(dv_fill_rect_io_t))
            return DV_ERR_INVAL;
        const dv_fill_rect_io_t *io = (const dv_fill_rect_io_t *)payload;
        return dv_fill_rect((dv_surface_t)a0, io->r, unpack_color(a1));
    }

    case DV_BLIT:
    {
        if (!payload || payload_size < sizeof(dv_blit_io_t))
            return DV_ERR_INVAL;
        const dv_blit_io_t *io = (const dv_blit_io_t *)payload;
        return dv_blit(io->src, io->sr, io->dst, io->dp);
    }

    case DV_BLIT_ALPHA:
    {
        if (!payload || payload_size < sizeof(dv_blit_io_t))
            return DV_ERR_INVAL;
        const dv_blit_io_t *io = (const dv_blit_io_t *)payload;
        /* a1 = use_pixel_alpha (0 storico = alpha uniforme in a0). */
        if (a1)
            return dv_blit_pixel_alpha(io->src, io->sr, io->dst, io->dp);
        return dv_blit_alpha(io->src, io->sr, io->dst, io->dp, (uint8_t)a0);
    }

    case DV_BLIT_STRETCHED:
    {
        if (!payload || payload_size < sizeof(dv_blit_stretched_io_t))
            return DV_ERR_INVAL;
        const dv_blit_stretched_io_t *io =
            (const dv_blit_stretched_io_t *)payload;
        return dv_blit_stretched(io->src, io->sr, io->dst, io->dr);
    }

    case DV_DRAW_GLYPHS:
    {
        if (!payload || payload_size < sizeof(dv_draw_glyphs_io_t))
            return DV_ERR_INVAL;
        const dv_draw_glyphs_io_t *hdr = (const dv_draw_glyphs_io_t *)payload;
        uint32_t need = sizeof(*hdr) + hdr->count * sizeof(dv_glyph_t);
        if (payload_size < need) return DV_ERR_INVAL;
        const dv_glyph_t *glyphs =
            (const dv_glyph_t *)((const uint8_t *)hdr + sizeof(*hdr));
        return dv_draw_glyphs(hdr->dst, hdr->atlas, glyphs, hdr->count, hdr->c);
    }

    /* Everything else: not on the fast path -> caller uses IPC. */
    default:
        return DV_ERR_NOSUPPORT;
    }
}
