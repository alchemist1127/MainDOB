/* MainDOB BGA video driver — QEMU `-vga std` (Bochs Graphics Adapter).
 * PCI 1234:1111. Linear framebuffer at BAR0, legacy VBE registers at I/O
 * 0x01CE/0x01CF, no IRQ. Implements the dobVideo protocol as plain C functions
 * (<dob/video.h>); transport is split: bga_transport_fast.c + bga_fast_entry.asm
 * (int 0x85 boomerang, data-plane DV_* opcodes, hot path) and bga_transport_ipc.c
 * (IPC, control-plane DOBVC_*). VRAM from VBE_DISPI_INDEX_VIDEO_MEMORY_64K, BAR0
 * managed by a coalescing free-list; two scanout pages at the pool bottom, page
 * flip = one Y_OFFSET write; hot paths use rep stosl/movsl (write-combining FB).
 * Caps: VRAM_MAP, PAGE_FLIP, VSYNC, ACCELERATED_BLIT, ALPHA_BLEND; no 3D/compute/
 * shader/overlay/hw-cursor (return DV_ERR_NOSUPPORT). */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <dob/server.h>
#include <dob/registry.h>
#include <dob/ipc.h>
#include <dob/types.h>
#include <dob/thread.h>
#include <dob/hotplug_driver.h>
#include <dob/video.h>
#include <DobVideoControl.h>

#include "bga_state.h"
#include "dobvc_protocol.h"

/* ==========================================================================
 *  Branch hints + hot/cold attributes
 * ========================================================================== */
#ifndef likely
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#endif
#define HOT             __attribute__((hot))
#define COLD            __attribute__((cold))
#define ALWAYS_INLINE   __attribute__((always_inline)) inline

/* ==========================================================================
 *  BGA hardware
 * ========================================================================== */

#define BGA_INDEX_PORT          0x01CE
#define BGA_DATA_PORT           0x01CF

#define VBE_DISPI_INDEX_ID                  0x00
#define VBE_DISPI_INDEX_XRES                0x01
#define VBE_DISPI_INDEX_YRES                0x02
#define VBE_DISPI_INDEX_BPP                 0x03
#define VBE_DISPI_INDEX_ENABLE              0x04
#define VBE_DISPI_INDEX_VIRT_WIDTH          0x06
#define VBE_DISPI_INDEX_VIRT_HEIGHT         0x07
#define VBE_DISPI_INDEX_X_OFFSET            0x08
#define VBE_DISPI_INDEX_Y_OFFSET            0x09
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K    0x0A

#define VBE_DISPI_DISABLED                  0x00
#define VBE_DISPI_ENABLED                   0x01
#define VBE_DISPI_LFB_ENABLED               0x40

#define VBE_DISPI_ID5                       0xB0C5

/* Interruttore double buffering.
 *   1 = due pagine primary + page flip via Y_OFFSET (comportamento storico).
 *   0 = DEPENNATO per il lavoro "finestre a superfici": una sola pagina,
 *       "back" e front coincidono (primary[1] == primary[0]), il flip
 *       diventa un no-op visivo. La compose disegna nel buffer VISIBILE:
 *       i flash intermedi del modello retained diventano osservabili —
 *       e' il banco di prova del refactor (compose = soli blit di pixel
 *       finali = niente flash anche in single buffer). Libera meta' del
 *       budget primary (~3 MiB a 1024x768): cio' che serve agli
 *       adattatori da 8 MiB (Armada E500). Da rivalutare a refactor
 *       compiuto: il double buffering tornera' OPZIONALE, non obbligato. */
#define BGA_DOUBLE_BUFFERING                0

static ALWAYS_INLINE void bga_write(uint16_t idx, uint16_t val)
{
    io_outw(BGA_INDEX_PORT, idx);
    io_outw(BGA_DATA_PORT, val);
}

static ALWAYS_INLINE uint16_t bga_read(uint16_t idx)
{
    io_outw(BGA_INDEX_PORT, idx);
    return io_inw(BGA_DATA_PORT);
}

/* ==========================================================================
 *  Inline-asm fast paths.  The framebuffer is mapped write-combining
 *  on QEMU stdvga, so rep stosl / rep movsl burst-coalesce 4-byte
 *  stores into 32/64-byte transactions on the host.  Beats a hand
 *  unrolled C loop by 1.5-3x on real measurements.
 * ========================================================================== */

HOT ALWAYS_INLINE
static void fast_fill32(volatile uint32_t *dst, uint32_t color, uint32_t count)
{
    __asm__ volatile (
        "rep stosl"
        : "+D"(dst), "+c"(count)
        : "a"(color)
        : "memory"
    );
}

HOT ALWAYS_INLINE
static void fast_copy32(volatile uint32_t *dst, const volatile uint32_t *src,
                        uint32_t count)
{
    __asm__ volatile (
        "rep movsl"
        : "+D"(dst), "+S"(src), "+c"(count)
        :
        : "memory"
    );
}

/* ==========================================================================
 *  Driver state -- defined out-of-line in bga_state.h so that the
 *  transport file can also see the surface/vproc/buffer/layer tables
 *  if it ever needs to.
 * ========================================================================== */

bga_state_t   g_bga;            /* exported; see bga_state.h */

/* Convenience handles to fields of g_bga, kept short for readability. */
#define G              (&g_bga)

ALWAYS_INLINE
static volatile bgra_t *surface_pixels(const surface_t *s)
{
    if (s->sys_pixels != NULL)
        return (volatile bgra_t *)s->sys_pixels;
    return (volatile bgra_t *)((volatile uint8_t *)G->vram + s->vram_offset);
}

/* ==========================================================================
 *  VRAM allocator -- free-list with bidirectional coalescing.  Metadata
 *  in host RAM (no in-band headers in the framebuffer).
 * ========================================================================== */

#define VRAM_ALIGN              16
#define VRAM_MIN_BLOCK          64
#define VRAM_MAX_BLOCKS         512

static vram_block_t  g_block_pool[VRAM_MAX_BLOCKS];
static vram_block_t *g_block_free = NULL;

static void block_pool_init(void)
{
    for (uint32_t i = 0; i < VRAM_MAX_BLOCKS - 1; i++)
        g_block_pool[i].next = &g_block_pool[i + 1];
    g_block_pool[VRAM_MAX_BLOCKS - 1].next = NULL;
    g_block_free = &g_block_pool[0];
}

static vram_block_t *block_alloc_meta(void)
{
    if (!g_block_free) return NULL;
    vram_block_t *b = g_block_free;
    g_block_free = b->next;
    memset(b, 0, sizeof(*b));
    return b;
}

static void block_free_meta(vram_block_t *b)
{
    b->next = g_block_free;
    g_block_free = b;
}

static void vram_init_pool(uint32_t base_offset, uint32_t total_size)
{
    block_pool_init();
    vram_block_t *b = block_alloc_meta();
    b->offset = base_offset;
    b->size   = total_size;
    b->used   = false;
    b->prev = b->next = NULL;
    G->blocks_head = b;
}

HOT
static uint32_t vram_alloc(uint32_t size)
{
    if (size < VRAM_MIN_BLOCK) size = VRAM_MIN_BLOCK;
    size = (size + VRAM_ALIGN - 1) & ~(VRAM_ALIGN - 1);

    for (vram_block_t *b = G->blocks_head; b; b = b->next)
    {
        if (b->used || b->size < size) continue;
        if (b->size > size + VRAM_MIN_BLOCK)
        {
            vram_block_t *rem = block_alloc_meta();
            if (rem)
            {
                rem->offset = b->offset + size;
                rem->size   = b->size - size;
                rem->used   = false;
                rem->prev   = b;
                rem->next   = b->next;
                if (rem->next) rem->next->prev = rem;
                b->next = rem;
                b->size = size;
            }
        }
        b->used = true;
        return b->offset;
    }
    return UINT32_MAX;
}

/* Aligned variant used by allocations whose hardware-visible
 * semantics depend on an offset multiple larger than VRAM_ALIGN --
 * specifically the scanout primary pages, whose Y_OFFSET register
 * write divides offset_bytes by (mode.width * 4) to get the
 * scanline.  Unaligned primary_offset -> fractional scanline ->
 * vertically-shifted/torn display.  align is in bytes, power of two. */
HOT
static uint32_t vram_alloc_aligned(uint32_t size, uint32_t align)
{
    if (align <= VRAM_ALIGN || (align & (align - 1)) != 0)
        return vram_alloc(size);
    if (size < VRAM_MIN_BLOCK) size = VRAM_MIN_BLOCK;
    size = (size + VRAM_ALIGN - 1) & ~(VRAM_ALIGN - 1);

    for (vram_block_t *b = G->blocks_head; b; b = b->next)
    {
        if (b->used) continue;
        uint32_t a_off = (b->offset + align - 1) & ~(align - 1);
        if (a_off < b->offset) continue;                /* overflow */
        uint32_t pad = a_off - b->offset;
        if (pad + size > b->size) continue;

        /* If the block starts unaligned, split off the leading pad
         * into a separate free block so the used range begins at
         * a_off.  Requires one metadata slot -- if exhausted, skip
         * this block and try the next; the allocator never wastes
         * a misaligned allocation by silently returning a wrong
         * offset. */
        if (pad > 0)
        {
            vram_block_t *post = block_alloc_meta();
            if (!post) continue;
            post->offset = a_off;
            post->size   = b->size - pad;
            post->used   = false;
            post->prev   = b;
            post->next   = b->next;
            if (post->next) post->next->prev = post;
            b->next = post;
            b->size = pad;
            b = post;
        }
        /* Split tail if remainder is large enough to bother. */
        if (b->size > size + VRAM_MIN_BLOCK)
        {
            vram_block_t *rem = block_alloc_meta();
            if (rem)
            {
                rem->offset = b->offset + size;
                rem->size   = b->size - size;
                rem->used   = false;
                rem->prev   = b;
                rem->next   = b->next;
                if (rem->next) rem->next->prev = rem;
                b->next = rem;
                b->size = size;
            }
        }
        b->used = true;
        return b->offset;
    }
    return UINT32_MAX;
}

static void vram_free_at(uint32_t offset)
{
    vram_block_t *b;
    for (b = G->blocks_head; b; b = b->next)
        if (b->offset == offset && b->used) break;
    if (!b) return;
    b->used = false;
    if (b->next && !b->next->used)
    {
        vram_block_t *n = b->next;
        b->size += n->size;
        b->next = n->next;
        if (b->next) b->next->prev = b;
        block_free_meta(n);
    }
    if (b->prev && !b->prev->used)
    {
        vram_block_t *p = b->prev;
        p->size += b->size;
        p->next = b->next;
        if (p->next) p->next->prev = p;
        block_free_meta(b);
    }
}

static uint64_t vram_total_free(void)
{
    uint64_t f = 0;
    for (vram_block_t *b = G->blocks_head; b; b = b->next)
        if (!b->used) f += b->size;
    return f;
}

static uint64_t vram_largest_free(void)
{
    uint64_t l = 0;
    for (vram_block_t *b = G->blocks_head; b; b = b->next)
        if (!b->used && b->size > l) l = b->size;
    return l;
}

/* ==========================================================================
 *  Slot allocators
 * ========================================================================== */

#define HANDLE_TO_SLOT(h)       ((uint32_t)(h) - 1)
#define SLOT_TO_HANDLE(s)       ((uint32_t)(s) + 1)

#define DEFINE_SLOT_ALLOC(NAME, FIELD, MAX) \
    static int alloc_##NAME##_slot(void) { \
        for (uint32_t i = 0; i < (MAX); i++) \
            if (!G->FIELD[i].used) { G->FIELD[i].used = true; return (int)i; } \
        return -1; \
    }

DEFINE_SLOT_ALLOC(vproc,    vprocs,    MAX_VPROCS)
DEFINE_SLOT_ALLOC(surface,  surfaces,  MAX_SURFACES)
DEFINE_SLOT_ALLOC(buffer,   buffers,   MAX_BUFFERS)
DEFINE_SLOT_ALLOC(fence,    fences,    MAX_FENCES)
DEFINE_SLOT_ALLOC(layer,    layers,    MAX_LAYERS)
DEFINE_SLOT_ALLOC(vthread,  vthreads,  MAX_VTHREADS)
DEFINE_SLOT_ALLOC(cmdlist,  cmdlists,  MAX_CMDLISTS)

static vproc_t *vproc_lookup(dv_vproc_t h)
{
    if (h == DV_HANDLE_NONE) return NULL;
    uint32_t s = HANDLE_TO_SLOT(h);
    if (s >= MAX_VPROCS || !G->vprocs[s].used) return NULL;
    return &G->vprocs[s];
}
static surface_t *surface_lookup(dv_surface_t h)
{
    if (h == DV_HANDLE_NONE) return NULL;
    uint32_t s = HANDLE_TO_SLOT(h);
    if (s >= MAX_SURFACES || !G->surfaces[s].used) return NULL;
    return &G->surfaces[s];
}
static buffer_t *buffer_lookup(dv_buffer_t h)
{
    if (h == DV_HANDLE_NONE) return NULL;
    uint32_t s = HANDLE_TO_SLOT(h);
    if (s >= MAX_BUFFERS || !G->buffers[s].used) return NULL;
    return &G->buffers[s];
}
static fence_t *fence_lookup(dv_fence_t h)
{
    if (h == DV_HANDLE_NONE) return NULL;
    uint32_t s = HANDLE_TO_SLOT(h);
    if (s >= MAX_FENCES || !G->fences[s].used) return NULL;
    return &G->fences[s];
}
static layer_t *layer_lookup(dv_layer_t h)
{
    if (h == DV_HANDLE_NONE) return NULL;
    uint32_t s = HANDLE_TO_SLOT(h);
    if (s >= MAX_LAYERS || !G->layers[s].used) return NULL;
    return &G->layers[s];
}
static cmdlist_t *cmdlist_lookup(dv_cmdlist_t h)
{
    if (h == DV_HANDLE_NONE) return NULL;
    uint32_t s = HANDLE_TO_SLOT(h);
    if (s >= MAX_CMDLISTS || !G->cmdlists[s].used) return NULL;
    return &G->cmdlists[s];
}

/* Forward decls for cmdlist replay machinery -- defined further down. */
static void cmdlist_replay(cmdlist_t *cl, surface_t *dst, int ox, int oy);

/* ==========================================================================
 *  Internal: BGA mode set + scanout primary pages
 * ========================================================================== */

static void primaries_free(void)
{
    if (G->shadow != NULL)
    {
        free(G->shadow);
        G->shadow = NULL;
    }
    if (G->primary_offset[0] != 0 || G->primary_bytes != 0)
    {
        vram_free_at(G->primary_offset[0]);
        if (G->primary_offset[1] != G->primary_offset[0])
            vram_free_at(G->primary_offset[1]);
        G->primary_offset[0] = G->primary_offset[1] = 0;
        G->primary_bytes = 0;
    }
}

static int primaries_alloc(uint32_t w, uint32_t h)
{
    uint32_t bytes = w * h * 4;
    /* Y_OFFSET in scanlines is computed as offset_bytes / (w * 4) on
     * scanout_program_back.  That divisor is the alignment we need on
     * each primary's offset, or the second page lands on a fractional
     * scanline and the display shows a vertical shift / tear band. */
    uint32_t align = w * 4;
    uint32_t a = vram_alloc_aligned(bytes, align);
#if BGA_DOUBLE_BUFFERING
    uint32_t b = (a == UINT32_MAX) ? UINT32_MAX
                                   : vram_alloc_aligned(bytes, align);
#else
    uint32_t b = a;                 /* single buffer: back == front     */
#endif
    if (a == UINT32_MAX || b == UINT32_MAX)
    {
        if (a != UINT32_MAX) vram_free_at(a);
        return DV_ERR_OOM_VRAM;
    }
    G->primary_offset[0] = a;
    G->primary_offset[1] = b;
    G->primary_bytes     = bytes;
    return DV_OK;
}

