/* decode_ogg.h — OGG Vorbis decoder for DobAudioPlayer */

#ifndef MAINDOB_DECODE_OGG_H
#define MAINDOB_DECODE_OGG_H

#include "decode_wav.h"   /* audio_format_t, decoder_ctx_t */

/* Open an OGG Vorbis file. */
decoder_ctx_t *ogg_open(int fd, audio_format_t *fmt);

/* Decode next chunk. Returns stereo frames written, 0 on EOF, -1 on error. */
int ogg_decode(decoder_ctx_t *ctx, int16_t *out, uint32_t max_frames);

/* Close and free. */
void ogg_close(decoder_ctx_t *ctx);

#endif /* MAINDOB_DECODE_OGG_H */
