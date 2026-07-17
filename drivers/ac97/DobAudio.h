/* DobAudio — MainDOB Audio Pipeline API
 *
 * Public header for any program that wants to produce sound.
 * Link with DobAudio_stub.c (static IPC stub with auto-reconnect).
 *
 * Usage (polling — simple, one-shot sounds):
 *
 *   int ch = dobaudio.channel.Open(48000, 16, 2);
 *   dobaudio.channel.Write(ch, pcm_data, pcm_len);
 *   dobaudio.channel.Close(ch);            // immediate stop, audio cut
 *
 * Usage (event-driven — streaming, recommended):
 *
 *   int ch = dobaudio.channel.Open(48000, 16, 2);
 *   dobaudio.channel.Subscribe(ch, my_port, 8192);   // notify at 8KB free
 *   dobaudio.channel.Write(ch, pcm, pcm_len);        // prime the ring
 *   // ... in event loop, on AUDIO_EVT_NEEDS_DATA: write more PCM ...
 *   dobaudio.channel.CloseDrain(ch);       // drain ring, then close
 *
 * All PCM data must be signed 16-bit little-endian, stereo.
 * Sample rate: any standard rate (8000–48000 Hz).
 */

#ifndef MAINDOB_DOB_AUDIO_H
#define MAINDOB_DOB_AUDIO_H

#include <dob/types.h>

/* IPC command codes (msg.code on request) */

#define AUDIO_CMD_OPEN            1
#define AUDIO_CMD_CLOSE           2
#define AUDIO_CMD_WRITE           3
#define AUDIO_CMD_SET_CHAN_VOL    4
#define AUDIO_CMD_SET_MASTER_VOL  5
#define AUDIO_CMD_GET_INFO        6
#define AUDIO_CMD_FLUSH           7
#define AUDIO_CMD_PAUSE           8
#define AUDIO_CMD_RESUME          9
#define AUDIO_CMD_SUBSCRIBE       10   /* arg0=ch_id, arg1=notify_port, arg2=low_watermark_bytes */
#define AUDIO_CMD_CLOSE_DRAIN     11   /* arg0=ch_id — destroy when ring empties */

/* Async event codes (sent by driver via ipc_post, msg.type=4) */

#define AUDIO_EVT_NEEDS_DATA      221  /* arg0=ch_id, arg1=free_bytes */

/* Constants */

#define AUDIO_NATIVE_RATE       48000
#define AUDIO_NATIVE_BITS       16
#define AUDIO_NATIVE_CHANNELS   2

/* Maximum PCM payload per WRITE call (bytes).
 * Larger writes should be split by the caller. */
#define AUDIO_MAX_WRITE_SIZE    8192

/* Info structure (returned by GET_INFO) */

typedef struct
{
    uint32_t mix_rate;
    uint8_t  mix_bits;
    uint8_t  mix_channels;
    uint8_t  master_volume;
    uint8_t  active_channels;
} dobaudio_info_t;

/* Client-side interface (populated by DobAudio_stub.c) */

typedef struct
{
    struct
    {
        int  (*Open)(uint32_t rate, uint8_t bits, uint8_t channels);
        int  (*Write)(int ch, const void *pcm, uint32_t len);
        int  (*Close)(int ch);
        int  (*CloseDrain)(int ch);
        int  (*Subscribe)(int ch, uint32_t notify_port, uint32_t low_watermark_bytes);
        int  (*SetVolume)(int ch, int vol);
        int  (*Flush)(int ch);
        int  (*Pause)(int ch);
        int  (*Resume)(int ch);
    } channel;

    struct
    {
        int  (*SetVolume)(int vol);
        int  (*GetVolume)(void);
    } master;

    int  (*GetInfo)(dobaudio_info_t *info);
} dobaudio_interface_t;

extern dobaudio_interface_t dobaudio;

#endif /* MAINDOB_DOB_AUDIO_H */
