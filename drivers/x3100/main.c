/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * MainDOB ATI Mach64 driver — v33, "HORZ_DIVBY2_EN giù".
 *
 * Target: Rage Mobility-P (PCI 1002:4C4D rev 0x64) on the Compaq Armada
 * E500, 8 MB SGRAM, 14.1" XGA LVDS panel CPT CLAA141XB01.
 *
 * v32 ha sistemato shadow CRTC e sync registers; foto mostra:
 *   - Linea centrale sparita (shadow+sync OK)
 *   - 7 barre prima del nero, full-height
 *   - MA barre compresse nella metà sinistra del pannello, bar 8 (nero)
 *     riempie la metà destra
 *
 * Il bug #6 è in LCD_GEN_CNTL.HORZ_DIVBY2_EN (bit 2). Il BIOS Compaq lo
 * tiene attivo per pilotare l'output VGA-text a metà pixel clock; quando
 * facciamo modeset verso 1024×768 graphics non lo puliamo, e il chip
 * continua a dimezzare il pixel clock orizzontale. Risultato: ogni
 * fb-pixel occupa 2 panel-pixel, 1024 fb-px → 512 panel-px = la
 * compressione 1:2 in foto.
 *
 * Verifica da BIOS dump: LCD_GEN_CNTL = 0x407524DE
 *   byte basso 0xDE = 1101_1110, bit 2 = 1 → HORZ_DIVBY2_EN attivo.
 *
 * atyfb in aty_var_to_crtc fa la pulizia completa:
 *   lcd_gen_cntl &= ~(HORZ_DIVBY2_EN | DIS_HOR_CRT_DIVBY2 | TVCLK_PM_EN
 *                   | USE_SHADOWED_ROWCUR | SHADOW_EN | SHADOW_RW_EN);
 *   lcd_gen_cntl |= DONT_SHADOW_VPAR | LOCK_8DOT;
 *
 * v33 replica esattamente questa sequenza, estendendo lo step 2.5 di
 * v32. Sole modifiche rispetto a v32: più bit nella mask di clear di
 * LCD_GEN_CNTL e SET esplicito di DONT_SHADOW_VPAR + LOCK_8DOT.
 *
 * Storia:
 *   v15..v29 — sviluppo iterativo, pitch già corretta
 *   v30      — regressione pitch
 *   v31      — fix dei 4 bug iniziali (pitch, DSP, PLL, EXT_VERT_STRETCH bit 12)
 *   v32      — fix #5: shadow CRTC + H/V_SYNC_STRT_WID
 *   v33      — fix #6: HORZ_DIVBY2_EN e altri bit di LCD_GEN_CNTL
 *
 * Conferme cross-sorgente:
 *   - atyfb_base.c::aty_var_to_crtc + aty_set_crtc (Linux master, Torvalds)
 *   - atyfb x3100_ct.c::aty_dsp_gt + aty_valid_pll_ct (Daniel Mantione)
 *   - xorg-xf86-video-mach64/atiregs.h bit definitions
 *   - PRG-215R3-00-10 Appendix K
 *   - atyfb boot log HP Omnibook 4150B (chip+panel identico)
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include <dob/types.h>
#include <dob/hotplug_driver.h>
#include <dob/server.h>
#include <dob/registry.h>
#include <dob/video.h>
#include <dob/ipc.h>            /* dob_ipc_post — push events to subscribers */
#include <DobVideoControl.h>    /* DOBVC_EVENT_MODE_CHANGED                  */

#include <DobSettings.h>

#include "x3100_state.h"
#include "x3100_hw.h"
#include "x3100_2d.h"

/* ==========================================================================
 *  ESPERIMENTO DIAGNOSTICO (v34) — SOLO comportamento.
 *  Nessun disegno nel framebuffer, nessun dump a schermo, nessun readback
 *  aggiuntivo. Una sola variabile cambia per build; il risultato si legge
 *  dalla geometria del pattern EBU esistente.
 *
 *    0 = baseline, identica a v33.
 *    1 = VCLK target dimezzato (~32.5 MHz): periodo ×2 → frequenza /2.
 *        Test dell'ipotesi "dot clock ~2× troppo alto" (compressione+sfarfallio).
 *    2 = HORZ_DIVBY2_EN forzato ATTIVO invece che pulito.
 *        Test del path "clock BIOS ~130 MHz + ÷2" (la repro PLL è un no-op?).
 * ========================================================================== */
#define MD_EXPERIMENT   1

/* ==========================================================================
 *  Identificazione PCI
 * ========================================================================== */

#define PCI_VENDOR_ATI                  0x1002u
#define PCI_DEV_X3100_LM               0x4C4Du
#define PCI_DEV_X3100_LR               0x4C52u

#define PCI_CFG_BAR0                    0x10
#define PCI_CFG_BAR1                    0x14

/* ==========================================================================
 *  Offset MMIO (Block 0, accesso via aperture-aliased a BAR0 + 0x7FFC00)
 *
 * Layout dell'aperture verificato su PRG Sez. 2.2.1 "Memory Map":
 *
 *     0x00000000 .. 0x007FF7FF   Frame Buffer (esteso fino qui)
 *     0x007FF800 .. 0x007FFBFF   MM Register Block 1 (overlay/multimedia)
 *     0x007FFC00 .. 0x007FFFFF   MM Register Block 0 (CRTC/DAC/draw eng)
 *     0x00800000 .. 0x00FFFFFF   Big-endian aperture
 *
 * Block 0 è SEMPRE accessibile via aperture-aliasing, indipendente da
 * BUS_EXT_REG_EN. Block 1 richiederebbe BUS_EXT_REG_EN; per ora non lo
 * tocchiamo (i registri LCD usati sotto sono in Block 0 via LCD_INDEX
 * indiretto). 
 *
 * Confermato dal report Everest del laptop di destinazione:
 *   BAR0 = 0x40000000 (16 MB), BAR3 = 0x41000000 (separato).
 */

/* LCD_GEN_CNTL bits — fonte: xorg-xf86-video-mach64/atiregs.h.
 *
 * Per cambiare risoluzione su un pannello LVDS bisogna manipolare
 * coerentemente questi bit, replicando esattamente cosa fa atyfb in
 * aty_var_to_crtc:
 *
 *   lcd_gen_cntl &= ~(HORZ_DIVBY2_EN | DIS_HOR_CRT_DIVBY2 | TVCLK_PM_EN
 *                   | USE_SHADOWED_ROWCUR | SHADOW_EN | SHADOW_RW_EN);
 *   lcd_gen_cntl |= DONT_SHADOW_VPAR | LOCK_8DOT;
 *
 * In particolare HORZ_DIVBY2_EN (bit 2) era il bug v32: il chip dimezza
 * il pixel clock orizzontale, comprimendo 1024 fb-px in 512 panel-px. */
#define M64_LCD_HORZ_DIVBY2_EN          (1u <<  2)
#define M64_LCD_LOCK_8DOT               (1u <<  4)
#define M64_LCD_DONT_SHADOW_VPAR        (1u <<  6)
#define M64_LCD_DIS_HOR_CRT_DIVBY2      (1u << 10)
#define M64_LCD_TVCLK_PM_EN             (1u << 16)
#define M64_LCD_USE_SHADOWED_ROWCUR     (1u << 29)
#define M64_LCD_CRTC_RW_SELECT          (1u << 27)
#define M64_LCD_SHADOW_EN               (1u << 30)
#define M64_LCD_SHADOW_RW_EN            (1u << 31)

/* PLL register indices (accesso via CLOCK_CNTL+1 = address, CLOCK_CNTL+2 = data).
 * Tutti i valori incrociati con x3100_ct.c di atyfb. */
#define M64_PLL_WR_EN_BIT               0x02
#define M64_MCLK_FB_DIV                 0x04
#define M64_VCLK_POST_DIV               0x06
#define M64_VCLK0_FB_DIV                0x07
#define M64_VCLK1_FB_DIV                0x08
#define M64_VCLK2_FB_DIV                0x09
#define M64_VCLK3_FB_DIV                0x0A
#define M64_PLL_EXT_CNTL                0x0E


/* CRTC_GEN_CNTL bit layout. */
#define M64_CRTC_PIX_BY_2_EN            0x00000020u
#define M64_CRTC_BLANK                  0x00000040u
#define M64_CRTC_PIX_WIDTH_32BPP        0x00000600u



#define M64_LCDIDX_LCD_GEN_CNTL         0x01
#define M64_LCDIDX_HORZ_STRETCHING      0x04
#define M64_LCDIDX_VERT_STRETCHING      0x05
#define M64_LCDIDX_EXT_VERT_STRETCH     0x06

/* ==========================================================================
 *  CRTC_OFF_PITCH packing
 *
 * Layout campo:
 *   bits 0..21:  display start offset in QWORD units (= bytes/8)
 *   bits 22..31: scanline pitch in PIXEL/8
 *
 * FONTE PRIMARIA ATI — RRG-S00700-05 pag 3-41 (CRTC_OFF_PITCH):
 *   "Display pitch in pixels-times-8" → campo pitch = pixel/8.
 *   "The pitch value must correspond exactly to the destination draw
 *    engine pitch", e DST_OFF_PITCH (PRG 215R3 pag 5-8) = (pitch px)/8.
 *
 * Quindi la pitch è in PIXEL/8, NON bytes/8: il chip ricava bytes/pixel
 * da PIX_WIDTH. Per 1024 px → 128, qualunque sia il bpp.
 *
 * (La macro M64_OFF_PITCH è definita in x3100_hw.h.) */

/* ==========================================================================
 *  Costanti chip-specifiche per Mach64 Mobility-P (LM) con SGRAM
 *
 * Lette dal log atyfb su HP Omnibook 4150B (chip+panel identico al Compaq
 * Armada E500 secondo il PRG e atyfb) e dalle tabelle in x3100_ct.c.
 * ========================================================================== */

/* Periodi in picosecondi (1e12 / freq_Hz, troncato a intero — esattamente
 * come fa atyfb in default_par->ref_clk_per). Uso periodi e non
 * frequenze perché la formula PLL atyfb è q = ref_per × ref_div × 4 /
 * target_per: tutti u32, niente bisogno di __udivdi3 (che il toolchain
 * freestanding i686-elf non linka). */
#define M64_XTAL_PERIOD_PS      33899u      /* 1e12 / 29498928  */
#define M64_VCLK_TARGET_PS      15384u      /* 1e12 / 65000000  */

/* Mobility-LM + SGRAM parameters (from atyfb aty_init_pll_ct + chip table). */
/* Mobility-LM SGRAM params — DEPRECATE: alimentavano la vecchia
 * aty_compute_dsp (formula non-canonica). La nuova versione (Appendix K)
 * usa M64_XCLK_MHZ e legge MEM_CNTL; queste non servono più. */
#define M64_MOB_FIFO_SIZE           24u
#define M64_MOB_XCLK_PAGE_FAULT     8u
#define M64_MOB_XCLK_MAX_RAS_DLY    6u
#define M64_MOB_DSP_LOOP_LATENCY    0u
#define M64_MOB_XCLK_REF_DIV        1u
#define M64_MOB_XCLK_POST_DIV       0u      /* postdividers[0] = 1 */

/* Tabella post divisori — atyfb x3100_ct.c::postdividers. */
static const uint8_t k_postdividers[5] = {1, 2, 4, 8, 3};

/* ==========================================================================
 *  Color constants (32 bpp BGRA little-endian: byte 0=B, 1=G, 2=R, 3=X).
 * ========================================================================== */

#define COL_BLACK    0x00000000u
#define COL_WHITE    0x00FFFFFFu
#define COL_RED      0x00FF0000u
#define COL_GREEN    0x0000FF00u
#define COL_BLUE     0x000000FFu
#define COL_YELLOW   0x00FFFF00u
#define COL_CYAN     0x0000FFFFu
#define COL_MAGENTA  0x00FF00FFu

/* ==========================================================================
 *  Driver state
 * ========================================================================== */

/* Stato migrato in g_x3100 (x3100_state.h).  Le ex-globali del
 * bring-up (g_x3100.mmio, g_fb, g_x3100.io_base, g_x3100.mode.width, g_x3100.mode.height, g_x3100.dev) sono ora
 * campi di g_x3100: .mmio, .vram, .io_base, .mode.width/height, .dev.
 * Il global g_x3100 è definito qui sotto. */
x3100_state_t  g_x3100;
pid_t           x3100_current_caller_pid;

/* ==========================================================================
 *  VRAM allocator — free-list with bidirectional coalescing.
 *
 *  Same model as the BGA driver (drivers/bga/main.c): block metadata
 *  lives in host RAM, never as in-band headers in the framebuffer.  The
 *  Mach64 contract (x3100_state.h) differs from BGA's in the interface:
 *    - vram_alloc(bytes, align) takes the alignment inline and returns
 *      the vram_block_t* (BGA returned a uint32 offset);
 *    - vram_free(vram_block_t*) frees by pointer (BGA freed by offset).
 *  surface_t stores only vram_offset, so vram_find(offset) recovers the
 *  block for the destroy path: alloc -> store b->offset; on destroy,
 *  vram_find(offset) then vram_free(blk).
 *
 *  align must be a power of two; values <= VRAM_ALIGN are rounded up to
 *  VRAM_ALIGN.  Aligned allocations matter for the scanout primary
 *  (CRTC offset is in qwords) and for engine source/dest pitch. */

#define VRAM_ALIGN              16u
#define VRAM_MIN_BLOCK          64u
#define VRAM_MAX_BLOCKS         512u

static vram_block_t  g_block_pool[VRAM_MAX_BLOCKS];
static vram_block_t *g_block_free;

