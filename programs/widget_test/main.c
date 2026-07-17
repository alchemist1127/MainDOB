/* Widget Test — Test program for the widget tray system.
 *
 * Creates a main window with instructions and a widget in the tray
 * with two buttons and a clickable text field.
 *
 *   [Ciao]   → popup "Ciao"
 *   [Mostra] → popup with textbox content
 *   textbox  → InputBox to change text
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <DobInterface.h>
#include <DobPopup.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/types.h>

/* GUI event codes */

#define GUI_EVT_KEY             202
#define GUI_EVT_MOUSE           201
#define GUI_EVT_CLOSE_REQ       210
#define GUI_EVT_RESIZE          212
#define GUI_EVT_WIDGET_CLICK    220

/* Colors */

#define COL_WIN_BG      0x00335588
#define COL_TEXT         0x00FFFFFF
#define COL_TEXT_DIM     0x00AABBCC

#define COL_WID_BG      0x00333344
#define COL_BTN_BG      0x00556688
#define COL_BTN_FG      0x00FFFFFF
#define COL_BTN_BORDER  0x00778899
#define COL_TB_BG       0x00222233
#define COL_TB_FG       0x00CCDDEE
#define COL_TB_BORDER   0x00667788

/* Layout constants */

#define FONT_W          8
#define FONT_H          16

/* Main window */
#define WIN_W           280
#define WIN_H           130

/* Widget surface */
#define WID_W           180
#define WID_H           80
#define WID_PAD         6

/* Textbox row (top of widget) */
#define TB_X            WID_PAD
#define TB_Y            WID_PAD
#define TB_W            (WID_W - 2 * WID_PAD)
#define TB_H            20

/* Button row (below textbox) */
#define BTN_GAP         8
#define BTN_W           ((TB_W - BTN_GAP) / 2)
#define BTN_H           24
#define BTN_Y           (TB_Y + TB_H + WID_PAD + 2)
#define BTN1_X          WID_PAD
#define BTN2_X          (BTN1_X + BTN_W + BTN_GAP)

/* State */

static uint32_t event_port;
static uint32_t win_id;
static uint32_t widget_id;

static dobui_widget_fb_ctx_t win_ctx;
static dobui_widget_fb_ctx_t wid_ctx;

static char tb_text[128] = "Scrivi qui...";

/* Hit test helper */

static bool hit(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw
        && y >= ry && y < ry + rh;
}

/* Draw — Main window */

static void draw_window(void)
{
    dobui_WidgetRestoreContext(&win_ctx);

    dobui_FillRect(win_id, 0, 0, WIN_W, WIN_H, COL_WIN_BG);

    int y = 16;
    dobui_DrawText(win_id, 20, y, "Widget Test", COL_TEXT, COL_WIN_BG);

    y += FONT_H + 12;
    dobui_DrawText(win_id, 20, y, "Clicca '<' nel pannello",
                   COL_TEXT_DIM, COL_WIN_BG);

    y += FONT_H + 4;
    dobui_DrawText(win_id, 20, y, "per aprire il widget tray.",
                   COL_TEXT_DIM, COL_WIN_BG);

    y += FONT_H + 12;
    dobui_DrawText(win_id, 20, y, "[Ciao]   -> popup Ciao",
                   COL_TEXT_DIM, COL_WIN_BG);

    y += FONT_H + 4;
    dobui_DrawText(win_id, 20, y, "[Mostra] -> popup testo",
                   COL_TEXT_DIM, COL_WIN_BG);

    dobui_Invalidate(win_id);
}

/* Draw — Widget */

