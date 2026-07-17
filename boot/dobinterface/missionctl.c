/* MainDOB DobInterface 2.0 — foglio: missionctl.c
 *
 * Mission Control ("Mostra finestre"): griglia di miniature di TUTTE
 * le finestre (visibili e nascoste), pre-renderizzate una tantum a
 * mc_enter rasterizzando in software l'ultimo cmdbuf di ciascuna in
 * una piccola bitmap, caricata come texture e liberata a mc_exit.
 *
 * NOVITA' 2.0 — MINIATURE FEDELI: il rasterizzatore delle miniature
 * ora rende anche i record BLIT_TEX leggendo i pixel veri dallo
 * SPECCHIO CPU del tex pool (win_tex_pool_cpu_find, mantenuto per lo
 * screenshot). Nel vecchio sistema quei record erano semplicemente
 * saltati e le anteprime avevano buchi al posto di icone e raster
 * dei client. Campionamento dst-driven (nearest): per ogni pixel di
 * destinazione della footprint mappata si campiona la sorgente —
 * O(area miniatura), non O(area texture). Convenzione alpha != 0 =
 * opaco, la stessa dello screenshot.
 *
 * Il testo si disegna a glifi veri 8x16 fatti restringere dallo
 * scale nearest — testo (piccolo) reale, non barre segnaposto. Anche
 * i DRAW_RECT rasterizzano i contorni (il baco legacy li saltava e
 * le miniature perdevano i bordi interni delle finestre). I
 * BLIT_INLINE restano fuori: a scala di miniatura un inline 32x32 e'
 * al piu' un pixel, e il percorso e' in pensione. */

#include "di_internal.h"

/* ================= stato condiviso (proprieta' di questo foglio) ====== */

bool mc_active = false;

/* ================= stato privato ====================================== */

static int mc_scroll       = 0;   /* offset di scroll in px       */
static int mc_total_height = 0;   /* altezza totale del contenuto */

static mc_thumb_t mc_thumbs[MAX_WINDOWS];
static int        mc_thumb_count = 0;

/* ================= verbi esecutivi: rasterizzatore miniature ========== */

typedef struct {
    uint32_t *dst;
    int       dst_w, dst_h;
    int       body_w, body_h;
    window_t *win;              /* per lo specchio CPU dei BLIT_TEX */
} mc_thumb_ctx_t;

static inline int mc_x0(const mc_thumb_ctx_t *c, int x) { return (x * c->dst_w) / c->body_w; }
static inline int mc_x1(const mc_thumb_ctx_t *c, int x) { return ((x * c->dst_w) + c->body_w - 1) / c->body_w; }
static inline int mc_y0(const mc_thumb_ctx_t *c, int y) { return (y * c->dst_h) / c->body_h; }
static inline int mc_y1(const mc_thumb_ctx_t *c, int y) { return ((y * c->dst_h) + c->body_h - 1) / c->body_h; }

static void mc_v_fill_rect(void *ctx, int x, int y, int rw, int rh, uint32_t c)
{
    mc_thumb_ctx_t *t = (mc_thumb_ctx_t *)ctx;
    int cx0 = x < 0 ? 0 : x;
    int cy0 = y < 0 ? 0 : y;
    int cx1 = x + rw; if (cx1 > t->body_w) cx1 = t->body_w;
    int cy1 = y + rh; if (cy1 > t->body_h) cy1 = t->body_h;
    if (cx0 >= cx1 || cy0 >= cy1) return;
    int dx0 = mc_x0(t, cx0), dx1 = mc_x1(t, cx1);
    int dy0 = mc_y0(t, cy0), dy1 = mc_y1(t, cy1);
    if (dx0 < 0) dx0 = 0;
    if (dx1 > t->dst_w) dx1 = t->dst_w;
    if (dy0 < 0) dy0 = 0;
    if (dy1 > t->dst_h) dy1 = t->dst_h;
    for (int yy = dy0; yy < dy1; yy++)
    {
        uint32_t *row = &t->dst[yy * t->dst_w];
        for (int xx = dx0; xx < dx1; xx++) row[xx] = c;
    }
}

/* Anche i contorni rasterizzano — chiude il baco legacy che saltava
 * DRAW_RECT lasciando le miniature senza bordi interni. */
