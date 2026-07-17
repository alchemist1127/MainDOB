/* decode_mp2.c — MPEG-1 Audio Layer II decoder.
 *
 * Scope corrente: MPEG-1 Layer II @ 44.1/48 kHz, bitrate 32-192 kbps/ch,
 *                 mode stereo/dual/mono (joint decoded as stereo).
 *                 Table A (sblimit=27).
 *
 * Pipeline (decode_frame_full):
 *   1. load_frame         — sync + header + lettura frame dal file
 *   2. decode_allocation  — bit allocation per subband/canale
 *   3. decode_scfsi       — scalefactor selection info
 *   4. decode_scalefactors
 *   5. decode_samples     — 12 triplet × sblimit × nch, requantize
 *   6. synthesize         — 36 time-step × nch via synthesis_filter
 */

#include "decode_mp2.h"
#include "synthesis_filter.h"
#include <DobFileSystem.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Constanti */

#define MAX_FRAME_SIZE  2048
#define PCM_PER_FRAME   1152
#define MP2_SBLIMIT_A   27

/* Tabelle ISO/IEC 11172-3 */

/* Bitrate Layer II MPEG-1 (kbps). Index 0 = free format (unsupported). */
static const uint16_t bitrate_table[15] =
{
    0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384
};

/* Sampling rate (Hz) */
static const uint32_t sample_rate_table[4] = { 44100, 48000, 32000, 0 };

/* Classe di quantizzazione — Table B.4 */
typedef struct
{
    uint16_t nlevels;
    uint8_t  group;       /* 1 se 3 campioni codificati in una parola */
    uint8_t  bits;        /* bit per parola (grouped) o per campione */
    float    C;
    float    D;
} quant_class_t;

static const quant_class_t quant_class[17] =
{
    {    3, 1,  5, 1.33333333f, 0.50000000f},
    {    5, 1,  7, 1.60000000f, 0.50000000f},
    {    7, 0,  3, 1.14285714f, 0.25000000f},
    {    9, 1, 10, 1.77777778f, 0.50000000f},
    {   15, 0,  4, 1.06666667f, 0.12500000f},
    {   31, 0,  5, 1.03225806f, 0.06250000f},
    {   63, 0,  6, 1.01587302f, 0.03125000f},
    {  127, 0,  7, 1.00787402f, 0.01562500f},
    {  255, 0,  8, 1.00392157f, 0.00781250f},
    {  511, 0,  9, 1.00195695f, 0.00390625f},
    { 1023, 0, 10, 1.00097752f, 0.00195312f},
    { 2047, 0, 11, 1.00048852f, 0.00097656f},
    { 4095, 0, 12, 1.00024420f, 0.00048828f},
    { 8191, 0, 13, 1.00012209f, 0.00024414f},
    {16383, 0, 14, 1.00006104f, 0.00012207f},
    {32767, 0, 15, 1.00003052f, 0.00006103f},
    {65535, 0, 16, 1.00001526f, 0.00003051f}
};

/* Table A (ISO Table B.2a) — sblimit=27, 48/44.1kHz normale.
 * nbal = numero bit per leggere l'allocation index di ogni subband. */
static const uint8_t nbal_table_a[MP2_SBLIMIT_A] =
{
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,   /* sb 0-10  */
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, /* sb 11-22 */
    2, 2, 2, 2                          /* sb 23-26 */
};

/* Mappa allocation-index → indice classe quantizzazione.
 * Table A (ISO 11172-3 Table B.2a, sblimit=27):
 *   SB 0-2   (nbal=4): nlevels {zero,3,7,15,31,63,127,255,511,1023,2047,
 *                               4095,8191,16383,32767,65535}  (skip 5,9)
 *   SB 3-10  (nbal=4): nlevels {zero,3,5,7,9,15,31,63,127,255,511,1023,
 *                               2047,4095,8191,65535}         (skip 16383,32767)
 *   SB 11-22 (nbal=3): nlevels {zero,3,5,7,9,15,31,65535}
 *   SB 23-26 (nbal=2): nlevels {zero,3,5,65535}
 */
