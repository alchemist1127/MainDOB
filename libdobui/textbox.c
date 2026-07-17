/* DobUITools — TextBox Implementation
 *
 * Two widgets sharing most of their machinery:
 *   - dob_textbox_t      — single-line, fixed-capacity inline buffer
 *   - dob_multitextbox_t — multi-line, heap-grown buffer, line cache,
 *                          differential redraw
 *
 * Layout of this file:
 *   1. Shared helpers (buffer ops, word bounds, multi-click)
 *   2. Single-line widget
 *   3. Multi-line — line cache
 *   4. Multi-line — init / buffer management
 *   5. Multi-line — drawing (differential)
 *   6. Multi-line — event handling
 */

#include "textbox.h"
#include "textbox_priv.h"
#include "scrollbar.h"
#include <DobInterface.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

/* Shared helpers */

static inline int int_min(int a, int b) { return a < b ? a : b; }
static inline int int_max(int a, int b) { return a > b ? a : b; }
static inline int int_abs(int a)        { return a < 0 ? -a : a; }

static bool is_word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

/* Generic buffer ops. Work on any (buf, *len) pair — used by both
 * widgets and by textbox_clip.c. */

void dobtb_priv_remove_range(char *buf, int *len, int a, int b)
{
    if (a < 0) a = 0;
    if (b > *len) b = *len;
    if (a >= b) return;
    int n = b - a;
    for (int i = a; i < *len - n; i++)
        buf[i] = buf[i + n];
    *len -= n;
    buf[*len] = '\0';
}

int dobtb_priv_insert_range(char *buf, int *len, int max,
                            int pos, const char *src, int n)
{
    if (pos < 0) pos = 0;
    if (pos > *len) pos = *len;
    int room = (max - 1) - *len;
    if (room <= 0 || n <= 0) return 0;
    if (n > room) n = room;

    for (int i = *len - 1; i >= pos; i--)
        buf[i + n] = buf[i];
    for (int i = 0; i < n; i++)
        buf[pos + i] = src[i];
    *len += n;
    buf[*len] = '\0';
    return n;
}

/* Word/non-word bounds around `pos` in `buf`. When `stop_nl` is set,
 * the scan stops at '\n' on either side (used by multi-line). */
static void word_bounds(const char *buf, int len, int pos,
                        int *a_out, int *b_out, bool stop_nl)
{
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;

    bool word_mode = (pos < len) && is_word_char(buf[pos]);
    if (!word_mode && pos > 0 && is_word_char(buf[pos - 1]))
    {
        word_mode = true;
        pos--;
    }

    int a = pos, b = pos;
    while (a > 0
        && (!stop_nl || buf[a - 1] != '\n')
        && is_word_char(buf[a - 1]) == word_mode)
        a--;
    while (b < len
        && (!stop_nl || buf[b] != '\n')
        && is_word_char(buf[b]) == word_mode)
        b++;

    *a_out = a;
    *b_out = b;
}

/* Update multi-click state with a fresh click at (x, y, now) and
 * return the new click count (1, 2, 3, ...). */
static int multiclick_bump(dob_multiclick_t *mc, int x, int y, uint32_t now)
{
    bool near = (now - mc->last_ms) < DOBTB_MULTICLICK_MS
             && int_abs(x - mc->last_x) <= DOBTB_MULTICLICK_SLOP
             && int_abs(y - mc->last_y) <= DOBTB_MULTICLICK_SLOP;
    mc->count   = near ? mc->count + 1 : 1;
    mc->last_ms = now;
    mc->last_x  = x;
    mc->last_y  = y;
    return mc->count;
}

/* Single-Line TextBox */

void dobtb_Init(dob_textbox_t *tb, uint32_t win_id,
                int x, int y, int w, int h)
{
    memset(tb, 0, sizeof(*tb));
    tb->win_id           = win_id;
    tb->x                = x;
    tb->y                = y;
    tb->w                = w;
    tb->h                = (h > 0) ? h : DOBTB_DEFAULT_H;
    tb->sel_anchor       = -1;
    tb->visible          = true;
    tb->enabled          = true;
    tb->anchor           = DOB_ANCHOR_CENTER_LEFT;
    tb->col_bg           = DOBTB_COL_BG;
    tb->col_text         = DOBTB_COL_TEXT;
    tb->col_cursor       = DOBTB_COL_CURSOR;
    tb->col_border       = DOBTB_COL_BORDER;
    tb->col_border_focus = DOBTB_COL_BORDER_FOCUS;
    tb->col_sel_bg       = DOBTB_COL_SEL_BG;
    tb->col_sel_text     = DOBTB_COL_SEL_TEXT;
    if (dobfocus_auto_register) dobfocus_auto_register(tb, DOB_CTRL_TEXTBOX);
}

void dobtb_SetText(dob_textbox_t *tb, const char *text)
{
    if (!text) text = "";
    int n = (int)strlen(text);
    if (n >= DOBTB_MAX_TEXT) n = DOBTB_MAX_TEXT - 1;
    memcpy(tb->text, text, (uint32_t)n);
    tb->text[n]    = '\0';
    tb->len        = n;
    tb->cursor     = n;
    tb->scroll_x   = 0;
    tb->sel_anchor = -1;
    tb->selecting  = false;
    tb->modified   = false;
}

const char *dobtb_GetText(dob_textbox_t *tb)  { return tb->text; }
void dobtb_SetFocus(dob_textbox_t *tb, bool f)
{
    tb->focused = f;
    if (!f) tb->selecting = false;
}
void dobtb_ClearModified(dob_textbox_t *tb)    { tb->modified = false; }

bool dobtb_HasSelection(const dob_textbox_t *tb)
{
    return tb->sel_anchor >= 0 && tb->sel_anchor != tb->cursor;
}

void dobtb_GetSelection(const dob_textbox_t *tb, int *a, int *b)
{
    int lo = int_min(tb->sel_anchor, tb->cursor);
    int hi = int_max(tb->sel_anchor, tb->cursor);
    if (a) *a = lo;
    if (b) *b = hi;
}

void dobtb_ClearSelection(dob_textbox_t *tb)
{
    tb->sel_anchor = -1;
}

void dobtb_priv_ensure_visible(dob_textbox_t *tb)
{
    int vis = (tb->w - 2 * DOBTB_PAD) / DOBTB_FONT_W;
    if (vis < 1) vis = 1;

    if (tb->cursor < tb->scroll_x)
        tb->scroll_x = tb->cursor;
    else if (tb->cursor >= tb->scroll_x + vis)
        tb->scroll_x = tb->cursor - vis + 1;
    if (tb->scroll_x < 0) tb->scroll_x = 0;
}

bool dobtb_priv_delete_selection(dob_textbox_t *tb)
{
    if (!dobtb_HasSelection(tb)) return false;
    int a, b; dobtb_GetSelection(tb, &a, &b);
    dobtb_priv_remove_range(tb->text, &tb->len, a, b);
    tb->cursor     = a;
    tb->sel_anchor = -1;
    tb->modified   = true;
    dobtb_priv_ensure_visible(tb);
    return true;
}

