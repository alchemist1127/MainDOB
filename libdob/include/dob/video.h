/* MainDOB dobVideo -- protocol call surface for video drivers.
 *
 * This header is the public ABI between any video client (notably the
 * GPU-side of dobinterface, also dobplayer, future games, and compute
 * consumers like mining or LLM inference) and any video driver
 * implementing the protocol (vbe, virtio_gpu, voodoo, radeon, ...).
 *
 * The protocol mirrors the kernel's <sys/syscall.h>: a flat surface of
 * opcode + arguments + return code, identical for every driver, with
 * capability negotiation for feature subsets the underlying hardware
 * does not implement.  Drivers report DV_ERR_NOSUPPORT for any call
 * outside their feature set; clients query the cap_query family at
 * attach time to learn what's available.
 *
 * The transport mechanism -- how a call physically reaches the driver --
 * is intentionally not specified by this header.  It is the concern of
 * the runtime layer that backs the wrappers below.
 *
 * Two planes.  The driver exposes two interfaces:
 *
 *   1. Data plane -- this header (dobVideo).  Fast-path, kernel-style
 *      flat call surface, restricted to processes that hold a
 *      vprocess.  Everything in this header is data-plane.
 *
 *   2. Control plane -- <dob/DobVideoControl.h>.  Traditional IPC
 *      stub-style API on the driver's normal service port, accessible
 *      by any process.  Holds the system-global write operations:
 *      mode_set, gamma_set, scanout_set, multi-display arrangement,
 *      power state, gpu reset.  This is what Settings or any common
 *      app talks to when it wants to change the resolution.  Read-only
 *      queries that are also useful at runtime to a vprocess
 *      (mode_list, mode_get_current, display_info, gamma_get,
 *      vsync_wait) are mirrored in this header (data plane) for cheap
 *      in-line access by vprocess code; clients without a vprocess
 *      use the control plane equivalents.
 *
 * What a vprocess is.  A vprocess is a first-class video-side process
 * with its own private VRAM, its own vthreads scheduled on the
 * driver's vcores, and isolation from peer vprocesses by neighborhood
 * rules.  It is NOT a worker thread of the driver, NOT an opaque
 * context, NOT a handle to driver-internal state.  It is a process,
 * with the same status in the video world that a process_t has in the
 * CPU world: created on attach, lives independently, dies on detach
 * (auto on the owning CPU process's death).  When the documentation
 * below says "vthread" it means a thread of the vprocess executed on
 * a vcore -- again a real execution context, not a driver worker.
 *
 * Categories:
 *   1. Return codes, common types, formats, capabilities
 *   2. vprocess / vthread management
 *   3. vcore execution model (CUDA-style parallelism)
 *   4. Display and mode (read-only; writes are in DobVideoControl.h)
 *   5. VRAM
 *   6. Surface
 *   7. Texture
 *   8. Buffer (vertex / index / uniform / generic)
 *   9. 2D primitives
 *  10. 3D pipeline and draw
 *  11. Programmable shaders
 *  12. Compute programs (mining, LLM, ...)
 *  13. Compositing and scanout
 *  14. Hardware cursor
 *  15. Hardware overlay
 *  16. Synchronization (fence, semaphore, barrier)
 *  17. Capability and introspection
 *  18. Asynchronous events
 */

#ifndef MAINDOB_DOB_VIDEO_H
#define MAINDOB_DOB_VIDEO_H

#include <dob/types.h>

/* ==========================================================================
 *  1. Return codes, common types, formats, capabilities
 * ========================================================================== */

/* Return codes.  All dobVideo wrappers return int32_t: >= 0 on success
 * (0 or a non-negative count / handle), < 0 on error. */
#define DV_OK                    0
#define DV_ERR_NOMEM            -1   /* host RAM allocation failed */
#define DV_ERR_INVAL            -2   /* malformed arguments */
#define DV_ERR_NOSUPPORT        -3   /* driver does not implement this opcode */
#define DV_ERR_NOTREADY         -4   /* vproc not attached / driver not ready */
#define DV_ERR_HANDLE           -5   /* invalid or revoked handle */
#define DV_ERR_OOM_VRAM         -6   /* out of video memory */
#define DV_ERR_QUOTA            -7   /* vproc quota exceeded */
#define DV_ERR_RESET            -8   /* GPU was reset; resources lost */
#define DV_ERR_TIMEOUT          -9   /* wait expired */
#define DV_ERR_BUSY            -10   /* resource currently in use */
#define DV_ERR_FORMAT          -11   /* pixel format unsupported in this role */
#define DV_ERR_RANGE           -12   /* index / coordinate out of range */
#define DV_ERR_PERM            -13   /* caller lacks the required capability */

/* Opcode space.  Each category gets a 16-entry slot; category byte = high,
 * index byte = low.  Opcodes are stable; gaps are deliberate for future
 * extension within a category. */
#define DV_OPCAT(op)            (((op) >> 8) & 0xFF)
#define DV_OPIDX(op)            ((op) & 0xFF)

/* ----- Opaque handles (uint32_t internally; typed for safety) -----
 *
 * Zero is the universal "none / invalid" handle.  Drivers must not
 * return zero from any successful create call. */
typedef uint32_t dv_vproc_t;
typedef uint32_t dv_vthread_t;
typedef uint32_t dv_surface_t;
typedef uint32_t dv_texture_t;
typedef uint32_t dv_buffer_t;
typedef uint32_t dv_pipeline_t;
typedef uint32_t dv_shader_t;
typedef uint32_t dv_compute_t;
typedef uint32_t dv_fence_t;
typedef uint32_t dv_semaphore_t;
typedef uint32_t dv_layer_t;
typedef uint32_t dv_overlay_t;
typedef uint32_t dv_cmdlist_t;

#define DV_HANDLE_NONE          0u

/* ----- Geometry primitives ----- */
typedef struct { int32_t  x, y; }                    dv_point_t;
typedef struct { int32_t  x, y; uint32_t w, h; }     dv_rect_t;
typedef struct { uint32_t r, g, b, a; }              dv_color_t;     /* 8-bit per channel, packed in low byte */

/* 4x4 row-major matrix for transforms (model/view/proj). */
typedef struct { float m[16]; }                      dv_mat4_t;

/* 2x3 affine matrix for 2D layer transforms. */
typedef struct { float m[6]; }                       dv_mat2x3_t;

/* ----- Pixel formats ----- */
typedef enum
{
    DV_FMT_NONE          = 0x00,

    /* Color, integer */
    DV_FMT_R8            = 0x10,    /* 1 ch  8b   -- alpha map, glyph atlas */
    DV_FMT_R8G8          = 0x11,    /* 2 ch  16b */
    DV_FMT_RGB565        = 0x20,    /* 3 ch  16b  -- legacy 16bpp displays */
    DV_FMT_RGB888        = 0x21,    /* 3 ch  24b  -- packed */
    DV_FMT_RGBA8888      = 0x22,    /* 4 ch  32b  -- standard */
    DV_FMT_BGRA8888      = 0x23,    /* 4 ch  32b  -- Windows-native */
    DV_FMT_RGB10A2       = 0x24,    /* 4 ch  32b  -- extended-range */
    DV_FMT_RGBA16F       = 0x25,    /* 4 ch  64b  -- HDR float */
    DV_FMT_RGBA32F       = 0x26,    /* 4 ch 128b  -- compute */

    /* Palette indexed */
    DV_FMT_PAL8          = 0x30,    /* 256-color, palette via palette_set */

    /* Depth / stencil */
    DV_FMT_DEPTH16       = 0x40,
    DV_FMT_DEPTH24_S8    = 0x41,
    DV_FMT_DEPTH32F      = 0x42,

    /* YUV (overlay, video decode) */
    DV_FMT_YUYV          = 0x50,    /* packed 4:2:2 */
    DV_FMT_YUV420P       = 0x51,    /* planar 4:2:0 -- MP2/MP4 decoders */
    DV_FMT_NV12          = 0x52,    /* semi-planar 4:2:0 */

    /* Compressed (texture only) */
    DV_FMT_BC1           = 0x60,    /* DXT1 -- RGB or RGBA1 */
    DV_FMT_BC3           = 0x61,    /* DXT5 -- RGBA */
} dv_format_t;

