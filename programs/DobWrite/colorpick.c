/* colorpick.c -- modeless RGB colour picker dialog (see colorpick.h).
 *
 * Built entirely from MainDOB widgets (slider, textbox, listview, button)
 * inside its own sub-window, so it never touches the caller's surface.
 * One instance at a time -> a single static state struct, no malloc/free.
 */
#include "colorpick.h"
#include <DobInterface.h>
#include <dobui_theme.h>
#include <slider.h>
#include <textbox.h>
#include <listview.h>
#include <button.h>
#include <string.h>

#define CP_W   380
#define CP_H   330
#define CP_M   12              /* outer margin */

/* ---- standard HTML/CSS named colours (names + parallel RGB) ---- */
static const char *CP_NAMES[] = {
    "aliceblue","antiquewhite","aqua","aquamarine","azure","beige","bisque","black",
    "blanchedalmond","blue","blueviolet","brown","burlywood","cadetblue","chartreuse","chocolate",
    "coral","cornflowerblue","cornsilk","crimson","cyan","darkblue","darkcyan","darkgoldenrod",
    "darkgray","darkgreen","darkkhaki","darkmagenta","darkolivegreen","darkorange","darkorchid","darkred",
    "darksalmon","darkseagreen","darkslateblue","darkslategray","darkturquoise","darkviolet","deeppink","deepskyblue",
    "dimgray","dodgerblue","firebrick","floralwhite","forestgreen","fuchsia","gainsboro","ghostwhite",
    "gold","goldenrod","gray","green","greenyellow","honeydew","hotpink","indianred",
    "indigo","ivory","khaki","lavender","lavenderblush","lawngreen","lemonchiffon","lightblue",
    "lightcoral","lightcyan","lightgoldenrodyellow","lightgray","lightgreen","lightpink","lightsalmon","lightseagreen",
    "lightskyblue","lightslategray","lightsteelblue","lightyellow","lime","limegreen","linen","magenta",
    "maroon","mediumaquamarine","mediumblue","mediumorchid","mediumpurple","mediumseagreen","mediumslateblue","mediumspringgreen",
    "mediumturquoise","mediumvioletred","midnightblue","mintcream","mistyrose","moccasin","navajowhite","navy",
    "oldlace","olive","olivedrab","orange","orangered","orchid","palegoldenrod","palegreen",
    "paleturquoise","palevioletred","papayawhip","peachpuff","peru","pink","plum","powderblue",
    "purple","rebeccapurple","red","rosybrown","royalblue","saddlebrown","salmon","sandybrown",
    "seagreen","seashell","sienna","silver","skyblue","slateblue","slategray","snow",
    "springgreen","steelblue","tan","teal","thistle","tomato","turquoise","violet",
    "wheat","white","whitesmoke","yellow","yellowgreen",
};
static const uint32_t CP_RGB[] = {
    0xF0F8FF,0xFAEBD7,0x00FFFF,0x7FFFD4,0xF0FFFF,0xF5F5DC,0xFFE4C4,0x000000,
    0xFFEBCD,0x0000FF,0x8A2BE2,0xA52A2A,0xDEB887,0x5F9EA0,0x7FFF00,0xD2691E,
    0xFF7F50,0x6495ED,0xFFF8DC,0xDC143C,0x00FFFF,0x00008B,0x008B8B,0xB8860B,
    0xA9A9A9,0x006400,0xBDB76B,0x8B008B,0x556B2F,0xFF8C00,0x9932CC,0x8B0000,
    0xE9967A,0x8FBC8F,0x483D8B,0x2F4F4F,0x00CED1,0x9400D3,0xFF1493,0x00BFFF,
    0x696969,0x1E90FF,0xB22222,0xFFFAF0,0x228B22,0xFF00FF,0xDCDCDC,0xF8F8FF,
    0xFFD700,0xDAA520,0x808080,0x008000,0xADFF2F,0xF0FFF0,0xFF69B4,0xCD5C5C,
    0x4B0082,0xFFFFF0,0xF0E68C,0xE6E6FA,0xFFF0F5,0x7CFC00,0xFFFACD,0xADD8E6,
    0xF08080,0xE0FFFF,0xFAFAD2,0xD3D3D3,0x90EE90,0xFFB6C1,0xFFA07A,0x20B2AA,
    0x87CEFA,0x778899,0xB0C4DE,0xFFFFE0,0x00FF00,0x32CD32,0xFAF0E6,0xFF00FF,
    0x800000,0x66CDAA,0x0000CD,0xBA55D3,0x9370DB,0x3CB371,0x7B68EE,0x00FA9A,
    0x48D1CC,0xC71585,0x191970,0xF5FFFA,0xFFE4E1,0xFFE4B5,0xFFDEAD,0x000080,
    0xFDF5E6,0x808000,0x6B8E23,0xFFA500,0xFF4500,0xDA70D6,0xEEE8AA,0x98FB98,
    0xAFEEEE,0xDB7093,0xFFEFD5,0xFFDAB9,0xCD853F,0xFFC0CB,0xDDA0DD,0xB0E0E6,
    0x800080,0x663399,0xFF0000,0xBC8F8F,0x4169E1,0x8B4513,0xFA8072,0xF4A460,
    0x2E8B57,0xFFF5EE,0xA0522D,0xC0C0C0,0x87CEEB,0x6A5ACD,0x708090,0xFFFAFA,
    0x00FF7F,0x4682B4,0xD2B48C,0x008080,0xD8BFD8,0xFF6347,0x40E0D0,0xEE82EE,
    0xF5DEB3,0xFFFFFF,0xF5F5F5,0xFFFF00,0x9ACD32,
};
#define CP_NCOLORS ((int)(sizeof(CP_RGB) / sizeof(CP_RGB[0])))