static int tb_hit_test(dob_textbox_t *tb, int x)
{
    int char_x = (x - tb->x - DOBTB_PAD + DOBTB_FONT_W / 2)
               / DOBTB_FONT_W + tb->scroll_x;
    if (char_x < 0)        char_x = 0;
    if (char_x > tb->len)  char_x = tb->len;
    return char_x;
}

/* Collapse selection to `side`: -1 = low end, +1 = high end. */
static void tb_collapse_selection(dob_textbox_t *tb, int side)
{
    if (!dobtb_HasSelection(tb)) return;
    int a, b; dobtb_GetSelection(tb, &a, &b);
    tb->cursor     = (side < 0) ? a : b;
    tb->sel_anchor = -1;
}

bool dobtb_OnKey(dob_textbox_t *tb, uint8_t key)
{
    if (!tb->visible || !tb->enabled || !tb->focused) return false;

    /* Ctrl+A (SelectAll) is handled here because it doesn't depend
     * on clipboard support. Ctrl+C/X/V live in textbox_clip.c and
     * are dispatched by the focus manager when it sees that the
     * clipboard add-on is linked in. */
    if (key == 0x01) { return dobtb_SelectAll(tb); }

    if (key == DOBTB_KEY_LEFT)
    {
        if (dobtb_HasSelection(tb))
            tb_collapse_selection(tb, -1);
        else if (tb->cursor > 0)
            tb->cursor--;
        dobtb_priv_ensure_visible(tb);
        return true;
    }
    if (key == DOBTB_KEY_RIGHT)
    {
        if (dobtb_HasSelection(tb))
            tb_collapse_selection(tb, +1);
        else if (tb->cursor < tb->len)
            tb->cursor++;
        dobtb_priv_ensure_visible(tb);
        return true;
    }
    if (key == DOBTB_KEY_HOME)
    {
        tb->cursor     = 0;
        tb->sel_anchor = -1;
        dobtb_priv_ensure_visible(tb);
        return true;
    }
    if (key == DOBTB_KEY_END)
    {
        tb->cursor     = tb->len;
        tb->sel_anchor = -1;
        dobtb_priv_ensure_visible(tb);
        return true;
    }
    if (key == '\b')
    {
        if (!dobtb_priv_delete_selection(tb) && tb->cursor > 0)
        {
            dobtb_priv_remove_range(tb->text, &tb->len,
                                    tb->cursor - 1, tb->cursor);
            tb->cursor--;
            tb->modified = true;
            dobtb_priv_ensure_visible(tb);
        }
        return true;
    }
    if (key == DOBTB_KEY_DELETE)
    {
        if (!dobtb_priv_delete_selection(tb) && tb->cursor < tb->len)
        {
            dobtb_priv_remove_range(tb->text, &tb->len,
                                    tb->cursor, tb->cursor + 1);
            tb->modified = true;
        }
        return true;
    }
    if (key == '\n' || key == 27)
        return false;
    if (dobtb_is_text_char(key))
    {
        dobtb_priv_delete_selection(tb);
        char c = (char)key;
        int put = dobtb_priv_insert_range(tb->text, &tb->len,
                                          DOBTB_MAX_TEXT,
                                          tb->cursor, &c, 1);
        if (put > 0)
        {
            tb->cursor++;
            tb->modified = true;
            dobtb_priv_ensure_visible(tb);
        }
        return true;
    }
    return false;
}

bool dobtb_OnClick(dob_textbox_t *tb, int x, int y)
{
    if (!tb->visible || !tb->enabled) return false;
    if (x < tb->x || x >= tb->x + tb->w
        || y < tb->y || y >= tb->y + tb->h) return false;

    int hit = tb_hit_test(tb, x);
    tb->cursor     = hit;
    tb->sel_anchor = -1;      /* No selection on a plain click. The anchor
                                 is set lazily on the first drag move (see
                                 dobtb_OnDrag), so a click whose release is
                                 never delivered cannot leave a dangling
                                 anchor that the next keystroke turns into a
                                 spurious 1-char selection. */
    tb->selecting  = true;
    dobtb_priv_ensure_visible(tb);
    return true;
}

bool dobtb_OnDrag(dob_textbox_t *tb, int x, int y)
{
    if (!tb->visible || !tb->enabled || !tb->selecting) return false;
    (void)y;
    int hit = tb_hit_test(tb, x);
    if (hit == tb->cursor) return false;
    if (tb->sel_anchor < 0) tb->sel_anchor = tb->cursor; /* anchor at drag origin */
    tb->cursor = hit;
    dobtb_priv_ensure_visible(tb);
    return true;
}

bool dobtb_OnRelease(dob_textbox_t *tb)
{
    if (!tb->selecting) return false;
    tb->selecting = false;
    /* If the drag ended in place, collapse the dangling anchor. */
    if (tb->sel_anchor == tb->cursor)
        tb->sel_anchor = -1;
    return false;
}

bool dobtb_OnDblClick(dob_textbox_t *tb, int x, int y)
{
    if (!tb->visible || !tb->enabled) return false;
    if (x < tb->x || x >= tb->x + tb->w
        || y < tb->y || y >= tb->y + tb->h) return false;

    int count = multiclick_bump(&tb->mc, x, y, clock_ms());
    int hit = tb_hit_test(tb, x);
    int a, b;
    bool keep_scroll = false;

    if (count == 1)
    {
        word_bounds(tb->text, tb->len, hit, &a, &b, false);
    }
    else
    {
        a = 0;
        b = tb->len;
        keep_scroll = true;     /* don't jump scroll_x on select-all */
        if (count > 2) tb->mc.count = 2;
    }

    tb->sel_anchor = a;
    tb->cursor     = b;
    if (!keep_scroll) dobtb_priv_ensure_visible(tb);
    return true;
}

bool dobtb_SelectAll(dob_textbox_t *tb)
{
    if (tb->len == 0) return false;
    tb->sel_anchor = 0;
    tb->cursor     = tb->len;
    /* Don't change scroll_x — "select all" should keep the view. */
    return true;
}

bool dobtb_Clear(dob_textbox_t *tb)
{
    if (tb->len == 0 && !dobtb_HasSelection(tb)) return false;
    tb->text[0]    = '\0';
    tb->len        = 0;
    tb->cursor     = 0;
    tb->scroll_x   = 0;
    tb->sel_anchor = -1;
    tb->selecting  = false;
    tb->modified   = true;
    return true;
}

