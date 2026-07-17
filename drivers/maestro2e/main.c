/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * MainDOB ESS Maestro-2E (ES1978) Audio Driver
 *
 * PCI driver for the ESS Maestro-2E, the onboard audio of the Compaq
 * Armada E500. Multi-channel PCM mixing exposed via the DobAudio IPC
 * service ("audio").
 *
 * The hardware layer (register map, Wave Processor / APU / wavecache
 * programming, the "Bob" timer, codec access) is a transliteration of
 * the Linux `sound/pci/es1968.c` driver
 *   Copyright (c) Matze Braun, Takashi Iwai, Zach Brown, Alan Cox.
 * Because that driver is GPL-2.0-or-later and this hardware layer is
 * derived from it, THIS FILE is likewise GPL-2.0-or-later. The MainDOB
 * integration layer (IPC, mixer, ring, widget) is original.
 *
 * Playback model:
 *   - One physically contiguous DMA ring (S16LE interleaved stereo).
 *   - Two APUs play it: APU0 = left lane, APU1 = right lane. The
 *     wavecache "stereo" bit interleaves the fetch; the right APU's
 *     sample address carries bit 23.
 *   - The APUs loop the ring forever (loop length = whole buffer).
 *   - The Maestro "Bob" timer raises the PCI IRQ ~200x/s; each IRQ we
 *     read APU0's current address, refill the consumed span, ack.
 *   - The codec is a standard AC97 codec reached through the Maestro's
 *     index/data port pair.
 *
 * STATUS — playback NOT yet working on real hardware (Compaq Armada
 * E500). The mixer/codec register path works (volume widget responds);
 * the playback datapath does not yet produce sound. Confirmed-correct
 * fixes applied here vs es1968.c (sound/pci/es1968.c, mainline):
 *   - PCMBAR page registers 0x01FC-0x01FF programmed (was missing).
 *   - Config A / Config B written as one dword via SYS_PCI_WRITE (the
 *     16-bit write was zeroing Config B).
 *   - wav_shift = 2 for 16-bit stereo: APU loop length and play-pointer
 *     in frames (was using the mono shift).
 *   - IRQ handler reads host-IRQ status 0x1A and acks like es1968.
 * Playback uses two APUs in 16BITLINEAR (verified — the wavecache
 * stereo bit + bit-23 address do the L/R split). An interim revision
 * wrongly used 16BITSTEREO; that has been reverted.
 *
 * KNOWN INCOMPLETE — see maestro_chip_setup(): the RING BUS (ports
 * 0x34-0x3A) that carries WP/APU output to the codec DAC, and the IDR
 * 0x08-0x0D serial-DAC setup, are NOT configured. This is the leading
 * suspect for the remaining silence (a chip whose mixer answers but
 * whose audio link is down). Those values must come from the real
 * es1968 snd_es1968_chip_init, not be guessed.
 *
 * Diagnostics (this machine has NO serial console): the mixer widget
 * shows a status line — "APU OK/FROZEN  m=<master> pd=<powerdown>" —
 * updated when playback starts. Read it off the laptop screen:
 *   "APU OK"     => DMA/wavecache/APU run; fault is downstream (ring
 *                  bus to codec, or speaker amp / EAPD).
 *   "APU FROZEN" => fault is upstream (DMA addr / wavecache / APU).
 *   pd bit 15 (EAPD) reflects the speaker-amp power state.
 * If the line never leaves "audio: idle", no client is sending audio.
 * (debug_print serial lines are still emitted, for the rare case a
 * serial cable is attached.)
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

/* ==========================================================================
 *  Maestro-2E hardware definitions  (verified vs es1968.c)
 * ==========================================================================
 */

#define ESS_VENDOR_ID            0x125D
#define ESS_DEV_MAESTRO_2E       0x1978
#define ESS_DEV_MAESTRO_2        0x1968

/* BAR0 I/O register offsets */
#define ESM_DATA                 0x00   /* maestro indirect data    */
#define ESM_INDEX                0x02   /* maestro indirect index   */
#define ESM_WP_IRQ_CLEAR         0x04   /* write 1 = clear WP int   */
#define WC_INDEX                 0x10   /* wavecache index          */
#define WC_DATA                  0x12   /* wavecache data           */
#define WC_CONTROL               0x14   /* wavecache control        */
#define ESM_PORT_HOST_IRQ        0x18   /* host IRQ control/status  */
#define ESM_AC97_INDEX           0x30   /* AC97 codec reg index     */
#define ESM_AC97_DATA            0x32   /* AC97 codec reg data      */

/* Maestro indirect register file (IDRn, selected via ESM_INDEX) */
#define IDR0_DATA_PORT           0x00
#define IDR1_CRAM_POINTER        0x01
#define IDR2_CRAM_DATA           0x02
#define IDR3_WAVE_DATA           0x03
#define IDR4_WAVE_PTR_LOW        0x04
#define IDR5_WAVE_PTR_HI         0x05
#define IDR6_TIMER_CTRL          0x06
#define IDR7_WAVE_ROMRAM         0x07
#define IDR_BOB_ENABLE_REG       0x11   /* Bob timer enable bit */
#define IDR_BOB_START_REG        0x17   /* Bob timer start bit  */

/* PCI config-space registers */
#define ESM_PCI_LEGACY_AUDIO     0x40
#define ESM_PCI_CONFIG_A         0x50
#define ESM_PCI_CONFIG_B         0x52
#define ESM_PCI_ACPI_COMMAND     0x54
#define ESM_PCI_DDMA             0x60

/* Config A bits (es1968: Values for the ESM_CONFIG_A) */
#define CFGA_PIC_SNOOP1          0x4000
#define CFGA_PIC_SNOOP2          0x2000
#define CFGA_DMA_CLEAR           0x0700
#define CFGA_SAFEGUARD           0x0800
#define CFGA_POST_WRITE          0x0080
#define CFGA_PCI_TIMING          0x0040
#define CFGA_SWAP_LR             0x0020
#define CFGA_SUBTR_DECODE        0x0002

/* Config B bits (es1968: Values for the ESM_CONFIG_B) */
#define CFGB_SPDIF               0x0100
#define CFGB_HWV                 0x0080
#define CFGB_DEBOUNCE            0x0040
#define CFGB_GPIO                0x0020
#define CFGB_CHI                 0x0010   /* disconnect CHI — fixes some laptops */
#define CFGB_IDMA                0x0008   /* undoc */
#define CFGB_MIDI_FIX            0x0004   /* undoc */
#define CFGB_IRQ_TO_ISA          0x0001   /* undoc */

