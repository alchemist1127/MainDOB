/* DobPopup (toast backend) — for windowless components.
 *
 * Programs with no window of their own (hotplug's DAS action runner,
 * the diagnostic tools) can't host a modal dialog.  This backend keeps
 * the dobpopup_* API but routes each message to the window manager as
 * a non-blocking toast banner (GUI_SHOW_TOAST), which dobinterface
 * renders on the desktop.
 *
 * Programs WITH a window + widget toolkit link DobPopup_stub.c instead,
 * which shows a real in-process modal dialog.
 *
 * A toast can't ask a question, so YESNO returns 0 (Yes) and InputBox
 * returns -1 (cancel).  The windowless callers only ever use Info and
 * Error, so nothing interactive is lost in practice.
 */

#include <DobPopup.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <stdio.h>
#include <string.h>

#define GUI_SHOW_TOAST  123

static uint32_t gui_port = 0;

static void post_toast(int severity, const char *title, const char *message)
{
    if (!gui_port) gui_port = dob_registry_find("dobinterface");
    if (!gui_port) return;   /* no WM running -> nothing to show */

    /* Collapse to one short line: "title: message" when both present. */
    char buf[128];
    int n;
    if (title && *title && message && *message)
        n = snprintf(buf, sizeof(buf), "%s: %s", title, message);
    else
        n = snprintf(buf, sizeof(buf), "%s",
                     (message && *message) ? message : (title ? title : ""));
    if (n < 0) n = 0;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;

    dob_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.code         = GUI_SHOW_TOAST;
    msg.arg0         = (uint32_t)severity;   /* 0=info, 2=error */
    msg.payload      = buf;
    msg.payload_size = (uint32_t)n + 1;
    dob_ipc_post(gui_port, &msg);
}

int dobpopup_Show(int type, const char *title, const char *message)
{
    if (type < 0 || type > 4) type = POPUP_INFO;
    post_toast(type, title, message);
    /* Non-interactive: no real choice. YESNO defaults to Yes (0). */
    return 0;
}

int dobpopup_InputBox(const char *title, const char *prompt,
                      const char *default_text,
                      char *out_text, uint32_t out_size)
{
    (void)default_text;
    /* At least surface what was being asked. */
    post_toast(POPUP_INFO, title, prompt);
    if (out_text && out_size > 0) out_text[0] = '\0';
    return -1;   /* cannot collect input without a window */
}
