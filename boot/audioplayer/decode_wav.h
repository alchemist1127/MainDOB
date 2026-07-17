/* decode_wav.h — WAV/RIFF decoder for DobAudioPlayer */

#ifndef MAINDOB_DECODE_WAV_H
#define MAINDOB_DECODE_WAV_H

#include <dob/types.h>

/* Audio format info extracted from file header */
typedef struct
{
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t channels;
    uint32_t data_size;         /* Total PCM data bytes */
    uint32_t data_offset;       /* File offset where PCM data begins */
} audio_format_t;

/* Decoder context — opaque, allocated by decoder_open */
typedef struct decoder_ctx decoder_ctx_t;

/* Open a WAV file. Returns context or NULL on error.
 * fd: file descriptor from DobFileSystem.
 * fmt: filled with format info on success. */
decoder_ctx_t *wav_open(int fd, audio_format_t *fmt);

/* Decode next chunk of PCM. Returns number of stereo frames written to out.
 * out must hold at least max_frames * 2 samples (stereo interleaved).
 * Returns 0 on EOF, -1 on error. */
int wav_decode(decoder_ctx_t *ctx, int16_t *out, uint32_t max_frames);

/* Close and free decoder context. */
void wav_close(decoder_ctx_t *ctx);

#endif /* MAINDOB_DECODE_WAV_H */
