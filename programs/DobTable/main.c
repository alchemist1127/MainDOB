/* MainDOB DobTable — standalone two-column key/value viewer.
 *
 * Spawned with the service name in argv[0]. Registers under that name,
 * creates one window holding a dob_table_t widget, and serves the
 * dobtable_protocol over IPC until the user closes the window or the
 * caller sends DOBTABLE_CLOSE.
 *
 * The widget logic lives in libdobui/table.c — main.c is just glue:
 *   - row data: caller-pushed strings stored in static buffers, the
 *     widget references those buffers directly.
 *   - clipboard: the side panel offers "Copia" which writes the
 *     selected (key: value) line via dobui_cliptext_set.
 *   - cursor: dobtbl_OnClick / OnRelease in the widget itself
 *     install / remove a CURSOR_HSPLIT override on the window via
 *     dobui_SetCursor — no involvement from main.c.
 *
 * Architecture mirrors the standard libdobui app pattern: dobui_run
 * owns the event loop, weak handlers (event_mouseclick, event_request,
 * etc.) are overridden where we need them. */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <dob/types.h>
#include <dob/ipc.h>
#include <dob/registry.h>

#include <DobInterface.h>

#include <app.h>
#include <table.h>
#include <focus.h>
#include <cliptext.h>

#include "dobtable_protocol.h"

/* Layout constants */

#include <dobui_theme.h>
#define COL_WIN_BG          DOBUI_SURFACE
#define TBL_PAD              8

/* Panel commands */
#define PANEL_CMDS          "Copia\nChiudi"
#define PANEL_CMD_COPY      0
#define PANEL_CMD_CLOSE     1

/* State */

static char         service_name[24] = {0};
static uint32_t     win_id           = 0;
static dob_table_t  table;

/* Row storage — caller pushes strings via IPC, we keep them in
 * fixed-size buffers and hand pointers to the widget.  Keeps memory
 * predictable and avoids heap fragmentation in long sessions. */
static char  keys_buf[DOBTABLE_MAX_ROWS][DOBTABLE_MAX_KEY_LEN];
static char  vals_buf[DOBTABLE_MAX_ROWS][DOBTABLE_MAX_VALUE_LEN];
static const char *keys_ptr[DOBTABLE_MAX_ROWS];
static const char *vals_ptr[DOBTABLE_MAX_ROWS];
static int   row_count = 0;

/* Header strings — same idea, fixed buffers we hand pointers from. */
static char  header_key_buf  [DOBTABLE_MAX_HEADER_LEN] = {0};
static char  header_value_buf[DOBTABLE_MAX_HEADER_LEN] = {0};
static bool  has_headers = false;

/* Drawing */

static void draw_all(void)
{
    if (!win_id) return;
    dobui_FillRect(win_id, 0, 0, dobui_width(), dobui_height(), COL_WIN_BG);
    dobtbl_Draw(&table);
    dobui_Invalidate(win_id);
}

/* Event handlers — libdobui framework dispatches mouse / key / panel
 * events here.  Most of the work goes through the focus manager,
 * which routes to the table widget automatically (the widget
 * self-registered at Init time). */

void event_start(void)
{
    win_id = dobui_window();

    /* Initialize the table.  Inset by TBL_PAD on all sides; the
     * widget will draw its own border. */
    dobtbl_Init(&table, win_id,
                TBL_PAD, TBL_PAD,
                dobui_width()  - 2 * TBL_PAD,
                dobui_height() - 2 * TBL_PAD);

    /* Selectable so the user can pick a row to copy.  No headers by
     * default; the caller will publish them via SET_HEADERS if it
     * wants any. */
    dobtbl_SetSelectable(&table, true);
    dobtbl_SetFocus(&table, true);

    /* Hand the panel to the focus manager so contextual clipboard
     * commands (Incolla / Copia tutto / ...) appear automatically
     * for any future text widget that may join the layout.  Our
     * own base commands stay visible whenever no text widget is
     * focused — which is always for now, since the table is the
     * only control. */
    dobfocus_attach_panel(win_id, PANEL_CMDS);

    /* Register the service AFTER the window is up, so a caller that
     * calls SetTitle the very next instant sees a window already
     * mapped (no flash of unstyled title). */
    dob_registry_register(service_name, dobui_port());

    draw_all();
}

void event_mouseclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    /* Focus manager hit-tests every registered control; OnClick on
     * the table either starts a divider drag or moves the row
     * selection — both internal to the widget. */
    if (dobfocus_click(x, y))
        draw_all();
}

