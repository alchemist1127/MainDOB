/* decode_mp3.h — MPEG Audio Layer II/III decoder for DobAudioPlayer */

#ifndef MAINDOB_DECODE_MP3_H
#define MAINDOB_DECODE_MP3_H

#include "decode_wav.h"   /* audio_format_t, decoder_ctx_t */

/* Open an MPEG audio file (MP2 or MP3).
 * Detects layer from frame header. */
decoder_ctx_t *mp3_open(int fd, audio_format_t *fmt);

/* Decode next chunk. Returns stereo frames written, 0 on EOF, -1 on error. */
int mp3_decode(decoder_ctx_t *ctx, int16_t *out, uint32_t max_frames);

/* Close and free. */
void mp3_close(decoder_ctx_t *ctx);

#endif /* MAINDOB_DECODE_MP3_H */
