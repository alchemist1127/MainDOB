/* BGA driver -- fast-transport C dispatcher.
 *
 * The kernel int 0x85 boomerang lands in bga_fast_entry (asm shim
 * in bga_fast_entry.asm), which marshals the 5 register args into a
 * cdecl call to us.  Argument layout per the boomerang ABI:
 *
 *   opcode   = EAX        -- dobVideo opcode
 *   a0       = EBX        -- scalar arg 0
 *   a1       = ECX        -- scalar arg 1
 *   a2       = EDX        -- scalar arg 2
 *   a3       = ESI        -- pointer to kernel payload buffer (or 0)
 *   a4       = EDI        -- payload size in bytes (or 0)
 *
 * The kernel has already copied the caller's payload into a
 * kernel-VA buffer before CR3 switch, and set ESI to that VA -- we
 * dereference it directly.  When we mutate the buffer (e.g. write
 * an OUT handle), the kernel copies it back to the caller after we
 * return.
 *
 * IO struct layouts must byte-match libdob/src/video.c.  The
 * dobVideo opcodes are the protocol; the IO struct layout is the
 * wire format -- change one side, change the other.
 *
 * The IPC transport remains for control-plane ops (DOBVC_*) and
 * any future ops too large for the 16 KiB boomerang payload cap.
 */

#include <dob/video.h>
#include <string.h>

#include "bga_state.h"

/* ---- packing helpers ---- */

static inline dv_color_t unpack_color(uint32_t pix)
{
    dv_color_t c;
    c.b = (uint8_t)(pix       & 0xFF);
    c.g = (uint8_t)((pix >> 8) & 0xFF);
    c.r = (uint8_t)((pix >> 16) & 0xFF);
    c.a = (uint8_t)((pix >> 24) & 0xFF);
    return c;
}

/* ---- IO struct definitions (must match libdob/src/video.c) ---- */

typedef struct {
    dv_vproc_attach_desc_t  desc;
    dv_vproc_t              out;
} dv_vproc_attach_io_t;

typedef struct {
    dv_vram_kind_t          kind;
    uint32_t                size_lo;
    uint32_t                size_hi;
    uint32_t                flags;
    dv_buffer_t             out;
} dv_vram_alloc_io_t;

typedef struct {
    dv_surface_desc_t       desc;
    dv_surface_t            out;
} dv_surface_create_io_t;

typedef struct {
    dv_texture_desc_t       desc;
    dv_texture_t            out;
} dv_texture_create_io_t;

typedef struct {
    dv_rect_t               r;
} dv_fill_rect_io_t;

typedef struct {
    dv_rect_t               sr;
    dv_point_t              dp;
    dv_surface_t            src;
    dv_surface_t            dst;
} dv_blit_io_t;

typedef struct {
    dv_point_t              a;
    dv_point_t              b;
    uint32_t                thickness;
    dv_color_t              c;
    dv_surface_t            dst;
} dv_draw_line_io_t;

typedef struct {
    dv_rect_t               r;
    uint32_t                thickness;
    dv_color_t              c;
    dv_surface_t            dst;
} dv_draw_rect_outline_io_t;

typedef struct {
    dv_color_t              c;
    dv_surface_t            dst;
    dv_texture_t            atlas;
    uint32_t                count;
    /* dv_glyph_t glyphs[count] follows */
} dv_draw_glyphs_io_t;

typedef struct {
    dv_layer_desc_t         desc;
    dv_layer_t              out;
} dv_layer_create_io_t;

typedef struct {
    dv_rect_t               r;
    /* uint32_t pixels[r.w * r.h] follows */
} dv_texture_update_region_io_t;

typedef struct {
    dv_rect_t               sr;
    dv_rect_t               dr;
    dv_surface_t            src;
    dv_surface_t            dst;
} dv_blit_stretched_io_t;

typedef struct {
    dv_rect_t               r;
    dv_color_t              ca;
    dv_color_t              cb;
    uint32_t                dir;
    dv_surface_t            dst;
} dv_fill_gradient_io_t;

typedef struct {
    dv_rect_t               r;
    int32_t                 dx;
    int32_t                 dy;
    dv_color_t              fill;
    dv_surface_t            dst;
} dv_scroll_region_io_t;