/* ----- Capability flags (returned by cap_query) -----
 * 64-bit bitmask.  Drivers OR together what they support.  Future bits
 * are reserved.  Clients must NOT assume any bit is set. */
#define DV_CAP_3D                   (1ULL << 0)   /* 3D pipeline: vbuf/draw/transform */
#define DV_CAP_SHADER_PROGRAMMABLE  (1ULL << 1)   /* shader_load + custom pipelines */
#define DV_CAP_COMPUTE              (1ULL << 2)   /* compute_dispatch */
#define DV_CAP_COMPUTE_PERSIST       (1ULL << 3)   /* compute_bind_persistent */
#define DV_CAP_COMPUTE_CHAIN         (1ULL << 4)   /* dispatch_chain */
#define DV_CAP_COMPUTE_INDIRECT      (1ULL << 5)   /* dispatch_indirect */
#define DV_CAP_COMPUTE_ITERATIVE     (1ULL << 6)   /* dispatch_iterative */
#define DV_CAP_COMPUTE_SIGNAL_COND   (1ULL << 7)   /* signal_on_condition */
#define DV_CAP_OVERLAY              (1ULL << 8)   /* hardware overlay plane */
#define DV_CAP_YUV                  (1ULL << 9)   /* YUV pixel formats */
#define DV_CAP_MIPS                 (1ULL << 10)
#define DV_CAP_ALPHA_BLEND          (1ULL << 11)
#define DV_CAP_DEPTH_TEST           (1ULL << 12)
#define DV_CAP_STENCIL              (1ULL << 13)
#define DV_CAP_MULTIDISPLAY         (1ULL << 14)
#define DV_CAP_HW_CURSOR            (1ULL << 15)
#define DV_CAP_ACCELERATED_BLIT     (1ULL << 16)
#define DV_CAP_VRAM_MAP             (1ULL << 17)  /* vram_map into client space */
#define DV_CAP_DMA                  (1ULL << 18)
#define DV_CAP_GAMMA                (1ULL << 19)
#define DV_CAP_PALETTE              (1ULL << 20)
#define DV_CAP_HW_SCROLL            (1ULL << 21)  /* legacy scroll_region */
#define DV_CAP_INSTANCING           (1ULL << 22)  /* draw_instanced */
#define DV_CAP_TEXTURE_COMPRESSION  (1ULL << 23)  /* BC1/BC3 */
#define DV_CAP_VSYNC                (1ULL << 24)
#define DV_CAP_PAGE_FLIP            (1ULL << 25)

/* ----- Limits (cap_query_limit) ----- */
typedef enum
{
    DV_LIMIT_MAX_TEX_W           = 1,
    DV_LIMIT_MAX_TEX_H           = 2,
    DV_LIMIT_MAX_RT_W            = 3,
    DV_LIMIT_MAX_RT_H            = 4,
    DV_LIMIT_MAX_LAYERS          = 5,
    DV_LIMIT_MAX_VTHREAD         = 6,
    DV_LIMIT_MAX_VCORE           = 7,
    DV_LIMIT_MAX_VRAM_BYTES      = 8,
    DV_LIMIT_MAX_VBUF_BYTES      = 9,
    DV_LIMIT_MAX_COMPUTE_THREADS  = 10,  /* per block */
    DV_LIMIT_MAX_COMPUTE_GRID     = 11,  /* per dimension */
    DV_LIMIT_MAX_TEXTURE_UNITS   = 12,
    DV_LIMIT_MAX_RENDER_TARGETS  = 13,  /* MRT count */
    DV_LIMIT_MAX_SHADER_BYTES    = 14,
    DV_LIMIT_MAX_DISPLAYS        = 15,
} dv_limit_t;

/* Pixel format usage role (cap_query_format) */
#define DV_FMT_USE_SAMPLE       (1u << 0)    /* readable as texture */
#define DV_FMT_USE_RENDERTARGET (1u << 1)    /* writable as RT */
#define DV_FMT_USE_BLEND        (1u << 2)    /* alpha blending into it */
#define DV_FMT_USE_SCANOUT      (1u << 3)    /* eligible for scanout */
#define DV_FMT_USE_OVERLAY      (1u << 4)
#define DV_FMT_USE_STORAGE      (1u << 5)    /* compute read-write */

/* ==========================================================================
 *  2. vprocess / vthread management        opcodes 0x0100..0x011F
 *
 *  A vprocess is the video-side counterpart of a CPU process_t.  It is
 *  a real, isolated execution entity -- owns its VRAM allocations, its
 *  vthreads, its compute programs, its surfaces and textures -- not a
 *  driver-internal worker or context object.  Created by vproc_attach,
 *  destroyed by vproc_detach (auto on owning CPU-process death).
 *  Quotas (memory, vthread count) are enforced per-vprocess.  A
 *  vthread is a thread of the vprocess scheduled onto a vcore.
 * ========================================================================== */

#define DV_VPROC_ATTACH           0x0100
#define DV_VPROC_DETACH           0x0101
#define DV_VPROC_INFO             0x0102
#define DV_VPROC_SET_QUOTA        0x0103

#define DV_VTHREAD_CREATE         0x0110
#define DV_VTHREAD_DESTROY        0x0111
#define DV_VTHREAD_PRIORITY       0x0112
#define DV_VTHREAD_YIELD          0x0113
#define DV_VTHREAD_JOIN           0x0114
#define DV_VTHREAD_WAIT_IDLE      0x0115

typedef struct
{
    uint64_t vram_quota_bytes;     /* cap on this vproc's VRAM use */
    uint32_t max_vthreads;         /* cap on concurrent vthreads */
    uint32_t flags;                /* reserved, pass 0 */
} dv_vproc_attach_desc_t;

typedef struct
{
    uint64_t vram_used_bytes;
    uint64_t vram_quota_bytes;
    uint32_t vthreads_active;
    uint32_t vcores_in_use;
    uint32_t fences_in_flight;
    uint32_t flags;
} dv_vproc_info_t;

typedef struct
{
    uint32_t entry_addr;           /* opaque to client; meaning is driver-local */
    uint32_t arg;                  /* passed to vthread on first dispatch */
    uint8_t  priority;             /* 0=high, 255=idle */
    uint32_t stack_vram_bytes;     /* private stack size in VRAM */
    int32_t  vcore_affinity;       /* -1 = any, otherwise vcore index */
    uint32_t flags;
} dv_vthread_desc_t;

int32_t dv_vproc_attach(const dv_vproc_attach_desc_t *desc, dv_vproc_t *out);
int32_t dv_vproc_detach(dv_vproc_t v);                       /* auto on process death */
int32_t dv_vproc_info  (dv_vproc_t v, dv_vproc_info_t *out);
int32_t dv_vproc_set_quota(dv_vproc_t v, uint64_t new_quota_bytes);

int32_t dv_vthread_create  (dv_vproc_t v, const dv_vthread_desc_t *d, dv_vthread_t *out);
int32_t dv_vthread_destroy (dv_vthread_t t);
int32_t dv_vthread_priority(dv_vthread_t t, uint8_t prio);
int32_t dv_vthread_yield   (dv_vthread_t t);
int32_t dv_vthread_join    (dv_vthread_t t, uint32_t timeout_ms);
int32_t dv_vthread_wait_idle(dv_vthread_t t, uint32_t timeout_ms);