static void mc_v_draw_rect(void *ctx, int x, int y, int rw, int rh, uint32_t c)
{
    mc_v_fill_rect(ctx, x,          y,          rw, 1,  c);
    mc_v_fill_rect(ctx, x,          y + rh - 1, rw, 1,  c);
    mc_v_fill_rect(ctx, x,          y,          1,  rh, c);
    mc_v_fill_rect(ctx, x + rw - 1, y,          1,  rh, c);
}

static void mc_v_draw_pixel(void *ctx, int x, int y, uint32_t c)
{
    mc_thumb_ctx_t *t = (mc_thumb_ctx_t *)ctx;
    if (x < 0 || y < 0 || x >= t->body_w || y >= t->body_h) return;
    int dx = mc_x0(t, x), dy = mc_y0(t, y);
    if (dx >= 0 && dx < t->dst_w && dy >= 0 && dy < t->dst_h)
        t->dst[dy * t->dst_w + dx] = c;
}

/* Testo nella miniatura: il vecchio dobinterface aveva testo vero
 * gratis scalando il pixel buffer della finestra; qui si disegnano i
 * glifi 8x16 veri e lo scale nearest li restringe — stesso risultato
 * netto, testo (piccolo) reale invece di un segnaposto. */
static void mc_v_draw_text(void *ctx, int x, int y, uint32_t fg, uint32_t bg,
                           const uint8_t *text, uint32_t len, int fixed)
{
    mc_thumb_ctx_t *t = (mc_thumb_ctx_t *)ctx;
    (void)fixed;   /* a pochi px la miniatura resta monospace */
    int rw = (int)len * DOB_FONT_W;
    int rh = DOB_FONT_H;

    /* Sfondo dietro l'intera run, scalato nella miniatura. */
    int cx0 = x < 0 ? 0 : x;
    int cy0 = y < 0 ? 0 : y;
    int cx1 = x + rw; if (cx1 > t->body_w) cx1 = t->body_w;
    int cy1 = y + rh; if (cy1 > t->body_h) cy1 = t->body_h;
    if (cx0 < cx1 && cy0 < cy1)
    {
        int dx0 = mc_x0(t, cx0), dx1 = mc_x1(t, cx1);
        int dy0 = mc_y0(t, cy0), dy1 = mc_y1(t, cy1);
        if (dx0 < 0) dx0 = 0;
        if (dx1 > t->dst_w) dx1 = t->dst_w;
        if (dy0 < 0) dy0 = 0;
        if (dy1 > t->dst_h) dy1 = t->dst_h;
        for (int yy = dy0; yy < dy1; yy++)
        {
            uint32_t *row = &t->dst[yy * t->dst_w];
            for (int xx = dx0; xx < dx1; xx++) row[xx] = bg;
        }
    }

    /* I glifi: ogni pixel acceso della cella 8x16 passa dallo scale
     * e viene piazzato in fg. */
    if (!text) return;
    uint32_t i = 0, ci = 0;
    while (i < len)
    {
        uint8_t gidx;
        i += dob_font_decode(text, len, i, &gidx);
        int gx = x + (int)ci * DOB_FONT_W;
        for (int row = 0; row < DOB_FONT_H; row++)
        {
            uint8_t bits = dob_font_row((char)gidx, row);
            if (!bits) continue;
            int py = y + row;
            if (py < 0 || py >= t->body_h) continue;
            int dy = mc_y0(t, py);
            if (dy < 0 || dy >= t->dst_h) continue;
            uint32_t *drow = &t->dst[dy * t->dst_w];
            for (int col = 0; col < DOB_FONT_W; col++)
            {
                if (!(bits & (0x80 >> col))) continue;
                int px = gx + col;
                if (px < 0 || px >= t->body_w) continue;
                int dx = mc_x0(t, px);
                if (dx >= 0 && dx < t->dst_w) drow[dx] = fg;
            }
        }
        ci++;
    }
}

/* NOVITA' 2.0: BLIT_TEX fedele dallo specchio CPU (vedi testa).
 * Campionamento dst-driven: per ogni pixel di destinazione della
 * footprint si mappa all'indietro nella sorgente (nearest). */
