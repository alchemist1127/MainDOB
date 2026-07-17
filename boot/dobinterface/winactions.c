/* MainDOB DobInterface 2.0 — foglio: winactions.c
 *
 * Le azioni sulle finestre: applicazione della geometria (sposta e
 * ridimensiona), massimizza/ripristina via motore servo, e
 * l'anteprima tratteggiata del resize.
 *
 * L'anteprima servo e' quattro dv_layer sottili a z=100, uno per
 * bordo del rettangolo tratteggiato: due surface a striscia
 * (SCREEN_W x 1 e 1 x SCREEN_H, ~14 KiB totali) pre-dipinte UNA
 * volta a init col pattern 4-acceso/4-spento alla lunghezza massima,
 * invece di una surface fullscreen a pixel-alpha (3 MB) — stessa
 * resa a tre ordini di grandezza meno VRAM. L'update per-drag e'
 * quattro dv_layer_update (src_rect serra la lunghezza viva del
 * bordo, dst_rect lo posiziona): zero rendering, zero upload.
 *
 * Fix 1.x inglobati nel design:
 *  - move-only: solo retarget del layer, atomico alla prossima
 *    compose — nessun re-render, nessun upload;
 *  - resize: ordine crea nuova -> segnaposto stirato -> rebind del
 *    layer (handle stabile) -> distruggi vecchia, cosi' il layer non
 *    resta mai senza sorgente e l'utente vede subito QUALCOSA alle
 *    misure nuove;
 *  - CONTRATTO tex pool al resize: lo stub client azzera il proprio
 *    mirror SENZA mandare TEX_FREE ("il WM le ha gia' distrutte") e
 *    ri-alloca alla prima BlitBuffer. Il server DEVE quindi
 *    distruggere il pool qui, o si riempie di orfani e le alloc
 *    successive falliscono: icone della ribbon sparite dopo un solo
 *    resize. */

#include "di_internal.h"

/* ================= stato condiviso (proprieta' di questo foglio) ====== */

servo_t win_servo;

/* ================= stato privato: anteprima servo ===================== */

static dv_surface_t g_servo_preview_surf_h = DV_HANDLE_NONE; /* bordi orizzontali (W x 1) */
static dv_surface_t g_servo_preview_surf_v = DV_HANDLE_NONE; /* bordi verticali   (1 x H) */
static dv_layer_t   g_servo_preview_top    = DV_HANDLE_NONE;
static dv_layer_t   g_servo_preview_bot    = DV_HANDLE_NONE;
static dv_layer_t   g_servo_preview_left   = DV_HANDLE_NONE;
static dv_layer_t   g_servo_preview_right  = DV_HANDLE_NONE;
static bool         g_servo_preview_visible = false;
#define SERVO_PREVIEW_Z   100

/* ================= verbi esecutivi: anteprima servo =================== */

/* Crea un layer-bordo invisibile (1x1: src/dst veri li fissa il
 * primo servo_draw_preview). Ritorna false su fallimento. */
static bool servo_edge_layer_create(dv_layer_t *out, dv_surface_t surf,
                                    const char *name)
{
    dv_layer_desc_t ld = {
        .source = surf, .z = SERVO_PREVIEW_Z, .alpha = 255,
        .visible = false, .use_pixel_alpha = true,
        .src_rect = { 0, 0, 1, 1 },
        .dst_rect = { 0, 0, 1, 1 },
    };
    if (dv_layer_create(g_vproc, &ld, out) != DV_OK)
    {
        char line[96];
        sprintf(line, "[dobinterface] servo preview layer %s create failed.\n",
                name);
        debug_print(line);
        return false;
    }
    return true;
}

void servo_preview_hide(void)
{
    if (!g_servo_preview_visible) return;
    dv_layer_set_visible(g_servo_preview_top,   false);
    dv_layer_set_visible(g_servo_preview_bot,   false);
    dv_layer_set_visible(g_servo_preview_left,  false);
    dv_layer_set_visible(g_servo_preview_right, false);
    g_servo_preview_visible = false;
}