/* ==========================================================================
 *  3. vcore execution model                opcodes 0x0120..0x012F
 * ========================================================================== */

#define DV_VCORE_COUNT            0x0120
#define DV_VCORE_INFO             0x0121
#define DV_VCORE_PARAMS_SET       0x0122
#define DV_VCORE_DISPATCH         0x0123  /* CUDA-style grid<<<>>> launch */
#define DV_VCORE_AFFINITY         0x0124
#define DV_VCORE_QUERY_STATE      0x0125

/* vcore type -- different vcores in the same driver may have different
 * personalities (general / vertex / fragment / compute / fixed-function). */
typedef enum
{
    DV_VCORE_GENERAL    = 0,
    DV_VCORE_VERTEX     = 1,
    DV_VCORE_FRAGMENT   = 2,
    DV_VCORE_COMPUTE    = 3,
    DV_VCORE_FIXED_2D   = 4,
} dv_vcore_kind_t;

typedef struct
{
    uint32_t  index;
    dv_vcore_kind_t kind;
    uint32_t  register_file_bytes;
    uint32_t  shared_mem_bytes;
    uint32_t  max_threads_per_block;
    uint32_t  warp_or_wave_size;     /* SIMD width; 1 on scalar backends */
    uint32_t  capabilities;          /* vcore-local subset of DV_CAP_* */
} dv_vcore_info_t;

/* Per-vthread vcore parameters (CUDA-style: registers, shared mem, ...) */
typedef struct
{
    uint32_t registers_per_thread;
    uint32_t shared_mem_bytes;
    uint32_t block_threads;
} dv_vcore_params_t;

/* Generic dispatch descriptor used by both vcore_dispatch and compute_dispatch.
 * Grid = total work units; block = work units per cooperating group. */
typedef struct
{
    uint32_t grid_x, grid_y, grid_z;
    uint32_t block_x, block_y, block_z;
    const void *args;          /* small uniform-style argument blob */
    uint32_t    args_size;
    dv_fence_t  fence_signal;  /* DV_HANDLE_NONE = no signal */
} dv_dispatch_t;

int32_t dv_vcore_count(uint32_t *out_count);
int32_t dv_vcore_info(uint32_t index, dv_vcore_info_t *out);
int32_t dv_vcore_params_set(dv_vthread_t t, const dv_vcore_params_t *p);
int32_t dv_vcore_dispatch(dv_vthread_t t, const dv_dispatch_t *d);
int32_t dv_vcore_affinity(dv_vthread_t t, int32_t vcore_index);  /* -1 = any */
int32_t dv_vcore_query_state(uint32_t vcore_index, uint32_t *out_busy_pct);

/* ==========================================================================
 *  4. Display and mode (read-only)         opcodes 0x0200..0x021F
 *
 *  Only read/query operations live in the data plane.  All writes
 *  that change global display state -- mode_set, gamma_set, scanout
 *  routing, multi-display arrangement, power state, gpu reset --
 *  belong to the control plane in <dob/DobVideoControl.h> and are
 *  reached by traditional IPC to the driver's service port.  Any
 *  process (dobinterface, Settings, a third-party tool) calls the
 *  control plane to alter display state.  vprocesses can still call
 *  the read-only entries below in their hot path without paying for
 *  IPC; non-vprocess clients use the control plane equivalents.
 * ========================================================================== */

#define DV_MODE_LIST              0x0200
#define DV_MODE_GET_CURRENT       0x0202
#define DV_DISPLAY_COUNT          0x0210
#define DV_DISPLAY_INFO           0x0211
#define DV_VSYNC_WAIT             0x0212
#define DV_GAMMA_GET              0x0215
#define DV_PALETTE_SET            0x0216    /* per-surface palette (PAL8 textures); not a global */

typedef struct
{
    uint32_t    width;
    uint32_t    height;
    uint32_t    refresh_hz;
    dv_format_t format;
    uint32_t    flags;       /* reserved */
} dv_mode_t;

typedef struct
{
    uint32_t display_id;
    uint32_t physical_w_mm, physical_h_mm;
    uint32_t connected;       /* boolean */
    char     name[32];
    uint8_t  edid_present;
    /* edid bytes available via a separate retrieve call if needed */
} dv_display_info_t;

/* Gamma ramp: 256 entries, 16-bit per channel R/G/B, packed. */
typedef struct
{
    uint16_t r[256];
    uint16_t g[256];
    uint16_t b[256];
} dv_gamma_ramp_t;

int32_t dv_mode_list   (uint32_t display_id, dv_mode_t *out, uint32_t *count);
int32_t dv_mode_get_current(uint32_t display_id, dv_mode_t *out);
int32_t dv_display_count(uint32_t *out_count);
int32_t dv_display_info(uint32_t display_id, dv_display_info_t *out);
int32_t dv_vsync_wait  (uint32_t display_id, uint32_t timeout_ms);
int32_t dv_gamma_get   (uint32_t display_id, dv_gamma_ramp_t *out);
int32_t dv_palette_set (dv_surface_t s, const uint32_t *palette_argb, uint32_t count);

/* ==========================================================================
 *  5. VRAM                                  opcodes 0x0300..0x031F
 * ========================================================================== */

#define DV_VRAM_ALLOC             0x0300
#define DV_VRAM_FREE              0x0301
#define DV_VRAM_INFO              0x0302
#define DV_VRAM_MAP               0x0303
#define DV_VRAM_UNMAP             0x0304
#define DV_VRAM_COPY              0x0305
#define DV_VRAM_LOCK              0x0306
#define DV_VRAM_UNLOCK            0x0307

/* VRAM region type -- guides driver placement and access patterns. */
typedef enum
{
    DV_VRAM_COLOR       = 1,   /* renderable color buffer */
    DV_VRAM_DEPTH       = 2,
    DV_VRAM_TEXTURE     = 3,
    DV_VRAM_VBUF        = 4,
    DV_VRAM_IBUF        = 5,
    DV_VRAM_UNIFORM     = 6,
    DV_VRAM_COMMAND     = 7,
    DV_VRAM_SCRATCH     = 8,
    DV_VRAM_STAGING     = 9,   /* CPU-staging: visible to host, slow GPU */
    DV_VRAM_IMMUTABLE   = 10,  /* uploaded once, cached aggressively */
} dv_vram_kind_t;

#define DV_VRAM_FLAG_MAPPABLE    (1u << 0)   /* eligible for vram_map */
#define DV_VRAM_FLAG_SHARED      (1u << 1)   /* visible to other vprocs (dangerous) */
#define DV_VRAM_FLAG_DMA         (1u << 2)   /* DMA-capable */

typedef struct
{
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint64_t largest_contig_bytes;
    uint32_t fragmentation_pct;     /* 0..100 */

    /* Cmdlist pool: per-window state lives in the driver's BSS
     * pool, not VRAM.  VRAM numbers above are largely static during
     * a session; these fields track real "system pressure" as the
     * user opens/closes windows.  Zero on drivers without a pool. */
    uint32_t cmdlist_pool_total_bytes;
    uint32_t cmdlist_pool_used_bytes;
    uint32_t cmdlist_count;          /* active cmdlist handles */
} dv_vram_info_t;

typedef struct
{
    dv_buffer_t src;     uint64_t src_offset;
    dv_buffer_t dst;     uint64_t dst_offset;
    uint64_t    bytes;
    dv_fence_t  fence_signal;
} dv_vram_copy_t;

