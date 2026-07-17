/* DobUITools — TextBox Clipboard Operations
 *
 * Copy/Cut/Paste/CopyAll for dob_textbox_t and dob_multitextbox_t.
 *
 * Split out of textbox.c so that programs which don't need clipboard
 * interop (e.g. simple dialogs) can link textbox.o alone without pulling in
 * cliptext.o and its DobConfig dependency. Programs that do want the
 * clipboard link textbox_clip.o and cliptext.o alongside.
 */

#include "textbox.h"
#include "textbox_priv.h"
#include "cliptext.h"
#include <stdlib.h>
#include <string.h>

/* Single-line */

bool dobtb_Copy(dob_textbox_t *tb)
{
    if (tb->masked) return false;            /* mai segreti in clipboard */
    if (!dobtb_HasSelection(tb)) return false;
    int a, b; dobtb_GetSelection(tb, &a, &b);
    dobui_cliptext_set(tb->text + a, b - a);
    return false;   /* no visual change */
}

bool dobtb_Cut(dob_textbox_t *tb)
{
    if (tb->masked)
    {
        /* Niente in clipboard, ma il delete della selezione resta un
         * edit legittimo. */
        if (!dobtb_HasSelection(tb)) return false;
        dobtb_priv_delete_selection(tb);
        return true;
    }
    if (!dobtb_HasSelection(tb)) return false;
    int a, b; dobtb_GetSelection(tb, &a, &b);
    dobui_cliptext_set(tb->text + a, b - a);
    dobtb_priv_delete_selection(tb);
    return true;
}

bool dobtb_Paste(dob_textbox_t *tb)
{
    int clip_size = dobui_cliptext_size();
    if (clip_size <= 0) return false;

    /* Single-line cap is small; the stack buffer always fits. */
    char buf[DOBTB_MAX_TEXT];
    int got = 0;
    if (dobui_cliptext_get(buf, sizeof(buf), &got) != 0 || got <= 0)
        return false;

    /* Sanitize for single-line: stop at first newline, strip other
     * control chars, convert '\t' to space. */
    int w = 0;
    for (int i = 0; i < got; i++)
    {
        char c = buf[i];
        if (c == '\n' || c == '\r') break;
        if      (c >= 32) buf[w++] = c;
        else if (c == '\t') buf[w++] = ' ';
    }
    if (w <= 0) return false;

    dobtb_priv_delete_selection(tb);
    int put = dobtb_priv_insert_range(tb->text, &tb->len, DOBTB_MAX_TEXT,
                                      tb->cursor, buf, w);
    if (put > 0)
    {
        tb->cursor  += put;
        tb->modified = true;
        dobtb_priv_ensure_visible(tb);
    }
    return put > 0;
}

bool dobtb_CopyAll(dob_textbox_t *tb)
{
    if (tb->masked) return false;            /* mai segreti in clipboard */
    if (tb->len == 0) return false;
    dobui_cliptext_set(tb->text, tb->len);
    return false;
}

/* Dispatch Ctrl+C/X/V for single-line. Returns true if consumed.
 * Kept here (not in textbox.c) so that programs linking textbox.o
 * without the clipboard add-on — simple dialogs, for instance — still build
 * cleanly. The focus manager calls this before dispatching OnKey. */
bool dobtb_OnKeyClip(dob_textbox_t *tb, uint8_t key)
{
    if (!tb->visible || !tb->enabled || !tb->focused) return false;
    if (key == 0x03) return dobtb_Copy(tb);
    if (key == 0x18) return dobtb_Cut(tb);
    if (key == 0x16) return dobtb_Paste(tb);
    return false;
}

/* Multi-line */

bool dobmt_Copy(dob_multitextbox_t *mt)
{
    if (!dobmt_HasSelection(mt)) return false;
    int a, b; dobmt_GetSelection(mt, &a, &b);
    dobui_cliptext_set(mt->text + a, b - a);
    return false;
}

bool dobmt_Cut(dob_multitextbox_t *mt)
{
    if (!dobmt_HasSelection(mt)) return false;
    int a, b; dobmt_GetSelection(mt, &a, &b);
    dobui_cliptext_set(mt->text + a, b - a);
    dobmt_priv_delete_selection(mt);
    return true;
}

bool dobmt_Paste(dob_multitextbox_t *mt)
{
    int clip_size = dobui_cliptext_size();
    if (clip_size <= 0) return false;

    char *scratch = (char *)malloc((uint32_t)clip_size + 1u);
    if (!scratch) return false;

    int got = 0;
    if (dobui_cliptext_get(scratch, clip_size + 1, &got) != 0 || got <= 0)
    {
        free(scratch);
        return false;
    }

    dobmt_priv_delete_selection(mt);

    if (!dobmt_priv_reserve(mt, mt->len + got + 1))
    {
        free(scratch);
        return false;
    }

    /* Single scan: detect newline presence while inserting. */
    bool has_nl = false;
    for (int i = 0; i < got; i++)
        if (scratch[i] == '\n') { has_nl = true; break; }

    int ins_at = mt->cursor;
    int put = dobtb_priv_insert_range(mt->text, &mt->len, mt->cap,
                                      ins_at, scratch, got);
    free(scratch);

    if (put > 0)
    {
        mt->cursor  += put;
        mt->modified = true;
        if (has_nl)
            dobmt_priv_mark_structural(mt);
        else
            dobmt_priv_lines_shift(mt, ins_at, put);
        dobmt_priv_ensure_visible(mt);
    }
    return put > 0;
}

bool dobmt_CopyAll(dob_multitextbox_t *mt)
{
    if (mt->len == 0) return false;
    dobui_cliptext_set(mt->text, mt->len);
    return false;
}

/* Dispatch Ctrl+C/X/V for multi-line. See dobtb_OnKeyClip. */
bool dobmt_OnKeyClip(dob_multitextbox_t *mt, uint8_t key)
{
    if (!mt->visible || !mt->enabled || !mt->focused) return false;
    if (key == 0x03) return dobmt_Copy(mt);
    if (key == 0x18) return dobmt_Cut(mt);
    if (key == 0x16) return dobmt_Paste(mt);
    return false;
}
