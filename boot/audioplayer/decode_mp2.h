/* decode_mp2.h — MPEG-1 Audio Layer II decoder */

#ifndef MAINDOB_DECODE_MP2_H
#define MAINDOB_DECODE_MP2_H

#include "decode_wav.h"   /* audio_format_t, decoder_ctx_t */

/* Apre un file MP2. fmt riporta sample_rate/channels letti dal primo frame. */
decoder_ctx_t *mp2_open(int fd, audio_format_t *fmt);

/* Decodifica fino a max_frames stereo frames. Ritorna frames scritti,
 * 0 su EOF, -1 su errore. */
int mp2_decode(decoder_ctx_t *ctx, int16_t *out, uint32_t max_frames);

/* Libera il contesto. */
void mp2_close(decoder_ctx_t *ctx);

#endif /* MAINDOB_DECODE_MP2_H */