COLD
static int bga_apply_mode(uint32_t w, uint32_t h, uint32_t bpp)
{
    if (bpp != 32) return DV_ERR_NOSUPPORT;
    if (w == 0 || h == 0 || w > 2560 || h > 1600) return DV_ERR_INVAL;

    primaries_free();
    int rc = primaries_alloc(w, h);
    if (rc != DV_OK) return rc;

    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bga_write(VBE_DISPI_INDEX_XRES,        (uint16_t)w);
    bga_write(VBE_DISPI_INDEX_YRES,        (uint16_t)h);
    bga_write(VBE_DISPI_INDEX_BPP,         (uint16_t)bpp);
    bga_write(VBE_DISPI_INDEX_VIRT_WIDTH,  (uint16_t)w);
#if BGA_DOUBLE_BUFFERING
    bga_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)(h * 2));
#else
    bga_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)h);

    /* Shadow alle dimensioni del modo (liberata da primaries_free al
     * prossimo cambio). Su fallimento resta NULL: compose diretta nel
     * primary — corretta ma con gli strati intermedi visibili. */
    G->shadow = malloc((size_t)w * h * 4u);
    if (G->shadow != NULL)
        fast_fill32((volatile uint32_t *)G->shadow, 0, w * h);
    else
        debug_print("[bga] shadow malloc fallita: compose diretta\n");
#endif
    bga_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    bga_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    bga_write(VBE_DISPI_INDEX_ENABLE,
              VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    G->mode.width      = w;
    G->mode.height     = h;
    G->mode.refresh_hz = 60;
    G->mode.format     = DV_FMT_BGRA8888;
    G->back_page = 1;

    fast_fill32((volatile uint32_t *)((volatile uint8_t *)G->vram + G->primary_offset[0]),
                0, w * h);
    fast_fill32((volatile uint32_t *)((volatile uint8_t *)G->vram + G->primary_offset[1]),
                0, w * h);

    /* Registra il framebuffer col kernel per il disegno del panic: indirizzo
     * FISICO della primary + geometria (32bpp, pitch = w*4). Cosi' un kernel
     * panic con la GUI attiva scrive il testo direttamente in questi pixel,
     * senza cambiare modo video. */
    (void)syscall5(SYS_SET_PANIC_FB,
                   (int)(G->vram_phys + G->primary_offset[0]),
                   (int)w, (int)h, (int)(w * 4u), 32);

    return DV_OK;
}

HOT
static void scanout_program_back(void)
{
    uint32_t off_bytes = G->primary_offset[G->back_page ^ 1];   /* now front */
    uint32_t y = off_bytes / (G->mode.width * 4);
    bga_write(VBE_DISPI_INDEX_Y_OFFSET, (uint16_t)y);
}

/* ==========================================================================
 *  Internal: software 2D primitives operating on surface_t.
 *  Used by the protocol functions below.
 * ========================================================================== */

HOT
static void sw_fill_rect(surface_t *s, dv_rect_t r, dv_color_t c)
{
    int32_t x0 = r.x < 0 ? 0 : r.x;
    int32_t y0 = r.y < 0 ? 0 : r.y;
    int32_t x1 = (int32_t)(r.x + r.w);
    int32_t y1 = (int32_t)(r.y + r.h);
    if (x1 > (int32_t)s->width)  x1 = s->width;
    if (y1 > (int32_t)s->height) y1 = s->height;
    if (unlikely(x0 >= x1 || y0 >= y1)) return;

    uint32_t pix = ((c.a & 0xFF) << 24) | ((c.r & 0xFF) << 16)
                 | ((c.g & 0xFF) << 8)  |  (c.b & 0xFF);
    uint32_t row_count = (uint32_t)(x1 - x0);
    volatile uint32_t *base = (volatile uint32_t *)surface_pixels(s);
    uint32_t pitch_w = s->pitch_words;

    for (int32_t y = y0; y < y1; y++)
        fast_fill32(base + (uint32_t)y * pitch_w + (uint32_t)x0, pix, row_count);
}

HOT
static void sw_blit(surface_t *src, dv_rect_t sr, surface_t *dst, dv_point_t dp)
{
    int32_t sx0 = sr.x, sy0 = sr.y;
    int32_t sx1 = sr.x + (int32_t)sr.w, sy1 = sr.y + (int32_t)sr.h;
    if (sx0 < 0) { dp.x -= sx0; sx0 = 0; }
    if (sy0 < 0) { dp.y -= sy0; sy0 = 0; }
    if (sx1 > (int32_t)src->width)  sx1 = src->width;
    if (sy1 > (int32_t)src->height) sy1 = src->height;
    int32_t dx0 = dp.x, dy0 = dp.y;
    if (dx0 < 0) { sx0 -= dx0; dx0 = 0; }
    if (dy0 < 0) { sy0 -= dy0; dy0 = 0; }
    int32_t w = sx1 - sx0;
    int32_t h = sy1 - sy0;
    if (dx0 + w > (int32_t)dst->width)  w = dst->width  - dx0;
    if (dy0 + h > (int32_t)dst->height) h = dst->height - dy0;
    if (unlikely(w <= 0 || h <= 0)) return;

    volatile uint32_t *src_base = (volatile uint32_t *)surface_pixels(src);
    volatile uint32_t *dst_base = (volatile uint32_t *)surface_pixels(dst);
    uint32_t src_pitch_w = src->pitch_words;
    uint32_t dst_pitch_w = dst->pitch_words;

    for (int32_t y = 0; y < h; y++)
        fast_copy32(dst_base + (uint32_t)(dy0 + y) * dst_pitch_w + (uint32_t)dx0,
                    src_base + (uint32_t)(sy0 + y) * src_pitch_w + (uint32_t)sx0,
                    (uint32_t)w);
}

HOT
/* Pixel-alpha blit: source pixels with alpha byte == 0 are skipped
 * (treated as fully transparent / out of coverage); other pixels
 * are written at full opacity (no blend with destination).  Used
 * by layers that flag use_pixel_alpha -- the cursor in particular,
 * which has a small bitmap with crisp edges and a transparent
 * background.  Not a full alpha blend (no fractional coverage); for
 * anti-aliased sources use a future sw_blit_premultiplied_alpha. */
HOT
static void sw_blit_pixel_alpha(surface_t *src, dv_rect_t sr,
                                surface_t *dst, dv_point_t dp)
{
    int32_t sx0 = sr.x, sy0 = sr.y;
    int32_t sx1 = sr.x + (int32_t)sr.w, sy1 = sr.y + (int32_t)sr.h;
    if (sx0 < 0) { dp.x -= sx0; sx0 = 0; }
    if (sy0 < 0) { dp.y -= sy0; sy0 = 0; }
    if (sx1 > (int32_t)src->width)  sx1 = src->width;
    if (sy1 > (int32_t)src->height) sy1 = src->height;
    int32_t dx0 = dp.x, dy0 = dp.y;
    if (dx0 < 0) { sx0 -= dx0; dx0 = 0; }
    if (dy0 < 0) { sy0 -= dy0; dy0 = 0; }
    int32_t w = sx1 - sx0;
    int32_t h = sy1 - sy0;
    if (dx0 + w > (int32_t)dst->width)  w = dst->width  - dx0;
    if (dy0 + h > (int32_t)dst->height) h = dst->height - dy0;
    if (unlikely(w <= 0 || h <= 0)) return;

    volatile uint32_t *src_base = (volatile uint32_t *)surface_pixels(src);
    volatile uint32_t *dst_base = (volatile uint32_t *)surface_pixels(dst);
    uint32_t src_pitch_w = src->pitch_words;
    uint32_t dst_pitch_w = dst->pitch_words;

    for (int32_t y = 0; y < h; y++)
    {
        const volatile uint32_t *srow = src_base
            + (uint32_t)(sy0 + y) * src_pitch_w + (uint32_t)sx0;
        volatile uint32_t       *drow = dst_base
            + (uint32_t)(dy0 + y) * dst_pitch_w + (uint32_t)dx0;
        for (int32_t x = 0; x < w; x++)
        {
            uint32_t s_p = srow[x];
            if ((s_p & 0xFF000000u) != 0)
                drow[x] = s_p;
        }
    }
}

static void sw_blit_alpha(surface_t *src, dv_rect_t sr,
                          surface_t *dst, dv_point_t dp, uint8_t alpha)
{
    int32_t sx0 = sr.x, sy0 = sr.y;
    int32_t sx1 = sr.x + (int32_t)sr.w, sy1 = sr.y + (int32_t)sr.h;
    if (sx0 < 0) { dp.x -= sx0; sx0 = 0; }
    if (sy0 < 0) { dp.y -= sy0; sy0 = 0; }
    if (sx1 > (int32_t)src->width)  sx1 = src->width;
    if (sy1 > (int32_t)src->height) sy1 = src->height;
    int32_t dx0 = dp.x, dy0 = dp.y;
    if (dx0 < 0) { sx0 -= dx0; dx0 = 0; }
    if (dy0 < 0) { sy0 -= dy0; dy0 = 0; }
    int32_t w = sx1 - sx0;
    int32_t h = sy1 - sy0;
    if (dx0 + w > (int32_t)dst->width)  w = dst->width  - dx0;
    if (dy0 + h > (int32_t)dst->height) h = dst->height - dy0;
    if (unlikely(w <= 0 || h <= 0)) return;
    if (alpha == 255) { sw_blit(src, sr, dst, dp); return; }
    if (unlikely(alpha == 0)) return;

    volatile uint32_t *src_base = (volatile uint32_t *)surface_pixels(src);
    volatile uint32_t *dst_base = (volatile uint32_t *)surface_pixels(dst);
    uint32_t src_pitch_w = src->pitch_words;
    uint32_t dst_pitch_w = dst->pitch_words;
    uint32_t a = alpha, ia = 256 - alpha;

    for (int32_t y = 0; y < h; y++)
    {
        const volatile uint32_t *srow = src_base + (uint32_t)(sy0 + y) * src_pitch_w + (uint32_t)sx0;
        volatile uint32_t       *drow = dst_base + (uint32_t)(dy0 + y) * dst_pitch_w + (uint32_t)dx0;
        for (int32_t x = 0; x < w; x++)
        {
            uint32_t s_p = srow[x], d_p = drow[x];
            uint32_t br = ((s_p & 0x00FF00FFu) * a + (d_p & 0x00FF00FFu) * ia) >> 8;
            uint32_t ga = (((s_p >> 8) & 0x00FF00FFu) * a + ((d_p >> 8) & 0x00FF00FFu) * ia) >> 8;
            drow[x] = (br & 0x00FF00FFu) | ((ga & 0x00FF00FFu) << 8);
        }
    }
}

/* ==========================================================================
 *  Internal: subscriber notification (control-plane events).
 * ========================================================================== */

void bga_notify_subscribers(uint32_t code, uint32_t display_id,
                            uint32_t arg0, uint32_t arg1)
{
    for (uint32_t i = 0; i < MAX_SUBSCRIBERS; i++)
    {
        if (!G->subs[i].used) continue;
        if (!(G->subs[i].mask & code)) continue;
        dobvc_event_t ev = { .code = code, .display_id = display_id,
                             .arg0 = arg0, .arg1 = arg1 };
        dob_msg_t msg = { 0 };
        msg.code = code;
        msg.payload = &ev;
        msg.payload_size = sizeof(ev);
        dob_ipc_post(G->subs[i].port, &msg);
    }
}

/* Currently-attached pid filling caller-context fields.  The IPC
 * transport sets this around each call so vproc_attach can read it. */
pid_t bga_current_caller_pid = 0;

/* ===== dobVideo protocol implementation (signatures match <dob/video.h>) ===== */

/* ---------- vprocess / vthread ---------- */

int32_t dv_vproc_attach(const dv_vproc_attach_desc_t *desc, dv_vproc_t *out)
{
    if (!out) return DV_ERR_INVAL;
    int slot = alloc_vproc_slot();
    if (slot < 0) return DV_ERR_NOMEM;
    G->vprocs[slot].owner_pid        = bga_current_caller_pid;
    G->vprocs[slot].vram_quota_bytes = G->vram_bytes / 4;
    G->vprocs[slot].vram_used_bytes  = 0;
    G->vprocs[slot].vthreads_active  = 0;
    G->vprocs[slot].fences_in_flight = 0;
    if (desc && desc->vram_quota_bytes > 0 && desc->vram_quota_bytes <= G->vram_bytes)
        G->vprocs[slot].vram_quota_bytes = desc->vram_quota_bytes;
    *out = (dv_vproc_t)SLOT_TO_HANDLE(slot);
    return DV_OK;
}

int32_t dv_vproc_detach(dv_vproc_t v)
{
    vproc_t *p = vproc_lookup(v);
    if (!p) return DV_ERR_HANDLE;
    for (uint32_t i = 0; i < MAX_SURFACES; i++)
        if (G->surfaces[i].used && G->surfaces[i].owner == v)
        {
            if (G->surfaces[i].sys_pixels != NULL)
            {
                free(G->surfaces[i].sys_pixels);
                G->surfaces[i].sys_pixels = NULL;
            }
            else
            {
                vram_free_at(G->surfaces[i].vram_offset);
            }
            G->surfaces[i].used = false;
        }
    for (uint32_t i = 0; i < MAX_BUFFERS; i++)
        if (G->buffers[i].used && G->buffers[i].owner == v)
        {
            vram_free_at(G->buffers[i].vram_offset);
            G->buffers[i].used = false;
        }
    for (uint32_t i = 0; i < MAX_FENCES; i++)
        if (G->fences[i].used && G->fences[i].owner == v) G->fences[i].used = false;
    for (uint32_t i = 0; i < MAX_LAYERS; i++)
        if (G->layers[i].used && G->layers[i].owner == v) G->layers[i].used = false;
    for (uint32_t i = 0; i < MAX_VTHREADS; i++)
        if (G->vthreads[i].used && G->vthreads[i].owner == v) G->vthreads[i].used = false;

    if (G->scanout_source != DV_HANDLE_NONE)
    {
        surface_t *s = surface_lookup(G->scanout_source);
        if (!s || s->owner == v) G->scanout_source = DV_HANDLE_NONE;
    }
    p->used = false;
    return DV_OK;
}

int32_t dv_vproc_info(dv_vproc_t v, dv_vproc_info_t *out)
{
    vproc_t *p = vproc_lookup(v);
    if (!p || !out) return DV_ERR_HANDLE;
    out->vram_used_bytes  = p->vram_used_bytes;
    out->vram_quota_bytes = p->vram_quota_bytes;
    out->vthreads_active  = p->vthreads_active;
    out->vcores_in_use    = 1;
    out->fences_in_flight = p->fences_in_flight;
    out->flags            = 0;
    return DV_OK;
}

int32_t dv_vproc_set_quota(dv_vproc_t v, uint64_t new_quota_bytes)
{
    vproc_t *p = vproc_lookup(v);
    if (!p) return DV_ERR_HANDLE;
    if (new_quota_bytes > G->vram_bytes) return DV_ERR_INVAL;
    if (new_quota_bytes < p->vram_used_bytes) return DV_ERR_QUOTA;
    p->vram_quota_bytes = new_quota_bytes;
    return DV_OK;
}

int32_t dv_vthread_create(dv_vproc_t v, const dv_vthread_desc_t *d, dv_vthread_t *out)
{
    vproc_t *p = vproc_lookup(v);
    if (!p || !out) return DV_ERR_HANDLE;
    int slot = alloc_vthread_slot();
    if (slot < 0) return DV_ERR_NOMEM;
    G->vthreads[slot].owner    = v;
    G->vthreads[slot].priority = d ? d->priority : 128;
    p->vthreads_active++;
    *out = (dv_vthread_t)SLOT_TO_HANDLE(slot);
    return DV_OK;
}

int32_t dv_vthread_destroy(dv_vthread_t t)
{
    if (t == DV_HANDLE_NONE) return DV_ERR_HANDLE;
    uint32_t s = HANDLE_TO_SLOT(t);
    if (s >= MAX_VTHREADS || !G->vthreads[s].used) return DV_ERR_HANDLE;
    vproc_t *p = vproc_lookup(G->vthreads[s].owner);
    if (p && p->vthreads_active > 0) p->vthreads_active--;
    G->vthreads[s].used = false;
    return DV_OK;
}

