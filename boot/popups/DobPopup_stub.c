/* DobPopup — in-process modal dialogs.
 *
 * This used to be a thin IPC stub to a separate "popups" server: the
 * caller blocked on a synchronous call while the daemon spawned the
 * window in its own process.  Now the dialog is a real secondary
 * window inside the *calling* program -- a modal child of its main
 * window (Windows-style overlay), drawn with the same adaptive layout
 * the old server used.
 *
 * The call still blocks until the user answers, but via a nested
 * receive loop on a private port instead of an IPC to a daemon.  The
 * caller's own event loop is suspended for the duration (exactly the
 * "held hostage" behaviour it had before), so its queued events drain
 * once the dialog closes.
 *
 *   - dobui_run apps: the dialog parents on dobui_window(), so the WM
 *     blocks the parent and the dialog floats over it.
 *   - programs with their own loop (dobui_window() == 0): the dialog is
 *     an unparented top-level window -- still modal-by-blocking, just
 *     not visually tied to a parent.
 *
 * Windowless components (no widget toolkit linked: hotplug, diagnostic
 * tools) use DobPopup_toast_stub.c instead, which routes to a WM toast.
 */

#include <DobPopup.h>
#include <DobInterface.h>
#include <app.h>          /* dobui_window() */
#include <button.h>
#include <textbox.h>
#include <dob/ipc.h>
#include <unistd.h>       /* port_create */
#include <string.h>

#define POPUP_W       350
#define POPUP_H_MAX   560

#define COL_WHITE     0x00FFFFFF
#define COL_BLACK     0x00000000
#define COL_ICON_INFO 0x00FF8800
#define COL_ICON_WARN 0x0000FFFF
#define COL_ICON_ERR  0x000000FF
#define COL_BODY      0x000000AA   /* blu standard del tema (DOBUI_SURFACE) */

/* GUI event codes (subset, from dobinterface) */
#define GUI_EVT_PANEL_CMD  200
#define GUI_EVT_MOUSE      201
#define GUI_EVT_KEY        202
#define GUI_EVT_CLOSE_REQ  210
#define GUI_EVT_FIRST      200
#define GUI_EVT_LAST       216

/* Private port the dialog window posts its events to.  One per
 * process, created lazily and reused: a dialog is always modal so at
 * most one is ever live. */
static uint32_t g_popup_port = 0;
static uint32_t popup_port(void)
{
    if (!g_popup_port) g_popup_port = (uint32_t)port_create();
    return g_popup_port;
}

/* ---- adaptive layout (moved verbatim from the old popups server) ---- */

static void draw_icon(uint32_t win, int type)
{
    int ix = 20, iy = 30;
    switch (type)
    {
        case POPUP_INFO:
            dobui_FillRect(win, ix, iy, 32, 32, COL_ICON_INFO);
            dobui_DrawText(win, ix + 12, iy + 8, "i", COL_WHITE, COL_ICON_INFO);
            break;
        case POPUP_WARNING:
            dobui_FillRect(win, ix, iy, 32, 32, COL_ICON_WARN);
            dobui_DrawText(win, ix + 12, iy + 8, "!", COL_BLACK, COL_ICON_WARN);
            break;
        case POPUP_ERROR:
            dobui_FillRect(win, ix, iy, 32, 32, COL_ICON_ERR);
            dobui_DrawText(win, ix + 12, iy + 8, "X", COL_WHITE, COL_ICON_ERR);
            break;
        case POPUP_YESNO:
            dobui_FillRect(win, ix, iy, 32, 32, COL_ICON_INFO);
            dobui_DrawText(win, ix + 12, iy + 8, "?", COL_WHITE, COL_ICON_INFO);
            break;
    }
}

static void draw_message(uint32_t win, const char *msg, int x, int y, int max_w)
{
    int cx = x, cy = y;
    int char_w = 8, line_h = 18;
    int chars_per_line = max_w / char_w;
    char line[64];
    int li = 0;
    while (*msg)
    {
        if (*msg == '\n' || li >= chars_per_line - 1)
        {
            line[li] = '\0';
            dobui_DrawText(win, cx, cy, line, COL_WHITE, COL_BODY);
            cy += line_h; li = 0;
            if (*msg == '\n') msg++;
            continue;
        }
        line[li++] = *msg++;
    }
    if (li > 0)
    {
        line[li] = '\0';
        dobui_DrawText(win, cx, cy, line, COL_WHITE, COL_BODY);
    }
}

