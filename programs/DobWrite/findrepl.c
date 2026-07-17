/* findrepl.c -- modern Find & Replace window (see findrepl.h).
 *
 * Pure UI: two text boxes (find / replace), a "find next" button, a "replace
 * all" button, a case toggle and a close button. Every document operation is
 * delegated to the host via the findrepl_host_* calls. The dialog stores NO
 * function pointers -- the previous design kept a callback struct as its last
 * field and a stray write corrupted it into a bad jump; that whole class of
 * bug is gone now.
 *
 * Drawing model: a control's event handler must draw into its own window (the
 * active context is set to the event's target). The host calls draw the MAIN
 * window, so after each one we call fr_restore() to switch the context back to
 * this dialog and repaint it.
 */
#include "findrepl.h"
#include <app.h>
#include <DobInterface.h>
#include <dobui_theme.h>
#include <stdbool.h>
#include <textbox.h>
#include <button.h>
#include <string.h>
#include <stdio.h>

#define FR_W 420
#define FR_H 190

typedef struct {
    dobui_win_t  *win;
    dob_textbox_t tb_find, tb_repl;
    dob_button_t  b_next, b_all, b_ci, b_close;
    int           focus;          /* 0 = find box, 1 = replace box, -1 = none */
    bool          ci;             /* ignore case */
    int           nmatch;         /* last search result count, -1 = not searched */
} fr_state;

static fr_state g_fr;

static void fr_ci_label(void)
{
    dobbtn_SetLabel(&g_fr.b_ci, g_fr.ci ? "Maiuscole: ignora" : "Maiuscole: rispetta");
}

static void fr_draw(void)
{
    if (!g_fr.win) return;
    uint32_t wid = dobui_win_id(g_fr.win);
    dobui_FillRect(wid, 0, 0, FR_W, FR_H, DOBUI_SURFACE);
    dobui_DrawText(wid, 12, 22, "Trova:",       DOBUI_TEXT, DOBUI_SURFACE);
    dobui_DrawText(wid, 12, 54, "Sostituisci:", DOBUI_TEXT, DOBUI_SURFACE);
    dobtb_Draw(&g_fr.tb_find);
    dobtb_Draw(&g_fr.tb_repl);
    dobbtn_Draw(&g_fr.b_next);
    dobbtn_Draw(&g_fr.b_all);
    dobbtn_Draw(&g_fr.b_ci);
    dobbtn_Draw(&g_fr.b_close);
    /* Show the count only when there is a search term: "N risultati" with an
     * empty Trova field is meaningless, and the field being empty means nmatch
     * hasn't been set by a real search (it can hold a stale value). */
    if (g_fr.nmatch >= 0 && g_fr.tb_find.len > 0) {
        char buf[40];
        if (g_fr.nmatch == 0) snprintf(buf, sizeof buf, "Nessun risultato");
        else snprintf(buf, sizeof buf, "%d risultat%s", g_fr.nmatch, g_fr.nmatch == 1 ? "o" : "i");
        dobui_DrawText(wid, 12, 164, buf, DOBUI_TEXT, DOBUI_SURFACE);
    }
    dobui_Invalidate(wid);
}

/* A host call painted the main window and left it active; return to us. */
static void fr_restore(void)
{
    if (g_fr.win) { dobui_SetActiveWindow(dobui_win_id(g_fr.win)); fr_draw(); }
}

static void fr_clear_focus(void)
{
    g_fr.focus = -1;
    dobtb_SetFocus(&g_fr.tb_find, false);
    dobtb_SetFocus(&g_fr.tb_repl, false);
}

static void fr_do_next(void)
{
    findrepl_host_find_next(dobtb_GetText(&g_fr.tb_find), g_fr.ci);
    g_fr.nmatch = findrepl_host_count();
    fr_restore();
}

static void fr_close(void)
{
    dobui_win_t *w = g_fr.win;
    g_fr.win = NULL;
    findrepl_host_closed();          /* clear highlights + repaint main */
    if (w) dobui_win_close(w);
}

/* ---- callbacks ---- */
static void fr_on_start(dobui_win_t *w)
{
    g_fr.win = w;
    uint32_t wid = dobui_win_id(w);

    dobtb_Init(&g_fr.tb_find, wid, 130, 12, FR_W - 130 - 14, 24);
    dobtb_Init(&g_fr.tb_repl, wid, 130, 44, FR_W - 130 - 14, 24);

    g_fr.ci = true;
    dobbtn_Init(&g_fr.b_next,  wid,  12,  82, 160, 26, "Trova successivo");
    dobbtn_Init(&g_fr.b_ci,    wid, 184,  82, FR_W - 184 - 14, 26, "");
    dobbtn_Init(&g_fr.b_all,   wid,  12, 120, 160, 26, "Sostituisci tutto");
    dobbtn_Init(&g_fr.b_close, wid, FR_W - 14 - 90, 120, 90, 26, "Chiudi");
    fr_ci_label();

    fr_clear_focus();
    fr_draw();
}

