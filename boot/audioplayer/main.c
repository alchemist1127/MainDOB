/* MainDOB Audio Player Service — event-driven
 *
 * IPC service that plays audio files from disk.
 * Reads via DobFileSystem, decodes (WAV/MP2/MP3/OGG), sends PCM
 * to the AC97 driver via DobAudio stub.
 *
 * Architecture:
 *   App → IPC "play /path" → this service → DobFileSystem → decode → DobAudio
 *
 * Feed mechanism: subscribes to AUDIO_EVT_NEEDS_DATA from the audio driver.
 * No timer. When the audio channel's ring crosses below the low watermark,
 * the driver posts an event; we decode and write more. Zero CPU when idle.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dob/server.h>
#include <dob/registry.h>
#include <dob/ipc.h>
#include <dob/types.h>
#include <DobFileSystem.h>
#include <DobAudio.h>
#include <DobAudioPlayer.h>

#include "decode_wav.h"
#include "decode_mp2.h"
#include "decode_mp3.h"
#include "decode_ogg.h"

/* Constants */

#define FEED_FRAMES         2048    /* Frames per decode cycle (= 8KB stereo 16-bit) */
#define LOW_WATERMARK_BYTES 32768   /* Notify when channel has < 32KB queued */

/* Playback State */

static uint32_t my_port   = 0;

static int             file_fd    = -1;
static int             audio_ch   = -1;
static decoder_ctx_t  *decoder    = NULL;
static uint8_t         cur_format = PLAYER_FMT_UNKNOWN;
static uint8_t         cur_state  = PLAYER_STATE_IDLE;
static uint8_t         cur_volume = 100;
static audio_format_t  cur_fmt;
static uint32_t        frames_played = 0;

static int  (*decode_fn)(decoder_ctx_t *, int16_t *, uint32_t) = NULL;
static void (*close_fn)(decoder_ctx_t *) = NULL;

static int16_t pcm_buf[FEED_FRAMES * 2];

/* Overflow carryover: when a Write() returns short because the ring is
 * full, we keep the leftover PCM here and drain it on the next feed. */
static int16_t overflow_buf[FEED_FRAMES * 2];
static uint32_t overflow_samples = 0;  /* total stereo samples still in buffer */
static uint32_t overflow_offset  = 0;  /* how many already consumed */

/*  *  Format Detection
 *
 *  Extension-based: simple, robust, and lets MP2/MP3 files with
 *  prepended ID3v2 tags Just Work — the MPEG decoders sync-scan
 *  byte-by-byte and walk over the tag on their own. WAV/OGG files
 *  in the wild never carry ID3 prefixes, so the dispatchers below
 *  always see a clean header at offset 0.
 */

static uint8_t
format_from_path(const char *path)
{
    if (!path) return PLAYER_FMT_UNKNOWN;

    /* Find last '.' after the last '/' */
    const char *dot = NULL;
    for (const char *p = path; *p; p++)
    {
        if (*p == '/')      dot = NULL;
        else if (*p == '.') dot = p;
    }
    if (!dot || !dot[1]) return PLAYER_FMT_UNKNOWN;

    /* Lowercase copy of the extension (max 4 chars + NUL) */
    char ext[5];
    uint32_t i = 0;
    for (const char *p = dot + 1; *p && i < sizeof(ext) - 1; p++, i++)
    {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        ext[i] = c;
    }
    ext[i] = '\0';

    if (strcmp(ext, "wav") == 0) return PLAYER_FMT_WAV;
    if (strcmp(ext, "mp2") == 0) return PLAYER_FMT_MP2;
    if (strcmp(ext, "mp3") == 0) return PLAYER_FMT_MP3;
    if (strcmp(ext, "ogg") == 0) return PLAYER_FMT_OGG;
    return PLAYER_FMT_UNKNOWN;
}

/* Playback Control */

static void
close_decoder_and_file(void)
{
    if (decoder && close_fn)
    {
        close_fn(decoder);
        decoder = NULL;
    }
    if (file_fd >= 0)
    {
        dobfs_Close(file_fd);
        file_fd = -1;
    }
    decode_fn = NULL;
    close_fn  = NULL;
}

static void
playback_stop(void)
{
    close_decoder_and_file();

    /* Immediate close: cuts audio right away */
    if (audio_ch >= 0)
    {
        dobaudio.channel.Close(audio_ch);
        audio_ch = -1;
    }

    cur_format = PLAYER_FMT_UNKNOWN;
    cur_state  = PLAYER_STATE_IDLE;
    frames_played = 0;
    overflow_samples = 0;
    overflow_offset  = 0;
    debug_print("[player] Stopped.\n");
}