int32_t dv_vram_alloc (dv_vproc_t v, dv_vram_kind_t kind, uint64_t bytes,
                       uint32_t flags, dv_buffer_t *out);
int32_t dv_vram_free  (dv_buffer_t b);
int32_t dv_vram_info  (dv_vproc_t v, dv_vram_info_t *out);
int32_t dv_vram_map   (dv_buffer_t b, void **out_vaddr);
int32_t dv_vram_unmap (dv_buffer_t b);
int32_t dv_vram_copy  (const dv_vram_copy_t *op);
int32_t dv_vram_lock  (dv_buffer_t b, uint32_t timeout_ms);
int32_t dv_vram_unlock(dv_buffer_t b);

/* ==========================================================================
 *  6. Surface (2D framebuffer-like target)  opcodes 0x0400..0x041F
 * ========================================================================== */

#define DV_SURFACE_CREATE         0x0400
#define DV_SURFACE_DESTROY        0x0401
#define DV_SURFACE_INFO           0x0402
#define DV_SURFACE_RESIZE         0x0403
#define DV_SURFACE_CLEAR          0x0404
#define DV_SURFACE_LOCK           0x0405
#define DV_SURFACE_UNLOCK         0x0406

#define DV_SURF_FLAG_MAPPABLE       (1u << 0)
#define DV_SURF_FLAG_RENDERTARGET   (1u << 1)
#define DV_SURF_FLAG_SCANOUT        (1u << 2)   /* eligible for scanout_set */
#define DV_SURF_FLAG_SHARED         (1u << 3)
#define DV_SURF_FLAG_PERSISTENT     (1u << 4)   /* survives mode change */
#define DV_SURF_FLAG_SYSRAM         (1u << 5)   /* pixels in driver system RAM:
                                                 * zero VRAM cost and no VRAM
                                                 * quota. For blit-composed
                                                 * content (window bodies) on
                                                 * VRAM-starved adapters. */

typedef struct
{
    uint32_t    width, height;
    dv_format_t format;
    uint32_t    flags;
} dv_surface_desc_t;

typedef struct
{
    uint32_t    width, height;
    dv_format_t format;
    uint32_t    pitch_bytes;
    uint32_t    flags;
} dv_surface_info_t;

int32_t dv_surface_create (dv_vproc_t v, const dv_surface_desc_t *d, dv_surface_t *out);
int32_t dv_surface_destroy(dv_surface_t s);
int32_t dv_surface_info   (dv_surface_t s, dv_surface_info_t *out);
int32_t dv_surface_resize (dv_surface_t s, uint32_t new_w, uint32_t new_h);
int32_t dv_surface_clear  (dv_surface_t s, dv_color_t color);
int32_t dv_surface_lock   (dv_surface_t s, void **out_pixels, uint32_t *out_pitch);
int32_t dv_surface_unlock (dv_surface_t s);

/* ==========================================================================
 *  7. Texture (sampled resource)            opcodes 0x0420..0x043F
 * ========================================================================== */

#define DV_TEXTURE_CREATE         0x0420
#define DV_TEXTURE_DESTROY        0x0421
#define DV_TEXTURE_UPLOAD         0x0422
#define DV_TEXTURE_UPDATE_REGION  0x0423
#define DV_TEXTURE_DOWNLOAD       0x0424
#define DV_TEXTURE_GENERATE_MIPS  0x0425
#define DV_TEXTURE_BIND           0x0426

#define DV_TEX_FLAG_MIPS            (1u << 0)
#define DV_TEX_FLAG_RENDERTARGET    (1u << 1)
#define DV_TEX_FLAG_MAPPABLE        (1u << 2)
#define DV_TEX_FLAG_DYNAMIC         (1u << 3)   /* updated frequently */
#define DV_TEX_FLAG_IMMUTABLE       (1u << 4)   /* uploaded once */
#define DV_TEX_FLAG_STORAGE         (1u << 5)   /* compute read-write */

typedef struct
{
    uint32_t    width, height;
    dv_format_t format;
    uint32_t    mip_levels;     /* 0 = compute from size */
    uint32_t    flags;
} dv_texture_desc_t;

int32_t dv_texture_create        (dv_vproc_t v, const dv_texture_desc_t *d,
                                  dv_texture_t *out);
int32_t dv_texture_destroy       (dv_texture_t t);
int32_t dv_texture_upload        (dv_texture_t t, const void *src, size_t src_bytes);
int32_t dv_texture_update_region (dv_texture_t t, dv_rect_t r,
                                  const void *src, uint32_t src_pitch);
int32_t dv_texture_download      (dv_texture_t t, void *dst, size_t dst_bytes);
int32_t dv_texture_generate_mips (dv_texture_t t);
int32_t dv_texture_bind          (uint32_t slot, dv_texture_t t);

/* ==========================================================================
 *  8. Buffer (vertex / index / uniform / generic)  opcodes 0x0440..0x045F
 * ========================================================================== */

#define DV_BUFFER_CREATE          0x0440
#define DV_BUFFER_DESTROY         0x0441
#define DV_BUFFER_UPDATE          0x0442
#define DV_BUFFER_MAP             0x0443
#define DV_BUFFER_UNMAP           0x0444
#define DV_BUFFER_BIND            0x0445

/* Buffer type matches dv_vram_kind_t for the storage roles, plus a
 * generic role for client-defined use. */
typedef enum
{
    DV_BUF_VERTEX       = DV_VRAM_VBUF,
    DV_BUF_INDEX        = DV_VRAM_IBUF,
    DV_BUF_UNIFORM      = DV_VRAM_UNIFORM,
    DV_BUF_GENERIC      = 100,
} dv_buffer_kind_t;

int32_t dv_buffer_create (dv_vproc_t v, dv_buffer_kind_t kind, uint64_t bytes,
                          uint32_t flags, dv_buffer_t *out);
int32_t dv_buffer_destroy(dv_buffer_t b);
int32_t dv_buffer_update (dv_buffer_t b, uint64_t offset,
                          const void *src, uint64_t bytes);
int32_t dv_buffer_map    (dv_buffer_t b, void **out_vaddr);
int32_t dv_buffer_unmap  (dv_buffer_t b);
int32_t dv_buffer_bind   (uint32_t slot, dv_buffer_kind_t kind,
                          dv_buffer_t b, uint64_t offset);

/* ==========================================================================
 *  9. 2D primitives                         opcodes 0x0500..0x052F
 * ========================================================================== */

#define DV_FILL_RECT              0x0500
#define DV_FILL_GRADIENT          0x0501
#define DV_BLIT                   0x0510
#define DV_BLIT_STRETCHED         0x0511
#define DV_BLIT_ALPHA             0x0512
#define DV_BLIT_YUV_TO_RGB        0x0513
#define DV_COPY_REGION            0x0514
#define DV_DRAW_LINE              0x0520
#define DV_DRAW_RECT_OUTLINE      0x0521
#define DV_DRAW_POLYGON           0x0522
#define DV_DRAW_CIRCLE            0x0523
#define DV_DRAW_GLYPHS            0x0524
#define DV_SCROLL_REGION          0x0525   /* legacy VGA-class accelerated scroll */

typedef enum
{
    DV_GRADIENT_HORIZONTAL = 0,
    DV_GRADIENT_VERTICAL   = 1,
    DV_GRADIENT_DIAGONAL   = 2,
    DV_GRADIENT_RADIAL     = 3,
} dv_gradient_dir_t;

typedef struct
{
    uint32_t glyph_index;     /* index into the bound glyph atlas */
    int32_t  x, y;            /* baseline origin in dst surface */
} dv_glyph_t;

int32_t dv_fill_rect       (dv_surface_t dst, dv_rect_t r, dv_color_t c);
int32_t dv_fill_gradient   (dv_surface_t dst, dv_rect_t r,
                            dv_color_t a, dv_color_t b, dv_gradient_dir_t dir);
