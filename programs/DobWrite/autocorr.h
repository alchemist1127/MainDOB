/* autocorr.h -- global, in-memory autocorrect (substitution) list plus a
 * real-time matcher.  Pure logic: no document, no UI, no IPC.
 *
 * The editor dialog edits the list through add / remove; the typing path
 * queries autocorr_match() after each keystroke and performs the document
 * edit itself.  The list is global (it has nothing to do with any open
 * document).  Persistence to disk is a separate concern handled elsewhere.
 */
#ifndef DOBWRITE_AUTOCORR_H
#define DOBWRITE_AUTOCORR_H

#include <dob/types.h>
#include <stdbool.h>

#define AC_MAX_ENTRIES 128
#define AC_FROM_CAP    32      /* max bytes of a "from" pattern (UTF-8) + NUL */
#define AC_TO_CAP      48      /* max bytes of a replacement + NUL            */

#define AUTOCORR_PATH  "/DATA/autocorr.cfg"   /* on-disk config for the global list */

void        autocorr_init(void);                 /* seed the built-in defaults */

int         autocorr_count(void);
const char *autocorr_from(int i);                /* NULL if i out of range */
const char *autocorr_to(int i);
bool        autocorr_add(const char *from, const char *to);  /* false if full/empty/too long */
void        autocorr_remove(int i);

void        autocorr_save(const char *path);     /* persist the list to disk */
void        autocorr_load(const char *path);     /* replace the list from disk; no-op if absent/invalid */

/* Real-time match.  `pre` holds the `pre_len` bytes immediately before the
 * caret.  If that tail ends with a known "from" pattern sitting on a word
 * boundary (start of buffer, or preceded by a non-word byte), returns true
 * with *match_len = bytes to delete and *to = the replacement.  The longest
 * matching pattern wins.  Returns false otherwise. */
bool        autocorr_match(const char *pre, int pre_len,
                           int *match_len, const char **to);

#endif /* DOBWRITE_AUTOCORR_H */