/* Feed loop: drains overflow, then decodes+writes until the ring is full
 * or we hit EOF. Drives both the initial prime (empty ring after Open)
 * and each NEEDS_DATA response. */
static void
handle_feed(void)
{
    if (cur_state != PLAYER_STATE_PLAYING || audio_ch < 0) return;

    /* Drain leftover PCM from a previously truncated write. */
    while (overflow_samples > overflow_offset)
    {
        int16_t *src = overflow_buf + overflow_offset;
        uint32_t remaining_samples = overflow_samples - overflow_offset;
        uint32_t remaining_bytes   = remaining_samples * sizeof(int16_t);
        int w = dobaudio.channel.Write(audio_ch, src, remaining_bytes);
        if (w < 0) return;
        if (w == 0) return;    /* ring still full, try next feed */
        uint32_t wrote_samples = (uint32_t)w / sizeof(int16_t);
        overflow_offset += wrote_samples;
        frames_played   += wrote_samples / 2;
        if ((uint32_t)w < remaining_bytes) return;
    }
    overflow_samples = 0;
    overflow_offset  = 0;

    if (!decode_fn) return;

    for (;;)
    {
        int frames = decode_fn(decoder, pcm_buf, FEED_FRAMES);

        if (frames <= 0)
        {
            debug_print("[player] EOF, draining.\n");
            close_decoder_and_file();
            if (audio_ch >= 0)
            {
                dobaudio.channel.CloseDrain(audio_ch);
                audio_ch = -1;
            }
            cur_state = PLAYER_STATE_IDLE;
            return;
        }

        uint32_t decoded_bytes = (uint32_t)frames * 4;   /* stereo 16-bit */
        int w = dobaudio.channel.Write(audio_ch, pcm_buf, decoded_bytes);
        if (w < 0) return;   /* IPC/driver error */

        frames_played += (uint32_t)w / 4;

        if ((uint32_t)w < decoded_bytes)
        {
            /* Ring full or partial write: park the unwritten tail for the
             * next feed. Handles w==0 (ring completely full) as well. */
            uint32_t wrote_samples    = (uint32_t)w / sizeof(int16_t);
            uint32_t decoded_samples  = (uint32_t)frames * 2;
            uint32_t leftover_samples = decoded_samples - wrote_samples;
            memcpy(overflow_buf, pcm_buf + wrote_samples,
                   leftover_samples * sizeof(int16_t));
            overflow_samples = leftover_samples;
            overflow_offset  = 0;
            return;
        }
    }
}

static int
playback_start(const char *path)
{
    if (cur_state != PLAYER_STATE_IDLE)
        playback_stop();

    cur_format = format_from_path(path);
    if (cur_format == PLAYER_FMT_UNKNOWN)
    {
        debug_print("[player] Unknown format (extension).\n");
        return -1;
    }

    file_fd = dobfs_Open(path, FS_READ);
    if (file_fd < 0)
    {
        debug_print("[player] Cannot open file.\n");
        return -1;
    }

    memset(&cur_fmt, 0, sizeof(cur_fmt));

    switch (cur_format)
    {
        case PLAYER_FMT_WAV:
            decoder = wav_open(file_fd, &cur_fmt);
            decode_fn = wav_decode; close_fn = wav_close; break;
        case PLAYER_FMT_MP2:
            decoder = mp2_open(file_fd, &cur_fmt);
            decode_fn = mp2_decode; close_fn = mp2_close; break;
        case PLAYER_FMT_MP3:
            decoder = mp3_open(file_fd, &cur_fmt);
            decode_fn = mp3_decode; close_fn = mp3_close; break;
        case PLAYER_FMT_OGG:
            decoder = ogg_open(file_fd, &cur_fmt);
            decode_fn = ogg_decode; close_fn = ogg_close; break;
        default:
            /* unreachable: format_from_path filtered UNKNOWN above */
            dobfs_Close(file_fd);
            file_fd = -1;
            return -1;
    }

    if (!decoder)
    {
        debug_print("[player] Decoder open failed.\n");
        dobfs_Close(file_fd);
        file_fd = -1;
        return -1;
    }

    audio_ch = dobaudio.channel.Open(cur_fmt.sample_rate, 16, 2);
    if (audio_ch < 0)
    {
        debug_print("[player] Cannot open audio channel.\n");
        close_fn(decoder);
        decoder = NULL;
        dobfs_Close(file_fd);
        file_fd = -1;
        return -1;
    }

    dobaudio.channel.SetVolume(audio_ch, (int)cur_volume);
    dobaudio.channel.Subscribe(audio_ch, my_port, LOW_WATERMARK_BYTES);

    cur_state = PLAYER_STATE_PLAYING;
    frames_played = 0;

    char tmp[128];
    snprintf(tmp, sizeof(tmp), "[player] Playing: fmt=%d rate=%u ch=%u\n",
             cur_format, cur_fmt.sample_rate, cur_fmt.channels);
    debug_print(tmp);

    /* Prime the ring. The first WRITE starts DMA in the driver.
     * Further feeds are driven by AUDIO_EVT_NEEDS_DATA. */
    handle_feed();
    return 0;
}

