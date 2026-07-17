/* MainDOB AC97 Audio Driver
 *
 * PCI driver for Intel ICH AC97. Multi-channel PCM mixing exposed
 * via IPC.
 *
 * Architecture:
 *   App → DobAudio stub → IPC → this driver → AC97 DMA → DAC → speaker
 *
 * Event-driven, no periodic timers:
 *   - BCIS IRQ refills buffers (~21ms at 48 kHz).
 *   - DMA self-stops on a BCIS with no data; next WRITE restarts it.
 *   - Clients may SUBSCRIBE to AUDIO_EVT_NEEDS_DATA; mixer sends it
 *     when a channel's ring crosses the client's low watermark.
 *   - Codec init: bounded polling with sleep_ms, one-shot at boot.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dob/server.h>
#include <dob/registry.h>
#include <dob/ipc.h>
#include <dob/types.h>
#include <dob/hotplug_driver.h>
#include <DobAudio.h>
#include <DobInterface.h>
#include <slider.h>
#include <label.h>
#include <separator.h>

/* AC97 Hardware Definitions */

/* NAM (Native Audio Mixer) — BAR0, I/O port space */
#define NAM_RESET           0x00
#define NAM_MASTER_VOL      0x02
#define NAM_PCM_OUT_VOL     0x18
#define NAM_EXT_AUDIO_CTRL  0x2A
#define NAM_PCM_DAC_RATE    0x2C
#define NAM_VENDOR_ID1      0x7C
#define NAM_VENDOR_ID2      0x7E

/* NABM (Native Audio Bus Master) — BAR1, I/O port space */
#define NABM_PCMOUT         0x10
#define PO_BDBAR            0x00
#define PO_CIV              0x04
#define PO_LVI              0x05
#define PO_SR               0x06
#define PO_CR               0x0B

/* Global registers */
#define NABM_GLOB_CTRL      0x2C
#define NABM_GLOB_STS       0x30

/* GLOB_STS bits */
#define GS_PRIMARY_RDY      (1 << 8)   /* Primary Codec Ready */

/* Status Register bits */
#define SR_LVBCI            (1 << 2)
#define SR_BCIS             (1 << 3)
#define SR_FIFOE            (1 << 4)

/* Control Register bits */
#define CR_RPBM             (1 << 0)
#define CR_RR               (1 << 1)
#define CR_LVBIE            (1 << 2)
#define CR_IOCE             (1 << 4)

/* Global Control bits */
#define GC_GIE              (1 << 0)
#define GC_COLD_RESET       (1 << 1)

/* BDL entry control word flag. Per ICH AC'97 specification, bit 15 of
 * the upper 16-bit word = IOC (Interrupt On Completion). */
#define BDL_IOC             (1 << 15)

/* Driver Constants */

#define NUM_BDL             32   /* AC97 BDL is a 32-entry ring (CIV is 5-bit) */
#define NUM_BUFS            2    /* Physical DMA buffers, cycled through BDL */
#define BDL_MASK            (NUM_BDL - 1)
#define BUF_FRAMES          1024 /* ~21ms at 48kHz */
#define BUF_SAMPLES         (BUF_FRAMES * 2)
#define BUF_BYTES           (BUF_SAMPLES * 2)

/* Per-channel ring: MUST be a power of 2 (enables mask indexing, free-running counters). */
#define DEFAULT_RING_SAMPLES 32768   /* ~341ms at 48kHz stereo, 64KB */
#define DEFAULT_RING_MASK    (DEFAULT_RING_SAMPLES - 1)

#define CODEC_READY_TIMEOUT_MS  500   /* Max wait for primary codec ready */
#define CODEC_POLL_INTERVAL_MS  2     /* Poll granularity during codec init */

#define GUI_EVT_WIDGET_CLICK    220

/*  *  Volume Mixer Widget Layout
 *
 *  Header "Volume Mixer" on top, master slider below, separator,
 *  then up to MXW_MAX_CHANS per-channel rows (label + slider).
 *  Layout composed of stateful dobUI primitives (labels, separators,
 *  sliders) rather than raw rects — the primitives already handle
 *  focus, clipping, and hit-testing, nothing to reinvent.
 */

#define MXW_W               220
#define MXW_H               228
#define MXW_PAD               8
#define MXW_SL_X             14
#define MXW_SL_W            130
#define MXW_HEADER_Y          8
#define MXW_MASTER_LBL_Y     28
#define MXW_MASTER_SL_Y      48
#define MXW_SEP_Y            78
#define MXW_CH_BASE_Y        90
#define MXW_CH_ROW_H         46    /* label + slider + breathing room */
#define MXW_CH_LBL_TO_SL     22    /* offset from row label_y to slider_y */
#define MXW_MAX_CHANS         3    /* visible channel rows */