int32_t dv_blit            (dv_surface_t src, dv_rect_t sr,
                            dv_surface_t dst, dv_point_t dp);
int32_t dv_blit_stretched  (dv_surface_t src, dv_rect_t sr,
                            dv_surface_t dst, dv_rect_t dr);
int32_t dv_blit_alpha      (dv_surface_t src, dv_rect_t sr,
                            dv_surface_t dst, dv_point_t dp, uint8_t alpha);
int32_t dv_blit_pixel_alpha(dv_surface_t src, dv_rect_t sr,
                            dv_surface_t dst, dv_point_t dp);
int32_t dv_blit_yuv_to_rgb (dv_buffer_t y_plane, dv_buffer_t u_plane, dv_buffer_t v_plane,
                            uint32_t src_w, uint32_t src_h, dv_format_t src_fmt,
                            dv_surface_t dst, dv_rect_t dr);
int32_t dv_copy_region     (dv_surface_t src, dv_rect_t sr,
                            dv_surface_t dst, dv_point_t dp);
int32_t dv_draw_line       (dv_surface_t dst, dv_point_t a, dv_point_t b,
                            uint32_t thickness, dv_color_t c);
int32_t dv_draw_rect_outline(dv_surface_t dst, dv_rect_t r,
                             uint32_t thickness, dv_color_t c);
int32_t dv_draw_polygon    (dv_surface_t dst, const dv_point_t *pts, uint32_t count,
                            dv_color_t c);
int32_t dv_draw_circle     (dv_surface_t dst, dv_point_t center, uint32_t radius,
                            uint32_t thickness, dv_color_t c);
int32_t dv_draw_glyphs     (dv_surface_t dst, dv_texture_t glyph_atlas,
                            const dv_glyph_t *glyphs, uint32_t count, dv_color_t c);
int32_t dv_scroll_region   (dv_surface_t s, dv_rect_t r, int32_t dx, int32_t dy,
                            dv_color_t fill);

/* ==========================================================================
 * 10. 3D pipeline and draw                  opcodes 0x0600..0x063F
 * ========================================================================== */

#define DV_PIPELINE_CREATE        0x0600
#define DV_PIPELINE_DESTROY       0x0601
#define DV_PIPELINE_BIND          0x0602
#define DV_VIEWPORT_SET           0x0610
#define DV_RENDERTARGET_SET       0x0611
#define DV_TRANSFORM_SET          0x0612
#define DV_CLEAR                  0x0613
#define DV_DEPTH_STATE_SET        0x0614
#define DV_BLEND_STATE_SET        0x0615
#define DV_RASTER_STATE_SET       0x0616
#define DV_DRAW                   0x0620
#define DV_DRAW_INDEXED           0x0621
#define DV_DRAW_INSTANCED         0x0622

typedef enum
{
    DV_PRIM_POINTS         = 0,
    DV_PRIM_LINES          = 1,
    DV_PRIM_LINE_STRIP     = 2,
    DV_PRIM_TRIANGLES      = 3,
    DV_PRIM_TRIANGLE_STRIP = 4,
    DV_PRIM_TRIANGLE_FAN   = 5,
} dv_primitive_t;

typedef enum
{
    DV_TRANSFORM_MODEL = 0,
    DV_TRANSFORM_VIEW  = 1,
    DV_TRANSFORM_PROJ  = 2,
} dv_transform_slot_t;

typedef enum
{
    DV_CULL_NONE  = 0,
    DV_CULL_BACK  = 1,
    DV_CULL_FRONT = 2,
} dv_cull_mode_t;

typedef enum
{
    DV_FILL_SOLID     = 0,
    DV_FILL_WIREFRAME = 1,
} dv_fill_mode_t;

typedef enum
{
    DV_CMP_NEVER       = 0,
    DV_CMP_LESS        = 1,
    DV_CMP_EQUAL       = 2,
    DV_CMP_LEQUAL      = 3,
    DV_CMP_GREATER     = 4,
    DV_CMP_NOTEQUAL    = 5,
    DV_CMP_GEQUAL      = 6,
    DV_CMP_ALWAYS      = 7,
} dv_compare_t;

typedef enum
{
    DV_BLEND_ZERO          = 0,
    DV_BLEND_ONE           = 1,
    DV_BLEND_SRC_ALPHA     = 2,
    DV_BLEND_INV_SRC_ALPHA = 3,
    DV_BLEND_DST_ALPHA     = 4,
    DV_BLEND_INV_DST_ALPHA = 5,
    DV_BLEND_SRC_COLOR     = 6,
    DV_BLEND_INV_SRC_COLOR = 7,
} dv_blend_factor_t;

/* Vertex layout -- single binding for now; multi-binding is a future
 * extension and would change this struct, not the opcode. */
typedef struct
{
    uint32_t    offset;
    dv_format_t format;       /* per-attribute scalar/vector format */
    uint32_t    semantic;     /* driver-defined semantic id (POSITION/NORMAL/...) */
} dv_vertex_attr_t;

typedef struct
{
    uint32_t                stride;
    uint32_t                attr_count;
    const dv_vertex_attr_t *attrs;
} dv_vertex_layout_t;

typedef struct
{
    dv_shader_t              vertex_shader;     /* may be 0 = fixed function */
    dv_shader_t              fragment_shader;
    dv_vertex_layout_t       layout;
    dv_primitive_t           topology;
    uint32_t                 flags;
} dv_pipeline_desc_t;

typedef struct
{
    bool         depth_test_enable;
    bool         depth_write_enable;
    dv_compare_t depth_func;
    bool         stencil_enable;
    /* stencil ops left out of this skeleton; add when first driver needs them */
} dv_depth_state_t;

typedef struct
{
    bool              enable;
    dv_blend_factor_t src_color, dst_color;
    dv_blend_factor_t src_alpha, dst_alpha;
} dv_blend_state_t;

typedef struct
{
    dv_cull_mode_t cull;
    dv_fill_mode_t fill;
    bool           scissor_enable;
    dv_rect_t      scissor_rect;
} dv_raster_state_t;

typedef struct
{
    dv_texture_t color_attachments[8];
    uint32_t     color_count;
    dv_texture_t depth_attachment;     /* DV_HANDLE_NONE = none */
} dv_rendertarget_set_t;

#define DV_CLEAR_COLOR    (1u << 0)
#define DV_CLEAR_DEPTH    (1u << 1)
#define DV_CLEAR_STENCIL  (1u << 2)

int32_t dv_pipeline_create (dv_vproc_t v, const dv_pipeline_desc_t *d,
                            dv_pipeline_t *out);
int32_t dv_pipeline_destroy(dv_pipeline_t p);
int32_t dv_pipeline_bind   (dv_pipeline_t p);
int32_t dv_viewport_set    (int32_t x, int32_t y, uint32_t w, uint32_t h,
                            float min_depth, float max_depth);
int32_t dv_rendertarget_set(const dv_rendertarget_set_t *rt);
int32_t dv_transform_set   (dv_transform_slot_t slot, const dv_mat4_t *m);
int32_t dv_clear           (uint32_t flags, dv_color_t color, float depth, uint8_t stencil);
int32_t dv_depth_state_set (const dv_depth_state_t *s);
int32_t dv_blend_state_set (const dv_blend_state_t *s);
int32_t dv_raster_state_set(const dv_raster_state_t *s);
int32_t dv_draw            (dv_primitive_t prim, uint32_t first_vertex, uint32_t vertex_count);
int32_t dv_draw_indexed    (dv_primitive_t prim, uint32_t index_count,
                            uint32_t first_index, int32_t base_vertex);