static const int8_t alloc_to_class_4_hi[16] =       /* SB 0-2 */
    {-1, 0, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const int8_t alloc_to_class_4_mid[16] =      /* SB 3-10 */
    {-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16};
static const int8_t alloc_to_class_3[8] =           /* SB 11-22 */
    {-1, 0, 1, 2, 3, 4, 5, 16};
static const int8_t alloc_to_class_2[4] =           /* SB 23-26 */
    {-1, 0, 1, 16};

/* Scalefactor table: 2^((3-i)/3). Precomputata in init_tables. */
static float scalefactor_table[64];
static int   tables_ready = 0;

static void
init_tables(void)
{
    if (tables_ready) return;

    /* 2^((3-i)/3) = 2 * (1/2)^(i/3) */
    static const float cbrt_ratios[3] =
        { 1.0f, 0.79370052598409979f, 0.62996052494743658f };

    for (int i = 0; i < 64; i++)
    {
        int   k = i / 3;
        int   r = i % 3;
        float p2 = 2.0f;
        for (int j = 0; j < k; j++) p2 *= 0.5f;
        scalefactor_table[i] = p2 * cbrt_ratios[r];
    }

    synfilt_init();
    tables_ready = 1;
}

/* Decoder context (opaco verso l'esterno) */

struct decoder_ctx
{
    int fd;

    /* Frame corrente */
    uint8_t  frame_buf[MAX_FRAME_SIZE];
    uint32_t frame_len;
    uint32_t bit_pos;

    /* Info header */
    uint32_t sample_rate;
    uint16_t bitrate_kbps;
    uint8_t  mode;       /* 0=stereo 1=joint 2=dual 3=mono */
    uint8_t  nch;
    uint8_t  sblimit;

    /* Stato sintesi per canale */
    synfilt_state_t synstate[2];

    /* Buffer per-frame */
    uint8_t allocation[2][32];
    uint8_t scfsi[2][32];
    uint8_t scalefactor[2][32][3];
    float   samples[2][36][32];

    /* Output buffer (1152 stereo frames) */
    int16_t  pcm_out[PCM_PER_FRAME * 2];
    uint32_t pcm_len;
    uint32_t pcm_pos;

    int eof;
};

/* Bit reader */

static uint32_t
read_bits(struct decoder_ctx *ctx, int n)
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++)
    {
        uint32_t bi = ctx->bit_pos++;
        uint32_t by = bi >> 3;
        if (by < ctx->frame_len)
        {
            uint32_t bit = (ctx->frame_buf[by] >> (7 - (bi & 7))) & 1u;
            r = (r << 1) | bit;
        }
        else
        {
            r <<= 1;
        }
    }
    return r;
}

/* load_frame — sync + header + read body */

static int
load_frame(struct decoder_ctx *ctx)
{
    /* (a) Trova sync word: 12 bit a 1 (0xFFF) */
    uint8_t prev = 0, cur = 0;
    int synced = 0;
    while (!synced)
    {
        int n = dobfs_Read(ctx->fd, &cur, 1);
        if (n < 1) { ctx->eof = 1; return -1; }
        if (prev == 0xFFu && (cur & 0xF0u) == 0xF0u) synced = 1;
        else prev = cur;
    }
    ctx->frame_buf[0] = 0xFFu;
    ctx->frame_buf[1] = cur;

    /* (b) Leggi i restanti 2 byte dell'header */
    int n = dobfs_Read(ctx->fd, ctx->frame_buf + 2, 2);
    if (n < 2) { ctx->eof = 1; return -1; }

    /* (c) Parse header */
    uint8_t vid        = (ctx->frame_buf[1] >> 3) & 0x3u;  /* 3 = MPEG-1 */
    uint8_t layer      = (ctx->frame_buf[1] >> 1) & 0x3u;  /* 2 = Layer II */
    uint8_t protection =  ctx->frame_buf[1]       & 0x1u;
    uint8_t bri        = (ctx->frame_buf[2] >> 4) & 0xFu;
    uint8_t sri        = (ctx->frame_buf[2] >> 2) & 0x3u;
    uint8_t padding    = (ctx->frame_buf[2] >> 1) & 0x1u;
    uint8_t mode       = (ctx->frame_buf[3] >> 6) & 0x3u;

    if (vid != 3 || layer != 2)   { debug_print("[mp2] not MPEG-1 L2\n"); return -1; }
    if (bri == 0 || bri == 15)    { debug_print("[mp2] bad bitrate\n"); return -1; }
    if (sri == 3)                 { debug_print("[mp2] bad srate\n"); return -1; }

    ctx->bitrate_kbps = bitrate_table[bri];
    ctx->sample_rate  = sample_rate_table[sri];
    ctx->mode         = mode;
    ctx->nch          = (mode == 3) ? 1 : 2;
    ctx->sblimit      = MP2_SBLIMIT_A;

    /* (d) Calcola lunghezza frame */
    uint32_t fl = (144u * (uint32_t)ctx->bitrate_kbps * 1000u) / ctx->sample_rate
                + (uint32_t)padding;
    if (fl < 4 || fl > MAX_FRAME_SIZE) { debug_print("[mp2] bad flen\n"); return -1; }
    ctx->frame_len = fl;

    /* (e) Leggi corpo del frame */
    uint32_t rem = fl - 4;
    if (rem > 0)
    {
        int got = dobfs_Read(ctx->fd, ctx->frame_buf + 4, (int)rem);
        if ((uint32_t)got < rem) { ctx->eof = 1; return -1; }
    }

    /* (f) Posiziona cursore: skip 4 byte header + eventuale CRC (2 byte) */
    ctx->bit_pos = 32u;
    if (protection == 0) ctx->bit_pos += 16u;

    return 0;
}