void dobtb_Draw(dob_textbox_t *tb)
{
    if (!tb->visible) return;

    uint32_t border = tb->focused ? tb->col_border_focus : tb->col_border;
    dobui_FillRect(tb->win_id, tb->x, tb->y, tb->w, tb->h, tb->col_bg);
    dobui_DrawRect(tb->win_id, tb->x, tb->y, tb->w, tb->h, border);

    int vis_chars = (tb->w - 2 * DOBTB_PAD) / DOBTB_FONT_W;
    if (vis_chars < 1) vis_chars = 1;

    char visible[256];
    int count = 0;
    for (int i = tb->scroll_x;
         i < tb->len && count < vis_chars && count < 255; i++)
        visible[count++] = tb->text[i];
    visible[count] = '\0';

    /* Modalita' password: si maschera QUI, sulla sola fetta visibile,
     * cosi' tutta la matematica a valle (selezione, cursore, anchor)
     * resta identica -- il font e' a cella fissa, un '*' occupa
     * esattamente lo spazio del carattere vero. */
    if (tb->masked)
        for (int i = 0; i < count; i++)
            visible[i] = '*';

    /* Horizontal anchor applies only when the full text fits. */
    int tw = count * DOBTB_FONT_W;
    int tx, ty;
    if (tb->scroll_x == 0 && tb->len <= vis_chars)
    {
        dob_anchor_pos(tb->anchor, tb->x, tb->y, tb->w, tb->h,
                       tw, DOBTB_FONT_H, DOBTB_PAD, &tx, &ty);
    }
    else
    {
        int dummy;
        tx = tb->x + DOBTB_PAD;
        dob_anchor_pos(tb->anchor, tb->x, tb->y, tb->w, tb->h,
                       tw, DOBTB_FONT_H, DOBTB_PAD, &dummy, &ty);
    }

    /* Selection band, if any: compute the slice in visible coords. */
    int sel_lo = -1, sel_hi = -1;
    if (dobtb_HasSelection(tb))
    {
        int a, b; dobtb_GetSelection(tb, &a, &b);
        sel_lo = int_max(0, a - tb->scroll_x);
        sel_hi = int_min(count, b - tb->scroll_x);
        if (sel_lo >= sel_hi) { sel_lo = sel_hi = -1; }
    }

    if (sel_lo < 0)
    {
        if (count > 0)
            dobui_DrawTextFixed(tb->win_id, tx, ty, visible,
                           tb->col_text, tb->col_bg);
    }
    else
    {
        /* Paint the three segments: before / selected / after. */
        int sx = tx + sel_lo * DOBTB_FONT_W;
        int sw = (sel_hi - sel_lo) * DOBTB_FONT_W;
        dobui_FillRect(tb->win_id, sx, tb->y + 2, sw, tb->h - 4,
                       tb->col_sel_bg);

        char seg[256];
        if (sel_lo > 0)
        {
            memcpy(seg, visible, (uint32_t)sel_lo);
            seg[sel_lo] = '\0';
            dobui_DrawTextFixed(tb->win_id, tx, ty, seg,
                           tb->col_text, tb->col_bg);
        }
        int mid_n = sel_hi - sel_lo;
        memcpy(seg, visible + sel_lo, (uint32_t)mid_n);
        seg[mid_n] = '\0';
        dobui_DrawTextFixed(tb->win_id, sx, ty, seg,
                       tb->col_sel_text, tb->col_sel_bg);
        if (sel_hi < count)
        {
            int n = count - sel_hi;
            memcpy(seg, visible + sel_hi, (uint32_t)n);
            seg[n] = '\0';
            int rx = tx + sel_hi * DOBTB_FONT_W;
            dobui_DrawTextFixed(tb->win_id, rx, ty, seg,
                           tb->col_text, tb->col_bg);
        }
    }

    /* Cursor: only when focused and no selection. */
    if (tb->focused && !dobtb_HasSelection(tb))
    {
        int cx = tx + (tb->cursor - tb->scroll_x) * DOBTB_FONT_W;
        dobui_FillRect(tb->win_id, cx, tb->y + 2, 2, tb->h - 4,
                       tb->col_cursor);
    }
}

/* Multi-Line — Line cache */

static void mt_rebuild_lines(dob_multitextbox_t *mt)
{
    int n = 1;
    for (int i = 0; i < mt->len; i++)
        if (mt->text[i] == '\n') n++;

    if (mt->line_cap < n)
    {
        int new_cap = mt->line_cap > 0 ? mt->line_cap : 64;
        while (new_cap < n)
        {
            int d = new_cap * 2;
            if (d <= new_cap) { new_cap = n; break; }
            new_cap = d;
        }
        int *nb = (int *)realloc(mt->line_offsets,
                                 (uint32_t)new_cap * sizeof(int));
        if (!nb) return;      /* leave dirty; linear fallback kicks in */
        mt->line_offsets = nb;
        mt->line_cap     = new_cap;
    }

    mt->line_offsets[0] = 0;
    int line = 1;
    for (int i = 0; i < mt->len && line < n; i++)
        if (mt->text[i] == '\n')
            mt->line_offsets[line++] = i + 1;
    mt->total_lines = n;
    mt->lines_dirty = false;
}

static inline void mt_ensure_lines(dob_multitextbox_t *mt)
{
    if (mt->lines_dirty) mt_rebuild_lines(mt);
}

void dobmt_priv_lines_invalidate(dob_multitextbox_t *mt)
{
    mt->lines_dirty = true;
    mt->wrap_dirty  = true;
}

/* Available text width in pixels — same calculation mt_draw_line and
 * mt_hit_test do, kept in one place. */
static inline int mt_text_w(const dob_multitextbox_t *mt)
{
    /* The scrollbar gutter is reserved unconditionally so the wrap
     * width never depends on whether the bar happens to be visible.
     * That keeps text from reflowing the instant it overflows and
     * avoids the wrap<->overflow circular dependency. */
    int tw = mt->w - DOBMT_SCROLLBAR_W;
    if (mt->show_line_numbers) tw -= DOBMT_LINENO_W + DOBMT_LEFT_PAD;
    if (tw < DOBTB_FONT_W) tw = DOBTB_FONT_W;
    return tw;
}

/* Rebuild wrap_offsets[] given the current text and widget width.
 * Called lazily; no-op when word_wrap is off. */
static void mt_rebuild_wrap(dob_multitextbox_t *mt)
{
    mt_ensure_lines(mt);

    int max_cols = mt_text_w(mt) / DOBTB_FONT_W;
    if (max_cols < 1) max_cols = 1;

    /* Worst case: text_len / max_cols + one entry per logical line
     * + a small margin for empty logical lines. */
    int max_v = mt->len / max_cols + mt->total_lines + 8;
    if (mt->wrap_cap < max_v)
    {
        int new_cap = mt->wrap_cap > 0 ? mt->wrap_cap : 64;
        while (new_cap < max_v)
        {
            int d = new_cap * 2;
            if (d <= new_cap) { new_cap = max_v; break; }
            new_cap = d;
        }
        int *nb = (int *)realloc(mt->wrap_offsets,
                                 (uint32_t)new_cap * sizeof(int));
        if (!nb) return;       /* leave dirty; next call retries */
        mt->wrap_offsets = nb;
        mt->wrap_cap     = new_cap;
    }

    int v = 0;
    for (int li = 0; li < mt->total_lines; li++)
    {
        int log_start = mt->line_offsets[li];
        int log_end   = (li + 1 < mt->total_lines)
                      ? mt->line_offsets[li + 1] - 1    /* exclude '\n' */
                      : mt->len;
        int run = log_end - log_start;
        if (run <= 0)
        {
            mt->wrap_offsets[v++] = log_start;
        }
        else
        {
            int chunks = (run + max_cols - 1) / max_cols;
            for (int c = 0; c < chunks; c++)
                mt->wrap_offsets[v++] = log_start + c * max_cols;
        }
    }
    mt->total_wrap_lines  = v;
    mt->wrap_built_for_w  = mt->w;
    mt->wrap_dirty        = false;
}

