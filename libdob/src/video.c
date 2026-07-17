/* libdob video -- client-side dv_* dispatch via int 0x85 boomerang.
 *
 * No IPC, ever.  Every dv_* function fires `int 0x85` with:
 *   EAX = opcode
 *   EBX/ECX/EDX = scalar args
 *   ESI = caller-AS pointer to a per-op IO struct
 *   EDI = sizeof(that struct)
 *
 * The kernel boomerang (kernel/syscall/video_boomerang.asm) copies
 * the IO struct to a kernel-VA buffer before CR3 switch, sets ESI
 * to that kernel VA for the driver dispatcher, calls the driver,
 * copies the (possibly-mutated) buffer back to the caller, and iret's
 * with the driver's int32_t return code in EAX.
 *
 * IO structs are bidirectional: ops that return a handle (create-
 * style) use one `_io_t` struct with IN args at the front and OUT
 * fields at the back.  Driver writes the OUT fields; kernel copies
 * back; libdob reads them after the call.
 *
 * Wire compatibility: each `_io_t` here must byte-match the
 * matching dispatcher case in every dobVideo driver's fast
 * dispatcher (drivers/bga/bga_transport_fast.c,
 * drivers/mach64/mach64_transport_fast.c, ...).  The struct
 * layout is the inter-process wire format -- change one side,
 * change all the others.
 */

#include <dob/video.h>
#include <sys/syscall.h>
#include <string.h>

/* ---- packing helpers ---- */

static inline uint32_t pack_color(dv_color_t c)
{
    return ((uint32_t)c.b)
         | ((uint32_t)c.g << 8)
         | ((uint32_t)c.r << 16)
         | ((uint32_t)c.a << 24);
}

/* ===== IO struct definitions (must match driver wire format) ===== */

typedef struct {
    dv_vproc_attach_desc_t  desc;        /* IN  */
    dv_vproc_t              out;         /* OUT */
} dv_vproc_attach_io_t;

typedef struct {
    dv_vram_kind_t          kind;        /* IN  */
    uint32_t                size_lo;     /* IN  (bytes & 0xFFFFFFFF) */
    uint32_t                size_hi;     /* IN  (bytes >> 32) */
    uint32_t                flags;       /* IN  */
    dv_buffer_t             out;         /* OUT */
} dv_vram_alloc_io_t;

typedef struct {
    dv_surface_desc_t       desc;        /* IN  */
    dv_surface_t            out;         /* OUT */
} dv_surface_create_io_t;

typedef struct {
    dv_texture_desc_t       desc;        /* IN  */
    dv_texture_t            out;         /* OUT */
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
    /* dv_glyph_t glyphs[count] follows immediately */
} dv_draw_glyphs_io_t;

/* Layer wire payload -- IN dv_layer_desc_t + OUT dv_layer_t. */
typedef struct {
    dv_layer_desc_t         desc;
    dv_layer_t              out;
} dv_layer_create_io_t;

/* Update-region wire payload: header + raw row data following.
 * One call uploads `r.h` rows of `r.w` BGRA pixels (4 bytes each)
 * into the destination texture at (r.x, r.y).  The pixel array
 * starts immediately after the header -- caller packs them tight
 * (src_pitch = r.w * 4 effectively, no row padding). */
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
    uint32_t                dir;        /* dv_gradient_dir_t */
    dv_surface_t            dst;
} dv_fill_gradient_io_t;

typedef struct {
    dv_rect_t               r;
    int32_t                 dx;
    int32_t                 dy;
    dv_color_t              fill;
    dv_surface_t            dst;
} dv_scroll_region_io_t;

/* ===== vproc lifecycle ===== */

int32_t dv_vproc_attach(const dv_vproc_attach_desc_t *desc, dv_vproc_t *out)
{
    if (!out) return DV_ERR_INVAL;
    dv_vproc_attach_io_t io = { 0 };
    if (desc) io.desc = *desc;
    int rc = dv_call_pl(DV_VPROC_ATTACH, 0, 0, 0, &io, sizeof(io));
    if (rc == DV_OK) *out = io.out;
    return rc;
}

int32_t dv_vproc_detach(dv_vproc_t v)
{
    return dv_call1(DV_VPROC_DETACH, (int)v);
}

