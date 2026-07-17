/* DobPicture v2 — riscrittura moderna del paint di MainDOB
 *
 * Il v1 era uno dei primi programmi di MainDOB: pre-eventi, pre-SHM,
 * render del viewport a run-length con chiamate per-pixel al server —
 * da cui il lag tra tratto e schermo, e il costo CPU/GPU/RAM. Questa
 * versione usa lo stack moderno:
 *
 *   - PANNELLO SHM (dobui_ShmPanelEnsure, come DobWrite): la vista E'
 *     il buffer condiviso col compositore; la consegna di un tratto e'
 *     un record di banda da 9 byte, non un upload. Ripiego automatico
 *     a buffer privato + BlitBufferDynamic se il pannello manca.
 *   - ORIENTATO A EVENTI (app.h): il tratto si disegna nel mousemove
 *     con interpolazione di Bresenham tra i campioni — zero polling,
 *     zero ritardo percepito; si ridipinge SOLO la banda sporca.
 *   - UNA SOLA TOOLBAR in alto (la barra sinistra e' riassorbita):
 *     strumenti con icone AD ALTO CONTRASTO, spessore, ZOOM 1/2/4/8x,
 *     palette inline, colore corrente, dimensioni tela.
 *   - Strumenti: matita, gomma, RIEMPIMENTO (flood fill, mancava),
 *     contagocce.
 *   - RIDIMENSIONAMENTO TELA dall'angolo in basso a destra: il cursore
 *     diventa quello di resize (override CURSOR_RESIZE), si trascina,
 *     si applica al rilascio (contenuto preservato, aree nuove
 *     bianche).
 *   - Codec BMP standard (canali corretti dal b125), stesso dialetto
 *     24bpp bottom-up.
 *
 * Struttura (convenzione MainDOB): blocchi ESECUTIVI in alto (tela,
 * codec, pittura della vista, toolbar), LOGICA in basso (gli event
 * handler orchestrano). */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <DobInterface.h>
#include <DobPopup.h>
#include <DobFiles.h>
#include <DobFileSystem.h>
#include <app.h>
#include <dob/mem.h>
#include "imgcodec_api.h"

/* ===================================================================
 * Tavolozza UI e geometria
 * =================================================================== */

#define COL_BG        0x00102040
#define COL_BAR       0x00081830
#define COL_FG        0x00FFD860
#define COL_DIM       0x008090B0
#define COL_SEL       0x0000FFFF
#define COL_VOID      0x00303030    /* fuori-tela                       */
#define COL_CANVAS_BG 0x00FFFFFF

#define TOP_H         64            /* toolbar unica, due file          */
#define CELL          24            /* bottone strumento                */
#define PAL_CELL      16
#define GRIP          10            /* maniglia resize tela (px vista)  */

#define CANVAS_MAX_W  2048
#define CANVAS_MAX_H  2048

enum { TOOL_PEN = 0, TOOL_ERASER, TOOL_FILL, TOOL_PICK, TOOL_PAN,
       TOOL_COUNT };

static const uint32_t PALETTE[24] = {
    0x00000000, 0x00FFFFFF, 0x00808080, 0x00C0C0C0,
    0x000000FF, 0x00000080, 0x0000FF00, 0x00008000,
    0x00FF0000, 0x00800000, 0x0000FFFF, 0x000088FF,
    0x00FF00FF, 0x00800080, 0x00FFFF00, 0x00004080,
    0x008888FF, 0x0000FF88, 0x00404040, 0x00606060,
    0x00888888, 0x00A0A0A0, 0x00D0D0D0, 0x00F0F0F0,
};

/* ===================================================================
 * Stato
 * =================================================================== */

static int       s_w, s_h;          /* finestra                         */
static uint32_t *s_canvas;          /* tela, 0x00RRGGBB                 */
static int       s_cw = 320, s_ch = 240;
static char      s_path[128];       /* file corrente ("" = senza nome)  */
static bool      s_modified;

static int       s_tool  = TOOL_PEN;
static int       s_brush = 2;       /* 1..16                            */
static int       s_zoom  = 1;       /* 1,2,4,8                          */
static int       s_scx, s_scy;      /* scroll tela (px tela)            */
static uint32_t  s_color = 0x00000000;

/* Vista: pannello SHM (o ripiego privato). */
static uint32_t *s_view;
static int       s_vw, s_vh;
static bool      s_view_is_panel;
static int       s_view_priv_n;

/* Tratto in corso. */
static bool      s_drawing;
static int       s_last_cx, s_last_cy;

/* Codec PNG/JPEG: .mem caricato a runtime (imgcodec_api.h) — la cosa
 * corretta: i codec sono di SISTEMA, non di questo programma; come
 * .mem li riusa chiunque (thumbnail di DobFiles, viewer futuri) senza
 * duplicare il codice in ogni binario. NULL = caricamento fallito:
 * DobPicture degrada a solo-BMP, mai un crash. */
static imgcodec_api_t *s_codec;