static void mc_v_blit_tex(void *ctx, int x, int y, uint32_t handle,
                          int sw, int sh)
{
    mc_thumb_ctx_t *t = (mc_thumb_ctx_t *)ctx;
    uint16_t tw = 0, th = 0;
    const uint32_t *cpu = win_tex_pool_cpu_find(t->win, handle, &tw, &th);
    if (!cpu) return;               /* specchio mancato: come il legacy */
    if (sw > (int)tw) sw = (int)tw;
    if (sh > (int)th) sh = (int)th;

    /* Serra la footprint al corpo, poi mappala nella miniatura. */
    int cx0 = x < 0 ? 0 : x;
    int cy0 = y < 0 ? 0 : y;
    int cx1 = x + sw; if (cx1 > t->body_w) cx1 = t->body_w;
    int cy1 = y + sh; if (cy1 > t->body_h) cy1 = t->body_h;
    if (cx0 >= cx1 || cy0 >= cy1) return;
    int dx0 = mc_x0(t, cx0), dx1 = mc_x1(t, cx1);
    int dy0 = mc_y0(t, cy0), dy1 = mc_y1(t, cy1);
    if (dx0 < 0) dx0 = 0;
    if (dx1 > t->dst_w) dx1 = t->dst_w;
    if (dy0 < 0) dy0 = 0;
    if (dy1 > t->dst_h) dy1 = t->dst_h;

    for (int yy = dy0; yy < dy1; yy++)
    {
        /* Inversa del mapping y: pixel del corpo al centro della
         * cella di destinazione, poi in coordinate texture. */
        int by = (yy * t->body_h) / t->dst_h;
        int sy = by - y;
        if (sy < 0 || sy >= sh) continue;
        uint32_t *drow = &t->dst[yy * t->dst_w];
        const uint32_t *srow = &cpu[(size_t)sy * tw];
        for (int xx = dx0; xx < dx1; xx++)
        {
            int bx = (xx * t->body_w) / t->dst_w;
            int sx = bx - x;
            if (sx < 0 || sx >= sw) continue;
            uint32_t p = srow[sx];
            if ((p >> 24) != 0)     /* convenzione: alpha!=0 = opaco */
                drow[xx] = p;
        }
    }
}

/* ================= terra di mezzo: raster e layout ==================== */

static bool mc_rasterize_window(window_t *w, uint32_t *dst,
                                int dst_w, int dst_h)
{
    if (!dst || dst_w <= 0 || dst_h <= 0) return false;
    if (!w->last_cmdbuf || w->last_cmdbuf_size <= DOBUI_CMDBUF_HDR_SIZE)
        return false;
    if (w->width <= 0 || w->height <= 0) return false;

    for (int i = 0; i < dst_w * dst_h; i++) dst[i] = COLOR_WHITE;

    mc_thumb_ctx_t tc = {
        .dst    = dst,
        .dst_w  = dst_w,
        .dst_h  = dst_h,
        .body_w = w->width,
        .body_h = w->height,
        .win    = w,
    };
    cmdbuf_visitor_t v = {
        .ctx         = &tc,
        .fill_rect   = mc_v_fill_rect,
        .draw_rect   = mc_v_draw_rect,
        .draw_pixel  = mc_v_draw_pixel,
        .draw_text   = mc_v_draw_text,
        .blit_inline = NULL,
        .blit_tex    = mc_v_blit_tex,   /* 2.0: fedele, dallo specchio */
    };
    cmdbuf_decode(w->last_cmdbuf, w->last_cmdbuf_size, &v);
    return true;
}

/* Layout: tutte le finestre (visibili + nascoste) come miniature in
 * griglia non sovrapposta nell'area desktop. Prima affiancate, a
 * capo quando serve. */