int32_t dv_vproc_set_quota(dv_vproc_t v, uint64_t new_quota_bytes)
{
    return dv_call3(DV_VPROC_SET_QUOTA, (int)v,
                    (int)(uint32_t)(new_quota_bytes & 0xFFFFFFFFu),
                    (int)(uint32_t)(new_quota_bytes >> 32));
}

int32_t dv_vram_info(dv_vproc_t v, dv_vram_info_t *out)
{
    if (!out) return DV_ERR_INVAL;
    dv_vram_info_t io;
    memset(&io, 0, sizeof(io));
    int rc = dv_call_pl(DV_VRAM_INFO, (int)v, 0, 0, &io, sizeof(io));
    if (rc == DV_OK) *out = io;
    return rc;
}

/* ===== VRAM ===== */

int32_t dv_vram_alloc(dv_vproc_t v, dv_vram_kind_t kind, uint64_t bytes,
                      uint32_t flags, dv_buffer_t *out)
{
    if (!out) return DV_ERR_INVAL;
    if (bytes == 0) return DV_ERR_INVAL;

    dv_vram_alloc_io_t io = {
        .kind    = kind,
        .size_lo = (uint32_t)(bytes & 0xFFFFFFFFu),
        .size_hi = (uint32_t)(bytes >> 32),
        .flags   = flags,
        .out     = DV_HANDLE_NONE,
    };
    int rc = dv_call_pl(DV_VRAM_ALLOC, (int)v, 0, 0, &io, sizeof(io));
    if (rc == DV_OK) *out = io.out;
    return rc;
}

int32_t dv_vram_free(dv_buffer_t b)
{
    return dv_call1(DV_VRAM_FREE, (int)b);
}

/* ===== surface ===== */

int32_t dv_surface_create(dv_vproc_t v, const dv_surface_desc_t *d,
                          dv_surface_t *out)
{
    if (!out || !d) return DV_ERR_INVAL;
    dv_surface_create_io_t io = { .desc = *d, .out = DV_HANDLE_NONE };
    int rc = dv_call_pl(DV_SURFACE_CREATE, (int)v, 0, 0, &io, sizeof(io));
    if (rc == DV_OK) *out = io.out;
    return rc;
}

int32_t dv_surface_destroy(dv_surface_t s)
{
    return dv_call1(DV_SURFACE_DESTROY, (int)s);
}

int32_t dv_surface_clear(dv_surface_t s, dv_color_t color)
{
    return dv_call2(DV_SURFACE_CLEAR, (int)s, (int)pack_color(color));
}

/* ===== texture ===== */

int32_t dv_texture_create(dv_vproc_t v, const dv_texture_desc_t *d,
                          dv_texture_t *out)
{
    if (!out || !d) return DV_ERR_INVAL;
    dv_texture_create_io_t io = { .desc = *d, .out = DV_HANDLE_NONE };
    int rc = dv_call_pl(DV_TEXTURE_CREATE, (int)v, 0, 0, &io, sizeof(io));
    if (rc == DV_OK) *out = io.out;
    return rc;
}

int32_t dv_texture_destroy(dv_texture_t t)
{
    return dv_call1(DV_TEXTURE_DESTROY, (int)t);
}

int32_t dv_texture_upload(dv_texture_t t, const void *src, size_t src_bytes)
{
    if (!src || src_bytes == 0) return DV_ERR_INVAL;
    /* The boomerang payload buffer is 16 KiB.  Texture data larger
     * than that must be split via dv_texture_update_region calls or
     * fall back to IPC; we refuse here to keep the contract clear. */
    if (src_bytes > 16384) return DV_ERR_INVAL;
    return dv_call_pl(DV_TEXTURE_UPLOAD, (int)t, 0, 0,
                      (void *)src, (unsigned)src_bytes);
}

/* dv_texture_update_region -- upload pixels into a sub-rect of a
 * texture/surface.  Internally chunked into row-strips that fit in
 * the boomerang's 16 KiB payload buffer (header + row data).
 *
 * src is a tightly-packed BGRA8888 buffer of `r.w * r.h` pixels.
 * src_pitch in BYTES -- typical caller passes r.w * 4, but a larger
 * pitch lets you upload from a wider source bitmap by selecting a
 * sub-rect; this stub re-packs into a tight layout per chunk so the
 * driver dispatcher can compute offsets simply.
 *
 * Returns DV_OK iff every chunk succeeded; on partial failure
 * stops at the first error and returns its code (the destination
 * texture is left in a partially-updated state -- callers that need
 * atomicity must coordinate via fences or external locking). */