int32_t dv_draw_instanced  (dv_primitive_t prim, uint32_t index_count,
                            uint32_t instance_count, uint32_t first_index, int32_t base_vertex);

/* ==========================================================================
 * 11. Programmable shaders                  opcodes 0x0700..0x070F
 * ========================================================================== */

#define DV_SHADER_LOAD            0x0700
#define DV_SHADER_DESTROY         0x0701
#define DV_SHADER_QUERY_LANG      0x0702
#define DV_SHADER_REFLECT         0x0703

typedef enum
{
    DV_SHADER_VERTEX    = 1,
    DV_SHADER_FRAGMENT  = 2,
    DV_SHADER_COMPUTE   = 3,
    DV_SHADER_GEOMETRY  = 4,
} dv_shader_stage_t;

/* Shader source format -- what the binary blob payload actually is.
 * The driver advertises which it accepts via shader_query_lang. */
typedef enum
{
    DV_SHADER_LANG_NONE     = 0,
    DV_SHADER_LANG_SPIRV    = 1,
    DV_SHADER_LANG_GLSL_SRC = 2,   /* textual GLSL */
    DV_SHADER_LANG_NATIVE   = 3,   /* driver-specific IR */
    /* MainDOB .mem blob -- i386 ET_DYN PIC ELF, R_386_RELATIVE-only,
     * exports __mem_exports symbol (struct of function pointers).
     * Convention: dv_compute_load with this lang loads the blob into
     * VRAM, applies relocations, resolves __mem_exports, and the
     * returned dv_compute_t handle is the VRAM address of that
     * symbol (cast to dv_compute_t).  The caller treats the handle
     * as a pointer to its private exports struct and calls function
     * pointers directly -- no dv_compute_dispatch needed.  Used by
     * dobinterface to host its rendering routines in VRAM. */
    DV_SHADER_LANG_MEM      = 4,
} dv_shader_lang_t;

typedef struct
{
    dv_shader_stage_t stage;
    dv_shader_lang_t  lang;
    const void       *blob;
    size_t            blob_size;
    const char       *entry;       /* NULL -> "main" */
} dv_shader_desc_t;

typedef struct
{
    uint32_t uniform_count;
    uint32_t sampler_count;
    uint32_t storage_count;
    uint32_t input_count;
    /* detailed per-binding info via a separate query; not in this skeleton */
} dv_shader_reflect_t;

int32_t dv_shader_load      (dv_vproc_t v, const dv_shader_desc_t *d, dv_shader_t *out);
int32_t dv_shader_destroy   (dv_shader_t sh);
int32_t dv_shader_query_lang(dv_shader_lang_t *out_supported_mask);  /* OR of DV_SHADER_LANG_* */
int32_t dv_shader_reflect   (dv_shader_t sh, dv_shader_reflect_t *out);

/* ==========================================================================
 * 12. Compute programs                       opcodes 0x0800..0x082F
 *
 * The hot path for mining and LLM: load once, dispatch many times with
 * varying inputs.  A "compute program" is the executable code unit the
 * vcores run when dispatched (analogous to a CUDA kernel; named
 * "compute program" specifically to avoid overloading the word
 * "kernel", which in MainDOB is reserved for the microkernel).
 * Persistent binds keep heavy resources (model weights, lookup tables)
 * attached to the program across invocations; chain and iterative
 * variants avoid CPU round-trips on the inner loop.
 * ========================================================================== */

#define DV_COMPUTE_LOAD               0x0800
#define DV_COMPUTE_DESTROY            0x0801
#define DV_COMPUTE_QUERY_RESOURCES    0x0802
#define DV_COMPUTE_BIND_PERSISTENT    0x0803
#define DV_COMPUTE_UNBIND_PERSISTENT  0x0804
#define DV_COMPUTE_PERSISTENT_STATE   0x0805

#define DV_COMPUTE_DISPATCH           0x0810   /* hot path: small-args dispatch */
#define DV_COMPUTE_DISPATCH_CHAIN     0x0811   /* sequence, GPU-side */
#define DV_COMPUTE_DISPATCH_ITERATIVE 0x0812   /* loop, GPU-side */
#define DV_COMPUTE_DISPATCH_INDIRECT  0x0813   /* grid_dim from a buffer */
#define DV_COMPUTE_DISPATCH_BATCH     0x0814   /* N dispatches submitted in one call */
#define DV_COMPUTE_SIGNAL_ON_COND     0x0815   /* fence signaled when condition holds */

typedef struct
{
    dv_shader_lang_t lang;
    const void      *blob;
    size_t           blob_size;
    const char      *entry;       /* function name, NULL -> "main" */
} dv_compute_desc_t;

typedef struct
{
    uint32_t registers_used;
    uint32_t shared_mem_required;
    uint32_t max_threads_per_block;
    uint32_t preferred_block_size;
} dv_compute_resources_t;

typedef enum
{
    DV_BIND_READ        = 0,   /* compute program reads only -- model weights, LUT */
    DV_BIND_WRITE       = 1,   /* compute program writes only */
    DV_BIND_READWRITE   = 2,   /* both -- KV cache, accumulator */
} dv_bind_access_t;

typedef enum
{
    DV_BIND_BUFFER  = 1,
    DV_BIND_TEXTURE = 2,
} dv_bind_kind_t;

typedef struct
{
    uint32_t          slot;
    dv_bind_kind_t    kind;
    union {
        dv_buffer_t   buffer;
        dv_texture_t  texture;
    } resource;
    dv_bind_access_t  access;
} dv_compute_bind_t;

/* dispatch_iterative: applies an additive step rule to a 32-bit field
 * inside the args struct on each iteration.  For mining: nonce_offset =
 * iter * block_size, grid stays constant.  For more complex stepping
 * use a chain or compute it inside the program. */
typedef struct
{
    uint32_t arg_offset;     /* byte offset in `args` where the counter sits */
    int32_t  step;           /* added per iteration (negative allowed) */
    uint32_t initial;        /* starting value at iter 0 */
} dv_iter_step_t;

typedef struct
{
    dv_compute_t          program;
    uint32_t             iter_count;
    uint32_t             grid_x, grid_y, grid_z;
    uint32_t             block_x, block_y, block_z;
    void                *args;          /* mutable; driver writes the step in place */
    uint32_t             args_size;
    const dv_iter_step_t *steps;
    uint32_t             step_count;
    dv_fence_t           fence_signal;
} dv_dispatch_iterative_t;

/* dispatch_chain: a sequence of dispatches where the GPU runs them
 * back-to-back without returning to the CPU.  links[] describes
 * memory dependencies between adjacent stages (output of i feeds
 * input of i+1).  Forward pass of an LLM is a chain of this kind. */
typedef struct
{
    dv_compute_t program;
    uint32_t     grid_x, grid_y, grid_z;
    uint32_t     block_x, block_y, block_z;
    const void  *args;
    uint32_t     args_size;
} dv_chain_node_t;

typedef struct
{
    const dv_chain_node_t *nodes;
    uint32_t               count;
    dv_fence_t             fence_signal;
} dv_dispatch_chain_t;

/* dispatch_indirect: the (grid_x, grid_y, grid_z) triple is read from
 * the GPU buffer at `args_buffer + offset`, allowing dispatch shape to
 * be computed by a previous stage. */
typedef struct
{
    dv_compute_t   program;
    dv_buffer_t   args_buffer;
    uint64_t      offset;
    uint32_t      block_x, block_y, block_z;
    const void   *args;
    uint32_t      args_size;
    dv_fence_t    fence_signal;
} dv_dispatch_indirect_t;

