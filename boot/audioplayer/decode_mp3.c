/* decode_mp3.c — MPEG Audio Layer II/III decoder (stub)
 *
 * Full implementation will follow. For now, returns NULL to indicate
 * that MP2/MP3 playback is not yet supported.
 */

#include "decode_mp3.h"
#include <stdlib.h>
#include <unistd.h>

decoder_ctx_t *
mp3_open(int fd, audio_format_t *fmt)
{
    (void)fd;
    (void)fmt;
    debug_print("[player] MP3 decoder not yet implemented.\n");
    return NULL;
}

int
mp3_decode(decoder_ctx_t *ctx, int16_t *out, uint32_t max_frames)
{
    (void)ctx;
    (void)out;
    (void)max_frames;
    return -1;
}

void
mp3_close(decoder_ctx_t *ctx)
{
    (void)ctx;
}