static void fr_on_click(dobui_win_t *w, int x, int y, uint8_t buttons)
{
    (void)w; (void)buttons;
    if (dobbtn_OnClick(&g_fr.b_next, x, y))  { fr_do_next(); return; }
    if (dobbtn_OnClick(&g_fr.b_all, x, y))   {
        findrepl_host_replace_all(dobtb_GetText(&g_fr.tb_find), dobtb_GetText(&g_fr.tb_repl), g_fr.ci);
        g_fr.nmatch = -1;                    /* matches cleared after a replace */
        fr_restore(); return; }
    if (dobbtn_OnClick(&g_fr.b_ci, x, y))    { g_fr.ci = !g_fr.ci; fr_ci_label(); fr_draw(); return; }
    if (dobbtn_OnClick(&g_fr.b_close, x, y)) { fr_close(); return; }

    if (dobtb_OnClick(&g_fr.tb_find, x, y)) {
        g_fr.focus = 0; dobtb_SetFocus(&g_fr.tb_find, true); dobtb_SetFocus(&g_fr.tb_repl, false); fr_draw(); return;
    }
    if (dobtb_OnClick(&g_fr.tb_repl, x, y)) {
        g_fr.focus = 1; dobtb_SetFocus(&g_fr.tb_repl, true); dobtb_SetFocus(&g_fr.tb_find, false); fr_draw(); return;
    }
}

static void fr_on_key(dobui_win_t *w, uint8_t key)
{
    (void)w;
    if (key == 27)              { fr_close();  return; }      /* Esc   -> close      */
    if (key == 13 || key == 10) { fr_do_next(); return; }     /* Invio -> find next  */

    if (g_fr.focus == 0) { if (dobtb_OnKey(&g_fr.tb_find, key)) fr_draw(); return; }
    if (g_fr.focus == 1) { if (dobtb_OnKey(&g_fr.tb_repl, key)) fr_draw(); return; }
}

static void fr_on_close(dobui_win_t *w) { (void)w; fr_close(); }

/* Textbox selection needs the drag / double-click / release events forwarded
 * to the widget; the dialog drives its controls by hand, so route them here.
 * Drag extends the selection in the focused box; double-click selects the word
 * (then line / all on repeats); release ends the in-flight selection. */
static void fr_on_mousemove(dobui_win_t *w, int x, int y, uint8_t buttons)
{
    (void)w; (void)buttons;
    bool ch = false;
    if      (g_fr.focus == 0) ch = dobtb_OnDrag(&g_fr.tb_find, x, y);
    else if (g_fr.focus == 1) ch = dobtb_OnDrag(&g_fr.tb_repl, x, y);
    if (ch) fr_draw();
}

static void fr_on_dblclick(dobui_win_t *w, int x, int y, uint8_t buttons)
{
    (void)w; (void)buttons;
    if (dobtb_OnDblClick(&g_fr.tb_find, x, y)) {
        g_fr.focus = 0; dobtb_SetFocus(&g_fr.tb_find, true); dobtb_SetFocus(&g_fr.tb_repl, false); fr_draw(); return;
    }
    if (dobtb_OnDblClick(&g_fr.tb_repl, x, y)) {
        g_fr.focus = 1; dobtb_SetFocus(&g_fr.tb_repl, true); dobtb_SetFocus(&g_fr.tb_find, false); fr_draw(); return;
    }
}

static void fr_on_release(dobui_win_t *w, int x, int y, uint8_t buttons)
{
    (void)w; (void)x; (void)y; (void)buttons;
    dobtb_OnRelease(&g_fr.tb_find);
    dobtb_OnRelease(&g_fr.tb_repl);
    fr_draw();
}

static const dobui_win_vtbl_t FR_VTBL = {
    .on_start     = fr_on_start,
    .on_key       = fr_on_key,
    .on_click     = fr_on_click,
    .on_mousemove = fr_on_mousemove,
    .on_dblclick  = fr_on_dblclick,
    .on_release   = fr_on_release,
    .on_close     = fr_on_close,
};

void findrepl_open(dobui_win_t *parent)
{
    if (g_fr.win) return;
    memset(&g_fr, 0, sizeof g_fr);
    g_fr.focus = -1;
    g_fr.nmatch = -1;
    g_fr.win = dobui_dialog_open(parent, "Trova e sostituisci", FR_W, FR_H,
                                 &FR_VTBL, &g_fr, false /* modeless */,
                                 DOBUI_WIN_NORESIZE | DOBUI_WIN_NOMAXIMIZE);
    /* widgets + first paint happen in fr_on_start */
}