/* signal_on_cond: the program writes a status word into `cond_buffer`
 * at `cond_offset`.  The fence is signaled when that word matches
 * `match_value` under `match_mask`.  Mining: the program writes the
 * winning nonce + a "found" flag; the fence fires the moment the flag
 * goes hot. */
typedef struct
{
    dv_compute_t program;
    dv_buffer_t  cond_buffer;
    uint64_t     cond_offset;
    uint32_t     match_value;
    uint32_t     match_mask;
    dv_fence_t   fence_signal;
} dv_signal_on_cond_t;

int32_t dv_compute_load    (dv_vproc_t v, const dv_compute_desc_t *d, dv_compute_t *out);
int32_t dv_compute_destroy (dv_compute_t k);
int32_t dv_compute_query_resources(dv_compute_t k, dv_compute_resources_t *out);

int32_t dv_compute_bind_persistent  (dv_compute_t k, const dv_compute_bind_t *b);
int32_t dv_compute_unbind_persistent(dv_compute_t k, uint32_t slot);
int32_t dv_compute_persistent_state (dv_compute_t k, dv_buffer_t state_buffer);

int32_t dv_compute_dispatch          (dv_compute_t k, const dv_dispatch_t *d);
int32_t dv_compute_dispatch_chain    (dv_vproc_t v, const dv_dispatch_chain_t *c);
int32_t dv_compute_dispatch_iterative(dv_vproc_t v, const dv_dispatch_iterative_t *it);
int32_t dv_compute_dispatch_indirect (dv_vproc_t v, const dv_dispatch_indirect_t *ind);
int32_t dv_compute_dispatch_batch    (dv_vproc_t v, const dv_dispatch_t *array, uint32_t count);
int32_t dv_compute_signal_on_cond    (dv_vproc_t v, const dv_signal_on_cond_t *s);

/* ==========================================================================
 * 13. Compositing and scanout               opcodes 0x0900..0x091F
 * ========================================================================== */

#define DV_LAYER_CREATE           0x0900
#define DV_LAYER_DESTROY          0x0901
#define DV_LAYER_UPDATE           0x0902
#define DV_LAYER_SET_TRANSFORM    0x0903
#define DV_LAYER_SET_VISIBLE      0x0904
#define DV_COMPOSE                0x0910
#define DV_PAGE_FLIP              0x0911
#define DV_COMPOSE_RECT           0x0912  /* recompose only a dirty rectangle */

typedef struct
{
    dv_surface_t source;
    int32_t      z;             /* higher = on top */
    uint8_t      alpha;         /* 0=transparent, 255=opaque */
    bool         visible;
    /* When true, the compositor honors the source pixel's alpha
     * channel: pixels where (src & 0xFF000000) == 0 are skipped
     * entirely (src is treated as 0% coverage), other pixels are
     * blitted at full opacity (this is straight-alpha, not pre-
     * multiplied -- anti-aliased edges will fringe).  Used by the
     * cursor layer where the surface contains a small bitmap with
     * fully opaque shape pixels and fully transparent background.
     * For full-rect opaque content (windows, desktop), leave false
     * and use the per-layer `alpha` field for fade effects. */
    bool         use_pixel_alpha;
    dv_rect_t    src_rect;
    dv_rect_t    dst_rect;
    /* Retained-mode alternative to `source`.  When != DV_HANDLE_NONE,
     * the compositor renders this layer by re-executing the command
     * list at compose time instead of blitting from a surface.  src_rect
     * is ignored in cmdlist mode (the list draws absolute into dst_rect).
     * dst_rect.{x,y} translates the cmdlist's coordinate origin; dst_rect.
     * {w,h} clips drawing to that area.  Cmdlist + source are mutually
     * exclusive: cmdlist wins if both are set.  See section 13b for the
     * cmdlist API. */
    dv_cmdlist_t cmdlist;
} dv_layer_desc_t;

#define DV_FLIP_VSYNC      (1u << 0)
#define DV_FLIP_TEAR       (1u << 1)   /* present immediately, may tear */

int32_t dv_layer_create    (dv_vproc_t v, const dv_layer_desc_t *d, dv_layer_t *out);
int32_t dv_layer_destroy   (dv_layer_t l);
int32_t dv_layer_update    (dv_layer_t l, const dv_layer_desc_t *d);
int32_t dv_layer_set_transform(dv_layer_t l, const dv_mat2x3_t *xform);
int32_t dv_layer_set_visible(dv_layer_t l, bool visible);
int32_t dv_compose         (uint32_t display_id, dv_fence_t fence_signal);
int32_t dv_compose_rect    (uint32_t display_id, int32_t rx, int32_t ry,
                            uint32_t rw, uint32_t rh, dv_fence_t fence_signal);
int32_t dv_page_flip       (uint32_t display_id, uint32_t flags, dv_fence_t fence_signal);

/* ==========================================================================
 * 13b. Command lists (retained-mode draw)    opcodes 0x0930..0x094F
 *
 * A command list is a retained-mode display list: a sequence of typed
 * draw records (fill_rect, blit, draw_glyphs, ...) that the compositor
 * re-executes every time the owning layer is composed.  Storage lives
 * in driver RAM (NOT vproc VRAM quota), so cmdlists are cheap.
 *
 * Typical use:
 *   dv_cmdlist_t cl;
 *   dv_cmdlist_create(vproc, 16*1024, &cl);
 *   dv_cmdlist_reset(cl);                            // clear any previous content
 *   dv_cmdlist_fill_rect(cl, (dv_rect_t){0,0,800,600}, COLOR_BG);
 *   dv_cmdlist_draw_glyphs(cl, atlas, glyphs, n, COLOR_WHITE);
 *   // ... build up ...
 *   // Attach to a layer:
 *   dv_layer_desc_t ld = { .cmdlist = cl, .z = 10, .visible = true,
 *                          .alpha = 255, .dst_rect = {0,0,800,600} };
 *   dv_layer_update(L, &ld);
 *
 * Updates: any subsequent dv_cmdlist_reset + fill_rect/... sequence
 * replaces the list in place; the next dv_compose picks up the new
 * commands automatically -- no need to re-bind to the layer.
 *
 * Coordinate space: cmdlist commands use absolute coordinates within
 * the layer's logical canvas.  At compose time the canvas origin is
 * translated to layer.dst_rect.{x,y} and drawing is clipped to
 * dst_rect.{w,h}.  This means a window's chrome at logical (0,0)
 * appears at the window's screen position automatically.
 * ========================================================================== */

#define DV_CMDLIST_CREATE         0x0930
#define DV_CMDLIST_DESTROY        0x0931
#define DV_CMDLIST_RESET          0x0932   /* clear all recorded ops */
#define DV_CMDLIST_FILL_RECT      0x0933
#define DV_CMDLIST_BLIT           0x0934   /* opaque blit from a surface */
#define DV_CMDLIST_BLIT_ALPHA     0x0935   /* per-pixel-alpha or constant alpha */
#define DV_CMDLIST_DRAW_GLYPHS    0x0936
#define DV_CMDLIST_DRAW_LINE      0x0937   /* axis-aligned only for now */
#define DV_CMDLIST_INFO           0x0938   /* query bytes used / commands count */

typedef struct
{
    uint32_t bytes_used;
    uint32_t bytes_capacity;
    uint32_t command_count;
    uint32_t reserved;
} dv_cmdlist_info_t;

/* Create a cmdlist with given capacity (bytes).  Capacity is the upper
 * bound of recorded command storage; allocations beyond the cap return
 * DV_ERR_QUOTA on the appending call.  Sizing: a typical UI window
 * chrome+text fits in 4-16 KiB.  Storage lives in driver RAM, not VRAM. */
int32_t dv_cmdlist_create     (dv_vproc_t v, uint32_t capacity_bytes,
                               dv_cmdlist_t *out);