int32_t dv_vthread_priority(dv_vthread_t t, uint8_t prio)
{
    if (t == DV_HANDLE_NONE) return DV_ERR_HANDLE;
    uint32_t s = HANDLE_TO_SLOT(t);
    if (s >= MAX_VTHREADS || !G->vthreads[s].used) return DV_ERR_HANDLE;
    G->vthreads[s].priority = prio;
    return DV_OK;
}

int32_t dv_vthread_yield(dv_vthread_t t)        { (void)t; return DV_OK; }
int32_t dv_vthread_join(dv_vthread_t t, uint32_t timeout_ms)
                                                { (void)t; (void)timeout_ms; return DV_OK; }
int32_t dv_vthread_wait_idle(dv_vthread_t t, uint32_t timeout_ms)
                                                { (void)t; (void)timeout_ms; return DV_OK; }

/* ---------- vcore ---------- */

int32_t dv_vcore_count(uint32_t *out_count)
{
    if (!out_count) return DV_ERR_INVAL;
    *out_count = 1;     /* MainDOB single-CPU; one fixed-2D vcore */
    return DV_OK;
}

int32_t dv_vcore_info(uint32_t index, dv_vcore_info_t *out)
{
    if (!out || index != 0) return DV_ERR_RANGE;
    out->index                 = 0;
    out->kind                  = DV_VCORE_FIXED_2D;
    out->register_file_bytes   = 0;
    out->shared_mem_bytes      = 0;
    out->max_threads_per_block = 1;
    out->warp_or_wave_size     = 1;
    out->capabilities          = 0;
    return DV_OK;
}

int32_t dv_vcore_params_set(dv_vthread_t t, const dv_vcore_params_t *p)
                                                { (void)t; (void)p; return DV_OK; }
int32_t dv_vcore_dispatch(dv_vthread_t t, const dv_dispatch_t *d)
                                                { (void)t; (void)d; return DV_ERR_NOSUPPORT; }
int32_t dv_vcore_affinity(dv_vthread_t t, int32_t vcore_index)
{
    (void)t;
    if (vcore_index != -1 && vcore_index != 0) return DV_ERR_RANGE;
    return DV_OK;
}
int32_t dv_vcore_query_state(uint32_t vcore_index, uint32_t *out_busy_pct)
{
    if (vcore_index != 0 || !out_busy_pct) return DV_ERR_RANGE;
    *out_busy_pct = 0;      /* no real load tracking yet */
    return DV_OK;
}

/* ---------- display and mode (read-only data plane) ---------- */

int32_t dv_mode_list(uint32_t display_id, dv_mode_t *out, uint32_t *count)
{
    if (display_id != 0 || !count) return DV_ERR_INVAL;
    uint32_t cap = *count;
    uint32_t n = G->mode_list_n < cap ? G->mode_list_n : cap;
    if (out) memcpy(out, G->mode_list_buf, n * sizeof(dv_mode_t));
    *count = n;
    return DV_OK;
}

int32_t dv_mode_get_current(uint32_t display_id, dv_mode_t *out)
{
    if (display_id != 0 || !out) return DV_ERR_INVAL;
    *out = G->mode;
    return DV_OK;
}

int32_t dv_display_count(uint32_t *out_count)
{
    if (!out_count) return DV_ERR_INVAL;
    *out_count = 1;
    return DV_OK;
}

int32_t dv_display_info(uint32_t display_id, dv_display_info_t *out)
{
    if (display_id != 0 || !out) return DV_ERR_INVAL;
    out->display_id    = 0;
    out->physical_w_mm = 0;
    out->physical_h_mm = 0;
    out->connected     = 1;
    out->edid_present  = 0;
    strcpy(out->name, "QEMU Bochs Display");
    return DV_OK;
}

/* VGA Input Status Register 1 — port 0x3DA. Bit 3 (0x08) = vertical retrace:
 * 0 = scanout in progress, 1 = vblank (safe to swap the displayed page without
 * tearing). Reading 0x3DA also clears the attribute-controller flip-flop, which
 * is harmless here. QEMU's stdvga honors it like real VGA (bit 3 high for a few
 * hundred µs each refresh; ~16.6 ms window at 60 Hz). */
#define VGA_INPUT_STATUS_1   0x3DA
#define VGA_VRETRACE_BIT     0x08

/* Spin until vertical-retrace polarity flips from 0 -> 1 -- the
 * scanline machine has just rolled off the bottom and is about to
 * start a new frame from the top.  Bounded by a generous timeout --
 * if for some reason the register is stuck (paravirt failure, bus
 * stall, etc.) we proceed without vsync rather than wedge the
 * boomerang forever.  Each polling iteration is ~1 us of inb, so
 * the timeout corresponds to ~30 ms of wallclock -- well over a
 * full 60 Hz frame, which is the longest legitimate wait. */
HOT ALWAYS_INLINE
static bool wait_for_vblank(void)
{
    /* Drain any in-progress vblank so we wait for the NEXT leading
     * edge, not the trailing one of a vblank we entered into.  Budget
     * sized to ~3 frames at 60 Hz (~50 ms wallclock on QEMU stdvga
     * where each io_inb is ~1 us).  Generous enough to absorb host
     * scheduling delays, tight enough that a stuck-vsync host (QEMU
     * with emulated retrace not advancing) doesn't lock the boomerang
     * for a full second.  Caller decides what to do on timeout --
     * dv_page_flip falls through to swap anyway (tear preferable to
     * freeze on stuck-retrace hosts). */
    uint32_t spins;
    for (spins = 60000; spins > 0; spins--)
        if (!(io_inb(VGA_INPUT_STATUS_1) & VGA_VRETRACE_BIT)) break;
    if (spins == 0) return false;

    for (spins = 60000; spins > 0; spins--)
        if (io_inb(VGA_INPUT_STATUS_1) & VGA_VRETRACE_BIT) return true;
    return false;
}

int32_t dv_vsync_wait(uint32_t display_id, uint32_t timeout_ms)
{
    /* Implemented via VGA Input Status Register 1 polling -- same
     * mechanism dv_page_flip uses internally for DV_FLIP_VSYNC.
     * `timeout_ms` caps the wait at roughly that many milliseconds
     * by translating to a polling-iteration budget (each io_inb is
     * ~1 us on QEMU stdvga, so ms x 1000 iterations).  At 60 Hz a
     * vblank arrives every 16.6 ms; timeout_ms < 17 may legitimately
     * miss the next vblank -- caller's choice. */
    if (display_id != 0) return DV_ERR_INVAL;

    uint32_t budget = (timeout_ms == 0) ? 30000u : timeout_ms * 1000u;

    /* Drain any in-progress vblank so we wait for the NEXT leading
     * edge, not the trailing one of a vblank we entered into. */
    uint32_t s;
    for (s = budget; s > 0; s--)
        if (!(io_inb(VGA_INPUT_STATUS_1) & VGA_VRETRACE_BIT)) break;
    if (s == 0) return DV_ERR_NOTREADY;

    for (s = budget; s > 0; s--)
        if (io_inb(VGA_INPUT_STATUS_1) & VGA_VRETRACE_BIT) return DV_OK;
    return DV_ERR_NOTREADY;
}

int32_t dv_gamma_get(uint32_t display_id, dv_gamma_ramp_t *out)
{
    (void)display_id; (void)out;
    return DV_ERR_NOSUPPORT;     /* BGA in 32 bpp has no gamma ramp */
}

int32_t dv_palette_set(dv_surface_t s, const uint32_t *palette_argb, uint32_t count)
{
    (void)s; (void)palette_argb; (void)count;
    return DV_ERR_NOSUPPORT;     /* no PAL8 surfaces */
}

/* ---------- VRAM ---------- */

int32_t dv_vram_alloc(dv_vproc_t v, dv_vram_kind_t kind, uint64_t bytes,
                      uint32_t flags, dv_buffer_t *out)
{
    (void)kind; (void)flags;
    vproc_t *p = vproc_lookup(v);
    if (!p || !out) return DV_ERR_HANDLE;
    if (bytes == 0 || bytes > UINT32_MAX) return DV_ERR_INVAL;
    if (p->vram_used_bytes + bytes > p->vram_quota_bytes) return DV_ERR_QUOTA;
    uint32_t off = vram_alloc((uint32_t)bytes);
    if (off == UINT32_MAX) return DV_ERR_OOM_VRAM;
    int slot = alloc_buffer_slot();
    if (slot < 0) { vram_free_at(off); return DV_ERR_NOMEM; }
    G->buffers[slot].owner       = v;
    G->buffers[slot].bytes       = bytes;
    G->buffers[slot].vram_offset = off;
    p->vram_used_bytes += bytes;
    *out = (dv_buffer_t)SLOT_TO_HANDLE(slot);
    return DV_OK;
}

int32_t dv_vram_free(dv_buffer_t b)
{
    buffer_t *bb = buffer_lookup(b);
    if (!bb) return DV_ERR_HANDLE;
    vproc_t *p = vproc_lookup(bb->owner);
    if (p) p->vram_used_bytes -= bb->bytes;
    vram_free_at(bb->vram_offset);
    bb->used = false;
    return DV_OK;
}

int32_t dv_vram_info(dv_vproc_t v, dv_vram_info_t *out)
{
    (void)v;
    if (!out) return DV_ERR_INVAL;
    out->total_bytes          = G->vram_bytes;
    out->free_bytes           = vram_total_free();
    out->largest_contig_bytes = vram_largest_free();
    /* VRAM on BGA fits in 32 bits (max 256 MB per VBE spec).  Stay
     * 32-bit to dodge __udivdi3 -- MainDOB doesn't link libgcc. */
    uint32_t free32    = (uint32_t)out->free_bytes;
    uint32_t largest32 = (uint32_t)out->largest_contig_bytes;
    out->fragmentation_pct = free32 ? 100 - (100u * largest32 / free32) : 0;

    /* Cmdlist pool: BSS reserved to back per-window dv_cmdlist
     * storage; grows on window create, shrinks on close. */
    out->cmdlist_pool_total_bytes = (uint32_t)CMDLIST_STORAGE_BYTES;
    out->cmdlist_pool_used_bytes  = G->cmdlist_storage_used;
    uint32_t cl_count = 0;
    for (uint32_t i = 0; i < MAX_CMDLISTS; i++)
        if (G->cmdlists[i].used) cl_count++;
    out->cmdlist_count = cl_count;
    return DV_OK;
}

int32_t dv_vram_map(dv_buffer_t b, void **out_vaddr)
{
    (void)b; (void)out_vaddr;
    return DV_ERR_NOSUPPORT;     /* needs cross-process BAR sharing; future */
}

int32_t dv_vram_unmap(dv_buffer_t b) { (void)b; return DV_ERR_NOSUPPORT; }

int32_t dv_vram_copy(const dv_vram_copy_t *op)
{
    if (!op) return DV_ERR_INVAL;
    buffer_t *src = buffer_lookup(op->src);
    buffer_t *dst = buffer_lookup(op->dst);
    if (!src || !dst) return DV_ERR_HANDLE;
    if (op->src_offset + op->bytes > src->bytes ||
        op->dst_offset + op->bytes > dst->bytes) return DV_ERR_INVAL;

    volatile uint8_t *s = (volatile uint8_t *)G->vram + src->vram_offset + op->src_offset;
    volatile uint8_t *d = (volatile uint8_t *)G->vram + dst->vram_offset + op->dst_offset;
    if ((((uintptr_t)s | (uintptr_t)d | op->bytes) & 3u) == 0)
        fast_copy32((volatile uint32_t *)d, (const volatile uint32_t *)s, op->bytes / 4);
    else
        memcpy((void *)d, (const void *)s, op->bytes);

    fence_t *f = fence_lookup(op->fence_signal);
    if (f) f->current_value = f->target_value;
    return DV_OK;
}

int32_t dv_vram_lock(dv_buffer_t b, uint32_t timeout_ms)
                                            { (void)b; (void)timeout_ms; return DV_OK; }
int32_t dv_vram_unlock(dv_buffer_t b)       { (void)b; return DV_OK; }

/* ---------- surface ---------- */

int32_t dv_surface_create(dv_vproc_t v, const dv_surface_desc_t *d, dv_surface_t *out)
{
    if (!d || !out) return DV_ERR_INVAL;
    vproc_t *p = vproc_lookup(v);
    if (!p) return DV_ERR_HANDLE;
    if (d->format != DV_FMT_BGRA8888 && d->format != DV_FMT_RGBA8888) return DV_ERR_FORMAT;
    if (unlikely(d->width == 0 || d->height == 0 ||
                 d->width > 8192 || d->height > 8192)) return DV_ERR_INVAL;

    uint32_t bytes = d->width * d->height * 4;

    /* Backing in RAM di sistema: nessun costo VRAM e nessuna quota
     * VRAM. Pensato per contenuti composti via blit (corpi finestra)
     * su adattatori con poca VRAM; le primitive sw_* non distinguono
     * (surface_pixels dispatcha). */
    if (d->flags & DV_SURF_FLAG_SYSRAM)
    {
        void *pix = malloc(bytes);
        if (!pix)
        {
            debug_print("[bga] surface_create SYSRAM: malloc fallita\n");
            return DV_ERR_NOMEM;
        }
        int sslot = alloc_surface_slot();
        if (sslot < 0)
        {
            free(pix);
            debug_print("[bga] surface_create SLOT EXHAUSTED (128)\n");
            return DV_ERR_NOMEM;
        }
        G->surfaces[sslot].owner       = v;
        G->surfaces[sslot].width       = d->width;
        G->surfaces[sslot].height      = d->height;
        G->surfaces[sslot].pitch_words = d->width;
        G->surfaces[sslot].format      = d->format;
        G->surfaces[sslot].flags       = d->flags;
        G->surfaces[sslot].vram_offset = UINT32_MAX;
        G->surfaces[sslot].vram_bytes  = 0;
        G->surfaces[sslot].sys_pixels  = pix;
        *out = (dv_surface_t)SLOT_TO_HANDLE(sslot);
        return DV_OK;
    }

    if (p->vram_used_bytes + bytes > p->vram_quota_bytes)
    {
        char buf[140];
        sprintf(buf, "[bga] surface_create %ux%u QUOTA FAIL used=%u+%u quota=%u\n",
                d->width, d->height, p->vram_used_bytes, bytes, p->vram_quota_bytes);
        debug_print(buf);
        return DV_ERR_QUOTA;
    }
    uint32_t off = vram_alloc(bytes);
    if (off == UINT32_MAX)
    {
        char buf[140];
        sprintf(buf, "[bga] surface_create %ux%u VRAM OOM (frag, used=%u quota=%u)\n",
                d->width, d->height, p->vram_used_bytes, p->vram_quota_bytes);
        debug_print(buf);
        return DV_ERR_OOM_VRAM;
    }

    int slot = alloc_surface_slot();
    if (slot < 0)
    {
        vram_free_at(off);
        debug_print("[bga] surface_create SLOT EXHAUSTED (128)\n");
        return DV_ERR_NOMEM;
    }
    G->surfaces[slot].owner       = v;
    G->surfaces[slot].width       = d->width;
    G->surfaces[slot].height      = d->height;
    G->surfaces[slot].pitch_words = d->width;
    G->surfaces[slot].format      = d->format;
    G->surfaces[slot].flags       = d->flags;
    G->surfaces[slot].sys_pixels  = NULL;
    G->surfaces[slot].vram_offset = off;
    G->surfaces[slot].vram_bytes  = bytes;
    p->vram_used_bytes += bytes;

    fast_fill32((volatile uint32_t *)((volatile uint8_t *)G->vram + off), 0,
                d->width * d->height);
    *out = (dv_surface_t)SLOT_TO_HANDLE(slot);
    return DV_OK;
}

int32_t dv_surface_destroy(dv_surface_t s)
{
    surface_t *ss = surface_lookup(s);
    if (!ss) return DV_ERR_HANDLE;
    vproc_t *p = vproc_lookup(ss->owner);
    if (ss->sys_pixels != NULL)
    {
        free(ss->sys_pixels);
        ss->sys_pixels = NULL;
    }
    else
    {
        if (p) p->vram_used_bytes -= ss->vram_bytes;
        vram_free_at(ss->vram_offset);
    }
    ss->used = false;
    if (G->scanout_source == s) G->scanout_source = DV_HANDLE_NONE;
    return DV_OK;
}