static void draw_widget(void)
{
    dobui_WidgetRestoreContext(&wid_ctx);

    /* Background */
    dobui_FillRect(widget_id, 0, 0, WID_W, WID_H, COL_WID_BG);

    /* Textbox */
    dobui_FillRect(widget_id, TB_X, TB_Y, TB_W, TB_H, COL_TB_BG);
    dobui_DrawRect(widget_id, TB_X, TB_Y, TB_W, TB_H, COL_TB_BORDER);

    int max_chars = (TB_W - 8) / FONT_W;
    if (max_chars > 127) max_chars = 127;
    char display[128];
    int tlen = (int)strlen(tb_text);
    int dlen = (tlen > max_chars) ? max_chars : tlen;
    memcpy(display, tb_text, (uint32_t)dlen);
    display[dlen] = '\0';
    dobui_DrawText(widget_id, TB_X + 4,
                   TB_Y + (TB_H - FONT_H) / 2,
                   display, COL_TB_FG, COL_TB_BG);

    /* Button "Ciao" */
    dobui_FillRect(widget_id, BTN1_X, BTN_Y, BTN_W, BTN_H, COL_BTN_BG);
    dobui_DrawRect(widget_id, BTN1_X, BTN_Y, BTN_W, BTN_H, COL_BTN_BORDER);
    int tw1 = 4 * FONT_W;
    dobui_DrawText(widget_id,
                   BTN1_X + (BTN_W - tw1) / 2,
                   BTN_Y + (BTN_H - FONT_H) / 2,
                   "Ciao", COL_BTN_FG, COL_BTN_BG);

    /* Button "Mostra" */
    dobui_FillRect(widget_id, BTN2_X, BTN_Y, BTN_W, BTN_H, COL_BTN_BG);
    dobui_DrawRect(widget_id, BTN2_X, BTN_Y, BTN_W, BTN_H, COL_BTN_BORDER);
    int tw2 = 6 * FONT_W;
    dobui_DrawText(widget_id,
                   BTN2_X + (BTN_W - tw2) / 2,
                   BTN_Y + (BTN_H - FONT_H) / 2,
                   "Mostra", COL_BTN_FG, COL_BTN_BG);

    dobui_WidgetInvalidate(widget_id);

    /* Restore window context as default */
    dobui_WidgetRestoreContext(&win_ctx);
}

/* Widget click handler */

static void handle_widget_click(int rx, int ry)
{
    /* Textbox clicked → InputBox to edit text */
    if (hit(rx, ry, TB_X, TB_Y, TB_W, TB_H))
    {
        char new_text[128];
        memset(new_text, 0, sizeof(new_text));
        int result = dobpopup_InputBox("Modifica testo",
                                       "Inserisci il nuovo testo:",
                                       tb_text,
                                       new_text, sizeof(new_text));
        if (result == 0 && new_text[0] != '\0')
        {
            memset(tb_text, 0, sizeof(tb_text));
            int n = (int)strlen(new_text);
            if (n > 127) n = 127;
            memcpy(tb_text, new_text, (uint32_t)n);
            tb_text[n] = '\0';
        }
        draw_widget();
        return;
    }

    /* Button "Ciao" */
    if (hit(rx, ry, BTN1_X, BTN_Y, BTN_W, BTN_H))
    {
        dobpopup_Info("Ciao", "Ciao!");
        return;
    }

    /* Button "Mostra" */
    if (hit(rx, ry, BTN2_X, BTN_Y, BTN_W, BTN_H))
    {
        dobpopup_Info("Testo", tb_text);
        return;
    }
}

/* Main — custom event loop (widget events not in app framework) */

int main(void)
{
    /* Create event port */
    event_port = (uint32_t)port_create();

    /* Wait for dobinterface */
    dob_registry_wait("dobinterface", 5000);

    /* Create main window */
    win_id = dobui_CreateWindow(WIN_W, WIN_H, event_port, "Widget Test");
    if (win_id == 0)
        _exit(1);

    /* Save window drawing context */
    dobui_WidgetSaveContext(&win_ctx);

    /* Create widget in the tray */
    widget_id = dobui_CreateWidget(WID_W, WID_H, event_port);
    if (widget_id == 0)
    {
        dobui_DestroyWindow(win_id);
        _exit(1);
    }

    /* Save widget drawing context */
    dobui_WidgetSaveContext(&wid_ctx);

    /* Initial draw */
    draw_window();
    draw_widget();

    /* Event loop */
    for (;;)
    {
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        dob_ipc_receive(event_port, &msg);

        switch (msg.code)
        {
            case GUI_EVT_WIDGET_CLICK:
            {
                /* arg3 = etype (1=click, 2=release, 6=drag). This widget
                 * only wants clicks; ignore drag/release to avoid flooding
                 * the InputBox popup on textbox drag. Accept arg3 == 0
                 * as well for back-compat with senders that don't set it. */
                if (msg.arg3 != 0 && msg.arg3 != 1) break;
                int rx = (int)msg.arg1;
                int ry = (int)msg.arg2;
                handle_widget_click(rx, ry);
                break;
            }

            case GUI_EVT_CLOSE_REQ:
            {
                dobui_DestroyWidget(widget_id);
                dobui_DestroyWindow(win_id);
                _exit(0);
                break;
            }

            case GUI_EVT_RESIZE:
            {
                dobui_HandleResize((int)msg.arg1, (int)msg.arg2,
                                   (int)msg.arg3);
                /* Re-save window context after resize remap */
                dobui_WidgetSaveContext(&win_ctx);
                draw_window();
                break;
            }

            default:
                break;
        }
    }
}
