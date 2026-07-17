/* MainDOB DobInterface 2.0 — foglio: screenshot.c
 *
 * SCREENSHOT (Stamp R Sist) — ricomposizione SOFTWARE della scena.
 *
 * Il compositore possiede la scena (z-order, chrome, cmdbuf
 * trattenuti, pannelli SHM mappati, specchi CPU delle texture,
 * cursore): la cattura si fa QUI, ricomponendo in un buffer RAM —
 * driver-indipendente per costruzione, identica su bga, mach64,
 * x3100 e ferro vero. Non e' un readback dell'hardware: cattura il
 * modello del compositore, che coincide con lo schermo finche' tutto
 * passa dalla pipeline dv_* (in MainDOB: per costruzione).
 *
 * v1: sfondo + finestre (chrome + corpo completo: cmdbuf, pannello
 * SHM, texture da specchio CPU) + cursore. FUORI v1 (documentato):
 * icone desktop, wpanel/tray, toast — richiedono il twin dei
 * rispettivi percorsi di disegno (v1.1).
 *
 * File: /DATA/Screenshots/AAAA-MM-GG-hh.mm.mmm.bmp (24bpp bottom-up,
 * lo stesso dialetto BMP che DobPicture scrive e legge). Sincrono
 * nel pump (decine di ms una tantum: accettato — e' l'utente stesso
 * che ha premuto il tasto). */

#include "di_internal.h"

/* ================= verbi esecutivi: raster nello schermo RAM ========== */

typedef struct {
    uint32_t *fb;                       /* schermo intero, BGRA          */
    int       fb_w, fb_h;
    int       ox, oy;                   /* origine del corpo corrente    */
    int       clip_w, clip_h;           /* clip del corpo (w->width/h)   */
    const window_t *win;                /* per blit_tex/shmpanel         */
} shot_ctx_t;

