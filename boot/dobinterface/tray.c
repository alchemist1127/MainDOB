/* MainDOB DobInterface 2.0 — foglio: tray.c
 *
 * Il tray di sistema (pannello widget, aperto dal '<' del footer):
 * mini-finestre SHM impilate. Ogni widget e' un buffer condiviso che
 * il programma proprietario rasterizza (via cmdbuf, foglio cmdbuf) e
 * una texture di cache lato server; il tray e' un dv_layer a cmdlist
 * fullscreen (0 VRAM) a WPANEL_LAYER_Z=120 — sopra ogni finestra
 * (10..73) e l'anteprima servo (100), sotto toast (500) e cursore
 * (999): il tray salta fuori sopra le finestre, come deve fare un
 * tray, senza disturbare il resto del backbuf. wpanel_draw resetta
 * la cmdlist a ogni frame e registra a coordinate schermo assolute:
 * i cambi di geometria non richiedono resize del layer.
 *
 * Lezione 1.x incisa nel blit: blit OPACO (dv_cmdlist_blit), NON
 * blit_alpha con use_pixel_alpha — le costanti colore MainDOB sono
 * 0x00BBGGRR (byte alpha = 0) e use_pixel_alpha=true tratterebbe
 * ogni pixel come trasparente: widget interamente invisibile, resta
 * solo il nero del pannello sotto (il sintomo "widget tutto nero"). */

#include "di_internal.h"

/* ================= stato condiviso (proprieta' di questo foglio) ====== */

widget_slot_t widgets[MAX_WIDGETS];
bool          widget_panel_open = false;
int           widget_hover_idx  = -1;   /* slot sotto il mouse (-1 = nessuno) */
int           widget_grab_idx   = -1;   /* slot in drag (-1 = nessuno)        */

/* ================= stato privato ====================================== */

static uint32_t next_widget_id = 1;

static int widget_scroll = 0;           /* offset di scroll in px */

/* Geometria del pannello — ricalcolata all'apertura e su add/remove */
static int wpanel_x = 0, wpanel_y = 0;
static int wpanel_w = 0, wpanel_h = 0;
static int wpanel_content_h = 0;        /* altezza impilata totale (puo' eccedere) */

static dv_layer_t   g_wpanel_layer   = DV_HANDLE_NONE;
static dv_cmdlist_t g_wpanel_cmdlist = DV_HANDLE_NONE;
static bool         g_wpanel_layer_visible = false;

/* ================= verbi esecutivi ==================================== */

int widget_find_by_id(uint32_t id)
{
    for (int i = 0; i < MAX_WIDGETS; i++)
    {
        if (widgets[i].used && widgets[i].id == id)
            return i;
    }
    return -1;
}

/* Toggle di visibilita' del layer, con guardia: il caso comune
 * (stato invariato) non costa nulla. */
static void wpanel_set_layer_visible(bool v)
{
    if (v == g_wpanel_layer_visible) return;
    g_wpanel_layer_visible = v;
    if (g_wpanel_layer != DV_HANDLE_NONE)
        dv_layer_set_visible(g_wpanel_layer, v);
}

/* Registra un rettangolo pieno nella cmdlist del tray (0x00BBGGRR). */
static void wpanel_fill(int x, int y, int w, int h, uint32_t color)
{
    if (w <= 0 || h <= 0) return;
    dv_rect_t r = { (int32_t)x, (int32_t)y, (uint32_t)w, (uint32_t)h };
    dv_cmdlist_fill_rect(g_wpanel_cmdlist, r, dv_color_from_u32(color));
}

bool wpanel_contains(int mx, int my)
{
    return mx >= wpanel_x && mx < wpanel_x + wpanel_w &&
           my >= wpanel_y && my < wpanel_y + wpanel_h;
}

/* ================= terra di mezzo: geometria e ciclo di vita ========== */

/* Ricalcola la geometria dai widget registrati. Ancorato in basso a
 * destra, a sinistra del pannello comandi. [PX] Il pannello e' alto
 * ESATTAMENTE quanto i suoi widget: clamp solo se la pila davvero
 * non entra nello schermo (raro; ci pensa lo scroll). Il vecchio cap
 * a 3/4 dello schermo nascondeva in silenzio widget che sarebbero
 * entrati — orologio 320px sotto mixer 200px e keymap 200px lo
 * superavano gia', e gli ultimi widget sparivano senza indizi. */
