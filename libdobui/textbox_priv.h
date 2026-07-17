/* DobUITools — TextBox internal helpers (library-private)
 *
 * Utilities shared between textbox.c (core behaviour) and
 * textbox_clip.c (Copy/Cut/Paste/CopyAll). Not part of the public
 * API — application code must not include this header.
 *
 * The split exists so that programs which don't need clipboard
 * interop (e.g. simple dialogs) can link textbox.o alone, without pulling
 * in cliptext.o and its DobConfig dependency.
 */

#ifndef MAINDOB_DOBUITOOLS_TEXTBOX_PRIV_H
#define MAINDOB_DOBUITOOLS_TEXTBOX_PRIV_H

#include "textbox.h"

/* ---- raw buffer ops (operate on any char buffer) ---- */
void dobtb_priv_remove_range(char *buf, int *len, int a, int b);
int  dobtb_priv_insert_range(char *buf, int *len, int max,
                             int pos, const char *src, int n);

/* ---- single-line helpers ---- */
void dobtb_priv_ensure_visible(dob_textbox_t *tb);
bool dobtb_priv_delete_selection(dob_textbox_t *tb);

/* ---- multi-line helpers ---- */
bool dobmt_priv_reserve(dob_multitextbox_t *mt, int needed);
void dobmt_priv_ensure_visible(dob_multitextbox_t *mt);
bool dobmt_priv_delete_selection(dob_multitextbox_t *mt);

/* Line-cache: invalidate forces a rebuild on next query; shift
 * adjusts cached offsets when a mutation neither adds nor removes
 * '\n' — only shifts the buffer past `from_offset` by `delta`
 * bytes. Much cheaper than a full rebuild on large buffers. */
void dobmt_priv_lines_invalidate(dob_multitextbox_t *mt);
void dobmt_priv_lines_shift(dob_multitextbox_t *mt,
                            int from_offset, int delta);
int  dobmt_priv_line_start(dob_multitextbox_t *mt, int line);
int  dobmt_priv_line_of(dob_multitextbox_t *mt, int offset);
int  dobmt_priv_total_lines(dob_multitextbox_t *mt);

/* Force a line-cache rebuild on the next query. */
void dobmt_priv_mark_structural(dob_multitextbox_t *mt);

#endif
