/* DobAudioPlayer_stub.c — Client-side IPC stub for the audio player service. */

#include <DobAudioPlayer.h>
#include <dob/ipc.h>
#include <dob/reconnect.h>
#include <string.h>

static uint32_t player_port = 0;
#define IPC_CALL(m, r) _dob_call_reconnect(&player_port, "audioplayer", 5000, m, r)

static int
player_play(const char *path)
{
    if (!path) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = PLAYER_CMD_PLAY;
    msg.payload = (void *)path;
    msg.payload_size = strlen(path) + 1;

    return IPC_CALL(&msg, &reply) == DOB_OK ? (int)reply.code : -1;
}

static int
player_stop(void)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = PLAYER_CMD_STOP;
    return IPC_CALL(&msg, &reply) == DOB_OK ? (int)reply.code : -1;
}

static int
player_pause(void)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = PLAYER_CMD_PAUSE;
    return IPC_CALL(&msg, &reply) == DOB_OK ? (int)reply.code : -1;
}

static int
player_resume(void)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = PLAYER_CMD_RESUME;
    return IPC_CALL(&msg, &reply) == DOB_OK ? (int)reply.code : -1;
}

static int
player_status(dobplayer_status_t *out)
{
    if (!out) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = PLAYER_CMD_STATUS;

    if (IPC_CALL(&msg, &reply) != DOB_OK)
        return -1;

    if (reply.payload && reply.payload_size >= sizeof(dobplayer_status_t))
        memcpy(out, reply.payload, sizeof(dobplayer_status_t));
    return (int)reply.code;
}

static int
player_set_volume(int vol)
{
    dob_msg_t msg = {0}, reply = {0};
    msg.code = PLAYER_CMD_SET_VOLUME;
    msg.arg0 = (uint32_t)vol;
    return IPC_CALL(&msg, &reply) == DOB_OK ? (int)reply.code : -1;
}

dobplayer_interface_t dobplayer = {
    .Play      = player_play,
    .Stop      = player_stop,
    .Pause     = player_pause,
    .Resume    = player_resume,
    .Status    = player_status,
    .SetVolume = player_set_volume,
};