void wpanel_calc_geometry(void)
{
    int max_w = 0;
    int total_h = WIDGET_PAD;

    for (int i = 0; i < MAX_WIDGETS; i++)
    {
        if (!widgets[i].used) continue;
        if (widgets[i].width > max_w) max_w = widgets[i].width;
        total_h += widgets[i].height + WIDGET_GAP;
    }
    if (total_h > WIDGET_PAD)
        total_h = total_h - WIDGET_GAP + WIDGET_PAD;  /* togli il gap finale, pad in fondo */
    else
        total_h = WIDGET_PAD * 2;  /* pannello vuoto */

    wpanel_content_h = total_h;
    wpanel_w = max_w + WIDGET_PAD * 2;
    if (wpanel_w < 120) wpanel_w = 120;

    wpanel_h = total_h;
    {
        int avail = SCREEN_H - 8;   /* qualche px sopra/sotto */
        if (wpanel_h > avail)
            wpanel_h = avail;
    }

    wpanel_x = panel_x - wpanel_w - 2;
    wpanel_y = SCREEN_H - wpanel_h - 4;

    /* Serra lo scroll. */
    int max_scroll = wpanel_content_h - wpanel_h;
    if (max_scroll < 0) max_scroll = 0;
    if (widget_scroll > max_scroll) widget_scroll = max_scroll;
    if (widget_scroll < 0) widget_scroll = 0;
}

/* Apre il tray: azzera scroll e hover, ricalcola la geometria.
 * (Il conteggio widget lo fa il chiamante: a zero widget mostra il
 * toast e non apre.) */
void wpanel_open(void)
{
    widget_panel_open = true;
    widget_scroll = 0;
    widget_hover_idx = -1;
    wpanel_calc_geometry();
}

/* Scroll del tray di delta_px, serrato al contenuto. Ritorna true se
 * la posizione e' cambiata (=> repaint). */
bool wpanel_scroll_by(int delta_px)
{
    int old_ws = widget_scroll;
    widget_scroll += delta_px;
    int max_ws = wpanel_content_h - wpanel_h;
    if (max_ws < 0) max_ws = 0;
    if (widget_scroll < 0) widget_scroll = 0;
    if (widget_scroll > max_ws) widget_scroll = max_ws;
    return widget_scroll != old_ws;
}

/* Porta su il layer del tray. Specchio di toast_init_video: layer a
 * cmdlist, fullscreen, 0 VRAM, mostrato solo a tray aperto. */
void wpanel_init_video(void)
{
    if (g_wpanel_layer != DV_HANDLE_NONE) return;        /* gia' su */

    /* Dimensionata per: 1 fill bg + 4 strisce bordo + fino a
     * MAX_WIDGETS blit. Ogni op sta ben sotto i 32 B; 4 KiB abbonda. */
    if (dv_cmdlist_create(g_vproc, 4096, &g_wpanel_cmdlist) != DV_OK)
    {
        debug_print("[dobinterface] wpanel cmdlist create failed.\n");
        g_wpanel_cmdlist = DV_HANDLE_NONE;
        return;
    }

    dv_layer_desc_t ld = {
        .cmdlist          = g_wpanel_cmdlist,
        .z                = WPANEL_LAYER_Z,
        .alpha            = 255,
        .visible          = false,        /* mostrato da wpanel_draw */
        .use_pixel_alpha  = false,        /* op opache               */
        .src_rect         = { 0, 0, 0, 0 },
        .dst_rect         = { 0, 0, SCREEN_W, SCREEN_H },
    };
    if (dv_layer_create(g_vproc, &ld, &g_wpanel_layer) != DV_OK)
    {
        dv_cmdlist_destroy(g_wpanel_cmdlist);
        g_wpanel_cmdlist = DV_HANDLE_NONE;
        g_wpanel_layer   = DV_HANDLE_NONE;
        return;
    }
    debug_print("[dobinterface] wpanel layer up (z=120, cmdlist-based, 0 VRAM).\n");
}

/* dst_rect a misura nuova dopo un cambio di modalita' video. */
void wpanel_layer_relayout(void)
{
    if (g_wpanel_layer == DV_HANDLE_NONE) return;
    dv_layer_desc_t ld = {
        .cmdlist  = g_wpanel_cmdlist, .z = WPANEL_LAYER_Z, .alpha = 255,
        .visible  = g_wpanel_layer_visible, .use_pixel_alpha = false,
        .src_rect = { 0, 0, 0, 0 },
        .dst_rect = { 0, 0, SCREEN_W, SCREEN_H },
    };
    dv_layer_update(g_wpanel_layer, &ld);
}

/* Nuovo slot widget: SHM del framebuffer + texture di cache.
 * Best-effort sulla texture: senza dv_* o con la quota tirata il
 * widget resta senza tex e wpanel_draw ne salta il blit (l'area
 * rende come bg del pannello) — meglio che fallire in silenzio. */