/* ================= terra di mezzo: init/teardown anteprima ============ */

/* Alloca le due strisce sottili, le pre-dipinge col tratteggio e
 * crea i quattro layer-bordo, tutti invisibili. servo_draw_preview
 * li rendera' visibili e li posizionera'. */
void servo_preview_init_video(void)
{
    /* Pattern del tratteggio costruito in RAM una volta, caricato
     * nelle due surface. */
    static uint32_t h_strip[8192];   /* fino a schermi larghi 8K */
    static uint32_t v_strip[8192];
    int hmax = SCREEN_W < 8192 ? SCREEN_W : 8192;
    int vmax = SCREEN_H < 8192 ? SCREEN_H : 8192;
    uint32_t white_bgra = 0xFF000000u
                        | ((uint32_t)((COLOR_WHITE >> 16) & 0xFF) << 16)
                        | ((uint32_t)((COLOR_WHITE >>  8) & 0xFF) <<  8)
                        |  (uint32_t)( COLOR_WHITE        & 0xFF);
    for (int i = 0; i < hmax; i++)
        h_strip[i] = ((i / 4) & 1) ? 0u : white_bgra;  /* 4-acceso, 4-spento; 0 = trasparente */
    for (int i = 0; i < vmax; i++)
        v_strip[i] = ((i / 4) & 1) ? 0u : white_bgra;

    dv_surface_desc_t sd_h = {
        .width = (uint32_t)hmax, .height = 1,
        .format = DV_FMT_BGRA8888, .flags = DV_SURF_FLAG_RENDERTARGET,
    };
    dv_surface_desc_t sd_v = {
        .width = 1, .height = (uint32_t)vmax,
        .format = DV_FMT_BGRA8888, .flags = DV_SURF_FLAG_RENDERTARGET,
    };
    if (dv_surface_create(g_vproc, &sd_h, &g_servo_preview_surf_h) != DV_OK ||
        dv_surface_create(g_vproc, &sd_v, &g_servo_preview_surf_v) != DV_OK)
    {
        debug_print("[dobinterface] servo preview surface create failed.\n");
        return;
    }
    dv_texture_update_region((dv_texture_t)g_servo_preview_surf_h,
                             (dv_rect_t){0,0,(uint32_t)hmax,1},
                             h_strip, (uint32_t)(hmax * 4));
    dv_texture_update_region((dv_texture_t)g_servo_preview_surf_v,
                             (dv_rect_t){0,0,1,(uint32_t)vmax},
                             v_strip, 4);

    if (!servo_edge_layer_create(&g_servo_preview_top,
                                 g_servo_preview_surf_h, "top"))    return;
    if (!servo_edge_layer_create(&g_servo_preview_bot,
                                 g_servo_preview_surf_h, "bottom")) return;
    if (!servo_edge_layer_create(&g_servo_preview_left,
                                 g_servo_preview_surf_v, "left"))   return;
    if (!servo_edge_layer_create(&g_servo_preview_right,
                                 g_servo_preview_surf_v, "right"))  return;

    g_servo_preview_visible = false;
    debug_print("[dobinterface] servo preview ready (4 edge layers, z=100).\n");
}