static inline void mt_ensure_wrap(dob_multitextbox_t *mt)
{
    if (!mt->word_wrap) return;
    if (mt->wrap_dirty || mt->wrap_built_for_w != mt->w)
        mt_rebuild_wrap(mt);
}

/* Visual-line helpers — when wrap is off, delegate to the logical
 * ones so call sites that always go through these keep their
 * pre-wrap semantics. */
static int mt_vline_start(dob_multitextbox_t *mt, int vline)
{
    if (!mt->word_wrap) return dobmt_priv_line_start(mt, vline);
    mt_ensure_wrap(mt);
    if (vline < 0) return 0;
    if (vline >= mt->total_wrap_lines) return mt->len;
    return mt->wrap_offsets[vline];
}

static int mt_total_vlines(dob_multitextbox_t *mt)
{
    if (!mt->word_wrap) return dobmt_priv_total_lines(mt);
    mt_ensure_wrap(mt);
    return mt->total_wrap_lines;
}

static int mt_vline_of(dob_multitextbox_t *mt, int offset)
{
    if (!mt->word_wrap) return dobmt_priv_line_of(mt, offset);
    mt_ensure_wrap(mt);
    if (mt->total_wrap_lines <= 0) return 0;
    if (offset <= 0) return 0;
    if (offset >= mt->len) return mt->total_wrap_lines - 1;
    int lo = 0, hi = mt->total_wrap_lines - 1;
    while (lo < hi)
    {
        int m = (lo + hi + 1) / 2;
        if (mt->wrap_offsets[m] <= offset) lo = m;
        else                                hi = m - 1;
    }
    return lo;
}

/* Visible-character count of a visual line: strips the trailing '\n'
 * if present (line ends with logical-line break, not a wrap point). */
static int mt_vline_visible_len(dob_multitextbox_t *mt, int vline)
{
    int total = mt_total_vlines(mt);
    if (vline < 0 || vline >= total) return 0;
    int s = mt_vline_start(mt, vline);
    int e = (vline + 1 < total) ? mt_vline_start(mt, vline + 1) : mt->len;
    if (e > s && mt->text[e - 1] == '\n') e--;
    return e - s;
}

/* Shift the tail of the line-offset cache by `delta` bytes when a
 * mutation has moved buffer content past `from_offset` without
 * adding or removing any '\n'. O(log N + shifted). */
void dobmt_priv_lines_shift(dob_multitextbox_t *mt,
                            int from_offset, int delta)
{
    if (mt->lines_dirty || delta == 0) return;

    /* Binary search the first offset strictly greater than from_offset. */
    int lo = 0, hi = mt->total_lines;
    while (lo < hi)
    {
        int mid = (lo + hi) >> 1;
        if (mt->line_offsets[mid] <= from_offset) lo = mid + 1;
        else                                      hi = mid;
    }
    for (int i = lo; i < mt->total_lines; i++)
        mt->line_offsets[i] += delta;

    /* A within-logical-line edit changes where wrap points fall; we
     * don't try to incrementally fix wrap_offsets[] (too easy to
     * miss an edge case) — invalidate so the next ensure rebuilds. */
    mt->wrap_dirty = true;
}

int dobmt_priv_line_start(dob_multitextbox_t *mt, int line)
{
    mt_ensure_lines(mt);
    if (mt->lines_dirty)
    {
        int cur = 0;
        for (int i = 0; i < mt->len; i++)
        {
            if (cur == line) return i;
            if (mt->text[i] == '\n') cur++;
        }
        return mt->len;
    }
    if (line < 0) return 0;
    if (line >= mt->total_lines) return mt->len;
    return mt->line_offsets[line];
}

int dobmt_priv_line_of(dob_multitextbox_t *mt, int offset)
{
    if (offset <= 0) return 0;
    mt_ensure_lines(mt);
    if (mt->lines_dirty)
    {
        int line = 0;
        for (int i = 0; i < offset && i < mt->len; i++)
            if (mt->text[i] == '\n') line++;
        return line;
    }
    int lo = 0, hi = mt->total_lines - 1, r = 0;
    while (lo <= hi)
    {
        int mid = (lo + hi) >> 1;
        if (mt->line_offsets[mid] <= offset) { r = mid; lo = mid + 1; }
        else                                   hi = mid - 1;
    }
    return r;
}

int dobmt_priv_total_lines(dob_multitextbox_t *mt)
{
    mt_ensure_lines(mt);
    if (mt->lines_dirty)
    {
        int n = 1;
        for (int i = 0; i < mt->len; i++)
            if (mt->text[i] == '\n') n++;
        return n;
    }
    return mt->total_lines;
}

int dobmt_CursorLine(dob_multitextbox_t *mt)
{
    return dobmt_priv_line_of(mt, mt->cursor);
}

int dobmt_CursorColumn(dob_multitextbox_t *mt)
{
    int ls = dobmt_priv_line_start(mt, dobmt_CursorLine(mt));
    return mt->cursor - ls;
}

/* Multi-Line — Line cache bookkeeping */

void dobmt_priv_mark_structural(dob_multitextbox_t *mt)
{
    mt->lines_dirty = true;
    mt->wrap_dirty  = true;
}

static int mt_visible_lines(dob_multitextbox_t *mt)
{
    int n = mt->h / DOBMT_LINE_H;
    return (n < 1) ? 1 : n;
}

/* Vertical scrollbar geometry (visual lines). False when everything
 * fits and no bar is shown. Fills *g (thumb along Y) and the bar's
 * left edge *sb_x. */
static bool mt_sb_geom(dob_multitextbox_t *mt, dob_scroll1d_t *g, int *sb_x)
{
    int vis   = mt_visible_lines(mt);
    int total = mt_total_vlines(mt);
    if (total <= vis) return false;

    *sb_x = mt->x + mt->w - DOBMT_SCROLLBAR_W;
    *g = dob_scroll1d(mt->y, mt->h, total, vis, mt->scroll_line);
    return g->max_scroll > 0;
}