static void shot_fill(shot_ctx_t *r, int x, int y, int w, int h, uint32_t c)
{
    if (w > r->clip_w - x) w = r->clip_w - x;
    if (h > r->clip_h - y) h = r->clip_h - y;
    int x0 = r->ox + (x < 0 ? 0 : x), y0 = r->oy + (y < 0 ? 0 : y);
    int x1 = r->ox + x + w,            y1 = r->oy + y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > r->fb_w) x1 = r->fb_w;
    if (y1 > r->fb_h) y1 = r->fb_h;
    for (int yy = y0; yy < y1; yy++)
    {
        uint32_t *row = &r->fb[yy * r->fb_w];
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

static void shot_v_fill_rect(void *ctx, int x, int y, int w, int h, uint32_t c)
{
    shot_fill((shot_ctx_t *)ctx, x, y, w, h, c);
}

static void shot_v_draw_rect(void *ctx, int x, int y, int w, int h, uint32_t c)
{
    if (w <= 0 || h <= 0) return;
    shot_fill(ctx, x,         y,         w, 1, c);
    shot_fill(ctx, x,         y + h - 1, w, 1, c);
    shot_fill(ctx, x,         y,         1, h, c);
    shot_fill(ctx, x + w - 1, y,         1, h, c);
}

static void shot_v_draw_pixel(void *ctx, int x, int y, uint32_t c)
{
    shot_fill(ctx, x, y, 1, 1, c);
}

static void shot_v_draw_text(void *ctx, int x, int y, uint32_t fg, uint32_t bg,
                             const uint8_t *text, uint32_t len, int fixed)
{
    shot_ctx_t *r = (shot_ctx_t *)ctx;
    int penx = x;
    uint32_t i = 0;
    while (i < len)
    {
        uint8_t gidx;
        i += dob_font_decode(text, len, i, &gidx);
        int adv = fixed ? DOB_FONT_W : dob_font_advance(gidx);
        int lft = fixed ? 0 : dob_font_left(gidx);
        shot_fill(r, penx, y, adv, DOB_FONT_H, bg);
        for (int row = 0; row < DOB_FONT_H; row++)
        {
            uint8_t bits = dob_font_data[gidx][row];
            for (int col = 0; col < DOB_FONT_W; col++)
            {
                if ((bits >> (DOB_FONT_W - 1 - col)) & 1)
                    shot_fill(r, penx + col - lft, y + row, 1, 1, fg);
            }
        }
        penx += adv;
    }
}

static void shot_v_blit_inline(void *ctx, int x, int y, int sw, int sh,
                               const uint32_t *src)
{
    shot_ctx_t *r = (shot_ctx_t *)ctx;
    for (int yy = 0; yy < sh; yy++)
        for (int xx = 0; xx < sw; xx++)
        {
            uint32_t p = src[yy * sw + xx];
            if (p != 0xFF000000u)       /* trasparenza legacy            */
                shot_fill(r, x + xx, y + yy, 1, 1, p);
        }
}

static void shot_v_blit_tex(void *ctx, int x, int y, uint32_t handle,
                            int w, int h)
{
    shot_ctx_t *r = (shot_ctx_t *)ctx;
    window_t *win = (window_t *)r->win;     /* accesso in sola lettura */
    int pi = tex_pool_find(win, (dv_texture_t)handle);
    if (pi < 0) return;                 /* handle non nel pool           */
    const uint32_t *cpu = win->tex_pool[pi].cpu;
    int tw = win->tex_pool[pi].w, th = win->tex_pool[pi].h;
    if (cpu == NULL)
    {
        shot_fill(r, x, y, w, h, 0x00808080u);  /* specchio mancato      */
        return;
    }
    if (w > tw) w = tw;
    if (h > th) h = th;
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
        {
            uint32_t p = cpu[(size_t)yy * tw + xx];
            if ((p >> 24) != 0)         /* convenzione: alpha!=0 = opaco */
                shot_fill(r, x + xx, y + yy, 1, 1, p);
        }
}

static void shot_v_blit_shmpanel(void *ctx, int x, int y, int w, int h,
                                 unsigned band_y0, unsigned band_rows)
{
    (void)band_y0; (void)band_rows;     /* istantanea: sempre integrale  */
    shot_ctx_t *r = (shot_ctx_t *)ctx;
    const window_t *win = r->win;
    if (win->panel_ptr == NULL) return;
    int pw = win->panel_w, ph = win->panel_h;
    if (w > pw) w = pw;
    if (h > ph) h = ph;
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            shot_fill(r, x + xx, y + yy, 1, 1,
                      win->panel_ptr[yy * pw + xx]);
}

/* ================= terra di mezzo: chrome e BMP ======================= */

/* Twin raster del chrome di win_bake: bordo 1px, header, titolo,
 * sfondo bianco del corpo. Stesse geometrie e colori. */
static void shot_window_chrome(shot_ctx_t *r, window_t *w)
{
    int sw = w->width + 2;
    int sh = w->height + 2 + WIN_HEADER_H;
    uint32_t border = w->focused ? COLOR_CYAN : COLOR_BLACK;
    uint32_t hdr    = w->focused ? COLOR_WIN_HEAD_ACTIVE : COLOR_WIN_HEAD;

    r->clip_w = sw; r->clip_h = sh;     /* chrome: clip a tutta finestra */
    shot_fill(r, 0, 0, sw, 1, border);
    shot_fill(r, 1, 1, sw - 2, WIN_HEADER_H, hdr);
    shot_fill(r, 0, 1, 1, sh - 2, border);
    shot_fill(r, sw - 1, 1, 1, sh - 2, border);
    shot_fill(r, 0, sh - 1, sw, 1, border);
    shot_fill(r, 1, 1 + WIN_HEADER_H, sw - 2, sh - 2 - WIN_HEADER_H,
              COLOR_WHITE);            /* corpo bianco PRIMA del replay */

    if (w->title[0])
    {
        shot_v_draw_text(r, 1 + 6, 1 + (WIN_HEADER_H - FONT_H) / 2,
                         COLOR_WHITE, hdr,
                         (const uint8_t *)w->title,
                         (uint32_t)strlen(w->title), 0);
    }
}

/* Scrittura BMP 24bpp bottom-up (il dialetto di DobPicture). */
static bool shot_write_bmp(const char *path, const uint32_t *fb,
                           int w, int h)
{
    int fd = dobfs_Open(path, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (fd < 0) return false;

    uint32_t row_bytes = ((uint32_t)w * 3u + 3u) & ~3u;
    uint32_t img_bytes = row_bytes * (uint32_t)h;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    uint32_t fsz = 54u + img_bytes;
    memcpy(hdr + 2,  &fsz, 4);
    uint32_t off = 54;            memcpy(hdr + 10, &off, 4);
    uint32_t bisz = 40;           memcpy(hdr + 14, &bisz, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    uint16_t planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &img_bytes, 4);
    bool ok = dobfs_Write(fd, hdr, sizeof(hdr)) == (int)sizeof(hdr);

    /* Una riga per volta, dal basso: BGRA -> BGR + padding a 4. */
    uint8_t *line = (uint8_t *)malloc(row_bytes);
    if (line == NULL) ok = false;
    for (int y = h - 1; ok && y >= 0; y--)
    {
        const uint32_t *src = &fb[y * w];
        for (int x = 0; x < w; x++)
        {
            line[x * 3 + 0] = (uint8_t)(src[x]);
            line[x * 3 + 1] = (uint8_t)(src[x] >> 8);
            line[x * 3 + 2] = (uint8_t)(src[x] >> 16);
        }
        for (uint32_t p = (uint32_t)w * 3u; p < row_bytes; p++) line[p] = 0;
        ok = dobfs_Write(fd, line, row_bytes) == (int)row_bytes;
    }
    free(line);
    dobfs_Close(fd);
    return ok;
}

/* ================= logica ad alto livello ============================= */

/* ORCHESTRATORE: ricomponi e salva. */
void screenshot_take(void)
{
    int sw = SCREEN_W, sh = SCREEN_H;
    uint32_t *fb = (uint32_t *)malloc((size_t)sw * sh * 4u);
    if (fb == NULL)
    {
        debug_print("[dobinterface] screenshot: malloc fallita\n");
        return;
    }

    /* Sfondo (v1: tinta piatta del desktop; icone/wpanel in v1.1). */
    for (int i = 0; i < sw * sh; i++) fb[i] = 0x00000022u;

    /* Finestre dal fondo alla cima, come la compose vera. */
    sort_windows_by_z();
    shot_ctx_t r = { .fb = fb, .fb_w = sw, .fb_h = sh };
    cmdbuf_visitor_t v = {
        .ctx          = &r,
        .fill_rect    = shot_v_fill_rect,
        .draw_rect    = shot_v_draw_rect,
        .draw_pixel   = shot_v_draw_pixel,
        .draw_text    = shot_v_draw_text,
        .blit_inline  = shot_v_blit_inline,
        .blit_tex     = shot_v_blit_tex,
        .blit_shmpanel= shot_v_blit_shmpanel,
    };
    for (int i = 0; i < sorted_count; i++)
    {
        window_t *w = &windows[sorted_wins[i]];
        if (!w->used || !w->visible || !w->ready) continue;

        r.win = w;
        r.ox = w->x; r.oy = w->y;
        shot_window_chrome(&r, w);

        /* Corpo: pannello SHM (se c'e') e replay del cmdbuf, con
         * origine e clip del CORPO — stessi offset di win_replay. */
        r.ox = w->x + 1; r.oy = w->y + 1 + WIN_HEADER_H;
        r.clip_w = w->width; r.clip_h = w->height;
        if (w->panel_ptr != NULL)
        {
            shot_v_blit_shmpanel(&r, w->panel_last_x, w->panel_last_y,
                                 w->panel_w, w->panel_h, 0, 0);
        }
        if (w->last_cmdbuf != NULL && w->last_cmdbuf_size > 0)
        {
            cmdbuf_decode(w->last_cmdbuf, w->last_cmdbuf_size, &v);
        }
    }

    /* Cursore (pixel-alpha, come il layer z=999): pixel dello sprite
     * via verbo del foglio cursor (0 = trasparente, 1 = nero,
     * 2 = bianco). */
    {
        r.ox = 0; r.oy = 0; r.clip_w = sw; r.clip_h = sh;
        for (int row = 0; row < CURSOR_H; row++)
            for (int col = 0; col < CURSOR_W; col++)
            {
                uint8_t px = cursor_sprite_pixel(current_cursor, row, col);
                if (px == 0) continue;
                shot_fill(&r, cursor_x + col, cursor_y + row, 1, 1,
                          px == 1 ? 0x00000000u : 0x00FFFFFFu);
            }
    }

    /* Nome: AAAA-MM-GG-hh.mm.mmm.bmp (spec: ore.minuti.millisecondi). */
    uint32_t t[7] = { 0 };
    gettime(t);
    char path[96];
    snprintf(path, sizeof(path),
             "/DATA/Screenshots/%04u-%02u-%02u-%02u.%02u.%03u.bmp",
             (unsigned)t[0], (unsigned)t[1], (unsigned)t[2],
             (unsigned)t[3], (unsigned)t[4], (unsigned)t[6]);

    bool ok = shot_write_bmp(path, fb, sw, sh);
    free(fb);
    debug_print(ok ? "[dobinterface] screenshot salvato.\n"
                   : "[dobinterface] screenshot: scrittura fallita.\n");
    if (ok) toast_show(path);           /* riscontro visivo all'utente   */
}