static void block_pool_init(void)
{
    for (uint32_t i = 0; i < VRAM_MAX_BLOCKS - 1u; i++)
        g_block_pool[i].next = &g_block_pool[i + 1u];
    g_block_pool[VRAM_MAX_BLOCKS - 1u].next = NULL;
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

void vram_init_pool(uint32_t base, uint32_t size)
{
    block_pool_init();
    g_x3100.blocks_head = NULL;
    vram_block_t *b = block_alloc_meta();
    if (!b) return;                  /* pool can't be empty here, but be safe */
    b->offset = base;
    b->size   = size;
    b->used   = false;
    b->prev = b->next = NULL;
    g_x3100.blocks_head = b;
}

vram_block_t *vram_alloc(uint32_t bytes, uint32_t align)
{
    if (bytes < VRAM_MIN_BLOCK) bytes = VRAM_MIN_BLOCK;
    bytes = (bytes + VRAM_ALIGN - 1u) & ~(VRAM_ALIGN - 1u);

    /* Normalize alignment: at least VRAM_ALIGN, must be power of two. */
    if (align < VRAM_ALIGN) align = VRAM_ALIGN;
    if ((align & (align - 1u)) != 0u) align = VRAM_ALIGN;   /* not po2 -> default */

    for (vram_block_t *b = g_x3100.blocks_head; b; b = b->next)
    {
        if (b->used) continue;

        uint32_t a_off = (b->offset + align - 1u) & ~(align - 1u);
        if (a_off < b->offset) continue;                    /* overflow */
        uint32_t pad = a_off - b->offset;
        if (pad + bytes > b->size) continue;                /* doesn't fit */

        /* Split off a leading pad block so the used range starts aligned. */
        if (pad > 0u)
        {
            vram_block_t *post = block_alloc_meta();
            if (!post) continue;                            /* no meta -> try next */
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

        /* Split the tail if the remainder is worth keeping. */
        if (b->size > bytes + VRAM_MIN_BLOCK)
        {
            vram_block_t *rem = block_alloc_meta();
            if (rem)
            {
                rem->offset = b->offset + bytes;
                rem->size   = b->size - bytes;
                rem->used   = false;
                rem->prev   = b;
                rem->next   = b->next;
                if (rem->next) rem->next->prev = rem;
                b->next = rem;
                b->size = bytes;
            }
        }

        b->used = true;
        return b;
    }
    return NULL;                     /* out of VRAM */
}

void vram_free(vram_block_t *b)
{
    if (!b || !b->used) return;
    b->used = false;

    /* Coalesce forward. */
    if (b->next && !b->next->used)
    {
        vram_block_t *n = b->next;
        b->size += n->size;
        b->next  = n->next;
        if (b->next) b->next->prev = b;
        block_free_meta(n);
    }
    /* Coalesce backward. */
    if (b->prev && !b->prev->used)
    {
        vram_block_t *p = b->prev;
        p->size += b->size;
        p->next  = b->next;
        if (p->next) p->next->prev = p;
        block_free_meta(b);
    }
}

/* Recover a block by its byte offset — used by destroy paths that only
 * stored vram_offset (surfaces, buffers).  Returns NULL if no used block
 * starts exactly at `offset`. */
static vram_block_t *vram_find(uint32_t offset)
{
    for (vram_block_t *b = g_x3100.blocks_head; b; b = b->next)
        if (b->used && b->offset == offset) return b;
    return NULL;
}

static uint64_t vram_total_free(void)
{
    uint64_t f = 0;
    for (vram_block_t *b = g_x3100.blocks_head; b; b = b->next)
        if (!b->used) f += b->size;
    return f;
}

static uint64_t vram_largest_free(void)
{
    uint64_t l = 0;
    for (vram_block_t *b = g_x3100.blocks_head; b; b = b->next)
        if (!b->used && b->size > l) l = b->size;
    return l;
}

/* ==========================================================================
 *  Handle <-> slot mapping and per-table slot allocators.
 *  Handle 0 is reserved as DV_HANDLE_NONE; slot i maps to handle i+1.
 * ========================================================================== */

#define HANDLE_TO_SLOT(h)       ((uint32_t)(h) - 1u)
#define SLOT_TO_HANDLE(s)       ((uint32_t)(s) + 1u)

#define DEFINE_SLOT_ALLOC(NAME, FIELD, MAX) \
    static int alloc_##NAME##_slot(void) { \
        for (uint32_t i = 0; i < (MAX); i++) \
            if (!g_x3100.FIELD[i].used) { g_x3100.FIELD[i].used = true; return (int)i; } \
        return -1; \
    }

DEFINE_SLOT_ALLOC(vproc,    vprocs,    MAX_VPROCS)
DEFINE_SLOT_ALLOC(surface,  surfaces,  MAX_SURFACES)
DEFINE_SLOT_ALLOC(buffer,   buffers,   MAX_BUFFERS)
DEFINE_SLOT_ALLOC(fence,    fences,    MAX_FENCES)
DEFINE_SLOT_ALLOC(layer,    layers,    MAX_LAYERS)
DEFINE_SLOT_ALLOC(vthread,  vthreads,  MAX_VTHREADS)
DEFINE_SLOT_ALLOC(cmdlist,  cmdlists,  MAX_CMDLISTS)

/* ==========================================================================
 *  MMIO + I/O low-level
 * ========================================================================== */

/* MMIO helpers: ora x3100_mmio_r32/w32/r8/w8 (inline in x3100_state.h). */


/* ==========================================================================
 *  dv protocol — vproc / surface / texture / vram_info  (block #1)
 *
 *  These are the first calls dobinterface makes at startup: it attaches a
 *  vproc (its GPU context), queries VRAM, and creates its surfaces and
 *  glyph-atlas textures.  Semantics mirror the BGA reference; the Mach64
 *  differences are: the allocator returns a vram_block_t* (we store
 *  b->offset in the surface and recover it via vram_find on destroy), and
 *  surface clear / texture upload go through the 2D engine (blitter /
 *  HOST_DATA) instead of the CPU.
 *
 *  Pixel format: only 32bpp (BGRA/RGBA 8888) is accepted on this build.
 *  format_bpp() is the single place that maps a dv_format_t to bytes per
 *  pixel, so adding 8/16/24bpp later (the DobSettings colour-depth idea)
 *  is a localized change here plus the engine setup, not a rewrite. */

static uint32_t format_bpp(dv_format_t fmt)
{
    switch (fmt)
    {
        case DV_FMT_BGRA8888:
        case DV_FMT_RGBA8888: return 4u;
        default:              return 0u;   /* unsupported on this build */
    }
}

/* Defined with the glyph path (block #2b); used by destroy/update_region. */
static void atlas_mono_invalidate(uint32_t slot);

/* Defined with the dirty-rect path; used by dv_vproc_detach to return the
 * scratch buffer to the pool when the display source goes away. */
static void scratch_release(void);

static vproc_t *vproc_lookup(dv_vproc_t h)
{
    if (h == DV_HANDLE_NONE) return NULL;
    uint32_t s = HANDLE_TO_SLOT(h);
    if (s >= MAX_VPROCS || !g_x3100.vprocs[s].used) return NULL;
    return &g_x3100.vprocs[s];
}

static surface_t *surface_lookup(dv_surface_t h)
{
    if (h == DV_HANDLE_NONE) return NULL;
    uint32_t s = HANDLE_TO_SLOT(h);
    if (s >= MAX_SURFACES || !g_x3100.surfaces[s].used) return NULL;
    return &g_x3100.surfaces[s];
}

/* Byte offset of a surface's pixels in VRAM. */
static uint32_t surface_off(const surface_t *s) { return s->vram_offset; }

/* ==========================================================================
 *  Backing delle surface — dispatch VRAM / SYSRAM
 *
 *  Una surface con DV_SURF_FLAG_SYSRAM vive in RAM di sistema
 *  (s->sys_pixels): zero VRAM, zero quota. E' il backing del backbuf e
 *  dei corpi finestra della dobinterface 1.1: in VRAM (qui: stolen
 *  memory della GMA) resta solo lo scanout. Il motore 2D legge e
 *  scrive SOLO VRAM: ogni percorso che tocca una surface decide qui —
 *  engine se tutte le parti sono VRAM, CPU (blocco sw_* sotto) appena
 *  una e' SYSRAM. Speculare al driver mach64.
 * ========================================================================== */

static inline bool surface_is_sysram(const surface_t *s)
{
    return s->sys_pixels != NULL;
}

/* Puntatore CPU alla base pixel, qualunque sia il backing. La VRAM e'
 * mappata linearmente (g_x3100.vram), quindi lo stesso codice serve
 * entrambi i mondi; volatile per il lato aperture. */
static inline volatile uint32_t *surface_pixels(const surface_t *s)
{
    if (s->sys_pixels) return (volatile uint32_t *)s->sys_pixels;
    return (volatile uint32_t *)((volatile uint8_t *)g_x3100.vram
                                 + s->vram_offset);
}

/* Drena il motore prima che la CPU legga o scriva pixel che il motore
 * puo' avere in volo: serve solo per surface VRAM. Le SYSRAM sono
 * invisibili al motore: no-op. */
static inline void surface_cpu_sync(const surface_t *s)
{
    if (!surface_is_sysram(s) && g_x3100.engine_ok) x3100_wait_for_idle();
}

/* Commit dei write CPU (posted) verso VRAM prima che il motore LEGGA
 * quei pixel — stesso schema del drain in dv_texture_update_region:
 * una lettura dall'aperture spinge fuori il posted-write buffer.
 * SYSRAM: no-op. */
static inline void surface_flush_writes(const surface_t *s)
{
    if (surface_is_sysram(s)) return;
    volatile uint32_t *p = surface_pixels(s);
    volatile uint32_t sink = *p;
    (void)sink;
}

/* Primitive rep-string (identiche al BGA): il loop C per-pixel su
 * puntatori volatile non e' ottimizzabile dal compilatore e dimezza la
 * banda utile; rep stosl/movsl saturano cio' che bus e aperture
 * concedono. repe cmpsl e' il confronto per la presentazione a diff
 * (count deve essere > 0: a zero i flag restano indefiniti). */
static inline void fast_fill32(volatile uint32_t *dst, uint32_t color,
                               uint32_t count)
{
    __asm__ volatile (
        "rep stosl"
        : "+D"(dst), "+c"(count)
        : "a"(color)
        : "memory"
    );
}

static inline void fast_copy32(volatile uint32_t *dst,
                               const volatile uint32_t *src, uint32_t count)
{
    __asm__ volatile (
        "rep movsl"
        : "+D"(dst), "+S"(src), "+c"(count)
        :
        : "memory"
    );
}

/* ==========================================================================
 *  Damage tracking — chi scrive marca, la compose ricompone il minimo
 *
 *  Ogni op che muta i pixel di una surface passa da
 *  surface_mark_dirty: bump della gen + unione del rettangolo nel box
 *  sporco. La compose in modalita' shadow confronta i layer visibili
 *  con lo snapshot del frame precedente e ricompone solo il bbox di
 *  danno; dv_page_flip presenta solo quel bbox. Frame identico =>
 *  zero lavoro; un orologio che ticchetta costa la sua area, non 3 MB
 *  di ricompose piu' 3 MB di presentazione.
 * ========================================================================== */

/* Unione di [x0,x1)x[y0,y1) nel bbox b (vuoto se bx0>=bx1). */
static inline void dmg_add(int32_t *bx0, int32_t *by0,
                           int32_t *bx1, int32_t *by1,
                           int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (x0 >= x1 || y0 >= y1) return;
    if (*bx0 >= *bx1 || *by0 >= *by1)
    {
        *bx0 = x0; *by0 = y0; *bx1 = x1; *by1 = y1;
        return;
    }
    if (x0 < *bx0) *bx0 = x0;
    if (y0 < *by0) *by0 = y0;
    if (x1 > *bx1) *bx1 = x1;
    if (y1 > *by1) *by1 = y1;
}

/* Registra una mutazione dei pixel di s nel rettangolo dato (coordinate
 * della surface, clampate qui). Aritmetica a 64 bit: w/h arrivano dal
 * client e non devono poter mandare in overflow il clamp. */
static void surface_mark_dirty(surface_t *s, int32_t x, int32_t y,
                               uint32_t w, uint32_t h)
{
    int64_t x0 = x < 0 ? 0 : x;
    int64_t y0 = y < 0 ? 0 : y;
    int64_t x1 = (int64_t)x + (int64_t)w;
    int64_t y1 = (int64_t)y + (int64_t)h;
    if (x1 > (int64_t)s->width)  x1 = (int64_t)s->width;
    if (y1 > (int64_t)s->height) y1 = (int64_t)s->height;
    if (x0 >= x1 || y0 >= y1) return;   /* tutto fuori: pixel intatti */
    s->gen++;
    dmg_add(&s->dirty_x0, &s->dirty_y0, &s->dirty_x1, &s->dirty_y1,
            (int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1);
}

/* ==========================================================================
 *  Primitive software (blocco sw_*) — draw CPU su surface
 *
 *  Controparte CPU delle op del motore, per i casi che il motore non
 *  copre: destinazione o sorgente SYSRAM. Semantica identica al
 *  riferimento BGA, pixel BGRA8888. Chi chiama ha gia' fatto
 *  surface_cpu_sync sulle parti VRAM coinvolte e surface_flush_writes
 *  a valle se il motore rileggera' la destinazione.
 * ========================================================================== */

/* clip_rect e' definita nel blocco cmdlist piu' sotto; le sw_* la usano. */
static bool clip_rect(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h,
                      uint32_t bw, uint32_t bh);

static void sw_fill_rect(surface_t *dst, dv_rect_t r, uint32_t bgra)
{
    int32_t x = r.x, y = r.y; uint32_t w = r.w, h = r.h;
    if (!clip_rect(&x, &y, &w, &h, dst->width, dst->height)) return;
    volatile uint32_t *dbase = surface_pixels(dst);
    uint32_t dp = dst->pitch_words;
    for (uint32_t row = 0; row < h; row++)
        fast_fill32(dbase + (uint32_t)(y + (int32_t)row) * dp + (uint32_t)x,
                    bgra, w);
}

/* Copia opaca src->dst. La source rect viene prima serrata ai bordi
 * della sorgente (una resize puo' lasciare sr piu' grande della
 * surface), poi la destinazione viene clippata; il ritaglio sinistro/
 * alto trasla l'origine sorgente di pari passo. */
static void sw_blit(surface_t *dst, surface_t *src, dv_rect_t sr,
                    int32_t dx, int32_t dy)
{
    int32_t sx = sr.x, sy = sr.y; uint32_t w = sr.w, h = sr.h;
    if (!clip_rect(&sx, &sy, &w, &h, src->width, src->height)) return;
    dx += (sx - sr.x); dy += (sy - sr.y);

    int32_t x = dx, y = dy, x0 = x, y0 = y;
    if (!clip_rect(&x, &y, &w, &h, dst->width, dst->height)) return;
    sx += (x - x0); sy += (y - y0);

    volatile uint32_t *sbase = surface_pixels(src);
    volatile uint32_t *dbase = surface_pixels(dst);
    uint32_t sp = src->pitch_words, dp = dst->pitch_words;
    for (uint32_t row = 0; row < h; row++)
        fast_copy32(dbase + (uint32_t)(y  + (int32_t)row) * dp + (uint32_t)x,
                    sbase + (uint32_t)(sy + (int32_t)row) * sp + (uint32_t)sx,
                    w);
}

/* Scala nearest-neighbor sr -> dr (usata dalla dobinterface per
 * conservare il corpo finestra su resize). Campiona con prodotto a
 * 64 bit per evitare overflow su rettangoli grandi. */
static void sw_blit_stretched(surface_t *dst, surface_t *src,
                              dv_rect_t sr, dv_rect_t dr)
{
    if (sr.w == 0u || sr.h == 0u || dr.w == 0u || dr.h == 0u) return;

    int32_t x = dr.x, y = dr.y; uint32_t w = dr.w, h = dr.h;
    int32_t x0 = x, y0 = y;
    if (!clip_rect(&x, &y, &w, &h, dst->width, dst->height)) return;
    uint32_t skip_x = (uint32_t)(x - x0), skip_y = (uint32_t)(y - y0);

    volatile uint32_t *sbase = surface_pixels(src);
    volatile uint32_t *dbase = surface_pixels(dst);
    uint32_t sp = src->pitch_words, dp = dst->pitch_words;

    for (uint32_t row = 0; row < h; row++)
    {
        uint32_t sy = (uint32_t)(((uint64_t)(row + skip_y) * sr.h) / dr.h)
                    + (uint32_t)sr.y;
        if (sy >= src->height) sy = src->height - 1u;
        volatile uint32_t *srow = sbase + sy * sp;
        volatile uint32_t *drow = dbase + (uint32_t)(y + (int32_t)row) * dp
                                        + (uint32_t)x;
        for (uint32_t col = 0; col < w; col++)
        {
            uint32_t sx = (uint32_t)(((uint64_t)(col + skip_x) * sr.w) / dr.w)
                        + (uint32_t)sr.x;
            if (sx >= src->width) sx = src->width - 1u;
            drow[col] = srow[sx];
        }
    }
}

/* Linea 1px in software (Bresenham intero): serve al replay dei
 * cmdlist quando la destinazione e' SYSRAM — il caso diagonale che
 * non si riduce a un fill. Clip per-pixel: le linee UI sono corte. */
static void sw_draw_line_1px(surface_t *dst, int32_t ax, int32_t ay,
                             int32_t bx, int32_t by, uint32_t bgra)
{
    volatile uint32_t *dbase = surface_pixels(dst);
    uint32_t dp = dst->pitch_words;
    int32_t dx =  (bx > ax ? bx - ax : ax - bx);
    int32_t dy = -(by > ay ? by - ay : ay - by);
    int32_t sx = ax < bx ? 1 : -1;
    int32_t sy = ay < by ? 1 : -1;
    int32_t err = dx + dy;
    for (;;)
    {
        if (ax >= 0 && ay >= 0 &&
            ax < (int32_t)dst->width && ay < (int32_t)dst->height)
            dbase[(uint32_t)ay * dp + (uint32_t)ax] = bgra;
        if (ax == bx && ay == by) break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; ax += sx; }
        if (e2 <= dx) { err += dx; ay += sy; }
    }
}

/* Glifi in software: campiona direttamente il canale alpha dell'atlas
 * (layout del riferimento: strip verticale, glifo g alle righe
 * [g*gh, g*gh+gh), gw = larghezza atlas, gh = altezza/256) e scrive
 * fg sui pixel coperti — stessa soglia (alpha != 0) della maschera
 * mono del percorso hardware, cosi' i due percorsi rendono identico. */
static void sw_draw_glyphs(surface_t *dst, surface_t *atlas,
                           const dv_glyph_t *glyphs, uint32_t count,
                           uint32_t fg_bgra, int32_t ox, int32_t oy)
{
    if (atlas->height < 256u) return;
    uint32_t gw = atlas->width;
    uint32_t gh = atlas->height / 256u;
    if (gw == 0u || gh == 0u) return;

    volatile uint32_t *abase = surface_pixels(atlas);
    volatile uint32_t *dbase = surface_pixels(dst);
    uint32_t ap = atlas->pitch_words, dp = dst->pitch_words;

    for (uint32_t i = 0; i < count; i++)
    {
        const dv_glyph_t *g = &glyphs[i];
        if (g->glyph_index >= 256u) continue;

        int32_t gx = g->x + ox, gy = g->y + oy;
        uint32_t w = gw, h = gh;
        int32_t gx0 = gx, gy0 = gy;
        if (!clip_rect(&gx, &gy, &w, &h, dst->width, dst->height)) continue;
        uint32_t clip_dx = (uint32_t)(gx - gx0);
        uint32_t clip_dy = (uint32_t)(gy - gy0);

        for (uint32_t r = 0; r < h; r++)
        {
            volatile uint32_t *arow = abase
                + (g->glyph_index * gh + clip_dy + r) * ap + clip_dx;
            volatile uint32_t *drow = dbase
                + (uint32_t)(gy + (int32_t)r) * dp + (uint32_t)gx;
            for (uint32_t c = 0; c < w; c++)
                if ((arow[c] & 0xFF000000u) != 0u) drow[c] = fg_bgra;
        }
    }
}

/* ---------- vproc ---------- */

int32_t dv_vproc_attach(const dv_vproc_attach_desc_t *desc, dv_vproc_t *out)
{
    if (!out) return DV_ERR_INVAL;
    int slot = alloc_vproc_slot();
    if (slot < 0) return DV_ERR_NOMEM;
    vproc_t *p = &g_x3100.vprocs[slot];
    p->owner_pid        = x3100_current_caller_pid;
    p->vram_quota_bytes = g_x3100.vram_bytes / 4u;   /* default: a quarter */
    p->vram_used_bytes  = 0;
    p->vthreads_active  = 0;
    p->fences_in_flight = 0;
    if (desc && desc->vram_quota_bytes > 0 &&
        desc->vram_quota_bytes <= g_x3100.vram_bytes)
        p->vram_quota_bytes = desc->vram_quota_bytes;
    *out = (dv_vproc_t)SLOT_TO_HANDLE(slot);
    return DV_OK;
}

int32_t dv_vproc_detach(dv_vproc_t v)
{
    vproc_t *p = vproc_lookup(v);
    if (!p) return DV_ERR_HANDLE;

    for (uint32_t i = 0; i < MAX_SURFACES; i++)
        if (g_x3100.surfaces[i].used && g_x3100.surfaces[i].owner == v)
        {
            surface_t *ss = &g_x3100.surfaces[i];
            /* Free any glyph mono-mask shadow for this surface FIRST.  The
             * single-surface path (dv_surface_destroy) does this; detach used
             * to skip it — leaking the mask VRAM AND leaving a stale "valid"
             * atlas entry that a later surface reusing this slot would wrongly
             * reuse (garbled glyphs). */
            atlas_mono_invalidate(i);
            /* SYSRAM PRIMA di tutto: ha block==NULL e vram_offset==0,
             * e il fallback vram_find(0) sotto TROVEREBBE il blocco del
             * primary (lo scanout sta a offset 0) e lo libererebbe —
             * schermo morto al primo detach di un client SYSRAM. */
            if (ss->sys_pixels)
            {
                free(ss->sys_pixels);
                ss->sys_pixels = NULL;
                ss->used = false;
                continue;
            }
            /* Free by stored pointer (see dv_surface_destroy) so a missed
             * offset lookup can't silently orphan the block on teardown. */
            if (ss->block) { vram_free(ss->block); ss->block = NULL; }
            else { vram_block_t *b = vram_find(ss->vram_offset); if (b) vram_free(b); }
            ss->used = false;
        }
    for (uint32_t i = 0; i < MAX_BUFFERS; i++)
        if (g_x3100.buffers[i].used && g_x3100.buffers[i].owner == v)
        {
            buffer_t *bb = &g_x3100.buffers[i];
            /* Free by stored pointer (same robustness as surfaces); fall back
             * to the offset lookup only if absent. */
            if (bb->block) { vram_free(bb->block); bb->block = NULL; }
            else { vram_block_t *b = vram_find(bb->vram_offset); if (b) vram_free(b); }
            bb->used = false;
        }
    for (uint32_t i = 0; i < MAX_FENCES; i++)
        if (g_x3100.fences[i].used && g_x3100.fences[i].owner == v)
            g_x3100.fences[i].used = false;
    for (uint32_t i = 0; i < MAX_LAYERS; i++)
        if (g_x3100.layers[i].used && g_x3100.layers[i].owner == v)
            g_x3100.layers[i].used = false;
    for (uint32_t i = 0; i < MAX_VTHREADS; i++)
        if (g_x3100.vthreads[i].used && g_x3100.vthreads[i].owner == v)
            g_x3100.vthreads[i].used = false;

    /* Reclaim cmdlists owned by this vproc.  This was MISSING and is a real
     * leak: every window's retained cmdlist holds both a slot and a reserved
     * span of the shared cmdlist_storage bump pool.  On client exit (vproc
     * detach / death) neither was reclaimed, so after enough app launches the
     * pool filled and dv_cmdlist_create returned DV_ERR_NOMEM — new windows
     * could no longer render.  dv_cmdlist_destroy frees the slot AND compacts
     * the bump pool (it only shuffles storage offsets, never slot indices, so
     * iterating by slot here is safe). */
    for (uint32_t i = 0; i < MAX_CMDLISTS; i++)
        if (g_x3100.cmdlists[i].used && g_x3100.cmdlists[i].owner == v)
            dv_cmdlist_destroy((dv_cmdlist_t)SLOT_TO_HANDLE(i));

    if (g_x3100.scanout_source != DV_HANDLE_NONE)
    {
        surface_t *s = surface_lookup(g_x3100.scanout_source);
        if (!s || s->owner == v)
        {
            g_x3100.scanout_source = DV_HANDLE_NONE;
            /* The display source for this driver is gone; the dirty-rect
             * scratch only served its composes, so return it to the pool
             * (it re-grows on demand if a new compositor attaches). */
            scratch_release();
        }
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
    if (new_quota_bytes > g_x3100.vram_bytes)  return DV_ERR_INVAL;
    if (new_quota_bytes < p->vram_used_bytes)   return DV_ERR_QUOTA;
    p->vram_quota_bytes = new_quota_bytes;
    return DV_OK;
}

/* ---------- vram info ---------- */

int32_t dv_vram_info(dv_vproc_t v, dv_vram_info_t *out)
{
    (void)v;
    if (!out) return DV_ERR_INVAL;
    out->total_bytes          = g_x3100.vram_bytes;
    out->free_bytes           = vram_total_free();
    out->largest_contig_bytes = vram_largest_free();
    /* 32-bit math: VRAM <= 8 MB, and MainDOB doesn't link __udivdi3. */
    uint32_t free32    = (uint32_t)out->free_bytes;
    uint32_t largest32 = (uint32_t)out->largest_contig_bytes;
    out->fragmentation_pct = free32 ? (100u - (100u * largest32 / free32)) : 0u;

    out->cmdlist_pool_total_bytes = (uint32_t)CMDLIST_STORAGE_BYTES;
    out->cmdlist_pool_used_bytes  = g_x3100.cmdlist_storage_used;
    uint32_t cl = 0;
    for (uint32_t i = 0; i < MAX_CMDLISTS; i++)
        if (g_x3100.cmdlists[i].used) cl++;
    out->cmdlist_count = cl;
    return DV_OK;
}

/* ---------- surface ---------- */

int32_t dv_surface_create(dv_vproc_t v, const dv_surface_desc_t *d, dv_surface_t *out)
{
    if (!d || !out) return DV_ERR_INVAL;
    vproc_t *p = vproc_lookup(v);
    if (!p) return DV_ERR_HANDLE;

    uint32_t bpp = format_bpp(d->format);
    if (bpp == 0u) return DV_ERR_FORMAT;
    if (d->width == 0u || d->height == 0u ||
        d->width > 8192u || d->height > 8192u) return DV_ERR_INVAL;

    /* Mach64 pitch MUST be a multiple of 8 pixels (PRG 3593: OFF_PITCH stores
     * pitch/8 in bits 31:22, so any non-multiple-of-8 width truncates and the
     * blitter steps rows with the WRONG stride → progressive diagonal skew,
     * seen on thumbnails/widgets of arbitrary width).  Round the row stride
     * (pitch) up to 8 px; the logical width stays d->width.  VRAM is sized for
     * the padded pitch so the blitter never reads/writes out of bounds. */
    uint32_t pitch_px = (d->width + 7u) & ~7u;
    uint32_t bytes = pitch_px * d->height * bpp;

    /* SYSRAM (il cuore del refactoring 1.1): pixel in RAM di sistema,
     * zero VRAM e zero quota — la dobinterface ci mette backbuf e corpi
     * finestra, e in VRAM resta solo lo scanout. Il pitch resta
     * 8-allineato per uniformita' (alla CPU non serve, ma cosi'
     * surface_info e i percorsi condivisi non hanno casi speciali).
     * Clear a zero come il ramo VRAM; niente motore, niente drain. */
    if (d->flags & DV_SURF_FLAG_SYSRAM)
    {
        uint8_t *pix = (uint8_t *)malloc(bytes);
        if (!pix)
        {
            debug_print("[x3100] surface_create SYSRAM: malloc fallita\n");
            return DV_ERR_NOMEM;
        }
        int sslot = alloc_surface_slot();
        if (sslot < 0)
        {
            free(pix);
            return DV_ERR_NOMEM;
        }
        memset(pix, 0, bytes);
        surface_t *s = &g_x3100.surfaces[sslot];
        s->owner       = v;
        s->width       = d->width;
        s->height      = d->height;
        s->pitch_words = pitch_px;
        s->format      = d->format;
        s->flags       = d->flags;
        s->vram_offset = 0u;
        s->vram_bytes  = 0u;            /* mai contata sulla quota VRAM */
        s->block       = NULL;
        s->sys_pixels  = pix;
        s->gen         = ++g_x3100.surface_gen_seed;   /* epoca univoca */
        s->dirty_x0 = s->dirty_y0 = s->dirty_x1 = s->dirty_y1 = 0;
        *out = (dv_surface_t)SLOT_TO_HANDLE(sslot);
        return DV_OK;
    }

    if (p->vram_used_bytes + bytes > p->vram_quota_bytes) return DV_ERR_QUOTA;

    vram_block_t *blk = vram_alloc(bytes, VRAM_ALIGN);
    if (!blk) return DV_ERR_OOM_VRAM;

    int slot = alloc_surface_slot();
    if (slot < 0)
    {
        vram_free(blk);
        return DV_ERR_NOMEM;
    }
    surface_t *s = &g_x3100.surfaces[slot];
    s->owner       = v;
    s->width       = d->width;
    s->height      = d->height;
    s->pitch_words = pitch_px;          /* 8-px-aligned stride (PRG 3593) */
    s->format      = d->format;
    s->flags       = d->flags;
    s->vram_offset = blk->offset;
    s->vram_bytes  = bytes;
    s->block       = blk;        /* free by pointer on destroy, not vram_find */
    s->sys_pixels  = NULL;       /* l'allocatore di slot NON azzera il record */
    s->gen         = ++g_x3100.surface_gen_seed;       /* epoca univoca */
    s->dirty_x0 = s->dirty_y0 = s->dirty_x1 = s->dirty_y1 = 0;
    p->vram_used_bytes += bytes;

    /* Clear to opaque black using the blitter (engine must be ready). */
    if (g_x3100.engine_ready)
    {
        x3100_hw_solid_fill(s->vram_offset, s->pitch_words,
                             0, 0, d->width, d->height, 0x00000000u);
        /* Drain before returning. The engine clear is ASYNC; callers
         * routinely follow surface_create with a CPU upload
         * (dv_texture_update_region writes pixels straight into this VRAM).
         * Without the drain the black fill can land AFTER the CPU upload
         * and wipe it — which on the real Rage Mobility produced black
         * Mission Control thumbnails and blank icons on first paint, while
         * QEMU's instant fill hid the race. */
        if (g_x3100.engine_ok) x3100_wait_for_idle();
    }

    *out = (dv_surface_t)SLOT_TO_HANDLE(slot);
    return DV_OK;
}

int32_t dv_surface_destroy(dv_surface_t s)
{
    surface_t *ss = surface_lookup(s);
    if (!ss) return DV_ERR_HANDLE;

    /* SYSRAM: solo il free del malloc — mai passata dalla quota VRAM
     * ne' dall'allocatore VRAM, quindi niente da restituire li'. */
    if (ss->sys_pixels)
    {
        atlas_mono_invalidate(HANDLE_TO_SLOT(s));
        free(ss->sys_pixels);
        ss->sys_pixels = NULL;
        ss->used = false;
        if (g_x3100.scanout_source == s) g_x3100.scanout_source = DV_HANDLE_NONE;
        return DV_OK;
    }

    vproc_t *p = vproc_lookup(ss->owner);
    if (p && p->vram_used_bytes >= ss->vram_bytes)
        p->vram_used_bytes -= ss->vram_bytes;
    atlas_mono_invalidate(HANDLE_TO_SLOT(s));   /* free any glyph mono shadow */
    /* Free by the block pointer stored at creation. The old path —
     * vram_find(ss->vram_offset) then vram_free — silently skipped the free
     * when the lookup missed, orphaning the block permanently. Freeing the
     * stored pointer cannot miss. Fall back to the offset lookup only if the
     * pointer is somehow absent (legacy/zero-init safety). */
    if (ss->block)
    {
        vram_free(ss->block);
        ss->block = NULL;
    }
    else
    {
        vram_block_t *b = vram_find(ss->vram_offset);
        if (b) vram_free(b);
    }
    ss->used = false;
    if (g_x3100.scanout_source == s) g_x3100.scanout_source = DV_HANDLE_NONE;
    return DV_OK;
}

int32_t dv_surface_info(dv_surface_t s, dv_surface_info_t *out)
{
    surface_t *ss = surface_lookup(s);
    if (!ss || !out) return DV_ERR_HANDLE;
    out->width       = ss->width;
    out->height      = ss->height;
    out->format      = ss->format;
    out->pitch_bytes = ss->pitch_words * format_bpp(ss->format);
    out->flags       = ss->flags;
    return DV_OK;
}

int32_t dv_surface_clear(dv_surface_t s, dv_color_t c)
{
    surface_t *ss = surface_lookup(s);
    if (!ss) return DV_ERR_HANDLE;
    uint32_t pix = (((uint32_t)c.a & 0xFFu) << 24) | (((uint32_t)c.r & 0xFFu) << 16)
                 | (((uint32_t)c.g & 0xFFu) << 8)  |  ((uint32_t)c.b & 0xFFu);
    surface_mark_dirty(ss, 0, 0, ss->width, ss->height);
    if (surface_is_sysram(ss))
    {
        dv_rect_t full = { 0, 0, ss->width, ss->height };
        sw_fill_rect(ss, full, pix);
        return DV_OK;
    }
    x3100_hw_solid_fill(ss->vram_offset, ss->pitch_words,
                         0, 0, ss->width, ss->height, pix);
    return DV_OK;
}

/* ---------- texture (a sampled surface; bind/mips are no-ops here) ---------- */

int32_t dv_texture_create(dv_vproc_t v, const dv_texture_desc_t *d, dv_texture_t *out)
{
    dv_surface_desc_t sd = {
        .width  = d ? d->width  : 0u,
        .height = d ? d->height : 0u,
        .format = d ? d->format : DV_FMT_BGRA8888,
        .flags  = d ? d->flags  : 0u,
    };
    return dv_surface_create(v, &sd, (dv_surface_t *)out);
}

int32_t dv_texture_destroy(dv_texture_t t)
{
    return dv_surface_destroy((dv_surface_t)t);
}

/* Upload a sub-rectangle of CPU pixels into the texture's VRAM via the
 * engine's HOST_DATA path (no CPU framebuffer writes).  src is tightly
 * packed at src_pitch bytes per row; we feed it row-aligned. */
int32_t dv_texture_update_region(dv_texture_t t, dv_rect_t r,
                                 const void *src, uint32_t src_pitch)
{
    surface_t *ss = surface_lookup((dv_surface_t)t);
    if (!ss || !src) return DV_ERR_HANDLE;
    if (r.x < 0 || r.y < 0 ||
        r.x + (int32_t)r.w > (int32_t)ss->width ||
        r.y + (int32_t)r.h > (int32_t)ss->height) return DV_ERR_RANGE;
    if (r.w == 0u || r.h == 0u) return DV_OK;

    uint32_t bpp = format_bpp(ss->format);
    if (bpp != 4u) return DV_ERR_FORMAT;

    /* Atlas content changed -> any cached 1bpp glyph mask is stale. */
    atlas_mono_invalidate(HANDLE_TO_SLOT((dv_surface_t)t));

    /* Write the texture with the CPU, NOT the engine's host_blit.  Per PRG
     * 5.2.1.3 a region the engine just wrote may read back garbage through the
     * aperture; the glyph path then reads this surface back in
     * atlas_mono_build to build the 1bpp mask, so an engine write here yields
     * a corrupted mask (noise text).  A CPU write is reliably read-back-able. */
    surface_mark_dirty(ss, r.x, r.y, r.w, r.h);
    const uint8_t *sp = (const uint8_t *)src;
    volatile uint8_t *dbase = (volatile uint8_t *)surface_pixels(ss);
    uint32_t dst_pitch_bytes = ss->pitch_words * bpp;
    for (uint32_t y = 0; y < r.h; y++)
    {
        volatile uint8_t *drow = dbase
            + (size_t)((uint32_t)r.y + y) * dst_pitch_bytes
            + (size_t)(uint32_t)r.x * bpp;
        const uint8_t *srow = sp + (size_t)y * src_pitch;
        for (uint32_t b = 0; b < r.w * bpp; b++) drow[b] = srow[b];
    }

    /* Commit the CPU writes so a later HARDWARE blit that READS this texture
     * sees the new pixels, not stale VRAM. The Mach64 takes CPU aperture
     * writes through a posted path; the 2D engine reads VRAM directly, and
     * the two are not implicitly ordered in the CPU-write -> engine-read
     * direction. Without forcing the writes to land, the hw_blit that draws
     * a freshly-uploaded texture (e.g. Mission Control thumbnails) read the
     * surface's previous contents — the black clear from surface_create —
     * so every thumbnail came out black. (This is the mirror of the PRG
     * 5.2.1.3 engine-write -> CPU-read hazard handled elsewhere; BGA never
     * hits it because it composes in software and CPU-reads what it
     * CPU-wrote.) A read-back of the last written word drains the posted
     * write buffer through the aperture; `volatile` keeps it from being
     * elided or reordered. This cost falls only on texture UPLOADS — the
     * on-demand paths (thumbnail build, tray cache) — never on the drag /
     * scroll hot path, which uploads nothing. */
    if (r.h > 0u && r.w > 0u && !surface_is_sysram(ss))
    {
        volatile uint8_t *last = dbase
            + (size_t)((uint32_t)r.y + r.h - 1u) * dst_pitch_bytes
            + (size_t)((uint32_t)r.x + r.w - 1u) * bpp;
        volatile uint32_t sink = *(volatile uint32_t *)last;
        (void)sink;
    }
    return DV_OK;
}

/* ==========================================================================
 *  dv protocol — command lists  (block #2a)
 *
 *  dobinterface draws by building command lists (background rects, glyph
 *  runs, blits) attached to layers, then composing.  A cmdlist is a packed
 *  byte stream of records in a BSS storage pool (NOT VRAM); cmdlist_replay
 *  walks it and turns each record into a hardware operation on a target
 *  surface.  Record/wire format is identical to the BGA reference so the
 *  two drivers stay interchangeable at the protocol level.
 *
 *  Replay mapping (this block):
 *    FILL_RECT  -> x3100_hw_solid_fill        (blitter)
 *    BLIT       -> x3100_hw_blit              (blitter)
 *    DRAW_LINE  -> x3100_hw_line / solid_fill (blitter; H/V as 1px fill)
 *  Staged for block #2b (hardware, needs mono-expansion / 3D-blend setup):
 *    DRAW_GLYPHS  -> 2D engine monochrome expansion (binary coverage,
 *                    matches the reference; dodges the LM texture-alpha
 *                    hardware defect entirely)
 *    BLIT_ALPHA   -> opaque (alpha==255) already maps to x3100_hw_blit;
 *                    constant/pixel alpha -> software over (rare; window
 *                    level), or 3D source-dest blend where it holds.
 * ========================================================================== */

#define CMDLIST_REC_FILL_RECT     1
#define CMDLIST_REC_BLIT          2
#define CMDLIST_REC_BLIT_ALPHA    3
#define CMDLIST_REC_DRAW_GLYPHS   4
#define CMDLIST_REC_DRAW_LINE     5

static cmdlist_t *cmdlist_lookup(dv_cmdlist_t h)
{
    if (h == DV_HANDLE_NONE) return NULL;
    uint32_t s = HANDLE_TO_SLOT(h);
    if (s >= MAX_CMDLISTS || !g_x3100.cmdlists[s].used) return NULL;
    return &g_x3100.cmdlists[s];
}

static uint32_t dv_color_to_bgra(dv_color_t c)
{
    return (((uint32_t)c.a & 0xFFu) << 24) | (((uint32_t)c.r & 0xFFu) << 16)
         | (((uint32_t)c.g & 0xFFu) << 8)  |  ((uint32_t)c.b & 0xFFu);
}

/* Append `bytes` from `src` at the cmdlist write head.  DV_ERR_QUOTA on
 * overflow.  command_count bumped only when count_as_record is true (the
 * last append of a record). Byte copy avoids importing memcpy; record
 * sizes are tiny. */
static int32_t cmdlist_append(cmdlist_t *cl, const void *src, uint32_t bytes,
                              bool count_as_record)
{
    if (cl->bytes_used + bytes > cl->capacity) return DV_ERR_QUOTA;
    uint8_t *base = &g_x3100.cmdlist_storage[cl->storage_off + cl->bytes_used];
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < bytes; i++) base[i] = s[i];
    cl->bytes_used += bytes;
    if (count_as_record) cl->command_count++;
    return DV_OK;
}

int32_t dv_cmdlist_create(dv_vproc_t v, uint32_t capacity_bytes, dv_cmdlist_t *out)
{
    if (!out) return DV_ERR_INVAL;
    if (!vproc_lookup(v)) return DV_ERR_HANDLE;
    if (capacity_bytes == 0u) return DV_ERR_INVAL;

    uint32_t cap = (capacity_bytes + 15u) & ~15u;
    if (g_x3100.cmdlist_storage_used + cap > (uint32_t)CMDLIST_STORAGE_BYTES)
        return DV_ERR_NOMEM;

    int slot = alloc_cmdlist_slot();
    if (slot < 0) return DV_ERR_NOMEM;

    cmdlist_t *cl = &g_x3100.cmdlists[slot];
    cl->owner         = v;
    cl->storage_off   = g_x3100.cmdlist_storage_used;
    cl->capacity      = cap;
    cl->bytes_used    = 0;
    cl->command_count = 0;
    cl->covers_fullscreen = false;
    g_x3100.cmdlist_storage_used += cap;

    *out = (dv_cmdlist_t)SLOT_TO_HANDLE(slot);
    return DV_OK;
}

int32_t dv_cmdlist_destroy(dv_cmdlist_t cl_h)
{
    cmdlist_t *cl = cmdlist_lookup(cl_h);
    if (!cl) return DV_ERR_HANDLE;

    /* Bump-pool compaction (same scheme as BGA): if at the top, retract
     * in O(1); else slide everything above down by `cap` and fix offsets. */
    uint32_t off = cl->storage_off;
    uint32_t cap = cl->capacity;
    cl->used = false;

    if (off + cap == g_x3100.cmdlist_storage_used)
    {
        g_x3100.cmdlist_storage_used -= cap;
    }
    else if (cap > 0u)
    {
        uint32_t move_src   = off + cap;
        uint32_t move_bytes = g_x3100.cmdlist_storage_used - move_src;
        if (move_bytes > 0u)
            memmove(&g_x3100.cmdlist_storage[off],
                    &g_x3100.cmdlist_storage[move_src], move_bytes);
        g_x3100.cmdlist_storage_used -= cap;
        for (int i = 0; i < MAX_CMDLISTS; i++)
        {
            cmdlist_t *o = &g_x3100.cmdlists[i];
            if (o == cl || !o->used) continue;
            if (o->storage_off > off) o->storage_off -= cap;
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
    cl->covers_fullscreen = false;
    return DV_OK;
}

int32_t dv_cmdlist_fill_rect(dv_cmdlist_t cl_h, dv_rect_t r, dv_color_t c)
{
    cmdlist_t *cl = cmdlist_lookup(cl_h);
    if (!cl) return DV_ERR_HANDLE;
    uint8_t tag = CMDLIST_REC_FILL_RECT;
    uint32_t bgra = dv_color_to_bgra(c);
    int32_t rc;
    if ((rc = cmdlist_append(cl, &tag,  1u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &r,    16u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &bgra, 4u, true )) != DV_OK) return rc;

    /* A FILL_RECT is opaque.  If it starts at the origin and spans the whole
     * screen, replaying this list overwrites every visible pixel, so compose
     * can skip the full-screen clear (kills the per-frame black flash).
     * Recompute as a property of the whole list: once a fullscreen fill is
     * present, the list covers the screen. */
    if (r.x <= 0 && r.y <= 0 &&
        (uint32_t)(r.x + (int32_t)r.w) >= g_x3100.mode.width &&
        (uint32_t)(r.y + (int32_t)r.h) >= g_x3100.mode.height)
        cl->covers_fullscreen = true;
    return DV_OK;
}

int32_t dv_cmdlist_blit(dv_cmdlist_t cl_h, dv_surface_t src, dv_rect_t sr, dv_point_t dp)
{
    cmdlist_t *cl = cmdlist_lookup(cl_h);
    if (!cl) return DV_ERR_HANDLE;
    uint8_t tag = CMDLIST_REC_BLIT;
    int32_t rc;
    if ((rc = cmdlist_append(cl, &tag,  1u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &src,  4u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &sr,   16u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &dp.x, 4u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &dp.y, 4u, true )) != DV_OK) return rc;

    /* An opaque BLIT that lands at the origin and spans the screen (e.g. a
     * wallpaper) also fully covers the frame -> compose may skip the clear. */
    if (dp.x <= 0 && dp.y <= 0 &&
        (uint32_t)(dp.x + (int32_t)sr.w) >= g_x3100.mode.width &&
        (uint32_t)(dp.y + (int32_t)sr.h) >= g_x3100.mode.height)
        cl->covers_fullscreen = true;
    return DV_OK;
}

int32_t dv_cmdlist_blit_alpha(dv_cmdlist_t cl_h, dv_surface_t src, dv_rect_t sr,
                              dv_point_t dp, uint8_t alpha, bool use_pixel_alpha)
{
    cmdlist_t *cl = cmdlist_lookup(cl_h);
    if (!cl) return DV_ERR_HANDLE;
    uint8_t tag = CMDLIST_REC_BLIT_ALPHA;
    uint8_t upa = use_pixel_alpha ? 1u : 0u;
    int32_t rc;
    if ((rc = cmdlist_append(cl, &tag,   1u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &src,   4u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &sr,    16u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &dp.x,  4u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &dp.y,  4u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &alpha, 1u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &upa,   1u, true )) != DV_OK) return rc;
    return DV_OK;
}

int32_t dv_cmdlist_draw_glyphs(dv_cmdlist_t cl_h, dv_texture_t atlas,
                               const dv_glyph_t *glyphs, uint32_t count,
                               dv_color_t color)
{
    cmdlist_t *cl = cmdlist_lookup(cl_h);
    if (!cl) return DV_ERR_HANDLE;
    if (count > 0u && !glyphs) return DV_ERR_INVAL;
    uint8_t tag = CMDLIST_REC_DRAW_GLYPHS;
    uint32_t bgra = dv_color_to_bgra(color);
    int32_t rc;
    if ((rc = cmdlist_append(cl, &tag,   1u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &atlas, 4u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &bgra,  4u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &count, 4u, false)) != DV_OK) return rc;
    if (count > 0u)
        if ((rc = cmdlist_append(cl, glyphs,
                                 count * (uint32_t)sizeof(dv_glyph_t),
                                 false)) != DV_OK) return rc;
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
    if ((rc = cmdlist_append(cl, &tag,       1u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &a.x,       4u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &a.y,       4u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &b.x,       4u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &b.y,       4u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &thickness, 4u, false)) != DV_OK) return rc;
    if ((rc = cmdlist_append(cl, &bgra,      4u, true )) != DV_OK) return rc;
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

/* Clip a rect to [0,w)x[0,h); returns false if fully clipped. */
static bool clip_rect(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h,
                      uint32_t bw, uint32_t bh)
{
    int32_t x0 = *x, y0 = *y;
    int32_t x1 = x0 + (int32_t)*w, y1 = y0 + (int32_t)*h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)bw) x1 = (int32_t)bw;
    if (y1 > (int32_t)bh) y1 = (int32_t)bh;
    if (x0 >= x1 || y0 >= y1) return false;
    *x = x0; *y = y0; *w = (uint32_t)(x1 - x0); *h = (uint32_t)(y1 - y0);
    return true;
}

/* Forward decls for the #2b hardware handlers (glyph/alpha). */
static void replay_draw_glyphs(surface_t *dst, surface_t *atlas,
                               const dv_glyph_t *glyphs, uint32_t count,
                               uint32_t fg_bgra, int32_t ox, int32_t oy);
static void replay_blit_alpha(surface_t *dst, surface_t *src, dv_rect_t sr,
                              int32_t dx, int32_t dy, uint8_t alpha,
                              bool use_pixel_alpha);

/* Execute every record in `cl` onto `dst`, translating coordinates by
 * (ox,oy) — used by dv_compose to place a layer at its dst_rect.  Records
 * referencing a destroyed surface are skipped so compose stays robust. */
static void cmdlist_replay(cmdlist_t *cl, surface_t *dst, int32_t ox, int32_t oy)
{
    const uint8_t *p   = &g_x3100.cmdlist_storage[cl->storage_off];
    const uint8_t *end = p + cl->bytes_used;
    uint32_t dpitch = dst->pitch_words;
    uint32_t doff   = dst->vram_offset;

    while (p < end)
    {
        uint8_t tag = *p++;
        switch (tag)
        {
            case CMDLIST_REC_FILL_RECT: {
                dv_rect_t r; uint32_t bgra;
                __builtin_memcpy(&r,    p, 16); p += 16;
                __builtin_memcpy(&bgra, p,  4); p +=  4;
                if (surface_is_sysram(dst))
                {
                    dv_rect_t tr = { r.x + ox, r.y + oy, r.w, r.h };
                    sw_fill_rect(dst, tr, bgra);
                    break;
                }
                int32_t x = r.x + ox, y = r.y + oy; uint32_t w = r.w, h = r.h;
                if (clip_rect(&x, &y, &w, &h, dst->width, dst->height))
                    x3100_hw_solid_fill(doff, dpitch,
                                         (uint32_t)x, (uint32_t)y, w, h, bgra);
                break;
            }
            case CMDLIST_REC_BLIT: {
                dv_surface_t sh; dv_rect_t sr; int32_t dx, dy;
                __builtin_memcpy(&sh, p, 4);  p += 4;
                __builtin_memcpy(&sr, p, 16); p += 16;
                __builtin_memcpy(&dx, p, 4);  p += 4;
                __builtin_memcpy(&dy, p, 4);  p += 4;
                surface_t *src = surface_lookup(sh);
                if (!src) break;
                /* Backing SYSRAM da una delle due parti: il blitter
                 * non legge ne' scrive la RAM di sistema — copia CPU
                 * dei pixel finali (sync mirati sulle parti VRAM,
                 * flush a valle: il record successivo puo' rileggere
                 * la destinazione col motore). */
                if (surface_is_sysram(src) || surface_is_sysram(dst))
                {
                    surface_cpu_sync(src);
                    surface_cpu_sync(dst);
                    sw_blit(dst, src, sr, dx + ox, dy + oy);
                    surface_flush_writes(dst);
                    break;
                }
                /* clip destination rect; source starts at sr.x/sr.y */
                int32_t x = dx + ox, y = dy + oy; uint32_t w = sr.w, h = sr.h;
                int32_t sx = sr.x, sy = sr.y;
                /* adjust source origin for left/top clip */
                int32_t x0 = x, y0 = y;
                if (clip_rect(&x, &y, &w, &h, dst->width, dst->height))
                {
                    sx += (x - x0); sy += (y - y0);
                    /* Same VRAM: overlap-aware path (see dv_blit). */
                    if (src->vram_offset == doff &&
                        src->pitch_words == dpitch)
                        x3100_hw_blit_overlap(doff, dpitch,
                                              (uint32_t)sx, (uint32_t)sy,
                                              (uint32_t)x, (uint32_t)y, w, h);
                    else
                        x3100_hw_blit(src->vram_offset, src->pitch_words,
                                       (uint32_t)sx, (uint32_t)sy,
                                       doff, dpitch, (uint32_t)x, (uint32_t)y, w, h);
                }
                break;
            }
            case CMDLIST_REC_BLIT_ALPHA: {
                dv_surface_t sh; dv_rect_t sr; int32_t dx, dy;
                uint8_t alpha, upa;
                __builtin_memcpy(&sh, p, 4);  p += 4;
                __builtin_memcpy(&sr, p, 16); p += 16;
                __builtin_memcpy(&dx, p, 4);  p += 4;
                __builtin_memcpy(&dy, p, 4);  p += 4;
                alpha = *p++; upa = *p++;
                surface_t *src = surface_lookup(sh);
                if (!src) break;
                if (!upa && alpha == 255u)
                {
                    /* opaque -> hardware blit (with clip); backing
                     * SYSRAM da una parte -> copia CPU (il blitter e'
                     * cieco alla RAM di sistema) */
                    if (surface_is_sysram(src) || surface_is_sysram(dst))
                    {
                        surface_cpu_sync(src);
                        surface_cpu_sync(dst);
                        sw_blit(dst, src, sr, dx + ox, dy + oy);
                        surface_flush_writes(dst);
                        break;
                    }
                    int32_t x = dx + ox, y = dy + oy; uint32_t w = sr.w, h = sr.h;
                    int32_t sx = sr.x, sy = sr.y, x0 = x, y0 = y;
                    if (clip_rect(&x, &y, &w, &h, dst->width, dst->height))
                    {
                        sx += (x - x0); sy += (y - y0);
                        /* Same VRAM: overlap-aware path (see dv_blit). */
                        if (src->vram_offset == doff &&
                            src->pitch_words == dpitch)
                            x3100_hw_blit_overlap(doff, dpitch,
                                                  (uint32_t)sx, (uint32_t)sy,
                                                  (uint32_t)x, (uint32_t)y, w, h);
                        else
                            x3100_hw_blit(src->vram_offset, src->pitch_words,
                                           (uint32_t)sx, (uint32_t)sy,
                                           doff, dpitch, (uint32_t)x, (uint32_t)y, w, h);
                    }
                }
                else
                {
                    /* true alpha: software over, which READS the destination
                     * (replay_blit_alpha blends against dst pixels). The
                     * destination here may have just been written by the
                     * hardware blitter (the background fill / opaque layers
                     * below this one). Per PRG 5.2.1.3 a region the engine
                     * just wrote reads back garbage through the aperture
                     * until the engine drains — which is exactly why icons
                     * (use_pixel_alpha) came out half/blank on the full
                     * compose path while the software-only BGA path was
                     * fine. Drain first so the CPU reads settled pixels. */
                    if (g_x3100.engine_ok) x3100_wait_for_idle();
                    replay_blit_alpha(dst, src, sr, dx + ox, dy + oy, alpha, upa);
                }
                break;
            }
            case CMDLIST_REC_DRAW_GLYPHS: {
                dv_texture_t ah; uint32_t bgra, count;
                __builtin_memcpy(&ah,    p, 4); p += 4;
                __builtin_memcpy(&bgra,  p, 4); p += 4;
                __builtin_memcpy(&count, p, 4); p += 4;
                const dv_glyph_t *glyphs = (const dv_glyph_t *)p;
                p += count * sizeof(dv_glyph_t);
                surface_t *atlas = surface_lookup((dv_surface_t)ah);
                if (!atlas) break;
                if (surface_is_sysram(dst))
                {
                    /* Espansione mono = motore = solo VRAM: in shadow/
                     * SYSRAM i glifi passano dal campionamento CPU. */
                    surface_cpu_sync(atlas);
                    sw_draw_glyphs(dst, atlas, glyphs, count, bgra, ox, oy);
                    break;
                }
                replay_draw_glyphs(dst, atlas, glyphs, count, bgra, ox, oy);
                break;
            }
            case CMDLIST_REC_DRAW_LINE: {
                int32_t ax, ay, bx, by; uint32_t thick, bgra;
                __builtin_memcpy(&ax,    p, 4); p += 4;
                __builtin_memcpy(&ay,    p, 4); p += 4;
                __builtin_memcpy(&bx,    p, 4); p += 4;
                __builtin_memcpy(&by,    p, 4); p += 4;
                __builtin_memcpy(&thick, p, 4); p += 4;
                __builtin_memcpy(&bgra,  p, 4); p += 4;
                (void)thick;   /* 1px for now (reference parity) */
                ax += ox; ay += oy; bx += ox; by += oy;
                if (surface_is_sysram(dst))
                {
                    if (ay == by || ax == bx)
                    {
                        dv_rect_t lr = {
                            ax < bx ? ax : bx,  ay < by ? ay : by,
                            (uint32_t)((ax < bx ? bx - ax : ax - bx) + 1),
                            (uint32_t)((ay < by ? by - ay : ay - by) + 1),
                        };
                        sw_fill_rect(dst, lr, bgra);
                    }
                    else
                        sw_draw_line_1px(dst, ax, ay, bx, by, bgra);
                    break;
                }
                if (ay == by) {            /* horizontal -> 1px fill */
                    int32_t x0 = ax < bx ? ax : bx, x1 = ax < bx ? bx : ax;
                    int32_t x = x0, y = ay; uint32_t w = (uint32_t)(x1 - x0 + 1), h = 1u;
                    if (clip_rect(&x, &y, &w, &h, dst->width, dst->height))
                        x3100_hw_solid_fill(doff, dpitch, (uint32_t)x, (uint32_t)y, w, h, bgra);
                } else if (ax == bx) {     /* vertical -> 1px fill */
                    int32_t y0 = ay < by ? ay : by, y1 = ay < by ? by : ay;
                    int32_t x = ax, y = y0; uint32_t w = 1u, h = (uint32_t)(y1 - y0 + 1);
                    if (clip_rect(&x, &y, &w, &h, dst->width, dst->height))
                        x3100_hw_solid_fill(doff, dpitch, (uint32_t)x, (uint32_t)y, w, h, bgra);
                } else {                   /* diagonal -> hardware Bresenham */
                    x3100_hw_line(doff, dpitch, ax, ay, bx, by, bgra);
                }
                break;
            }
            default:
                return;   /* corrupt stream: stop rather than read garbage */
        }
    }
}

/* ---- #2b: hardware glyph path (mono expansion) + software alpha-over ----
 *
 * GLYPHS: the atlas is a 32bpp surface with coverage in the alpha channel
 * (binary, matching the reference).  We expand each glyph in HARDWARE via
 * x3100_hw_mono_blit, which needs a 1bpp linear mask in VRAM.  That mask
 * is built lazily, once per atlas, into a small VRAM allocation tracked by
 * a side table (so surface_t / state.h stay untouched), and invalidated by
 * dv_texture_update_region when the atlas changes (it may be DYNAMIC).
 *
 * ALPHA-OVER (blit_alpha with constant/pixel alpha): genuinely needs a
 * destination read, which is slow on this hardware and rare (window-level,
 * a handful of call sites).  Done in software over the VRAM mapping. */

#define ATLAS_MONO_NONE   0xFFFFFFFFu

static struct {
    bool     valid;
    uint32_t mono_off;          /* VRAM byte offset of the 1bpp mask */
    uint32_t mono_pitch_bytes;  /* row stride of the mask, in bytes  */
    uint32_t glyph_w, glyph_h;  /* glyph cell size                   */
} g_atlas_mono[MAX_SURFACES];

/* Invalidate (and free) an atlas's mono shadow — called on atlas change
 * or destroy.  slot is the surface slot index. */
static void atlas_mono_invalidate(uint32_t slot)
{
    if (slot >= MAX_SURFACES) return;
    if (g_atlas_mono[slot].valid && g_atlas_mono[slot].mono_off != ATLAS_MONO_NONE)
    {
        vram_block_t *b = vram_find(g_atlas_mono[slot].mono_off);
        if (b) vram_free(b);
    }
    g_atlas_mono[slot].valid = false;
    g_atlas_mono[slot].mono_off = ATLAS_MONO_NONE;
}

/* Build the 1bpp mask for an atlas by reading its alpha channel once from
 * VRAM and packing coverage bits (alpha != 0 -> 1).  Atlas layout (matches
 * reference): height = 256 * glyph_h, width = glyph_w, glyph N at row
 * N*glyph_h.  Returns false if the atlas geometry is unusable or VRAM for
 * the mask can't be allocated. */
static bool atlas_mono_build(surface_t *atlas, uint32_t slot)
{
    if (atlas->height < 256u) return false;
    uint32_t gw = atlas->width;
    uint32_t gh = atlas->height / 256u;
    if (gw == 0u || gh == 0u) return false;

    /* Pack each glyph as a CONTINUOUS bit stream: gw*gh bits with NO per-row
     * padding (bit index = row*gw + col), MSB-first — exactly what XY_TEXT_BLT
     * reads in bit-packing mode.  Each glyph's BYTE size is rounded UP to a
     * DWORD so every glyph start is dword-aligned: XY_TEXT_BLT's source address
     * (BR12) must be dword-aligned, and glyph g starts at base + g*glyph_bytes
     * (base block is >=16-aligned).  The pad bits past gw*gh are left 0. */
    uint32_t glyph_bits  = gw * gh;
    uint32_t glyph_bytes = ((glyph_bits + 31u) >> 5) << 2;   /* ceil(bits/32)*4 */
    uint32_t mask_bytes  = glyph_bytes * 256u;

    vram_block_t *blk = vram_alloc(mask_bytes, VRAM_ALIGN);
    if (!blk) return false;

    volatile uint8_t  *mono = (volatile uint8_t *)g_x3100.vram + blk->offset;
    /* Lettura via dispatch: regge anche un atlas SYSRAM (il MASCHERONE
     * mono resta in VRAM in ogni caso — lo consuma il motore). */
    volatile uint32_t *apx  = surface_pixels(atlas);
    uint32_t apitch = atlas->pitch_words;

    /* For each glyph (stacked vertically in the atlas: glyph g occupies atlas
     * rows [g*gh, g*gh+gh)), emit gw*gh bits continuously, MSB-first. */
    for (uint32_t g = 0; g < 256u; g++)
    {
        volatile uint8_t *gbase = mono + (uint32_t)g * glyph_bytes;
        for (uint32_t b = 0; b < glyph_bytes; b++) gbase[b] = 0u;   /* clear */

        uint32_t bit = 0;                       /* bit index within this glyph */
        for (uint32_t r = 0; r < gh; r++)
        {
            volatile uint32_t *src = apx + (uint32_t)(g * gh + r) * apitch;
            for (uint32_t c = 0; c < gw; c++, bit++)
            {
                if ((src[c] & 0xFF000000u) != 0u)
                    gbase[bit >> 3] |= (uint8_t)(0x80u >> (bit & 7u));  /* MSB-first (matches SW rasterizer) */
            }
        }
    }

    /* Commit the CPU mask writes to VRAM so the ENGINE (XY_TEXT_BLT) reads the
     * new mask, not stale memory — the same CPU-write -> engine-read hazard the
     * texture upload guards against.  A read-back of the last dword drains the
     * posted aperture write buffer; volatile keeps it from being elided.  Cost
     * falls only on a mask (re)build, never on the per-frame glyph blits. */
    {
        volatile uint32_t *last = (volatile uint32_t *)(mono + mask_bytes - 4u);
        volatile uint32_t sink = *last;
        (void)sink;
    }

    g_atlas_mono[slot].valid            = true;
    g_atlas_mono[slot].mono_off         = blk->offset;
    g_atlas_mono[slot].mono_pitch_bytes = glyph_bytes;   /* now: bytes per glyph */
    g_atlas_mono[slot].glyph_w          = gw;
    g_atlas_mono[slot].glyph_h          = gh;
    return true;
}

static void replay_draw_glyphs(surface_t *dst, surface_t *atlas,
                               const dv_glyph_t *glyphs, uint32_t count,
                               uint32_t fg_bgra, int32_t ox, int32_t oy)
{
    if (!dst || !atlas || !glyphs || count == 0u) return;

    uint32_t slot = (uint32_t)(atlas - &g_x3100.surfaces[0]);
    if (slot >= MAX_SURFACES) return;

    if (!g_atlas_mono[slot].valid)
        if (!atlas_mono_build(atlas, slot)) return;

    uint32_t gw    = g_atlas_mono[slot].glyph_w;
    uint32_t gh    = g_atlas_mono[slot].glyph_h;
    uint32_t mpb   = g_atlas_mono[slot].mono_pitch_bytes;
    uint32_t mbase = g_atlas_mono[slot].mono_off;

    /* Engine path: set a clip rect = the destination surface in the
     * XY_SETUP_BLT, then emit each glyph at its FULL cell and let the BLT
     * engine clip the overflow (text the app draws past a window edge).  The
     * 1bpp source is then always read at its native gw-bit row stride, so an
     * over-wide glyph is clipped, not sheared.  No CPU writes and NO drain —
     * the chrome fill queued before this run and these glyph blits run in ring
     * order, so the fill lands before each glyph. */
    if (g_x3100.engine_ok)
    {
        x3100_hw_text_setup(dst->vram_offset, dst->pitch_words, fg_bgra,
                            dst->width, dst->height);
        for (uint32_t i = 0; i < count; i++)
        {
            const dv_glyph_t *g = &glyphs[i];
            if (g->glyph_index >= 256u) continue;

            int32_t gx = g->x + ox, gy = g->y + oy;
            /* Coarse reject of glyphs wholly off the surface (the engine
             * trivial-rejects them too, but skip the ring traffic). */
            if (gx >= (int32_t)dst->width  || gx + (int32_t)gw <= 0 ||
                gy >= (int32_t)dst->height || gy + (int32_t)gh <= 0) continue;

            /* Full glyph at its dword-aligned base; the engine clips the
             * destination, so no source top-clip skip is needed. */
            x3100_hw_text_blit(mbase + g->glyph_index * mpb, gx, gy, gw, gh);
        }
        return;
    }

    /* CPU fallback (engine unavailable — a degraded boot state): masked writes
     * through the aperture, original conservative clipping (full glyph when
     * unclipped, byte-aligned top-clip, skip awkward partials).  No drain: with
     * no working engine there is nothing queued ahead that needs to land. */
    for (uint32_t i = 0; i < count; i++)
    {
        const dv_glyph_t *g = &glyphs[i];
        if (g->glyph_index >= 256u) continue;

        int32_t gx = g->x + ox, gy = g->y + oy;
        uint32_t w = gw, h = gh;
        int32_t gx0 = gx, gy0 = gy;
        if (!clip_rect(&gx, &gy, &w, &h, dst->width, dst->height)) continue;

        uint32_t clip_dx = (uint32_t)(gx - gx0);
        uint32_t clip_dy = (uint32_t)(gy - gy0);
        if (clip_dx != 0u) continue;                 /* no horizontal sub-glyph clip */
        uint32_t skip_bits = clip_dy * gw;
        if ((skip_bits & 7u) != 0u) continue;        /* keep byte alignment */
        uint32_t mono_off = mbase + g->glyph_index * mpb + (skip_bits >> 3);

        x3100_hw_mono_blit(mono_off, mpb,
                            dst->vram_offset, dst->pitch_words,
                            (uint32_t)gx, (uint32_t)gy, w, h, fg_bgra);
    }
}

/* Software alpha-over: blends a w*h source rect over the destination in the
 * VRAM mapping.  Reads dest (slow on this HW) — used only for the rare
 * constant/pixel-alpha window blits.  Matches the reference's visual rule:
 *   out = src*a + dst*(1-a),  a = const alpha or per-pixel src alpha. */
static void replay_blit_alpha(surface_t *dst, surface_t *src, dv_rect_t sr,
                              int32_t dx, int32_t dy, uint8_t alpha,
                              bool use_pixel_alpha)
{
    if (!dst || !src) return;
    int32_t x = dx, y = dy; uint32_t w = sr.w, h = sr.h;
    int32_t sx = sr.x, sy = sr.y, x0 = x, y0 = y;
    if (!clip_rect(&x, &y, &w, &h, dst->width, dst->height)) return;
    sx += (x - x0); sy += (y - y0);

    /* Base via surface_pixels: identico per VRAM (aperture mappata) e
     * SYSRAM — l'alpha-over CPU funziona su qualunque combinazione di
     * backing. Chi chiama drena il motore se una delle due e' VRAM. */
    volatile uint32_t *sbase = surface_pixels(src);
    volatile uint32_t *dbase = surface_pixels(dst);
    uint32_t sp = src->pitch_words, dp = dst->pitch_words;

    for (uint32_t row = 0; row < h; row++)
    {
        volatile uint32_t *srow = sbase + (uint32_t)(sy + (int32_t)row) * sp + (uint32_t)sx;
        volatile uint32_t *drow = dbase + (uint32_t)(y  + (int32_t)row) * dp + (uint32_t)x;
        for (uint32_t col = 0; col < w; col++)
        {
            uint32_t s = srow[col];
            uint32_t a = use_pixel_alpha ? (s >> 24) : (uint32_t)alpha;
            if (a == 0u) continue;
            if (a == 255u) { drow[col] = s; continue; }
            uint32_t d = drow[col];
            uint32_t ia = 255u - a;
            uint32_t rb = ((( (s & 0x00FF00FFu) * a) + ((d & 0x00FF00FFu) * ia)) >> 8) & 0x00FF00FFu;
            uint32_t g  = ((( (s & 0x0000FF00u) * a) + ((d & 0x0000FF00u) * ia)) >> 8) & 0x0000FF00u;
            drow[col] = 0xFF000000u | rb | g;
        }
    }
}

/* ==========================================================================
 *  dv protocol — draw diretti su surface  (block #2c)
 *
 *  Le op che la dobinterface 1.1 usa per bakare backbuf e corpi
 *  finestra come pixel finali (niente piu' replay di cmdlist a ogni
 *  compose). Instradamento per op: destinazione E sorgente in VRAM ->
 *  motore 2D (stesse primitive del replay); appena una parte e' SYSRAM
 *  -> blocco sw_* (il motore non vede la RAM di sistema). Il caso
 *  caldo e' interamente SYSRAM (backbuf/body), dove non serve alcun
 *  drain; i casi misti drenano prima (surface_cpu_sync) e committano
 *  dopo (surface_flush_writes). Speculare al driver mach64.
 * ========================================================================== */

int32_t dv_fill_rect(dv_surface_t dsth, dv_rect_t r, dv_color_t c)
{
    surface_t *dst = surface_lookup(dsth);
    if (!dst) return DV_ERR_HANDLE;
    uint32_t bgra = dv_color_to_bgra(c);
    surface_mark_dirty(dst, r.x, r.y, r.w, r.h);

    if (!surface_is_sysram(dst) && g_x3100.engine_ok)
    {
        int32_t x = r.x, y = r.y; uint32_t w = r.w, h = r.h;
        if (clip_rect(&x, &y, &w, &h, dst->width, dst->height))
            x3100_hw_solid_fill(dst->vram_offset, dst->pitch_words,
                                (uint32_t)x, (uint32_t)y, w, h, bgra);
        return DV_OK;
    }
    surface_cpu_sync(dst);
    sw_fill_rect(dst, r, bgra);
    surface_flush_writes(dst);
    return DV_OK;
}

int32_t dv_blit(dv_surface_t srch, dv_rect_t sr,
                dv_surface_t dsth, dv_point_t dp)
{
    surface_t *src = surface_lookup(srch);
    surface_t *dst = surface_lookup(dsth);
    if (!src || !dst) return DV_ERR_HANDLE;
    surface_mark_dirty(dst, dp.x, dp.y, sr.w, sr.h);

    if (!surface_is_sysram(src) && !surface_is_sysram(dst) && g_x3100.engine_ok)
    {
        int32_t x = dp.x, y = dp.y; uint32_t w = sr.w, h = sr.h;
        int32_t sx = sr.x, sy = sr.y, x0 = x, y0 = y;
        if (clip_rect(&x, &y, &w, &h, dst->width, dst->height))
        {
            sx += (x - x0); sy += (y - y0);
            /* Same VRAM region (scroll / same-surface move): route through
             * the overlap-aware primitive.  A plain forward XY_SRC_COPY on
             * overlapping rects with dst below/right of src eats its own
             * source rows mid-scan and corrupts silently; hw_blit_overlap
             * forwards when safe and band-decomposes (still fully in-engine)
             * when a reverse order is required. */
            if (src->vram_offset == dst->vram_offset &&
                src->pitch_words == dst->pitch_words)
                x3100_hw_blit_overlap(dst->vram_offset, dst->pitch_words,
                                      (uint32_t)sx, (uint32_t)sy,
                                      (uint32_t)x, (uint32_t)y, w, h);
            else
                x3100_hw_blit(src->vram_offset, src->pitch_words,
                              (uint32_t)sx, (uint32_t)sy,
                              dst->vram_offset, dst->pitch_words,
                              (uint32_t)x, (uint32_t)y, w, h);
        }
        return DV_OK;
    }
    surface_cpu_sync(src);
    surface_cpu_sync(dst);
    sw_blit(dst, src, sr, dp.x, dp.y);
    surface_flush_writes(dst);
    return DV_OK;
}

int32_t dv_blit_alpha(dv_surface_t srch, dv_rect_t sr,
                      dv_surface_t dsth, dv_point_t dp, uint8_t alpha)
{
    surface_t *src = surface_lookup(srch);
    surface_t *dst = surface_lookup(dsth);
    if (!src || !dst) return DV_ERR_HANDLE;
    if (alpha == 0u) return DV_OK;
    if (alpha == 255u) return dv_blit(srch, sr, dsth, dp);
    surface_mark_dirty(dst, dp.x, dp.y, sr.w, sr.h);

    /* Alpha vero: over in software (legge la destinazione). */
    surface_cpu_sync(src);
    surface_cpu_sync(dst);
    replay_blit_alpha(dst, src, sr, dp.x, dp.y, alpha, false);
    surface_flush_writes(dst);
    return DV_OK;
}

int32_t dv_blit_pixel_alpha(dv_surface_t srch, dv_rect_t sr,
                            dv_surface_t dsth, dv_point_t dp)
{
    surface_t *src = surface_lookup(srch);
    surface_t *dst = surface_lookup(dsth);
    if (!src || !dst) return DV_ERR_HANDLE;
    surface_mark_dirty(dst, dp.x, dp.y, sr.w, sr.h);
    surface_cpu_sync(src);
    surface_cpu_sync(dst);
    replay_blit_alpha(dst, src, sr, dp.x, dp.y, 255u, true);
    surface_flush_writes(dst);
    return DV_OK;
}

int32_t dv_blit_stretched(dv_surface_t srch, dv_rect_t sr,
                          dv_surface_t dsth, dv_rect_t dr)
{
    surface_t *src = surface_lookup(srch);
    surface_t *dst = surface_lookup(dsth);
    if (!src || !dst) return DV_ERR_HANDLE;
    surface_mark_dirty(dst, dr.x, dr.y, dr.w, dr.h);

    /* 1:1 "stretch" is just a copy — clients route resizes through this op
     * unconditionally, and the no-scale frames were paying the full CPU
     * path.  Hand them to dv_blit (engine when both sides are VRAM,
     * overlap-aware for same-surface). */
    if (sr.w == dr.w && sr.h == dr.h)
    {
        dv_point_t dp = { dr.x, dr.y };
        return dv_blit(srch, sr, dsth, dp);
    }

    /* Solo CPU: il motore di questo blocco non scala (il blitter GEN4 non
     * ha scaling; serve il render engine — Fase 3b). */
    surface_cpu_sync(src);
    surface_cpu_sync(dst);
    sw_blit_stretched(dst, src, sr, dr);
    surface_flush_writes(dst);
    return DV_OK;
}

int32_t dv_draw_glyphs(dv_surface_t dsth, dv_texture_t atlash,
                       const dv_glyph_t *glyphs, uint32_t count,
                       dv_color_t c)
{
    surface_t *dst   = surface_lookup(dsth);
    surface_t *atlas = surface_lookup((dv_surface_t)atlash);
    if (!dst || !atlas) return DV_ERR_HANDLE;
    if (!glyphs || count == 0u) return DV_OK;
    uint32_t bgra = dv_color_to_bgra(c);

    /* Danno: bbox della run di glifi (celle gw x gh). */
    if (atlas->height >= 256u && atlas->width > 0u)
    {
        uint32_t gw = atlas->width, gh = atlas->height / 256u;
        int32_t bx0 = glyphs[0].x, by0 = glyphs[0].y;
        int32_t bx1 = bx0, by1 = by0;
        for (uint32_t i = 1; i < count; i++)
        {
            if (glyphs[i].x < bx0) bx0 = glyphs[i].x;
            if (glyphs[i].y < by0) by0 = glyphs[i].y;
            if (glyphs[i].x > bx1) bx1 = glyphs[i].x;
            if (glyphs[i].y > by1) by1 = glyphs[i].y;
        }
        surface_mark_dirty(dst, bx0, by0,
                           (uint32_t)(bx1 - bx0) + gw,
                           (uint32_t)(by1 - by0) + gh);
    }

    /* Destinazione VRAM con atlas VRAM: espansione mono in hardware
     * (stesso percorso del replay #2b, maschera 1bpp riusata). */
    if (!surface_is_sysram(dst) && !surface_is_sysram(atlas)
        && g_x3100.engine_ok)
    {
        replay_draw_glyphs(dst, atlas, glyphs, count, bgra, 0, 0);
        return DV_OK;
    }
    surface_cpu_sync(atlas);
    surface_cpu_sync(dst);
    sw_draw_glyphs(dst, atlas, glyphs, count, bgra, 0, 0);
    surface_flush_writes(dst);
    return DV_OK;
}

/* ==========================================================================
 *  dv protocol — layers, compositing, page flip  (block #3)
 *
 *  Same structure as the BGA reference, with one deliberate difference:
 *  SINGLE FRAMEBUFFER (state.h decision).  BGA composes into a back page
 *  and page_flip swaps; we compose directly into the one primary at VRAM
 *  offset 0 and align to vblank so the fast 2D engine writes during the
 *  retrace -> effectively tear-free without a reserved back page.
 *  dv_page_flip is therefore a no-op that returns DV_OK.
 * ========================================================================== */

static layer_t *layer_lookup(dv_layer_t h)
{
    if (h == DV_HANDLE_NONE) return NULL;
    uint32_t s = HANDLE_TO_SLOT(h);
    if (s >= MAX_LAYERS || !g_x3100.layers[s].used) return NULL;
    return &g_x3100.layers[s];
}

static fence_t *fence_lookup(dv_fence_t h)
{
    if (h == DV_HANDLE_NONE) return NULL;
    uint32_t s = HANDLE_TO_SLOT(h);
    if (s >= MAX_FENCES || !g_x3100.fences[s].used) return NULL;
    return &g_x3100.fences[s];
}

/* Polled wait for the start of vertical blank, on pipe B's scan-line counter
 * (PIPEBDSL): leave any current vblank, then wait for the next one to start —
 * which is exactly when the armed DSPBSURF latches.  Bounded so a stuck pipe
 * can't hang us.  The bound is WALL-CLOCK (clock_ms), not an MMIO-read count:
 * see below. */
static void x3100_wait_for_vblank(void)
{
    /* The bound MUST be wall-clock, not a fixed MMIO-read count.  The old guard
     * (0x20000 reads) is only ~25-35 ms IF each MMIO read costs ~125-270 ns; on
     * this chipset reads can be ~50 ns, making the same count elapse in ~6-7 ms
     * — LESS than one 60 Hz refresh (16.7 ms).  The loop then gave up BEFORE the
     * next vblank, the caller swapped front/back before DSPBSURF had latched,
     * and the following dv_compose drew into the page STILL being scanned: the
     * see-through FLASH on window drag / resize / command-bar updates.  (bga and
     * mach64 don't hit this — they wait on a real vblank STATUS bit, not a
     * scanline count.)  A clock_ms() deadline of one refresh + margin (~20 ms)
     * guarantees we never return before the hardware latch, which happens at the
     * next vblank, at most one refresh away — regardless of how PIPEB_DSL reads
     * or how fast MMIO is.  When DSL behaves we still return promptly at the real
     * vblank (≤16.7 ms); only a genuinely stuck pipe waits the full ~20 ms.
     *
     * SLEEP-UNTIL-VBLANK (CPU fix): this used to be a PURE MMIO spin — an
     * average of ~8.3 ms of busy CPU per vsync'd flip. With any client
     * repainting on a timer (taskmgr: tick 300 ms -> 3.33 flip/s) that
     * alone burned a steady double-digit slice of a core INSIDE
     * dobinterface, read in the field as "8-10% at idle". Now we compute
     * the lines left to the vblank from the CURRENT scanline and
     * de-schedule (sleep_ms) for slightly LESS than that time — the
     * divisor uses vtotal ≈ active + 12.5% blanking, rounding the sleep
     * DOWN — then finish with the short precise spin. The latch guarantee
     * is untouched: the spin below still runs to the true vblank edge
     * with the same wall-clock deadline. */
    uint32_t h     = g_x3100.mode.height;
    uint32_t start = clock_ms();

    {
        uint32_t dsl = x3100_mmio_r32(g_x3100.reg_pipe_dsl) & 0x00001FFFu;
        if (dsl < h)
        {
            uint32_t vtotal_est = h + h / 8u;          /* ~12.5% blanking */
            uint32_t ms = (h - dsl) * 16u / vtotal_est; /* < time to vblank */
            if (ms > 2u) sleep_ms(ms - 1u);
        }
    }

    while ((x3100_mmio_r32(g_x3100.reg_pipe_dsl) & 0x00001FFFu) >= h)
        if ((uint32_t)(clock_ms() - start) >= 20u) return;
    while ((x3100_mmio_r32(g_x3100.reg_pipe_dsl) & 0x00001FFFu) < h)
        if ((uint32_t)(clock_ms() - start) >= 20u) return;
}

/* Most-recent dv_compose layer-order snapshot, for the on-screen engine
 * diagnostic (no serial logs on the Armada). Filled in dv_compose after the
 * z-sort, copied out in x3100_engine_diag. */
static uint32_t g_diag_compose_layer_count = 0;
static int32_t  g_diag_compose_layer_z[16];
static uint32_t g_diag_compose_layer_is_cmdlist[16];
static uint32_t g_diag_compose_layer_x[16];
static uint32_t g_diag_compose_layer_w[16];

int32_t dv_layer_create(dv_vproc_t v, const dv_layer_desc_t *d, dv_layer_t *out)
{
    if (!d || !out) return DV_ERR_INVAL;
    if (!vproc_lookup(v)) return DV_ERR_HANDLE;
    if (d->source  != DV_HANDLE_NONE && !surface_lookup(d->source))  return DV_ERR_HANDLE;
    if (d->cmdlist != DV_HANDLE_NONE && !cmdlist_lookup(d->cmdlist)) return DV_ERR_HANDLE;
    int slot = alloc_layer_slot();
    if (slot < 0) return DV_ERR_NOMEM;
    layer_t *L = &g_x3100.layers[slot];
    L->owner           = v;
    L->source          = d->source;
    L->z               = d->z;
    L->alpha           = d->alpha;
    L->visible         = d->visible;
    L->use_pixel_alpha = d->use_pixel_alpha;
    L->src_rect        = d->src_rect;
    L->dst_rect        = d->dst_rect;
    L->cmdlist         = d->cmdlist;
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

/* Build a transient surface_t describing the COMPOSE TARGET page, so the
 * replay/blit primitives (which take a surface_t*) can target it. In
 * double-buffer mode this is the BACK page (hidden); dv_page_flip later
 * makes it visible. In single-buffer fallback back_offset == front_offset
 * == 0, so this is the visible primary, exactly as before. */
static surface_t primary_surface(void)
{
    surface_t s;
    s.used        = true;
    s.owner       = DV_HANDLE_NONE;
    s.width       = g_x3100.mode.width;
    s.height      = g_x3100.mode.height;
    s.pitch_words = g_x3100.scan_pitch_px;  /* PADDED scanout pitch */
    s.format      = DV_FMT_BGRA8888;
    s.flags       = 0;
    s.vram_offset = g_x3100.back_offset;    /* compose into the hidden page */
    s.vram_bytes  = g_x3100.primary_bytes;
    s.block       = NULL;
    s.sys_pixels  = NULL;                    /* la pagina e' VRAM, sempre */
    return s;
}

int32_t dv_compose(uint32_t display_id, dv_fence_t fence_signal)
{
    if (display_id != 0) return DV_ERR_INVAL;

    /* Gather visible layers, insertion-sort by z (low to high). */
    layer_t *vis[MAX_LAYERS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_LAYERS; i++)
        if (g_x3100.layers[i].used && g_x3100.layers[i].visible)
            vis[n++] = &g_x3100.layers[i];
    for (uint32_t i = 1; i < n; i++)
    {
        layer_t *cur = vis[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && vis[j]->z > cur->z) { vis[j + 1] = vis[j]; j--; }
        vis[j + 1] = cur;
    }

    /* Snapshot the post-sort paint order for on-screen diagnostics (no logs
     * on the metal). Records z / kind / x / w of each visible layer in the
     * exact order they will be painted, so enginediag can show whether
     * windows really sit above the desktop-icon backbuf layer. */
    {
        uint32_t cap = n < 16u ? n : 16u;
        g_diag_compose_layer_count = cap;
        for (uint32_t i = 0; i < cap; i++)
        {
            g_diag_compose_layer_z[i]          = vis[i]->z;
            g_diag_compose_layer_is_cmdlist[i] = (vis[i]->cmdlist != DV_HANDLE_NONE) ? 1u : 0u;
            g_diag_compose_layer_x[i]          = (uint32_t)vis[i]->dst_rect.x;
            g_diag_compose_layer_w[i]          = vis[i]->dst_rect.w;
        }
    }

    surface_t prim = primary_surface();

    /* Frame singolo con shadow: la compose intera avviene FUORI
     * schermo, in RAM di sistema — prim diventa una surface SYSRAM che
     * punta alla shadow e ogni percorso sotto (clear, cmdlist replay,
     * blit di layer) instrada da solo sul ramo CPU. La presentazione
     * e' di dv_page_flip. Niente attesa di vblank qui: nulla di cio'
     * che disegniamo e' visibile. */
    if (!g_x3100.double_buffered && g_x3100.shadow != NULL)
    {
        prim.vram_offset = 0u;              /* inutilizzato: backing sysram */
        prim.vram_bytes  = 0u;
        prim.sys_pixels  = g_x3100.shadow;
    }

    /* ---- damage: quanto di questo frame e' davvero cambiato? ----
     * Solo in modalita' shadow: confronto dei layer visibili con lo
     * snapshot dell'ultimo frame. Geometria/z/alpha/sorgente diversi
     * => vecchia U nuova area dipinta; stessa geometria ma gen della
     * sorgente avanzata => box sporco della sorgente traslato a
     * schermo. Layer cmdlist (legacy, il replay non sa di scissor) o
     * primo frame => danno pieno. Il box cb* delimita TUTTO il lavoro
     * a valle: clear, blit dei layer, presentazione. */
    bool use_damage = (!g_x3100.double_buffered && g_x3100.shadow != NULL);
    int32_t cbx0 = 0, cby0 = 0;
    int32_t cbx1 = (int32_t)g_x3100.mode.width;
    int32_t cby1 = (int32_t)g_x3100.mode.height;
    bool dmg_nothing = false;
    if (use_damage)
    {
        bool dmg_full = !g_x3100.composed_valid;
        int32_t dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;   /* bbox, vuoto */
        bool seen[MAX_LAYERS];
        for (uint32_t s = 0; s < MAX_LAYERS; s++) seen[s] = false;

        for (uint32_t i = 0; i < n && !dmg_full; i++)
        {
            layer_t *L = vis[i];
            uint32_t slot = (uint32_t)(L - g_x3100.layers);
            seen[slot] = true;
            if (L->cmdlist != DV_HANDLE_NONE) { dmg_full = true; break; }
            surface_t *src = surface_lookup(L->source);
            if (!src) continue;
            int32_t sslot = (int32_t)(src - g_x3100.surfaces);
            composed_layer_t *P = &g_x3100.composed[slot];

            int32_t nx0 = L->dst_rect.x, ny0 = L->dst_rect.y;
            int32_t nx1 = nx0 + (int32_t)L->src_rect.w;
            int32_t ny1 = ny0 + (int32_t)L->src_rect.h;

            if (!P->used || P->is_cmdlist || P->surf_slot != sslot ||
                P->z != L->z || P->alpha != L->alpha ||
                P->use_pixel_alpha != L->use_pixel_alpha ||
                P->dst_x != L->dst_rect.x || P->dst_y != L->dst_rect.y ||
                P->src_rect.x != L->src_rect.x ||
                P->src_rect.y != L->src_rect.y ||
                P->src_rect.w != L->src_rect.w ||
                P->src_rect.h != L->src_rect.h)
            {
                if (P->used && !P->is_cmdlist)
                    dmg_add(&dx0, &dy0, &dx1, &dy1,
                            P->dst_x, P->dst_y,
                            P->dst_x + (int32_t)P->src_rect.w,
                            P->dst_y + (int32_t)P->src_rect.h);
                dmg_add(&dx0, &dy0, &dx1, &dy1, nx0, ny0, nx1, ny1);
            }
            else if (P->src_gen != src->gen)
            {
                if (src->dirty_x0 < src->dirty_x1 &&
                    src->dirty_y0 < src->dirty_y1)
                {
                    /* box sporco della sorgente -> coordinate schermo */
                    int32_t tx0 = nx0 + (src->dirty_x0 - L->src_rect.x);
                    int32_t ty0 = ny0 + (src->dirty_y0 - L->src_rect.y);
                    int32_t tx1 = nx0 + (src->dirty_x1 - L->src_rect.x);
                    int32_t ty1 = ny0 + (src->dirty_y1 - L->src_rect.y);
                    if (tx0 < nx0) tx0 = nx0;
                    if (ty0 < ny0) ty0 = ny0;
                    if (tx1 > nx1) tx1 = nx1;
                    if (ty1 > ny1) ty1 = ny1;
                    dmg_add(&dx0, &dy0, &dx1, &dy1, tx0, ty0, tx1, ty1);
                }
                else
                    /* gen avanzata senza box (op tutta clippata o caso
                     * anomalo): prudenza, danno = area dipinta. */
                    dmg_add(&dx0, &dy0, &dx1, &dy1, nx0, ny0, nx1, ny1);
            }
        }
        /* Layer spariti o nascosti dall'ultimo frame: la loro vecchia
         * area va ridipinta con quel che c'e' sotto. */
        for (uint32_t s = 0; s < MAX_LAYERS && !dmg_full; s++)
        {
            composed_layer_t *P = &g_x3100.composed[s];
            if (!P->used || seen[s]) continue;
            if (P->is_cmdlist) { dmg_full = true; break; }
            dmg_add(&dx0, &dy0, &dx1, &dy1,
                    P->dst_x, P->dst_y,
                    P->dst_x + (int32_t)P->src_rect.w,
                    P->dst_y + (int32_t)P->src_rect.h);
        }

        if (!dmg_full)
        {
            /* clamp allo schermo */
            if (dx0 < 0) dx0 = 0;
            if (dy0 < 0) dy0 = 0;
            if (dx1 > cbx1) dx1 = cbx1;
            if (dy1 > cby1) dy1 = cby1;
            if (dx0 >= dx1 || dy0 >= dy1)
                dmg_nothing = true;         /* frame identico: zero lavoro */
            else
            {
                cbx0 = dx0; cby0 = dy0; cbx1 = dx1; cby1 = dy1;
            }
        }
    }

    /* Double-buffer: compose into the hidden back page. No vblank wait here
     * — nothing we draw is visible until dv_page_flip swaps the scanout
     * offset, so the ~9.5 ms full recompose happens entirely off-screen and
     * the panel never sees a half-built frame. (Single-buffer fallback:
     * back==front, so we ARE drawing into the visible page; keep the old
     * vblank alignment in that case to stay tear-light.) */
    if (!g_x3100.double_buffered && g_x3100.shadow == NULL)
        x3100_wait_for_vblank();

    /* Skip the full-screen clear when the bottom layer already covers every
     * pixel opaquely — replaying/blitting it overwrites the whole frame, so
     * the clear is redundant and only causes a visible black flash on this
     * single-buffer path.  Two covering cases:
     *   (a) a fullscreen opaque SURFACE cover, or
     *   (b) a cmdlist whose contents start with a fullscreen opaque fill
     *       (the desktop background) — covers_fullscreen tracks this. */
    bool skip_clear = false;
    if (n > 0)
    {
        layer_t *bot = vis[0];
        bool full_dst =
            bot->dst_rect.x == 0 && bot->dst_rect.y == 0 &&
            bot->dst_rect.w == g_x3100.mode.width &&
            bot->dst_rect.h == g_x3100.mode.height;

        if (bot->cmdlist != DV_HANDLE_NONE)
        {
            cmdlist_t *bcl = cmdlist_lookup(bot->cmdlist);
            if (bcl && bcl->covers_fullscreen) skip_clear = true;
        }
        else if (full_dst &&
                 bot->alpha == 255u && !bot->use_pixel_alpha &&
                 bot->src_rect.w == g_x3100.mode.width &&
                 bot->src_rect.h == g_x3100.mode.height)
            skip_clear = true;
    }
    if (!skip_clear && !dmg_nothing)
    {
        if (surface_is_sysram(&prim))
        {
            /* Solo il box di danno: fuori la shadow e' gia' corrente. */
            dv_rect_t box = { cbx0, cby0,
                              (uint32_t)(cbx1 - cbx0), (uint32_t)(cby1 - cby0) };
            sw_fill_rect(&prim, box, 0x00000000u);
        }
        else
            x3100_hw_solid_fill(prim.vram_offset, prim.pitch_words,
                                 0, 0, prim.width, prim.height, 0x00000000u);
    }

    for (uint32_t i = 0; i < n && !dmg_nothing; i++)
    {
        layer_t *L = vis[i];
        if (L->cmdlist != DV_HANDLE_NONE)        /* cmdlist mode wins */
        {
            cmdlist_t *cl = cmdlist_lookup(L->cmdlist);
            if (cl) cmdlist_replay(cl, &prim, L->dst_rect.x, L->dst_rect.y);
            /* No drain here. cmdlist_replay leaves engine ops in flight, but
             * the engine executes them in FIFO order, and the NEXT layer is
             * one of:
             *   - another engine op (HW blit / fill): FIFO already orders it
             *     after this layer, so it lands on top correctly;
             *   - a CPU alpha-blit that READS the page: that branch drains
             *     before reading (below), which is the only moment ordering
             *     actually matters (engine-write must precede CPU-read).
             * Draining unconditionally here was over-serialization: it stalled
             * the CPU after every layer, ~60×/s during a drag/scroll, and the
             * variable drain time made the frame rate irregular (stutter).
             * Correctness (icons-under-windows, no read-back garbage) is
             * preserved by the pre-read drains and the final pre-flip drain. */
            continue;
        }
        surface_t *src = surface_lookup(L->source);
        if (!src) continue;
        int32_t dx = L->dst_rect.x, dy = L->dst_rect.y;
        dv_rect_t sr = L->src_rect;
        if (use_damage)
        {
            /* Ritaglio sul box di danno: l'origine sorgente trasla di
             * pari passo. Fuori dal box non si dipinge nulla. */
            int32_t ix0 = dx > cbx0 ? dx : cbx0;
            int32_t iy0 = dy > cby0 ? dy : cby0;
            int32_t ix1 = dx + (int32_t)sr.w; if (ix1 > cbx1) ix1 = cbx1;
            int32_t iy1 = dy + (int32_t)sr.h; if (iy1 > cby1) iy1 = cby1;
            if (ix0 >= ix1 || iy0 >= iy1) continue;
            sr.x += (ix0 - dx); sr.y += (iy0 - dy);
            sr.w = (uint32_t)(ix1 - ix0); sr.h = (uint32_t)(iy1 - iy0);
            dx = ix0; dy = iy0;
        }
        if (L->use_pixel_alpha)
        {
            /* Software over READS dst — drain pending engine writes to the
             * page first (PRG 5.2.1.3 read-back hazard, and the moment that
             * enforces z-order against engine-drawn layers below). */
            if (g_x3100.engine_ok) x3100_wait_for_idle();
            replay_blit_alpha(&prim, src, sr, dx, dy, 255u, true);
        }
        else if (L->alpha == 255u)
        {
            if (surface_is_sysram(src) || surface_is_sysram(&prim))
            {
                /* Il caso principe della pipeline 1.1: backbuf e corpi
                 * finestra vivono in RAM di sistema, che il blitter non
                 * legge (e in modalita' shadow anche la destinazione e'
                 * RAM) — copia CPU dei pixel finali nella pagina di
                 * compose. Sync mirati sulle parti VRAM, flush a valle
                 * (no-op sulla shadow). Niente flash: ogni pixel
                 * scritto e' definitivo, mai stati intermedi a schermo. */
                surface_cpu_sync(src);
                surface_cpu_sync(&prim);
                sw_blit(&prim, src, sr, dx, dy);
                surface_flush_writes(&prim);
                continue;
            }
            /* opaque surface blit via the hardware blitter (clipped). Pure
             * engine op — FIFO-ordered after prior engine work, no drain. */
            uint32_t w = sr.w, h = sr.h;
            int32_t x = dx, y = dy, sx = sr.x, sy = sr.y, x0 = x, y0 = y;
            if (clip_rect(&x, &y, &w, &h, prim.width, prim.height))
            {
                sx += (x - x0); sy += (y - y0);
                x3100_hw_blit(src->vram_offset, src->pitch_words,
                               (uint32_t)sx, (uint32_t)sy,
                               prim.vram_offset, prim.pitch_words,
                               (uint32_t)x, (uint32_t)y, w, h);
            }
        }
        else
        {
            /* Constant-alpha software over also READS dst — same pre-read
             * drain as the pixel-alpha branch. */
            if (g_x3100.engine_ok) x3100_wait_for_idle();
            replay_blit_alpha(&prim, src, sr, dx, dy, L->alpha, false);
        }
    }

    /* Make sure the engine has drained before signalling completion, so a
     * fence waiter sees a finished frame.  In CPU mode the writes are
     * already complete, so skip the (pointless, possibly spinning) drain. */
    if (use_damage)
    {
        /* Snapshot per il prossimo frame + consumo dei box sporchi
         * delle sorgenti composte + accumulo del box da presentare
         * (unione: piu' compose possono precedere un solo flip). */
        for (uint32_t s = 0; s < MAX_LAYERS; s++)
            g_x3100.composed[s].used = false;
        for (uint32_t i = 0; i < n; i++)
        {
            layer_t *L = vis[i];
            uint32_t slot = (uint32_t)(L - g_x3100.layers);
            composed_layer_t *P = &g_x3100.composed[slot];
            P->used            = true;
            P->is_cmdlist      = (L->cmdlist != DV_HANDLE_NONE);
            P->z               = L->z;
            P->alpha           = L->alpha;
            P->use_pixel_alpha = L->use_pixel_alpha;
            P->src_rect        = L->src_rect;
            P->dst_x           = L->dst_rect.x;
            P->dst_y           = L->dst_rect.y;
            P->surf_slot       = -1;
            P->src_gen         = 0u;
            if (!P->is_cmdlist)
            {
                surface_t *lsrc = surface_lookup(L->source);
                if (lsrc)
                {
                    P->surf_slot = (int32_t)(lsrc - g_x3100.surfaces);
                    P->src_gen   = lsrc->gen;
                    lsrc->dirty_x0 = lsrc->dirty_y0 = 0;
                    lsrc->dirty_x1 = lsrc->dirty_y1 = 0;
                }
            }
        }
        g_x3100.composed_valid = true;

        if (!dmg_nothing)
            dmg_add(&g_x3100.present_x0, &g_x3100.present_y0,
                    &g_x3100.present_x1, &g_x3100.present_y1,
                    cbx0, cby0, cbx1, cby1);
    }

    if (g_x3100.engine_ok) x3100_wait_for_idle();

    fence_t *f = fence_lookup(fence_signal);
    if (f) f->current_value = f->target_value;
    return DV_OK;
}

/* Real page-flip on the double-buffered path: the back page now holds the
 * finished frame (composed off-screen by dv_compose). Wait for vblank, then
 * point the CRTC display-start at the back page by writing CRTC_OFF_PITCH —
 * an atomic register write that swaps the whole visible frame in one go and
 * always fits inside the 784 us blank (the compose did NOT). Then exchange
 * front/back so the next dv_compose targets the page that just became
 * hidden. DV_FLIP_VSYNC is honoured by waiting for the blank before the
 * swap; without it we swap immediately (a single torn frame, e.g. the
 * cursor path, where tear on a tiny sprite is invisible).
 *
 * Single-buffer fallback (back==front): there is no page to flip — compose
 * already drew into the visible primary — so this just signals the fence,
 * exactly as before. */

/* Wall-clock wait (ms) after arming DSPBSURF before reusing the old front
 * page.  The latch is at the next vblank, <= one refresh (~16.7 ms @ 60 Hz)
 * after the arm; 20 covers that plus clock granularity.  Raise toward ~34 to
 * also cover a worst-case one-frame arm slip if a flash ever survives. */
#define X3100_FLIP_LATCH_MS     20u

int32_t dv_page_flip(uint32_t display_id, uint32_t flags, dv_fence_t fence_signal)
{
    if (display_id != 0) return DV_ERR_INVAL;

    /* Presentazione shadow (frame singolo): UNA copia sequenziale di
     * pixel finali sul front visibile — l'unica scrittura che lo
     * scanout veda mai; gli strati della compose vivono in RAM di
     * sistema. Drain prima: un compose_rect precedente puo' avere op
     * del motore in volo sul front. VSYNC allinea la partenza al
     * blank; la copia sfora comunque il blank, quindi al peggio una
     * riga di tear in transito sui full-repaint — mai il flash
     * "sfondo si', finestre non ancora". */
    if (!g_x3100.double_buffered && g_x3100.shadow != NULL)
    {
        if (g_x3100.engine_ok) x3100_wait_for_idle();
        if (flags & DV_FLIP_VSYNC)
            x3100_wait_for_vblank();

        volatile uint32_t *fb = (volatile uint32_t *)
            ((volatile uint8_t *)g_x3100.vram + g_x3100.front_offset);
        const uint32_t *sh = (const uint32_t *)g_x3100.shadow;
        uint32_t w = g_x3100.scan_pitch_px;   /* row stride: padded pitch */

        /* Presentazione a box: copia SOLO il bbox di danno accumulato
         * dalle compose dal flip precedente. Frame identico => box
         * vuoto => zero scritture verso l'aperture. La correttezza sta
         * nell'invariante (vedi x3100_state.h): il box e' sempre un
         * sottoinsieme di cio' che la compose ha appena ricomposto
         * nella shadow, quindi cio' che sta fuori (patch in place di
         * compose_rect comprese) non regredisce mai. */
        int32_t px0 = g_x3100.present_x0, py0 = g_x3100.present_y0;
        int32_t px1 = g_x3100.present_x1, py1 = g_x3100.present_y1;
        if (px0 < px1 && py0 < py1)
        {
            uint32_t bw = (uint32_t)(px1 - px0);
            for (int32_t y = py0; y < py1; y++)
                fast_copy32(fb + (uint32_t)y * w + (uint32_t)px0,
                            sh + (uint32_t)y * w + (uint32_t)px0, bw);
            g_x3100.present_x0 = g_x3100.present_y0 = 0;
            g_x3100.present_x1 = g_x3100.present_y1 = 0;
        }

        fence_t *sf = fence_lookup(fence_signal);
        if (sf) sf->current_value = sf->target_value;
        return DV_OK;
    }

    if (g_x3100.double_buffered)
    {
        /* The engine must have drained so the back page is final before we
         * make it visible. dv_compose already drains when engine_ok, but a
         * caller may flip without an intervening full compose; be safe. */
        if (g_x3100.engine_ok) x3100_wait_for_idle();

        /* Arm DSPBSURF, then guarantee the latch by WALL-CLOCK before reusing
         * the old front page.  This is the definitive fix for the residual
         * RANDOM flash.
         *
         * DSPBSURF on i965 is vblank-latched: the armed value loads at the
         * start of the NEXT vblank.  Two things must hold to avoid reusing a
         * page that is still on screen:
         *   (1) the arm must not land in the narrow pre-vblank window where the
         *       hardware has already sampled the armed value (else it slips one
         *       frame).  We arm only when DSL is comfortably inside active scan
         *       (< 7/8 of the active height = >= ~1/8 frame before vblank), and
         *       DEBOUNCE that test (two consecutive agreeing samples) so a
         *       single garbled PIPEB_DSL reading can't trigger an early arm.
         *   (2) we must not reuse the old front until the latch has actually
         *       happened.  PIPEB_DSL on this panel occasionally returns a
         *       spurious sample, and the previous code polled DSL to detect the
         *       latching vblank — a bad sample let that wait exit early and the
         *       swap reused a page still being scanned (the flash).  Instead we
         *       wait by clock_ms: the latch is at the next vblank, AT MOST one
         *       refresh (~16.7 ms at 60 Hz) after the arm — guaranteed by the
         *       panel timing, independent of how DSL reads.  Waiting one refresh
         *       + margin therefore makes the reuse provably safe WITHOUT trusting
         *       any DSL sample.
         *
         * Cost: the full-frame flip paces to ~50 fps; the dirty-rect path
         * (command bar / resize) does not go through here and is unaffected.
         * Knob: if a flash ever survives this, raising X3100_FLIP_LATCH_MS to
         * ~34 covers even a worst-case one-frame arm slip (trading fps for a
         * hard guarantee). */
        (void)flags;
        {
            uint32_t h    = g_x3100.mode.height;
            uint32_t safe = h - (h >> 3);               /* 7/8 of active height */
            uint32_t t0   = clock_ms();
            for (;;)
            {
                if ((uint32_t)(clock_ms() - t0) >= 20u) break;   /* stuck DSL: bail */
                uint32_t d1 = x3100_mmio_r32(g_x3100.reg_pipe_dsl) & 0x00001FFFu;
                if (d1 >= safe) { sleep_ms(1); continue; }  /* yield, not hot-spin */
                uint32_t d2 = x3100_mmio_r32(g_x3100.reg_pipe_dsl) & 0x00001FFFu;
                if (d2 < safe) break;                            /* two samples agree */
            }
        }
        uint32_t t_arm = clock_ms();
        x3100_mmio_w32(g_x3100.reg_dsp_surf, x3100_gaddr(g_x3100.back_offset));
        (void)x3100_mmio_r32(g_x3100.reg_dsp_surf);     /* post the arming write */

        /* Wait out the latch by wall-clock (yielding, not busy-spinning). */
        while ((uint32_t)(clock_ms() - t_arm) < X3100_FLIP_LATCH_MS)
            sleep_ms(1);

        /* The armed surface has now latched: the back page is the scanned-out
         * front.  Reuse the old front as the new back only at this point. */
        uint32_t tmp          = g_x3100.front_offset;
        g_x3100.front_offset  = g_x3100.back_offset;
        g_x3100.back_offset   = tmp;
    }

    fence_t *f = fence_lookup(fence_signal);
    if (f) f->current_value = f->target_value;
    return DV_OK;
}

/* dv_compose_rect — recompose ONLY the pixels inside (rx,ry,rw,rh),
 * respecting z-order, leaving every pixel outside the rectangle exactly
 * as it was.  This is the dirty-rectangle path: dobinterface calls it
 * with the rectangle that actually changed (a clicked button, a scrolled
 * bar, a resized window's union rect) instead of recomposing the whole
 * screen.  Because only the changed pixels are touched, the panel never
 * scans a half-cleared frame, which is what eliminates the "see-through"
 * flash that is visible on real hardware (the Mach64/E500) even though
 * QEMU's emulation hides it.
 *
 * Mechanism: the hardware SCISSOR (SC_LEFT/RIGHT/TOP/BOTTOM) is set to the
 * dirty rect for the whole pass.  The engine then discards any pixel a
 * primitive would write outside the rect — automatically, for fills,
 * blits, glyphs and alpha alike — so we can replay every layer's cmdlist
 * unmodified and the result is clipped to the rect.  No global clear: the
 * bottom (background) layer redraws the rect's backdrop, then the windows
 * above it redraw their portion, all within the scissor.  Scissor is
 * restored to full-screen before returning so the normal full dv_compose
 * path keeps working.
 *
 * CPU-fallback note: when the blitter is unavailable (engine_ok == false)
 * the software paths do their own clip_rect against the surface bounds,
 * NOT against this rect, so the scissor has no effect and a full-rect clip
 * isn't guaranteed.  On this hardware the engine is up (engine_ok), so the
 * scissor path is the live one; the CPU path still produces a correct
 * (if not minimal) frame because each layer clips to the surface. */
/* Ensure the scratch buffer is at least w x h pixels.  Grows (frees +
 * reallocates) when a larger rect arrives; reuses the existing block
 * otherwise.  Returns false if VRAM can't satisfy the request — the
 * caller then falls back to composing directly into the primary. */
static bool scratch_ensure(uint32_t w, uint32_t h)
{
    if (g_x3100.scratch_block &&
        g_x3100.scratch_w >= w && g_x3100.scratch_h >= h)
        return true;

    /* Grow to cover the request; round pitch to 8 px (Mach64 surface
     * rule) and bias capacity up a little so small growth steps don't
     * thrash the allocator. */
    uint32_t nw = w > g_x3100.scratch_w ? w : g_x3100.scratch_w;
    uint32_t nh = h > g_x3100.scratch_h ? h : g_x3100.scratch_h;
    uint32_t pitch = (nw + 7u) & ~7u;
    uint32_t bytes = pitch * nh * 4u;          /* 32bpp */

    vram_block_t *nb = vram_alloc(bytes, VRAM_ALIGN);
    if (!nb) return false;

    if (g_x3100.scratch_block) vram_free(g_x3100.scratch_block);
    g_x3100.scratch_block       = nb;
    g_x3100.scratch_vram_offset = nb->offset;
    g_x3100.scratch_w           = nw;
    g_x3100.scratch_h           = nh;
    g_x3100.scratch_pitch_words = pitch;
    return true;
}

/* Release the dirty-rect scratch back to the pool and reset its size so a
 * later compose re-grows it on demand.  Called when the display source (the
 * compositor) detaches: the scratch grows monotonically while a compositor
 * lives (freeing per-frame would thrash the allocator), so this is the point
 * to hand its up-to-full-screen block back. */
static void scratch_release(void)
{
    if (g_x3100.scratch_block) vram_free(g_x3100.scratch_block);
    g_x3100.scratch_block       = NULL;
    g_x3100.scratch_vram_offset = 0u;
    g_x3100.scratch_w           = 0u;
    g_x3100.scratch_h           = 0u;
    g_x3100.scratch_pitch_words = 0u;
}

/* Build a transient surface_t describing the VISIBLE front page. Used by
 * dv_compose_rect, which patches a small finished rectangle straight into
 * the page currently being scanned out (the last fully-composed frame, so
 * it is coherent everywhere outside the rect). In single-buffer fallback
 * front_offset == back_offset == 0, so this equals primary_surface(). */
static surface_t front_surface(void)
{
    surface_t s = primary_surface();
    s.vram_offset = g_x3100.front_offset;   /* the page the CRTC shows */
    return s;
}

int32_t dv_compose_rect(uint32_t display_id, int32_t rx, int32_t ry,
                        uint32_t rw, uint32_t rh, dv_fence_t fence_signal)
{
    if (display_id != 0) return DV_ERR_INVAL;
    if (rw == 0u || rh == 0u) return DV_OK;

    /* True dirty-rectangle path (ported from the Mach64 reference): compose
     * ONLY the changed region off-screen into a scratch buffer, then land it on
     * the VISIBLE front page with ONE vblank-aligned blit.  No page-flip and no
     * full recompose — every pixel outside the rect is already correct in the
     * front page (it is the last fully-composed frame), so we patch it in place.
     *
     * Why a scratch and not the i965 hardware scissor (the SC scissor
     * registers, or XY_SETUP_CLIP_BLT): the engine scissor would clip only
     * the hardware BLTs, but glyphs and
     * alpha-over run through the CPU path on this silicon and ignore it — they
     * would redraw the whole layer every call.  Composing into a scratch sized
     * to the rect makes the destination *be* the rect, so the replay paths'
     * own clip_rect(dst->w/h) clips EVERYTHING (fills, blits, glyphs, alpha)
     * to the dirty region for free.  The only pixels the panel ever observes
     * are the single final blit, landed during blanking. */
    surface_t prim = front_surface();          /* the page the CRTC is showing */

    /* Clamp the rect to the screen. */
    int32_t x0 = rx < 0 ? 0 : rx;
    int32_t y0 = ry < 0 ? 0 : ry;
    int32_t x1 = rx + (int32_t)rw;              /* exclusive */
    int32_t y1 = ry + (int32_t)rh;
    if (x1 > (int32_t)prim.width)  x1 = (int32_t)prim.width;
    if (y1 > (int32_t)prim.height) y1 = (int32_t)prim.height;
    if (x0 >= x1 || y0 >= y1) return DV_OK;
    uint32_t cw = (uint32_t)(x1 - x0);
    uint32_t ch = (uint32_t)(y1 - y0);

    /* Gather visible layers, insertion-sort by z (low to high). */
    layer_t *vis[MAX_LAYERS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_LAYERS; i++)
        if (g_x3100.layers[i].used && g_x3100.layers[i].visible)
            vis[n++] = &g_x3100.layers[i];
    for (uint32_t i = 1; i < n; i++)
    {
        layer_t *cur = vis[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && vis[j]->z > cur->z) { vis[j + 1] = vis[j]; j--; }
        vis[j + 1] = cur;
    }

    /* Fast path: compose the region into the scratch, then one blit to front.
     * Needs the engine (for the landing blit) and a scratch allocation. */
    if (g_x3100.engine_ok && scratch_ensure(cw, ch))
    {
        surface_t scr;
        scr.used        = true;
        scr.owner       = DV_HANDLE_NONE;
        scr.format      = DV_FMT_BGRA8888;
        scr.flags       = 0;
        scr.width       = cw;
        scr.height      = ch;
        scr.pitch_words = g_x3100.scratch_pitch_words;
        scr.vram_offset = g_x3100.scratch_vram_offset;
        scr.vram_bytes  = g_x3100.scratch_pitch_words * ch * 4u;
        scr.block       = NULL;
        scr.sys_pixels  = NULL;     /* lo scratch e' VRAM: campi backing
                                     * espliciti, la struct e' su stack */

        /* Clear the scratch rect first.  The full compose can skip its clear
         * when the bottom layer covers the screen, but here the scratch block
         * is REUSED across calls and the dirty rect may sit over a non-covering
         * layer stack — so clear unconditionally to avoid blitting stale pixels
         * from a previous rect onto the front page.  One small fill; negligible
         * next to the layer replays, and FIFO-ordered before them. */
        x3100_hw_solid_fill(scr.vram_offset, scr.pitch_words,
                            0u, 0u, cw, ch, 0x00000000u);

        int32_t ox = -x0, oy = -y0;            /* translate layers into the rect */
        for (uint32_t i = 0; i < n; i++)
        {
            layer_t *L = vis[i];
            if (L->cmdlist != DV_HANDLE_NONE)
            {
                cmdlist_t *cl = cmdlist_lookup(L->cmdlist);
                if (cl) cmdlist_replay(cl, &scr, L->dst_rect.x + ox, L->dst_rect.y + oy);
                continue;
            }
            surface_t *src = surface_lookup(L->source);
            if (!src) continue;
            int32_t dx = L->dst_rect.x + ox, dy = L->dst_rect.y + oy;
            if (L->use_pixel_alpha)
            {
                if (g_x3100.engine_ok) x3100_wait_for_idle();
                replay_blit_alpha(&scr, src, L->src_rect, dx, dy, 255u, true);
            }
            else if (L->alpha == 255u)
            {
                if (surface_is_sysram(src))
                {
                    /* Sorgente in RAM di sistema: copia CPU nello
                     * scratch. Il flush conta doppio qui — il blit
                     * finale scratch->front e' del MOTORE e deve
                     * leggere pixel gia' atterrati, non posted. */
                    if (g_x3100.engine_ok) x3100_wait_for_idle();
                    sw_blit(&scr, src, L->src_rect, dx, dy);
                    surface_flush_writes(&scr);
                    continue;
                }
                uint32_t w = L->src_rect.w, h = L->src_rect.h;
                int32_t x = dx, y = dy, sx = L->src_rect.x, sy = L->src_rect.y, xc = x, yc = y;
                if (clip_rect(&x, &y, &w, &h, scr.width, scr.height))
                {
                    sx += (x - xc); sy += (y - yc);
                    x3100_hw_blit(src->vram_offset, src->pitch_words,
                                  (uint32_t)sx, (uint32_t)sy,
                                  scr.vram_offset, scr.pitch_words,
                                  (uint32_t)x, (uint32_t)y, w, h);
                }
            }
            else
            {
                if (g_x3100.engine_ok) x3100_wait_for_idle();
                replay_blit_alpha(&scr, src, L->src_rect, dx, dy, L->alpha, false);
            }
        }

        /* Region composed off-screen.  Drain so the scratch is final, wait for
         * vblank, then the single observable operation: blit the finished rect
         * onto the front page during blanking. */
        x3100_wait_for_idle();
        x3100_wait_for_vblank();
        x3100_hw_blit(scr.vram_offset, scr.pitch_words, 0u, 0u,
                      prim.vram_offset, prim.pitch_words,
                      (uint32_t)x0, (uint32_t)y0, cw, ch);
        x3100_wait_for_idle();

        fence_t *f = fence_lookup(fence_signal);
        if (f) f->current_value = f->target_value;
        return DV_OK;
    }

    /* Fallback (no scratch VRAM or CPU-only engine): full recompose into the
     * back page + page-flip.  Correct and tear-free, just not incremental —
     * this is the historical behaviour, kept only for the degraded case. */
    int32_t rc = dv_compose(display_id, DV_HANDLE_NONE);
    if (rc != DV_OK) return rc;
    return dv_page_flip(display_id, DV_FLIP_VSYNC, fence_signal);
}


/* ==========================================================================
 *  main
 * ========================================================================== */

/* ==========================================================================
 *  Control plane — mode / display / capabilities / driver info  (block #5)
 *
 *  Cold-path queries served over IPC (x3100_transport_ipc.c).  The mode
 *  list is fixed to the single panel-native 1024x768x32 for now (runtime
 *  colour-depth switching is the planned DobSettings EPS feature, block 7);
 *  mode_set accepts only that mode.  Capabilities are declared HONESTLY:
 *  what the 2D engine + this driver actually do, NOSUPPORT for the rest. */

int32_t dv_mode_list(uint32_t display_id, dv_mode_t *out, uint32_t *io_count)
{
    if (display_id != 0 || !out || !io_count) return DV_ERR_INVAL;
    if (*io_count < 1u) { *io_count = 1u; return DV_ERR_RANGE; }
    out[0].width = 1024u; out[0].height = 768u;
    out[0].refresh_hz = 60u; out[0].format = DV_FMT_BGRA8888; out[0].flags = 0u;
    *io_count = 1u;
    return DV_OK;
}

int32_t dv_mode_get_current(uint32_t display_id, dv_mode_t *out)
{
    if (display_id != 0 || !out) return DV_ERR_INVAL;
    out->width = g_x3100.mode.width; out->height = g_x3100.mode.height;
    out->refresh_hz = 60u; out->format = DV_FMT_BGRA8888; out->flags = 0u;
    return DV_OK;
}


int32_t dv_display_count(uint32_t *out)
{
    if (!out) return DV_ERR_INVAL;
    *out = 1u;
    return DV_OK;
}

int32_t dv_display_info(uint32_t display_id, dv_display_info_t *out)
{
    if (display_id != 0 || !out) return DV_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    out->display_id = 0;
    out->physical_w_mm = 286;  out->physical_h_mm = 214;   /* ~14.1" XGA panel */
    out->connected = 1u;
    out->edid_present = 0u;
    const char *nm = "LVDS-1";
    for (uint32_t i = 0; nm[i] && i < sizeof(out->name) - 1u; i++) out->name[i] = nm[i];
    return DV_OK;
}

int32_t x3100_subscribe(uint32_t port, uint32_t event_mask)
{
    /* The patched dobinterface boots assuming 1024x768 and only learns the
     * real resolution from a pushed DOBVC_EVENT_MODE_CHANGED, after which it
     * re-queries DOBVC_OP_MODE_GET (we answer 1280x800) and re-lays-out the
     * desktop.  So when a client subscribes to mode-change events, push one
     * immediately to sync it to our native panel mode.
     *
     * The reply to this subscribe call returns via the per-thread rendezvous
     * (dob_ipc_reply on sender_tid in the transport), so this async post to
     * the client's own port queues independently and is consumed by its event
     * loop — no ordering race with the reply. */
    if (event_mask & DOBVC_EVENT_MODE_CHANGED)
    {
        dob_msg_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.code = DOBVC_EVENT_MODE_CHANGED;   /* == 1, unambiguous on the GUI port */
        ev.arg0 = g_x3100.mode.width;         /* hint; client re-queries MODE_GET  */
        ev.arg1 = g_x3100.mode.height;
        dob_ipc_post(port, &ev);
    }
    return DV_OK;
}

int32_t dv_driver_info(dv_driver_info_t *out)
{
    if (!out) return DV_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    const char *nm = "mach64";
    const char *vd = "MainDOB";
    for (uint32_t i = 0; nm[i] && i < sizeof(out->name) - 1u; i++)   out->name[i]   = nm[i];
    for (uint32_t i = 0; vd[i] && i < sizeof(out->vendor) - 1u; i++) out->vendor[i] = vd[i];
    out->version_major = 0; out->version_minor = 3; out->version_patch = 0;
    out->pci_vendor_id = 0x1002;   /* ATI */
    out->pci_device_id = 0x4C4D;   /* Rage Mobility-P (MACH64LM) */
    return DV_OK;
}

/* Honest capability set for the Rage Mobility-P with this driver:
 *  - PAGE_FLIP advertised (the call succeeds as a no-op; single-buffer)
 *  - ACCELERATED_BLIT / HW_SCROLL: real 2D engine BitBlt
 *  - ALPHA_BLEND: supported at the protocol level (constant/pixel alpha is
 *    served, in software for the rare window case; glyphs are hardware
 *    mono-expansion).  Declared so clients use blit_alpha rather than
 *    pre-flattening.
 *  - VSYNC: vblank-aligned compose.
 *  NOT advertised: 3D / SHADER / COMPUTE / VRAM_MAP / HW_CURSOR / OVERLAY
 *  (those land in later blocks or never on this chip). */
int32_t dv_cap_query(uint64_t *out)
{
    if (!out) return DV_ERR_INVAL;
    /* HW_CURSOR is advertised: the i965 CRTC overlays a 64x64 ARGB sprite (B,
     * on pipe B) at scanout, so the pointer moves with a single CURBPOS write
     * — no dv_compose per mouse-move.  The sprite encoder (x3100_modeset.c)
     * copies dobinterface's BGRA bitmap verbatim into the ARGB sprite, so the
     * full transparent/black/white shape renders as authored. */
    *out = DV_CAP_PAGE_FLIP | DV_CAP_ACCELERATED_BLIT | DV_CAP_ALPHA_BLEND
         | DV_CAP_VSYNC | DV_CAP_HW_SCROLL | DV_CAP_HW_CURSOR;
    return DV_OK;
}

int32_t dv_cap_query_limit(dv_limit_t which, uint64_t *out)
{
    if (!out) return DV_ERR_INVAL;
    switch (which)
    {
        case DV_LIMIT_MAX_DISPLAYS: *out = 1u;    return DV_OK;
        case DV_LIMIT_MAX_LAYERS:   *out = MAX_LAYERS; return DV_OK;
        case DV_LIMIT_MAX_TEX_W:
        case DV_LIMIT_MAX_TEX_H:
        case DV_LIMIT_MAX_RT_W:
        case DV_LIMIT_MAX_RT_H:     *out = 8192u; return DV_OK;
        default:                    *out = 0u;    return DV_ERR_NOSUPPORT;
    }
}

int32_t dv_cap_query_format(dv_format_t fmt, uint32_t *out)
{
    if (!out) return DV_ERR_INVAL;
    if (format_bpp(fmt) == 4u) { *out = 0xFFFFFFFFu; return DV_OK; }  /* all uses */
    *out = 0u;
    return DV_OK;
}

/* ----- Hardware cursor: dv_* entry points (symmetric with BGA, which
 * returns NOSUPPORT).  These delegate to the x3100_cursor.c HW cursor. --- */
int32_t dv_cursor_set_bitmap(uint32_t display_id, const dv_cursor_desc_t *d)
{
    (void)display_id;
    return x3100_cursor_set_bitmap(d);
}
int32_t dv_cursor_set_position(uint32_t display_id, int32_t x, int32_t y)
{
    (void)display_id;
    return x3100_cursor_set_position(x, y);
}
int32_t dv_cursor_show(uint32_t display_id)  { (void)display_id; return x3100_cursor_show(); }
int32_t dv_cursor_hide(uint32_t display_id)  { (void)display_id; return x3100_cursor_hide(); }

void x3100_shutdown_for_detach(void)
{
    /* Blank the display and quiesce the engine before releasing the device
     * on hotplug detach.  Best-effort; the kernel reclaims VRAM mapping. */
    if (g_x3100.engine_ready) x3100_wait_for_idle();
    debug_print("[x3100] detach: releasing device.\n");
}


/* ==========================================================================
 *  Bootstrap — probe/init and main(), mirroring the BGA sequence exactly.
 *
 *  Ordering is the kernel contract: the "video" registry name is the
 *  single-shot readiness signal that unparks dobinterface, so it must be
 *  registered ONLY after BOTH the IPC port and the int 0x85 boomerang slot
 *  are live — otherwise an early dv_call hits an empty boomerang slot and
 *  returns DV_ERR_NOTREADY.
 * ========================================================================== */

/* Doppio frame: SCELTA dell'utente, non default. La ristrutturazione di
 * dobinterface ha eliminato i flash allo spostamento delle finestre che
 * il page-flip era nato a coprire: il doppio frame resta disponibile ma
 * come opt-in via DobSettings (video.double_buffer, BOOL, default
 * "false" = frame singolo). A "false"/assente/errore la back page non
 * viene NEMMENO allocata — su ferro con VRAM contata (Armada E500: 8 MB)
 * il frame singolo restituisce ~3 MB al pool superfici. La declare e'
 * idempotente (schema aggiornato a ogni boot, valore dell'utente
 * preservato); la lettura passa dallo stub con riconnessione bounded
 * (2 s max se settingsd non risponde), poi si va comunque avanti col
 * default: il driver video non resta mai in ostaggio del daemon. */
static bool x3100_double_buffer_opted_in(void)
{
    declareSetting("video.double_buffer", SETTING_BOOL,
                   "Doppio frame (page-flip anti-flicker)", "false", 0);
    const char *v = getSetting("video.double_buffer");
    return (v != NULL) && (strcmp(v, "true") == 0);
}

static bool x3100_probe_and_init(void)
{
    /* Map BARs (MMIO/GTT/GMADR), build the GTT-backed UMA VRAM pool, and
     * bring up the render-ring memory.  Sets g_x3100.mode (1280x800 native)
     * and g_x3100.vram_bytes (16 MB pool). */
    if (!x3100_hw_init())
    {
        debug_print("[x3100] FATAL: hw_init failed.\n");
        return false;
    }

    /* Init the allocator over the whole pool, then carve the primary at
     * offset 0.  (vram_bytes was set by x3100_hw_init.) */
    vram_init_pool(0, g_x3100.vram_bytes);

    /* Page size on the PADDED scanout pitch (see scan_pitch_px). */
    g_x3100.primary_bytes  = g_x3100.scan_pitch_px * g_x3100.mode.height * 4u;
    {
        /* 4096: DSPSURF latches bits 31:12 — the page base MUST be 4 KB
         * aligned. The old VRAM_ALIGN (16) worked only because
         * 1280*800*4 happens to be a 4 KB multiple. */
        vram_block_t *pb = vram_alloc(g_x3100.primary_bytes, 4096u);
        if (!pb || pb->offset != 0u)
        {
            debug_print("[x3100] FATAL: primary not at VRAM offset 0.\n");
            return false;
        }
        g_x3100.primary_offset = pb->offset;   /* 0 */
    }

    /* Allocate the back page for real double-buffering. The primary (front)
     * is at offset 0; the back page is a second full-frame buffer. Compose
     * targets the back page off-screen, then dv_page_flip swaps the CRTC
     * scanout offset at vblank. If this allocation fails we fall back to
     * single-buffer (front == back), which is the historical behaviour —
     * correct, just not flicker-free. */
    g_x3100.front_offset    = g_x3100.primary_offset;   /* 0 = visible at boot */
    g_x3100.back_offset     = g_x3100.primary_offset;   /* default: no back page */
    g_x3100.double_buffered = false;
    if (x3100_double_buffer_opted_in())
    {
        vram_block_t *bb = vram_alloc(g_x3100.primary_bytes, 4096u); /* DSPSURF: 4 KB */
        if (bb)
        {
            g_x3100.back_offset     = bb->offset;
            g_x3100.double_buffered = true;
            /* nota: i tag di questi debug erano rimasti "[mach64]" — 
             * fossile del clone da cui questo driver e' nato. */
            debug_print("[x3100] double-buffer ON (opt-in, real page-flip)\n");
        }
        else
        {
            debug_print("[x3100] opt-in ma back page non allocabile -> "
                        "frame singolo\n");
        }
    }
    else
    {
        debug_print("[x3100] frame singolo (video.double_buffer=false)\n");
    }

    /* Shadow frame per il frame singolo (vedi x3100_state.h): senza,
     * la compose piena scrive nel VISIBILE strato per strato — prima
     * il backbuf (desktop sopra le finestre), poi le finestre una a
     * una: le "tempeste di flash" viste sul Mach64/E500, stesso
     * meccanismo qui. Con la shadow la compose baka in RAM di sistema
     * e dv_page_flip presenta pixel finali in una sola passata
     * sequenziale. Costa ~3 MB di RAM di sistema, ZERO VRAM.
     * A doppio frame attivo non serve (il page-flip vero fa gia' da
     * schermo); su malloc fallita si degrada alla compose diretta. */
    g_x3100.shadow = NULL;
    if (!g_x3100.double_buffered)
    {
        g_x3100.shadow = (uint8_t *)malloc(g_x3100.primary_bytes);
        if (g_x3100.shadow != NULL)
            memset(g_x3100.shadow, 0, g_x3100.primary_bytes);
        else
            debug_print("[x3100] shadow malloc fallita: compose diretta\n");
    }
    /* Damage tracking a zero: primo frame a danno pieno, niente da
     * presentare finche' una compose non produce danno. */
    g_x3100.surface_gen_seed = 0u;
    g_x3100.composed_valid   = false;
    for (uint32_t ci = 0; ci < MAX_LAYERS; ci++)
        g_x3100.composed[ci].used = false;
    g_x3100.present_x0 = g_x3100.present_y0 = 0;
    g_x3100.present_x1 = g_x3100.present_y1 = 0;

    /* Clear the visible framebuffer FIRST, via the CPU, before touching
     * the 2D engine.  The framebuffer is directly mapped (g_x3100.vram),
     * so this cannot fail regardless of engine state — it guarantees a
     * clean screen even if the 2D engine never comes up, and overwrites
     * any leftover VRAM from a previous boot.  ~3 MB of writes, one-time.
     * Clear the back page too (when double-buffered): after the first flip
     * the CRTC scans it out, so it must not show stale VRAM for one frame. */
    {
        uint32_t px = g_x3100.scan_pitch_px * g_x3100.mode.height;
        volatile uint32_t *fb = (volatile uint32_t *)g_x3100.vram;
        for (uint32_t i = 0; i < px; i++) fb[i] = 0x00000000u;
        if (g_x3100.double_buffered)
        {
            volatile uint32_t *bb =
                (volatile uint32_t *)((volatile uint8_t *)g_x3100.vram
                                      + g_x3100.back_offset);
            for (uint32_t i = 0; i < px; i++) bb[i] = 0x00000000u;
        }
    }

    /* Arm the easy-path scanout: switch plane B onto our (now-cleared) front
     * page and turn off the legacy VGA plane.  The panel shows black instead
     * of BIOS VGA text from here on; reboot reverts it. */
    x3100_modeset();

    /* Bring up the 2D engine for 32bpp at the primary pitch.  This is a
     * best-effort accelerator init: if the engine doesn't quiesce (the
     * register sequence is not yet validated on this exact silicon), we
     * log and continue rather than aborting the whole driver — the
     * display is already live and "video" must still be registered so
     * dobinterface can start.  Blits will simply be unavailable until the
     * engine path is fixed. */
    if (x3100_engine_reset() != 0)
        debug_print("[x3100] WARN: 2D engine reset timed out; "
                    "continuing without acceleration.\n");
    x3100_engine_setup_for_format(0, g_x3100.mode.width);  /* arg ignored on X3100 */
    g_x3100.engine_ready = true;

    /* Hardware 2D engine validated on this silicon (solid_fill, blit, and
     * mono-blit all confirmed on-screen).  Render the desktop via the
     * accelerator instead of the CPU fallback. */
    g_x3100.engine_ok = true;

    /* ---- 2D ENGINE VISUAL TEST (boot, pre-compositor) ------------------
     * The enginediag readback is UNRELIABLE per PRG 5.2.1.3 (aperture read of
     * a region the engine just wrote "may or may not" return the drawn
     * pixel).  So confirm the engine VISUALLY: the scanout reads physical
     * VRAM directly, with none of that caveat.  Here, before the compositor
     * starts and while the screen is black, fill the WHOLE primary via the
     * HARDWARE blitter with a bright colour and hold it.
     *
     *   If the screen turns solid MAGENTA for ~4 s -> the 2D fill WORKS on
     *     hardware (the earlier "NO" verdicts were false negatives from the
     *     aperture-readback caveat).  Next step: set engine_ok = true.
     *   If the screen stays BLACK -> the engine truly is not writing, and
     *     CONFIG_CNTL / the aperture state (now in the diag) is the lead.
     *
     * Set X3100_ENGINE_VISUAL_TEST to 0 to skip once decided. */
#define X3100_ENGINE_VISUAL_TEST 0
#if X3100_ENGINE_VISUAL_TEST
    {
        uint32_t pitch = g_x3100.scan_pitch_px;
        g_x3100.engine_ok = true;                /* force the hardware path */

        /* (1) SOLID FILL: full screen magenta (validated). */
        x3100_hw_solid_fill(0, pitch, 0, 0,
                             g_x3100.mode.width, g_x3100.mode.height,
                             0x00FF00FFu);
        x3100_wait_for_idle();

        /* (2) draw a distinct CYAN source block at top-left, then BLIT
         * (copy) it to the right.  If the hardware blit works you see two
         * cyan blocks: the original at (50,50) and a copy at (400,50). */
        x3100_hw_solid_fill(0, pitch, 50, 50, 200, 150, 0x0000FFFFu); /* cyan src */
        x3100_wait_for_idle();
        x3100_hw_blit(0, pitch, 50, 50,          /* src x,y */
                       0, pitch, 400, 50,          /* dst x,y */
                       200, 150);                  /* w,h */
        x3100_wait_for_idle();

        /* (3) MONO-BLIT orientation probe: an 'F' is asymmetric on BOTH axes,
         * so it reveals horizontal AND vertical flips unambiguously.  Packed
         * continuously, LSB-first, like the glyph atlas.  Drawn at (50,300). */
        {
            uint32_t mask_off = g_x3100.scan_pitch_px * g_x3100.mode.height * 4u;
            volatile uint8_t *m = (volatile uint8_t *)g_x3100.vram + mask_off;
            /* 12x8 'F' (bit c counted left->right via 0x800>>c):
             * row0 full top bar, row1 left, row2 left+mid bar, rows3-7 left. */
            static const uint16_t Frow[8] = {
                0xFFFu, 0x800u, 0xFC0u, 0x800u, 0x800u, 0x800u, 0x800u, 0x000u
            };
            uint32_t glyph_bytes = (12u * 8u + 7u) / 8u;   /* 12 bytes */
            for (uint32_t b = 0; b < glyph_bytes; b++) m[b] = 0u;
            uint32_t bit = 0;
            for (uint32_t r = 0; r < 8u; r++)
                for (uint32_t c = 0; c < 12u; c++, bit++)
                    if (Frow[r] & (0x800u >> c))
                        m[bit >> 3] |= (uint8_t)(0x80u >> (bit & 7u));  /* MSB-first */
            /* mono blit no longer self-drains; make the underlying fills land
             * before the CPU writes the glyph on top. */
            x3100_wait_for_idle();
            x3100_hw_mono_blit(mask_off, glyph_bytes, 0, pitch, 50, 300, 12, 8, 0x00FFFF00u);
            x3100_wait_for_idle();
        }

        /* Leave engine_ok = TRUE: after this hold, dobinterface composes on
         * hardware.  Watch the transition magenta/cyan/yellow -> desktop. */
        sleep_ms(4000);                            /* hold so it's observable */
    }
#endif
    return true;
}

/* Forward decls: live in x3100_transport_ipc.c / x3100_fast_entry.asm. */
extern int  x3100_transport_ipc_run(uint32_t port);
extern void x3100_fast_entry(void);

int main(void)
{
    set_priority(1);

    dob_server_init("DobVideoControl");
    uint32_t port = dob_server_get_port();

    /* Do NOT register "video" yet — see ordering note above. */

    dob_registry_wait("hotplug", 5000);
    if (!dob_driver_attach(&g_x3100.dev))
    {
        debug_print("[x3100] FATAL: hotplug attach failed.\n");
        _exit(1);
    }

    debug_print("[x3100] MainDOB Mach64 video driver v0.3 starting...\n");
    pci_enable_bus_master(&g_x3100.dev);

    if (!x3100_probe_and_init()) _exit(1);

    /* Install the int 0x85 fast path (data plane). */
    static __attribute__((aligned(16))) uint8_t x3100_dispatch_stack[16384];
    static __attribute__((aligned(16))) uint8_t x3100_payload_buf[16384];

    if (syscall3(SYS_REGISTER_VIDEO_DRIVER,
                 (int)(uintptr_t)x3100_fast_entry,
                 (int)(uintptr_t)(x3100_dispatch_stack + sizeof x3100_dispatch_stack),
                 (int)(uintptr_t)x3100_payload_buf) == 0)
        debug_print("[x3100] int 0x85 fast path registered.\n");
    else
        debug_print("[x3100] WARN: int 0x85 fast path registration failed.\n");

    /* Both IPC port and boomerang slot are up -> announce readiness. */
    dob_registry_register("video", port);
    debug_print("[x3100] Ready.\n");

    /* Serve the control plane; the data plane runs via the boomerang. */
    return x3100_transport_ipc_run(port);
}