void dobmt_priv_ensure_visible(dob_multitextbox_t *mt)
{
    int cv    = mt_vline_of(mt, mt->cursor);
    int vis   = mt_visible_lines(mt);
    int total = mt_total_vlines(mt);

    if (cv < mt->scroll_line)
        mt->scroll_line = cv;
    else if (cv >= mt->scroll_line + vis)
        mt->scroll_line = cv - vis + 1;
    if (mt->scroll_line < 0) mt->scroll_line = 0;

    /* Clamp upper bound — keeps the widget from showing past-EOB
     * blank rows when content shrinks (delete, wrap toggle). */
    int max_scroll = total - vis;
    if (max_scroll < 0) max_scroll = 0;
    if (mt->scroll_line > max_scroll) mt->scroll_line = max_scroll;
}

/* Multi-Line — Init / buffer management */

bool dobmt_priv_reserve(dob_multitextbox_t *mt, int needed)
{
    if (needed <= 0) needed = 1;
    if (mt->cap >= needed) return true;

    int new_cap = mt->cap > 0 ? mt->cap : (int)DOBMT_INITIAL_CAP;
    while (new_cap < needed)
    {
        int d = new_cap * 2;
        if (d <= new_cap) { new_cap = needed; break; }
        new_cap = d;
    }
    char *nb = (char *)realloc(mt->text, (uint32_t)new_cap);
    if (!nb) return false;
    if (mt->text == NULL) nb[0] = '\0';
    mt->text = nb;
    mt->cap  = new_cap;
    return true;
}

void dobmt_Init(dob_multitextbox_t *mt, uint32_t win_id,
                int x, int y, int w, int h)
{
    memset(mt, 0, sizeof(*mt));
    mt->win_id            = win_id;
    mt->x                 = x;
    mt->y                 = y;
    mt->w                 = w;
    mt->h                 = h;
    mt->show_line_numbers = true;
    mt->sel_anchor        = -1;
    mt->lines_dirty       = true;
    mt->wrap_dirty        = true;
    mt->word_wrap         = true;
    mt->visible           = true;
    mt->enabled           = true;
    mt->col_bg            = DOBTB_COL_BG;
    mt->col_text          = DOBTB_COL_TEXT;
    mt->col_cursor        = DOBTB_COL_CURSOR;
    mt->col_border        = DOBTB_COL_BORDER;
    mt->col_border_focus  = DOBTB_COL_BORDER_FOCUS;
    mt->col_lineno_bg     = DOBTB_COL_LINENO_BG;
    mt->col_lineno_fg     = DOBTB_COL_LINENO_FG;
    mt->col_sel_bg        = DOBTB_COL_SEL_BG;
    mt->col_sel_text      = DOBTB_COL_SEL_TEXT;
    mt->col_scrollbar     = DOBMT_COL_SCROLL;
    if (dobfocus_auto_register) dobfocus_auto_register(mt, DOB_CTRL_MULTITEXTBOX);
}

void dobmt_InitFill(dob_multitextbox_t *mt, uint32_t win_id,
                    int x, int y)
{
    dobmt_Init(mt, win_id, x, y, 100, 100);
    mt->fill_mode = true;
}

void dobmt_SetSize(dob_multitextbox_t *mt, int win_w, int win_h)
{
    if (!mt->fill_mode) return;
    int nw = win_w - mt->x;
    int nh = win_h - mt->y;
    if (nw < 10)            nw = 10;
    if (nh < DOBMT_LINE_H)  nh = DOBMT_LINE_H;
    if (nw != mt->w || nh != mt->h)
    {
        mt->w = nw;
        mt->h = nh;
        dobmt_priv_mark_structural(mt);
    }
}

void dobmt_Free(dob_multitextbox_t *mt)
{
    if (dobfocus_auto_unregister) dobfocus_auto_unregister(mt);
    free(mt->text);
    free(mt->line_offsets);
    free(mt->wrap_offsets);
    mt->text             = NULL;
    mt->cap              = 0;
    mt->len              = 0;
    mt->cursor           = 0;
    mt->sel_anchor       = -1;
    mt->selecting        = false;
    mt->line_offsets     = NULL;
    mt->line_cap         = 0;
    mt->total_lines      = 0;
    mt->lines_dirty      = true;
    mt->wrap_offsets     = NULL;
    mt->wrap_cap         = 0;
    mt->total_wrap_lines = 0;
    mt->wrap_dirty       = true;
}

void dobmt_SetText(dob_multitextbox_t *mt, const char *text, int len)
{
    if (text == NULL) len = 0;
    else if (len < 0) len = (int)strlen(text);
    if (len < 0) len = 0;

    if (len > 0)
    {
        if (!dobmt_priv_reserve(mt, len + 1))
        {
            if (mt->cap <= 0) return;
            if (len > mt->cap - 1) len = mt->cap - 1;
        }
        memcpy(mt->text, text, (uint32_t)len);
        mt->text[len] = '\0';
    }
    else if (mt->text)
    {
        mt->text[0] = '\0';
    }

    mt->len         = len;
    mt->cursor      = 0;
    mt->scroll_line = 0;
    mt->sel_anchor  = -1;
    mt->selecting   = false;
    mt->modified    = false;
    dobmt_priv_mark_structural(mt);
}

const char *dobmt_GetText(dob_multitextbox_t *mt, int *out_len)
{
    if (out_len) *out_len = mt->len;
    return mt->text ? mt->text : "";
}

void dobmt_SetWordWrap(dob_multitextbox_t *mt, bool on)
{
    if (mt->word_wrap == on) return;
    mt->word_wrap   = on;
    mt->wrap_dirty  = true;
    mt->scroll_line = 0;
    dobmt_priv_ensure_visible(mt);   /* bring cursor back into view */
}

bool dobmt_GetWordWrap(const dob_multitextbox_t *mt)
{
    return mt->word_wrap;
}

void dobmt_SetFocus(dob_multitextbox_t *mt, bool f)
{
    if (mt->focused == f) return;
    mt->focused = f;
    if (!f) mt->selecting = false;
    dobmt_priv_mark_structural(mt);
}

bool dobmt_HasSelection(const dob_multitextbox_t *mt)
{
    return mt->sel_anchor >= 0 && mt->sel_anchor != mt->cursor;
}

void dobmt_GetSelection(const dob_multitextbox_t *mt, int *a, int *b)
{
    int lo = int_min(mt->sel_anchor, mt->cursor);
    int hi = int_max(mt->sel_anchor, mt->cursor);
    if (a) *a = lo;
    if (b) *b = hi;
}

void dobmt_ClearSelection(dob_multitextbox_t *mt)
{
    mt->sel_anchor = -1;
}

bool dobmt_priv_delete_selection(dob_multitextbox_t *mt)
{
    if (!dobmt_HasSelection(mt)) return false;
    int a, b; dobmt_GetSelection(mt, &a, &b);

    bool has_nl = false;
    for (int i = a; i < b; i++)
        if (mt->text[i] == '\n') { has_nl = true; break; }

    dobtb_priv_remove_range(mt->text, &mt->len, a, b);
    mt->cursor     = a;
    mt->sel_anchor = -1;
    mt->modified   = true;

    if (has_nl)
        dobmt_priv_mark_structural(mt);
    else
        dobmt_priv_lines_shift(mt, a, -(b - a));
    dobmt_priv_ensure_visible(mt);
    return true;
}

