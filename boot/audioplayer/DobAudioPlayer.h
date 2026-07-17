/* DobAudioPlayer — MainDOB Media Playback Service
 *
 * High-level audio file playback. Uses the AC97 driver for PCM output.
 * Supports: WAV, MP2, MP3, OGG Vorbis.
 *
 * Usage:
 *   #include <DobAudioPlayer.h>
 *
 *   dobplayer.Play("/DATA/Music/song.mp3");
 *   dobplayer.Pause();
 *   dobplayer.Resume();
 *   dobplayer.Stop();
 *
 * For real-time audio (games, synths), use DobAudio.h directly.
 * This service is a convenience layer for file-based playback.
 */

#ifndef MAINDOB_DOB_AUDIO_PLAYER_H
#define MAINDOB_DOB_AUDIO_PLAYER_H

#include <dob/types.h>

/* IPC command codes */

#define PLAYER_CMD_PLAY       1
#define PLAYER_CMD_STOP       2
#define PLAYER_CMD_PAUSE      3
#define PLAYER_CMD_RESUME     4
#define PLAYER_CMD_STATUS     5
#define PLAYER_CMD_SET_VOLUME 6

/* Playback state */

#define PLAYER_STATE_IDLE     0
#define PLAYER_STATE_PLAYING  1
#define PLAYER_STATE_PAUSED   2

/* Format identifiers */

#define PLAYER_FMT_UNKNOWN  0
#define PLAYER_FMT_WAV      1
#define PLAYER_FMT_MP2      2
#define PLAYER_FMT_MP3      3
#define PLAYER_FMT_OGG      4

/* Status structure (returned by STATUS) */

typedef struct
{
    uint8_t  state;           /* PLAYER_STATE_* */
    uint8_t  format;          /* PLAYER_FMT_* */
    uint8_t  volume;          /* 0–100 */
    uint8_t  _pad;
    uint32_t position_ms;     /* Current position in milliseconds */
    uint32_t duration_ms;     /* Total duration (0 if unknown) */
    uint32_t sample_rate;
    uint8_t  bits;
    uint8_t  channels;
} dobplayer_status_t;

/* Client-side interface */

typedef struct
{
    int  (*Play)(const char *path);
    int  (*Stop)(void);
    int  (*Pause)(void);
    int  (*Resume)(void);
    int  (*Status)(dobplayer_status_t *out);
    int  (*SetVolume)(int vol);
} dobplayer_interface_t;

extern dobplayer_interface_t dobplayer;

#endif /* MAINDOB_DOB_AUDIO_PLAYER_H */