/* decode_allocation — legge nbal bit per subband/canale */

static void
decode_allocation(struct decoder_ctx *ctx)
{
    for (int sb = 0; sb < ctx->sblimit; sb++)
    {
        int nbal = nbal_table_a[sb];
        for (int ch = 0; ch < ctx->nch; ch++)
        {
            ctx->allocation[ch][sb] = (uint8_t)read_bits(ctx, nbal);
        }
    }
}

/* decode_scfsi — 2 bit per subband/canale se allocation != 0 */

static void
decode_scfsi(struct decoder_ctx *ctx)
{
    for (int sb = 0; sb < ctx->sblimit; sb++)
    {
        for (int ch = 0; ch < ctx->nch; ch++)
        {
            if (ctx->allocation[ch][sb])
                ctx->scfsi[ch][sb] = (uint8_t)read_bits(ctx, 2);
            else
                ctx->scfsi[ch][sb] = 0;
        }
    }
}

/* decode_scalefactors — 6 bit × (1..3) per subband/canale usato */

static void
decode_scalefactors(struct decoder_ctx *ctx)
{
    for (int sb = 0; sb < ctx->sblimit; sb++)
    {
        for (int ch = 0; ch < ctx->nch; ch++)
        {
            if (!ctx->allocation[ch][sb]) continue;

            uint8_t a, b, c;
            switch (ctx->scfsi[ch][sb])
            {
                case 0: /* 3 indipendenti */
                    a = (uint8_t)read_bits(ctx, 6);
                    b = (uint8_t)read_bits(ctx, 6);
                    c = (uint8_t)read_bits(ctx, 6);
                    break;
                case 1: /* parte 0=1, 2 indipendente */
                    a = b = (uint8_t)read_bits(ctx, 6);
                    c = (uint8_t)read_bits(ctx, 6);
                    break;
                case 2: /* tutte uguali */
                    a = b = c = (uint8_t)read_bits(ctx, 6);
                    break;
                default: /* scfsi=3: parte 0 indip, 1=2 */
                    a = (uint8_t)read_bits(ctx, 6);
                    b = c = (uint8_t)read_bits(ctx, 6);
                    break;
            }
            ctx->scalefactor[ch][sb][0] = a;
            ctx->scalefactor[ch][sb][1] = b;
            ctx->scalefactor[ch][sb][2] = c;
        }
    }
}

/*  * decode_samples — 12 triplet × sblimit × nch, requantize
 *
 * Requantization (ISO 11172-3 Annex 3-A.2.4.3):
 *   1. invertire MSB del codice letto
 *   2. interpretare come signed two's complement su nb bit
 *   3. fraction = signed / 2^(nb-1) → [-1, ~1)
 *   4. v = C * (fraction + D) * scalefactor
 */

static float
requantize(uint32_t raw, const quant_class_t *q, float sf)
{
    int nb    = q->bits;
    int msb   = 1 << (nb - 1);
    int sflip = ((int)raw) ^ msb;
    int sgn   = (sflip >= msb) ? (sflip - 2 * msb) : sflip;

    float fraction = (float)sgn / (float)msb;
    return q->C * (fraction + q->D) * sf;
}