int32_t dv_surface_info(dv_surface_t s, dv_surface_info_t *out)
{
    surface_t *ss = surface_lookup(s);
    if (!ss || !out) return DV_ERR_HANDLE;
    out->width  = ss->width;
    out->height = ss->height;
    out->format = ss->format;
    out->pitch_bytes = ss->pitch_words * 4;
    out->flags  = ss->flags;
    return DV_OK;
}

int32_t dv_surface_resize(dv_surface_t s, uint32_t new_w, uint32_t new_h)
{ (void)s; (void)new_w; (void)new_h; return DV_ERR_NOSUPPORT; }

int32_t dv_surface_clear(dv_surface_t s, dv_color_t c)
{
    surface_t *ss = surface_lookup(s);
    if (!ss) return DV_ERR_HANDLE;
    uint32_t pix = ((c.a & 0xFF) << 24) | ((c.r & 0xFF) << 16)
                 | ((c.g & 0xFF) << 8)  |  (c.b & 0xFF);
    fast_fill32((volatile uint32_t *)surface_pixels(ss), pix,
                ss->width * ss->height);
    return DV_OK;
}

int32_t dv_surface_lock(dv_surface_t s, void **out_pixels, uint32_t *out_pitch)
                                            { (void)s; (void)out_pixels; (void)out_pitch;
                                              return DV_ERR_NOSUPPORT; }
int32_t dv_surface_unlock(dv_surface_t s)   { (void)s; return DV_ERR_NOSUPPORT; }

/* ---------- texture ---------- */
/* BGA has no texturing pipeline.  Textures expose as plain
 * surfaces backed by VRAM but the bind/sample/mips ops are no-ops. */

int32_t dv_texture_create(dv_vproc_t v, const dv_texture_desc_t *d, dv_texture_t *out)
{
    dv_surface_desc_t sd = {
        .width = d ? d->width : 0, .height = d ? d->height : 0,
        .format = d ? d->format : DV_FMT_BGRA8888, .flags = d ? d->flags : 0,
    };
    return dv_surface_create(v, &sd, (dv_surface_t *)out);
}

int32_t dv_texture_destroy(dv_texture_t t) { return dv_surface_destroy((dv_surface_t)t); }

int32_t dv_texture_upload(dv_texture_t t, const void *src, size_t src_bytes)
{
    surface_t *ss = surface_lookup((dv_surface_t)t);
    if (!ss || !src) return DV_ERR_HANDLE;
    uint32_t want = ss->width * ss->height * 4;
    if (src_bytes < want) return DV_ERR_INVAL;
    if ((((uintptr_t)src | want) & 3u) == 0)
        fast_copy32((volatile uint32_t *)surface_pixels(ss),
                    (const volatile uint32_t *)src, want / 4);
    else
        memcpy((void *)surface_pixels(ss), src, want);
    return DV_OK;
}

int32_t dv_texture_update_region(dv_texture_t t, dv_rect_t r,
                                 const void *src, uint32_t src_pitch)
{
    surface_t *ss = surface_lookup((dv_surface_t)t);
    if (!ss || !src) return DV_ERR_HANDLE;
    if (r.x < 0 || r.y < 0 ||
        r.x + (int32_t)r.w > (int32_t)ss->width ||
        r.y + (int32_t)r.h > (int32_t)ss->height) return DV_ERR_RANGE;
    volatile uint32_t *base = (volatile uint32_t *)surface_pixels(ss);
    uint32_t pitch_w = ss->pitch_words;
    const uint8_t *sp = (const uint8_t *)src;
    for (uint32_t y = 0; y < r.h; y++)
    {
        fast_copy32(base + (uint32_t)(r.y + (int32_t)y) * pitch_w + (uint32_t)r.x,
                    (const volatile uint32_t *)(sp + y * src_pitch), r.w);
    }
    return DV_OK;
}

int32_t dv_texture_download(dv_texture_t t, void *dst, size_t dst_bytes)
{
    surface_t *ss = surface_lookup((dv_surface_t)t);
    if (!ss || !dst) return DV_ERR_HANDLE;
    uint32_t want = ss->width * ss->height * 4;
    if (dst_bytes < want) return DV_ERR_INVAL;
    memcpy(dst, (const void *)surface_pixels(ss), want);
    return DV_OK;
}

int32_t dv_texture_generate_mips(dv_texture_t t) { (void)t; return DV_ERR_NOSUPPORT; }
int32_t dv_texture_bind(uint32_t slot, dv_texture_t t)
                                                 { (void)slot; (void)t; return DV_ERR_NOSUPPORT; }

/* ---------- buffer ---------- */

int32_t dv_buffer_create(dv_vproc_t v, dv_buffer_kind_t kind, uint64_t bytes,
                         uint32_t flags, dv_buffer_t *out)
{
    return dv_vram_alloc(v, (dv_vram_kind_t)kind, bytes, flags, out);
}

int32_t dv_buffer_destroy(dv_buffer_t b) { return dv_vram_free(b); }

int32_t dv_buffer_update(dv_buffer_t b, uint64_t offset,
                         const void *src, uint64_t bytes)
{
    buffer_t *bb = buffer_lookup(b);
    if (!bb || !src) return DV_ERR_HANDLE;
    if (offset + bytes > bb->bytes) return DV_ERR_RANGE;
    volatile uint8_t *dst = (volatile uint8_t *)G->vram + bb->vram_offset + offset;
    if ((((uintptr_t)src | (uintptr_t)dst | bytes) & 3u) == 0)
        fast_copy32((volatile uint32_t *)dst,
                    (const volatile uint32_t *)src, bytes / 4);
    else
        memcpy((void *)dst, src, bytes);
    return DV_OK;
}

int32_t dv_buffer_map(dv_buffer_t b, void **out_vaddr)
{ (void)b; (void)out_vaddr; return DV_ERR_NOSUPPORT; }
int32_t dv_buffer_unmap(dv_buffer_t b)
{ (void)b; return DV_ERR_NOSUPPORT; }
int32_t dv_buffer_bind(uint32_t slot, dv_buffer_kind_t k, dv_buffer_t b, uint64_t off)
{ (void)slot; (void)k; (void)b; (void)off; return DV_ERR_NOSUPPORT; }

/* ---------- 2D primitives ---------- */

int32_t dv_fill_rect(dv_surface_t dst, dv_rect_t r, dv_color_t c)
{
    surface_t *s = surface_lookup(dst);
    if (!s) return DV_ERR_HANDLE;
    sw_fill_rect(s, r, c);
    return DV_OK;
}


int32_t dv_fill_gradient(dv_surface_t dst, dv_rect_t r,
                         dv_color_t a, dv_color_t b, dv_gradient_dir_t dir)
{
    /* Linear horizontal/vertical gradient.  Diagonal and radial
     * are NOSUPPORT on BGA -- they want either bilinear sampling or
     * radius math that isn't worth a software fallback right now. */
    if (dir != DV_GRADIENT_HORIZONTAL && dir != DV_GRADIENT_VERTICAL)
        return DV_ERR_NOSUPPORT;
    surface_t *s = surface_lookup(dst);
    if (!s) return DV_ERR_HANDLE;
    if (r.w == 0 || r.h == 0) return DV_OK;

    int32_t x0 = r.x < 0 ? 0 : r.x;
    int32_t y0 = r.y < 0 ? 0 : r.y;
    int32_t x1 = r.x + (int32_t)r.w;
    int32_t y1 = r.y + (int32_t)r.h;
    if (x1 > (int32_t)s->width)  x1 = s->width;
    if (y1 > (int32_t)s->height) y1 = s->height;
    if (x0 >= x1 || y0 >= y1) return DV_OK;

    volatile uint32_t *base = (volatile uint32_t *)surface_pixels(s);
    uint32_t pitch_w = s->pitch_words;

    /* For each output pixel along the gradient axis, lerp 8-bit
     * channels in fixed-point.  Span is the original (unclamped)
     * rect dimension along that axis so clipping doesn't shift the
     * gradient ramp. */
    if (dir == DV_GRADIENT_VERTICAL)
    {
        uint32_t span = r.h;
        for (int32_t y = y0; y < y1; y++)
        {
            uint32_t t = (uint32_t)((y - r.y) * 256) / span;
            uint8_t rr = (uint8_t)((a.r * (256 - t) + b.r * t) >> 8);
            uint8_t gg = (uint8_t)((a.g * (256 - t) + b.g * t) >> 8);
            uint8_t bb = (uint8_t)((a.b * (256 - t) + b.b * t) >> 8);
            uint8_t aa = (uint8_t)((a.a * (256 - t) + b.a * t) >> 8);
            uint32_t pix = ((uint32_t)aa << 24) | ((uint32_t)rr << 16)
                         | ((uint32_t)gg <<  8) |  (uint32_t)bb;
            fast_fill32(base + (uint32_t)y * pitch_w + (uint32_t)x0, pix,
                        (uint32_t)(x1 - x0));
        }
    }
    else
    { /* DV_GRADIENT_HORIZONTAL */
        uint32_t span = r.w;
        /* Precompute the row once, then memcpy to each row of dst. */
        uint32_t row_count = (uint32_t)(x1 - x0);
        if (row_count == 0) return DV_OK;
        /* Stack-allocated scratch up to a reasonable cap; for wider
         * spans the BGA driver isn't expected to be the right path
         * anyway (caller would prefer a uniform fill or pre-rendered
         * texture). */
        if (row_count > 4096) row_count = 4096;
        uint32_t scratch[4096];
        for (uint32_t i = 0; i < row_count; i++)
        {
            uint32_t t = ((uint32_t)((int32_t)i + (x0 - r.x)) * 256) / span;
            uint8_t rr = (uint8_t)((a.r * (256 - t) + b.r * t) >> 8);
            uint8_t gg = (uint8_t)((a.g * (256 - t) + b.g * t) >> 8);
            uint8_t bb = (uint8_t)((a.b * (256 - t) + b.b * t) >> 8);
            uint8_t aa = (uint8_t)((a.a * (256 - t) + b.a * t) >> 8);
            scratch[i] = ((uint32_t)aa << 24) | ((uint32_t)rr << 16)
                       | ((uint32_t)gg <<  8) |  (uint32_t)bb;
        }
        for (int32_t y = y0; y < y1; y++)
        {
            fast_copy32(base + (uint32_t)y * pitch_w + (uint32_t)x0,
                        (volatile uint32_t *)scratch, row_count);
        }
    }
    return DV_OK;
}

int32_t dv_blit(dv_surface_t src, dv_rect_t sr, dv_surface_t dst, dv_point_t dp)
{
    surface_t *s = surface_lookup(src);
    surface_t *d = surface_lookup(dst);
    if (!s || !d) return DV_ERR_HANDLE;
    sw_blit(s, sr, d, dp);
    return DV_OK;
}

int32_t dv_blit_stretched(dv_surface_t s, dv_rect_t sr,
                          dv_surface_t d, dv_rect_t dr)
{
    /* Nearest-neighbor scaling.  For each destination pixel, sample
     * the source at the corresponding position via fixed-point
     * 16.16 ratio.  Adequate for thumbnails (Mission Control
     * preview) and resize previews where filtering quality matters
     * less than throughput.  A bilinear variant is a future
     * enhancement. */
    surface_t *src = surface_lookup(s);
    surface_t *dst = surface_lookup(d);
    if (!src || !dst) return DV_ERR_HANDLE;
    if (sr.w == 0 || sr.h == 0 || dr.w == 0 || dr.h == 0) return DV_OK;

    /* Clamp source rect to source surface bounds. */
    int32_t sx0 = sr.x, sy0 = sr.y;
    int32_t sx1 = sr.x + (int32_t)sr.w, sy1 = sr.y + (int32_t)sr.h;
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx1 > (int32_t)src->width)  sx1 = src->width;
    if (sy1 > (int32_t)src->height) sy1 = src->height;
    if (sx0 >= sx1 || sy0 >= sy1) return DV_OK;

    /* Clamp destination rect to destination surface bounds. */
    int32_t dx0 = dr.x, dy0 = dr.y;
    int32_t dx1 = dr.x + (int32_t)dr.w, dy1 = dr.y + (int32_t)dr.h;
    if (dx0 < 0) dx0 = 0;
    if (dy0 < 0) dy0 = 0;
    if (dx1 > (int32_t)dst->width)  dx1 = dst->width;
    if (dy1 > (int32_t)dst->height) dy1 = dst->height;
    if (dx0 >= dx1 || dy0 >= dy1) return DV_OK;

    volatile uint32_t *src_base = (volatile uint32_t *)surface_pixels(src);
    volatile uint32_t *dst_base = (volatile uint32_t *)surface_pixels(dst);
    uint32_t src_pitch_w = src->pitch_words;
    uint32_t dst_pitch_w = dst->pitch_words;

    /* Fixed-point 16.16 step.  Use the ORIGINAL (unclamped) dr for
     * the ratio so that clamping doesn't change the mapping. */
    uint32_t src_w_clamp = (uint32_t)(sx1 - sx0);
    uint32_t src_h_clamp = (uint32_t)(sy1 - sy0);
    uint32_t step_x = (src_w_clamp << 16) / dr.w;
    uint32_t step_y = (src_h_clamp << 16) / dr.h;

    /* Offset into source for the clamped portion of dst. */
    uint32_t base_dy_off = (uint32_t)(dy0 - dr.y);
    uint32_t base_dx_off = (uint32_t)(dx0 - dr.x);

    for (int32_t dy = dy0; dy < dy1; dy++)
    {
        uint32_t sy_q = (base_dy_off + (uint32_t)(dy - dy0)) * step_y;
        uint32_t sy = (uint32_t)sy0 + (sy_q >> 16);
        if (sy >= (uint32_t)sy1) sy = (uint32_t)sy1 - 1;
        const volatile uint32_t *srow = src_base + sy * src_pitch_w;
        volatile uint32_t       *drow = dst_base + (uint32_t)dy * dst_pitch_w;
        for (int32_t dx = dx0; dx < dx1; dx++)
        {
            uint32_t sx_q = (base_dx_off + (uint32_t)(dx - dx0)) * step_x;
            uint32_t sx = (uint32_t)sx0 + (sx_q >> 16);
            if (sx >= (uint32_t)sx1) sx = (uint32_t)sx1 - 1;
            drow[dx] = srow[sx];
        }
    }
    return DV_OK;
}

int32_t dv_blit_alpha(dv_surface_t src, dv_rect_t sr,
                      dv_surface_t dst, dv_point_t dp, uint8_t alpha)
{
    surface_t *s = surface_lookup(src);
    surface_t *d = surface_lookup(dst);
    if (!s || !d) return DV_ERR_HANDLE;
    sw_blit_alpha(s, sr, d, dp, alpha);
    return DV_OK;
}

/* Variante per-pixel del blit diretto: stessa convenzione alpha del
 * ramo use_pixel_alpha delle cmdlist (0xFF000000 sul pixel sorgente =
 * trasparente, eredita' di BlitBuffer). Serve al bake del compositor:
 * i record blit_inline/blit_tex la richiedono e il percorso diretto ne
 * era privo. */
int32_t dv_blit_pixel_alpha(dv_surface_t src, dv_rect_t sr,
                            dv_surface_t dst, dv_point_t dp)
{
    surface_t *s = surface_lookup(src);
    surface_t *d = surface_lookup(dst);
    if (!s || !d) return DV_ERR_HANDLE;
    sw_blit_pixel_alpha(s, sr, d, dp);
    return DV_OK;
}

int32_t dv_blit_yuv_to_rgb(dv_buffer_t y, dv_buffer_t u, dv_buffer_t vp,
                           uint32_t src_w, uint32_t src_h, dv_format_t src_fmt,
                           dv_surface_t dst, dv_rect_t dr)
{ (void)y; (void)u; (void)vp; (void)src_w; (void)src_h; (void)src_fmt;
  (void)dst; (void)dr; return DV_ERR_NOSUPPORT; }

int32_t dv_copy_region(dv_surface_t src, dv_rect_t sr,
                       dv_surface_t dst, dv_point_t dp)
{ return dv_blit(src, sr, dst, dp); }