static int count_message_lines(const char *msg, int max_w)
{
    if (!msg) return 1;
    int char_w = 8;
    int chars_per_line = max_w / char_w;
    if (chars_per_line < 2) chars_per_line = 2;
    int lines = 0, li = 0;
    while (*msg)
    {
        if (*msg == '\n' || li >= chars_per_line - 1)
        {
            lines++; li = 0;
            if (*msg == '\n') msg++;
            continue;
        }
        li++; msg++;
    }
    if (li > 0) lines++;
    return lines < 1 ? 1 : lines;
}

/* Create the dialog window: a modal child of the caller's main window
 * when it has one (dobui_run apps), else a plain top-level window.
 * Returns 0 on failure. */
static uint32_t dialog_create(int h, const char *title)
{
    uint32_t win = dobui_CreateWindow(POPUP_W, h, popup_port(), title);
    if (!win) return 0;
    uint32_t parent = dobui_window();
    if (parent) dobui_SetParent(win, parent, /*modal=*/true);
    dobui_SetWindowFlags(win, DOBUI_WIN_NORESIZE | DOBUI_WIN_NOMAXIMIZE);
    dobui_SetActiveWindow(win);
    return win;
}

/* True if `m` is a GUI event addressed to our dialog window. */
static bool is_dialog_event(const dob_msg_t *m, uint32_t win)
{
    return m->type != 1
        && m->code >= GUI_EVT_FIRST && m->code <= GUI_EVT_LAST
        && m->arg0 == win;
}

/* Reply empty to a stray sync request so its sender isn't stuck while
 * our nested loop owns the port. */
static void drain_sync(const dob_msg_t *m)
{
    if (m->type == 1)
    {
        dob_msg_t r; memset(&r, 0, sizeof(r));
        dob_ipc_reply(m->sender_tid, &r);
    }
}

int dobpopup_Show(int type, const char *title, const char *message)
{
    if (!title)   title = "";
    if (!message) message = "";
    if (type < 0 || type > 4) type = POPUP_INFO;

    /* Height adapts to the wrapped message (same math as the server). */
    int nlines = count_message_lines(message, POPUP_W - 75);
    int text_bottom = 30 + nlines * 18;
    if (text_bottom < 30 + 32) text_bottom = 30 + 32;
    int win_h = text_bottom + 12 + DOBBTN_DEFAULT_H + 12;
    if (win_h < 110)         win_h = 110;
    if (win_h > POPUP_H_MAX) win_h = POPUP_H_MAX;

    uint32_t win = dialog_create(win_h, title);
    if (!win) return -1;

    dobui_FillRect(win, 0, 0, POPUP_W, win_h, COL_BODY);
    draw_icon(win, type);
    draw_message(win, message, 65, 30, POPUP_W - 75);

    dob_button_row_t row;
    int by = win_h - DOBBTN_DEFAULT_H - 12;
    dobbtn_RowInit(&row, win, POPUP_W / 2, by);
    if (type == POPUP_YESNO)
    {
        dobbtn_RowAdd(&row, "Si");
        dobbtn_RowAdd(&row, "No");
        dobui_SetPanelCommands(win, "Si\nNo");
    }
    else
    {
        dobbtn_RowAdd(&row, "OK");
        dobui_SetPanelCommands(win, "OK");
    }
    dobbtn_RowLayout(&row);
    dobbtn_RowDraw(&row);
    dobui_Invalidate(win);

    int choice = 0;
    for (;;)
    {
        dob_msg_t msg; memset(&msg, 0, sizeof(msg));
        if (dob_ipc_receive(popup_port(), &msg) != DOB_OK) { choice = -1; break; }

        if (!is_dialog_event(&msg, win)) { drain_sync(&msg); continue; }

        if (msg.code == GUI_EVT_MOUSE && msg.arg3 == 1)
        {
            int lx = (int)(int16_t)(msg.arg1 & 0xFFFF);
            int ly = (int)(int16_t)((msg.arg1 >> 16) & 0xFFFF);
            int b = dobbtn_RowOnClick(&row, lx, ly);
            if (b >= 0) { choice = b; break; }
        }
        else if (msg.code == GUI_EVT_PANEL_CMD) { choice = (int)msg.arg1; break; }
        else if (msg.code == GUI_EVT_CLOSE_REQ) { choice = -1; break; }
    }

    dobui_DestroyWindow(win);

    /* Same return contract as the old server: YESNO yields 1 only for
     * an explicit "No"; everything else (including close) is 0 / Yes. */
    if (type == POPUP_YESNO) return (choice == 1) ? 1 : 0;
    return 0;
}