/* Legacy audio control (PCI 0x40): bit 15 = master disable */
#define ESS_DISABLE_AUDIO        0x8000

/* Host IRQ status port (read; es1968 interrupt handler reads 0x1A) */
#define ESM_PORT_HOST_IRQ_STAT   0x1A

/* Wavecache PCMBAR page registers. The per-APU wavecache entry encodes
 * only address bits 3..15; the rest of the 28-bit physical base comes
 * from these four page registers (bank-selected by sample-address bits
 * 22/23 — which is why all four are programmed identically). */
#define WC_PCMBAR_PAGE           0x01FC   /* ..0x01FF */

/* Host IRQ control bits (ESM_PORT_HOST_IRQ) */
#define ESM_RESET_MAESTRO        0x8000
#define ESM_RESET_DIRECTSOUND    0x4000
#define ESM_HIRQ_DSIE            0x0004   /* DirectSound/WP IRQ enable */

/* Bob timer */
#define ESM_BOB_ENABLE           0x0001
#define ESM_BOB_START            0x0001
#define ESS_SYSCLK               50000000
#define ESM_BOB_FREQ             200      /* timer ticks/second */

/* APU mode (reg 0, bits 4-7) */
#define ESM_APU_MODE_SHIFT       4
#define ESM_APU_OFF              0x00
#define ESM_APU_16BITLINEAR      0x01   /* 16-bit mono player (measure_clock) */
#define ESM_APU_16BITSTEREO      0x02   /* 16-bit paired stereo player */

/* APU reg-0 base: DMA enable + filter; mode OR'd in at trigger.
 * Verified — es1968_measure_clock writes exactly 0x400F. */
#define APU_REG0_BASE            0x400F

#define APU_LEFT                 0
#define APU_RIGHT                1

#define MAESTRO_CLOCK            48000    /* es1968 default reference clock */

/* AC97 codec register numbers — standard AC97 */
#define AC97_RESET               0x00
#define AC97_MASTER_VOL          0x02
#define AC97_HEADPHONE_VOL       0x04   /* not present on all codecs */
#define AC97_MASTER_MONO_VOL     0x06
#define AC97_PC_BEEP_VOL         0x0A
#define AC97_PCM_OUT_VOL         0x18
#define AC97_EXT_AUDIO_CTRL      0x2A
#define AC97_PCM_DAC_RATE        0x2C
#define AC97_POWERDOWN           0x26   /* Powerdown ctrl/stat; bit15 = EAPD */
#define AC97_EAPD                0x8000 /* External Amplifier Power Down */
#define AC97_VENDOR_ID1          0x7C
#define AC97_EXT_VRA             0x0001

/* ==========================================================================
 *  Driver constants  (hardware-agnostic)
 * ==========================================================================
 */

#define RING_FRAMES          8192               /* ~170 ms @ 48k */
#define RING_SAMPLES         (RING_FRAMES * 2)
#define RING_BYTES           (RING_SAMPLES * 2)
#define RING_FRAME_MASK      (RING_FRAMES - 1)

#define DEFAULT_RING_SAMPLES 32768
#define DEFAULT_RING_MASK    (DEFAULT_RING_SAMPLES - 1)

#define MIX_CHUNK_SAMPLES    2048

#define CODEC_READY_TIMEOUT_MS  500
#define CODEC_POLL_INTERVAL_MS  2
#define AC97_BUSY_TIMEOUT       100000
#define APU_POLL_TIMEOUT        1000

#define SILENCE_IRQS_TO_STOP    16   /* ~80 ms grace at 200 Hz */

#define GUI_EVT_WIDGET_CLICK    220

/* --- Volume Mixer widget layout --- */
#define MXW_W               220
#define MXW_H               264   /* +36 over channels for the diag line */
#define MXW_PAD               8
#define MXW_SL_X             14
#define MXW_SL_W            130
#define MXW_HEADER_Y          8
#define MXW_MASTER_LBL_Y     28
#define MXW_MASTER_SL_Y      48
#define MXW_SEP_Y            78
#define MXW_CH_BASE_Y        90
#define MXW_CH_ROW_H         46
#define MXW_CH_LBL_TO_SL     22
#define MXW_MAX_CHANS         3
#define MXW_DIAG_Y          236   /* below the 3 channel rows (90+3*46) */

#define MXW_COL_BG          0x001A1A2E
#define MXW_COL_HEADER_FG   0x00E0E0FF
#define MXW_COL_LABEL       0x00AAAACC
#define MXW_COL_MASTER      0x000077DD
#define MXW_COL_CHAN        0x000066AA

/* Client Channel */

typedef struct channel
{
    uint32_t        id;
    uint32_t        owner_pid;
    uint8_t         volume;
    bool            paused;
    bool            closing;
    uint32_t        sample_rate;
    uint8_t         bits;
    uint8_t         num_channels;

    int16_t        *ring;
    uint32_t        ring_size;
    uint32_t        ring_mask;
    uint32_t        w_pos;
    uint32_t        r_pos;

    uint32_t        notify_port;
    uint32_t        low_watermark;
    bool            notify_armed;

    struct channel *next;
} channel_t;

/* ==========================================================================
 *  Driver State
 * ==========================================================================
 */

static uint16_t io_base   = 0;        /* BAR0 — single I/O window */
static uint8_t  esm_irq   = 0;
static uint32_t my_port   = 0;
static uint8_t  pci_bus, pci_slot, pci_func;

static int16_t *dma_ring      = NULL;
static uint32_t dma_ring_phys = 0;
static bool     hw_running    = false;
static uint32_t last_play_frame   = 0;
static uint8_t  silence_irq_count = 0;
static uint16_t apu_base[2] = { 0, 0 };

static uint8_t   master_volume = 80;
static channel_t *chan_head = NULL;
static uint32_t   next_chan_id = 1;
static uint32_t   current_rate = 48000;

static uint32_t widget_id = 0;
static dobui_widget_fb_ctx_t wctx;

/* On-screen diagnostics (this machine has no serial console).
 * g_diag is rendered as a label in the mixer widget; the probe in
 * out_start and the codec readback fill it so the fault can be read
 * directly off the laptop screen. */
static char     g_diag[64] = "audio: idle";
static uint16_t g_cdc_master = 0xFFFF;
static uint16_t g_cdc_pdown  = 0xFFFF;
static dob_label_t     mxw_lbl_header;
static dob_label_t     mxw_lbl_master;
static dob_slider_t    mxw_sl_master;
static dob_separator_t mxw_sep;
static dob_label_t     mxw_lbl_chan[MXW_MAX_CHANS];
static dob_slider_t    mxw_sl_chan[MXW_MAX_CHANS];
static uint32_t        mxw_chan_slot_id[MXW_MAX_CHANS];
static dob_label_t     mxw_lbl_diag;