int32_t dv_draw_line(dv_surface_t dst, dv_point_t a, dv_point_t b,
                     uint32_t thickness, dv_color_t c)
{
    surface_t *s = surface_lookup(dst);
    if (!s) return DV_ERR_HANDLE;
    if (thickness == 0) thickness = 1;

    /* Horizontal line: single fill_rect -- fastest path, common case
     * (panel separators).  Vertical similarly. */
    if (a.y == b.y)
    {
        int32_t x0 = a.x < b.x ? a.x : b.x;
        int32_t x1 = a.x < b.x ? b.x : a.x;
        dv_rect_t r = { x0, a.y, (uint32_t)(x1 - x0 + 1), thickness };
        sw_fill_rect(s, r, c);
        return DV_OK;
    }
    if (a.x == b.x)
    {
        int32_t y0 = a.y < b.y ? a.y : b.y;
        int32_t y1 = a.y < b.y ? b.y : a.y;
        dv_rect_t r = { a.x, y0, thickness, (uint32_t)(y1 - y0 + 1) };
        sw_fill_rect(s, r, c);
        return DV_OK;
    }

    /* Diagonal: Bresenham, plotting a (thickness x thickness) square
     * at each step.  Square stamping is cheap for small thickness;
     * proper end-cap geometry is a future enhancement. */
    int32_t x0 = a.x, y0 = a.y, x1 = b.x, y1 = b.y;
    int32_t dx =  x1 - x0; if (dx < 0) dx = -dx;
    int32_t dy = -(y1 - y0); if (dy > 0) dy = -dy;   /* dy <= 0 in algo */
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    int32_t half = (int32_t)(thickness / 2);

    while (1)
    {
        dv_rect_t pt = { x0 - half, y0 - half, thickness, thickness };
        sw_fill_rect(s, pt, c);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    return DV_OK;
}

int32_t dv_draw_rect_outline(dv_surface_t dst, dv_rect_t r,
                             uint32_t thickness, dv_color_t c)
{
    surface_t *s = surface_lookup(dst);
    if (!s) return DV_ERR_HANDLE;
    if (thickness == 0) thickness = 1;
    /* Top, bottom, left, right strips drawn as rects. */
    dv_rect_t top    = { r.x, r.y,                                r.w, thickness };
    dv_rect_t bottom = { r.x, r.y + (int32_t)r.h - (int32_t)thickness, r.w, thickness };
    dv_rect_t left   = { r.x, r.y + (int32_t)thickness,
                         thickness, r.h > 2 * thickness ? r.h - 2 * thickness : 0 };
    dv_rect_t right  = { r.x + (int32_t)r.w - (int32_t)thickness,
                         r.y + (int32_t)thickness,
                         thickness, left.h };
    sw_fill_rect(s, top, c);
    sw_fill_rect(s, bottom, c);
    sw_fill_rect(s, left, c);
    sw_fill_rect(s, right, c);
    return DV_OK;
}

int32_t dv_draw_polygon(dv_surface_t dst, const dv_point_t *pts, uint32_t count, dv_color_t c)
{ (void)dst; (void)pts; (void)count; (void)c; return DV_ERR_NOSUPPORT; }

int32_t dv_draw_circle(dv_surface_t dst, dv_point_t center, uint32_t radius,
                       uint32_t thickness, dv_color_t c)
{ (void)dst; (void)center; (void)radius; (void)thickness; (void)c; return DV_ERR_NOSUPPORT; }

/* dv_draw_glyphs — render glyphs from `glyph_atlas` onto `dst` at per-glyph
 * (x,y), recolored to `c`. Atlas layout (BGA): vertical strip of 256 glyphs,
 * glyph i at (0, i*glyph_h, atlas_w, glyph_h). Only atlas pixels with alpha > 0
 * are drawn (filled opaque with `c`), so a 1-bit font uploaded as a BGRA atlas
 * recolors at draw time. (x,y) is the glyph cell's TOP-LEFT, not a baseline. */
int32_t dv_draw_glyphs(dv_surface_t dst, dv_texture_t glyph_atlas,
                       const dv_glyph_t *glyphs, uint32_t count, dv_color_t c)
{
    surface_t *d = surface_lookup(dst);
    surface_t *a = surface_lookup((dv_surface_t)glyph_atlas);
    if (!d || !a) return DV_ERR_HANDLE;
    if (!glyphs || count == 0) return DV_OK;

    /* Atlas geometry.  256 cells vertically. */
    if (a->height < 256) return DV_ERR_INVAL;     /* not a glyph strip */
    uint32_t glyph_w = a->width;
    uint32_t glyph_h = a->height / 256;
    if (glyph_w == 0 || glyph_h == 0) return DV_ERR_INVAL;

    uint32_t fg = ((c.a & 0xFF) << 24) | ((c.r & 0xFF) << 16)
                | ((c.g & 0xFF) << 8)  |  (c.b & 0xFF);
    /* Force opaque so the recolored output composes solidly even
     * if caller passed alpha=0 by accident. */
    fg |= 0xFF000000u;

    volatile uint32_t *atlas_base = (volatile uint32_t *)surface_pixels(a);
    volatile uint32_t *dst_base   = (volatile uint32_t *)surface_pixels(d);
    uint32_t atlas_pitch_w = a->pitch_words;
    uint32_t dst_pitch_w   = d->pitch_words;

    for (uint32_t gi = 0; gi < count; gi++)
    {
        const dv_glyph_t *g = &glyphs[gi];
        if (g->glyph_index >= 256) continue;       /* out of atlas */

        int32_t gx0 = g->x;
        int32_t gy0 = g->y;
        int32_t gx1 = gx0 + (int32_t)glyph_w;
        int32_t gy1 = gy0 + (int32_t)glyph_h;

        /* Source-side offsets (atlas) when destination is clipped. */
        int32_t src_dx = 0, src_dy = 0;
        if (gx0 < 0) { src_dx = -gx0; gx0 = 0; }
        if (gy0 < 0) { src_dy = -gy0; gy0 = 0; }
        if (gx1 > (int32_t)d->width)  gx1 = d->width;
        if (gy1 > (int32_t)d->height) gy1 = d->height;
        if (gx0 >= gx1 || gy0 >= gy1) continue;

        uint32_t src_y0 = g->glyph_index * glyph_h + (uint32_t)src_dy;
        uint32_t src_x0 = (uint32_t)src_dx;

        for (int32_t dy = gy0; dy < gy1; dy++)
        {
            const volatile uint32_t *srow = atlas_base
                + (src_y0 + (uint32_t)(dy - gy0)) * atlas_pitch_w + src_x0;
            volatile uint32_t       *drow = dst_base
                + (uint32_t)dy * dst_pitch_w + (uint32_t)gx0;
            int32_t span = gx1 - gx0;
            for (int32_t dx = 0; dx < span; dx++)
            {
                if ((srow[dx] & 0xFF000000u) != 0)
                    drow[dx] = fg;
            }
        }
    }
    return DV_OK;
}

int32_t dv_scroll_region(dv_surface_t s, dv_rect_t r, int32_t dx, int32_t dy, dv_color_t fill)
{
    /* Scroll the contents of `r` within surface `s` by (dx, dy)
     * pixels.  The exposed strip(s) are filled with `fill`.  Common
     * use case: terminal-style upward scroll (dx=0, dy<0).
     *
     * Implementation: for each destination row that has a valid
     * source row inside `r`, memmove the overlapping span; rows
     * with no valid source (the exposed strip) get fill_rect.
     * Same logic for the horizontal exposed columns. */
    surface_t *surf = surface_lookup(s);
    if (!surf) return DV_ERR_HANDLE;
    if (r.w == 0 || r.h == 0) return DV_OK;

    /* Clamp r to surface bounds. */
    int32_t x0 = r.x < 0 ? 0 : r.x;
    int32_t y0 = r.y < 0 ? 0 : r.y;
    int32_t x1 = r.x + (int32_t)r.w;
    int32_t y1 = r.y + (int32_t)r.h;
    if (x1 > (int32_t)surf->width)  x1 = surf->width;
    if (y1 > (int32_t)surf->height) y1 = surf->height;
    if (x0 >= x1 || y0 >= y1) return DV_OK;

    volatile uint32_t *base = (volatile uint32_t *)surface_pixels(surf);
    uint32_t pitch_w = surf->pitch_words;

    /* If the scroll exceeds the rect dimensions, the entire rect
     * becomes "exposed" -- just fill it. */
    if (dx <= -(int32_t)(x1 - x0) || dx >= (int32_t)(x1 - x0) ||
        dy <= -(int32_t)(y1 - y0) || dy >= (int32_t)(y1 - y0))
    {
        dv_rect_t whole = { x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) };
        sw_fill_rect(surf, whole, fill);
        return DV_OK;
    }

    /* Vertical row copy direction.  Up-scroll (dy<0) copies top to
     * bottom (low-y row first); down-scroll (dy>0) copies bottom to
     * top.  Horizontal handled by memmove which is safe for any
     * direction within a single row. */
    int32_t copy_h = (y1 - y0) - (dy > 0 ? dy : -dy);
    int32_t copy_w = (x1 - x0) - (dx > 0 ? dx : -dx);

    if (dy < 0)
    {
        /* Source row is BELOW destination row; iterate top-down. */
        for (int32_t i = 0; i < copy_h; i++)
        {
            int32_t dst_y = y0 + i;
            int32_t src_y = dst_y - dy;     /* dy<0 -> src_y > dst_y */
            int32_t dst_x = (dx >= 0) ? (x0 + dx) : x0;
            int32_t src_x = (dx >= 0) ? x0       : (x0 - dx);
            volatile uint32_t *drow = base + (uint32_t)dst_y * pitch_w + (uint32_t)dst_x;
            volatile uint32_t *srow = base + (uint32_t)src_y * pitch_w + (uint32_t)src_x;
            /* memmove since src/dst rows can be the same when dy=0 + dx?0 */
            memmove((void *)drow, (const void *)srow, (uint32_t)copy_w * 4);
        }
    }
    else
    {
        /* dy >= 0: source row is ABOVE destination; iterate bottom-up. */
        for (int32_t i = copy_h - 1; i >= 0; i--)
        {
            int32_t dst_y = (dy >= 0 ? y0 + dy : y0) + i;
            int32_t src_y = dst_y - dy;
            int32_t dst_x = (dx >= 0) ? (x0 + dx) : x0;
            int32_t src_x = (dx >= 0) ? x0       : (x0 - dx);
            volatile uint32_t *drow = base + (uint32_t)dst_y * pitch_w + (uint32_t)dst_x;
            volatile uint32_t *srow = base + (uint32_t)src_y * pitch_w + (uint32_t)src_x;
            memmove((void *)drow, (const void *)srow, (uint32_t)copy_w * 4);
        }
    }

    /* Fill the exposed strip(s).  Up to two strips: one along the
     * vertical scroll direction, one along the horizontal. */
    if (dy < 0)
    {
        dv_rect_t strip = { x0, y1 + dy, (uint32_t)(x1 - x0), (uint32_t)(-dy) };
        sw_fill_rect(surf, strip, fill);
    }
    else if (dy > 0)
    {
        dv_rect_t strip = { x0, y0, (uint32_t)(x1 - x0), (uint32_t)dy };
        sw_fill_rect(surf, strip, fill);
    }
    if (dx < 0)
    {
        dv_rect_t strip = { x1 + dx, y0, (uint32_t)(-dx), (uint32_t)(y1 - y0) };
        sw_fill_rect(surf, strip, fill);
    }
    else if (dx > 0)
    {
        dv_rect_t strip = { x0, y0, (uint32_t)dx, (uint32_t)(y1 - y0) };
        sw_fill_rect(surf, strip, fill);
    }
    return DV_OK;
}

/* ---------- 3D, shaders, compute: not implemented on BGA ---------- */

int32_t dv_pipeline_create(dv_vproc_t v, const dv_pipeline_desc_t *d, dv_pipeline_t *out)
{ (void)v; (void)d; (void)out; return DV_ERR_NOSUPPORT; }
int32_t dv_pipeline_destroy(dv_pipeline_t p)        { (void)p; return DV_ERR_NOSUPPORT; }
int32_t dv_pipeline_bind(dv_pipeline_t p)           { (void)p; return DV_ERR_NOSUPPORT; }
int32_t dv_viewport_set(int32_t x, int32_t y, uint32_t w, uint32_t h, float a, float b)
{ (void)x; (void)y; (void)w; (void)h; (void)a; (void)b; return DV_ERR_NOSUPPORT; }
int32_t dv_rendertarget_set(const dv_rendertarget_set_t *rt) { (void)rt; return DV_ERR_NOSUPPORT; }
int32_t dv_transform_set(dv_transform_slot_t slot, const dv_mat4_t *m)
{ (void)slot; (void)m; return DV_ERR_NOSUPPORT; }
int32_t dv_clear(uint32_t flags, dv_color_t color, float depth, uint8_t stencil)
{ (void)flags; (void)color; (void)depth; (void)stencil; return DV_ERR_NOSUPPORT; }
int32_t dv_depth_state_set(const dv_depth_state_t *s)   { (void)s; return DV_ERR_NOSUPPORT; }
int32_t dv_blend_state_set(const dv_blend_state_t *s)   { (void)s; return DV_ERR_NOSUPPORT; }
int32_t dv_raster_state_set(const dv_raster_state_t *s) { (void)s; return DV_ERR_NOSUPPORT; }
int32_t dv_draw(dv_primitive_t p, uint32_t fv, uint32_t vc)
{ (void)p; (void)fv; (void)vc; return DV_ERR_NOSUPPORT; }
int32_t dv_draw_indexed(dv_primitive_t p, uint32_t ic, uint32_t fi, int32_t bv)
{ (void)p; (void)ic; (void)fi; (void)bv; return DV_ERR_NOSUPPORT; }
int32_t dv_draw_instanced(dv_primitive_t p, uint32_t ic, uint32_t inst, uint32_t fi, int32_t bv)
{ (void)p; (void)ic; (void)inst; (void)fi; (void)bv; return DV_ERR_NOSUPPORT; }

int32_t dv_shader_load(dv_vproc_t v, const dv_shader_desc_t *d, dv_shader_t *out)
{ (void)v; (void)d; (void)out; return DV_ERR_NOSUPPORT; }
int32_t dv_shader_destroy(dv_shader_t sh) { (void)sh; return DV_ERR_NOSUPPORT; }
int32_t dv_shader_query_lang(dv_shader_lang_t *out)
{ if (out) *out = DV_SHADER_LANG_NONE; return DV_OK; }
int32_t dv_shader_reflect(dv_shader_t sh, dv_shader_reflect_t *out)
{ (void)sh; (void)out; return DV_ERR_NOSUPPORT; }

int32_t dv_compute_load(dv_vproc_t v, const dv_compute_desc_t *d, dv_compute_t *out)
{ (void)v; (void)d; (void)out; return DV_ERR_NOSUPPORT; }
int32_t dv_compute_destroy(dv_compute_t k) { (void)k; return DV_ERR_NOSUPPORT; }
int32_t dv_compute_query_resources(dv_compute_t k, dv_compute_resources_t *out)
{ (void)k; (void)out; return DV_ERR_NOSUPPORT; }
int32_t dv_compute_bind_persistent(dv_compute_t k, const dv_compute_bind_t *b)
{ (void)k; (void)b; return DV_ERR_NOSUPPORT; }
int32_t dv_compute_unbind_persistent(dv_compute_t k, uint32_t slot)
{ (void)k; (void)slot; return DV_ERR_NOSUPPORT; }
int32_t dv_compute_persistent_state(dv_compute_t k, dv_buffer_t state_buffer)
{ (void)k; (void)state_buffer; return DV_ERR_NOSUPPORT; }
int32_t dv_compute_dispatch(dv_compute_t k, const dv_dispatch_t *d)
{ (void)k; (void)d; return DV_ERR_NOSUPPORT; }
int32_t dv_compute_dispatch_chain(dv_vproc_t v, const dv_dispatch_chain_t *c)
{ (void)v; (void)c; return DV_ERR_NOSUPPORT; }
int32_t dv_compute_dispatch_iterative(dv_vproc_t v, const dv_dispatch_iterative_t *it)
{ (void)v; (void)it; return DV_ERR_NOSUPPORT; }
int32_t dv_compute_dispatch_indirect(dv_vproc_t v, const dv_dispatch_indirect_t *ind)
{ (void)v; (void)ind; return DV_ERR_NOSUPPORT; }
int32_t dv_compute_dispatch_batch(dv_vproc_t v, const dv_dispatch_t *arr, uint32_t n)
{ (void)v; (void)arr; (void)n; return DV_ERR_NOSUPPORT; }
int32_t dv_compute_signal_on_cond(dv_vproc_t v, const dv_signal_on_cond_t *s)
{ (void)v; (void)s; return DV_ERR_NOSUPPORT; }