static void
decode_samples(struct decoder_ctx *ctx)
{
    for (int t = 0; t < 12; t++)
    {
        for (int sb = 0; sb < ctx->sblimit; sb++)
        {
            int nbal = nbal_table_a[sb];
            const int8_t *map;
            if      (nbal == 4) map = (sb < 3) ? alloc_to_class_4_hi
                                                : alloc_to_class_4_mid;
            else if (nbal == 3) map = alloc_to_class_3;
            else                map = alloc_to_class_2;

            for (int ch = 0; ch < ctx->nch; ch++)
            {
                uint8_t alloc = ctx->allocation[ch][sb];
                if (alloc == 0)
                {
                    ctx->samples[ch][t * 3 + 0][sb] = 0.0f;
                    ctx->samples[ch][t * 3 + 1][sb] = 0.0f;
                    ctx->samples[ch][t * 3 + 2][sb] = 0.0f;
                    continue;
                }

                int cls = map[alloc];
                const quant_class_t *q = &quant_class[cls];
                uint8_t sf_idx = ctx->scalefactor[ch][sb][t / 4];
                float   sf     = scalefactor_table[sf_idx];

                uint32_t raw[3];
                if (q->group)
                {
                    /* 3 campioni codificati in una parola da q->bits */
                    uint32_t code = read_bits(ctx, q->bits);
                    uint32_t nl = q->nlevels;
                    raw[0] = code % nl; code /= nl;
                    raw[1] = code % nl; code /= nl;
                    raw[2] = code % nl;
                    /* Per grouped: q->bits copre 3 campioni; requantize usa
                     * un "bits virtuale" basato su log2(nlevels). Lo gestisco
                     * passando una copia del class con bits fake. */
                    /* nlevels: 3→2bit, 5→3bit, 9→4bit per-campione */
                    quant_class_t qv = *q;
                    qv.bits = (nl == 3) ? 2 : ((nl == 5) ? 3 : 4);
                    for (int i = 0; i < 3; i++)
                        ctx->samples[ch][t * 3 + i][sb] = requantize(raw[i], &qv, sf);
                }
                else
                {
                    for (int i = 0; i < 3; i++)
                    {
                        uint32_t s = read_bits(ctx, q->bits);
                        ctx->samples[ch][t * 3 + i][sb] = requantize(s, q, sf);
                    }
                }
            }
        }

        /* Per dual/stereo: se un canale ha alloc=0 ma l'altro no, skip già
         * gestito dentro il loop. Per mono (nch=1) il canale 1 non è letto. */
    }

    /* Azzera subband sopra sblimit */
    for (int s = 0; s < 36; s++)
        for (int sb = ctx->sblimit; sb < 32; sb++)
            for (int ch = 0; ch < 2; ch++)
                ctx->samples[ch][s][sb] = 0.0f;
}

/* synthesize — 36 step × nch via synthesis_filter */

static void
synthesize(struct decoder_ctx *ctx)
{
    for (int s = 0; s < 36; s++)
    {
        for (int ch = 0; ch < ctx->nch; ch++)
        {
            synfilt_process(&ctx->synstate[ch],
                            ctx->samples[ch][s],
                            &ctx->pcm_out[s * 32 * 2],
                            ch);
        }

        /* Mono → duplica L in R */
        if (ctx->nch == 1)
        {
            for (int j = 0; j < 32; j++)
            {
                int16_t v = ctx->pcm_out[(s * 32 + j) * 2];
                ctx->pcm_out[(s * 32 + j) * 2 + 1] = v;
            }
        }
    }
    ctx->pcm_len = PCM_PER_FRAME;
    ctx->pcm_pos = 0;
}

/* decode_frame_full — top-level pipeline */

static int
decode_frame_full(struct decoder_ctx *ctx)
{
    if (load_frame(ctx) < 0)       return -1;
    decode_allocation(ctx);
    decode_scfsi(ctx);
    decode_scalefactors(ctx);
    decode_samples(ctx);
    synthesize(ctx);
    return 0;
}

/* API pubblica */

decoder_ctx_t *
mp2_open(int fd, audio_format_t *fmt)
{
    init_tables();

    struct decoder_ctx *ctx = calloc(1, sizeof(struct decoder_ctx));
    if (!ctx) return NULL;

    ctx->fd = fd;
    ctx->eof = 0;
    synfilt_reset(&ctx->synstate[0]);
    synfilt_reset(&ctx->synstate[1]);

    /* Decodifica il primo frame per popolare fmt */
    if (decode_frame_full(ctx) < 0)
    {
        free(ctx);
        return NULL;
    }

    fmt->sample_rate     = ctx->sample_rate;
    fmt->bits_per_sample = 16;
    fmt->channels        = 2;        /* usciamo sempre stereo interleaved */
    fmt->data_size       = 0;
    fmt->data_offset     = 0;

    return (decoder_ctx_t *)ctx;
}

int
mp2_decode(decoder_ctx_t *opaque, int16_t *out, uint32_t max_frames)
{
    struct decoder_ctx *ctx = (struct decoder_ctx *)opaque;
    uint32_t written = 0;

    while (written < max_frames)
    {
        /* Se il buffer PCM corrente è vuoto, decodifica un nuovo frame */
        if (ctx->pcm_pos >= ctx->pcm_len)
        {
            if (ctx->eof) break;
            if (decode_frame_full(ctx) < 0) break;
        }

        uint32_t avail = ctx->pcm_len - ctx->pcm_pos;
        uint32_t need  = max_frames - written;
        uint32_t take  = avail < need ? avail : need;

        memcpy(out + written * 2,
               ctx->pcm_out + ctx->pcm_pos * 2,
               take * 2 * sizeof(int16_t));

        ctx->pcm_pos += take;
        written      += take;
    }

    return (int)written;
}

void
mp2_close(decoder_ctx_t *opaque)
{
    if (opaque) free(opaque);
}