#define MXW_COL_BG          0x001A1A2E
#define MXW_COL_HEADER_FG   0x00E0E0FF
#define MXW_COL_LABEL       0x00AAAACC
#define MXW_COL_MASTER      0x000077DD
#define MXW_COL_CHAN        0x000066AA

/* BDL Entry (8 bytes, hardware format) */

typedef struct
{
    uint32_t addr;
    uint16_t samples;
    uint16_t flags;
} __attribute__((packed)) ac97_bdl_t;

/* Client Channel — dynamically allocated, linked list */

typedef struct channel
{
    uint32_t        id;
    uint32_t        owner_pid;
    uint8_t         volume;           /* 0..100 */
    bool            paused;
    bool            closing;          /* destroy when ring drains to 0 */
    uint32_t        sample_rate;
    uint8_t         bits;
    uint8_t         num_channels;

    int16_t        *ring;             /* malloc'd, size = ring_size samples */
    uint32_t        ring_size;        /* power of 2 */
    uint32_t        ring_mask;        /* ring_size - 1 */
    uint32_t        w_pos;            /* free-running 32-bit sample index */
    uint32_t        r_pos;            /* free-running 32-bit sample index */

    /* Opt-in low-watermark notification */
    uint32_t        notify_port;      /* 0 = no subscriber */
    uint32_t        low_watermark;    /* in samples */
    bool            notify_armed;     /* set on WRITE, cleared on notify */

    struct channel *next;
} channel_t;

/* Driver State */

/* Hardware */
static uint16_t nam_base  = 0;
static uint16_t nabm_base = 0;
static uint8_t  ac97_irq  = 0;
static uint32_t my_port   = 0;

/* DMA pipeline */
static ac97_bdl_t *bdl       = NULL;
static uint32_t    bdl_phys  = 0;
static int16_t    *dma_buf[NUM_BUFS];
static uint32_t    dma_buf_phys[NUM_BUFS];
static bool        hw_running = false;
static uint8_t     last_civ   = 0;
static uint8_t     silence_irq_count = 0;

/* Grace period: how many consecutive IRQs with an empty ring before we
 * halt DMA. At ~21ms per IRQ, 4 gives ~84ms of slack — enough for a
 * player to recover from disk I/O lag without start/stop cycling. */
#define SILENCE_IRQS_TO_STOP  4

/* Mixer */
static uint8_t     master_volume = 80;
static channel_t  *chan_head = NULL;
static uint32_t    next_chan_id = 1;
static uint32_t    current_rate = 48000;

/* Widget */
static uint32_t widget_id = 0;
static dobui_widget_fb_ctx_t wctx;

static dob_label_t     mxw_lbl_header;
static dob_label_t     mxw_lbl_master;
static dob_slider_t    mxw_sl_master;
static dob_separator_t mxw_sep;

static dob_label_t     mxw_lbl_chan[MXW_MAX_CHANS];
static dob_slider_t    mxw_sl_chan[MXW_MAX_CHANS];
/* Slot → channel id mapping. 0 = slot unused / channel absent. */
static uint32_t        mxw_chan_slot_id[MXW_MAX_CHANS];

/* Forward declaration: widget redraws on channel list changes */
static void widget_draw(void);

/* Forward declarations */
static void set_dac_rate(uint32_t rate);

/* Channel Helpers — free-running counters, no modulo */

static channel_t *
chan_find(uint32_t id)
{
    for (channel_t *c = chan_head; c; c = c->next)
        if (c->id == id) return c;
    return NULL;
}

static inline uint32_t
chan_available(const channel_t *ch)
{
    return ch->w_pos - ch->r_pos;   /* unsigned wrap is correct */
}

static inline uint32_t
chan_free(const channel_t *ch)
{
    return ch->ring_size - chan_available(ch);
}

static channel_t *
chan_create(uint32_t owner_pid, uint32_t rate, uint8_t bits, uint8_t nch)
{
    channel_t *c = (channel_t *)malloc(sizeof(channel_t));
    if (!c) return NULL;

    c->ring = (int16_t *)malloc(DEFAULT_RING_SAMPLES * sizeof(int16_t));
    if (!c->ring)
    {
        free(c);
        return NULL;
    }

    memset(c->ring, 0, DEFAULT_RING_SAMPLES * sizeof(int16_t));
    c->id            = next_chan_id++;
    if (c->id == 0) c->id = next_chan_id++;   /* skip id 0 on wrap */
    c->owner_pid     = owner_pid;
    c->volume        = 100;
    c->paused        = false;
    c->closing       = false;
    c->sample_rate   = rate;
    c->bits          = bits;
    c->num_channels  = nch;
    c->ring_size     = DEFAULT_RING_SAMPLES;
    c->ring_mask     = DEFAULT_RING_MASK;
    c->w_pos         = 0;
    c->r_pos         = 0;
    c->notify_port   = 0;
    c->low_watermark = 0;
    c->notify_armed  = false;

    c->next    = chan_head;
    chan_head  = c;
    return c;
}