/* ---------- compositing and scanout ---------- */

int32_t dv_layer_create(dv_vproc_t v, const dv_layer_desc_t *d, dv_layer_t *out)
{
    if (!d || !out) return DV_ERR_INVAL;
    if (!vproc_lookup(v)) return DV_ERR_HANDLE;
    if (d->source  != DV_HANDLE_NONE && !surface_lookup(d->source))  return DV_ERR_HANDLE;
    if (d->cmdlist != DV_HANDLE_NONE && !cmdlist_lookup(d->cmdlist)) return DV_ERR_HANDLE;
    int slot = alloc_layer_slot();
    if (slot < 0)
    {
        debug_print("[bga] layer_create SLOT EXHAUSTED (64)\n");
        return DV_ERR_NOMEM;
    }
    G->layers[slot].owner           = v;
    G->layers[slot].source          = d->source;
    G->layers[slot].z               = d->z;
    G->layers[slot].alpha           = d->alpha;
    G->layers[slot].visible         = d->visible;
    G->layers[slot].use_pixel_alpha = d->use_pixel_alpha;
    G->layers[slot].src_rect        = d->src_rect;
    G->layers[slot].dst_rect        = d->dst_rect;
    G->layers[slot].cmdlist         = d->cmdlist;
    *out = (dv_layer_t)SLOT_TO_HANDLE(slot);
    return DV_OK;
}

int32_t dv_layer_destroy(dv_layer_t l)
{
    layer_t *ll = layer_lookup(l);
    if (!ll) return DV_ERR_HANDLE;
    ll->used = false;
    return DV_OK;
}

int32_t dv_layer_update(dv_layer_t l, const dv_layer_desc_t *d)
{
    layer_t *ll = layer_lookup(l);
    if (!ll || !d) return DV_ERR_HANDLE;
    if (d->source  != DV_HANDLE_NONE && !surface_lookup(d->source))  return DV_ERR_HANDLE;
    if (d->cmdlist != DV_HANDLE_NONE && !cmdlist_lookup(d->cmdlist)) return DV_ERR_HANDLE;
    ll->source          = d->source;
    ll->z               = d->z;
    ll->alpha           = d->alpha;
    ll->visible         = d->visible;
    ll->use_pixel_alpha = d->use_pixel_alpha;
    ll->src_rect        = d->src_rect;
    ll->dst_rect        = d->dst_rect;
    ll->cmdlist         = d->cmdlist;
    return DV_OK;
}

int32_t dv_layer_set_transform(dv_layer_t l, const dv_mat2x3_t *xform)
{ (void)l; (void)xform; return DV_ERR_NOSUPPORT; }

int32_t dv_layer_set_visible(dv_layer_t l, bool visible)
{
    layer_t *ll = layer_lookup(l);
    if (!ll) return DV_ERR_HANDLE;
    ll->visible = visible;
    return DV_OK;
}

HOT
int32_t dv_compose(uint32_t display_id, dv_fence_t fence_signal)
{
    if (display_id != 0) return DV_ERR_INVAL;

    layer_t *visible[MAX_LAYERS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_LAYERS; i++)
        if (G->layers[i].used && G->layers[i].visible) visible[n++] = &G->layers[i];
    if (n > 1)
        for (uint32_t i = 1; i < n; i++)
        {
            layer_t *cur = visible[i];
            int32_t j = (int32_t)i - 1;
            while (j >= 0 && visible[j]->z > cur->z) { visible[j + 1] = visible[j]; j--; }
            visible[j + 1] = cur;
        }

    surface_t back_desc = {
        .used = true, .owner = 0,
        .width = G->mode.width, .height = G->mode.height,
        .pitch_words = G->mode.width,
        .format = DV_FMT_BGRA8888, .flags = 0,
        .vram_offset = (G->shadow != NULL) ? UINT32_MAX
                                           : G->primary_offset[G->back_page],
        .vram_bytes  = (G->shadow != NULL) ? 0 : G->primary_bytes,
        .sys_pixels  = G->shadow,          /* NULL = compose diretta */
    };

    /* Back-buffer clear: only needed when the bottommost visible layer doesn't
     * fully cover the screen opaquely (skipping saves ~3 MiB writes/frame at
     * 1024x768, significant since dv_compose runs on every fb_flip with IF=0).
     * "Covers everything opaquely" = n>0, visible[0]->alpha==255,
     * !use_pixel_alpha, dst_rect covers (0,0,mode.w,mode.h), src matches dst (no
     * scaling). Otherwise fall back to a full clear. */
    bool skip_clear = false;
    if (n > 0)
    {
        layer_t *bot = visible[0];
        /* Cmdlist-mode bottom layer cannot promise full opaque
         * coverage (no static src_rect to inspect; the recorded ops
         * could fill anything or nothing).  Conservative: always clear
         * when bottom is a cmdlist.  A future optimization could let
         * the caller advertise "this cmdlist covers fullscreen with
         * opaque pixels" via a flag, but for v1 we take the safe path. */
        if (bot->cmdlist == DV_HANDLE_NONE &&
            bot->alpha == 255 && !bot->use_pixel_alpha &&
            bot->dst_rect.x == 0 && bot->dst_rect.y == 0 &&
            bot->dst_rect.w == G->mode.width &&
            bot->dst_rect.h == G->mode.height &&
            bot->src_rect.w == G->mode.width &&
            bot->src_rect.h == G->mode.height)
        {
            skip_clear = true;
        }
    }
    if (!skip_clear)
    {
        fast_fill32((volatile uint32_t *)surface_pixels(&back_desc), 0,
                    G->mode.width * G->mode.height);
    }
    for (uint32_t i = 0; i < n; i++)
    {
        layer_t *L = visible[i];
        /* Cmdlist mode takes precedence over surface mode.  When a
         * layer has a cmdlist attached, compose replays its recorded
         * draw ops directly onto the back page at the layer's dst_rect
         * origin.  No intermediate surface, no upload, no pixel cache
         * -- the chrome / icons / window content is reconstructed each
         * frame from its retained-mode description.  Trades a small
         * amount of CPU per layer for ~100x reduction in VRAM hold. */
        if (L->cmdlist != DV_HANDLE_NONE)
        {
            cmdlist_t *cl = cmdlist_lookup(L->cmdlist);
            if (!cl) continue;
            cmdlist_replay(cl, &back_desc, L->dst_rect.x, L->dst_rect.y);
            continue;
        }
        surface_t *src = surface_lookup(L->source);
        if (!src) continue;
        dv_point_t dp = { L->dst_rect.x, L->dst_rect.y };
        if (L->use_pixel_alpha)  sw_blit_pixel_alpha(src, L->src_rect, &back_desc, dp);
        else if (L->alpha == 255) sw_blit(src, L->src_rect, &back_desc, dp);
        else                      sw_blit_alpha(src, L->src_rect, &back_desc, dp, L->alpha);
    }

    fence_t *f = fence_lookup(fence_signal);
    if (f) f->current_value = f->target_value;
    return DV_OK;
}

/* Command list — retained-mode display list. Records are tightly packed (not
 * aligned), 1-byte tag first, host-endian; reads memcpy into a stack struct.
 *   FILL_RECT   : tag(1)+rect(16)+color(4) = 21 B
 *   BLIT        : tag(1)+src(4)+sr(16)+dx(4)+dy(4) = 29 B
 *   BLIT_ALPHA  : BLIT + alpha(1)+use_pixel_alpha(1) = 31 B
 *   DRAW_GLYPHS : tag(1)+atlas(4)+color(4)+count(4)+glyph[count] = 13+12*count B
 *   DRAW_LINE   : tag(1)+ax+ay+bx+by(16)+thickness(4)+color(4) = 25 B */

#define CMDLIST_REC_FILL_RECT     1
#define CMDLIST_REC_BLIT          2
#define CMDLIST_REC_BLIT_ALPHA    3
#define CMDLIST_REC_DRAW_GLYPHS   4
#define CMDLIST_REC_DRAW_LINE     5

static dv_color_t bgra_to_dv_color(uint32_t bgra)
{
    dv_color_t c = {
        .b = (bgra)       & 0xFF,
        .g = (bgra >> 8)  & 0xFF,
        .r = (bgra >> 16) & 0xFF,
        .a = (bgra >> 24) & 0xFF,
    };
    return c;
}
static uint32_t dv_color_to_bgra(dv_color_t c)
{
    return ((c.a & 0xFF) << 24) | ((c.r & 0xFF) << 16)
         | ((c.g & 0xFF) << 8)  |  (c.b & 0xFF);
}

/* Forward decls -- defined later */
static void sw_draw_glyphs_into(surface_t *dst, surface_t *atlas,
                                const dv_glyph_t *glyphs, uint32_t count,
                                dv_color_t color, int dx, int dy);
static void sw_draw_line_into(surface_t *dst, dv_point_t a, dv_point_t b,
                              uint32_t thickness, dv_color_t color,
                              int ox, int oy);

/* Append `bytes` bytes from `src` into cmdlist `cl`'s storage at its
 * current write head.  Returns DV_OK on success, DV_ERR_QUOTA if
 * appending would overflow capacity.  command_count is bumped on
 * success (caller-controlled -- pass false for partial-record appends). */
static int32_t cmdlist_append(cmdlist_t *cl, const void *src, uint32_t bytes,
                              bool count_as_record)
{
    if (cl->bytes_used + bytes > cl->capacity) return DV_ERR_QUOTA;
    uint8_t *base = &G->cmdlist_storage[cl->storage_off + cl->bytes_used];
    /* Manual byte copy to avoid pulling in memcpy from libc -- keeps
     * the driver's import surface minimal.  Bytes counts here are
     * tiny (<= 100 per record). */
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < bytes; i++) base[i] = s[i];
    cl->bytes_used += bytes;
    if (count_as_record) cl->command_count++;
    return DV_OK;
}

int32_t dv_cmdlist_create(dv_vproc_t v, uint32_t capacity_bytes,
                          dv_cmdlist_t *out)
{
    if (!out) return DV_ERR_INVAL;
    if (!vproc_lookup(v)) return DV_ERR_HANDLE;
    if (capacity_bytes == 0) return DV_ERR_INVAL;

    /* Round up to 16 to keep storage alignment-friendly for future
     * record formats that might want it (current packed format does
     * not require alignment, but the bump pointer stays aligned). */
    uint32_t cap = (capacity_bytes + 15u) & ~15u;
    if (G->cmdlist_storage_used + cap > CMDLIST_STORAGE_BYTES)
    {
        char buf[140];
        sprintf(buf, "[bga] cmdlist_create cap=%u POOL FAIL used=%u total=%u\n",
                cap, G->cmdlist_storage_used, (uint32_t)CMDLIST_STORAGE_BYTES);
        debug_print(buf);
        return DV_ERR_NOMEM;
    }

    int slot = alloc_cmdlist_slot();
    if (slot < 0)
    {
        debug_print("[bga] cmdlist_create SLOT EXHAUSTED (128)\n");
        return DV_ERR_NOMEM;
    }

    G->cmdlists[slot].owner         = v;
    G->cmdlists[slot].storage_off   = G->cmdlist_storage_used;
    G->cmdlists[slot].capacity      = cap;
    G->cmdlists[slot].bytes_used    = 0;
    G->cmdlists[slot].command_count = 0;
    G->cmdlist_storage_used += cap;

    *out = (dv_cmdlist_t)SLOT_TO_HANDLE(slot);
    return DV_OK;
}

int32_t dv_cmdlist_destroy(dv_cmdlist_t cl_h)
{
    cmdlist_t *cl = cmdlist_lookup(cl_h);
    if (!cl) return DV_ERR_HANDLE;

    /* Storage pool compaction. The pool is a bump-allocated contiguous arena;
     * destroy used to leave a hole the bump allocator could never reuse, so
     * enough grow/destroy cycles fragmented it and dv_cmdlist_create returned
     * DV_ERR_NOMEM despite free space. Fix: on destroy, slide every cmdlist with
     * a higher storage_off back by this one's capacity (updating storage_off
     * in-place) and retract the bump pointer. Fast path: if this cmdlist is
     * already at the top, just retract — the common rebind case. */
    uint32_t off = cl->storage_off;
    uint32_t cap = cl->capacity;

    cl->used = false;

    if (off + cap == G->cmdlist_storage_used)
    {
        /* Top of pool -- O(1) retraction. */
        G->cmdlist_storage_used -= cap;
    }
    else if (cap > 0)
    {
        /* Slide everything above this cmdlist down by `cap` bytes,
         * then update the storage_off of each affected cmdlist. */
        uint32_t move_src   = off + cap;
        uint32_t move_bytes = G->cmdlist_storage_used - move_src;
        if (move_bytes > 0)
        {
            memmove(&G->cmdlist_storage[off],
                    &G->cmdlist_storage[move_src],
                    move_bytes);
        }
        G->cmdlist_storage_used -= cap;
        for (int i = 0; i < MAX_CMDLISTS; i++)
        {
            cmdlist_t *other = &G->cmdlists[i];
            if (other == cl) continue;
            if (!other->used) continue;
            if (other->storage_off > off)
                other->storage_off -= cap;
        }
    }

    return DV_OK;
}

int32_t dv_cmdlist_reset(dv_cmdlist_t cl_h)
{
    cmdlist_t *cl = cmdlist_lookup(cl_h);
    if (!cl) return DV_ERR_HANDLE;
    cl->bytes_used = 0;
    cl->command_count = 0;
    return DV_OK;
}

int32_t dv_cmdlist_fill_rect(dv_cmdlist_t cl_h, dv_rect_t r, dv_color_t c)
{
    cmdlist_t *cl = cmdlist_lookup(cl_h);
    if (!cl) return DV_ERR_HANDLE;
    uint8_t tag = CMDLIST_REC_FILL_RECT;
    uint32_t bgra = dv_color_to_bgra(c);
    int32_t rc;
    if ((rc = cmdlist_append(cl, &tag,  1,  false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &r,    16, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &bgra, 4,  true )) != DV_OK) return rc;
    return DV_OK;
}

int32_t dv_cmdlist_blit(dv_cmdlist_t cl_h, dv_surface_t src,
                        dv_rect_t sr, dv_point_t dp)
{
    cmdlist_t *cl = cmdlist_lookup(cl_h);
    if (!cl) return DV_ERR_HANDLE;
    uint8_t tag = CMDLIST_REC_BLIT;
    int32_t rc;
    if ((rc = cmdlist_append(cl, &tag,    1, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &src,    4, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &sr,    16, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &dp.x,   4, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &dp.y,   4, true )) != DV_OK) return rc;
    return DV_OK;
}

int32_t dv_cmdlist_blit_alpha(dv_cmdlist_t cl_h, dv_surface_t src,
                              dv_rect_t sr, dv_point_t dp, uint8_t alpha,
                              bool use_pixel_alpha)
{
    cmdlist_t *cl = cmdlist_lookup(cl_h);
    if (!cl) return DV_ERR_HANDLE;
    uint8_t tag = CMDLIST_REC_BLIT_ALPHA;
    uint8_t upa = use_pixel_alpha ? 1 : 0;
    int32_t rc;
    if ((rc = cmdlist_append(cl, &tag,    1, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &src,    4, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &sr,    16, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &dp.x,   4, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &dp.y,   4, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &alpha,  1, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &upa,    1, true )) != DV_OK) return rc;
    return DV_OK;
}