static void mc_compute_layout(void)
{
    mc_thumb_count = 0;

    /* Solo le finestre che hanno disegnato almeno una volta: le
     * non-ready sarebbero miniature vuote. */
    int indices[MAX_WINDOWS];
    int count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
    {
        if (windows[i].used && windows[i].ready)
            indices[count++] = i;
    }
    if (count == 0) return;

    /* Lato miniatura: circa 1/4 del desktop, aggiustato per entrare
     * davvero in griglia. */
    int thumb_size = desktop_w / 3;
    if (thumb_size > SCREEN_H / 3) thumb_size = SCREEN_H / 3;
    if (thumb_size > 250) thumb_size = 250;
    if (thumb_size < 100) thumb_size = 100;

    int label_h = FONT_H + 4;  /* spazio per il titolo sotto */
    int cell_w = thumb_size + MC_THUMB_GAP;
    int cell_h = thumb_size + WIN_HEADER_H + label_h + MC_THUMB_GAP;

    int cols = desktop_w / cell_w;
    if (cols < 1) cols = 1;

    /* Centra in orizzontale. */
    int total_row_w = cols * cell_w - MC_THUMB_GAP;
    int start_x = (desktop_w - total_row_w) / 2;
    if (start_x < MC_THUMB_GAP) start_x = MC_THUMB_GAP;

    int start_y = MC_THUMB_GAP;

    for (int i = 0; i < count; i++)
    {
        int col = i % cols;
        int row = i / cols;

        mc_thumbs[mc_thumb_count].win_idx = indices[i];
        mc_thumbs[mc_thumb_count].x = start_x + col * cell_w;
        mc_thumbs[mc_thumb_count].y = start_y + row * cell_h;
        mc_thumbs[mc_thumb_count].thumb_size = thumb_size;
        mc_thumb_count++;
    }

    int rows = (count + cols - 1) / cols;
    mc_total_height = start_y + rows * cell_h;
}

/* Disegna una miniatura (header mini, blit 1:1 della texture
 * pre-renderizzata, bordo, titolo sotto). */
static void mc_draw_thumb(mc_thumb_t *t, int scroll_y)
{
    window_t *w = &windows[t->win_idx];
    int tx = t->x;
    int ty = t->y - scroll_y;
    int ts = t->thumb_size;

    /* Salta se del tutto fuori schermo. */
    if (ty + ts + WIN_HEADER_H + FONT_H + 4 < 0 || ty > SCREEN_H)
        return;

    /* Mini barra header. */
    uint32_t hdr_color = COLOR_WIN_HEAD;
    int mini_header = 16;
    fb_fill_rect(tx, ty, ts, mini_header, hdr_color);

    /* Blit della miniatura pre-renderizzata. La texture e' a misura
     * dell'area corpo: blit 1:1, nessuno stretch. */
    int body_y = ty + mini_header;
    int th = ts - mini_header;
    if (t->tex != DV_HANDLE_NONE && th > 0
        && g_backbuf_surf != DV_HANDLE_NONE)
    {
        dv_rect_t  sr = { 0, 0, (uint32_t)t->tex_w, (uint32_t)t->tex_h };
        dv_point_t dp = { tx, body_y };
        dv_blit((dv_surface_t)t->tex, sr, g_backbuf_surf, dp);
    }

    /* Bordo: bianco se era visibile, grigio se era nascosta. */
    uint32_t border_color = w->visible ? COLOR_WHITE : COLOR_WIN_BORDER;
    fb_draw_rect(tx - 1, ty - 1, ts + 2, ts + 2, border_color, 1);

    /* Titolo sotto, troncato alla larghezza e centrato. */
    int label_y = ty + ts + 2;
    if (label_y >= 0 && label_y < SCREEN_H)
    {
        char title_buf[32];
        int max_chars = ts / FONT_W;
        if (max_chars > 31) max_chars = 31;
        strncpy(title_buf, w->title, (uint32_t)max_chars);
        title_buf[max_chars] = '\0';

        int tw = (int)font_string_width(title_buf);
        int title_x = tx + (ts - tw) / 2;
        if (title_x < tx) title_x = tx;
        font_draw_string(title_x, label_y, title_buf, COLOR_WHITE, COLOR_BLACK);
    }
}

/* ================= logica ad alto livello ============================= */

void mc_draw(void)
{
    /* Velo scuro sull'area desktop. */
    fb_fill_rect(0, 0, desktop_w, SCREEN_H, 0x00000022);

    /* Titolo. */
    font_draw_string(MC_THUMB_GAP, 4 - mc_scroll, "Mostra finestre", COLOR_CYAN, 0x00000022);

    for (int i = 0; i < mc_thumb_count; i++)
        mc_draw_thumb(&mc_thumbs[i], mc_scroll);
}