int widget_create(int w, int h, pid_t owner_pid, uint32_t owner_port)
{
    int idx = -1;
    for (int i = 0; i < MAX_WIDGETS; i++)
    {
        if (!widgets[i].used) { idx = i; break; }
    }
    if (idx < 0) return -1;

    uint32_t fb_vaddr = 0;
    uint32_t fb_size = (uint32_t)(w * h) * 4;
    int shm_id = shm_create(fb_size, &fb_vaddr);
    if (shm_id < 0 || !fb_vaddr) return -1;

    /* Pulizia al colore di sfondo del pannello. */
    uint32_t *buf = (uint32_t *)fb_vaddr;
    for (int i = 0; i < w * h; i++) buf[i] = COLOR_PANEL_BG;

    widget_slot_t *ws = &widgets[idx];
    memset(ws, 0, sizeof(*ws));
    ws->used        = true;
    ws->id          = next_widget_id++;
    ws->owner_pid   = owner_pid;
    ws->owner_port  = owner_port;
    ws->width       = w;
    ws->height      = h;
    ws->shm_id      = shm_id;
    ws->buffer      = buf;
    ws->content_dirty = true;
    ws->cache_tex   = DV_HANDLE_NONE;

    if (w > 0 && h > 0)
    {
        dv_texture_desc_t td = {
            .width      = (uint32_t)w,
            .height     = (uint32_t)h,
            .format     = DV_FMT_BGRA8888,
            .mip_levels = 1,
            /* SYSRAM: letta dalla CPU a ogni bake (vedi scratch). */
            .flags      = DV_TEX_FLAG_DYNAMIC | DV_SURF_FLAG_SYSRAM,
        };
        int32_t trc = dv_texture_create(g_vproc, &td, &ws->cache_tex);
        if (trc != DV_OK)
        {
            ws->cache_tex = DV_HANDLE_NONE;
            char dl[120];
            sprintf(dl, "[diag] widget %d cache_tex %dx%d FAILED rc=%d\n",
                    idx, w, h, (int)trc);
            debug_print(dl);
        }
        else
        {
            char dl[100];
            sprintf(dl, "[diag] widget %d cache_tex %dx%d created handle=%u\n",
                    idx, w, h, (unsigned)ws->cache_tex);
            debug_print(dl);
        }
    }
    else
    {
        char dl[100];
        sprintf(dl, "[diag] widget %d cache_tex SKIP (w=%d h=%d)\n",
                idx, w, h);
        debug_print(dl);
    }

    return idx;
}

void widget_destroy(int idx)
{
    if (idx < 0 || idx >= MAX_WIDGETS) return;
    widget_slot_t *ws = &widgets[idx];
    if (!ws->used) return;

    if (ws->cache_tex != DV_HANDLE_NONE)
    {
        dv_texture_destroy(ws->cache_tex);
        ws->cache_tex = DV_HANDLE_NONE;
    }

    if (ws->shm_id >= 0)
        shm_unmap(ws->shm_id);

    cmdbuf_reasm_free(&ws->reasm);

    ws->used   = false;
    ws->buffer = NULL;
    ws->shm_id = -1;
}

/* Pulizia dei widget di un processo morto. */
void widget_cleanup_for_pid(pid_t pid)
{
    for (int i = 0; i < MAX_WIDGETS; i++)
    {
        if (widgets[i].used && widgets[i].owner_pid == pid)
            widget_destroy(i);
    }
}

/* ================= hit-test e inoltro input =========================== */

/* Hit-test: indice dello slot widget o -1. */
int wpanel_hit_test(int mx, int my)
{
    if (!widget_panel_open) return -1;
    if (!wpanel_contains(mx, my)) return -1;

    for (int i = 0; i < MAX_WIDGETS; i++)
    {
        widget_slot_t *ws = &widgets[i];
        if (!ws->used) continue;
        if (my >= ws->hit_y && my < ws->hit_y + ws->hit_h)
            return i;
    }
    return -1;
}

/* Inoltra un evento mouse (click, drag o release) al proprietario del
 * widget. etype: 1 = click (button down), 2 = release (button up),
 * 6 = drag (mossa a bottone premuto). I client che badano solo ai
 * click possono ignorare arg3 e trattare tutto come click — ma
 * dovrebbero filtrare a etype == 1 per non raddoppiare sui drag. */
