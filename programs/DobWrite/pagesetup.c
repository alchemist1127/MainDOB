/* pagesetup.c -- see pagesetup.h. */
#include "pagesetup.h"
#include <DobInterface.h>
#include <dobui_theme.h>
#include <stdbool.h>
#include <textbox.h>
#include <button.h>
#include <string.h>

#define PS_W 312
#define PS_H 282

/* Fields: 0=width 1=height 2=marginL 3=marginR 4=marginT 5=marginB */
typedef struct {
    dobui_win_t  *win;
    dob_textbox_t tb[6];
    dob_button_t  u_btn[3];            /* unit: 0=px 1=cm 2=in */
    dob_button_t  b_def, b_apply, b_cancel;
    dob_button_t  b_scope[2];         /* scope: 0=whole doc, 1=current section */
    dob_button_t  b_cols[6];          /* column count 1..6                      */
    int           unit;
    int           scope;
    int           cols;               /* selected column count 1..6             */
    int           focus;              /* focused textbox index, -1 none */
    uint32_t      tw[6];              /* working values, in twips */
    uint32_t      btn_bg;            /* captured default button background */
    PageSetup     base;              /* original setup (keeps fields we don't edit) */
    pagesetup_apply_cb on_apply;
    void         *ud;
} ps_state;

static ps_state g_ps;

/* ---- unit conversions, integer only (no libc float formatting) ----
 * twips: 1 inch = 1440. px at 96 dpi: 1 px = 15 twips.
 * cm/inch are carried as hundredths of the unit so two decimals survive. */
static long tw_to_disp(uint32_t tw, int unit, int *is_int)
{
    if (unit == 0) { *is_int = 1; return ((long)tw + 7) / 15; }      /* whole px */
    *is_int = 0;
    if (unit == 1) return ((long)tw * 254 + 720) / 1440;            /* cm * 100  */
    return ((long)tw * 100 + 720) / 1440;                          /* inch * 100 */
}
static uint32_t disp_to_tw(long v, int unit)
{
    if (v < 0) v = 0;
    if (unit == 0) return (uint32_t)(v * 15);
    if (unit == 1) return (uint32_t)((v * 1440 + 127) / 254);
    return (uint32_t)((v * 1440 + 50) / 100);
}

static void long_to_str(long v, char *out)
{
    char tmp[16]; int i = 0;
    if (v < 0) v = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v > 0 && i < 15) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    int k = 0; while (i > 0) out[k++] = tmp[--i]; out[k] = 0;
}
static void disp_to_str(long v, int is_int, char *out)
{
    if (is_int) { long_to_str(v, out); return; }
    long ip = v / 100, fp = v % 100; if (fp < 0) fp = -fp;
    char a[16]; long_to_str(ip, a);
    int k = 0; while (a[k]) { out[k] = a[k]; k++; }
    out[k++] = ',';                                      /* Italian decimal comma */
    out[k++] = (char)('0' + (int)(fp / 10));
    out[k++] = (char)('0' + (int)(fp % 10));
    out[k] = 0;
}
static long str_to_disp(const char *s, int unit)
{
    long ip = 0, fp = 0; int fdig = 0; const char *p = s;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') { ip = ip * 10 + (*p - '0'); p++; }
    if (*p == '.' || *p == ',') {
        p++;
        while (*p >= '0' && *p <= '9' && fdig < 2) { fp = fp * 10 + (*p - '0'); p++; fdig++; }
    }
    while (fdig < 2) { fp *= 10; fdig++; }
    if (unit == 0) return ip;                            /* px: integer only */
    return ip * 100 + fp;                                /* hundredths */
}

/* ---- fields <-> working twips ---- */
static void ps_show_fields(void)
{
    for (int i = 0; i < 6; i++) {
        int is_int; long d = tw_to_disp(g_ps.tw[i], g_ps.unit, &is_int);
        char buf[24]; disp_to_str(d, is_int, buf);
        dobtb_SetText(&g_ps.tb[i], buf);
    }
}
static void ps_read_fields(void)
{
    for (int i = 0; i < 6; i++) {
        long d = str_to_disp(dobtb_GetText(&g_ps.tb[i]), g_ps.unit);
        g_ps.tw[i] = disp_to_tw(d, g_ps.unit);
    }
}