int32_t dv_texture_update_region(dv_texture_t t, dv_rect_t r,
                                 const void *src, uint32_t src_pitch)
{
    if (!src) return DV_ERR_INVAL;
    if (r.w == 0 || r.h == 0) return DV_OK;

    const uint32_t header_bytes = sizeof(dv_texture_update_region_io_t);
    const uint32_t bytes_per_pixel = 4;
    const uint32_t row_bytes = r.w * bytes_per_pixel;
    if (row_bytes > 16384 - header_bytes)
    {
        /* A single row exceeds the cap.  Caller must subdivide
         * horizontally -- we don't auto-split rows because that
         * complicates the wire format (per-chunk x-offset). */
        return DV_ERR_INVAL;
    }

    /* How many rows fit per chunk? */
    uint32_t rows_per_chunk = (16384 - header_bytes) / row_bytes;
    if (rows_per_chunk == 0) rows_per_chunk = 1;
    if (rows_per_chunk > r.h) rows_per_chunk = r.h;

    uint8_t buf[16384];
    const uint8_t *src_bytes = (const uint8_t *)src;

    for (uint32_t row_off = 0; row_off < r.h; row_off += rows_per_chunk)
    {
        uint32_t chunk_rows = rows_per_chunk;
        if (row_off + chunk_rows > r.h) chunk_rows = r.h - row_off;

        dv_texture_update_region_io_t *hdr =
            (dv_texture_update_region_io_t *)buf;
        hdr->r.x = r.x;
        hdr->r.y = r.y + (int32_t)row_off;
        hdr->r.w = r.w;
        hdr->r.h = chunk_rows;

        /* Pack chunk_rows of row_bytes from src (with src_pitch
         * stride) into the buffer immediately after the header. */
        uint8_t *dst = buf + header_bytes;
        for (uint32_t y = 0; y < chunk_rows; y++)
        {
            memcpy(dst + y * row_bytes,
                   src_bytes + (row_off + y) * src_pitch,
                   row_bytes);
        }

        uint32_t chunk_payload_size = header_bytes + chunk_rows * row_bytes;
        int rc = dv_call_pl(DV_TEXTURE_UPDATE_REGION, (int)t, 0, 0,
                            buf, chunk_payload_size);
        if (rc != DV_OK) return rc;
    }

    return DV_OK;
}

/* ===== 2D primitives -- execution lives in the driver ===== */

int32_t dv_fill_rect(dv_surface_t dst, dv_rect_t r, dv_color_t c)
{
    dv_fill_rect_io_t io = { .r = r };
    return dv_call_pl(DV_FILL_RECT, (int)dst, (int)pack_color(c), 0,
                      &io, sizeof(io));
}

int32_t dv_blit(dv_surface_t src, dv_rect_t sr,
                dv_surface_t dst, dv_point_t dp)
{
    dv_blit_io_t io = { .sr = sr, .dp = dp, .src = src, .dst = dst };
    return dv_call_pl(DV_BLIT, 0, 0, 0, &io, sizeof(io));
}

int32_t dv_blit_alpha(dv_surface_t src, dv_rect_t sr,
                      dv_surface_t dst, dv_point_t dp, uint8_t alpha)
{
    dv_blit_io_t io = { .sr = sr, .dp = dp, .src = src, .dst = dst };
    return dv_call_pl(DV_BLIT_ALPHA, alpha, 0, 0, &io, sizeof(io));
}

int32_t dv_blit_pixel_alpha(dv_surface_t src, dv_rect_t sr,
                            dv_surface_t dst, dv_point_t dp)
{
    /* arg1 = use_pixel_alpha: 0xFF000000 sul pixel sorgente =
     * trasparente (convenzione BlitBuffer, come il ramo
     * use_pixel_alpha delle cmdlist). */
    dv_blit_io_t io = { .sr = sr, .dp = dp, .src = src, .dst = dst };
    return dv_call_pl(DV_BLIT_ALPHA, 0xFF, 1, 0, &io, sizeof(io));
}

int32_t dv_blit_stretched(dv_surface_t src, dv_rect_t sr,
                          dv_surface_t dst, dv_rect_t dr)
{
    dv_blit_stretched_io_t io = {
        .sr = sr, .dr = dr, .src = src, .dst = dst,
    };
    return dv_call_pl(DV_BLIT_STRETCHED, 0, 0, 0, &io, sizeof(io));
}