static void widget_draw(void);
static void set_output_rate(uint32_t rate);

/* ==========================================================================
 *  Maestro Low-Level I/O  (verified vs es1968.c)
 * ==========================================================================
 */

/* Maestro indirect registers: index -> ESM_INDEX, data <-> ESM_DATA. */
static void
maestro_write(uint8_t reg, uint16_t val)
{
    io_outw(io_base + ESM_INDEX, reg);
    io_outw(io_base + ESM_DATA,  val);
}

static uint16_t
maestro_read(uint8_t reg)
{
    io_outw(io_base + ESM_INDEX, reg);
    return io_inw(io_base + ESM_DATA);
}

/* APU CRAM access. CRAM pointer = (channel << 4) | reg, via IDR1; data
 * transfers through IDR0. es1968 polls for the pointer/data to read back
 * — the indirect bus is racy without it. */
static void
apu_index_set(uint16_t index)
{
    maestro_write(IDR1_CRAM_POINTER, index);
    for (int i = 0; i < APU_POLL_TIMEOUT; i++)
        if (maestro_read(IDR1_CRAM_POINTER) == index)
            return;
}

static void
apu_data_set(uint16_t data)
{
    for (int i = 0; i < APU_POLL_TIMEOUT; i++)
    {
        if (maestro_read(IDR0_DATA_PORT) == data)
            return;
        maestro_write(IDR0_DATA_PORT, data);
    }
}

static void
apu_set(uint8_t apu, uint8_t reg, uint16_t data)
{
    apu_index_set((uint16_t)((apu << 4) | (reg & 0x0F)));
    apu_data_set(data);
}

static uint16_t
apu_get(uint8_t apu, uint8_t reg)
{
    apu_index_set((uint16_t)((apu << 4) | (reg & 0x0F)));
    return maestro_read(IDR0_DATA_PORT);
}

/* Wavecache has its own dedicated index/data ports (NOT the IDR file). */
static void
wave_set(uint16_t reg, uint16_t val)
{
    io_outw(io_base + WC_INDEX, reg);
    io_outw(io_base + WC_DATA,  val);
}

/* --- AC97 codec, through the Maestro index/data pair --- */

static void
codec_wait_idle(void)
{
    for (int i = 0; i < AC97_BUSY_TIMEOUT; i++)
        if (!(io_inb(io_base + ESM_AC97_INDEX) & 1))
            return;
    debug_print("[maestro] codec bus stayed busy.\n");
}

static void
codec_write(uint8_t reg, uint16_t val)
{
    codec_wait_idle();
    io_outw(io_base + ESM_AC97_DATA, val);
    io_outb(io_base + ESM_AC97_INDEX, reg & 0x7F);
}

static uint16_t
codec_read(uint8_t reg)
{
    codec_wait_idle();
    io_outb(io_base + ESM_AC97_INDEX, (reg & 0x7F) | 0x80);
    codec_wait_idle();
    return io_inw(io_base + ESM_AC97_DATA);
}

/* ==========================================================================
 *  Bob Timer  (verified — es1968 snd_es1968_bob_start/stop)
 * ==========================================================================
 */

static void
bob_stop(void)
{
    uint16_t r;
    r = maestro_read(IDR_BOB_ENABLE_REG);
    maestro_write(IDR_BOB_ENABLE_REG, (uint16_t)(r & ~ESM_BOB_ENABLE));
    r = maestro_read(IDR_BOB_START_REG);
    maestro_write(IDR_BOB_START_REG, (uint16_t)(r & ~ESM_BOB_START));
}

/* Program the Bob timer for `bob_freq` ticks/second. The prescale/divide
 * search is lifted verbatim from es1968 snd_es1968_bob_start. */
static void
bob_start(int bob_freq)
{
    int prescale;
    int divide;

    for (prescale = 5; prescale < 12; prescale++)
        if (bob_freq > (ESS_SYSCLK >> (prescale + 9)))
            break;

    divide = 1;
    while ((prescale > 5) && (divide < 32))
    {
        prescale--;
        divide <<= 1;
    }
    divide >>= 1;

    for (; divide < 31; divide++)
        if (bob_freq > ((ESS_SYSCLK >> (prescale + 9)) / (divide + 1)))
            break;

    if (divide == 0)
    {
        divide++;
        if (prescale > 5) prescale--;
    }
    else if (divide > 1)
    {
        divide--;
    }

    maestro_write(IDR6_TIMER_CTRL,
                  (uint16_t)(0x9000 | (prescale << 5) | divide));
    maestro_write(IDR_BOB_ENABLE_REG,
                  (uint16_t)(maestro_read(IDR_BOB_ENABLE_REG) | 1));
    maestro_write(IDR_BOB_START_REG,
                  (uint16_t)(maestro_read(IDR_BOB_START_REG) | 1));
}

/* ==========================================================================
 *  Channel Helpers
 * ==========================================================================
 */

static channel_t *
chan_find(uint32_t id)
{
    for (channel_t *c = chan_head; c; c = c->next)
        if (c->id == id) return c;
    return NULL;
}

static inline uint32_t
chan_available(const channel_t *ch) { return ch->w_pos - ch->r_pos; }

static inline uint32_t
chan_free(const channel_t *ch) { return ch->ring_size - chan_available(ch); }