static void
chan_unlink_and_free(uint32_t id)
{
    channel_t **pp = &chan_head;
    while (*pp)
    {
        if ((*pp)->id == id)
        {
            channel_t *victim = *pp;
            *pp = victim->next;
            free(victim->ring);
            free(victim);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* NAM / NABM I/O Helpers */

static uint16_t nam_read16(uint16_t reg)  { return io_inw(nam_base + reg); }
static void nam_write16(uint16_t reg, uint16_t val) { io_outw(nam_base + reg, val); }

static uint8_t  nabm_read8(uint16_t reg)  { return io_inb(nabm_base + reg); }
static uint16_t nabm_read16(uint16_t reg) { return io_inw(nabm_base + reg); }
static uint32_t nabm_read32(uint16_t reg) { return io_inl(nabm_base + reg); }
static void nabm_write8(uint16_t reg, uint8_t val)   { io_outb(nabm_base + reg, val); }
static void nabm_write16(uint16_t reg, uint16_t val) { io_outw(nabm_base + reg, val); }
static void nabm_write32(uint16_t reg, uint32_t val) { io_outl(nabm_base + reg, val); }

/* Volume */

static void
apply_master_volume(uint8_t vol)
{
    if (vol == 0)
    {
        nam_write16(NAM_MASTER_VOL, 0x8000);
        return;
    }
    uint16_t atten = (uint16_t)(63 - (vol * 63 / 100));
    nam_write16(NAM_MASTER_VOL, (atten << 8) | atten);
}

static void
apply_pcm_volume(void)
{
    nam_write16(NAM_PCM_OUT_VOL, 0x0000);
}

/* Mixer — fast path, no division, no modulo in inner loop */

static int32_t mix_accum[BUF_SAMPLES];

static void
mixer_fill_buffer(int16_t *dest, uint32_t num_samples)
{
    memset(mix_accum, 0, num_samples * sizeof(int32_t));

    for (channel_t *ch = chan_head; ch; ch = ch->next)
    {
        if (ch->paused) continue;

        uint32_t avail = chan_available(ch);
        uint32_t to_read = avail < num_samples ? avail : num_samples;
        if (to_read == 0) continue;

        /* Fold per-channel volume + master volume into a single 8.8 fixed-point
         * scalar. Applied via >>8 shift in the inner loop — no division. */
        int32_t combined = ((int32_t)ch->volume * (int32_t)master_volume * 256) / 10000;

        int16_t *ring = ch->ring;
        uint32_t mask = ch->ring_mask;
        uint32_t r = ch->r_pos;

        for (uint32_t s = 0; s < to_read; s++)
        {
            mix_accum[s] += ((int32_t)ring[r & mask] * combined) >> 8;
            r++;
        }
        ch->r_pos = r;

        /* Low-watermark notification: one-shot until re-armed by WRITE */
        if (ch->notify_armed && ch->notify_port)
        {
            uint32_t after = chan_available(ch);
            if (after < ch->low_watermark)
            {
                dob_msg_t sig;
                memset(&sig, 0, sizeof(sig));
                sig.code = AUDIO_EVT_NEEDS_DATA;
                sig.arg0 = ch->id;
                sig.arg1 = chan_free(ch) * sizeof(int16_t);
                if (dob_ipc_post(ch->notify_port, &sig) == DOB_OK)
                    ch->notify_armed = false;
            }
        }
    }

    /* Clip and write out */
    for (uint32_t s = 0; s < num_samples; s++)
    {
        int32_t v = mix_accum[s];
        if (v > 32767)  v = 32767;
        if (v < -32768) v = -32768;
        dest[s] = (int16_t)v;
    }

    /* Sweep out drained closing channels */
    channel_t **pp = &chan_head;
    while (*pp)
    {
        channel_t *c = *pp;
        if (c->closing && chan_available(c) == 0)
        {
            *pp = c->next;
            free(c->ring);
            free(c);
        }
        else
        {
            pp = &c->next;
        }
    }
}

/* DMA Control */

static void
dma_stop(void)
{
    if (!hw_running) return;
    nabm_write8(NABM_PCMOUT + PO_CR, 0);
    hw_running = false;
}

static void
dma_start(void)
{
    if (hw_running) return;

    /* Program sample rate from the first active channel */
    if (chan_head && chan_head->sample_rate != current_rate)
        set_dac_rate(chan_head->sample_rate);

    mixer_fill_buffer(dma_buf[0], BUF_SAMPLES);
    mixer_fill_buffer(dma_buf[1], BUF_SAMPLES);

    nabm_write8(NABM_PCMOUT + PO_CR, CR_RR);
    (void)nabm_read8(NABM_PCMOUT + PO_CR);

    nabm_write32(NABM_PCMOUT + PO_BDBAR, bdl_phys);
    nabm_write8(NABM_PCMOUT + PO_LVI, NUM_BDL - 1);
    nabm_write8(NABM_PCMOUT + PO_CR, CR_RPBM | CR_IOCE | CR_LVBIE);

    last_civ = 0;
    silence_irq_count = 0;
    hw_running = true;
}

static bool
any_channel_has_data(void)
{
    for (channel_t *ch = chan_head; ch; ch = ch->next)
        if (!ch->paused && chan_available(ch) > 0)
            return true;
    return false;
}

/* IRQ Handler — the only thing that drives refills */

static void
handle_irq(void)
{
    uint16_t sr = nabm_read16(NABM_PCMOUT + PO_SR);
    nabm_write16(NABM_PCMOUT + PO_SR, sr & (SR_LVBCI | SR_BCIS | SR_FIFOE));

    if (!(sr & SR_BCIS)) return;
    if (!hw_running) return;

    /* Refill whatever the chip has consumed since we last looked. When
     * channels are empty, mixer_fill_buffer writes silence — we keep the
     * DMA flowing so we never get into start/stop cycles on transient
     * player lag (e.g. disk I/O). A grace counter halts DMA only after
     * a sustained run of empty IRQs. */
    bool had_data = any_channel_has_data();

    uint8_t civ = nabm_read8(NABM_PCMOUT + PO_CIV);
    uint8_t advanced = (uint8_t)((civ - last_civ) & BDL_MASK);
    uint8_t refills  = advanced > NUM_BUFS ? NUM_BUFS : advanced;

    for (uint8_t i = 0; i < refills; i++)
    {
        uint8_t slot = (uint8_t)((last_civ + i) & BDL_MASK);
        mixer_fill_buffer(dma_buf[slot & (NUM_BUFS - 1)], BUF_SAMPLES);
    }
    last_civ = civ;
    nabm_write8(NABM_PCMOUT + PO_LVI, (uint8_t)((civ - 1) & BDL_MASK));

    if (had_data)
        silence_irq_count = 0;
    else if (++silence_irq_count >= SILENCE_IRQS_TO_STOP)
        dma_stop();
}

/* Codec Init */

static bool
ac97_init_hardware(hotplug_device_t *dev)
{
    nam_base  = (uint16_t)(dev->bar[0] & 0xFFFC);
    nabm_base = (uint16_t)(dev->bar[1] & 0xFFFC);

    char tmp[64];
    snprintf(tmp, sizeof(tmp), "[audio] NAM: 0x%x, NABM: 0x%x\n", nam_base, nabm_base);
    debug_print(tmp);

    pci_enable_bus_master(dev);

    if (dev->irq_line == 0 || dev->irq_line >= 16)
    {
        debug_print("[audio] FATAL: invalid IRQ line.\n");
        return false;
    }

    /* Try the BIOS-assigned line first (polite: maybe we're lucky). */
    ac97_irq = dev->irq_line;
    if (irq_register(ac97_irq, my_port) == 0)
    {
        debug_print("[audio] IRQ registered on default line.\n");
    }
    else
    {
        /* Collision with another driver. Ask the kernel for a free line,
         * rewire the chipset so AC97 fires on it, then register. */
        int free_line = irq_find_free();
        if (free_line < 0)
        {
            debug_print("[audio] FATAL: no free IRQ lines available.\n");
            return false;
        }

        if (irq_wire_device(dev->bus, dev->slot, dev->func, (uint8_t)free_line) != 0)
        {
            debug_print("[audio] FATAL: chipset rewire failed.\n");
            return false;
        }

        ac97_irq = (uint8_t)free_line;
        if (irq_register(ac97_irq, my_port) != 0)
        {
            debug_print("[audio] FATAL: register on rewired line failed.\n");
            return false;
        }

        char tmp2[80];
        snprintf(tmp2, sizeof(tmp2), "[audio] IRQ %u busy, migrated to %u.\n",
                 dev->irq_line, ac97_irq);
        debug_print(tmp2);
    }

    /* Cold reset */
    nabm_write32(NABM_GLOB_CTRL, GC_COLD_RESET | GC_GIE);
    return true;
}

static bool
codec_wait_ready(void)
{
    /* Bounded polling — one-shot at boot, not in hot path. sleep_ms yields
     * to the scheduler, so other processes can run while we wait. */
    uint32_t waited = 0;
    while (waited < CODEC_READY_TIMEOUT_MS)
    {
        uint32_t sts = nabm_read32(NABM_GLOB_STS);
        if (sts & GS_PRIMARY_RDY) return true;
        sleep_ms(CODEC_POLL_INTERVAL_MS);
        waited += CODEC_POLL_INTERVAL_MS;
    }
    return false;
}

static void
set_dac_rate(uint32_t rate)
{
    nam_write16(NAM_PCM_DAC_RATE, (uint16_t)rate);
    current_rate = rate;
}

static void
ac97_finish_init(void)
{
    uint16_t vid1 = nam_read16(NAM_VENDOR_ID1);
    uint16_t vid2 = nam_read16(NAM_VENDOR_ID2);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "[audio] Codec: %04x:%04x\n", vid1, vid2);
    debug_print(tmp);

    nam_write16(NAM_RESET, 0xFFFF);

    /* Enable Variable Rate Audio */
    uint16_t ext_ctrl = nam_read16(NAM_EXT_AUDIO_CTRL);
    nam_write16(NAM_EXT_AUDIO_CTRL, ext_ctrl | 0x0001);
    set_dac_rate(48000);

    apply_master_volume(master_volume);
    apply_pcm_volume();
}

/* DMA Pipeline Initialization */

static bool
pipeline_init(void)
{
    bdl = (ac97_bdl_t *)dma_alloc(NUM_BDL * sizeof(ac97_bdl_t), &bdl_phys);
    if (!bdl) return false;

    for (int i = 0; i < NUM_BUFS; i++)
    {
        dma_buf[i] = (int16_t *)dma_alloc(BUF_BYTES, &dma_buf_phys[i]);
        if (!dma_buf[i]) return false;
        memset(dma_buf[i], 0, BUF_BYTES);
    }

    for (int i = 0; i < NUM_BDL; i++)
    {
        bdl[i].addr    = dma_buf_phys[i & (NUM_BUFS - 1)];
        bdl[i].samples = BUF_SAMPLES;
        bdl[i].flags   = BDL_IOC;
    }

    return true;
}

/*  *  Widget: Volume Mixer
 *
 *  Master volume on top plus up to MXW_MAX_CHANS per-channel sliders,
 *  each bound to a live audio channel. Slots are (re)bound to channels
 *  on every draw, so opening or closing a channel reshuffles the visible
 *  rows automatically.
 *
 *  Input: dobinterface delivers only click events to widgets, not drag
 *  or release. We commit each slider hit with an inline OnClick/OnRelease
 *  pair — the slider template is built for drag interaction and only
 *  writes `value` on release, so the pair gives "click = jump to position"
 *  semantics without any new dobinterface plumbing.
 */

static void
widget_draw(void)
{
    if (!widget_id) return;

    dobui_WidgetRestoreContext(&wctx);

    /* Background */
    dobui_FillRect(widget_id, 0, 0, MXW_W, MXW_H, MXW_COL_BG);

    /* Header + master */
    doblbl_Draw(&mxw_lbl_header);
    doblbl_Draw(&mxw_lbl_master);
    mxw_sl_master.value = (int)master_volume;
    dobsl_Draw(&mxw_sl_master);

    dobsep_Draw(&mxw_sep);

    /* Slot binding — sticky.
     *
     *  We must NOT walk chan_head in order and reassign slot 0..N every frame:
     *  if a channel closed while the user was dragging a different channel's
     *  slider, the slider could shift slots mid-drag or the slot under the
     *  cursor could be wiped by the unused-clear pass.
     *
     *  Pass 1: pin any slot whose slider is currently grabbed to its bound
     *          channel id, provided the channel still exists. If the bound
     *          channel disappeared, release the grab.
     *  Pass 2: walk chan_head and fill the remaining free slots with
     *          channels that aren't already pinned.
     *  Pass 3: for each slot, either draw its bound channel or clear the
     *          row area. Pinned slots are drawn first via their existing
     *          mxw_chan_slot_id[] entries.
     */
    bool slot_used[MXW_MAX_CHANS] = { false };

    for (int i = 0; i < MXW_MAX_CHANS; i++)
    {
        if (mxw_sl_chan[i].grabbed && mxw_chan_slot_id[i] != 0)
        {
            if (chan_find(mxw_chan_slot_id[i]))
            {
                slot_used[i] = true;
            }
            else
            {
                mxw_sl_chan[i].grabbed = false;
                mxw_chan_slot_id[i]    = 0;
            }
        }
    }

    for (channel_t *c = chan_head; c; c = c->next)
    {
        bool already_bound = false;
        for (int i = 0; i < MXW_MAX_CHANS; i++)
        {
            if (slot_used[i] && mxw_chan_slot_id[i] == c->id)
            {
                already_bound = true;
                break;
            }
        }
        if (already_bound) continue;

        int free_slot = -1;
        for (int i = 0; i < MXW_MAX_CHANS; i++)
        {
            if (!slot_used[i]) { free_slot = i; break; }
        }
        if (free_slot < 0) break;   /* widget full — extra channels hidden */

        mxw_chan_slot_id[free_slot] = c->id;
        slot_used[free_slot]        = true;
    }

    for (int i = 0; i < MXW_MAX_CHANS; i++)
    {
        int row_y = MXW_CH_BASE_Y + i * MXW_CH_ROW_H;

        if (!slot_used[i])
        {
            mxw_chan_slot_id[i] = 0;
            dobui_FillRect(widget_id, 0, row_y, MXW_W, MXW_CH_ROW_H, MXW_COL_BG);
            continue;
        }

        channel_t *c = chan_find(mxw_chan_slot_id[i]);
        if (!c) continue;   /* shouldn't happen after pass 1 but be safe */

        char tmp[32];
        snprintf(tmp, sizeof(tmp), "Ch %u (pid %u)",
                 (unsigned)c->id, (unsigned)c->owner_pid);
        doblbl_SetText(&mxw_lbl_chan[i], tmp);
        mxw_sl_chan[i].value = (int)c->volume;

        doblbl_Draw(&mxw_lbl_chan[i]);
        dobsl_Draw(&mxw_sl_chan[i]);
    }

    dobui_WidgetInvalidate(widget_id);
}

static void
widget_create(void)
{
    widget_id = dobui_CreateWidget(MXW_W, MXW_H, my_port);
    if (!widget_id) return;

    dobui_WidgetSaveContext(&wctx);

    /* Header label */
    doblbl_Init(&mxw_lbl_header, widget_id, MXW_PAD, MXW_HEADER_Y,
                "Volume Mixer");
    mxw_lbl_header.col_text = MXW_COL_HEADER_FG;
    mxw_lbl_header.col_bg   = MXW_COL_BG;

    /* Master row */
    doblbl_Init(&mxw_lbl_master, widget_id, MXW_SL_X, MXW_MASTER_LBL_Y,
                "Master");
    mxw_lbl_master.col_text = MXW_COL_LABEL;
    mxw_lbl_master.col_bg   = MXW_COL_BG;

    dobsl_Init(&mxw_sl_master, widget_id,
               MXW_SL_X, MXW_MASTER_SL_Y, MXW_SL_W, 0);
    mxw_sl_master.min         = 0;
    mxw_sl_master.max         = 100;
    mxw_sl_master.value       = (int)master_volume;
    mxw_sl_master.col_fill    = MXW_COL_MASTER;
    mxw_sl_master.col_text    = MXW_COL_HEADER_FG;
    mxw_sl_master.col_text_bg = MXW_COL_BG;
    mxw_sl_master.show_value  = true;

    /* Separator between master and channel list */
    dobsep_Init(&mxw_sep, widget_id, MXW_PAD, MXW_SEP_Y,
                MXW_W - MXW_PAD * 2, false);

    /* Channel rows: pre-initialised with empty text, filled by draw */
    for (int i = 0; i < MXW_MAX_CHANS; i++)
    {
        int lbl_y = MXW_CH_BASE_Y + i * MXW_CH_ROW_H;
        int sl_y  = lbl_y + MXW_CH_LBL_TO_SL;

        doblbl_Init(&mxw_lbl_chan[i], widget_id, MXW_SL_X, lbl_y, "");
        mxw_lbl_chan[i].col_text = MXW_COL_LABEL;
        mxw_lbl_chan[i].col_bg   = MXW_COL_BG;

        dobsl_Init(&mxw_sl_chan[i], widget_id,
                   MXW_SL_X, sl_y, MXW_SL_W, 0);
        mxw_sl_chan[i].min         = 0;
        mxw_sl_chan[i].max         = 100;
        mxw_sl_chan[i].value       = 100;
        mxw_sl_chan[i].col_fill    = MXW_COL_CHAN;
        mxw_sl_chan[i].col_text    = MXW_COL_HEADER_FG;
        mxw_sl_chan[i].col_text_bg = MXW_COL_BG;
        mxw_sl_chan[i].show_value  = true;

        mxw_chan_slot_id[i] = 0;
    }

    widget_draw();
}

/* Dispatch a widget mouse event by etype (arg3).
 *
 *   1 = click   → OnClick on the slider under (x,y); start grab
 *   6 = drag    → OnDrag on the grabbed slider; updates drag_value live
 *   2 = release → OnRelease on the grabbed slider; commits drag_value
 *                 to the matching audio channel's volume (or master).
 *
 * During drag we redraw to show the updated drag_value in the slider
 * label — this is "il segreto del 335": the slider's Draw uses
 * `grabbed ? drag_value : value`, so continuous drag events translate
 * into continuous live-updated labels. Audio is NOT actually affected
 * until release: the intermediate drag_value isn't committed, so the
 * mixer keeps using the previous volume until the user lets go. */
static void
handle_widget_event(dob_msg_t *msg)
{
    if (msg->arg0 != widget_id) return;

    int mx = (int)msg->arg1;
    int my = (int)msg->arg2;
    uint32_t etype = msg->arg3;

    dobui_WidgetRestoreContext(&wctx);

    if (etype == 1)
    {
        /* Click — let the slider under the cursor grab. */
        if (dobsl_OnClick(&mxw_sl_master, mx, my))
        {
            widget_draw();
            return;
        }
        for (int i = 0; i < MXW_MAX_CHANS; i++)
        {
            if (mxw_chan_slot_id[i] == 0) continue;
            if (dobsl_OnClick(&mxw_sl_chan[i], mx, my))
            {
                widget_draw();
                return;
            }
        }
        return;
    }

    if (etype == 6)
    {
        /* Drag — forward to every slider; only the grabbed one responds. */
        if (dobsl_OnDrag(&mxw_sl_master, mx, my))
        {
            widget_draw();
            return;
        }
        for (int i = 0; i < MXW_MAX_CHANS; i++)
        {
            if (mxw_chan_slot_id[i] == 0) continue;
            if (dobsl_OnDrag(&mxw_sl_chan[i], mx, my))
            {
                widget_draw();
                return;
            }
        }
        return;
    }

    if (etype == 2)
    {
        /* Release — commit the grabbed slider's drag_value to the real
         * audio state. Check grabbed *before* OnRelease because release
         * clears the flag. */
        if (mxw_sl_master.grabbed)
        {
            dobsl_OnRelease(&mxw_sl_master);
            master_volume = (uint8_t)mxw_sl_master.value;
            apply_master_volume(master_volume);
            widget_draw();
            return;
        }
        for (int i = 0; i < MXW_MAX_CHANS; i++)
        {
            if (!mxw_sl_chan[i].grabbed) continue;
            dobsl_OnRelease(&mxw_sl_chan[i]);
            channel_t *c = chan_find(mxw_chan_slot_id[i]);
            if (c) c->volume = (uint8_t)mxw_sl_chan[i].value;
            widget_draw();
            return;
        }
        return;
    }
}

/* IPC Command Handler */

static dob_status_t
handle_command(dob_msg_t *msg, dob_msg_t *reply)
{
    if (dob_driver_is_detach(msg))
    {
        dma_stop();
        dob_driver_released();
        _exit(0);
    }

    switch (msg->code)
    {
        case AUDIO_CMD_OPEN:
        {
            uint32_t rate = msg->arg0;
            uint8_t  bits = (uint8_t)msg->arg1;
            uint8_t  nch  = (uint8_t)msg->arg2;

            if (bits != 16 || nch != 2 || rate < 8000 || rate > 48000)
            {
                reply->code = (uint32_t)-1;
                return DOB_OK;
            }

            channel_t *ch = chan_create(msg->sender_pid, rate, bits, nch);
            if (!ch)
            {
                reply->code = (uint32_t)-1;
                return DOB_OK;
            }

            if (rate != current_rate)
            {
                if (hw_running) dma_stop();
                set_dac_rate(rate);
            }

            reply->arg0 = ch->id;
            reply->code = 0;
            widget_draw();
            return DOB_OK;
        }

        case AUDIO_CMD_CLOSE:
        {
            channel_t *ch = chan_find(msg->arg0);
            if (!ch) { reply->code = (uint32_t)-1; return DOB_OK; }
            chan_unlink_and_free(ch->id);
            if (!chan_head) dma_stop();
            reply->code = 0;
            widget_draw();
            return DOB_OK;
        }

        case AUDIO_CMD_CLOSE_DRAIN:
        {
            channel_t *ch = chan_find(msg->arg0);
            if (!ch) { reply->code = (uint32_t)-1; return DOB_OK; }
            ch->closing = true;
            ch->notify_port = 0;   /* stop notifications */
            /* If already drained, free now */
            if (chan_available(ch) == 0)
            {
                chan_unlink_and_free(ch->id);
                if (!chan_head) dma_stop();
                widget_draw();
            }
            reply->code = 0;
            return DOB_OK;
        }

        case AUDIO_CMD_WRITE:
        {
            channel_t *ch = chan_find(msg->arg0);
            if (!ch || ch->closing || !msg->payload || msg->payload_size == 0)
            {
                reply->code = (uint32_t)-1;
                return DOB_OK;
            }

            uint32_t samples_in = msg->payload_size / 2;
            uint32_t space = chan_free(ch);
            if (samples_in > space) samples_in = space;

            const int16_t *src = (const int16_t *)msg->payload;
            int16_t *ring = ch->ring;
            uint32_t mask = ch->ring_mask;
            uint32_t w = ch->w_pos;
            for (uint32_t s = 0; s < samples_in; s++)
            {
                ring[w & mask] = src[s];
                w++;
            }
            ch->w_pos = w;

            /* Re-arm the watermark trigger. Always set on successful write;
             * the mixer will fire again immediately if we're still below
             * watermark (self-limiting at IRQ rate, ~50Hz worst case). */
            if (ch->notify_port) ch->notify_armed = true;

            if (!hw_running) dma_start();

            reply->arg0 = samples_in * 2;
            reply->code = 0;
            return DOB_OK;
        }

        case AUDIO_CMD_SUBSCRIBE:
        {
            channel_t *ch = chan_find(msg->arg0);
            if (!ch) { reply->code = (uint32_t)-1; return DOB_OK; }
            ch->notify_port   = msg->arg1;
            uint32_t samples  = msg->arg2 / 2;
            if (samples >= ch->ring_size) samples = ch->ring_size - 1;
            ch->low_watermark = samples;
            ch->notify_armed  = true;
            reply->code = 0;
            return DOB_OK;
        }

        case AUDIO_CMD_SET_CHAN_VOL:
        {
            channel_t *ch = chan_find(msg->arg0);
            if (!ch || msg->arg1 > 100)
            {
                reply->code = (uint32_t)-1;
                return DOB_OK;
            }
            ch->volume = (uint8_t)msg->arg1;
            reply->code = 0;
            return DOB_OK;
        }

        case AUDIO_CMD_SET_MASTER_VOL:
        {
            if (msg->arg0 > 100) { reply->code = (uint32_t)-1; return DOB_OK; }
            master_volume = (uint8_t)msg->arg0;
            apply_master_volume(master_volume);
            if (widget_id) widget_draw();
            reply->code = 0;
            return DOB_OK;
        }

        case AUDIO_CMD_GET_INFO:
        {
            reply->arg0 = master_volume;
            uint32_t count = 0;
            for (channel_t *c = chan_head; c; c = c->next) count++;
            reply->arg1 = count;
            reply->code = 0;
            return DOB_OK;
        }

        case AUDIO_CMD_FLUSH:
        {
            channel_t *ch = chan_find(msg->arg0);
            if (!ch) { reply->code = (uint32_t)-1; return DOB_OK; }
            ch->r_pos = ch->w_pos;
            reply->code = 0;
            return DOB_OK;
        }

        case AUDIO_CMD_PAUSE:
        {
            channel_t *ch = chan_find(msg->arg0);
            if (!ch) { reply->code = (uint32_t)-1; return DOB_OK; }
            ch->paused = true;
            reply->code = 0;
            return DOB_OK;
        }

        case AUDIO_CMD_RESUME:
        {
            channel_t *ch = chan_find(msg->arg0);
            if (!ch) { reply->code = (uint32_t)-1; return DOB_OK; }
            ch->paused = false;
            if (!hw_running) dma_start();
            reply->code = 0;
            return DOB_OK;
        }

        default:
            return DOB_ERR_INVALID;
    }
}

/* Main */

int
main(void)
{
    hotplug_device_t dev;

    dob_server_init("audio");
    my_port = dob_server_get_port();

    dob_registry_wait("hotplug", 5000);

    if (!dob_driver_attach(&dev))
    {
        debug_print("[audio] No hotplug or no device assigned.\n");
        _exit(1);
    }

    debug_print("[audio] MainDOB AC97 Audio Driver starting...\n");

    if (!ac97_init_hardware(&dev))
    {
        debug_print("[audio] Hardware init failed.\n");
        _exit(1);
    }

    /* Wait for codec ready — bounded polling, scheduler-yielding. */
    if (!codec_wait_ready())
        debug_print("[audio] Warning: codec ready bit never set, continuing.\n");

    ac97_finish_init();

    if (!pipeline_init())
    {
        debug_print("[audio] Pipeline init failed.\n");
        _exit(1);
    }

    widget_create();
    debug_print("[audio] Ready (event-driven, no idle wakeups).\n");

    /* Event loop — blocks in dob_ipc_receive. Wakes only on:
     *   - IRQ BCIS (buffer completion)
     *   - Client IPC call (OPEN, WRITE, ...)
     *   - Widget click (master volume slider)
     * Zero CPU cost when no audio is playing. */
    for (;;)
    {
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        dob_ipc_receive(my_port, &msg);

        if (msg.type == 3)
        {
            handle_irq();
            irq_done(ac97_irq);
            continue;
        }

        if (msg.type == 1)
        {
            dob_msg_t reply;
            memset(&reply, 0, sizeof(reply));
            reply.code = (uint32_t)handle_command(&msg, &reply);
            dob_ipc_reply(msg.sender_tid, &reply);
            continue;
        }

        if (msg.type == 4 && msg.code == GUI_EVT_WIDGET_CLICK)
        {
            handle_widget_event(&msg);
            continue;
        }
    }

    return 0;
}