int32_t dv_fill_gradient(dv_surface_t dst, dv_rect_t r,
                         dv_color_t a, dv_color_t b, dv_gradient_dir_t dir)
{
    dv_fill_gradient_io_t io = {
        .r = r, .ca = a, .cb = b, .dir = (uint32_t)dir, .dst = dst,
    };
    return dv_call_pl(DV_FILL_GRADIENT, 0, 0, 0, &io, sizeof(io));
}

int32_t dv_scroll_region(dv_surface_t s, dv_rect_t r,
                         int32_t dx, int32_t dy, dv_color_t fill)
{
    dv_scroll_region_io_t io = {
        .r = r, .dx = dx, .dy = dy, .fill = fill, .dst = s,
    };
    return dv_call_pl(DV_SCROLL_REGION, 0, 0, 0, &io, sizeof(io));
}

int32_t dv_draw_line(dv_surface_t dst, dv_point_t a, dv_point_t b,
                     uint32_t thickness, dv_color_t c)
{
    dv_draw_line_io_t io = {
        .a = a, .b = b, .thickness = thickness, .c = c, .dst = dst,
    };
    return dv_call_pl(DV_DRAW_LINE, 0, 0, 0, &io, sizeof(io));
}

int32_t dv_draw_rect_outline(dv_surface_t dst, dv_rect_t r,
                             uint32_t thickness, dv_color_t c)
{
    dv_draw_rect_outline_io_t io = {
        .r = r, .thickness = thickness, .c = c, .dst = dst,
    };
    return dv_call_pl(DV_DRAW_RECT_OUTLINE, 0, 0, 0, &io, sizeof(io));
}

int32_t dv_draw_glyphs(dv_surface_t dst, dv_texture_t glyph_atlas,
                       const dv_glyph_t *glyphs, uint32_t count,
                       dv_color_t c)
{
    if (!glyphs || count == 0) return DV_ERR_INVAL;

    /* Header + variable-length glyph array, packed into a stack
     * buffer.  Stay under the 16 KiB boomerang cap; for typical UI
     * runs this fits comfortably (256 glyphs * 16 B ? 4 KiB). */
    uint32_t glyphs_bytes = count * sizeof(dv_glyph_t);
    if (sizeof(dv_draw_glyphs_io_t) + glyphs_bytes > 16384)
        return DV_ERR_INVAL;

    uint8_t buf[16384];
    dv_draw_glyphs_io_t *hdr = (dv_draw_glyphs_io_t *)buf;
    hdr->c     = c;
    hdr->dst   = dst;
    hdr->atlas = glyph_atlas;
    hdr->count = count;
    memcpy(buf + sizeof(*hdr), glyphs, glyphs_bytes);

    return dv_call_pl(DV_DRAW_GLYPHS, 0, 0, 0, buf,
                      (unsigned)(sizeof(*hdr) + glyphs_bytes));
}

/* ===== layer / compose ===== */

int32_t dv_layer_create(dv_vproc_t v, const dv_layer_desc_t *d,
                        dv_layer_t *out)
{
    if (!out || !d) return DV_ERR_INVAL;
    dv_layer_create_io_t io = { .desc = *d, .out = DV_HANDLE_NONE };
    int rc = dv_call_pl(DV_LAYER_CREATE, (int)v, 0, 0, &io, sizeof(io));
    if (rc == DV_OK) *out = io.out;
    return rc;
}

int32_t dv_layer_destroy(dv_layer_t l)
{
    return dv_call1(DV_LAYER_DESTROY, (int)l);
}

int32_t dv_layer_update(dv_layer_t l, const dv_layer_desc_t *d)
{
    if (!d) return DV_ERR_INVAL;
    /* Reuse same wire layout (no OUT field needed for update, but
     * the dispatcher reads only desc -- extra trailing bytes are
     * harmless). */
    dv_layer_create_io_t io = { .desc = *d, .out = DV_HANDLE_NONE };
    return dv_call_pl(DV_LAYER_UPDATE, (int)l, 0, 0, &io, sizeof(io));
}

int32_t dv_layer_set_visible(dv_layer_t l, bool visible)
{
    return dv_call2(DV_LAYER_SET_VISIBLE, (int)l, visible ? 1 : 0);
}