/* Multi-Line — Drawing */

/* Draw a single screen row.  `line_num` is a visual-line index;
 * when word_wrap is off it's identical to a logical line.  Returns
 * the buffer offset after the row, or -1 if the row is past EOB. */
static int mt_draw_line(dob_multitextbox_t *mt, int screen_line,
                        bool draw_cursor, int sel_a, int sel_b)
{
    int line_num = mt->scroll_line + screen_line;
    int row_y    = mt->y + screen_line * DOBMT_LINE_H;
    int text_x   = mt->x;
    int text_w   = mt->w - DOBMT_SCROLLBAR_W;   /* reserve the bar gutter */

    dobui_FillRect(mt->win_id, mt->x, row_y, mt->w, DOBMT_LINE_H, mt->col_bg);

    int offset    = mt_vline_start(mt, line_num);
    int total_v   = mt_total_vlines(mt);
    bool past_end = (line_num >= total_v && offset >= mt->len
                     && !(line_num == 0 && mt->len == 0));

    if (mt->show_line_numbers)
    {
        /* In wrap mode, only the visual row that starts a logical
         * line carries the line number; continuation rows show only
         * the gutter background. */
        int logical_line_for_lineno = -1;
        if (!past_end)
        {
            int log_line = dobmt_priv_line_of(mt, offset);
            int log_start = dobmt_priv_line_start(mt, log_line);
            if (log_start == offset)
                logical_line_for_lineno = log_line;
        }
        dobui_FillRect(mt->win_id, mt->x, row_y,
                       DOBMT_LINENO_W, DOBMT_LINE_H, mt->col_lineno_bg);
        if (logical_line_for_lineno >= 0)
        {
            char lnum[8];
            int n = logical_line_for_lineno + 1;
            lnum[0] = (n >= 100) ? (char)('0' + (n / 100) % 10) : ' ';
            lnum[1] = (n >= 10)  ? (char)('0' + (n / 10) % 10)  : ' ';
            lnum[2] = (char)('0' + n % 10);
            lnum[3] = '\0';
            dobui_DrawTextFixed(mt->win_id, mt->x + 2, row_y + 1, lnum,
                           mt->col_lineno_fg, mt->col_lineno_bg);
        }
        text_x += DOBMT_LINENO_W + DOBMT_LEFT_PAD;
        text_w -= DOBMT_LINENO_W + DOBMT_LEFT_PAD;
    }

    if (past_end) return -1;

    /* How many chars belong to this visual row?  In wrap mode it's
     * the wrap chunk size; in non-wrap mode it's "until '\n' or
     * widget edge" — covered uniformly by mt_vline_visible_len. */
    int row_chars = mt_vline_visible_len(mt, line_num);
    int max_cols  = text_w / DOBTB_FONT_W;
    if (max_cols < 1)   max_cols = 1;
    if (max_cols > 255) max_cols = 255;
    if (row_chars > max_cols) row_chars = max_cols;

    char linebuf[256];
    int  lpos = 0;
    int  cursor_col = -1;
    for (int k = 0; k < row_chars; k++)
    {
        int i = offset + k;
        if (i == mt->cursor) cursor_col = lpos;
        /* Read as unsigned: an accented byte (>=0x80) is negative as a
         * signed char and would fail a ">= 32" test, blanking the
         * glyph. Control bytes (0..31, 127) still collapse to space. */
        uint8_t ch = (uint8_t)mt->text[i];
        linebuf[lpos++] = (ch >= 32 && ch != 127) ? mt->text[i] : ' ';
    }
    linebuf[lpos] = '\0';

    /* Cursor positioned at end of this visual row.  In wrap mode the
     * cursor sits *just before* the wrap point on the row that
     * contains it, so only show end-of-row cursor when (a) end of
     * logical line, or (b) end of buffer. */
    int row_end_offset = offset + row_chars;
    if (mt->cursor == row_end_offset && cursor_col < 0)
    {
        bool is_logical_eol = (row_end_offset >= mt->len
                               || mt->text[row_end_offset] == '\n');
        if (is_logical_eol) cursor_col = lpos;
    }

    /* Selection intersected with this row. */
    int la = -1, lb = -1;
    bool nl_inside = false;
    if (sel_a < sel_b)
    {
        int a_col = sel_a - offset;
        int b_col = sel_b - offset;
        if (a_col < 0)       a_col = 0;
        if (b_col > lpos + 1) { b_col = lpos + 1; nl_inside = true; }
        if (a_col < b_col && a_col <= lpos)
        {
            la = a_col;
            lb = (b_col > lpos) ? lpos : b_col;
        }
    }

    if (la < 0)
    {
        if (lpos > 0)
            dobui_DrawTextFixed(mt->win_id, text_x, row_y + 1, linebuf,
                           mt->col_text, mt->col_bg);
    }
    else
    {
        int mid_x = text_x + la * DOBTB_FONT_W;
        int mid_w = (lb - la) * DOBTB_FONT_W;
        if (nl_inside)
        {
            int rem = text_w - (mid_x - text_x);
            if (rem > mid_w) mid_w = rem;
        }
        dobui_FillRect(mt->win_id, mid_x, row_y,
                       mid_w, DOBMT_LINE_H, mt->col_sel_bg);

        char seg[256];
        if (la > 0)
        {
            memcpy(seg, linebuf, (uint32_t)la);
            seg[la] = '\0';
            dobui_DrawTextFixed(mt->win_id, text_x, row_y + 1, seg,
                           mt->col_text, mt->col_bg);
        }
        if (lb > la)
        {
            int n = lb - la;
            memcpy(seg, linebuf + la, (uint32_t)n);
            seg[n] = '\0';
            dobui_DrawTextFixed(mt->win_id, mid_x, row_y + 1, seg,
                           mt->col_sel_text, mt->col_sel_bg);
        }
        if (lb < lpos)
        {
            int n = lpos - lb;
            memcpy(seg, linebuf + lb, (uint32_t)n);
            seg[n] = '\0';
            int rx = text_x + lb * DOBTB_FONT_W;
            dobui_DrawTextFixed(mt->win_id, rx, row_y + 1, seg,
                           mt->col_text, mt->col_bg);
        }
    }

    if (draw_cursor && mt->focused && cursor_col >= 0
        && !dobmt_HasSelection(mt))
    {
        int cx = text_x + cursor_col * DOBTB_FONT_W;
        dobui_FillRect(mt->win_id, cx, row_y, 2, DOBMT_LINE_H,
                       mt->col_cursor);
    }

    /* Advance past the visual row.  In non-wrap mode that's past
     * the '\n'; in wrap mode it's just to the next wrap point
     * (no '\n' to skip). */
    int next = offset + row_chars;
    if (next < mt->len && mt->text[next] == '\n') next++;
    return next;
}