void event_mousemove(int x, int y, uint8_t buttons)
{
    (void)buttons;
    /* Drag — only meaningful when a control is grabbed (column
     * splitter in our case). dobfocus_drag returns true while the
     * grab is active; we redraw to update the visible split. */
    if (dobfocus_drag(x, y))
        draw_all();
}

void event_mouserelease(int x, int y, uint8_t buttons)
{
    (void)x; (void)y; (void)buttons;
    dobfocus_release();
    draw_all();
}

void event_scroll(int delta)
{
    if (dobfocus_scroll(delta))
        draw_all();
}

void event_key(uint8_t key)
{
    if (dobfocus_key(key))
        draw_all();
}

void event_resize(int w, int h)
{
    /* Relayout the table to the new window size. The widget keeps
     * its current key_col_w if still legal; if not, the SetKeyColumnWidth
     * inside the public API would clamp it.  Here we just resize the
     * outer dimensions and re-clamp explicitly so a shrink doesn't
     * leave the divider past the new right edge. */
    table.w = w - 2 * TBL_PAD;
    table.h = h - 2 * TBL_PAD;
    dobtbl_SetKeyColumnWidth(&table, table.key_col_w);
    draw_all();
}

/* Panel — base commands "Copia" / "Chiudi", plus whatever contextual
 * commands the focus manager may have spliced in (none, for now). */

static void copy_selected_to_clipboard(void)
{
    int idx = dobtbl_GetSelectedIndex(&table);
    if (idx < 0) return;

    const char *k = dobtbl_GetSelectedKey(&table);
    const char *v = dobtbl_GetSelectedValue(&table);
    if (!k) k = "";
    if (!v) v = "";

    /* Format: "key: value".  Reasonable when pasted into editors,
     * messages, etc. */
    char buf[DOBTABLE_MAX_KEY_LEN + 2 + DOBTABLE_MAX_VALUE_LEN];
    int n = snprintf(buf, sizeof(buf), "%s: %s", k, v);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    dobui_cliptext_set(buf, n);
}

void event_panel(int cmd_idx)
{
    /* Contextual clipboard commands first — same idiom as uidemo. */
    if (dobfocus_panel(cmd_idx))
    {
        draw_all();
        return;
    }

    switch (cmd_idx)
    {
        case PANEL_CMD_COPY:
            copy_selected_to_clipboard();
            break;
        case PANEL_CMD_CLOSE:
            dobui_quit();
            break;
        default:
            break;
    }
}

/* IPC request handler — the caller's protocol opcodes.  GUI events
 * (200..220) and TIMER (70) are filtered out by the framework and
 * never reach us here, so we can switch on msg->code without
 * worrying about overlap. */

static void reply_status(dob_msg_t *msg, int32_t status)
{
    dob_msg_t reply = {0};
    reply.arg0 = (uint32_t)status;
    dob_ipc_reply(msg->sender_tid, &reply);
}

static void handle_set_title(dob_msg_t *msg)
{
    if (!msg->payload || msg->payload_size == 0
        || msg->payload_size > DOBTABLE_MAX_TITLE_LEN)
    {
        reply_status(msg, -1);
        return;
    }
    /* Defensive NUL-terminate — the payload may not be properly
     * terminated even if the wire spec says so. */
    char title[DOBTABLE_MAX_TITLE_LEN];
    int n = (int)msg->payload_size;
    if (n > (int)sizeof(title) - 1) n = (int)sizeof(title) - 1;
    memcpy(title, msg->payload, (uint32_t)n);
    title[n] = '\0';

    if (win_id) dobui_SetTitle(win_id, title);
    reply_status(msg, 0);
}

static void handle_set_headers(dob_msg_t *msg)
{
    if (!msg->payload || msg->payload_size == 0)
    {
        /* Treat empty payload as "remove headers". */
        has_headers = false;
        dobtbl_SetHeaders(&table, NULL, NULL);
        draw_all();
        reply_status(msg, 0);
        return;
    }

    /* Parse two NUL-terminated strings back-to-back. Defensive
     * scanning so a malformed payload cannot read past the end. */
    const char *p   = (const char *)msg->payload;
    int         len = (int)msg->payload_size;

    int kn = 0;
    while (kn < len && p[kn] != '\0') kn++;
    if (kn >= len || kn >= DOBTABLE_MAX_HEADER_LEN) { reply_status(msg, -1); return; }

    int v0 = kn + 1;
    int vn = 0;
    while (v0 + vn < len && p[v0 + vn] != '\0') vn++;
    if (v0 + vn >= len || vn >= DOBTABLE_MAX_HEADER_LEN) { reply_status(msg, -1); return; }

    memcpy(header_key_buf,   p,        (uint32_t)(kn + 1));
    memcpy(header_value_buf, p + v0,   (uint32_t)(vn + 1));
    has_headers = (kn > 0) || (vn > 0);

    dobtbl_SetHeaders(&table,
                      kn > 0 ? header_key_buf   : NULL,
                      vn > 0 ? header_value_buf : NULL);
    draw_all();
    reply_status(msg, 0);
}

