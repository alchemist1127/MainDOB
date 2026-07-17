/* DobAudio_stub.c — Client-side IPC stub for the audio driver.
 *
 * Link this into any program that needs to produce sound.
 * Uses _dob_call_reconnect for self-healing if the driver restarts.
 */

#include <DobAudio.h>
#include <dob/ipc.h>
#include <dob/reconnect.h>
#include <string.h>

static uint32_t audio_port = 0;
#define IPC_CALL(m, r) _dob_call_reconnect(&audio_port, "audio", 5000, m, r)

/* Channel API */

static int
audio_chan_open(uint32_t rate, uint8_t bits, uint8_t channels)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = AUDIO_CMD_OPEN;
    msg.arg0 = rate;
    msg.arg1 = bits;
    msg.arg2 = channels;

    if (IPC_CALL(&msg, &reply) != DOB_OK)
        return -1;
    return (int32_t)reply.code < 0 ? -1 : (int)reply.arg0;
}

static int
audio_chan_write(int ch, const void *pcm, uint32_t len)
{
    if (!pcm || len == 0) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = AUDIO_CMD_WRITE;
    msg.arg0 = (uint32_t)ch;
    msg.payload = (void *)pcm;
    msg.payload_size = len;

    if (IPC_CALL(&msg, &reply) != DOB_OK)
        return -1;
    return (int32_t)reply.code < 0 ? -1 : (int)reply.arg0;
}

static int
audio_chan_close(int ch)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = AUDIO_CMD_CLOSE;
    msg.arg0 = (uint32_t)ch;
    return IPC_CALL(&msg, &reply) == DOB_OK ? (int)reply.code : -1;
}

static int
audio_chan_close_drain(int ch)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = AUDIO_CMD_CLOSE_DRAIN;
    msg.arg0 = (uint32_t)ch;
    return IPC_CALL(&msg, &reply) == DOB_OK ? (int)reply.code : -1;
}

static int
audio_chan_subscribe(int ch, uint32_t notify_port, uint32_t low_watermark_bytes)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = AUDIO_CMD_SUBSCRIBE;
    msg.arg0 = (uint32_t)ch;
    msg.arg1 = notify_port;
    msg.arg2 = low_watermark_bytes;
    return IPC_CALL(&msg, &reply) == DOB_OK ? (int)reply.code : -1;
}

static int
audio_chan_set_volume(int ch, int vol)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = AUDIO_CMD_SET_CHAN_VOL;
    msg.arg0 = (uint32_t)ch;
    msg.arg1 = (uint32_t)vol;
    return IPC_CALL(&msg, &reply) == DOB_OK ? (int)reply.code : -1;
}

static int
audio_chan_flush(int ch)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = AUDIO_CMD_FLUSH;
    msg.arg0 = (uint32_t)ch;
    return IPC_CALL(&msg, &reply) == DOB_OK ? (int)reply.code : -1;
}

static int
audio_chan_pause(int ch)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = AUDIO_CMD_PAUSE;
    msg.arg0 = (uint32_t)ch;
    return IPC_CALL(&msg, &reply) == DOB_OK ? (int)reply.code : -1;
}

static int
audio_chan_resume(int ch)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = AUDIO_CMD_RESUME;
    msg.arg0 = (uint32_t)ch;
    return IPC_CALL(&msg, &reply) == DOB_OK ? (int)reply.code : -1;
}

/* Master API */

static int
audio_master_set_volume(int vol)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = AUDIO_CMD_SET_MASTER_VOL;
    msg.arg0 = (uint32_t)vol;
    return IPC_CALL(&msg, &reply) == DOB_OK ? (int)reply.code : -1;
}

static int
audio_master_get_volume(void)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = AUDIO_CMD_GET_INFO;
    if (IPC_CALL(&msg, &reply) != DOB_OK)
        return -1;
    return (int)reply.arg0;
}

/* Info */

static int
audio_get_info(dobaudio_info_t *info)
{
    if (!info) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = AUDIO_CMD_GET_INFO;

    if (IPC_CALL(&msg, &reply) != DOB_OK)
        return -1;

    info->master_volume   = (uint8_t)reply.arg0;
    info->active_channels = (uint8_t)reply.arg1;
    info->mix_rate        = AUDIO_NATIVE_RATE;
    info->mix_bits        = AUDIO_NATIVE_BITS;
    info->mix_channels    = AUDIO_NATIVE_CHANNELS;
    return 0;
}

/* Interface struct — single global instance */

dobaudio_interface_t dobaudio = {
    .channel = {
        .Open       = audio_chan_open,
        .Write      = audio_chan_write,
        .Close      = audio_chan_close,
        .CloseDrain = audio_chan_close_drain,
        .Subscribe  = audio_chan_subscribe,
        .SetVolume  = audio_chan_set_volume,
        .Flush      = audio_chan_flush,
        .Pause      = audio_chan_pause,
        .Resume     = audio_chan_resume,
    },
    .master = {
        .SetVolume = audio_master_set_volume,
        .GetVolume = audio_master_get_volume,
    },
    .GetInfo = audio_get_info,
};