/* Smonta strisce e layer (relayout: verranno ricostruiti a misura). */
void servo_preview_teardown(void)
{
    if (g_servo_preview_top   != DV_HANDLE_NONE) dv_layer_destroy(g_servo_preview_top);
    if (g_servo_preview_bot   != DV_HANDLE_NONE) dv_layer_destroy(g_servo_preview_bot);
    if (g_servo_preview_left  != DV_HANDLE_NONE) dv_layer_destroy(g_servo_preview_left);
    if (g_servo_preview_right != DV_HANDLE_NONE) dv_layer_destroy(g_servo_preview_right);
    if (g_servo_preview_surf_h != DV_HANDLE_NONE) dv_surface_destroy(g_servo_preview_surf_h);
    if (g_servo_preview_surf_v != DV_HANDLE_NONE) dv_surface_destroy(g_servo_preview_surf_v);

    g_servo_preview_top   = DV_HANDLE_NONE;
    g_servo_preview_bot   = DV_HANDLE_NONE;
    g_servo_preview_left  = DV_HANDLE_NONE;
    g_servo_preview_right = DV_HANDLE_NONE;
    g_servo_preview_surf_h = DV_HANDLE_NONE;
    g_servo_preview_surf_v = DV_HANDLE_NONE;
    g_servo_preview_visible = false;
}

/* ================= logica ad alto livello ============================= */

/* win_apply_geometry — applica posizione e misura a una finestra.
 * Move-only: solo retarget del layer. Resize: nuova surface alle
 * misure nuove con la vecchia stirata come segnaposto finche' il
 * client non risponde al GUI_EVT_RESIZE con un frame fresco (poi
 * win_bake la sovrascrive comunque dal last_cmdbuf corrente). */
void win_apply_geometry(int idx, int new_x, int new_y, int new_w, int new_h)
{
    if (idx < 0 || !windows[idx].used) return;
    window_t *w = &windows[idx];

    bool size_changed = (new_w != w->width || new_h != w->height);

    /* La posizione si aggiorna sempre, subito. */
    w->x = new_x;
    w->y = new_y;

    if (!size_changed)
    {
        if (w->body_layer != DV_HANDLE_NONE)
            win_update_layer_pos(w);
        di_mark_dirty(DIRTY_FULL);
        return;
    }

    int old_sw = win_surf_w(w);            /* misure della surface uscente */
    int old_sh = win_surf_h(w);
    w->width   = new_w;
    w->height  = new_h;
    di_mark_dirty(DIRTY_FULL);

    if (w->body_surf != DV_HANDLE_NONE && w->body_layer != DV_HANDLE_NONE)
    {
        int nsw = win_surf_w(w);
        int nsh = win_surf_h(w);
        dv_surface_desc_t sd = {
            .width  = (uint32_t)nsw,
            .height = (uint32_t)nsh,
            .format = DV_FMT_BGRA8888,
            .flags  = DV_SURF_FLAG_SYSRAM,
        };
        dv_surface_t new_surf = DV_HANDLE_NONE;
        if (dv_surface_create(g_vproc, &sd, &new_surf) == DV_OK)
        {
            dv_surface_t old_surf = w->body_surf;
            dv_blit_stretched(old_surf,
                (dv_rect_t){ 0, 0, (uint32_t)old_sw, (uint32_t)old_sh },
                new_surf,
                (dv_rect_t){ 0, 0, (uint32_t)nsw, (uint32_t)nsh });
            w->body_surf = new_surf;
            win_update_layer_pos(w);          /* rebind + misure nuove */
            dv_surface_destroy(old_surf);

            win_tex_pool_free(w);   /* contratto resize (vedi testa) */
        }
        else
        {
            win_free_video(w);
            win_tex_pool_free(w);   /* contratto resize: mirror azzerato */
            win_alloc_video(w);
        }
    }
    else
    {
        /* Fallback: senza video (surface o layer mai nati) si passa
         * dal ciclo pieno free+alloc. */
        win_free_video(w);
        win_tex_pool_free(w);       /* contratto resize: mirror azzerato */
        win_alloc_video(w);
    }
    w->surface_dirty = true;

    /* Notifica il client. arg3 riservato (era l'id SHM); -1 per ABI. */
    send_win_event(idx, GUI_EVT_RESIZE, (uint32_t)new_w, (uint32_t)new_h,
                   (uint32_t)-1);
}