void widget_send_mouse(int idx, int mx, int my, uint32_t etype)
{
    widget_slot_t *ws = &widgets[idx];
    if (!ws->used || ws->owner_port == 0) return;

    int rel_x = mx - (wpanel_x + WIDGET_PAD);
    int rel_y = my - ws->hit_y;

    /* Serra ai confini della surface del widget. */
    if (rel_x < 0) rel_x = 0;
    if (rel_x >= ws->width) rel_x = ws->width - 1;
    if (rel_y < 0) rel_y = 0;
    if (rel_y >= ws->height) rel_y = ws->height - 1;

    dob_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.code = GUI_EVT_WIDGET_CLICK;
    msg.arg0 = ws->id;
    msg.arg1 = (uint32_t)rel_x;
    msg.arg2 = (uint32_t)rel_y;
    msg.arg3 = etype;
    dob_ipc_post(ws->owner_port, &msg);
}

/* ================= logica ad alto livello: disegno ==================== */

/* Disegna il tray sul suo dv_layer. Il layer copre tutto lo schermo,
 * quindi le coordinate restano assolute (la cmdlist registra op solo
 * dove il tray e' davvero — il resto del layer e' pass-through).
 * Chiamata a ogni compose: da chiuso nasconde il layer e ritorna
 * (un solo check di visibilita' con guardia). Il bordo si disegna
 * per ULTIMO cosi' il contenuto non lo sovrascrive mai. */
void wpanel_draw(void)
{
    if (!widget_panel_open
        || g_wpanel_cmdlist == DV_HANDLE_NONE
        || g_wpanel_layer == DV_HANDLE_NONE)
    {
        wpanel_set_layer_visible(false);
        return;
    }

    dv_cmdlist_reset(g_wpanel_cmdlist);

    /* Sfondo. */
    wpanel_fill(wpanel_x, wpanel_y, wpanel_w, wpanel_h, COLOR_PANEL_BG);

    /* Regione di clip rientrata di 1 px per preservare il bordo. */
    int clip_y0 = wpanel_y + 1;
    int clip_y1 = wpanel_y + wpanel_h - 1;
    int clip_x1 = wpanel_x + wpanel_w - WIDGET_PAD - 1;

    int y = wpanel_y + WIDGET_PAD - widget_scroll;

    for (int i = 0; i < MAX_WIDGETS; i++)
    {
        widget_slot_t *ws = &widgets[i];
        if (!ws->used) continue;

        int draw_x = wpanel_x + WIDGET_PAD;
        int draw_y = y;
        int draw_h = ws->height;

        /* Registra la hit zone (coordinate assolute, pre-clip). */
        ws->hit_y = draw_y;
        ws->hit_h = draw_h;

        /* Clip: salta se del tutto fuori dall'area visibile. */
        if (draw_y + draw_h <= clip_y0 || draw_y >= clip_y1)
        {
            y += draw_h + WIDGET_GAP;
            continue;
        }

        /* Blit della texture di cache nella cmdlist del tray, serrato
         * all'area visibile. Blit OPACO (vedi lezione in testa). */
        if (ws->cache_tex != DV_HANDLE_NONE)
        {
            int src_y0 = 0;
            int dst_y0 = draw_y;
            if (dst_y0 < clip_y0)
            {
                src_y0 = clip_y0 - dst_y0;
                dst_y0 = clip_y0;
            }
            int dst_y1 = draw_y + draw_h;
            if (dst_y1 > clip_y1) dst_y1 = clip_y1;
            int copy_h = dst_y1 - dst_y0;

            int copy_w = ws->width;
            if (draw_x + copy_w > clip_x1) copy_w = clip_x1 - draw_x;

            if (copy_w > 0 && copy_h > 0)
            {
                dv_rect_t  sr = { 0, (uint32_t)src_y0,
                                  (uint32_t)copy_w, (uint32_t)copy_h };
                dv_point_t dp = { draw_x, dst_y0 };
                dv_cmdlist_blit(g_wpanel_cmdlist,
                                (dv_surface_t)ws->cache_tex,
                                sr, dp);
            }
        }

        y += draw_h + WIDGET_GAP;
    }

    /* Bordo per ultimo — quattro strisce da 1 px. */
    wpanel_fill(wpanel_x,                wpanel_y,                wpanel_w, 1,        COLOR_STRUCT_LINE);
    wpanel_fill(wpanel_x,                wpanel_y + wpanel_h - 1, wpanel_w, 1,        COLOR_STRUCT_LINE);
    wpanel_fill(wpanel_x,                wpanel_y,                1,        wpanel_h, COLOR_STRUCT_LINE);
    wpanel_fill(wpanel_x + wpanel_w - 1, wpanel_y,                1,        wpanel_h, COLOR_STRUCT_LINE);

    wpanel_set_layer_visible(true);
}