/* IPC Command Handler */

static dob_status_t
handle_command(dob_msg_t *msg, dob_msg_t *reply)
{
    switch (msg->code)
    {
        case PLAYER_CMD_PLAY:
        {
            if (!msg->payload || msg->payload_size == 0)
            {
                reply->code = (uint32_t)-1;
                return DOB_OK;
            }
            char path[256];
            uint32_t len = msg->payload_size;
            if (len >= sizeof(path)) len = sizeof(path) - 1;
            memcpy(path, msg->payload, len);
            path[len] = '\0';
            reply->code = (uint32_t)playback_start(path);
            return DOB_OK;
        }

        case PLAYER_CMD_STOP:
            playback_stop();
            reply->code = 0;
            return DOB_OK;

        case PLAYER_CMD_PAUSE:
            if (cur_state == PLAYER_STATE_PLAYING && audio_ch >= 0)
            {
                dobaudio.channel.Pause(audio_ch);
                cur_state = PLAYER_STATE_PAUSED;
            }
            reply->code = 0;
            return DOB_OK;

        case PLAYER_CMD_RESUME:
            if (cur_state == PLAYER_STATE_PAUSED && audio_ch >= 0)
            {
                dobaudio.channel.Resume(audio_ch);
                cur_state = PLAYER_STATE_PLAYING;
            }
            reply->code = 0;
            return DOB_OK;

        case PLAYER_CMD_STATUS:
        {
            static dobplayer_status_t status;
            memset(&status, 0, sizeof(status));
            status.state       = cur_state;
            status.format      = cur_format;
            status.volume      = cur_volume;
            if (cur_fmt.sample_rate > 0)
                status.position_ms = (frames_played * 1000) / cur_fmt.sample_rate;
            status.sample_rate = cur_fmt.sample_rate;
            status.bits        = (uint8_t)cur_fmt.bits_per_sample;
            status.channels    = (uint8_t)cur_fmt.channels;

            if (cur_format == PLAYER_FMT_WAV && cur_fmt.sample_rate > 0)
            {
                uint32_t total_frames = cur_fmt.data_size /
                    (cur_fmt.channels * (cur_fmt.bits_per_sample / 8));
                status.duration_ms = (total_frames * 1000) / cur_fmt.sample_rate;
            }
            reply->payload = &status;
            reply->payload_size = sizeof(status);
            reply->code = 0;
            return DOB_OK;
        }

        case PLAYER_CMD_SET_VOLUME:
        {
            if (msg->arg0 > 100) { reply->code = (uint32_t)-1; return DOB_OK; }
            cur_volume = (uint8_t)msg->arg0;
            if (audio_ch >= 0)
                dobaudio.channel.SetVolume(audio_ch, (int)cur_volume);
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
    debug_print("[player] Starting...\n");
    dob_server_init("audioplayer");
    my_port = dob_server_get_port();

    if (dob_registry_wait("audio", 10000) == 0)
        debug_print("[player] Warning: audio driver not found.\n");
    if (dob_registry_wait("DobFileSystem", 10000) == 0)
        debug_print("[player] Warning: DobFileSystem not found.\n");

    debug_print("[player] Ready (event-driven, no idle wakeups).\n");

    /* Event loop — blocks in dob_ipc_receive. Wakes only on:
     *   - Client IPC call (play/stop/pause/...)
     *   - AUDIO_EVT_NEEDS_DATA from driver (only while playing)
     * Zero CPU cost when no playback is active. */
    for (;;)
    {
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        dob_ipc_receive(my_port, &msg);

        if (msg.type == 1)
        {
            dob_msg_t reply;
            memset(&reply, 0, sizeof(reply));
            reply.code = (uint32_t)handle_command(&msg, &reply);
            dob_ipc_reply(msg.sender_tid, &reply);
            continue;
        }

        if (msg.type == 4 && msg.code == AUDIO_EVT_NEEDS_DATA)
        {
            handle_feed();
            continue;
        }
    }

    return 0;
}