/* Indice in mc_thumbs[] o -1. */
int mc_hit_test(int mx, int my)
{
    for (int i = 0; i < mc_thumb_count; i++)
    {
        mc_thumb_t *t = &mc_thumbs[i];
        int ty = t->y - mc_scroll;
        if (mx >= t->x && mx < t->x + t->thumb_size &&
            my >= ty && my < ty + t->thumb_size + 16)
        {
            return i;
        }
    }
    return -1;
}

/* mc_enter — layout + pre-render di ogni miniatura: raster CPU del
 * last_cmdbuf in una piccola bitmap, upload come texture, handle
 * nella thumb. One-shot, non per frame. */
void mc_enter(void)
{
    mc_active = true;
    mc_scroll = 0;
    widget_panel_open = false;
    mc_compute_layout();

    for (int i = 0; i < mc_thumb_count; i++)
    {
        mc_thumb_t *t = &mc_thumbs[i];
        t->tex   = DV_HANDLE_NONE;
        t->tex_w = 0;
        t->tex_h = 0;
        window_t *w = &windows[t->win_idx];
        int tw = t->thumb_size;
        int th = t->thumb_size - 16;   /* combacia col body_y di mc_draw_thumb */
        if (tw <= 0 || th <= 0) continue;

        uint32_t *bmp = (uint32_t *)malloc((uint32_t)tw * (uint32_t)th * 4u);
        if (!bmp) continue;
        if (!mc_rasterize_window(w, bmp, tw, th))
        {
            free(bmp);
            continue;
        }

        dv_texture_desc_t td = {
            .width      = (uint32_t)tw,
            .height     = (uint32_t)th,
            .format     = DV_FMT_BGRA8888,
            .mip_levels = 1,
            /* SYSRAM: letta dalla CPU a ogni bake (vedi scratch). */
            .flags      = DV_TEX_FLAG_DYNAMIC | DV_SURF_FLAG_SYSRAM,
        };
        dv_texture_t tex = DV_HANDLE_NONE;
        if (dv_texture_create(g_vproc, &td, &tex) == DV_OK)
        {
            dv_rect_t r = { 0, 0, (uint32_t)tw, (uint32_t)th };
            dv_texture_update_region(tex, r, bmp, (uint32_t)tw * 4u);
            t->tex   = tex;
            t->tex_w = tw;
            t->tex_h = th;
        }
        free(bmp);
    }

    di_mark_dirty(DIRTY_FULL);
}

/* mc_exit — libera le texture; con focus_idx valido ripristina e
 * focalizza quella finestra (con lo stesso recovery del video
 * liberato di GUI_WIN_RAISE: body_layer == NONE marca "liberata"). */
void mc_exit(int focus_idx)
{
    mc_active = false;

    for (int i = 0; i < mc_thumb_count; i++)
    {
        if (mc_thumbs[i].tex != DV_HANDLE_NONE)
        {
            dv_texture_destroy(mc_thumbs[i].tex);
            mc_thumbs[i].tex = DV_HANDLE_NONE;
        }
    }

    if (focus_idx >= 0 && focus_idx < mc_thumb_count)
    {
        int wi = mc_thumbs[focus_idx].win_idx;
        if (windows[wi].body_layer == DV_HANDLE_NONE)
        {
            win_alloc_video(&windows[wi]);
            windows[wi].surface_dirty = true;
        }
        windows[wi].visible = true;
        z_sort_valid = false;
        win_focus(wi);
    }
    di_mark_dirty(DIRTY_FULL);
}

/* Scroll di Mission Control: a rotella o per prossimita' ai bordi.
 * Ritorna true se l'offset e' cambiato (=> repaint). Chiamato dal
 * loop eventi solo con mc_active. */
bool mc_handle_scroll(int scroll, int cur_y)
{
    if (mc_total_height <= SCREEN_H) return false;
    int old_mc_scroll = mc_scroll;
    int max_scroll = mc_total_height - SCREEN_H;
    if (scroll != 0) mc_scroll += scroll * MC_SCROLL_SPEED * 2;
    else if (cur_y < MC_EDGE_SCROLL) mc_scroll -= MC_SCROLL_SPEED;
    else if (cur_y > SCREEN_H - MC_EDGE_SCROLL) mc_scroll += MC_SCROLL_SPEED;
    if (mc_scroll < 0) mc_scroll = 0;
    if (mc_scroll > max_scroll) mc_scroll = max_scroll;
    return mc_scroll != old_mc_scroll;
}
