/* decode_wav.c — WAV/RIFF decoder
 *
 * Parses RIFF/WAVE container, extracts PCM data.
 * For 16-bit stereo: raw pass-through (driver handles sample rate via VRA).
 * For 8-bit or mono: converts to 16-bit stereo.
 */

#include "decode_wav.h"
#include <DobFileSystem.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  /* debug_print */

/* Read buffer size (bytes) — dimensionato per una chiamata wav_decode con
 * max_frames=2048 input 16-bit stereo o 8-bit tri-canale */
#define READ_BUF_SIZE   8192

/* Decoder Context */

struct decoder_ctx
{
    int             fd;
    audio_format_t  fmt;
    uint32_t        bytes_read;     /* Bytes già letti da file (non consumati) */

    /* Read buffer (streaming) */
    uint8_t         buf[READ_BUF_SIZE];
    uint32_t        buf_pos;
    uint32_t        buf_len;
};

/* RIFF Helpers */

static uint16_t
read_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t
read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool
tag_eq(const uint8_t *p, const char *tag)
{
    return p[0] == (uint8_t)tag[0] && p[1] == (uint8_t)tag[1] &&
           p[2] == (uint8_t)tag[2] && p[3] == (uint8_t)tag[3];
}

/* Open: parse RIFF header */

decoder_ctx_t *
wav_open(int fd, audio_format_t *fmt)
{
    uint8_t hdr[44];
    int n = dobfs_Read(fd, hdr, 44);
    if (n < 44)
    {
        debug_print("[player] wav: header read failed\n");
        return NULL;
    }

    /* RIFF....WAVE */
    if (!tag_eq(hdr, "RIFF") || !tag_eq(hdr + 8, "WAVE"))
    {
        debug_print("[player] wav: bad RIFF/WAVE magic\n");
        return NULL;
    }

    /* Parse chunks: scan for "fmt " and "data" */
    uint16_t audio_fmt = 0;
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits = 0;
    uint32_t data_size = 0;
    uint32_t data_offset = 0;

    /* Standard WAV: fmt at offset 12, data follows */
    uint32_t pos = 12;
    uint8_t chunk_hdr[8];

    /* Re-read from start to handle non-standard chunk ordering */
    /* Reset position by closing and reopening - not ideal.
     * Instead, parse the initial 44 bytes which covers standard WAV. */

    /* Check if fmt chunk is at standard position (offset 12) */
    if (tag_eq(hdr + 12, "fmt "))
    {
        uint32_t fmt_size = read_u32_le(hdr + 16);
        (void)fmt_size;
        audio_fmt    = read_u16_le(hdr + 20);
        num_channels = read_u16_le(hdr + 22);
        sample_rate  = read_u32_le(hdr + 24);
        /* skip byte rate (28) and block align (32) */
        bits         = read_u16_le(hdr + 34);
    }
    else
    {
        return NULL;  /* Non-standard layout not supported */
    }

    /* Only PCM format (1) supported */
    if (audio_fmt != 1) return NULL;

    /* Find data chunk — usually at offset 36 */
    if (tag_eq(hdr + 36, "data"))
    {
        data_size   = read_u32_le(hdr + 40);
        data_offset = 44;
    }
    else
    {
        /* Scan forward for "data" chunk.
         * The file cursor is at byte 44. Read chunk headers. */
        pos = 36;
        bool found = false;

        /* We already have bytes 36..43 in hdr.
         * Check if there's a non-data chunk here and skip it. */
        while (!found)
        {
            uint32_t chunk_sz;

            if (pos <= 36)
            {
                /* Still in the initial header buffer */
                if (!tag_eq(hdr + pos, "data"))
                {
                    chunk_sz = read_u32_le(hdr + pos + 4);
                    pos += 8 + chunk_sz;
                    /* Need to seek — read from file */
                    /* For simplicity, read and discard */
                    uint32_t to_skip = pos - 44;
                    uint8_t skip[256];
                    while (to_skip > 0)
                    {
                        uint32_t r = to_skip > 256 ? 256 : to_skip;
                        int got = dobfs_Read(fd, skip, r);
                        if (got <= 0) return NULL;
                        to_skip -= (uint32_t)got;
                    }
                }
                else
                {
                    data_size = read_u32_le(hdr + pos + 4);
                    data_offset = pos + 8;
                    found = true;
                }
            }
            else
            {
                /* Read chunk header from file */
                int got = dobfs_Read(fd, chunk_hdr, 8);
                if (got < 8) return NULL;

                if (tag_eq(chunk_hdr, "data"))
                {
                    data_size = read_u32_le(chunk_hdr + 4);
                    data_offset = pos + 8;
                    found = true;
                }
                else
                {
                    chunk_sz = read_u32_le(chunk_hdr + 4);
                    /* Skip this chunk */
                    uint8_t skip[256];
                    uint32_t to_skip = chunk_sz;
                    while (to_skip > 0)
                    {
                        uint32_t r = to_skip > 256 ? 256 : to_skip;
                        int sgot = dobfs_Read(fd, skip, r);
                        if (sgot <= 0) return NULL;
                        to_skip -= (uint32_t)sgot;
                    }
                    pos += 8 + chunk_sz;
                }
            }
        }
    }

    if (data_size == 0 || sample_rate == 0 || bits == 0 || num_channels == 0)
        return NULL;

    /* Allocate context */
    decoder_ctx_t *ctx = (decoder_ctx_t *)malloc(sizeof(decoder_ctx_t));
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(decoder_ctx_t));
    ctx->fd = fd;
    ctx->fmt.sample_rate    = sample_rate;
    ctx->fmt.bits_per_sample = bits;
    ctx->fmt.channels       = num_channels;
    ctx->fmt.data_size      = data_size;
    ctx->fmt.data_offset    = data_offset;

    if (fmt) *fmt = ctx->fmt;

    return ctx;
}