static const char *CP_LBL[3] = { "R:", "G:", "B:" };

/* ---- single-instance state ---- */
typedef struct {
    dobui_win_t   *win;
    dob_slider_t   sl[3];
    dob_textbox_t  tb[3];
    dob_listview_t lv;
    dob_button_t   ok, cancel;
    int            focus;          /* which value box has focus, -1 = none */
    colorpick_cb   cb;
    void          *ud;
    uint32_t       init_rgb;
    char           title[64];
} cp_state;

static cp_state g_cp;

/* ---- tiny self-contained conversions (no libc number formatting) ---- */
static void u8_to_str(int v, char *out)
{
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    if (v >= 100)      { out[0] = '0' + v / 100; out[1] = '0' + (v / 10) % 10; out[2] = '0' + v % 10; out[3] = 0; }
    else if (v >= 10)  { out[0] = '0' + v / 10;  out[1] = '0' + v % 10;        out[2] = 0; }
    else               { out[0] = '0' + v;       out[1] = 0; }
}
static int str_to_u8(const char *s)
{
    int v = 0;
    while (*s) { if (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); if (v > 255) v = 255; } s++; }
    return v;
}
static void hex6(uint32_t rgb, char *out)
{
    static const char H[] = "0123456789ABCDEF";
    out[0] = '#';
    out[1] = H[(rgb >> 20) & 0xF]; out[2] = H[(rgb >> 16) & 0xF];
    out[3] = H[(rgb >> 12) & 0xF]; out[4] = H[(rgb >> 8)  & 0xF];
    out[5] = H[(rgb >> 4)  & 0xF]; out[6] = H[rgb & 0xF];
    out[7] = 0;
}

/* current value of a channel (live during a drag, committed otherwise) */
static int cp_chan(int i) { return g_cp.sl[i].grabbed ? g_cp.sl[i].drag_value : g_cp.sl[i].value; }
static uint32_t cp_rgb(void)
{
    return ((uint32_t)cp_chan(0) << 16) | ((uint32_t)cp_chan(1) << 8) | (uint32_t)cp_chan(2);
}

static void cp_sync_tb(void)
{
    char b[8];
    for (int i = 0; i < 3; i++) { u8_to_str(cp_chan(i), b); dobtb_SetText(&g_cp.tb[i], b); }
}
static void cp_set_rgb(uint32_t rgb)
{
    dobsl_SetValue(&g_cp.sl[0], (int)((rgb >> 16) & 0xff));
    dobsl_SetValue(&g_cp.sl[1], (int)((rgb >> 8)  & 0xff));
    dobsl_SetValue(&g_cp.sl[2], (int)( rgb        & 0xff));
    cp_sync_tb();
}

