/* MainDOB DobInterface 2.0 — foglio: toast.c
 *
 * Il toast: banner non bloccante su dv_layer dedicato a z=500 (sopra
 * le finestre, sotto il cursore). Nato per i crash dei processi e i
 * messaggi dei componenti senza finestra: da dobinterface non si puo'
 * usare dobpopup (deadlock: popup -> CreateWindow IPC -> dobinterface,
 * che e' bloccato in attesa della reply del popup). Persiste finche'
 * non viene cliccato — niente auto-dismiss (sotto lag, un toast a
 * tempo puo' scadere prima che l'utente lo veda).
 *
 * Il layer e' a cmdlist (0 VRAM): [fill_rect della pillola,
 * draw_glyphs del testo]; la visibilita' e' l'interruttore
 * show/hide, la cmdlist si ricostruisce a ogni nuovo messaggio. */

#include "di_internal.h"

/* ================= stato condiviso (proprieta' di questo foglio) ====== */

bool toast_active = false;

/* ================= stato privato ====================================== */

static char     toast_text[128];
static uint32_t toast_start_ms = 0;

static dv_layer_t   g_toast_layer   = DV_HANDLE_NONE;
static dv_cmdlist_t g_toast_cmdlist = DV_HANDLE_NONE;

/* ================= verbi esecutivi ==================================== */

/* Ricostruisce la cmdlist per il toast_text corrente e mostra il
 * layer. Coordinate locali al canvas del layer (0..SCREEN_W x
 * 0..TOAST_LAYER_H); il dst_rect trasla a TOAST_LAYER_Y. Il
 * chiamante garantisce toast_active. */
static void toast_layer_refresh(void)
{
    if (g_toast_cmdlist == DV_HANDLE_NONE || g_toast_layer == DV_HANDLE_NONE) return;

    int tw = (int)font_string_width(toast_text) + 20;
    int tx = (desktop_w - tw) / 2;
    if (tx < 0) tx = 0;
    if (tx + tw > SCREEN_W) tw = SCREEN_W - tx;

    dv_cmdlist_reset(g_toast_cmdlist);

    /* Pillola — 0xCC3333 ad alpha pieno. */
    dv_color_t pill = { .b = 0x33, .g = 0x33, .r = 0xCC, .a = 0xFF };
    dv_rect_t  pill_r = { tx, 0, (uint32_t)tw, TOAST_LAYER_H };
    dv_cmdlist_fill_rect(g_toast_cmdlist, pill_r, pill);

    /* Testo bianco centrato in verticale. */
    dv_glyph_t glyphs[160];
    uint32_t   gcount = string_to_glyphs(toast_text, tx + 10,
                                         5,   /* baseline nella pillola */
                                         glyphs, 160);
    if (gcount > 0 && g_glyph_atlas != DV_HANDLE_NONE)
    {
        dv_color_t white = { .b = 0xFF, .g = 0xFF, .r = 0xFF, .a = 0xFF };
        dv_cmdlist_draw_glyphs(g_toast_cmdlist, g_glyph_atlas,
                               glyphs, gcount, white);
    }

    /* dv_layer_set_visible: update a campo singolo, piu' economico di
     * un dv_layer_update pieno. */
    dv_layer_set_visible(g_toast_layer, true);
}

/* ================= verbi pubblici del foglio ========================== */

/* Porta su il layer del toast (best-effort, da video_init). */
void toast_init_video(void)
{
    if (g_toast_layer != DV_HANDLE_NONE) return;         /* gia' su */

    /* Cmdlist dimensionata per: 1 fill_rect (~21 B) + 1 header
     * draw_glyphs (~13 B) + fino a ~120 glifi (12 B l'uno = 1440 B).
     * 2 KiB lascia ampio margine al toast realistico piu' lungo. */
    if (dv_cmdlist_create(g_vproc, 2048, &g_toast_cmdlist) != DV_OK)
    {
        debug_print("[dobinterface] toast cmdlist create failed.\n");
        g_toast_cmdlist = DV_HANDLE_NONE;
        return;
    }

    dv_layer_desc_t ld = {
        .cmdlist          = g_toast_cmdlist,
        .z                = 500,
        .alpha            = 255,
        .visible          = false,        /* mostrato da toast_show */
        .use_pixel_alpha  = false,        /* op opache dirette      */
        .src_rect         = { 0, 0, 0, 0 },
        .dst_rect         = { 0, TOAST_LAYER_Y, SCREEN_W, TOAST_LAYER_H },
    };
    if (dv_layer_create(g_vproc, &ld, &g_toast_layer) != DV_OK)
    {
        dv_cmdlist_destroy(g_toast_cmdlist);
        g_toast_cmdlist = DV_HANDLE_NONE;
        g_toast_layer   = DV_HANDLE_NONE;
        return;
    }
    debug_print("[dobinterface] toast layer up (z=500, cmdlist-based, 0 VRAM).\n");
}

void toast_layer_hide(void)
{
    if (g_toast_layer == DV_HANDLE_NONE) return;
    dv_layer_set_visible(g_toast_layer, false);
}

/* dst_rect a misura nuova dopo un cambio di modalita' video. */
void toast_layer_relayout(void)
{
    if (g_toast_layer == DV_HANDLE_NONE) return;
    dv_layer_desc_t ld = {
        .cmdlist  = g_toast_cmdlist, .z = 500, .alpha = 255,
        .visible  = toast_active, .use_pixel_alpha = false,
        .src_rect = { 0, 0, 0, 0 },
        .dst_rect = { 0, TOAST_LAYER_Y, SCREEN_W, TOAST_LAYER_H },
    };
    dv_layer_update(g_toast_layer, &ld);
}

/* ================= logica ad alto livello ============================= */

/* toast_show — mostra il banner. Se il layer non e' nato (init
 * best-effort fallito), il toast semplicemente non si vede e resta
 * il repaint pieno come ripiego. */
void toast_show(const char *msg)
{
    strncpy(toast_text, msg, sizeof(toast_text) - 1);
    toast_text[sizeof(toast_text) - 1] = '\0';
    toast_start_ms = (uint32_t)clock_ms();
    toast_active = true;

    if (g_toast_layer != DV_HANDLE_NONE)
    {
        toast_layer_refresh();
        fb_flip();
    }
    else
    {
        di_mark_dirty(DIRTY_FULL);
    }
}