/*  *  Decode: pull bytes into buf, then bulk-convert to 16-bit stereo
 *
 *  Tre percorsi:
 *    - Fast path 16-bit stereo: lettura diretta file→out, niente conversione
 *    - Bulk conversion: refill buf, poi loop specializzato per (bps, nch)
 *      che produce più frame in una sola passata
 */

int
wav_decode(decoder_ctx_t *ctx, int16_t *out, uint32_t max_frames)
{
    if (!ctx || !out || max_frames == 0) return -1;

    /* Fast path: 16-bit stereo → memcpy diretto da file a out */
    if (ctx->fmt.bits_per_sample == 16 && ctx->fmt.channels == 2)
    {
        uint32_t want_bytes = max_frames * 4;
        uint32_t remaining  = ctx->fmt.data_size - ctx->bytes_read;
        if (remaining == 0) return 0;
        uint32_t to_read = want_bytes < remaining ? want_bytes : remaining;

        int n = dobfs_Read(ctx->fd, (uint8_t *)out, to_read);
        if (n <= 0) return 0;
        ctx->bytes_read += (uint32_t)n;
        return n / 4;
    }

    /* Conversion path: refill buf se serve, poi bulk-convert */
    uint32_t bps         = ctx->fmt.bits_per_sample / 8;
    uint32_t nch         = ctx->fmt.channels;
    uint32_t frame_bytes = bps * nch;
    uint32_t frames_out  = 0;

    if (frame_bytes == 0) return -1;

    while (frames_out < max_frames)
    {
        uint32_t avail_bytes  = ctx->buf_len - ctx->buf_pos;
        uint32_t avail_frames = avail_bytes / frame_bytes;

        /* Refill quando il buffer non contiene più un frame intero */
        if (avail_frames == 0)
        {
            if (avail_bytes > 0)
                memmove(ctx->buf, ctx->buf + ctx->buf_pos, avail_bytes);
            ctx->buf_pos = 0;
            ctx->buf_len = avail_bytes;

            uint32_t remaining = ctx->fmt.data_size - ctx->bytes_read;
            if (remaining == 0) break;

            uint32_t to_read = READ_BUF_SIZE - ctx->buf_len;
            if (to_read > remaining) to_read = remaining;

            int n = dobfs_Read(ctx->fd, ctx->buf + ctx->buf_len, to_read);
            if (n <= 0) break;
            ctx->buf_len    += (uint32_t)n;
            ctx->bytes_read += (uint32_t)n;

            avail_frames = (ctx->buf_len - ctx->buf_pos) / frame_bytes;
            if (avail_frames == 0) break;
        }

        /* Quanti frame convertire in questa passata */
        uint32_t burst = max_frames - frames_out;
        if (burst > avail_frames) burst = avail_frames;

        const uint8_t *p  = ctx->buf + ctx->buf_pos;
        int16_t       *op = out + frames_out * 2;

        /* Loop specializzati per (bps, nch) — zero chiamate per-sample */
        if (bps == 2 && nch == 1)
        {
            /* 16-bit mono → 16-bit stereo (duplica L→R) */
            for (uint32_t i = 0; i < burst; i++)
            {
                int16_t v = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
                op[0] = v;
                op[1] = v;
                p  += 2;
                op += 2;
            }
        }
        else if (bps == 1 && nch == 1)
        {
            /* 8-bit unsigned mono → 16-bit stereo */
            for (uint32_t i = 0; i < burst; i++)
            {
                int16_t v = (int16_t)(((int)p[0] - 128) << 8);
                op[0] = v;
                op[1] = v;
                p  += 1;
                op += 2;
            }
        }
        else if (bps == 1 && nch == 2)
        {
            /* 8-bit unsigned stereo → 16-bit stereo */
            for (uint32_t i = 0; i < burst; i++)
            {
                op[0] = (int16_t)(((int)p[0] - 128) << 8);
                op[1] = (int16_t)(((int)p[1] - 128) << 8);
                p  += 2;
                op += 2;
            }
        }
        else if (bps == 2 && nch >= 2)
        {
            /* 16-bit multichannel: prendi solo i primi 2 canali */
            for (uint32_t i = 0; i < burst; i++)
            {
                op[0] = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
                op[1] = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
                p  += frame_bytes;
                op += 2;
            }
        }
        else
        {
            /* Formato non gestito (es. 24/32-bit): silenzio */
            for (uint32_t i = 0; i < burst * 2; i++) op[i] = 0;
        }

        ctx->buf_pos += burst * frame_bytes;
        frames_out   += burst;
    }

    return (int)frames_out;
}

void
wav_close(decoder_ctx_t *ctx)
{
    if (ctx)
        free(ctx);
}