int32_t bga_fast_dispatch(uint32_t opcode,
                          uint32_t a0, uint32_t a1,
                          uint32_t a2, uint32_t a3,
                          uint32_t a4, uint32_t caller_pid)
{
    void    *payload      = (void *)(uintptr_t)a3;
    uint32_t payload_size = a4;

    /* Stash the caller's pid for the duration of this dispatch.
     * dv_vproc_attach reads this when stamping owner_pid on a fresh
     * vproc slot; without it the slot would inherit the pid of the
     * last IPC-transport caller (or zero), which breaks
     * dv_vproc_detach-on-death cleanup. */
    bga_current_caller_pid = (pid_t)caller_pid;

    switch (opcode)
    {
    /* ===================================================================
     * Scalar-only opcodes (no payload -- pre-existing fast path)
     * =================================================================== */

    case DV_VPROC_DETACH:
        return dv_vproc_detach((dv_vproc_t)a0);

    case DV_VPROC_SET_QUOTA:
        return dv_vproc_set_quota((dv_vproc_t)a0,
                                  ((uint64_t)a2 << 32) | (uint64_t)a1);

    case DV_VRAM_INFO:
    {
        /* Caller passes a pointer to a dv_vram_info_t in the payload
         * slot; driver fills it and the kernel copies it back. */
        if (!payload || payload_size < sizeof(dv_vram_info_t))
            return DV_ERR_INVAL;
        return dv_vram_info((dv_vproc_t)a0, (dv_vram_info_t *)payload);
    }

    case DV_VTHREAD_DESTROY:
        return dv_vthread_destroy((dv_vthread_t)a0);

    case DV_VTHREAD_PRIORITY:
        return dv_vthread_priority((dv_vthread_t)a0, (uint8_t)a1);

    case DV_VTHREAD_YIELD:
        return dv_vthread_yield((dv_vthread_t)a0);

    case DV_VTHREAD_JOIN:
        return dv_vthread_join((dv_vthread_t)a0, a1);

    case DV_VTHREAD_WAIT_IDLE:
        return dv_vthread_wait_idle((dv_vthread_t)a0, a1);

    case DV_VCORE_AFFINITY:
        return dv_vcore_affinity((dv_vthread_t)a0, (int32_t)a1);

    case DV_VSYNC_WAIT:
        return dv_vsync_wait(a0, a1);

    case DV_VRAM_FREE:
        return dv_vram_free((dv_buffer_t)a0);

    case DV_VRAM_UNMAP:
        return dv_vram_unmap((dv_buffer_t)a0);

    case DV_VRAM_LOCK:
        return dv_vram_lock((dv_buffer_t)a0, a1);

    case DV_VRAM_UNLOCK:
        return dv_vram_unlock((dv_buffer_t)a0);

    case DV_SURFACE_DESTROY:
        return dv_surface_destroy((dv_surface_t)a0);

    case DV_SURFACE_CLEAR:
        return dv_surface_clear((dv_surface_t)a0, unpack_color(a1));

    case DV_SURFACE_RESIZE:
        return dv_surface_resize((dv_surface_t)a0, a1, a2);

    case DV_SURFACE_UNLOCK:
        return dv_surface_unlock((dv_surface_t)a0);

    case DV_TEXTURE_DESTROY:
        return dv_texture_destroy((dv_texture_t)a0);

    case DV_TEXTURE_GENERATE_MIPS:
        return dv_texture_generate_mips((dv_texture_t)a0);

    case DV_TEXTURE_BIND:
        return dv_texture_bind(a0, (dv_texture_t)a1);

    case DV_BUFFER_DESTROY:
        return dv_buffer_destroy((dv_buffer_t)a0);

    case DV_BUFFER_UNMAP:
        return dv_buffer_unmap((dv_buffer_t)a0);

    case DV_COMPOSE:
        return dv_compose(a0, (dv_fence_t)a1);

    case DV_COMPOSE_RECT:
    {
        /* Client packs: a0 = (rx<<16)|ry, a1 = (rw<<16)|rh, fence in
         * a2, display_id in a3 (vedi wrapper libdob). Coordinate con
         * segno a 16 bit. */
        int32_t  rx = (int16_t)((uint32_t)a0 >> 16);
        int32_t  ry = (int16_t)((uint32_t)a0 & 0xFFFF);
        uint32_t rw = ((uint32_t)a1 >> 16);
        uint32_t rh = ((uint32_t)a1 & 0xFFFF);
        return dv_compose_rect_present((uint32_t)a3, rx, ry, rw, rh,
                                       (dv_fence_t)a2);
    }

    case DV_PAGE_FLIP:
        return dv_page_flip(a0, a1, (dv_fence_t)a2);

    /* ===================================================================
     * Payload-bearing opcodes (read/write the kernel buffer at a3)
     * =================================================================== */

    case DV_VPROC_ATTACH:
    {
        if (!payload || payload_size < sizeof(dv_vproc_attach_io_t))
            return DV_ERR_INVAL;
        dv_vproc_attach_io_t *io = (dv_vproc_attach_io_t *)payload;
        dv_vproc_t v;
        int32_t rc = dv_vproc_attach(&io->desc, &v);
        if (rc == DV_OK) io->out = v;
        return rc;
    }

    case DV_VRAM_ALLOC:
    {
        if (!payload || payload_size < sizeof(dv_vram_alloc_io_t))
            return DV_ERR_INVAL;
        dv_vram_alloc_io_t *io = (dv_vram_alloc_io_t *)payload;
        uint64_t bytes = ((uint64_t)io->size_hi << 32) | (uint64_t)io->size_lo;
        dv_buffer_t b;
        int32_t rc = dv_vram_alloc((dv_vproc_t)a0, io->kind, bytes,
                                   io->flags, &b);
        if (rc == DV_OK) io->out = b;
        return rc;
    }

    case DV_SURFACE_CREATE:
    {
        if (!payload || payload_size < sizeof(dv_surface_create_io_t))
            return DV_ERR_INVAL;
        dv_surface_create_io_t *io = (dv_surface_create_io_t *)payload;
        dv_surface_t s;
        int32_t rc = dv_surface_create((dv_vproc_t)a0, &io->desc, &s);
        if (rc == DV_OK) io->out = s;
        return rc;
    }

    case DV_TEXTURE_CREATE:
    {
        if (!payload || payload_size < sizeof(dv_texture_create_io_t))
            return DV_ERR_INVAL;
        dv_texture_create_io_t *io = (dv_texture_create_io_t *)payload;
        dv_texture_t t;
        int32_t rc = dv_texture_create((dv_vproc_t)a0, &io->desc, &t);
        if (rc == DV_OK) io->out = t;
        return rc;
    }

    case DV_TEXTURE_UPLOAD:
        if (!payload || payload_size == 0) return DV_ERR_INVAL;
        return dv_texture_upload((dv_texture_t)a0, payload, payload_size);

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
        /* a1 = use_pixel_alpha (0 storico = alpha uniforme a0). */
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

    case DV_FILL_GRADIENT:
    {
        if (!payload || payload_size < sizeof(dv_fill_gradient_io_t))
            return DV_ERR_INVAL;
        const dv_fill_gradient_io_t *io =
            (const dv_fill_gradient_io_t *)payload;
        return dv_fill_gradient(io->dst, io->r, io->ca, io->cb,
                                (dv_gradient_dir_t)io->dir);
    }

    case DV_SCROLL_REGION:
    {
        if (!payload || payload_size < sizeof(dv_scroll_region_io_t))
            return DV_ERR_INVAL;
        const dv_scroll_region_io_t *io =
            (const dv_scroll_region_io_t *)payload;
        return dv_scroll_region(io->dst, io->r, io->dx, io->dy, io->fill);
    }

    case DV_DRAW_LINE:
    {
        if (!payload || payload_size < sizeof(dv_draw_line_io_t))
            return DV_ERR_INVAL;
        const dv_draw_line_io_t *io = (const dv_draw_line_io_t *)payload;
        return dv_draw_line(io->dst, io->a, io->b, io->thickness, io->c);
    }

    case DV_DRAW_RECT_OUTLINE:
    {
        if (!payload || payload_size < sizeof(dv_draw_rect_outline_io_t))
            return DV_ERR_INVAL;
        const dv_draw_rect_outline_io_t *io =
            (const dv_draw_rect_outline_io_t *)payload;
        return dv_draw_rect_outline(io->dst, io->r, io->thickness, io->c);
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

    case DV_TEXTURE_UPDATE_REGION:
    {
        if (!payload || payload_size < sizeof(dv_texture_update_region_io_t))
            return DV_ERR_INVAL;
        const dv_texture_update_region_io_t *io =
            (const dv_texture_update_region_io_t *)payload;
        uint32_t need = sizeof(*io) + io->r.w * io->r.h * 4;
        if (payload_size < need) return DV_ERR_INVAL;
        const void *pixels = (const void *)((const uint8_t *)io + sizeof(*io));
        /* src_pitch = r.w * 4 because libdob packs rows tightly. */
        return dv_texture_update_region((dv_texture_t)a0, io->r,
                                        pixels, io->r.w * 4);
    }

    case DV_LAYER_CREATE:
    {
        if (!payload || payload_size < sizeof(dv_layer_create_io_t))
            return DV_ERR_INVAL;
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
        if (!payload || payload_size < sizeof(dv_layer_create_io_t))
            return DV_ERR_INVAL;
        const dv_layer_create_io_t *io =
            (const dv_layer_create_io_t *)payload;
        return dv_layer_update((dv_layer_t)a0, &io->desc);
    }

    case DV_LAYER_SET_VISIBLE:
        return dv_layer_set_visible((dv_layer_t)a0, a1 != 0);

    /* ===================================================================
     * Cmdlist (retained-mode) opcodes
     * =================================================================== */

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
        /* a0 = cmdlist, a1 = packed color (bgra), payload = dv_rect_t */
        if (!payload || payload_size < sizeof(dv_rect_t)) return DV_ERR_INVAL;
        const dv_rect_t *r = (const dv_rect_t *)payload;
        return dv_cmdlist_fill_rect((dv_cmdlist_t)a0, *r, unpack_color(a1));
    }

    case DV_CMDLIST_BLIT:
    {
        /* a0 = cmdlist, payload = { dv_surface_t src; dv_rect_t sr; dv_point_t dp; } */
        struct { dv_surface_t src; dv_rect_t sr; dv_point_t dp; } *io;
        if (!payload || payload_size < sizeof(*io)) return DV_ERR_INVAL;
        io = (typeof(io))payload;
        return dv_cmdlist_blit((dv_cmdlist_t)a0, io->src, io->sr, io->dp);
    }

    case DV_CMDLIST_BLIT_ALPHA:
    {
        /* a0 = cmdlist, a1 = (alpha<<8) | use_pixel_alpha,
         * payload = { dv_surface_t src; dv_rect_t sr; dv_point_t dp; } */
        struct { dv_surface_t src; dv_rect_t sr; dv_point_t dp; } *io;
        if (!payload || payload_size < sizeof(*io)) return DV_ERR_INVAL;
        io = (typeof(io))payload;
        uint8_t alpha = (uint8_t)(a1 >> 8);
        bool    upa   = (a1 & 1) != 0;
        return dv_cmdlist_blit_alpha((dv_cmdlist_t)a0, io->src, io->sr,
                                     io->dp, alpha, upa);
    }

    case DV_CMDLIST_DRAW_GLYPHS:
    {
        /* a0 = cmdlist, a1 = packed color (bgra),
         * payload = { dv_texture_t atlas; uint32_t count; dv_glyph_t[count]; } */
        if (!payload || payload_size < sizeof(uint32_t) * 2) return DV_ERR_INVAL;
        const uint8_t *p = (const uint8_t *)payload;
        dv_texture_t atlas;  uint32_t count;
        __builtin_memcpy(&atlas, p,     4);
        __builtin_memcpy(&count, p + 4, 4);
        uint32_t need = 8 + count * sizeof(dv_glyph_t);
        if (payload_size < need) return DV_ERR_INVAL;
        const dv_glyph_t *glyphs = (const dv_glyph_t *)(p + 8);
        return dv_cmdlist_draw_glyphs((dv_cmdlist_t)a0, atlas, glyphs,
                                      count, unpack_color(a1));
    }

    case DV_CMDLIST_DRAW_LINE:
    {
        /* a0 = cmdlist, a1 = packed color (bgra), a2 = thickness,
         * payload = { dv_point_t a; dv_point_t b; } */
        struct { dv_point_t a; dv_point_t b; } *io;
        if (!payload || payload_size < sizeof(*io)) return DV_ERR_INVAL;
        io = (typeof(io))payload;
        return dv_cmdlist_draw_line((dv_cmdlist_t)a0, io->a, io->b, a2,
                                    unpack_color(a1));
    }

    case DV_CMDLIST_INFO:
    {
        /* a0 = cmdlist, payload = out dv_cmdlist_info_t */
        if (!payload || payload_size < sizeof(dv_cmdlist_info_t)) return DV_ERR_INVAL;
        return dv_cmdlist_info((dv_cmdlist_t)a0, (dv_cmdlist_info_t *)payload);
    }

    /* Capability query (boomerang/fast path). */
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

    /* Hardware cursor opcodes: dispatched for interface symmetry with the
     * mach64 driver.  BGA has no HW cursor, so these resolve to the
     * dv_cursor_* stubs that return DV_ERR_NOSUPPORT — dobinterface sees
     * the same opcodes on both drivers and keys off DV_CAP_HW_CURSOR. */
    case DV_CURSOR_SET_BITMAP: {
        if (!payload || payload_size < sizeof(dv_cursor_desc_t)) return DV_ERR_INVAL;
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

    /* Anything else -- caller falls back to the IPC transport (which
     * still routes the dv_* but with full message marshaling). */
    default:
        return DV_ERR_NOSUPPORT;
    }
}

