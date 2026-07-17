/* BGA driver internal state -- not part of the public protocol.
 *
 * Shared between main.c (where the dobVideo functions are
 * implemented) and any transport front-end that needs to fill
 * caller-context fields (such as the calling pid) before invoking a
 * dv_* function. */

#ifndef MAINDOB_DRIVERS_BGA_STATE_H
#define MAINDOB_DRIVERS_BGA_STATE_H

#include <dob/types.h>
#include <dob/hotplug_driver.h>
#include <dob/video.h>

#define MAX_VPROCS              16
#define MAX_SURFACES            128
#define MAX_BUFFERS             64
#define MAX_FENCES              256
#define MAX_LAYERS              64
#define MAX_VTHREADS            32
#define MAX_SUBSCRIBERS         8
#define MAX_MODE_LIST           24
#define MAX_CMDLISTS            128
#define CMDLIST_STORAGE_BYTES   (8 * 1024 * 1024)   /* 8 MiB pool, compacted on destroy.
                                                     * Was 2 MiB until a session with many
                                                     * dense windows (DobTable etc., each
                                                     * growing its cmdlist to ~512 KiB)
                                                     * hit POOL FAIL after ~4 windows.
                                                     * 8 MiB lets ~16 dense windows or
                                                     * ~100 light ones coexist; storage
                                                     * lives in BGA process BSS so it
                                                     * costs nothing in VRAM. */

typedef uint32_t bgra_t;

typedef struct vram_block
{
    uint32_t offset;
    uint32_t size;
    bool     used;
    struct vram_block *prev, *next;
} vram_block_t;

typedef struct
{
    bool      used;
    pid_t     owner_pid;
    uint64_t  vram_quota_bytes;
    uint64_t  vram_used_bytes;
    uint32_t  vthreads_active;
    uint32_t  fences_in_flight;
} vproc_t;

typedef struct
{
    bool         used;
    dv_vproc_t   owner;
    uint32_t     width, height;
    uint32_t     pitch_words;
    dv_format_t  format;
    uint32_t     flags;
    uint32_t     vram_offset;   /* UINT32_MAX se sys_pixels e' attivo */
    uint32_t     vram_bytes;
    void        *sys_pixels;    /* backing in RAM di sistema (flag SYSRAM):
                                 * zero VRAM, zero quota; NULL = VRAM */
} surface_t;

typedef struct
{
    bool        used;
    dv_vproc_t  owner;
    uint64_t    bytes;
    uint32_t    vram_offset;
} buffer_t;

typedef struct
{
    bool        used;
    dv_vproc_t  owner;
    uint64_t    target_value;
    uint64_t    current_value;
} fence_t;

typedef struct
{
    bool         used;
    dv_vproc_t   owner;
    dv_surface_t source;
    int32_t      z;
    uint8_t      alpha;
    bool         visible;
    bool         use_pixel_alpha;   /* honor src pixel alpha (0 = skip) */
    dv_rect_t    src_rect;
    dv_rect_t    dst_rect;
    dv_cmdlist_t cmdlist;           /* retained-mode source; if non-zero,
                                       compose uses this instead of `source` */
} layer_t;

/* Command list -- retained-mode display list.  Storage is a slice of
 * the global g_cmdlist_storage pool: each cmdlist owns [storage_off,
 * storage_off + capacity).  Records are written sequentially at
 * `bytes_used` (which always points just past the last record).
 * No record straddles the boundary -- append checks remaining space
 * before writing.
 *
 * Records start with a 1-byte opcode (CMDLIST_REC_*) followed by a
 * tightly-packed payload.  See cmdlist_rec_* structs in main.c. */
typedef struct
{
    bool       used;
    dv_vproc_t owner;
    uint32_t   storage_off;    /* offset into g_cmdlist_storage */
    uint32_t   capacity;       /* bytes reserved (constant after create) */
    uint32_t   bytes_used;     /* current write head, 0..capacity */
    uint32_t   command_count;  /* number of records appended since last reset */
} cmdlist_t;

typedef struct
{
    bool       used;
    dv_vproc_t owner;
    uint32_t   worker_tid;
    uint8_t    priority;
} vthread_t;

typedef struct
{
    bool     used;
    uint32_t port;
    uint32_t mask;
} subscriber_t;

typedef struct
{
    hotplug_device_t dev;
    volatile bgra_t *vram;
    uint32_t  vram_phys;
    uint32_t  vram_bytes;

    dv_mode_t mode;
    dv_mode_t mode_list_buf[MAX_MODE_LIST];
    uint32_t  mode_list_n;

    uint32_t  primary_offset[2];
    uint32_t  primary_bytes;
    uint8_t   back_page;

    /* Shadow framebuffer (solo single buffer): la compose scrive QUI,
     * in RAM di sistema, e dv_page_flip presenta con UNA copia di
     * pixel finali sul primary. Lo scanout non vede mai gli strati
     * intermedi (backbuf -> finestra sotto -> finestra sopra) che in
     * compose-diretta-nel-visibile appaiono come view-through. NULL =
     * fallback compose diretta (malloc fallita o double buffering). */
    void     *shadow;

    vram_block_t *blocks_head;

    vproc_t      vprocs[MAX_VPROCS];
    surface_t    surfaces[MAX_SURFACES];
    buffer_t     buffers[MAX_BUFFERS];
    fence_t      fences[MAX_FENCES];
    layer_t      layers[MAX_LAYERS];
    vthread_t    vthreads[MAX_VTHREADS];
    subscriber_t subs[MAX_SUBSCRIBERS];
    cmdlist_t    cmdlists[MAX_CMDLISTS];

    /* Single bump-allocated storage pool for cmdlist record streams.
     * Each cmdlist reserves [storage_off, +capacity) at create time.
     * On destroy the slice is compacted out: the top of the pool is
     * retracted (O(1)) or, if the slice is in the middle, everything
     * above it slides down and the bump pointer drops by `capacity`.
     * See dv_cmdlist_destroy in main.c.
     *
     * If both the pool and the slot table are full at create time,
     * dv_cmdlist_create returns DV_ERR_NOMEM. */
    uint8_t      cmdlist_storage[CMDLIST_STORAGE_BYTES];
    uint32_t     cmdlist_storage_used;   /* bump pointer */

    dv_surface_t scanout_source;
    volatile uint64_t vsync_count;
} bga_state_t;

/* Interni condivisi fra i TU del driver (main.c <-> transport). */
int32_t dv_compose_rect_present(uint32_t display_id,
                                int32_t rx, int32_t ry,
                                uint32_t rw, uint32_t rh,
                                dv_fence_t fence_signal);

/* The single global, defined in main.c. */
extern bga_state_t g_bga;

/* Caller pid context -- set by the transport around each invocation. */
extern pid_t bga_current_caller_pid;

/* Internal helpers used by the transport. */
void bga_notify_subscribers(uint32_t code, uint32_t display_id,
                            uint32_t arg0, uint32_t arg1);
int32_t bga_internal_scanout_set(dv_surface_t s);
int32_t bga_internal_mode_set(const dv_mode_t *m);
void    bga_recompute_mode_list(void);
void    bga_gpu_reset_full(void);
int     bga_subscribe(uint32_t port, uint32_t mask);

#endif
