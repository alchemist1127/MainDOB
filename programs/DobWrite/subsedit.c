/* subsedit.c -- see subsedit.h.
 *
 * The shared dob_table_t widget is read-only by contract (it references
 * caller-owned const strings).  We honour that: the table only displays and
 * selects; all editing happens in the two text boxes and is committed into
 * the autocorr core, then mirrored back into the table via SetRows.  No
 * change to the shared widget.
 */
#include "subsedit.h"
#include <DobInterface.h>
#include <dobui_theme.h>
#include <stdbool.h>
#include <table.h>
#include <textbox.h>
#include <button.h>
#include "autocorr.h"
#include <string.h>

#define SE_W 420
#define SE_H 340

typedef struct {
    dobui_win_t  *win;
    dob_table_t   tbl;
    dob_textbox_t tb_from, tb_to;
    dob_button_t  b_add, b_del, b_close;
    int           focus;                  /* 0 = from box, 1 = to box, -1 = none */
    const char   *keys[AC_MAX_ENTRIES];   /* mirror of the core list for the table */
    const char   *vals[AC_MAX_ENTRIES];
} se_state;

static se_state g_se;

static bool se_streq(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

/* rebuild the table's row pointers from the (possibly shifted) core list */
static void se_refresh(void)
{
    int n = autocorr_count();
    if (n > AC_MAX_ENTRIES) n = AC_MAX_ENTRIES;
    for (int i = 0; i < n; i++) {
        g_se.keys[i] = autocorr_from(i);
        g_se.vals[i] = autocorr_to(i);
    }
    dobtbl_SetRows(&g_se.tbl, g_se.keys, g_se.vals, n);
}

static void se_clear_focus(void)
{
    g_se.focus = -1;
    dobtb_SetFocus(&g_se.tb_from, false);
    dobtb_SetFocus(&g_se.tb_to, false);
}

static void se_draw(void)
{
    if (!g_se.win) return;
    uint32_t wid = dobui_win_id(g_se.win);
    dobui_FillRect(wid, 0, 0, SE_W, SE_H, DOBUI_SURFACE);
    dobtbl_Draw(&g_se.tbl);
    dobui_DrawText(wid, 12, 226, "Sostituisci:", DOBUI_TEXT, DOBUI_SURFACE);
    dobui_DrawText(wid, 12, 256, "Con:",         DOBUI_TEXT, DOBUI_SURFACE);
    dobtb_Draw(&g_se.tb_from);
    dobtb_Draw(&g_se.tb_to);
    dobbtn_Draw(&g_se.b_add);
    dobbtn_Draw(&g_se.b_del);
    dobbtn_Draw(&g_se.b_close);
    dobui_Invalidate(wid);
}

static void se_load_selection(void)
{
    int sel = dobtbl_GetSelectedIndex(&g_se.tbl);
    if (sel < 0) return;
    dobtb_SetText(&g_se.tb_from, autocorr_from(sel));
    dobtb_SetText(&g_se.tb_to,   autocorr_to(sel));
}

static void se_add(void)
{
    const char *f = dobtb_GetText(&g_se.tb_from);
    const char *t = dobtb_GetText(&g_se.tb_to);
    if (!f || !f[0]) return;
    /* update semantics: if "from" already exists, drop the old one first */
    for (int i = 0; i < autocorr_count(); i++)
        if (se_streq(autocorr_from(i), f)) { autocorr_remove(i); break; }
    autocorr_add(f, t);
    autocorr_save(AUTOCORR_PATH);
    dobtb_SetText(&g_se.tb_from, "");
    dobtb_SetText(&g_se.tb_to, "");
    se_clear_focus();
    se_refresh();
    se_draw();
}

static void se_del(void)
{
    int sel = dobtbl_GetSelectedIndex(&g_se.tbl);
    if (sel < 0) return;
    autocorr_remove(sel);
    autocorr_save(AUTOCORR_PATH);
    se_refresh();
    se_draw();
}

static void se_close(void)
{
    dobui_win_t *w = g_se.win;
    g_se.win = NULL;
    if (w) dobui_win_close(w);
}

/* ---- callbacks ---- */
static void se_on_start(dobui_win_t *w)
{
    g_se.win = w;
    uint32_t wid = dobui_win_id(w);

    dobtbl_Init(&g_se.tbl, wid, 12, 12, SE_W - 24, 200);
    dobtbl_SetHeaders(&g_se.tbl, "Sostituisci", "Con");
    dobtbl_SetSelectable(&g_se.tbl, true);

    dobtb_Init(&g_se.tb_from, wid, 110, 222, SE_W - 110 - 14, 22);
    dobtb_Init(&g_se.tb_to,   wid, 110, 252, SE_W - 110 - 14, 22);

    dobbtn_Init(&g_se.b_add,   wid,  12, 288, 96, 26, "Aggiungi");
    dobbtn_Init(&g_se.b_del,   wid, 114, 288, 96, 26, "Rimuovi");
    dobbtn_Init(&g_se.b_close, wid, SE_W - 14 - 96, 288, 96, 26, "Chiudi");

    se_clear_focus();
    se_refresh();
    se_draw();
}

static void se_on_click(dobui_win_t *w, int x, int y, uint8_t buttons)
{
    (void)w; (void)buttons;
    if (dobbtn_OnClick(&g_se.b_add, x, y))   { se_add();   return; }
    if (dobbtn_OnClick(&g_se.b_del, x, y))   { se_del();   return; }
    if (dobbtn_OnClick(&g_se.b_close, x, y)) { se_close(); return; }

    if (dobtbl_OnClick(&g_se.tbl, x, y)) {
        se_load_selection();
        se_clear_focus();
        se_draw();
        return;
    }
    if (dobtb_OnClick(&g_se.tb_from, x, y)) {
        g_se.focus = 0;
        dobtb_SetFocus(&g_se.tb_from, true);
        dobtb_SetFocus(&g_se.tb_to, false);
        se_draw();
        return;
    }
    if (dobtb_OnClick(&g_se.tb_to, x, y)) {
        g_se.focus = 1;
        dobtb_SetFocus(&g_se.tb_to, true);
        dobtb_SetFocus(&g_se.tb_from, false);
        se_draw();
        return;
    }
}

static void se_on_key(dobui_win_t *w, uint8_t key)
{
    (void)w;
    if (key == 27)              { se_close(); return; }       /* Esc   -> close       */
    if (key == 13 || key == 10) { se_add();   return; }       /* Invio -> Aggiungi    */

    if (g_se.focus == 0) { if (dobtb_OnKey(&g_se.tb_from, key)) se_draw(); return; }
    if (g_se.focus == 1) { if (dobtb_OnKey(&g_se.tb_to,   key)) se_draw(); return; }

    if (dobtbl_OnKey(&g_se.tbl, key)) { se_load_selection(); se_draw(); }
}

static void se_on_mousemove(dobui_win_t *w, int x, int y, uint8_t buttons)
{
    (void)w; (void)buttons;
    if (dobtbl_OnDrag(&g_se.tbl, x, y)) { se_draw(); return; }
    bool ch = false;
    if      (g_se.focus == 0) ch = dobtb_OnDrag(&g_se.tb_from, x, y);
    else if (g_se.focus == 1) ch = dobtb_OnDrag(&g_se.tb_to, x, y);
    if (ch) se_draw();
}

/* double-click selects the word (then whole field on repeats) in a from/to box */
static void se_on_dblclick(dobui_win_t *w, int x, int y, uint8_t buttons)
{
    (void)w; (void)buttons;
    if (dobtb_OnDblClick(&g_se.tb_from, x, y)) {
        g_se.focus = 0; dobtb_SetFocus(&g_se.tb_from, true); dobtb_SetFocus(&g_se.tb_to, false); se_draw(); return;
    }
    if (dobtb_OnDblClick(&g_se.tb_to, x, y)) {
        g_se.focus = 1; dobtb_SetFocus(&g_se.tb_to, true); dobtb_SetFocus(&g_se.tb_from, false); se_draw(); return;
    }
}

static void se_on_release(dobui_win_t *w, int x, int y, uint8_t buttons)
{
    (void)w; (void)x; (void)y; (void)buttons;
    dobbtn_OnRelease(&g_se.b_add);
    dobbtn_OnRelease(&g_se.b_del);
    dobbtn_OnRelease(&g_se.b_close);
    dobtbl_OnRelease(&g_se.tbl);
    dobtb_OnRelease(&g_se.tb_from);
    dobtb_OnRelease(&g_se.tb_to);
    se_draw();
}

static void se_on_scroll(dobui_win_t *w, int delta)
{
    (void)w;
    if (dobtbl_OnScroll(&g_se.tbl, delta)) se_draw();
}

static void se_on_close(dobui_win_t *w)
{
    (void)w;
    se_close();
}

static const dobui_win_vtbl_t SE_VTBL = {
    .on_start     = se_on_start,
    .on_key       = se_on_key,
    .on_click     = se_on_click,
    .on_dblclick  = se_on_dblclick,
    .on_mousemove = se_on_mousemove,
    .on_release   = se_on_release,
    .on_scroll    = se_on_scroll,
    .on_close     = se_on_close,
};

void subsedit_open(dobui_win_t *parent)
{
    if (g_se.win) return;
    memset(&g_se, 0, sizeof g_se);
    g_se.focus = -1;
    g_se.win = dobui_dialog_open(parent, "Sostituzioni automatiche", SE_W, SE_H,
                                 &SE_VTBL, &g_se, false /* modeless */,
                                 DOBUI_WIN_NORESIZE | DOBUI_WIN_NOMAXIMIZE);
    /* widgets + first paint happen in se_on_start */
}