/* Paint every visible row. */
static void mt_draw_full(dob_multitextbox_t *mt)
{
    mt_ensure_wrap(mt);
    int vis        = mt_visible_lines(mt);
    int cur_vline  = mt_vline_of(mt, mt->cursor);
    int cur_screen = cur_vline - mt->scroll_line;

    dobui_FillRect(mt->win_id, mt->x, mt->y, mt->w, mt->h, mt->col_bg);

    int sel_a = -1, sel_b = -1;
    if (dobmt_HasSelection(mt)) dobmt_GetSelection(mt, &sel_a, &sel_b);

    for (int s = 0; s < vis; s++)
    {
        if (mt_draw_line(mt, s, s == cur_screen, sel_a, sel_b) < 0) break;
    }

    /* Vertical scrollbar over the reserved right-edge gutter. Track uses
     * the widget background; the thumb geometry is the same the drag
     * hit-test consumes (mt_sb_geom), so painted and grabbable match. */
    {
        dob_scroll1d_t sg;
        int sb_x;
        if (mt_sb_geom(mt, &sg, &sb_x))
        {
            dobui_FillRect(mt->win_id, sb_x, mt->y,
                           DOBMT_SCROLLBAR_W, mt->h, mt->col_bg);
            dobui_FillRect(mt->win_id, sb_x + 1, sg.thumb_off,
                           DOBMT_SCROLLBAR_W - 2, sg.thumb_len,
                           mt->col_scrollbar);
        }
    }

    uint32_t bc = mt->focused ? mt->col_border_focus : mt->col_border;
    dobui_DrawRect(mt->win_id, mt->x, mt->y, mt->w, mt->h, bc);
}

void dobmt_Draw(dob_multitextbox_t *mt)
{
    /* Each Invalidate resets the server cmdlist, so we always emit
     * the full visible range — incremental dirty-tracking would
     * leave non-dirty lines unpainted. */
    if (!mt->visible) return;
    mt_draw_full(mt);
}

/* Multi-Line — Event handling */

static int mt_hit_test(dob_multitextbox_t *mt, int x, int y)
{
    int text_x = mt->x;
    if (mt->show_line_numbers)
        text_x += DOBMT_LINENO_W + DOBMT_LEFT_PAD;

    int rel_y = y - mt->y;
    int line;
    if (rel_y < 0)
    {
        /* Drag above the top edge -> one row above the current top.
         * ensure_visible() will then scroll up. */
        line = mt->scroll_line - 1;
        if (line < 0) line = 0;
    }
    else
    {
        line = rel_y / DOBMT_LINE_H + mt->scroll_line;
    }
    int col = (x - text_x + DOBTB_FONT_W / 2) / DOBTB_FONT_W;
    if (col < 0) col = 0;

    int offset    = mt_vline_start(mt, line);
    int max_chars = mt_vline_visible_len(mt, line);
    if (col > max_chars) col = max_chars;
    return offset + col;
}

/* Collapse a live selection to one endpoint: -1 = low, +1 = high. */
static void mt_collapse_selection(dob_multitextbox_t *mt, int side)
{
    if (!dobmt_HasSelection(mt)) return;
    int a, b; dobmt_GetSelection(mt, &a, &b);
    mt->cursor     = (side < 0) ? a : b;
    mt->sel_anchor = -1;
}

/* Logical-line scan for empty-buffer and no-cache paths. */
static void mt_paragraph_bounds(dob_multitextbox_t *mt, int pos,
                                int *a_out, int *b_out)
{
    if (pos < 0)     pos = 0;
    if (pos > mt->len) pos = mt->len;
    int a = pos, b = pos;
    while (a > 0      && mt->text[a - 1] != '\n') a--;
    while (b < mt->len && mt->text[b]     != '\n') b++;
    *a_out = a;
    *b_out = b;
}

bool dobmt_OnKey(dob_multitextbox_t *mt, uint8_t key)
{
    if (!mt->visible || !mt->enabled || !mt->focused) return false;

    /* Ctrl+A: local, no clipboard dependency. Ctrl+C/X/V dispatched
     * by the focus manager via the textbox_clip add-on. */
    if (key == 0x01) { return dobmt_SelectAll(mt); }

    if (key == DOBTB_KEY_LEFT)
    {
        if (dobmt_HasSelection(mt))
            mt_collapse_selection(mt, -1);
        else if (mt->cursor > 0)
            mt->cursor--;
        dobmt_priv_ensure_visible(mt);
        return true;
    }
    if (key == DOBTB_KEY_RIGHT)
    {
        if (dobmt_HasSelection(mt))
            mt_collapse_selection(mt, +1);
        else if (mt->cursor < mt->len)
            mt->cursor++;
        dobmt_priv_ensure_visible(mt);
        return true;
    }
    if (key == DOBTB_KEY_UP)
    {
        int cv = mt_vline_of(mt, mt->cursor);
        if (cv > 0)
        {
            int cur_start  = mt_vline_start(mt, cv);
            int col        = mt->cursor - cur_start;
            int prev_start = mt_vline_start(mt, cv - 1);
            int prev_len   = mt_vline_visible_len(mt, cv - 1);
            if (col > prev_len) col = prev_len;
            mt->cursor     = prev_start + col;
            mt->sel_anchor = -1;
            dobmt_priv_ensure_visible(mt);
        }
        return true;
    }
    if (key == DOBTB_KEY_DOWN)
    {
        int cv     = mt_vline_of(mt, mt->cursor);
        int total  = mt_total_vlines(mt);
        if (cv + 1 < total)
        {
            int cur_start  = mt_vline_start(mt, cv);
            int col        = mt->cursor - cur_start;
            int next_start = mt_vline_start(mt, cv + 1);
            int next_len   = mt_vline_visible_len(mt, cv + 1);
            if (col > next_len) col = next_len;
            mt->cursor     = next_start + col;
            mt->sel_anchor = -1;
            dobmt_priv_ensure_visible(mt);
        }
        return true;
    }
    if (key == DOBTB_KEY_HOME)
    {
        mt->cursor     = mt_vline_start(mt, mt_vline_of(mt, mt->cursor));
        mt->sel_anchor = -1;
        dobmt_priv_ensure_visible(mt);
        return true;
    }
    if (key == DOBTB_KEY_END)
    {
        int cv  = mt_vline_of(mt, mt->cursor);
        mt->cursor     = mt_vline_start(mt, cv) + mt_vline_visible_len(mt, cv);
        mt->sel_anchor = -1;
        dobmt_priv_ensure_visible(mt);
        return true;
    }
    if (key == '\b')
    {
        if (!dobmt_priv_delete_selection(mt) && mt->cursor > 0)
        {
            bool was_nl = (mt->text[mt->cursor - 1] == '\n');
            dobtb_priv_remove_range(mt->text, &mt->len,
                                    mt->cursor - 1, mt->cursor);
            mt->cursor--;
            mt->modified = true;
            if (was_nl)
                dobmt_priv_mark_structural(mt);
            else
                dobmt_priv_lines_shift(mt, mt->cursor, -1);
            dobmt_priv_ensure_visible(mt);
        }
        return true;
    }
    if (key == DOBTB_KEY_DELETE)
    {
        if (!dobmt_priv_delete_selection(mt) && mt->cursor < mt->len)
        {
            bool was_nl = (mt->text[mt->cursor] == '\n');
            dobtb_priv_remove_range(mt->text, &mt->len,
                                    mt->cursor, mt->cursor + 1);
            mt->modified = true;
            if (was_nl)
                dobmt_priv_mark_structural(mt);
            else
                dobmt_priv_lines_shift(mt, mt->cursor, -1);
        }
        return true;
    }
    if (key == '\n' || key == '\t' || dobtb_is_text_char(key))
    {
        dobmt_priv_delete_selection(mt);
        if (!dobmt_priv_reserve(mt, mt->len + (key == '\t' ? 4 : 1) + 1))
            return true;

        if (key == '\t')
        {
            char spaces[4] = { ' ', ' ', ' ', ' ' };
            int put = dobtb_priv_insert_range(mt->text, &mt->len, mt->cap,
                                              mt->cursor, spaces, 4);
            if (put > 0)
            {
                dobmt_priv_lines_shift(mt, mt->cursor, put);
                mt->cursor  += put;
                mt->modified = true;
                dobmt_priv_ensure_visible(mt);
            }
            return true;
        }

        char c = (char)key;
        int put = dobtb_priv_insert_range(mt->text, &mt->len, mt->cap,
                                          mt->cursor, &c, 1);
        if (put > 0)
        {
            if (c == '\n')
                dobmt_priv_mark_structural(mt);
            else
                dobmt_priv_lines_shift(mt, mt->cursor, 1);
            mt->cursor++;
            mt->modified = true;
            dobmt_priv_ensure_visible(mt);
        }
        return true;
    }
    return false;
}