static void cp_draw(void)
{
    if (!g_cp.win) return;
    uint32_t wid = dobui_win_id(g_cp.win);
    dobui_FillRect(wid, 0, 0, CP_W, CP_H, DOBUI_SURFACE);

    for (int i = 0; i < 3; i++) {
        int ry = 16 + i * 34;
        dobui_DrawText(wid, CP_M, ry, CP_LBL[i], DOBUI_TEXT, DOBUI_SURFACE);
        dobsl_Draw(&g_cp.sl[i]);
        dobtb_Draw(&g_cp.tb[i]);
    }

    /* preview swatch + hex code */
    uint32_t c = cp_rgb();
    int px = 298, py = 16, pw = 68, ph = 84;
    dobui_FillRect(wid, px - 1, py - 1, pw + 2, ph + 2, DOBUI_TEXT_ALT);   /* border */
    dobui_FillRect(wid, px, py, pw, ph, c);
    char hx[8]; hex6(c, hx);
    dobui_DrawText(wid, px + 6, py + ph + 6, hx, DOBUI_TEXT_ALT, DOBUI_SURFACE);

    dobui_DrawText(wid, CP_M, 110, "Colori standard:", DOBUI_TEXT, DOBUI_SURFACE);
    doblv_Draw(&g_cp.lv);

    dobbtn_Draw(&g_cp.ok);
    dobbtn_Draw(&g_cp.cancel);

    dobui_Invalidate(wid);
}

/* close (optionally delivering the colour) and free the slot for reuse */
static void cp_close(bool ok, uint32_t rgb)
{
    dobui_win_t *w = g_cp.win;
    colorpick_cb cb = g_cp.cb;
    void *ud = g_cp.ud;
    g_cp.win = NULL;                 /* re-arm before anything else */
    if (w) dobui_win_close(w);
    if (ok && cb) cb(rgb, ud);       /* deliver after the dialog is gone */
}

/* ---- per-window callbacks ---- */
static void cp_on_start(dobui_win_t *w)
{
    g_cp.win = w;
    uint32_t wid = dobui_win_id(w);

    for (int i = 0; i < 3; i++) {
        int ry = 16 + i * 34;
        dobsl_Init(&g_cp.sl[i], wid, 34, ry, 200, 16);
        dobsl_SetRange(&g_cp.sl[i], 0, 255);
        g_cp.sl[i].show_value = false;
        dobtb_Init(&g_cp.tb[i], wid, 242, ry - 3, 44, 22);
    }
    doblv_Init(&g_cp.lv, wid, CP_M, 126, CP_W - 2 * CP_M, 150);
    doblv_SetItems(&g_cp.lv, CP_NAMES, CP_NCOLORS);

    dobbtn_Init(&g_cp.ok,     wid, 298, 288, 68, 28, "OK");
    dobbtn_Init(&g_cp.cancel, wid, 222, 288, 68, 28, "Annulla");

    g_cp.focus = -1;
    cp_set_rgb(g_cp.init_rgb & 0x00FFFFFFu);
    cp_draw();
}

static void cp_on_click(dobui_win_t *w, int x, int y, uint8_t buttons)
{
    (void)w; (void)buttons;

    if (dobbtn_OnClick(&g_cp.ok, x, y))     { cp_close(true, cp_rgb()); return; }
    if (dobbtn_OnClick(&g_cp.cancel, x, y)) { cp_close(false, 0);       return; }

    bool moved = false;
    for (int i = 0; i < 3; i++) if (dobsl_OnClick(&g_cp.sl[i], x, y)) moved = true;
    if (moved) { g_cp.focus = -1; for (int j = 0; j < 3; j++) dobtb_SetFocus(&g_cp.tb[j], false); cp_sync_tb(); cp_draw(); return; }

    for (int i = 0; i < 3; i++) {
        if (dobtb_OnClick(&g_cp.tb[i], x, y)) {
            g_cp.focus = i;
            for (int j = 0; j < 3; j++) dobtb_SetFocus(&g_cp.tb[j], j == i);
            cp_draw();
            return;
        }
    }

    if (doblv_OnClick(&g_cp.lv, x, y)) {
        int sel = doblv_GetSelectedIndex(&g_cp.lv);
        if (sel >= 0 && sel < CP_NCOLORS) cp_set_rgb(CP_RGB[sel]);
        g_cp.focus = -1;
        for (int j = 0; j < 3; j++) dobtb_SetFocus(&g_cp.tb[j], false);
        cp_draw();
    }
}