/* win_toggle_maximize — massimizza o ripristina via motore servo
 * (che ricorda la geometria di ripristino), poi allinea l'etichetta
 * del comando pannello. */
void win_toggle_maximize(int idx)
{
    if (idx < 0 || !windows[idx].used) return;
    window_t *w = &windows[idx];
    servo_result_t result;

    if (w->maximized)
    {
        if (!servo_restore(&win_servo, &result))
            return;

        w->maximized = false;
        win_apply_geometry(idx, result.x, result.y, result.w, result.h);
    }
    else
    {
        servo_maximize(&win_servo,
                       w->x, w->y, w->width, w->height,
                       desktop_w, SCREEN_H,
                       &result);

        win_apply_geometry(idx, result.x, result.y, result.w, result.h);
        w->maximized = true;
    }

    panel_set_maximize_label(w->maximized);  /* Ingrandisci/Finestra */
    panel_recalculate();
    di_mark_dirty(DIRTY_FULL);
}

/* servo_draw_preview — rettangolo tratteggiato dell'anteprima alle
 * misure vive del servo. Percorso principale: quattro dv_layer_update
 * (vedi testa). Fallback quando i layer dedicati non sono nati a
 * init: tratteggio come fill_rect sul backbuf — visivamente
 * equivalente durante il moto continuo; si perde solo l'update
 * geometry-only a costo zero dei layer dedicati. L'anteprima e'
 * serrata all'area desktop: mai disegnare nel pannello. */
void servo_draw_preview(void)
{
    if (!servo_is_active(&win_servo))
    {
        if (g_servo_preview_visible) servo_preview_hide();
        return;
    }

    int px = win_servo.target_x;
    int py = win_servo.target_y;
    int pw = win_servo.target_w;
    int ph = win_servo.target_h + WIN_HEADER_H;

    if (px + pw > desktop_w) pw = desktop_w - px;
    if (pw <= 0) return;

    if (g_servo_preview_top != DV_HANDLE_NONE)
    {
        dv_layer_desc_t top = {
            .source = g_servo_preview_surf_h, .z = SERVO_PREVIEW_Z,
            .alpha = 255, .visible = true, .use_pixel_alpha = true,
            .src_rect = { 0, 0, (uint32_t)pw, 1 },
            .dst_rect = { px, py, (uint32_t)pw, 1 },
        };
        dv_layer_desc_t bot = top;
        bot.dst_rect.y = py + ph - 1;

        dv_layer_desc_t left = {
            .source = g_servo_preview_surf_v, .z = SERVO_PREVIEW_Z,
            .alpha = 255, .visible = true, .use_pixel_alpha = true,
            .src_rect = { 0, 0, 1, (uint32_t)ph },
            .dst_rect = { px, py, 1, (uint32_t)ph },
        };
        dv_layer_desc_t right = left;
        right.dst_rect.x = px + pw - 1;

        dv_layer_update(g_servo_preview_top,   &top);
        dv_layer_update(g_servo_preview_bot,   &bot);
        dv_layer_update(g_servo_preview_left,  &left);
        dv_layer_update(g_servo_preview_right, &right);
        g_servo_preview_visible = true;
        return;
    }

    for (int i = 0; i < pw; i += 8)
    {
        int dash_w = (i + 4 <= pw) ? 4 : (pw - i);
        if (dash_w <= 0) break;
        fb_fill_rect(px + i, py,             dash_w, 1, COLOR_WHITE);
        fb_fill_rect(px + i, py + ph - 1,    dash_w, 1, COLOR_WHITE);
    }
    for (int i = 0; i < ph; i += 8)
    {
        int dash_h = (i + 4 <= ph) ? 4 : (ph - i);
        if (dash_h <= 0) break;
        fb_fill_rect(px,           py + i, 1, dash_h, COLOR_WHITE);
        fb_fill_rect(px + pw - 1,  py + i, 1, dash_h, COLOR_WHITE);
    }
}
