/* decode_ogg.c — OGG Vorbis decoder (stub)
 *
 * Full implementation will follow. For now, returns NULL to indicate
 * that OGG Vorbis playback is not yet supported.
 */

#include "decode_ogg.h"
#include <stdlib.h>
#include <unistd.h>

decoder_ctx_t *
ogg_open(int fd, audio_format_t *fmt)
{
    (void)fd;
    (void)fmt;
    debug_print("[player] OGG Vorbis decoder not yet implemented.\n");
    return NULL;
}

int
ogg_decode(decoder_ctx_t *ctx, int16_t *out, uint32_t max_frames)
{
    (void)ctx;
    (void)out;
    (void)max_frames;
    return -1;
}

void
ogg_close(decoder_ctx_t *ctx)
{
    (void)ctx;
}