int32_t dv_cmdlist_draw_glyphs(dv_cmdlist_t cl_h, dv_texture_t atlas,
                               const dv_glyph_t *glyphs, uint32_t count,
                               dv_color_t color)
{
    cmdlist_t *cl = cmdlist_lookup(cl_h);
    if (!cl) return DV_ERR_HANDLE;
    if (count > 0 && !glyphs) return DV_ERR_INVAL;
    uint8_t tag = CMDLIST_REC_DRAW_GLYPHS;
    uint32_t bgra = dv_color_to_bgra(color);
    int32_t rc;
    if ((rc = cmdlist_append(cl, &tag,    1, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &atlas,  4, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &bgra,   4, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &count,  4, false)) != DV_OK) return rc;
    if (count > 0)
    {
        if ((rc = cmdlist_append(cl, glyphs,
                                 count * (uint32_t)sizeof(dv_glyph_t),
                                 false)) != DV_OK) return rc;
    }
    cl->command_count++;
    return DV_OK;
}

int32_t dv_cmdlist_draw_line(dv_cmdlist_t cl_h, dv_point_t a, dv_point_t b,
                             uint32_t thickness, dv_color_t color)
{
    cmdlist_t *cl = cmdlist_lookup(cl_h);
    if (!cl) return DV_ERR_HANDLE;
    uint8_t tag = CMDLIST_REC_DRAW_LINE;
    uint32_t bgra = dv_color_to_bgra(color);
    int32_t rc;
    if ((rc = cmdlist_append(cl, &tag,        1, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &a.x,        4, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &a.y,        4, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &b.x,        4, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &b.y,        4, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &thickness,  4, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &bgra,       4, true )) != DV_OK) return rc;
    return DV_OK;
}

int32_t dv_cmdlist_info(dv_cmdlist_t cl_h, dv_cmdlist_info_t *out)
{
    cmdlist_t *cl = cmdlist_lookup(cl_h);
    if (!cl || !out) return DV_ERR_HANDLE;
    out->bytes_used     = cl->bytes_used;
    out->bytes_capacity = cl->capacity;
    out->command_count  = cl->command_count;
    out->reserved       = 0;
    return DV_OK;
}

/* sw_draw_glyphs_into -- same pixel-pushing logic as dv_draw_glyphs
 * but operating on already-resolved surface_t* (skip handle lookup
 * since cmdlist_replay has the pointers) and with an (ox, oy)
 * translation applied to each glyph's destination position. */
static void sw_draw_glyphs_into(surface_t *dst, surface_t *atlas,
                                const dv_glyph_t *glyphs, uint32_t count,
                                dv_color_t color, int ox, int oy)
{
    if (!dst || !atlas || !glyphs || count == 0) return;
    if (atlas->height < 256) return;
    uint32_t glyph_w = atlas->width;
    uint32_t glyph_h = atlas->height / 256;
    if (glyph_w == 0 || glyph_h == 0) return;

    uint32_t fg = ((color.a & 0xFF) << 24) | ((color.r & 0xFF) << 16)
                | ((color.g & 0xFF) << 8)  |  (color.b & 0xFF);
    fg |= 0xFF000000u;

    volatile uint32_t *atlas_base = (volatile uint32_t *)surface_pixels(atlas);
    volatile uint32_t *dst_base   = (volatile uint32_t *)surface_pixels(dst);
    uint32_t atlas_pitch_w = atlas->pitch_words;
    uint32_t dst_pitch_w   = dst->pitch_words;

    for (uint32_t gi = 0; gi < count; gi++)
    {
        const dv_glyph_t *g = &glyphs[gi];
        if (g->glyph_index >= 256) continue;

        int32_t gx0 = g->x + ox;
        int32_t gy0 = g->y + oy;
        int32_t gx1 = gx0 + (int32_t)glyph_w;
        int32_t gy1 = gy0 + (int32_t)glyph_h;

        int32_t src_dx = 0, src_dy = 0;
        if (gx0 < 0) { src_dx = -gx0; gx0 = 0; }
        if (gy0 < 0) { src_dy = -gy0; gy0 = 0; }
        if (gx1 > (int32_t)dst->width)  gx1 = dst->width;
        if (gy1 > (int32_t)dst->height) gy1 = dst->height;
        if (gx0 >= gx1 || gy0 >= gy1) continue;

        uint32_t src_y0 = g->glyph_index * glyph_h + (uint32_t)src_dy;
        uint32_t src_x0 = (uint32_t)src_dx;

        for (int32_t dy = gy0; dy < gy1; dy++)
        {
            const volatile uint32_t *srow = atlas_base
                + (src_y0 + (uint32_t)(dy - gy0)) * atlas_pitch_w + src_x0;
            volatile uint32_t       *drow = dst_base
                + (uint32_t)dy * dst_pitch_w + (uint32_t)gx0;
            int32_t span = gx1 - gx0;
            for (int32_t dx = 0; dx < span; dx++)
            {
                if ((srow[dx] & 0xFF000000u) != 0)
                    drow[dx] = fg;
            }
        }
    }
}

/* sw_draw_line_into -- axis-aligned 1-px-thick lines, no thickness
 * support yet (the field is reserved for future expansion).  Diagonal
 * lines fall back to a naive Bresenham; rare in UI use, optimize
 * later if a profile says so. */
static void sw_draw_line_into(surface_t *dst, dv_point_t a, dv_point_t b,
                              uint32_t thickness, dv_color_t color,
                              int ox, int oy)
{
    if (!dst) return;
    (void)thickness;   /* not yet implemented; treat as 1 */
    int32_t ax = a.x + ox, ay = a.y + oy;
    int32_t bx = b.x + ox, by = b.y + oy;
    dv_color_t c = color;

    /* Horizontal */
    if (ay == by)
    {
        int32_t x0 = ax < bx ? ax : bx;
        int32_t x1 = ax < bx ? bx : ax;
        dv_rect_t r = { x0, ay, (uint32_t)(x1 - x0 + 1), 1 };
        sw_fill_rect(dst, r, c);
        return;
    }
    /* Vertical */
    if (ax == bx)
    {
        int32_t y0 = ay < by ? ay : by;
        int32_t y1 = ay < by ? by : ay;
        dv_rect_t r = { ax, y0, 1, (uint32_t)(y1 - y0 + 1) };
        sw_fill_rect(dst, r, c);
        return;
    }
    /* Bresenham -- single-pixel-wide. */
    int32_t dx =  (bx > ax ? bx - ax : ax - bx);
    int32_t dy = -(by > ay ? by - ay : ay - by);
    int32_t sx = ax < bx ? 1 : -1;
    int32_t sy = ay < by ? 1 : -1;
    int32_t err = dx + dy;
    uint32_t fg = dv_color_to_bgra(c);
    volatile uint32_t *base = (volatile uint32_t *)surface_pixels(dst);
    uint32_t pitch_w = dst->pitch_words;
    int32_t  W = (int32_t)dst->width;
    int32_t  H = (int32_t)dst->height;
    for (;;)
    {
        if (ax >= 0 && ax < W && ay >= 0 && ay < H)
            base[(uint32_t)ay * pitch_w + (uint32_t)ax] = fg;
        if (ax == bx && ay == by) break;
        int32_t e2 = err * 2;
        if (e2 >= dy) { err += dy; ax += sx; }
        if (e2 <= dx) { err += dx; ay += sy; }
    }
}

/* Replay: execute every record in `cl`'s storage onto `dst`.  Offset
 * (ox, oy) translates every record's coordinate origin -- used by
 * dv_compose to position a layer's cmdlist at the layer's dst_rect.
 * Records that reference a surface (BLIT, BLIT_ALPHA, DRAW_GLYPHS)
 * silently skip on unknown surface handles to keep compose robust
 * to source destruction races. */
HOT
static void cmdlist_replay(cmdlist_t *cl, surface_t *dst, int ox, int oy)
{
    const uint8_t *p   = &G->cmdlist_storage[cl->storage_off];
    const uint8_t *end = p + cl->bytes_used;

    while (p < end)
    {
        uint8_t tag = *p++;
        switch (tag)
        {
            case CMDLIST_REC_FILL_RECT: {
                dv_rect_t r;  uint32_t bgra;
                __builtin_memcpy(&r,    p,     16); p += 16;
                __builtin_memcpy(&bgra, p,      4); p +=  4;
                r.x += ox; r.y += oy;
                sw_fill_rect(dst, r, bgra_to_dv_color(bgra));
                break;
            }
            case CMDLIST_REC_BLIT: {
                dv_surface_t src_h; dv_rect_t sr; int32_t dx, dy;
                __builtin_memcpy(&src_h, p, 4);  p += 4;
                __builtin_memcpy(&sr,    p, 16); p += 16;
                __builtin_memcpy(&dx,    p, 4);  p += 4;
                __builtin_memcpy(&dy,    p, 4);  p += 4;
                surface_t *src = surface_lookup(src_h);
                if (!src) break;
                dv_point_t dp = { dx + ox, dy + oy };
                sw_blit(src, sr, dst, dp);
                break;
            }
            case CMDLIST_REC_BLIT_ALPHA: {
                dv_surface_t src_h; dv_rect_t sr; int32_t dx, dy;
                uint8_t alpha, use_pixel_alpha;
                __builtin_memcpy(&src_h,           p, 4);  p += 4;
                __builtin_memcpy(&sr,              p, 16); p += 16;
                __builtin_memcpy(&dx,              p, 4);  p += 4;
                __builtin_memcpy(&dy,              p, 4);  p += 4;
                alpha           = *p++;
                use_pixel_alpha = *p++;
                surface_t *src = surface_lookup(src_h);
                if (!src) break;
                dv_point_t dp = { dx + ox, dy + oy };
                if (use_pixel_alpha)        sw_blit_pixel_alpha(src, sr, dst, dp);
                else if (alpha == 255)      sw_blit(src, sr, dst, dp);
                else                        sw_blit_alpha(src, sr, dst, dp, alpha);
                break;
            }
            case CMDLIST_REC_DRAW_GLYPHS: {
                dv_texture_t atlas_h; uint32_t bgra, count;
                __builtin_memcpy(&atlas_h, p, 4); p += 4;
                __builtin_memcpy(&bgra,    p, 4); p += 4;
                __builtin_memcpy(&count,   p, 4); p += 4;
                const dv_glyph_t *glyphs = (const dv_glyph_t *)p;
                p += count * sizeof(dv_glyph_t);
                surface_t *atlas = surface_lookup((dv_surface_t)atlas_h);
                if (!atlas) break;
                sw_draw_glyphs_into(dst, atlas, glyphs, count,
                                    bgra_to_dv_color(bgra), ox, oy);
                break;
            }
            case CMDLIST_REC_DRAW_LINE: {
                int32_t ax, ay, bx, by; uint32_t thickness, bgra;
                __builtin_memcpy(&ax,        p, 4); p += 4;
                __builtin_memcpy(&ay,        p, 4); p += 4;
                __builtin_memcpy(&bx,        p, 4); p += 4;
                __builtin_memcpy(&by,        p, 4); p += 4;
                __builtin_memcpy(&thickness, p, 4); p += 4;
                __builtin_memcpy(&bgra,      p, 4); p += 4;
                sw_draw_line_into(dst, (dv_point_t){ax, ay}, (dv_point_t){bx, by},
                                  thickness, bgra_to_dv_color(bgra), ox, oy);
                break;
            }
            default:
                /* Corrupt record stream -- bail out of replay rather
                 * than continuing into garbage.  Should not happen
                 * unless cmdlist memory was scribbled. */
                return;
        }
    }
}

/* dv_page_flip honors DV_FLIP_VSYNC by polling VGA Input Status Register 1 (port
 * 0x3DA bit 3) for vertical retrace before swapping the scanout register (see
 * wait_for_vblank). Without it, the Y_OFFSET write lands mid-scan and tears.
 * The spin holds IF=0 for up to one refresh (~16.6 ms at 60 Hz) — acceptable
 * since the fb_flip caller blocks on this anyway and waiting a frame IS vsync
 * (intrinsic, not overhead). No vsync = immediate swap (tear is the caller's
 * choice). */
HOT
/* Compose parziale "onesta per meta'": BGA non ha il percorso
 * scissorato (la compose e' comunque piena), ma la PRESENTAZIONE puo'
 * essere limitata al rettangolo: in modalita' shadow si copia sul
 * primary la sola banda richiesta — il senso di fb_flip_rect
 * (immediatezza, meno byte verso la VRAM) e' preservato. In compose
 * diretta il primary e' gia' aggiornato dalla compose stessa. */
int32_t dv_compose_rect_present(uint32_t display_id,
                                int32_t rx, int32_t ry,
                                uint32_t rw, uint32_t rh,
                                dv_fence_t fence_signal)
{
    int32_t rc = dv_compose(display_id, DV_HANDLE_NONE);
    if (rc != DV_OK) return rc;

    if (G->shadow != NULL)
    {
        int32_t x0 = rx, y0 = ry;
        int32_t x1 = rx + (int32_t)rw, y1 = ry + (int32_t)rh;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int32_t)G->mode.width)  x1 = (int32_t)G->mode.width;
        if (y1 > (int32_t)G->mode.height) y1 = (int32_t)G->mode.height;
        if (x0 < x1 && y0 < y1)
        {
            uint32_t pitch_w = G->mode.width;
            volatile uint32_t *dst = (volatile uint32_t *)
                ((volatile uint8_t *)G->vram + G->primary_offset[0]);
            const volatile uint32_t *src =
                (const volatile uint32_t *)G->shadow;
            for (int32_t y = y0; y < y1; y++)
                fast_copy32(dst + (uint32_t)y * pitch_w + (uint32_t)x0,
                            src + (uint32_t)y * pitch_w + (uint32_t)x0,
                            (uint32_t)(x1 - x0));
        }
    }

    fence_t *f = fence_lookup(fence_signal);
    if (f) f->current_value = f->target_value;
    return DV_OK;
}

int32_t dv_page_flip(uint32_t display_id, uint32_t flags, dv_fence_t fence_signal)
{
    if (display_id != 0) return DV_ERR_INVAL;

    if (flags & DV_FLIP_VSYNC)
    {
        /* If the vblank wait timed out (stuck retrace, host stall),
         * swap anyway.  Tearing on a single frame is far less
         * disruptive than a dropped frame -- the latter shows up as
         * a perceptible hitch and, if the stall recurs, sustained
         * sfarfallio.  The next call resumes vsync-aligned swaps
         * once the retrace bit catches up. */
        (void)wait_for_vblank();
    }

    /* Presentazione shadow (single buffer): UNA copia sequenziale di
     * pixel finali sul primary visibile. E' l'unica scrittura che lo
     * scanout veda mai: gli strati della compose vivono in RAM di
     * sistema. Il flip di pagina sotto resta un no-op visivo (le due
     * pagine coincidono) e viene saltato. */
    if (G->shadow != NULL)
    {
        fast_copy32((volatile uint32_t *)
                        ((volatile uint8_t *)G->vram + G->primary_offset[0]),
                    (const volatile uint32_t *)G->shadow,
                    G->mode.width * G->mode.height);
        fence_t *sf = fence_lookup(fence_signal);
        if (sf) sf->current_value = sf->target_value;
        return DV_OK;
    }

    G->back_page ^= 1;
    scanout_program_back();
    fence_t *f = fence_lookup(fence_signal);
    if (f) f->current_value = f->target_value;
    return DV_OK;
}

/* ---------- hardware cursor: not present on BGA ---------- */

int32_t dv_cursor_set_bitmap(uint32_t display_id, const dv_cursor_desc_t *d)
{ (void)display_id; (void)d; return DV_ERR_NOSUPPORT; }
int32_t dv_cursor_set_position(uint32_t display_id, int32_t x, int32_t y)
{ (void)display_id; (void)x; (void)y; return DV_ERR_NOSUPPORT; }
int32_t dv_cursor_show(uint32_t display_id) { (void)display_id; return DV_ERR_NOSUPPORT; }
int32_t dv_cursor_hide(uint32_t display_id) { (void)display_id; return DV_ERR_NOSUPPORT; }

/* ---------- hardware overlay: not present on BGA ---------- */