static void *codec_alloc(uint32_t n) { return malloc(n); }
static void  codec_free(void *p)     { free(p); }

static void codec_load(void)
{
    dobfs_stat_t st;
    const char *path = "/SYSTEM/PROGRAMS/DobPicture/imgcodec.mem";
    if (dobfs_Stat(path, &st) != 0 || st.size == 0)
    {
        return;
    }
    uint8_t *blob = (uint8_t *)malloc(st.size);
    if (blob == NULL)
    {
        return;
    }
    int fd = dobfs_Open(path, FS_READ);
    uint32_t got = 0;
    while (fd >= 0 && got < st.size)
    {
        int n = dobfs_Read(fd, blob + got, st.size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    if (fd >= 0) dobfs_Close(fd);
    if (got == st.size)
    {
        s_codec = (imgcodec_api_t *)dob_mem_load(blob, st.size);
        if (s_codec != NULL &&
            s_codec->version == IMGCODEC_API_VERSION)
        {
            s_codec->set_allocator(codec_alloc, codec_free);
        }
        else
        {
            s_codec = NULL;         /* ABI diversa: meglio niente        */
        }
    }
    free(blob);                     /* il kernel ha gia' copiato         */
}

/* Legge un intero file (per probe+decode). */
static uint8_t *read_whole(const char *path, uint32_t *out_n)
{
    dobfs_stat_t st;
    if (dobfs_Stat(path, &st) != 0 || st.size == 0) return NULL;
    uint8_t *b = (uint8_t *)malloc(st.size);
    if (b == NULL) return NULL;
    int fd = dobfs_Open(path, FS_READ);
    uint32_t got = 0;
    while (fd >= 0 && got < st.size)
    {
        int n = dobfs_Read(fd, b + got, st.size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    if (fd >= 0) dobfs_Close(fd);
    if (got != st.size) { free(b); return NULL; }
    *out_n = st.size;
    return b;
}

/* Pan con lo strumento mano. */
static bool s_panning;
static int  s_pan_lx, s_pan_ly;     /* ultimo campione mouse (vista)    */

/* Modificatori (convenzione MainDOB: Ctrl+scroll = orizzontale). */
static uint8_t s_mods;

/* Resize della tela dall'angolo. */
static bool      s_resizing;
static int       s_target_cw, s_target_ch;
static bool      s_cursor_grip;     /* override attivo?                 */

/* ===================================================================
 * ESECUTIVI — tela
 * =================================================================== */

static bool canvas_alloc(int w, int h, bool keep)
{
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (w > CANVAS_MAX_W) w = CANVAS_MAX_W;
    if (h > CANVAS_MAX_H) h = CANVAS_MAX_H;

    uint32_t *nc = (uint32_t *)malloc((size_t)w * h * 4u);
    if (nc == NULL)
    {
        return false;
    }
    for (int i = 0; i < w * h; i++)
    {
        nc[i] = COL_CANVAS_BG;
    }
    if (keep && s_canvas != NULL)
    {
        int cw = s_cw < w ? s_cw : w;
        int ch = s_ch < h ? s_ch : h;
        for (int y = 0; y < ch; y++)
        {
            memcpy(&nc[y * w], &s_canvas[y * s_cw], (size_t)cw * 4u);
        }
    }
    free(s_canvas);
    s_canvas = nc;
    s_cw = w;
    s_ch = h;
    return true;
}

static inline uint32_t canvas_get(int x, int y)
{
    if (x < 0 || x >= s_cw || y < 0 || y >= s_ch)
    {
        return COL_VOID;
    }
    return s_canvas[y * s_cw + x];
}

/* Timbro quadrato del pennello, ritorna il rettangolo tela sporcato. */
static void stamp(int cx, int cy, uint32_t col,
                  int *dx0, int *dy0, int *dx1, int *dy1)
{
    int r = s_brush / 2;
    int x0 = cx - r, y0 = cy - r;
    int x1 = x0 + s_brush, y1 = y0 + s_brush;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s_cw) x1 = s_cw;
    if (y1 > s_ch) y1 = s_ch;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            s_canvas[y * s_cw + x] = col;
    if (x0 < *dx0) *dx0 = x0;
    if (y0 < *dy0) *dy0 = y0;
    if (x1 > *dx1) *dx1 = x1;
    if (y1 > *dy1) *dy1 = y1;
}

/* Segmento di tratto con Bresenham fra due campioni del mouse: e' cio'
 * che elimina i "buchi" (e il lag percepito) del v1, che timbrava solo
 * sul campione corrente. */
static void stroke_segment(int x0, int y0, int x1, int y1, uint32_t col,
                           int *dx0, int *dy0, int *dx1, int *dy1)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;)
    {
        stamp(x0, y0, col, dx0, dy0, dx1, dy1);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Flood fill (lo strumento che mancava): BFS con stack esplicito su
 * heap — mai ricorsione su una tela 2048x2048. */
static void flood_fill(int sx, int sy, uint32_t col)
{
    if (sx < 0 || sx >= s_cw || sy < 0 || sy >= s_ch)
    {
        return;
    }
    uint32_t old = s_canvas[sy * s_cw + sx];
    if (old == col)
    {
        return;
    }
    int cap = s_cw * s_ch;
    int *stack = (int *)malloc((size_t)cap * 4u);
    if (stack == NULL)
    {
        return;
    }
    int top = 0;
    stack[top++] = sy * s_cw + sx;
    s_canvas[sy * s_cw + sx] = col;
    while (top > 0)
    {
        int p = stack[--top];
        int x = p % s_cw, y = p / s_cw;
        static const int off[4][2] = { {1,0},{-1,0},{0,1},{0,-1} };
        for (int k = 0; k < 4; k++)
        {
            int nx = x + off[k][0], ny = y + off[k][1];
            if (nx < 0 || nx >= s_cw || ny < 0 || ny >= s_ch) continue;
            int np = ny * s_cw + nx;
            if (s_canvas[np] != old) continue;
            s_canvas[np] = col;
            if (top < cap) stack[top++] = np;
        }
    }
    free(stack);
}

/* ===================================================================
 * ESECUTIVI — codec BMP (standard, canali corretti: b125)
 * =================================================================== */

static bool bmp_save(const char *path)
{
    int fd = dobfs_Open(path, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (fd < 0) return false;
    uint32_t row = ((uint32_t)s_cw * 3u + 3u) & ~3u;
    uint32_t img = row * (uint32_t)s_ch;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0]='B'; hdr[1]='M';
    uint32_t fsz = 54u + img;      memcpy(hdr+2,  &fsz, 4);
    uint32_t off = 54, bisz = 40;  memcpy(hdr+10, &off, 4);
    memcpy(hdr+14, &bisz, 4);
    memcpy(hdr+18, &s_cw, 4);      memcpy(hdr+22, &s_ch, 4);
    uint16_t pl = 1, bpp = 24;     memcpy(hdr+26, &pl, 2);
    memcpy(hdr+28, &bpp, 2);       memcpy(hdr+34, &img, 4);
    bool ok = dobfs_Write(fd, hdr, 54) == 54;
    uint8_t *line = (uint8_t *)malloc(row);
    if (line == NULL) ok = false;
    for (int y = s_ch - 1; ok && y >= 0; y--)
    {
        const uint32_t *src = &s_canvas[y * s_cw];
        for (int x = 0; x < s_cw; x++)
        {
            line[x*3+0] = (uint8_t)(src[x]);        /* B */
            line[x*3+1] = (uint8_t)(src[x] >> 8);   /* G */
            line[x*3+2] = (uint8_t)(src[x] >> 16);  /* R */
        }
        for (uint32_t p = (uint32_t)s_cw*3u; p < row; p++) line[p] = 0;
        ok = dobfs_Write(fd, line, row) == (int)row;
    }
    free(line);
    dobfs_Close(fd);
    return ok;
}

static bool bmp_load(const char *path)
{
    int fd = dobfs_Open(path, FS_READ);
    if (fd < 0) return false;
    uint8_t hdr[54];
    if (dobfs_Read(fd, hdr, 54) != 54 || hdr[0] != 'B' || hdr[1] != 'M')
    {
        dobfs_Close(fd);
        return false;
    }
    int bw, bh;
    memcpy(&bw, hdr+18, 4);
    memcpy(&bh, hdr+22, 4);
    uint16_t bpp; memcpy(&bpp, hdr+28, 2);
    bool bottom_up = bh > 0;
    if (bh < 0) bh = -bh;
    if (bw <= 0 || bh <= 0 || bw > CANVAS_MAX_W || bh > CANVAS_MAX_H ||
        (bpp != 24 && bpp != 32))
    {
        dobfs_Close(fd);
        return false;
    }
    uint32_t doff; memcpy(&doff, hdr+10, 4);
    /* salta l'eventuale resto d'header */
    for (uint32_t skip = doff; skip > 54; )
    {
        uint8_t junk[64];
        uint32_t n = skip - 54 > 64 ? 64 : skip - 54;
        if (dobfs_Read(fd, junk, n) != (int)n) break;
        skip -= n;
    }
    if (!canvas_alloc(bw, bh, false))
    {
        dobfs_Close(fd);
        return false;
    }
    int bypp = bpp / 8;
    uint32_t row = ((uint32_t)bw * bypp + 3u) & ~3u;
    uint8_t *line = (uint8_t *)malloc(row);
    if (line == NULL)
    {
        dobfs_Close(fd);
        return false;
    }
    for (int r = 0; r < bh; r++)
    {
        if (dobfs_Read(fd, line, row) != (int)row) break;
        int y = bottom_up ? bh - 1 - r : r;
        for (int x = 0; x < bw; x++)
        {
            const uint8_t *p = &line[x * bypp];
            s_canvas[y * bw + x] = ((uint32_t)p[2] << 16)
                                 | ((uint32_t)p[1] << 8) | p[0];
        }
    }
    free(line);
    dobfs_Close(fd);
    return true;
}

/* ===================================================================
 * ESECUTIVI — pittura della vista (SHM, a bande sporche)
 * =================================================================== */

static void view_ensure(void)
{
    uint32_t win = dobui_window();
    int vw = s_w, vh = s_h - TOP_H;
    if (vw < 1) vw = 1;
    if (vh < 1) vh = 1;
    uint32_t *panel = dobui_ShmPanelEnsure(win, vw, vh);
    if (panel != NULL)
    {
        if (!s_view_is_panel) { free(s_view); s_view_priv_n = 0; }
        s_view = panel;
        s_view_is_panel = true;
    }
    else
    {
        if (s_view_is_panel) { s_view = NULL; s_view_priv_n = 0; }
        s_view_is_panel = false;
        int need = vw * vh;
        if (need > s_view_priv_n)
        {
            free(s_view);
            s_view = (uint32_t *)malloc((size_t)need * 4u);
            s_view_priv_n = s_view ? need : 0;
        }
    }
    s_vw = vw;
    s_vh = vh;
}

/* Ridipinge il rettangolo di VISTA [vx0,vy0)-(vx1,vy1) dal canvas
 * (nearest allo zoom corrente; fuori-tela = grigio; maniglia grip). */
static void view_paint(int vx0, int vy0, int vx1, int vy1)
{
    if (s_view == NULL) return;
    if (vx0 < 0) vx0 = 0;
    if (vy0 < 0) vy0 = 0;
    if (vx1 > s_vw) vx1 = s_vw;
    if (vy1 > s_vh) vy1 = s_vh;
    for (int vy = vy0; vy < vy1; vy++)
    {
        uint32_t *row = &s_view[vy * s_vw];
        int cy = s_scy + vy / s_zoom;
        for (int vx = vx0; vx < vx1; vx++)
        {
            row[vx] = canvas_get(s_scx + vx / s_zoom, cy);
        }
    }

    /* Maniglia di resize: quadratino sull'angolo basso-destra della
     * TELA (in coordinate vista), se visibile. */
    int gx = (s_cw - s_scx) * s_zoom - GRIP;
    int gy = (s_ch - s_scy) * s_zoom - GRIP;
    for (int y = 0; y < GRIP; y++)
        for (int x = 0; x < GRIP; x++)
        {
            int vx = gx + x, vy = gy + y;
            if (vx >= vx0 && vx < vx1 && vy >= vy0 && vy < vy1)
                s_view[vy * s_vw + vx] =
                    ((x + y) & 2) ? COL_FG : COL_BAR;
        }
}

static void toolbar_records(void);      /* definita sotto: solo record  */

/* PRESENT: l'unico punto che chiama Invalidate. CONTRATTO del
 * framework (imparato a caro prezzo alla prima prova): il cmdbuf e'
 * immediate-mode per frame — ogni Invalidate consegna un buffer che
 * SOSTITUISCE il precedente. Quindi ogni frame ri-registra TUTTO il
 * contenuto non-pannello (la toolbar: ~40 record, spiccioli) e
 * dichiara la banda del pannello sporcata in QUELLO STESSO frame.
 * La prima versione faceva Invalidate separati: il frame col solo
 * blit cancellava la toolbar — finestra inusabile. */
static void present(int band_y0, int band_rows)
{
    uint32_t win = dobui_window();
    toolbar_records();
    if (s_view_is_panel)
    {
        if (band_rows <= 0) { band_y0 = 0; band_rows = 1; }
        dobui_ShmPanelBlit(win, 0, TOP_H, s_vw, s_vh,
                           band_y0, band_rows);
    }
    else if (s_view != NULL)
    {
        dobui_BlitBufferDynamic(win, 0, TOP_H, s_view, s_vw, s_vh);
    }
    dobui_Invalidate(win);
}

/* Consegna della banda di vista sporcata (ridipinta da view_paint). */
static void view_flush(int vx0, int vy0, int vx1, int vy1)
{
    (void)vx0; (void)vx1;
    if (vy0 < 0) vy0 = 0;
    if (vy1 > s_vh) vy1 = s_vh;
    if (vy0 >= vy1) return;
    present(vy0, vy1 - vy0);
}

/* Rettangolo TELA sporcato -> ridipingi e consegna la banda di vista. */
static void dirty_canvas_rect(int cx0, int cy0, int cx1, int cy1)
{
    int vx0 = (cx0 - s_scx) * s_zoom;
    int vy0 = (cy0 - s_scy) * s_zoom;
    int vx1 = (cx1 - s_scx) * s_zoom;
    int vy1 = (cy1 - s_scy) * s_zoom;
    view_paint(vx0, vy0, vx1, vy1);
    view_flush(vx0, vy0, vx1, vy1);
}

static void view_full_repaint(void)
{
    view_paint(0, 0, s_vw, s_vh);
    view_flush(0, 0, s_vw, s_vh);
}

/* ===================================================================
 * ESECUTIVI — toolbar (icone ad alto contrasto)
 * =================================================================== */

static int tool_btn_x(int i)   { return 8 + i * (CELL + 4); }
static int zoom_x(void)        { return tool_btn_x(TOOL_COUNT) + 12; }
static int brush_x(void)       { return zoom_x() + 120; }
static int dims_x(void)        { return brush_x() + 150; }
static int pal_x(void)         { return 8; }
#define BTN_W   16              /* bottoni [-] [+]                     */
#define SLD_X   470             /* slider RGB (fila 2)                 */
#define SLD_W   120
#define SLD_H   8

/* Bottone [-]/[+]: disegno + hit test condivisi. */
static void draw_pm(uint32_t win, int x, int y, char ch)
{
    dobui_FillRect(win, x, y, BTN_W, BTN_W, 0x00204070);
    dobui_DrawRect(win, x, y, BTN_W, BTN_W, COL_DIM);
    char t[2] = { ch, 0 };
    dobui_DrawTextFixed(win, x + 4, y + 1, t, COL_FG, 0x00204070);
}

static bool hit(int mx, int my, int x, int y, int w, int h)
{
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

/* Icone 16x16 disegnate a fill: giallo pieno su fondo scuro — il v1
 * le aveva quasi invisibili. */
static void draw_tool_icon(uint32_t win, int x, int y, int tool, bool sel)
{
    uint32_t bg = sel ? 0x00204070 : COL_BAR;
    uint32_t fg = COL_FG;
    dobui_FillRect(win, x, y, CELL, CELL, bg);
    dobui_DrawRect(win, x, y, CELL, CELL, sel ? COL_SEL : COL_DIM);
    int cx = x + 4, cy = y + 4;
    switch (tool)
    {
    case TOOL_PEN:                                  /* matita: diagonale */
        for (int i = 0; i < 12; i++)
            dobui_FillRect(win, cx + i, cy + 11 - i, 2, 2, fg);
        break;
    case TOOL_ERASER:                               /* gomma: blocco     */
        dobui_FillRect(win, cx + 1, cy + 4, 12, 8, fg);
        dobui_FillRect(win, cx + 3, cy + 6, 8, 4, bg);
        break;
    case TOOL_FILL:                                 /* secchiello        */
        dobui_FillRect(win, cx + 2, cy + 2, 9, 9, fg);
        dobui_FillRect(win, cx + 4, cy + 4, 5, 5, bg);
        dobui_FillRect(win, cx + 10, cy + 10, 4, 4, fg);   /* goccia   */
        break;
    case TOOL_PICK:                                 /* contagocce        */
        dobui_FillRect(win, cx + 10, cy + 1, 4, 4, fg);
        for (int i = 0; i < 9; i++)
            dobui_FillRect(win, cx + 9 - i, cy + 3 + i, 2, 2, fg);
        break;
    case TOOL_PAN:                                  /* mano: croce       */
        dobui_FillRect(win, cx + 6, cy + 1, 3, 13, fg);
        dobui_FillRect(win, cx + 1, cy + 6, 13, 3, fg);
        dobui_FillRect(win, cx + 5, cy,     5, 3, fg);
        break;
    }
}

static void toolbar_records(void)
{
    uint32_t win = dobui_window();
    char t[64];

    dobui_FillRect(win, 0, 0, s_w, TOP_H, COL_BAR);

    /* Fila 1: strumenti, zoom, spessore, dimensioni tela. */
    for (int i = 0; i < TOOL_COUNT; i++)
    {
        draw_tool_icon(win, tool_btn_x(i), 4, i, i == s_tool);
    }
    draw_pm(win, zoom_x(), 8, '-');
    snprintf(t, sizeof(t), "%dx", s_zoom);
    dobui_DrawTextFixed(win, zoom_x() + BTN_W + 6, 10, t, COL_FG, COL_BAR);
    draw_pm(win, zoom_x() + BTN_W + 34, 8, '+');
    dobui_DrawTextFixed(win, zoom_x() + 2 * BTN_W + 56, 10, "zoom",
                        COL_DIM, COL_BAR);

    draw_pm(win, brush_x(), 8, '-');
    snprintf(t, sizeof(t), "%dpx", s_brush);
    dobui_DrawTextFixed(win, brush_x() + BTN_W + 6, 10, t, COL_FG, COL_BAR);
    draw_pm(win, brush_x() + BTN_W + 40, 8, '+');
    dobui_DrawTextFixed(win, brush_x() + 2 * BTN_W + 62, 10, "penna",
                        COL_DIM, COL_BAR);
    if (s_resizing)
    {
        snprintf(t, sizeof(t), "tela -> %dx%d (rilascia per applicare)",
                 s_target_cw < 8 ? 8 : s_target_cw,
                 s_target_ch < 8 ? 8 : s_target_ch);
        dobui_DrawTextFixed(win, dims_x(), 8, t, COL_SEL, COL_BAR);
    }
    else
    {
        snprintf(t, sizeof(t), "tela %dx%d%s", s_cw, s_ch,
                 s_modified ? " *" : "");
        dobui_DrawTextFixed(win, dims_x(), 8, t, COL_DIM, COL_BAR);
    }

    /* Fila 2: palette + colore corrente. */
    for (int i = 0; i < 24; i++)
    {
        int px = pal_x() + i * (PAL_CELL + 2);
        dobui_FillRect(win, px, 38, PAL_CELL, PAL_CELL, PALETTE[i]);
        if (PALETTE[i] == s_color)
        {
            dobui_DrawRect(win, px - 1, 37, PAL_CELL + 2, PAL_CELL + 2,
                           COL_SEL);
        }
    }
    int curx = pal_x() + 24 * (PAL_CELL + 2) + 10;
    dobui_FillRect(win, curx, 36, 20, 20, s_color);
    dobui_DrawRect(win, curx, 36, 20, 20, COL_FG);

    /* Slider RGB (reintegrate dal v1: regolazione fine del colore).
     * Tre tracce orizzontali; il riempimento e' il valore del canale,
     * tinto del canale stesso. Drag continuo (vedi mousemove). */
    static const char *SL = "RGB";
    for (int ch = 0; ch < 3; ch++)
    {
        int sy = 34 + ch * (SLD_H + 2);
        int val = (int)((s_color >> (16 - 8 * ch)) & 0xFF);
        uint32_t tint = 0x00404040u | ((uint32_t)0xFF << (16 - 8 * ch));
        char lbl[2] = { SL[ch], 0 };
        dobui_DrawTextFixed(win, SLD_X - 12, sy - 2, lbl, COL_DIM, COL_BAR);
        dobui_FillRect(win, SLD_X, sy, SLD_W, SLD_H, 0x00202020);
        dobui_FillRect(win, SLD_X, sy, val * SLD_W / 255, SLD_H, tint);
        dobui_DrawRect(win, SLD_X, sy, SLD_W, SLD_H, COL_DIM);
        snprintf(t, sizeof(t), "%3d", val);
        dobui_DrawTextFixed(win, SLD_X + SLD_W + 6, sy - 2, t,
                            COL_FG, COL_BAR);
    }
}

/* Drag su una slider RGB: ritorna true se (mx,my) la riguarda e
 * aggiorna il canale. Condiviso da click e mousemove. */
static bool rgb_slider_input(int mx, int my)
{
    if (mx < SLD_X - 4 || mx > SLD_X + SLD_W + 4) return false;
    for (int ch = 0; ch < 3; ch++)
    {
        int sy = 34 + ch * (SLD_H + 2);
        if (my < sy - 2 || my >= sy + SLD_H + 2) continue;
        int v = (mx - SLD_X) * 255 / SLD_W;
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        int shift = 16 - 8 * ch;
        s_color = (s_color & ~(0xFFu << shift)) | ((uint32_t)v << shift);
        return true;
    }
    return false;
}

/* ===================================================================
 * ESECUTIVI — coordinate e azioni
 * =================================================================== */

static bool view_to_canvas(int mx, int my, int *cx, int *cy)
{
    if (my < TOP_H) return false;
    *cx = s_scx + (mx) / s_zoom;
    *cy = s_scy + (my - TOP_H) / s_zoom;
    return true;
}

static bool over_grip(int mx, int my)
{
    int gx = (s_cw - s_scx) * s_zoom;
    int gy = (s_ch - s_scy) * s_zoom + TOP_H;
    return mx >= gx - GRIP && mx < gx && my >= gy - GRIP && my < gy;
}

static void do_new(void)
{
    if (s_modified &&
        dobpopup_YesNo("DobPicture", "Perdere le modifiche?") != 0)
        return;
    canvas_alloc(320, 240, false);
    s_scx = s_scy = 0;
    s_path[0] = '\0';
    s_modified = false;
    view_full_repaint();
}

static void do_open(void)
{
    char p[128];
    if (dobfiles_PickFile(".bmp|.png|.jpg|.jpeg", "/DATA/Pictures",
                          p, sizeof(p)) != 0)
        return;

    bool ok = false;
    uint32_t n = 0;
    uint8_t *blob = read_whole(p, &n);
    if (blob != NULL && s_codec != NULL &&
        s_codec->probe(blob, (int)n) != 0)
    {
        uint32_t *px;
        int w, h;
        if (s_codec->decode(blob, n, &px, &w, &h,
                            CANVAS_MAX_W, CANVAS_MAX_H) == 0)
        {
            free(s_canvas);
            s_canvas = px;          /* stesso allocatore: nostro malloc  */
            s_cw = w;
            s_ch = h;
            ok = true;
        }
    }
    free(blob);
    if (!ok)
    {
        ok = bmp_load(p);           /* BMP, o ripiego                    */
    }
    if (!ok)
    {
        dobpopup_Info("DobPicture", "Impossibile aprire il file.");
        return;
    }
    snprintf(s_path, sizeof(s_path), "%s", p);
    s_scx = s_scy = 0;
    s_modified = false;
    view_full_repaint();
}

static void do_save(bool ask_path)
{
    char p[128];
    if (ask_path || s_path[0] == '\0')
    {
        if (dobfiles_PickSavePath("immagine.bmp",
                                  s_codec ? ".bmp|.png|.jpg" : ".bmp",
                                  "/DATA/Pictures", p, sizeof(p)) != 0)
            return;
        snprintf(s_path, sizeof(s_path), "%s", p);
    }

    /* Formato per estensione: PNG/JPEG via codec .mem, BMP nativo. */
    size_t plen = strlen(s_path);
    bool want_png = plen > 4 && strcmp(s_path + plen - 4, ".png") == 0;
    bool want_jpg = (plen > 4 && strcmp(s_path + plen - 4, ".jpg") == 0)
        || (plen > 5 && strcmp(s_path + plen - 5, ".jpeg") == 0);
    if ((want_png || want_jpg) && s_codec != NULL)
    {
        uint8_t *out = NULL;
        uint32_t out_n = 0;
        int rc = want_png
            ? s_codec->encode_png(s_canvas, s_cw, s_ch, &out, &out_n)
            : s_codec->encode_jpeg(s_canvas, s_cw, s_ch, 90,
                                   &out, &out_n);
        bool ok = false;
        if (rc == 0)
        {
            int fd = dobfs_Open(s_path, FS_WRITE | FS_CREATE | FS_TRUNC);
            if (fd >= 0)
            {
                ok = dobfs_Write(fd, out, out_n) == (int)out_n;
                dobfs_Close(fd);
            }
            free(out);
        }
        if (ok) { s_modified = false; present(0, 0); }
        else    { dobpopup_Info("DobPicture", "Scrittura fallita."); }
        return;
    }
    if (bmp_save(s_path))
    {
        s_modified = false;
        present(0, 0);
    }
    else
    {
        dobpopup_Info("DobPicture", "Scrittura fallita.");
    }
}

/* ===================================================================
 * LOGICA — gli event handler orchestrano
 * =================================================================== */

void event_start(void)
{
    s_w = dobui_width();
    s_h = dobui_height();
    codec_load();                   /* PNG/JPEG se il .mem c'e'          */
    canvas_alloc(320, 240, false);
    view_ensure();
    view_full_repaint();
    dobui_set_panel("Nuovo\nApri\nSalva\nSalva con nome");
}

void event_resize(int w, int h)
{
    s_w = w;
    s_h = h;
    view_ensure();                  /* il pannello segue la finestra     */
    view_full_repaint();
}

void event_panel(int cmd)
{
    switch (cmd)
    {
    case 0: do_new();       break;
    case 1: do_open();      break;
    case 2: do_save(false); break;
    case 3: do_save(true);  break;
    }
}

void event_mouseclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    uint32_t win = dobui_window();

    if (y < TOP_H)                  /* toolbar                           */
    {
        if (hit(x, y, zoom_x(), 8, BTN_W, BTN_W) && s_zoom > 1)
        { s_zoom /= 2; view_full_repaint(); return; }
        if (hit(x, y, zoom_x() + BTN_W + 34, 8, BTN_W, BTN_W) && s_zoom < 8)
        { s_zoom *= 2; view_full_repaint(); return; }
        if (hit(x, y, brush_x(), 8, BTN_W, BTN_W) && s_brush > 1)
        { s_brush--; present(0, 0); return; }
        if (hit(x, y, brush_x() + BTN_W + 40, 8, BTN_W, BTN_W) &&
            s_brush < 16)
        { s_brush++; present(0, 0); return; }
        if (rgb_slider_input(x, y)) { present(0, 0); return; }
        for (int i = 0; i < TOOL_COUNT; i++)
        {
            int bx = tool_btn_x(i);
            if (x >= bx && x < bx + CELL && y >= 4 && y < 4 + CELL)
            {
                s_tool = i;
                present(0, 0);      /* solo toolbar                      */
                return;
            }
        }
        for (int i = 0; i < 24; i++)
        {
            int px = pal_x() + i * (PAL_CELL + 2);
            if (x >= px && x < px + PAL_CELL && y >= 38 && y < 38 + PAL_CELL)
            {
                s_color = PALETTE[i];
                present(0, 0);
                return;
            }
        }
        return;
    }

    if (over_grip(x, y))            /* inizio resize tela                */
    {
        s_resizing  = true;
        s_target_cw = s_cw;
        s_target_ch = s_ch;
        dobui_SetCursor(win, CURSOR_RESIZE);
        return;
    }

    int cx, cy;
    if (!view_to_canvas(x, y, &cx, &cy))
        return;

    if (s_tool == TOOL_PAN)         /* inizio trascinamento vista        */
    {
        s_panning = true;
        s_pan_lx = x;
        s_pan_ly = y;
        return;
    }

    switch (s_tool)
    {
    case TOOL_FILL:
    {
        flood_fill(cx, cy, s_color);
        s_modified = true;
        view_full_repaint();        /* il fill puo' toccare tutto        */
        break;
    }
    case TOOL_PICK:
        if (cx >= 0 && cx < s_cw && cy >= 0 && cy < s_ch)
        {
            s_color = s_canvas[cy * s_cw + cx];
            s_tool  = TOOL_PEN;     /* preso il colore, torna a matita   */
            present(0, 0);
        }
        break;
    default:                        /* matita / gomma: inizio tratto     */
    {
        s_drawing = true;
        s_last_cx = cx;
        s_last_cy = cy;
        int dx0 = s_cw, dy0 = s_ch, dx1 = 0, dy1 = 0;
        stamp(cx, cy, s_tool == TOOL_ERASER ? COL_CANVAS_BG : s_color,
              &dx0, &dy0, &dx1, &dy1);
        if (dx0 < dx1)
        {
            s_modified = true;
            dirty_canvas_rect(dx0, dy0, dx1, dy1);
        }
        break;
    }
    }
}

void event_mousemove(int x, int y, uint8_t buttons)
{
    if ((buttons & 1) && y < TOP_H && rgb_slider_input(x, y))
    {
        present(0, 0);              /* drag continuo sulla slider        */
        return;
    }

    if (s_panning)                  /* pan: la vista segue il mouse      */
    {
        s_scx -= (x - s_pan_lx) / s_zoom;
        s_scy -= (y - s_pan_ly) / s_zoom;
        if (s_scx < 0) s_scx = 0;
        if (s_scy < 0) s_scy = 0;
        s_pan_lx = x;
        s_pan_ly = y;
        view_full_repaint();
        return;
    }

    if (s_resizing)                 /* trascinamento della maniglia      */
    {
        int cx, cy;
        if (view_to_canvas(x, y, &cx, &cy))
        {
            s_target_cw = cx;
            s_target_ch = cy;
            present(0, 0);          /* toolbar mostra il bersaglio       */
        }
        return;
    }

    if (!s_drawing)
        return;

    int cx, cy;
    if (!view_to_canvas(x, y, &cx, &cy))
        return;
    int dx0 = s_cw, dy0 = s_ch, dx1 = 0, dy1 = 0;
    stroke_segment(s_last_cx, s_last_cy, cx, cy,
                   s_tool == TOOL_ERASER ? COL_CANVAS_BG : s_color,
                   &dx0, &dy0, &dx1, &dy1);
    s_last_cx = cx;
    s_last_cy = cy;
    if (dx0 < dx1)
    {
        s_modified = true;
        dirty_canvas_rect(dx0, dy0, dx1, dy1);
    }
}

void event_mouserelease(int x, int y, uint8_t buttons)
{
    (void)x; (void)y; (void)buttons;
    uint32_t win = dobui_window();
    if (s_resizing)                 /* applica al rilascio               */
    {
        s_resizing = false;
        dobui_SetCursor(win, CURSOR_DEFAULT);
        if (canvas_alloc(s_target_cw, s_target_ch, true))
        {
            s_modified = true;
        }
        view_full_repaint();
        return;
    }
    s_drawing = false;
    s_panning = false;
}

void event_key(uint8_t key)
{
    switch (key)
    {
    case '+': if (s_brush < 16) { s_brush++; present(0, 0); } break;
    case '-': if (s_brush > 1)  { s_brush--; present(0, 0); } break;
    case 'z': case 'Z':
        if (s_zoom < 8) { s_zoom *= 2; view_full_repaint(); }
        break;
    case 'x': case 'X':
        if (s_zoom > 1) { s_zoom /= 2; view_full_repaint(); }
        break;
    case KEY_LEFT:  if (s_scx > 0) { s_scx -= 16; view_full_repaint(); } break;
    case KEY_RIGHT: s_scx += 16; view_full_repaint(); break;
    case KEY_UP:    if (s_scy > 0) { s_scy -= 16; view_full_repaint(); } break;
    case KEY_DOWN:  s_scy += 16; view_full_repaint(); break;
    }
    if (s_scx < 0) s_scx = 0;
    if (s_scy < 0) s_scy = 0;
}

void event_scroll(int delta)
{
    /* Convenzione MainDOB: Ctrl+rotellina scorre sull'ALTRO asse. */
    if (s_mods & DOBUI_MOD_CTRL)
    {
        s_scx += delta * 24;
        if (s_scx < 0) s_scx = 0;
    }
    else
    {
        s_scy += delta * 24;
        if (s_scy < 0) s_scy = 0;
    }
    view_full_repaint();
}

void event_modchange(uint8_t mods)
{
    s_mods = mods;                  /* cache per la convenzione scroll   */
}

int main(void)
{
    dobui_run("DobPicture", 760, 540);
    return 0;
}