static void ps_draw(void)
{
    if (!g_ps.win) return;
    uint32_t wid = dobui_win_id(g_ps.win);
    dobui_FillRect(wid, 0, 0, PS_W, PS_H, DOBUI_SURFACE);
    dobui_DrawText(wid, 12, 16, "Unita:", DOBUI_TEXT, DOBUI_SURFACE);
    for (int i = 0; i < 3; i++) {
        g_ps.u_btn[i].col_bg = (i == g_ps.unit) ? 0x00A0C8F0u : g_ps.btn_bg;
        dobbtn_Draw(&g_ps.u_btn[i]);
    }
    dobui_DrawText(wid, 12, 48, "Larghezza:", DOBUI_TEXT, DOBUI_SURFACE);
    dobui_DrawText(wid, 12, 80, "Altezza:",   DOBUI_TEXT, DOBUI_SURFACE);
    dobtb_Draw(&g_ps.tb[0]);
    dobtb_Draw(&g_ps.tb[1]);
    dobui_DrawText(wid, 12, 116, "Margini:", DOBUI_TEXT, DOBUI_SURFACE);
    dobui_DrawText(wid, 12, 144,  "Sx:",  DOBUI_TEXT, DOBUI_SURFACE);
    dobui_DrawText(wid, 160, 144, "Dx:",  DOBUI_TEXT, DOBUI_SURFACE);
    dobui_DrawText(wid, 12, 176,  "Sup:", DOBUI_TEXT, DOBUI_SURFACE);
    dobui_DrawText(wid, 160, 176, "Inf:", DOBUI_TEXT, DOBUI_SURFACE);
    for (int i = 2; i < 6; i++) dobtb_Draw(&g_ps.tb[i]);
    dobui_DrawText(wid, 12, 200, "Ambito:", DOBUI_TEXT, DOBUI_SURFACE);
    for (int si = 0; si < 2; si++) {
        g_ps.b_scope[si].col_bg = (si == g_ps.scope) ? 0x00A0C8F0u : g_ps.btn_bg;
        dobbtn_Draw(&g_ps.b_scope[si]);
    }
    dobbtn_Draw(&g_ps.b_def);
    dobbtn_Draw(&g_ps.b_apply);
    dobbtn_Draw(&g_ps.b_cancel);
    dobui_Invalidate(wid);
}

static void ps_close(void)
{
    dobui_win_t *w = g_ps.win;
    g_ps.win = NULL;
    if (w) dobui_win_close(w);
}

static void ps_apply(void)
{
    ps_read_fields();
    PageSetup ps = g_ps.base;                            /* preserve bg_color etc. */
    ps.width  = g_ps.tw[0] ? g_ps.tw[0] : ps.width;
    ps.height = g_ps.tw[1] ? g_ps.tw[1] : ps.height;
    ps.margin_left   = g_ps.tw[2];
    ps.margin_right  = g_ps.tw[3];
    ps.margin_top    = g_ps.tw[4];
    ps.margin_bottom = g_ps.tw[5];
    /* keep at least some printable area */
    if (ps.margin_left + ps.margin_right >= ps.width)
        ps.margin_left = ps.margin_right = ps.width / 8;
    if (ps.margin_top + ps.margin_bottom >= ps.height)
        ps.margin_top = ps.margin_bottom = ps.height / 8;
    ps.columns    = g_ps.base.columns;      /* columns are owned by the columns popup now */
    ps.column_gap = g_ps.base.column_gap;
    if (g_ps.on_apply) g_ps.on_apply(&ps, g_ps.scope, g_ps.ud);
}

static void ps_defaults(void)
{
    g_ps.tw[0] = 11906; g_ps.tw[1] = 16838;             /* A4 */
    g_ps.tw[2] = g_ps.tw[3] = g_ps.tw[4] = g_ps.tw[5] = 1440;  /* 1 inch */
    ps_show_fields();
    ps_draw();
}

/* ---- callbacks ---- */
static void ps_on_start(dobui_win_t *w)
{
    g_ps.win = w;
    uint32_t wid = dobui_win_id(w);
    dobbtn_Init(&g_ps.u_btn[0], wid, 70,  12, 38, 22, "px");
    dobbtn_Init(&g_ps.u_btn[1], wid, 112, 12, 38, 22, "cm");
    dobbtn_Init(&g_ps.u_btn[2], wid, 154, 12, 38, 22, "in");
    g_ps.btn_bg = g_ps.u_btn[0].col_bg;                 /* theme default */
    dobtb_Init(&g_ps.tb[0], wid, 120, 44, 120, 22);
    dobtb_Init(&g_ps.tb[1], wid, 120, 76, 120, 22);
    dobtb_Init(&g_ps.tb[2], wid, 50,  140, 90, 22);
    dobtb_Init(&g_ps.tb[3], wid, 190, 140, 90, 22);
    dobtb_Init(&g_ps.tb[4], wid, 50,  172, 90, 22);
    dobtb_Init(&g_ps.tb[5], wid, 190, 172, 90, 22);
    dobbtn_Init(&g_ps.b_def,    wid, 12,  246, 100, 26, "Predefiniti");
    dobbtn_Init(&g_ps.b_apply,  wid, 130, 246, 80,  26, "Applica");
    dobbtn_Init(&g_ps.b_cancel, wid, 220, 246, 80,  26, "Annulla");
    dobbtn_Init(&g_ps.b_scope[0], wid, 12,  214, 142, 22, "Intero documento");
    dobbtn_Init(&g_ps.b_scope[1], wid, 160, 214, 140, 22, "Sezione corrente");
    g_ps.focus = -1;
    ps_show_fields();
    ps_draw();
}