static void cp_on_mousemove(dobui_win_t *w, int x, int y, uint8_t buttons)
{
    (void)w; (void)buttons;
    bool any = false;
    for (int i = 0; i < 3; i++) if (dobsl_OnDrag(&g_cp.sl[i], x, y)) any = true;
    if (any) { cp_sync_tb(); cp_draw(); return; }
    if (g_cp.focus >= 0 && g_cp.focus < 3 && dobtb_OnDrag(&g_cp.tb[g_cp.focus], x, y)) cp_draw();
}

static void cp_on_release(dobui_win_t *w, int x, int y, uint8_t buttons)
{
    (void)w; (void)x; (void)y; (void)buttons;
    for (int i = 0; i < 3; i++) dobsl_OnRelease(&g_cp.sl[i]);
    for (int i = 0; i < 3; i++) dobtb_OnRelease(&g_cp.tb[i]);
    dobbtn_OnRelease(&g_cp.ok);
    dobbtn_OnRelease(&g_cp.cancel);
    cp_sync_tb();
    cp_draw();
}

/* double-click selects the word (then the whole value on repeats) in an RGB box */
static void cp_on_dblclick(dobui_win_t *w, int x, int y, uint8_t buttons)
{
    (void)w; (void)buttons;
    for (int i = 0; i < 3; i++)
        if (dobtb_OnDblClick(&g_cp.tb[i], x, y)) {
            g_cp.focus = i;
            for (int j = 0; j < 3; j++) dobtb_SetFocus(&g_cp.tb[j], j == i);
            cp_draw();
            return;
        }
}

static void cp_on_key(dobui_win_t *w, uint8_t key)
{
    (void)w;
    if (key == 27)               { cp_close(false, 0);       return; }   /* Esc    -> Annulla */
    if (key == 13 || key == 10)  { cp_close(true, cp_rgb()); return; }   /* Invio  -> OK      */

    if (g_cp.focus >= 0 && g_cp.focus < 3) {
        if (dobtb_OnKey(&g_cp.tb[g_cp.focus], key)) {
            dobsl_SetValue(&g_cp.sl[g_cp.focus], str_to_u8(dobtb_GetText(&g_cp.tb[g_cp.focus])));
            cp_draw();                 /* note: do NOT re-sync the box -> keep what was typed */
        }
        return;
    }
    if (doblv_OnKey(&g_cp.lv, key)) {
        int sel = doblv_GetSelectedIndex(&g_cp.lv);
        if (sel >= 0 && sel < CP_NCOLORS) cp_set_rgb(CP_RGB[sel]);
        cp_draw();
    }
}

static void cp_on_scroll(dobui_win_t *w, int delta)
{
    (void)w;
    if (doblv_OnScroll(&g_cp.lv, delta)) cp_draw();
}

static void cp_on_close(dobui_win_t *w)
{
    (void)w;
    cp_close(false, 0);
}

static const dobui_win_vtbl_t CP_VTBL = {
    .on_start     = cp_on_start,
    .on_key       = cp_on_key,
    .on_click     = cp_on_click,
    .on_dblclick  = cp_on_dblclick,
    .on_mousemove = cp_on_mousemove,
    .on_release   = cp_on_release,
    .on_scroll    = cp_on_scroll,
    .on_close     = cp_on_close,
};

void colorpick_open(dobui_win_t *parent, const char *title,
                    uint32_t initial_rgb, colorpick_cb on_pick, void *user)
{
    if (g_cp.win) return;            /* one picker at a time */
    memset(&g_cp, 0, sizeof g_cp);
    g_cp.cb       = on_pick;
    g_cp.ud       = user;
    g_cp.init_rgb = initial_rgb;
    g_cp.focus    = -1;
    strncpy(g_cp.title, title ? title : "Colore", sizeof g_cp.title - 1);

    g_cp.win = dobui_dialog_open(parent, g_cp.title, CP_W, CP_H, &CP_VTBL,
                                 &g_cp, false /* modeless */,
                                 DOBUI_WIN_NORESIZE | DOBUI_WIN_NOMAXIMIZE);
    /* widgets are created and first-painted in cp_on_start */
}