/* ===== present ===== */

int32_t dv_compose(uint32_t display_id, dv_fence_t fence_signal)
{
    return dv_call2(DV_COMPOSE, (int)display_id, (int)fence_signal);
}

/* dv_compose_rect — recompose only the dirty rectangle (rx,ry,rw,rh).
 * Coordinates and extents are <= screen dimensions (max 4096), so each
 * pair packs into one 32-bit word (hi<<16 | lo).  display_id rides in the
 * 4th scalar; it is 0 for the single display. */
int32_t dv_compose_rect(uint32_t display_id, int32_t rx, int32_t ry,
                        uint32_t rw, uint32_t rh, dv_fence_t fence_signal)
{
    int xy = (int)(((uint32_t)(rx & 0xFFFF) << 16) | ((uint32_t)ry & 0xFFFF));
    int wh = (int)(((rw & 0xFFFFu) << 16) | (rh & 0xFFFFu));
    return dv_call4(DV_COMPOSE_RECT, xy, wh,
                    (int)fence_signal, (int)display_id);
}

int32_t dv_page_flip(uint32_t display_id, uint32_t flags,
                     dv_fence_t fence_signal)
{
    return dv_call3(DV_PAGE_FLIP, (int)display_id, (int)flags,
                    (int)fence_signal);
}

/* ===== capability query ===== */
/* Returns the 64-bit capability mask in a small IO struct (the boomerang
 * copies it back).  Must byte-match the driver fast-dispatch case. */
typedef struct { uint32_t lo, hi; } dv_caps_io_t;

int32_t dv_cap_query(uint64_t *out_capabilities)
{
    if (!out_capabilities) return DV_ERR_INVAL;
    dv_caps_io_t io = { 0, 0 };
    int rc = dv_call_pl(DV_CAP_QUERY, 0, 0, 0, &io, sizeof(io));
    if (rc == DV_OK)
        *out_capabilities = ((uint64_t)io.hi << 32) | (uint64_t)io.lo;
    return rc;
}

int32_t dv_cap_query_limit(dv_limit_t which, uint64_t *out_value)
{
    if (!out_value) return DV_ERR_INVAL;
    dv_caps_io_t io = { 0, 0 };
    int rc = dv_call_pl(DV_CAP_QUERY_LIMIT, (int)which, 0, 0, &io, sizeof(io));
    if (rc == DV_OK)
        *out_value = ((uint64_t)io.hi << 32) | (uint64_t)io.lo;
    return rc;
}

int32_t dv_cap_query_format(dv_format_t fmt, uint32_t *out_usage_flags)
{
    if (!out_usage_flags) return DV_ERR_INVAL;
    uint32_t io = 0;
    int rc = dv_call_pl(DV_CAP_QUERY_FORMAT, (int)fmt, 0, 0, &io, sizeof(io));
    if (rc == DV_OK) *out_usage_flags = io;
    return rc;
}

/* ===== hardware cursor ===== */
/* set_bitmap carries the pixel payload after the descriptor; the driver
 * reads width/height/hotspot/format then the pixels.  We marshal them into
 * one buffer so the single boomerang payload contains everything. */
int32_t dv_cursor_set_bitmap(uint32_t display_id, const dv_cursor_desc_t *d)
{
    if (!d || !d->pixels) return DV_ERR_INVAL;
    if (d->width == 0u || d->height == 0u) return DV_ERR_INVAL;
    /* Wire layout: [dv_cursor_desc_t hdr][w*h*4 bytes pixels].  hdr.pixels
     * is ignored on the wire (driver uses the trailing bytes). */
    uint32_t px_bytes = d->width * d->height * 4u;
    uint32_t total = (uint32_t)sizeof(dv_cursor_desc_t) + px_bytes;
    if (total > 16u * 1024u) return DV_ERR_INVAL;     /* boomerang 16K cap */
    static uint8_t buf[16u * 1024u];
    dv_cursor_desc_t hdr = *d;
    hdr.pixels = 0;                                   /* not meaningful cross-AS */
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), d->pixels, px_bytes);
    return dv_call_pl(DV_CURSOR_SET_BITMAP, (int)display_id, 0, 0,
                      buf, (int)total);
}

int32_t dv_cursor_set_position(uint32_t display_id, int32_t x, int32_t y)
{
    return dv_call3(DV_CURSOR_SET_POSITION, (int)display_id, (int)x, (int)y);
}