int32_t dv_overlay_create(dv_vproc_t v, const dv_overlay_desc_t *d, dv_overlay_t *out)
{ (void)v; (void)d; (void)out; return DV_ERR_NOSUPPORT; }
int32_t dv_overlay_destroy(dv_overlay_t o)         { (void)o; return DV_ERR_NOSUPPORT; }
int32_t dv_overlay_update(dv_overlay_t o, const dv_overlay_update_t *u)
{ (void)o; (void)u; return DV_ERR_NOSUPPORT; }
int32_t dv_overlay_set_visible(dv_overlay_t o, bool visible)
{ (void)o; (void)visible; return DV_ERR_NOSUPPORT; }

/* ---------- synchronization ---------- */

int32_t dv_fence_create(dv_vproc_t v, dv_fence_t *out)
{
    vproc_t *p = vproc_lookup(v);
    if (!p || !out) return DV_ERR_HANDLE;
    int slot = alloc_fence_slot();
    if (slot < 0) return DV_ERR_NOMEM;
    G->fences[slot].owner         = v;
    G->fences[slot].target_value  = 1;
    G->fences[slot].current_value = 0;
    p->fences_in_flight++;
    *out = (dv_fence_t)SLOT_TO_HANDLE(slot);
    return DV_OK;
}

int32_t dv_fence_destroy(dv_fence_t f)
{
    fence_t *ff = fence_lookup(f);
    if (!ff) return DV_ERR_HANDLE;
    vproc_t *p = vproc_lookup(ff->owner);
    if (p && p->fences_in_flight > 0) p->fences_in_flight--;
    ff->used = false;
    return DV_OK;
}

int32_t dv_fence_signal(dv_fence_t f)
{
    fence_t *ff = fence_lookup(f);
    if (!ff) return DV_ERR_HANDLE;
    ff->current_value = ff->target_value;
    return DV_OK;
}

int32_t dv_fence_wait(dv_fence_t f, uint32_t timeout_ms)
{
    /* Software fences on BGA are always signaled by the time the
     * caller can observe an unsignaled state: every dv_* that signals
     * a fence does so synchronously inside its dispatch, so by the
     * time the client calls fence_wait the signal has already
     * happened (the dispatch is single-threaded).  If unsignaled
     * here, the client is out of order and would deadlock -- better
     * to surface that as NOTREADY than to spin. */
    (void)timeout_ms;
    fence_t *ff = fence_lookup(f);
    if (!ff) return DV_ERR_HANDLE;
    return ff->current_value >= ff->target_value ? DV_OK : DV_ERR_NOTREADY;
}

int32_t dv_fence_poll(dv_fence_t f)
{
    fence_t *ff = fence_lookup(f);
    if (!ff) return DV_ERR_HANDLE;
    return ff->current_value >= ff->target_value ? DV_OK : DV_ERR_NOTREADY;
}

int32_t dv_semaphore_create(dv_vproc_t v, uint32_t initial, dv_semaphore_t *out)
{ (void)v; (void)initial; (void)out; return DV_ERR_NOSUPPORT; }
int32_t dv_semaphore_destroy(dv_semaphore_t s)            { (void)s; return DV_ERR_NOSUPPORT; }
int32_t dv_semaphore_signal(dv_semaphore_t s)             { (void)s; return DV_ERR_NOSUPPORT; }
int32_t dv_semaphore_wait(dv_semaphore_t s, uint32_t to)  { (void)s; (void)to; return DV_ERR_NOSUPPORT; }
int32_t dv_barrier_insert(dv_vthread_t t, dv_barrier_kind_t k)
                                                          { (void)t; (void)k; return DV_OK; }

/* ---------- capability and introspection ---------- */

int32_t dv_cap_query(uint64_t *out_capabilities)
{
    if (!out_capabilities) return DV_ERR_INVAL;
    *out_capabilities =
          DV_CAP_PAGE_FLIP
        | DV_CAP_ACCELERATED_BLIT
        | DV_CAP_ALPHA_BLEND
        | DV_CAP_VSYNC          /* real, polled on VGA Input Status 1 (port 0x3DA bit 3) */
        | DV_CAP_HW_SCROLL;     /* dv_scroll_region -- memmove fallback, no real HW accel on BGA */
    /* No DV_CAP_VRAM_MAP (cross-process BAR sharing not implemented).
     * No DV_CAP_HW_CURSOR (BGA has no hardware cursor; cursor is
     * a dv_layer composited in software).
     * No DV_CAP_3D / DV_CAP_SHADER_PROGRAMMABLE / DV_CAP_COMPUTE
     * (BGA is a dumb framebuffer, no execution units).
     * dv_draw_line / dv_draw_glyphs / dv_blit_stretched /
     * dv_fill_gradient are part of the 2D baseline implied by
     * DV_CAP_ACCELERATED_BLIT -- no separate flags. */
    return DV_OK;
}

int32_t dv_cap_query_limit(dv_limit_t which, uint64_t *out_value)
{
    if (!out_value) return DV_ERR_INVAL;
    switch (which)
    {
        case DV_LIMIT_MAX_TEX_W:        *out_value = 8192;            return DV_OK;
        case DV_LIMIT_MAX_TEX_H:        *out_value = 8192;            return DV_OK;
        case DV_LIMIT_MAX_RT_W:         *out_value = 2560;            return DV_OK;
        case DV_LIMIT_MAX_RT_H:         *out_value = 1600;            return DV_OK;
        case DV_LIMIT_MAX_LAYERS:       *out_value = MAX_LAYERS;      return DV_OK;
        case DV_LIMIT_MAX_VTHREAD:      *out_value = MAX_VTHREADS;    return DV_OK;
        case DV_LIMIT_MAX_VCORE:        *out_value = 1;               return DV_OK;
        case DV_LIMIT_MAX_VRAM_BYTES:   *out_value = G->vram_bytes;   return DV_OK;
        case DV_LIMIT_MAX_VBUF_BYTES:   *out_value = G->vram_bytes / 4; return DV_OK;
        case DV_LIMIT_MAX_DISPLAYS:     *out_value = 1;               return DV_OK;
        default:                        return DV_ERR_NOSUPPORT;
    }
}

int32_t dv_cap_query_format(dv_format_t fmt, uint32_t *out_usage_flags)
{
    if (!out_usage_flags) return DV_ERR_INVAL;
    if (fmt == DV_FMT_BGRA8888 || fmt == DV_FMT_RGBA8888)
        *out_usage_flags = DV_FMT_USE_SAMPLE | DV_FMT_USE_RENDERTARGET
                         | DV_FMT_USE_BLEND  | DV_FMT_USE_SCANOUT;
    else
        *out_usage_flags = 0;
    return DV_OK;
}

int32_t dv_driver_info(dv_driver_info_t *out)
{
    if (!out) return DV_ERR_INVAL;
    strcpy(out->name,   "bga");
    strcpy(out->vendor, "QEMU/Bochs");
    out->version_major = 0; out->version_minor = 3; out->version_patch = 0;
    out->pci_vendor_id = G->dev.vendor_id;
    out->pci_device_id = G->dev.device_id;
    return DV_OK;
}

/* ============================================================================
 *  End of dobVideo protocol implementation.
 * ============================================================================ */

/* ==========================================================================
 *  Helpers for the transport layer (and for control-plane handling)
 *  exposed via bga_state.h.
 * ========================================================================== */

int32_t bga_internal_scanout_set(dv_surface_t s)
{
    if (s == DV_HANDLE_NONE) { G->scanout_source = DV_HANDLE_NONE; return DV_OK; }
    surface_t *ss = surface_lookup(s);
    if (!ss) return DV_ERR_HANDLE;
    if (!(ss->flags & DV_SURF_FLAG_SCANOUT)) return DV_ERR_PERM;
    G->scanout_source = s;
    return DV_OK;
}

int32_t bga_internal_mode_set(const dv_mode_t *m)
{
    if (!m) return DV_ERR_INVAL;
    int rc = bga_apply_mode(m->width, m->height, 32);
    if (rc != DV_OK) return rc;
    bga_notify_subscribers(DOBVC_EVENT_MODE_CHANGED, 0, m->width, m->height);
    return DV_OK;
}

void bga_recompute_mode_list(void)
{
    static const struct { uint32_t w, h; } catalog[] = {
        {  640,  480 }, {  800,  600 }, { 1024,  768 },
        { 1152,  864 }, { 1280,  720 }, { 1280, 1024 },
        { 1366,  768 }, { 1440,  900 }, { 1600,  900 },
        { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 },
        { 2048, 1152 }, { 2560, 1440 },
    };
    G->mode_list_n = 0;
    for (uint32_t i = 0; i < sizeof(catalog) / sizeof(catalog[0]); i++)
    {
        uint64_t need = (uint64_t)catalog[i].w * catalog[i].h * 4ull * 2ull;
        if (need <= G->vram_bytes && G->mode_list_n < MAX_MODE_LIST)
        {
            G->mode_list_buf[G->mode_list_n++] = (dv_mode_t){
                .width = catalog[i].w, .height = catalog[i].h,
                .refresh_hz = 60, .format = DV_FMT_BGRA8888, .flags = 0,
            };
        }
    }
}

void bga_gpu_reset_full(void)
{
    for (uint32_t i = 0; i < MAX_VPROCS;   i++) G->vprocs[i].used   = false;
    for (uint32_t i = 0; i < MAX_SURFACES; i++)
    {
        /* I backing in RAM di sistema vanno restituiti alla heap: il
         * reset azzera le tabelle, non la memoria del processo. */
        if (G->surfaces[i].used && G->surfaces[i].sys_pixels != NULL)
        {
            free(G->surfaces[i].sys_pixels);
            G->surfaces[i].sys_pixels = NULL;
        }
        G->surfaces[i].used = false;
    }
    for (uint32_t i = 0; i < MAX_BUFFERS;  i++) G->buffers[i].used  = false;
    for (uint32_t i = 0; i < MAX_FENCES;   i++) G->fences[i].used   = false;
    for (uint32_t i = 0; i < MAX_LAYERS;   i++) G->layers[i].used   = false;
    for (uint32_t i = 0; i < MAX_VTHREADS; i++) G->vthreads[i].used = false;
    G->scanout_source = DV_HANDLE_NONE;
    vram_init_pool(0, G->vram_bytes);
    bga_apply_mode(G->mode.width, G->mode.height, 32);
    bga_notify_subscribers(DOBVC_EVENT_GPU_RESET, 0, 0, 0);
}

int bga_subscribe(uint32_t port, uint32_t mask)
{
    int slot = -1;
    for (int i = 0; i < MAX_SUBSCRIBERS; i++)
        if (G->subs[i].used && G->subs[i].port == port) { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < MAX_SUBSCRIBERS; i++)
            if (!G->subs[i].used) { slot = i; break; }
    if (slot < 0) return DV_ERR_NOMEM;
    G->subs[slot].used = true;
    G->subs[slot].port = port;
    G->subs[slot].mask = mask;
    return DV_OK;
}

void bga_shutdown_for_detach(void)
{
    debug_print("[bga] DETACH received, releasing.\n");
    bga_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    dob_driver_released();
    _exit(0);
}

/* ==========================================================================
 *  No vsync emulation: BGA has no hardware vsync interrupt and MainDOB
 *  rules out idle wakeups.  G->vsync_count stays at 0 forever; nothing
 *  ticks.  See dv_vsync_wait() above for the honest NOSUPPORT.
 * ========================================================================== */

/* ==========================================================================
 *  Bootstrap and main
 * ========================================================================== */

COLD
static bool bga_probe_and_init(void)
{
    bga_write(VBE_DISPI_INDEX_ID, VBE_DISPI_ID5);
    if (bga_read(VBE_DISPI_INDEX_ID) != VBE_DISPI_ID5)
    {
        debug_print("[bga] FATAL: BGA register window not responding.\n");
        return false;
    }

    uint16_t vram_64k = bga_read(VBE_DISPI_INDEX_VIDEO_MEMORY_64K);
    G->vram_bytes = (uint32_t)vram_64k * 65536u;
    if (G->vram_bytes == 0) G->vram_bytes = 16u * 1024u * 1024u;

    G->vram_phys = G->dev.bar[0] & ~0x0Fu;
    if (G->vram_phys == 0)
    {
        debug_print("[bga] FATAL: BAR0 not assigned.\n");
        return false;
    }
    G->vram = (volatile bgra_t *)mmap_phys(G->vram_phys, G->vram_bytes);
    if (!G->vram)
    {
        debug_print("[bga] FATAL: VRAM mmap failed.\n");
        return false;
    }
    {
        char buf[80];
        snprintf(buf, sizeof(buf), "[bga] VRAM: %u MB at phys 0x%08x mapped\n",
                 G->vram_bytes / (1024 * 1024), G->vram_phys);
        debug_print(buf);
    }

    vram_init_pool(0, G->vram_bytes);
    bga_recompute_mode_list();

    if (bga_apply_mode(1024, 768, 32) != DV_OK)
    {
        debug_print("[bga] FATAL: default mode_set failed.\n");
        return false;
    }
    return true;
}

/* Forward decl: lives in bga_transport_ipc.c */
extern int bga_transport_ipc_run(uint32_t port);

/* Forward decl: lives in bga_fast_entry.asm */
extern void bga_fast_entry(void);

int main(void)
{
    set_priority(1);

    dob_server_init("DobVideoControl");
    uint32_t port = dob_server_get_port();

    /* NOTE: do not register the "video" name in the registry here.
     * Other modules (notably dobinterface) declare needs:video in
     * Startup_modules and the kernel parks them until the registry
     * entry appears.  Registering now would unpark them too early --
     * the int 0x85 boomerang slot below isn't installed yet, so any
     * dv_call would return DV_ERR_NOTREADY (-4).  The registration
     * is moved to the very end of init, after the fast path is
     * guaranteed live.  This makes "video registered" a single-shot
     * readiness signal that covers both the IPC port and the
     * boomerang slot. */

    dob_registry_wait("hotplug", 5000);
    if (!dob_driver_attach(&G->dev))
    {
        debug_print("[bga] FATAL: hotplug attach failed.\n");
        _exit(1);
    }

    debug_print("[bga] MainDOB BGA video driver v0.3 starting...\n");
    pci_enable_bus_master(&G->dev);

    if (!bga_probe_and_init()) _exit(1);

    /* Register int 0x85 fast path.  Once this returns 0, callers
     * doing `int 0x85` with EAX=opcode will land in bga_fast_entry,
     * which dispatches the register-friendly subset of dv_* opcodes
     * directly.  The IPC transport keeps running in parallel for
     * payload-bearing opcodes the fast path doesn't yet cover. */
    /* Ring-3 boomerang dispatch resources, in bga's own user-mapped AS: a stack
     * the kernel IRETs onto at ring 3, and a buffer the kernel copies the caller
     * payload into (>= the kernel's G_PAYLOAD_MAX = 16384). A userspace process's
     * static BSS is user-mapped, so these are valid ring-3 addresses. */
    static __attribute__((aligned(16))) uint8_t bga_dispatch_stack[16384];
    static __attribute__((aligned(16))) uint8_t bga_payload_buf[16384];

    if (syscall3(SYS_REGISTER_VIDEO_DRIVER,
                 (int)(uintptr_t)bga_fast_entry,
                 (int)(uintptr_t)(bga_dispatch_stack + sizeof bga_dispatch_stack), /* stack top, grows down */
                 (int)(uintptr_t)bga_payload_buf) == 0)
        debug_print("[bga] int 0x85 fast path registered.\n");
    else
        debug_print("[bga] WARN: int 0x85 fast path registration failed.\n");

    /* Now that BOTH the IPC port AND the boomerang slot are up,
     * announce readiness via the registry.  Any thread parked with
     * needs:video (dobinterface, future GPU clients) wakes here and
     * can immediately make dv_calls -- the boomerang will land in
     * bga_fast_entry, not in the empty-slot fallback. */
    dob_registry_register("video", port);

    debug_print("[bga] Ready (event-driven, no idle wakeups).\n");

    /* Run the control-plane IPC loop.  The data plane is served by
     * the int 0x85 boomerang registered above; this loop only sees
     * the infrequent DOBVC_* control calls. */
    return bga_transport_ipc_run(port);
}