static void ps_on_click(dobui_win_t *w, int x, int y, uint8_t buttons)
{
    (void)w; (void)buttons;
    for (int i = 0; i < 3; i++)
        if (dobbtn_OnClick(&g_ps.u_btn[i], x, y)) {
            dobbtn_OnRelease(&g_ps.u_btn[i]);
            if (g_ps.unit != i) { ps_read_fields(); g_ps.unit = i; ps_show_fields(); }
            ps_draw(); return;
        }
    if (dobbtn_OnClick(&g_ps.b_def, x, y))    { dobbtn_OnRelease(&g_ps.b_def);    ps_defaults(); return; }
    if (dobbtn_OnClick(&g_ps.b_apply, x, y))  { dobbtn_OnRelease(&g_ps.b_apply);  ps_apply(); ps_close(); return; }
    if (dobbtn_OnClick(&g_ps.b_cancel, x, y)) { dobbtn_OnRelease(&g_ps.b_cancel); ps_close(); return; }
    for (int si = 0; si < 2; si++)
        if (dobbtn_OnClick(&g_ps.b_scope[si], x, y)) { dobbtn_OnRelease(&g_ps.b_scope[si]); g_ps.scope = si; ps_draw(); return; }
    for (int i = 0; i < 6; i++)
        if (dobtb_OnClick(&g_ps.tb[i], x, y)) {
            g_ps.focus = i;
            for (int k = 0; k < 6; k++) dobtb_SetFocus(&g_ps.tb[k], k == i);
            ps_draw(); return;
        }
}

static void ps_on_key(dobui_win_t *w, uint8_t key)
{
    (void)w;
    if (key == 27) { ps_close(); return; }                       /* Esc    */
    if (key == 13 || key == 10) { ps_apply(); ps_close(); return; } /* Invio */
    if (g_ps.focus >= 0 && g_ps.focus < 6)
        if (dobtb_OnKey(&g_ps.tb[g_ps.focus], key)) ps_draw();
}

static void ps_on_release(dobui_win_t *w, int x, int y, uint8_t buttons)
{
    (void)w; (void)x; (void)y; (void)buttons;
    for (int i = 0; i < 3; i++) dobbtn_OnRelease(&g_ps.u_btn[i]);
    dobbtn_OnRelease(&g_ps.b_def);
    dobbtn_OnRelease(&g_ps.b_apply);
    dobbtn_OnRelease(&g_ps.b_cancel);
    for (int i = 0; i < 2; i++) dobbtn_OnRelease(&g_ps.b_scope[i]);
    for (int i = 0; i < 6; i++) dobtb_OnRelease(&g_ps.tb[i]);
    ps_draw();
}

/* Drag extends the selection in the focused field; double-click selects the
 * word (then the whole value on repeats). The dialog drives its widgets by
 * hand, so these events must be forwarded explicitly. */
static void ps_on_mousemove(dobui_win_t *w, int x, int y, uint8_t buttons)
{
    (void)w; (void)buttons;
    if (g_ps.focus >= 0 && g_ps.focus < 6 && dobtb_OnDrag(&g_ps.tb[g_ps.focus], x, y)) ps_draw();
}

static void ps_on_dblclick(dobui_win_t *w, int x, int y, uint8_t buttons)
{
    (void)w; (void)buttons;
    for (int i = 0; i < 6; i++)
        if (dobtb_OnDblClick(&g_ps.tb[i], x, y)) {
            g_ps.focus = i;
            for (int k = 0; k < 6; k++) dobtb_SetFocus(&g_ps.tb[k], k == i);
            ps_draw();
            return;
        }
}

static void ps_on_close(dobui_win_t *w) { (void)w; ps_close(); }

static const dobui_win_vtbl_t PS_VTBL = {
    .on_start     = ps_on_start,
    .on_key       = ps_on_key,
    .on_click     = ps_on_click,
    .on_dblclick  = ps_on_dblclick,
    .on_mousemove = ps_on_mousemove,
    .on_release   = ps_on_release,
    .on_close     = ps_on_close,
};

void pagesetup_open(dobui_win_t *parent, const PageSetup *current,
                    pagesetup_apply_cb on_apply, void *ud)
{
    if (g_ps.win) return;
    memset(&g_ps, 0, sizeof g_ps);
    g_ps.base = *current;
    g_ps.tw[0] = current->width;        g_ps.tw[1] = current->height;
    g_ps.tw[2] = current->margin_left;  g_ps.tw[3] = current->margin_right;
    g_ps.tw[4] = current->margin_top;   g_ps.tw[5] = current->margin_bottom;
    g_ps.unit = 1;                        /* default centimetres */
    g_ps.cols = (int)(current->columns < 1 ? 1 : (current->columns > 6 ? 6 : current->columns));
    g_ps.focus = -1;
    g_ps.on_apply = on_apply; g_ps.ud = ud;
    g_ps.win = dobui_dialog_open(parent, "Imposta pagina", PS_W, PS_H,
                                 &PS_VTBL, &g_ps, false /* modeless */,
                                 DOBUI_WIN_NORESIZE | DOBUI_WIN_NOMAXIMIZE);
}