static channel_t *
chan_create(uint32_t owner_pid, uint32_t rate, uint8_t bits, uint8_t nch)
{
    channel_t *c = (channel_t *)malloc(sizeof(channel_t));
    if (!c) return NULL;

    c->ring = (int16_t *)malloc(DEFAULT_RING_SAMPLES * sizeof(int16_t));
    if (!c->ring) { free(c); return NULL; }

    memset(c->ring, 0, DEFAULT_RING_SAMPLES * sizeof(int16_t));
    c->id            = next_chan_id++;
    if (c->id == 0) c->id = next_chan_id++;
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

    c->next   = chan_head;
    chan_head = c;
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

/* ==========================================================================
 *  Volume
 * ==========================================================================
 */

static void
apply_master_volume(uint8_t vol)
{
    if (vol == 0)
    {
        codec_write(AC97_MASTER_VOL, 0x8000);
        return;
    }
    uint16_t atten = (uint16_t)(63 - (vol * 63 / 100));
    codec_write(AC97_MASTER_VOL, (uint16_t)((atten << 8) | atten));
}

static void
apply_pcm_volume(void)
{
    codec_write(AC97_PCM_OUT_VOL, 0x0000);   /* 0 dB — driver mixes */
}

/* ==========================================================================
 *  Software Mixer
 * ==========================================================================
 */

static int32_t mix_accum[MIX_CHUNK_SAMPLES];

static void
mixer_fill_buffer(int16_t *dest, uint32_t num_samples)
{
    memset(mix_accum, 0, num_samples * sizeof(int32_t));

    for (channel_t *ch = chan_head; ch; ch = ch->next)
    {
        if (ch->paused) continue;

        uint32_t avail   = chan_available(ch);
        uint32_t to_read = avail < num_samples ? avail : num_samples;
        if (to_read == 0) continue;

        int32_t combined = ((int32_t)ch->volume * (int32_t)master_volume * 256)
                           / 10000;

        int16_t *ring = ch->ring;
        uint32_t mask = ch->ring_mask;
        uint32_t r    = ch->r_pos;

        for (uint32_t s = 0; s < to_read; s++)
        {
            mix_accum[s] += ((int32_t)ring[r & mask] * combined) >> 8;
            r++;
        }
        ch->r_pos = r;

        if (ch->notify_armed && ch->notify_port)
        {
            if (chan_available(ch) < ch->low_watermark)
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

    for (uint32_t s = 0; s < num_samples; s++)
    {
        int32_t v = mix_accum[s];
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        dest[s] = (int16_t)v;
    }

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

/* Fill a wrapping span of the output ring [start_frame, +count). */
static void
ring_fill_span(uint32_t start_frame, uint32_t count_frames)
{
    uint32_t f = start_frame & RING_FRAME_MASK;

    while (count_frames > 0)
    {
        uint32_t to_wrap  = RING_FRAMES - f;
        uint32_t chunk_fr = count_frames < to_wrap ? count_frames : to_wrap;
        if (chunk_fr > (MIX_CHUNK_SAMPLES / 2))
            chunk_fr = MIX_CHUNK_SAMPLES / 2;

        mixer_fill_buffer(&dma_ring[f * 2], chunk_fr * 2);

        f = (f + chunk_fr) & RING_FRAME_MASK;
        count_frames -= chunk_fr;
    }
}

static bool
any_channel_has_data(void)
{
    for (channel_t *ch = chan_head; ch; ch = ch->next)
        if (!ch->paused && chan_available(ch) > 0)
            return true;
    return false;
}

/* ==========================================================================
 *  Wave Processor — APU / wavecache  (verified vs es1968 measure_clock /
 *  playback_setup / program_wavecache)
 * ==========================================================================
 */

/* Sample-rate -> APU pitch. compute_rate = (rate << 16) / clock. */
static void
apu_set_freq(uint8_t apu, uint32_t rate)
{
    /* compute_rate = (rate << 16) / clock — es1968 does this in 32 bits.
     * rate <= 48000 so (rate << 16) <= 0xBB800000, still inside uint32_t;
     * a 64-bit divide here would pull in libgcc's __udivdi3, which the
     * driver link line does not provide. Keep it strictly 32-bit. */
    uint32_t freq = ((uint32_t)rate << 16) / MAESTRO_CLOCK;
    /* es1968 apu_set_freq: reg2 keeps its low byte, takes the low 8 bits
     * of freq in the high byte plus 0x10; reg3 = freq >> 8. */
    apu_set(apu, 2, (uint16_t)((apu_get(apu, 2) & 0x00FF)
                               | ((freq & 0xFF) << 8) | 0x10));
    apu_set(apu, 3, (uint16_t)(freq >> 8));
}

/* Wavecache control word for one APU lane. Index = apu << 3. */
static void
wavecache_program_lane(uint8_t apu)
{
    uint32_t tmpval = (dma_ring_phys - 0x10) & 0xFFF8;
    tmpval |= 2;                       /* stereo (8-bit would be |4) */
    wave_set((uint16_t)(apu << 3), (uint16_t)tmpval);
}

/* Program one APU as a 16-bit-linear looping player over one lane of the
 * interleaved stereo ring. lane = 0 (left) or 1 (right).
 *
 * pa is the APU sample address: offset from the DMA base (0 — our ring
 * IS the whole DMA region), in words, with bit 22 = System RAM and, for
 * the right lane, bit 23; then >>1 because the data is 16-bit.
 * size = dma_size >> wav_shift, wav_shift = 2 (16-bit stereo, per
 * es1968 pcm_prepare). Register values match snd_es1968_playback_setup
 * for the stereo path. */
static void
apu_program_lane(uint8_t apu, int lane, uint32_t rate)
{
    uint32_t pa   = 0;                       /* ring base == DMA base */
    uint32_t size = RING_BYTES >> 2;         /* wav_shift = 2: 16-bit stereo.
                                              * es1968 pcm_prepare: shift 1,
                                              * +1 if (stereo && 16bit). The
                                              * APU counts per-lane samples,
                                              * so loop length = frames. v1
                                              * used >>1 (the mono value) and
                                              * looped past the buffer. */

    pa |= 0x00400000;                        /* System RAM (bit 22) */
    if (lane) pa |= 0x00800000;              /* right channel (bit 23) */
    pa >>= 1;                                /* 16-bit */

    apu_base[lane] = (uint16_t)(pa & 0xFFFF);

    wavecache_program_lane(apu);

    for (int i = 0; i < 16; i++)
        apu_set(apu, (uint8_t)i, 0x0000);

    apu_set(apu, 4, (uint16_t)(((pa >> 16) & 0xFF) << 8)); /* wave page hi */
    apu_set(apu, 5, (uint16_t)(pa & 0xFFFF));              /* current/start */
    apu_set(apu, 6, (uint16_t)((pa + size) & 0xFFFF));     /* loop end */
    apu_set(apu, 7, (uint16_t)size);                       /* loop length */
    apu_set(apu, 8, 0x0000);
    apu_set(apu, 9, 0xD000);                               /* amplitude */
    /* reg 10: pan / filter tuning. Left at the original 0x8F08 — pan
     * placement is NOT the silence cause, so this stays unchanged
     * until the path produces sound and we can tune the stereo image. */
    apu_set(apu, 10, 0x8F08);
    /* Routing reg: 0 — verified, es1968 playback writes exactly 0
     * ("clear routing stuff"); the L/R split is carried by bit 23 of
     * the sample address + the wavecache stereo bit. */
    apu_set(apu, 11, 0x0000);
    apu_set(apu, 0, APU_REG0_BASE);                        /* DMA on, filter */

    apu_set_freq(apu, rate);
}

/* Set APU operating mode (reg 0 bits 4-7) — also the run/halt trigger. */
static void
apu_trigger(uint8_t apu, uint8_t mode)
{
    apu_set(apu, 0,
            (uint16_t)((apu_get(apu, 0) & 0xFF0F)
                       | (mode << ESM_APU_MODE_SHIFT)));
}

/* APU0 current play position as a frame offset in [0, RING_FRAMES).
 * The APU sample address (pa) was computed as byte_offset>>2 = frame
 * index, so reg5 - base[0] is already in FRAMES — no extra shift.
 * (An earlier revision halved this, matching a different unit; it was
 * inconsistent with the >>2 addressing.) */
static uint32_t
apu_play_frame(void)
{
    uint16_t reg5   = apu_get(APU_LEFT, 5);
    uint16_t offset = (uint16_t)(reg5 - apu_base[0]);            /* frames */
    return (uint32_t)offset & RING_FRAME_MASK;
}

/* ==========================================================================
 *  Output Transport
 * ==========================================================================
 */

static void
out_stop(void)
{
    if (!hw_running) return;
    apu_trigger(APU_LEFT,  ESM_APU_OFF);
    apu_trigger(APU_RIGHT, ESM_APU_OFF);
    bob_stop();
    hw_running = false;
}

static void
out_start(void)
{
    if (hw_running) return;

    if (chan_head && chan_head->sample_rate != current_rate)
        set_output_rate(chan_head->sample_rate);

    memset(dma_ring, 0, RING_BYTES);
    ring_fill_span(0, RING_FRAMES);

    apu_program_lane(APU_LEFT,  0, current_rate);
    apu_program_lane(APU_RIGHT, 1, current_rate);

    /* es1968 pcm_start: reload reg5 = base, then trigger the mode.
     * Mode = 16BITLINEAR on BOTH lanes — verified vs es1968: stereo
     * playback uses two LINEAR players, the wavecache stereo bit +
     * bit-23 address do the L/R split. (An earlier revision of this
     * driver used 16BITSTEREO here; that was wrong.) */
    apu_set(APU_LEFT,  5, apu_base[0]);
    apu_trigger(APU_LEFT,  ESM_APU_16BITLINEAR);
    apu_set(APU_RIGHT, 5, apu_base[1]);
    apu_trigger(APU_RIGHT, ESM_APU_16BITLINEAR);

    /* Clear and enable Wave-Processor interrupts. */
    io_outw(io_base + ESM_WP_IRQ_CLEAR, 1);
    io_outw(io_base + ESM_PORT_HOST_IRQ,
            (uint16_t)(io_inw(io_base + ESM_PORT_HOST_IRQ) | ESM_HIRQ_DSIE));

    bob_start(ESM_BOB_FREQ);

    last_play_frame   = apu_play_frame();
    silence_irq_count = 0;
    hw_running        = true;

    /* DECISIVE PROBE: is the hardware actually CONSUMING the ring?
     * Read APU0's position, wait, read again. If it advanced, the
     * DMA -> wavecache -> APU path is alive and the fault is downstream
     * (ring bus to codec, or the speaker amp). If it is frozen, the
     * fault is upstream (DMA address / wavecache / APU programming) and
     * no codec-side fix will matter. This one line tells us which half
     * of the driver to stop suspecting. */
    uint16_t p0 = apu_get(APU_LEFT, 5);
    sleep_ms(20);
    uint16_t p1 = apu_get(APU_LEFT, 5);
    char dbg[96];
    snprintf(dbg, sizeof(dbg),
             "[maestro] APU pos %04x -> %04x (%s)\n",
             p0, p1,
             (p0 == p1) ? "FROZEN: digital path" : "advancing: path OK");
    debug_print(dbg);

    /* Same result, on screen (no serial on this box). Compact so it
     * fits the widget label: APU state + the two codec regs that matter.
     * pd bit15 (EAPD) = speaker-amp power on most laptops. */
    snprintf(g_diag, sizeof(g_diag),
             "APU %s  m=%04x pd=%04x",
             (p0 == p1) ? "FROZEN" : "OK", g_cdc_master, g_cdc_pdown);
    widget_draw();
}

/* ==========================================================================
 *  IRQ Handler — Bob timer tick drives every refill
 * ==========================================================================
 */

static void
handle_irq(void)
{
    /* es1968 interrupt: read the host IRQ status byte (0x1A); 0 means
     * not ours (shared PCI line). Ack mirrors es1968 exactly:
     * outw(inw(io+4) & 1, io+4). */
    uint8_t event = io_inb(io_base + ESM_PORT_HOST_IRQ_STAT);
    if (!event)
        return;

    io_outw(io_base + ESM_WP_IRQ_CLEAR,
            (uint16_t)(io_inw(io_base + ESM_WP_IRQ_CLEAR) & 1));

    if (!hw_running) return;

    bool had_data = any_channel_has_data();

    uint32_t now   = apu_play_frame();
    uint32_t delta = (now - last_play_frame) & RING_FRAME_MASK;

    if (delta > 0)
        ring_fill_span(last_play_frame, delta);
    last_play_frame = now;

    if (had_data)
        silence_irq_count = 0;
    else if (++silence_irq_count >= SILENCE_IRQS_TO_STOP)
        out_stop();
}

/* ==========================================================================
 *  Chip / Codec Initialisation
 * ==========================================================================
 */

/* PCI chip configuration — verified vs es1968 snd_es1968_chip_init.
 *
 * NOTE on width: SYS_PCI_WRITE is dword-wide. Config A (0x50) and
 * Config B (0x52) share one dword, so they are read-modified-written
 * TOGETHER — v1 wrote a 16-bit Config A value as a dword and silently
 * zeroed Config B. */
static void
maestro_pci_init(void)
{
    uint32_t ab = pci_config_read(pci_bus, pci_slot, pci_func,
                                  ESM_PCI_CONFIG_A);
    uint16_t cfga = (uint16_t)(ab & 0xFFFF);
    uint16_t cfgb = (uint16_t)(ab >> 16);

    /* Config A — es1968 order, bit for bit. v1 SET SAFEGUARD; es1968
     * clears it ("Stop Whatever Catastrophe"). */
    cfga &= (uint16_t)~CFGA_DMA_CLEAR;          /* DMA mode bits = 0    */
    cfga &= (uint16_t)~(CFGA_PIC_SNOOP1 | CFGA_PIC_SNOOP2);
    cfga &= (uint16_t)~CFGA_SAFEGUARD;
    cfga |=  CFGA_POST_WRITE;
    cfga |=  CFGA_PCI_TIMING;
    cfga &= (uint16_t)~CFGA_SWAP_LR;
    cfga &= (uint16_t)~CFGA_SUBTR_DECODE;

    /* Config B — es1968 order. CHI disconnect is the bit that fixed
     * other ESS laptops (Dell Latitude/Inspiron) in es1968's history. */
    cfgb &= (uint16_t)~(1 << 15);               /* clock multiplier off */
    cfgb &= (uint16_t)~(1 << 14);               /* external clock       */
    cfgb &= (uint16_t)~CFGB_SPDIF;
    cfgb |=  CFGB_HWV;
    cfgb |=  CFGB_DEBOUNCE;
    cfgb &= (uint16_t)~CFGB_GPIO;
    cfgb |=  CFGB_CHI;
    cfgb &= (uint16_t)~CFGB_IDMA;
    cfgb &= (uint16_t)~CFGB_MIDI_FIX;
    cfgb &= (uint16_t)~(1 << 1);                /* reserved, write 0    */
    cfgb &= (uint16_t)~CFGB_IRQ_TO_ISA;

    pci_config_write(pci_bus, pci_slot, pci_func, ESM_PCI_CONFIG_A,
                     ((uint32_t)cfgb << 16) | cfga);

    /* Legacy ISA audio: bit 15 = master disable (es1968 writes 0x8000).
     * v1 wrote 0, which clears the per-port enables but leaves the
     * legacy block powered. (Dword write also zeroes 0x42, as v1
     * already did.) */
    pci_config_write(pci_bus, pci_slot, pci_func, ESM_PCI_LEGACY_AUDIO,
                     ESS_DISABLE_AUDIO);
    pci_config_write(pci_bus, pci_slot, pci_func, ESM_PCI_DDMA, 0);
    pci_config_write(pci_bus, pci_slot, pci_func, ESM_PCI_ACPI_COMMAND, 0);
}

/* Reset pulse — verified vs es1968 snd_es1968_reset (same pulse, udelay
 * 10us there; our 10ms sleeps are a superset and harmless at boot). */
static void
maestro_reset_chip(void)
{
    io_outw(io_base + ESM_PORT_HOST_IRQ,
            ESM_RESET_MAESTRO | ESM_RESET_DIRECTSOUND);
    sleep_ms(10);
    io_outw(io_base + ESM_PORT_HOST_IRQ, 0);
    sleep_ms(10);
}

/* Post-reset chip bring-up.
 *
 * IMPORTANT — INCOMPLETE. The codec *register* path (volumes) works
 * without this, because AC97 reg access uses ESM_AC97_INDEX/DATA
 * (0x30/0x32). But PLAYBACK audio travels WP/APU -> RING BUS -> codec
 * DAC over ports 0x34-0x3A, and that link is NOT configured here yet.
 * A silent chip whose mixer still responds is the classic signature of
 * an unconfigured ring bus.
 *
 * TODO (needs the real values from es1968 snd_es1968_chip_init — the
 * same file this driver was transliterated from):
 *   - ESM_RING_BUS_DEST    (0x34)
 *   - ESM_RING_BUS_CONTR_A (0x36)
 *   - ESM_RING_BUS_CONTR_B (0x38)   // RINGB_EN_2CODEC etc.
 *   - ESM_RING_BUS_SDO     (0x3A)
 *   - the IDR 0x08-0x0D "DirectSound" / serial DAC setup block
 * I removed a from-memory guess at the 0x08-0x0D block: shipping
 * unverified writes onto the audio datapath is how you get a chip that
 * is silent in a *different* way. Paste me that part of your es1968.c
 * and I'll transliterate it exactly. */
static void
maestro_chip_setup(void)
{
    /* Wavecache scratch/record entry tables cleared (low risk). */
    for (uint16_t i = 0; i < 16; i++)
    {
        wave_set((uint16_t)(0x01E0 + i), 0x0000);
        wave_set((uint16_t)(0x01D0 + i), 0x0000);
    }

    /* Wavecache enable bit. NOTE: the exact WC_CONTROL bit pattern is
     * one of the values to confirm against es1968 chip_init. */
    uint16_t wc = io_inw(io_base + WC_CONTROL);
    wc |= 0x0100;                              /* wavecache enable */
    io_outw(io_base + WC_CONTROL, wc);

    /* Clear all APU control RAM — the BIOS POST/beep can leave stale
     * APU programs running. Safe and matches es1968 init. */
    for (uint16_t apu = 0; apu < 64; apu++)
        for (uint8_t r = 0; r < 16; r++)
            apu_set((uint8_t)apu, r, 0x0000);
}

static bool
maestro_init_hardware(hotplug_device_t *dev)
{
    io_base  = (uint16_t)(dev->bar[0] & 0xFFFC);
    pci_bus  = dev->bus;
    pci_slot = dev->slot;
    pci_func = dev->func;

    char tmp[80];
    snprintf(tmp, sizeof(tmp),
             "[maestro] %04x:%04x io=0x%x irq=%u\n",
             dev->vendor_id, dev->device_id, io_base, dev->irq_line);
    debug_print(tmp);

    pci_enable_bus_master(dev);
    maestro_pci_init();
    maestro_reset_chip();
    maestro_chip_setup();

    if (dev->irq_line == 0 || dev->irq_line >= 16)
    {
        debug_print("[maestro] FATAL: invalid IRQ line.\n");
        return false;
    }

    esm_irq = dev->irq_line;
    if (irq_register(esm_irq, my_port) == 0)
    {
        debug_print("[maestro] IRQ registered on default line.\n");
    }
    else
    {
        int free_line = irq_find_free();
        if (free_line < 0)
        {
            debug_print("[maestro] FATAL: no free IRQ lines.\n");
            return false;
        }
        if (irq_wire_device(dev->bus, dev->slot, dev->func,
                            (uint8_t)free_line) != 0)
        {
            debug_print("[maestro] FATAL: chipset rewire failed.\n");
            return false;
        }
        esm_irq = (uint8_t)free_line;
        if (irq_register(esm_irq, my_port) != 0)
        {
            debug_print("[maestro] FATAL: register on rewired line failed.\n");
            return false;
        }
        snprintf(tmp, sizeof(tmp),
                 "[maestro] IRQ %u busy, migrated to %u.\n",
                 dev->irq_line, esm_irq);
        debug_print(tmp);
    }
    return true;
}

static bool
codec_wait_ready(void)
{
    uint32_t waited = 0;
    while (waited < CODEC_READY_TIMEOUT_MS)
    {
        uint16_t r = codec_read(AC97_RESET);
        if (r != 0x0000 && r != 0xFFFF) return true;
        sleep_ms(CODEC_POLL_INTERVAL_MS);
        waited += CODEC_POLL_INTERVAL_MS;
    }
    return false;
}

static void
set_output_rate(uint32_t rate)
{
    codec_write(AC97_PCM_DAC_RATE, (uint16_t)rate);
    if (hw_running)
    {
        apu_set_freq(APU_LEFT,  rate);
        apu_set_freq(APU_RIGHT, rate);
    }
    current_rate = rate;
}

static void
codec_finish_init(void)
{
    uint16_t vid = codec_read(AC97_VENDOR_ID1);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "[maestro] AC97 codec vendor: %04x\n", vid);
    debug_print(tmp);

    codec_write(AC97_RESET, 0xFFFF);

    uint16_t ext = codec_read(AC97_EXT_AUDIO_CTRL);
    codec_write(AC97_EXT_AUDIO_CTRL, ext | AC97_EXT_VRA);
    set_output_rate(48000);

    apply_master_volume(master_volume);
    apply_pcm_volume();

    /* HYPOTHESIS (not verified on the E500, low risk): toggle off the
     * codec's External Amplifier Power Down. On laptops the internal
     * speaker amp is very often wired to EAPD (AC97 reg 0x26 bit 15),
     * and a codec that powers up with EAPD set is mute through the
     * speakers while every register still reads/writes fine — i.e.
     * exactly this symptom. Clearing bit 15 powers the amp UP on most
     * codecs (a few invert it; if this makes a hiss/pop appear, we're
     * on the right track and can refine). */
    uint16_t pd = codec_read(AC97_POWERDOWN);
    codec_write(AC97_POWERDOWN, (uint16_t)(pd & ~AC97_EAPD));

    /* Also explicitly unmute the common output taps in case the codec
     * came up muted on a path other than Master. PCM out already 0dB. */
    codec_write(AC97_MASTER_VOL,      0x0000);   /* 0dB, unmuted */
    codec_write(AC97_MASTER_MONO_VOL, 0x0000);
    codec_write(AC97_HEADPHONE_VOL,   0x0000);

    /* DIAGNOSTICS — read back what actually stuck, so the serial log
     * tells us whether the codec analog side is sane. */
    g_cdc_master = codec_read(AC97_MASTER_VOL);
    g_cdc_pdown  = codec_read(AC97_POWERDOWN);
    char dbg[96];
    snprintf(dbg, sizeof(dbg),
             "[maestro] codec: master=%04x pcm=%04x pdown=%04x ext=%04x\n",
             g_cdc_master, codec_read(AC97_PCM_OUT_VOL),
             g_cdc_pdown, codec_read(AC97_EXT_AUDIO_CTRL));
    debug_print(dbg);
}

static bool
pipeline_init(void)
{
    /* The wavecache addresses only the low 28 bits of the bus and wants
     * the whole buffer in one window — a single dma_alloc satisfies both. */
    dma_ring = (int16_t *)dma_alloc(RING_BYTES, &dma_ring_phys);
    if (!dma_ring)
    {
        debug_print("[maestro] FATAL: DMA ring alloc failed.\n");
        return false;
    }
    if (dma_ring_phys & 0xF0000000u)
        debug_print("[maestro] WARNING: DMA buffer above 28-bit range.\n");

    memset(dma_ring, 0, RING_BYTES);

    /* PCMBAR — THE missing piece in v1, and the primary cause of total
     * silence. The per-APU wavecache entry encodes only address bits
     * 3..15 of the buffer; the upper bits of the physical base live in
     * the four page registers 0x01FC-0x01FF (bank-selected by sample-
     * address bits 22/23, hence all four identical). Without them the
     * wavecache fetched "PCM" from physical page 0.
     * Verified vs es1968 snd_es1968_pcm / resume:
     *   wave_set_register(chip, 0x01FC + i, chip->dma.addr >> 12);   */
    for (uint16_t i = 0; i < 4; i++)
        wave_set((uint16_t)(WC_PCMBAR_PAGE + i),
                 (uint16_t)(dma_ring_phys >> 12));

    return true;
}

/* ==========================================================================
 *  Volume Mixer Widget
 * ==========================================================================
 */

static void
widget_draw(void)
{
    if (!widget_id) return;

    dobui_WidgetRestoreContext(&wctx);
    dobui_FillRect(widget_id, 0, 0, MXW_W, MXW_H, MXW_COL_BG);

    doblbl_Draw(&mxw_lbl_header);
    doblbl_Draw(&mxw_lbl_master);
    mxw_sl_master.value = (int)master_volume;
    dobsl_Draw(&mxw_sl_master);
    dobsep_Draw(&mxw_sep);

    bool slot_used[MXW_MAX_CHANS] = { false };

    for (int i = 0; i < MXW_MAX_CHANS; i++)
    {
        if (mxw_sl_chan[i].grabbed && mxw_chan_slot_id[i] != 0)
        {
            if (chan_find(mxw_chan_slot_id[i]))
                slot_used[i] = true;
            else
            {
                mxw_sl_chan[i].grabbed = false;
                mxw_chan_slot_id[i]    = 0;
            }
        }
    }

    for (channel_t *c = chan_head; c; c = c->next)
    {
        bool already = false;
        for (int i = 0; i < MXW_MAX_CHANS; i++)
            if (slot_used[i] && mxw_chan_slot_id[i] == c->id) { already = true; break; }
        if (already) continue;

        int free_slot = -1;
        for (int i = 0; i < MXW_MAX_CHANS; i++)
            if (!slot_used[i]) { free_slot = i; break; }
        if (free_slot < 0) break;

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
        if (!c) continue;

        char tmp[32];
        snprintf(tmp, sizeof(tmp), "Ch %u (pid %u)",
                 (unsigned)c->id, (unsigned)c->owner_pid);
        doblbl_SetText(&mxw_lbl_chan[i], tmp);
        mxw_sl_chan[i].value = (int)c->volume;

        doblbl_Draw(&mxw_lbl_chan[i]);
        dobsl_Draw(&mxw_sl_chan[i]);
    }

    /* Diagnostic line (no serial on the target hardware). */
    dobui_FillRect(widget_id, 0, MXW_DIAG_Y, MXW_W,
                   MXW_H - MXW_DIAG_Y, MXW_COL_BG);
    doblbl_SetText(&mxw_lbl_diag, g_diag);
    doblbl_Draw(&mxw_lbl_diag);

    dobui_WidgetInvalidate(widget_id);
}

static void
widget_create(void)
{
    widget_id = dobui_CreateWidget(MXW_W, MXW_H, my_port);
    if (!widget_id) return;

    dobui_WidgetSaveContext(&wctx);

    doblbl_Init(&mxw_lbl_header, widget_id, MXW_PAD, MXW_HEADER_Y,
                "Volume Mixer");
    mxw_lbl_header.col_text = MXW_COL_HEADER_FG;
    mxw_lbl_header.col_bg   = MXW_COL_BG;

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

    dobsep_Init(&mxw_sep, widget_id, MXW_PAD, MXW_SEP_Y,
                MXW_W - MXW_PAD * 2, false);

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

    doblbl_Init(&mxw_lbl_diag, widget_id, MXW_SL_X, MXW_DIAG_Y, g_diag);
    mxw_lbl_diag.col_text = MXW_COL_LABEL;
    mxw_lbl_diag.col_bg   = MXW_COL_BG;

    widget_draw();
}

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
        if (dobsl_OnClick(&mxw_sl_master, mx, my)) { widget_draw(); return; }
        for (int i = 0; i < MXW_MAX_CHANS; i++)
        {
            if (mxw_chan_slot_id[i] == 0) continue;
            if (dobsl_OnClick(&mxw_sl_chan[i], mx, my)) { widget_draw(); return; }
        }
        return;
    }

    if (etype == 6)
    {
        if (dobsl_OnDrag(&mxw_sl_master, mx, my)) { widget_draw(); return; }
        for (int i = 0; i < MXW_MAX_CHANS; i++)
        {
            if (mxw_chan_slot_id[i] == 0) continue;
            if (dobsl_OnDrag(&mxw_sl_chan[i], mx, my)) { widget_draw(); return; }
        }
        return;
    }

    if (etype == 2)
    {
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

/* ==========================================================================
 *  IPC Command Handler — DobAudio protocol
 * ==========================================================================
 */

static dob_status_t
handle_command(dob_msg_t *msg, dob_msg_t *reply)
{
    if (dob_driver_is_detach(msg))
    {
        out_stop();
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
            if (!ch) { reply->code = (uint32_t)-1; return DOB_OK; }

            if (rate != current_rate)
            {
                if (hw_running) out_stop();
                set_output_rate(rate);
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
            if (!chan_head) out_stop();
            reply->code = 0;
            widget_draw();
            return DOB_OK;
        }

        case AUDIO_CMD_CLOSE_DRAIN:
        {
            channel_t *ch = chan_find(msg->arg0);
            if (!ch) { reply->code = (uint32_t)-1; return DOB_OK; }
            ch->closing     = true;
            ch->notify_port = 0;
            if (chan_available(ch) == 0)
            {
                chan_unlink_and_free(ch->id);
                if (!chan_head) out_stop();
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
            uint32_t space      = chan_free(ch);
            if (samples_in > space) samples_in = space;

            const int16_t *src = (const int16_t *)msg->payload;
            int16_t *ring = ch->ring;
            uint32_t mask = ch->ring_mask;
            uint32_t w    = ch->w_pos;
            for (uint32_t s = 0; s < samples_in; s++)
            {
                ring[w & mask] = src[s];
                w++;
            }
            ch->w_pos = w;

            if (ch->notify_port) ch->notify_armed = true;
            if (!hw_running)
            {
                /* One-shot: confirms a client really delivered PCM and
                 * we are starting the hardware. If this never prints,
                 * the problem is upstream (no app writing audio), not
                 * in the chip programming at all. */
                static bool announced = false;
                if (!announced)
                {
                    char dbg[80];
                    snprintf(dbg, sizeof(dbg),
                             "[maestro] first WRITE: ch=%u bytes=%u, starting hw\n",
                             (unsigned)ch->id, (unsigned)msg->payload_size);
                    debug_print(dbg);
                    announced = true;
                }
                out_start();
            }

            reply->arg0 = samples_in * 2;
            reply->code = 0;
            return DOB_OK;
        }

        case AUDIO_CMD_SUBSCRIBE:
        {
            channel_t *ch = chan_find(msg->arg0);
            if (!ch) { reply->code = (uint32_t)-1; return DOB_OK; }
            ch->notify_port  = msg->arg1;
            uint32_t samples = msg->arg2 / 2;
            if (samples >= ch->ring_size) samples = ch->ring_size - 1;
            ch->low_watermark = samples;
            ch->notify_armed  = true;
            reply->code = 0;
            return DOB_OK;
        }

        case AUDIO_CMD_SET_CHAN_VOL:
        {
            channel_t *ch = chan_find(msg->arg0);
            if (!ch || msg->arg1 > 100) { reply->code = (uint32_t)-1; return DOB_OK; }
            ch->volume  = (uint8_t)msg->arg1;
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
            ch->r_pos   = ch->w_pos;
            reply->code = 0;
            return DOB_OK;
        }

        case AUDIO_CMD_PAUSE:
        {
            channel_t *ch = chan_find(msg->arg0);
            if (!ch) { reply->code = (uint32_t)-1; return DOB_OK; }
            ch->paused  = true;
            reply->code = 0;
            return DOB_OK;
        }

        case AUDIO_CMD_RESUME:
        {
            channel_t *ch = chan_find(msg->arg0);
            if (!ch) { reply->code = (uint32_t)-1; return DOB_OK; }
            ch->paused = false;
            if (!hw_running) out_start();
            reply->code = 0;
            return DOB_OK;
        }

        default:
            return DOB_ERR_INVALID;
    }
}

/* ==========================================================================
 *  Main
 * ==========================================================================
 */

int
main(void)
{
    hotplug_device_t dev;

    dob_server_init("audio");
    my_port = dob_server_get_port();

    dob_registry_wait("hotplug", 5000);

    if (!dob_driver_attach(&dev))
    {
        debug_print("[maestro] No hotplug or no device assigned.\n");
        _exit(1);
    }

    debug_print("[maestro] MainDOB ESS Maestro-2E Audio Driver starting...\n");

    if (!maestro_init_hardware(&dev))
    {
        debug_print("[maestro] Hardware init failed.\n");
        _exit(1);
    }

    if (!codec_wait_ready())
        debug_print("[maestro] Warning: AC97 codec did not answer.\n");

    codec_finish_init();

    if (!pipeline_init())
    {
        debug_print("[maestro] Pipeline init failed.\n");
        _exit(1);
    }

    widget_create();
    debug_print("[maestro] Ready (event-driven, no idle wakeups).\n");

    for (;;)
    {
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        dob_ipc_receive(my_port, &msg);

        if (msg.type == 3)
        {
            handle_irq();
            irq_done(esm_irq);
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