int32_t dv_cursor_show(uint32_t display_id)
{
    return dv_call1(DV_CURSOR_SHOW, (int)display_id);
}

int32_t dv_cursor_hide(uint32_t display_id)
{
    return dv_call1(DV_CURSOR_HIDE, (int)display_id);
}

/* ===== cmdlist (retained-mode) ===== */

int32_t dv_cmdlist_create(dv_vproc_t v, uint32_t capacity_bytes,
                          dv_cmdlist_t *out)
{
    if (!out) return DV_ERR_INVAL;
    dv_cmdlist_t cl = DV_HANDLE_NONE;
    int rc = dv_call_pl(DV_CMDLIST_CREATE, (int)v, (int)capacity_bytes, 0,
                        &cl, sizeof(cl));
    if (rc == DV_OK) *out = cl;
    return rc;
}

int32_t dv_cmdlist_destroy(dv_cmdlist_t cl)
{
    return dv_call1(DV_CMDLIST_DESTROY, (int)cl);
}

int32_t dv_cmdlist_reset(dv_cmdlist_t cl)
{
    return dv_call1(DV_CMDLIST_RESET, (int)cl);
}

int32_t dv_cmdlist_fill_rect(dv_cmdlist_t cl, dv_rect_t r, dv_color_t c)
{
    return dv_call_pl(DV_CMDLIST_FILL_RECT, (int)cl, (int)pack_color(c), 0,
                      &r, sizeof(r));
}

int32_t dv_cmdlist_blit(dv_cmdlist_t cl, dv_surface_t src,
                        dv_rect_t sr, dv_point_t dp)
{
    struct { dv_surface_t src; dv_rect_t sr; dv_point_t dp; } io =
        { .src = src, .sr = sr, .dp = dp };
    return dv_call_pl(DV_CMDLIST_BLIT, (int)cl, 0, 0, &io, sizeof(io));
}

int32_t dv_cmdlist_blit_alpha(dv_cmdlist_t cl, dv_surface_t src,
                              dv_rect_t sr, dv_point_t dp, uint8_t alpha,
                              bool use_pixel_alpha)
{
    struct { dv_surface_t src; dv_rect_t sr; dv_point_t dp; } io =
        { .src = src, .sr = sr, .dp = dp };
    /* Pack alpha + use_pixel_alpha into a1 to save a payload field. */
    uint32_t packed = ((uint32_t)alpha << 8) | (use_pixel_alpha ? 1u : 0u);
    return dv_call_pl(DV_CMDLIST_BLIT_ALPHA, (int)cl, (int)packed, 0,
                      &io, sizeof(io));
}

int32_t dv_cmdlist_draw_glyphs(dv_cmdlist_t cl, dv_texture_t atlas,
                               const dv_glyph_t *glyphs, uint32_t count,
                               dv_color_t color)
{
    if (count > 0 && !glyphs) return DV_ERR_INVAL;

    /* Payload layout: [atlas:4][count:4][dv_glyph_t * count]. */
    uint32_t glyphs_bytes = count * (uint32_t)sizeof(dv_glyph_t);
    if (8 + glyphs_bytes > 16384) return DV_ERR_INVAL;

    uint8_t buf[16384];
    memcpy(buf,     &atlas, 4);
    memcpy(buf + 4, &count, 4);
    if (glyphs_bytes > 0)
        memcpy(buf + 8, glyphs, glyphs_bytes);

    return dv_call_pl(DV_CMDLIST_DRAW_GLYPHS, (int)cl, (int)pack_color(color), 0,
                      buf, 8 + glyphs_bytes);
}

int32_t dv_cmdlist_draw_line(dv_cmdlist_t cl, dv_point_t a, dv_point_t b,
                             uint32_t thickness, dv_color_t color)
{
    struct { dv_point_t a; dv_point_t b; } io = { .a = a, .b = b };
    return dv_call_pl(DV_CMDLIST_DRAW_LINE, (int)cl, (int)pack_color(color),
                      (int)thickness, &io, sizeof(io));
}

int32_t dv_cmdlist_info(dv_cmdlist_t cl, dv_cmdlist_info_t *out)
{
    if (!out) return DV_ERR_INVAL;
    return dv_call_pl(DV_CMDLIST_INFO, (int)cl, 0, 0, out, sizeof(*out));
}