static void input_redraw(uint32_t win, int h, const char *prompt,
                         dob_textbox_t *tb, dob_button_row_t *row)
{
    dobui_SetActiveWindow(win);
    dobui_FillRect(win, 0, 0, POPUP_W, h, COL_BODY);
    if (prompt) draw_message(win, prompt, 20, 15, POPUP_W - 40);
    dobtb_Draw(tb);
    dobbtn_RowDraw(row);
    dobui_Invalidate(win);
}

int dobpopup_InputBox(const char *title, const char *prompt,
                      const char *default_text,
                      char *out_text, uint32_t out_size)
{
    if (!title)        title = "";
    if (!prompt)       prompt = "";
    if (!default_text) default_text = "";

    int nlines  = count_message_lines(prompt, POPUP_W - 40);
    int field_y = 15 + nlines * 18 + 10;
    int field_h = 20;
    int input_h = field_y + field_h + 14 + DOBBTN_DEFAULT_H + 12;
    if (input_h < 120)         input_h = 120;
    if (input_h > POPUP_H_MAX) input_h = POPUP_H_MAX;

    uint32_t win = dialog_create(input_h, title);
    if (!win) { if (out_text && out_size) out_text[0] = '\0'; return -1; }

    dob_textbox_t    tb;
    dob_button_row_t row;

    dobui_FillRect(win, 0, 0, POPUP_W, input_h, COL_BODY);
    draw_message(win, prompt, 20, 15, POPUP_W - 40);
    dobtb_Init(&tb, win, 20, field_y, POPUP_W - 40, field_h);
    tb.focused = true;
    if (default_text[0]) dobtb_SetText(&tb, default_text);
    dobtb_Draw(&tb);
    int by = input_h - DOBBTN_DEFAULT_H - 12;
    dobbtn_RowInit(&row, win, POPUP_W / 2, by);
    dobbtn_RowAdd(&row, "Conferma");
    dobbtn_RowAdd(&row, "Annulla");
    dobbtn_RowLayout(&row);
    dobbtn_RowDraw(&row);
    dobui_SetPanelCommands(win, "Conferma\nAnnulla");
    dobui_Invalidate(win);

    int  result = -1;
    bool done   = false;
    while (!done)
    {
        dob_msg_t msg; memset(&msg, 0, sizeof(msg));
        if (dob_ipc_receive(popup_port(), &msg) != DOB_OK) { result = -1; break; }

        if (!is_dialog_event(&msg, win)) { drain_sync(&msg); continue; }

        if (msg.code == GUI_EVT_KEY)
        {
            uint8_t key = (uint8_t)msg.arg1;
            if      (key == '\n') { result = 0;  done = true; }
            else if (key == 27)   { result = -1; done = true; }
            else if (dobtb_OnKey(&tb, key)) input_redraw(win, input_h, prompt, &tb, &row);
        }
        else if (msg.code == GUI_EVT_MOUSE)
        {
            int lx = (int)(int16_t)(msg.arg1 & 0xFFFF);
            int ly = (int)(int16_t)((msg.arg1 >> 16) & 0xFFFF);
            uint32_t et = msg.arg3;    /* 1=click 2=release 3=dblclick 6=move */
            if (et == 1)
            {
                int b = dobbtn_RowOnClick(&row, lx, ly);
                if      (b == 0) { result = 0;  done = true; }
                else if (b == 1) { result = -1; done = true; }
                else if (dobtb_OnClick(&tb, lx, ly)) input_redraw(win, input_h, prompt, &tb, &row);
            }
            else if (et == 6) { if (dobtb_OnDrag(&tb, lx, ly))     input_redraw(win, input_h, prompt, &tb, &row); }  /* drag: extend selection */
            else if (et == 3) { if (dobtb_OnDblClick(&tb, lx, ly)) input_redraw(win, input_h, prompt, &tb, &row); }  /* dblclick: word / all */
            else if (et == 2) { dobtb_OnRelease(&tb);              input_redraw(win, input_h, prompt, &tb, &row);  }  /* release: end selection */
        }
        else if (msg.code == GUI_EVT_PANEL_CMD)
        {
            if      ((int)msg.arg1 == 0) { result = 0;  done = true; }
            else if ((int)msg.arg1 == 1) { result = -1; done = true; }
        }
        else if (msg.code == GUI_EVT_CLOSE_REQ) { result = -1; done = true; }
    }

    /* Capture the text BEFORE destroying the window. */
    if (result == 0 && out_text && out_size > 0)
    {
        const char *t = dobtb_GetText(&tb);
        uint32_t n = (uint32_t)strlen(t);
        if (n > out_size - 1) n = out_size - 1;
        memcpy(out_text, t, n);
        out_text[n] = '\0';
    }
    else if (out_text && out_size > 0)
    {
        out_text[0] = '\0';
    }

    dobui_DestroyWindow(win);
    return result;
}