static void handle_add_rows(dob_msg_t *msg)
{
    int n = (int)msg->arg0;
    if (n <= 0) { reply_status(msg, 0); return; }
    if (row_count + n > DOBTABLE_MAX_ROWS) { reply_status(msg, -1); return; }
    if (!msg->payload) { reply_status(msg, -1); return; }

    /* Walk the packed (key\0value\0)*count blob in lockstep with the
     * row counter. Reject silently if the blob is short or
     * mis-terminated; whatever we already inserted stays. */
    const char *p   = (const char *)msg->payload;
    int         off = 0;
    int         len = (int)msg->payload_size;

    for (int i = 0; i < n; i++)
    {
        /* key */
        int kn = 0;
        while (off + kn < len && p[off + kn] != '\0') kn++;
        if (off + kn >= len || kn >= DOBTABLE_MAX_KEY_LEN)
        { reply_status(msg, -1); return; }

        memcpy(keys_buf[row_count], p + off, (uint32_t)(kn + 1));
        off += kn + 1;

        /* value */
        int vn = 0;
        while (off + vn < len && p[off + vn] != '\0') vn++;
        if (off + vn >= len || vn >= DOBTABLE_MAX_VALUE_LEN)
        { reply_status(msg, -1); return; }

        memcpy(vals_buf[row_count], p + off, (uint32_t)(vn + 1));
        off += vn + 1;

        keys_ptr[row_count] = keys_buf[row_count];
        vals_ptr[row_count] = vals_buf[row_count];
        row_count++;
    }

    /* Re-publish the (possibly-grown) row arrays to the widget.
     * SetRows resets selection and scroll — that is fine for an
     * "I just got new data" situation. */
    dobtbl_SetRows(&table, keys_ptr, vals_ptr, row_count);
    draw_all();
    reply_status(msg, 0);
}

static void handle_clear(dob_msg_t *msg)
{
    row_count = 0;
    dobtbl_SetRows(&table, NULL, NULL, 0);
    draw_all();
    reply_status(msg, 0);
}

static void handle_show(dob_msg_t *msg)
{
    if (win_id)
    {
        dobui_Raise(win_id);
        draw_all();
    }
    reply_status(msg, 0);
}

static void handle_close(dob_msg_t *msg)
{
    /* Reply BEFORE exiting so the caller doesn't get an IPC error.
     * After this point the process tears down; no other messages
     * will be processed. */
    reply_status(msg, 0);
    dobui_quit();
}

void event_request(dob_msg_t *msg)
{
    switch (msg->code)
    {
        case DOBTABLE_SET_TITLE:    handle_set_title(msg);   break;
        case DOBTABLE_SET_HEADERS:  handle_set_headers(msg); break;
        case DOBTABLE_ADD_ROWS:     handle_add_rows(msg);    break;
        case DOBTABLE_CLEAR:        handle_clear(msg);       break;
        case DOBTABLE_SHOW:         handle_show(msg);        break;
        case DOBTABLE_CLOSE:        handle_close(msg);       break;
        default:
            /* Unknown opcode — reply with error so the caller's IPC
             * call terminates instead of hanging on a non-reply. */
            reply_status(msg, -1);
            break;
    }
}

/* Bootstrap */

int main(int argc, char **argv)
{
    /* argv[0] is the requested service name (no Unix-style program
     * name slot — see <dob/spawn.h>).  Without it we can't be
     * addressed; bail rather than registering something useless. */
    if (argc < 1 || !argv[0] || !argv[0][0])
    {
        debug_print("[DobTable] FATAL: service name missing in argv[0]\n");
        return 1;
    }

    int n = (int)strlen(argv[0]);
    if (n >= (int)sizeof(service_name)) n = (int)sizeof(service_name) - 1;
    memcpy(service_name, argv[0], (uint32_t)n);
    service_name[n] = '\0';

    /* dobui_run never returns. Window title is provisional — the
     * caller can change it via SET_TITLE before Show. */
    dobui_run("Tabella", DOBTABLE_WIN_W, DOBTABLE_WIN_H);
    return 0;
}