int32_t dv_cmdlist_destroy    (dv_cmdlist_t cl);
int32_t dv_cmdlist_reset      (dv_cmdlist_t cl);

/* Recording primitives.  Each returns DV_OK on success, DV_ERR_QUOTA
 * if the cmdlist's capacity is exhausted, DV_ERR_HANDLE if the list
 * handle is invalid.  All coordinates are in the layer's logical
 * canvas (translated by layer.dst_rect at compose). */
int32_t dv_cmdlist_fill_rect  (dv_cmdlist_t cl, dv_rect_t r, dv_color_t c);
int32_t dv_cmdlist_blit       (dv_cmdlist_t cl, dv_surface_t src,
                               dv_rect_t sr, dv_point_t dp);
int32_t dv_cmdlist_blit_alpha (dv_cmdlist_t cl, dv_surface_t src,
                               dv_rect_t sr, dv_point_t dp, uint8_t alpha,
                               bool use_pixel_alpha);
int32_t dv_cmdlist_draw_glyphs(dv_cmdlist_t cl, dv_texture_t atlas,
                               const dv_glyph_t *glyphs, uint32_t count,
                               dv_color_t color);
int32_t dv_cmdlist_draw_line  (dv_cmdlist_t cl, dv_point_t a, dv_point_t b,
                               uint32_t thickness, dv_color_t color);

int32_t dv_cmdlist_info       (dv_cmdlist_t cl, dv_cmdlist_info_t *out);

/* ==========================================================================
 * 14. Hardware cursor                       opcodes 0x0A00..0x0A0F
 * ========================================================================== */

#define DV_CURSOR_SET_BITMAP      0x0A00
#define DV_CURSOR_SET_POSITION    0x0A01
#define DV_CURSOR_SHOW            0x0A02
#define DV_CURSOR_HIDE            0x0A03

typedef struct
{
    uint32_t    width, height;
    uint32_t    hotspot_x, hotspot_y;
    dv_format_t format;       /* must be DV_FMT_RGBA8888 if alpha cursor */
    const void *pixels;
} dv_cursor_desc_t;

int32_t dv_cursor_set_bitmap  (uint32_t display_id, const dv_cursor_desc_t *d);
int32_t dv_cursor_set_position(uint32_t display_id, int32_t x, int32_t y);
int32_t dv_cursor_show        (uint32_t display_id);
int32_t dv_cursor_hide        (uint32_t display_id);

/* ==========================================================================
 * 15. Hardware overlay (video plane)        opcodes 0x0B00..0x0B0F
 * ========================================================================== */

#define DV_OVERLAY_CREATE         0x0B00
#define DV_OVERLAY_DESTROY        0x0B01
#define DV_OVERLAY_UPDATE         0x0B02
#define DV_OVERLAY_SET_VISIBLE    0x0B03

typedef struct
{
    uint32_t    display_id;
    uint32_t    src_w, src_h;
    dv_format_t src_format;     /* typically YUV* */
    uint32_t    flags;          /* reserved */
} dv_overlay_desc_t;

typedef struct
{
    dv_buffer_t y_or_packed;
    dv_buffer_t u_plane;        /* DV_HANDLE_NONE for packed formats */
    dv_buffer_t v_plane;        /* DV_HANDLE_NONE for packed/NV12     */
    dv_rect_t   dst_rect;
    uint8_t     alpha;
    uint32_t    color_key;      /* 0xFFFFFFFF = none */
} dv_overlay_update_t;

int32_t dv_overlay_create     (dv_vproc_t v, const dv_overlay_desc_t *d,
                               dv_overlay_t *out);
int32_t dv_overlay_destroy    (dv_overlay_t o);
int32_t dv_overlay_update     (dv_overlay_t o, const dv_overlay_update_t *u);
int32_t dv_overlay_set_visible(dv_overlay_t o, bool visible);

/* ==========================================================================
 * 16. Synchronization                       opcodes 0x0C00..0x0C2F
 * ========================================================================== */

#define DV_FENCE_CREATE           0x0C00
#define DV_FENCE_DESTROY          0x0C01
#define DV_FENCE_SIGNAL           0x0C02   /* explicit signal from CPU side */
#define DV_FENCE_WAIT             0x0C03
#define DV_FENCE_POLL             0x0C04

#define DV_SEMAPHORE_CREATE       0x0C10
#define DV_SEMAPHORE_DESTROY      0x0C11
#define DV_SEMAPHORE_SIGNAL       0x0C12
#define DV_SEMAPHORE_WAIT         0x0C13

#define DV_BARRIER_INSERT         0x0C20

typedef enum
{
    DV_BARRIER_RENDER_TO_SAMPLE = 1,   /* RT -> texture sampling */
    DV_BARRIER_WRITE_TO_READ    = 2,   /* compute write -> read */
    DV_BARRIER_FULL             = 3,   /* full pipeline barrier */
} dv_barrier_kind_t;

int32_t dv_fence_create   (dv_vproc_t v, dv_fence_t *out);
int32_t dv_fence_destroy  (dv_fence_t f);
int32_t dv_fence_signal   (dv_fence_t f);
int32_t dv_fence_wait     (dv_fence_t f, uint32_t timeout_ms);
int32_t dv_fence_poll     (dv_fence_t f);   /* returns DV_OK or DV_ERR_NOTREADY */

int32_t dv_semaphore_create (dv_vproc_t v, uint32_t initial, dv_semaphore_t *out);
int32_t dv_semaphore_destroy(dv_semaphore_t s);
int32_t dv_semaphore_signal (dv_semaphore_t s);
int32_t dv_semaphore_wait   (dv_semaphore_t s, uint32_t timeout_ms);

int32_t dv_barrier_insert(dv_vthread_t t, dv_barrier_kind_t kind);

/* ==========================================================================
 * 17. Capability and introspection          opcodes 0x0D00..0x0D0F
 * ========================================================================== */

#define DV_CAP_QUERY              0x0D00
#define DV_CAP_QUERY_LIMIT        0x0D01
#define DV_CAP_QUERY_FORMAT       0x0D02
#define DV_DRIVER_INFO            0x0D03

typedef struct
{
    char     name[32];           /* driver short name: "vbe", "virtio_gpu", ... */
    char     vendor[32];
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t version_patch;
    uint16_t pci_vendor_id;       /* 0 if not PCI */
    uint16_t pci_device_id;
} dv_driver_info_t;

int32_t dv_cap_query        (uint64_t *out_capabilities);
int32_t dv_cap_query_limit  (dv_limit_t which, uint64_t *out_value);
int32_t dv_cap_query_format (dv_format_t fmt, uint32_t *out_usage_flags);  /* DV_FMT_USE_* */
int32_t dv_driver_info      (dv_driver_info_t *out);

/* ==========================================================================
 * 18. Asynchronous events                   opcodes 0x0E00..0x0E0F
 *
 * Events flow driver -> client.  They are not "calls" the client makes;
 * they are codes the client receives through whatever event delivery
 * mechanism the runtime layer provides.  Documented here for protocol
 * completeness so every backend uses the same codes.
 * ========================================================================== */

#define DV_EVENT_VSYNC               0x0E00
#define DV_EVENT_FENCE_SIGNALED      0x0E01
#define DV_EVENT_DISPLAY_HOTPLUG     0x0E02
#define DV_EVENT_MODE_CHANGED        0x0E03   /* control plane just changed a display's mode */
#define DV_EVENT_GPU_RESET           0x0E04
#define DV_EVENT_VPROC_OOM           0x0E05
#define DV_EVENT_VTHREAD_FAULT       0x0E06

#endif /* MAINDOB_DOB_VIDEO_H */