bool dobmt_OnClick(dob_multitextbox_t *mt, int x, int y)
{
    if (!mt->visible || !mt->enabled) return false;
    if (x < mt->x || x >= mt->x + mt->w
        || y < mt->y || y >= mt->y + mt->h) return false;

    /* Scrollbar column: drive the bar, never place the caret. */
    {
        dob_scroll1d_t sg;
        int sb_x;
        if (mt_sb_geom(mt, &sg, &sb_x) && x >= sb_x)
        {
            if (dob_scroll1d_hit_thumb(&sg, y))
            {
                mt->sb_drag = true;
                mt->sb_grab = y - sg.thumb_off;
            }
            else
            {
                int vis = mt_visible_lines(mt);
                mt->scroll_line += (y < sg.thumb_off) ? -vis : vis;
                if (mt->scroll_line < 0) mt->scroll_line = 0;
                if (mt->scroll_line > sg.max_scroll) mt->scroll_line = sg.max_scroll;
            }
            return true;
        }
    }

    int hit = mt_hit_test(mt, x, y);
    mt->cursor     = hit;
    mt->sel_anchor = -1;      /* lazy anchor — see dobtb_OnClick */
    mt->selecting  = true;
    dobmt_priv_ensure_visible(mt);
    return true;
}

bool dobmt_OnDrag(dob_multitextbox_t *mt, int x, int y)
{
    if (!mt->visible || !mt->enabled) return false;

    /* Scrollbar drag takes precedence over text selection. */
    if (mt->sb_drag)
    {
        dob_scroll1d_t sg;
        int sb_x;
        if (!mt_sb_geom(mt, &sg, &sb_x)) { mt->sb_drag = false; return false; }
        int ns = dob_scroll1d_from_pos(&sg, y, mt->sb_grab);
        if (ns == mt->scroll_line) return false;
        mt->scroll_line = ns;
        return true;
    }

    if (!mt->selecting) return false;
    int hit = mt_hit_test(mt, x, y);
    if (hit == mt->cursor) return false;
    if (mt->sel_anchor < 0) mt->sel_anchor = mt->cursor; /* anchor at drag origin */
    mt->cursor = hit;
    dobmt_priv_ensure_visible(mt);
    return true;
}

bool dobmt_OnRelease(dob_multitextbox_t *mt)
{
    mt->sb_drag = false;
    if (!mt->selecting) return false;
    mt->selecting = false;
    if (mt->sel_anchor == mt->cursor)
        mt->sel_anchor = -1;
    return false;
}

bool dobmt_OnDblClick(dob_multitextbox_t *mt, int x, int y)
{
    if (!mt->visible || !mt->enabled) return false;
    if (x < mt->x || x >= mt->x + mt->w
        || y < mt->y || y >= mt->y + mt->h) return false;

    int count = multiclick_bump(&mt->mc, x, y, clock_ms());
    int hit = mt_hit_test(mt, x, y);
    int a, b;
    bool keep_scroll = false;

    if (count == 1)
    {
        word_bounds(mt->text, mt->len, hit, &a, &b, true);
    }
    else if (count == 2)
    {
        mt_paragraph_bounds(mt, hit, &a, &b);
    }
    else
    {
        /* Select-all: leave the scroll position untouched — moving
         * the cursor to mt->len would otherwise jump to the bottom. */
        a = 0;
        b = mt->len;
        keep_scroll = true;
        if (count > 3) mt->mc.count = 3;
    }

    mt->sel_anchor = a;
    mt->cursor     = b;
    if (!keep_scroll) dobmt_priv_ensure_visible(mt);
    else              dobmt_priv_mark_structural(mt);
    return true;
}

bool dobmt_OnScroll(dob_multitextbox_t *mt, int delta)
{
    if (!mt->visible || !mt->enabled) return false;

    int vis   = mt_visible_lines(mt);
    int total = mt_total_vlines(mt);

    int old = mt->scroll_line;
    mt->scroll_line += delta * 3;
    if (mt->scroll_line < 0) mt->scroll_line = 0;
    int max_scroll = total - vis;
    if (max_scroll < 0) max_scroll = 0;
    if (mt->scroll_line > max_scroll) mt->scroll_line = max_scroll;

    return mt->scroll_line != old;
}

bool dobmt_SelectAll(dob_multitextbox_t *mt)
{
    if (mt->len == 0) return false;
    mt->sel_anchor = 0;
    mt->cursor     = mt->len;
    dobmt_priv_mark_structural(mt);
    return true;
}

bool dobmt_Clear(dob_multitextbox_t *mt)
{
    if (mt->len == 0 && !dobmt_HasSelection(mt)) return false;
    if (mt->text) mt->text[0] = '\0';
    mt->len         = 0;
    mt->cursor      = 0;
    mt->scroll_line = 0;
    mt->sel_anchor  = -1;
    mt->selecting   = false;
    mt->modified    = true;
    dobmt_priv_mark_structural(mt);
    return true;
}
