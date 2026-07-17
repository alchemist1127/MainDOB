/* DobUITools — Text Clipboard
 *
 * Inter-process UTF-8 text clipboard. A single shared memory region
 * holds the current payload; its id is published in dobconfig under
 * "clipboard_text". Every client maps the region once and from then
 * on reads and writes through the mapping without further IPC.
 *
 * SHM layout (little-endian):
 *      uint32 magic    — CLIPTEXT_MAGIC when live, CLIPTEXT_STALE
 *                        after the region has been superseded
 *      uint32 seq      — bumped on every Set/Clear, lets readers
 *                        detect changes without IPC
 *      uint32 used     — payload bytes currently in use
 *      uint32 cap      — payload bytes available after the header
 *      char   payload[cap]   — raw UTF-8, not NUL-terminated
 *
 * The payload is replaced in place when it fits in `cap`. Only when
 * a Set exceeds the current capacity do we create a new region,
 * publish its id, and mark the old region's magic as STALE so
 * readers still holding the old mapping know to re-attach.
 */

#ifndef MAINDOB_DOBUITOOLS_CLIPTEXT_H
#define MAINDOB_DOBUITOOLS_CLIPTEXT_H

#include <dob/types.h>

/* Replace clipboard contents. `len < 0` means strlen(text).
 * Returns 0 on success, -1 on error. An empty string (len == 0)
 * is a valid payload — use dobui_cliptext_clear to drop the
 * clipboard entirely. */
int  dobui_cliptext_set(const char *text, int len);

/* Copy current clipboard contents into `buf` (up to bufsize-1 bytes,
 * always NUL-terminated). Sets *out_len to bytes copied if non-NULL.
 * Returns 0 on success, -1 if the clipboard is empty or unavailable. */
int  dobui_cliptext_get(char *buf, int bufsize, int *out_len);

/* Size in bytes of the current payload, or -1 if the clipboard is
 * empty or unavailable. O(1) after the first call in a process —
 * a single memory read from the mapped region. */
int  dobui_cliptext_size(void);

/* Drop the clipboard (size becomes -1 for all readers). */
int  dobui_cliptext_clear(void);

#endif
