/* MainDOB DobInterface 2.0 — foglio: cmdbuf.c
 *
 * Il wire-format dei contenuti client, dall'IPC ai pixel:
 *
 *  - RIASSEMBLAGGIO dei cmdbuf segmentati (il buffer IPC strippa in
 *    silenzio i payload oltre IPC_BUF_SIZE; lo stub client spedisce a
 *    segmenti e qui si ricompone, per finestra o per widget);
 *  - DECODER a visitor: unica fonte di verita' del formato; i
 *    consumatori forniscono callback per-opcode (bake finestra qui,
 *    SHM widget qui, miniature MC e screenshot nei loro fogli);
 *  - REPLAY FINESTRA: visitor win_v_* che emette draw diretti dv_*
 *    sulla surface del corpo, con reveal typed-text e pannello SHM;
 *  - REPLAY WIDGET: visitor widget_v_* che rasterizza in software
 *    nell'SHM del widget e ne specchia la texture di cache;
 *  - TEX POOL: ricerca degli handle e, novita' 2.0, accesso allo
 *    SPECCHIO CPU dei raster (win_tex_pool_cpu_find) — e' il verbo
 *    che abilita le miniature FEDELI del Mission Control: i blit_tex
 *    nelle anteprime non sono piu' buchi, si leggono i pixel veri
 *    dalla copia cache gia' mantenuta per lo screenshot. */

#include "di_internal.h"

/* ================= verbi esecutivi: lettori di filo =================== */

/* Little-endian, senza padding, come sul filo. */
static inline uint16_t cb_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static inline int16_t cb_i16(const uint8_t *p)
{
    return (int16_t)cb_u16(p);
}
static inline uint32_t cb_u32(const uint8_t *p)
{
    return (uint32_t)p[0]        | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ================= riassemblaggio ===================================== */

void cmdbuf_reasm_free(cmdbuf_reasm_t *r)
{
    if (r->buf) free(r->buf);
    memset(r, 0, sizeof(*r));
}

/* Ingest di un messaggio Invalidate. Ritorna true quando il cmdbuf e'
 * COMPLETO: out_buf e out_size descrivono il buffer da consumare, che
 * per legacy e single-segment e' il payload del messaggio stesso
 * (zero copie) e per il multi-segmento e' r->buf (valido fino al
 * prossimo ingest su questo reasm). false = segmento intermedio
 * assorbito, o sequenza scartata: niente da consumare. */
bool cmdbuf_reasm_ingest(cmdbuf_reasm_t *r, const dob_msg_t *msg,
                         const uint8_t **out_buf, uint32_t *out_size)
{
    const uint8_t *pl = (const uint8_t *)msg->payload;
    uint32_t       sz = msg->payload_size;

    /* Legacy monoblocco o segmento unico: consegna diretta. */
    if (msg->arg2 <= 1)
    {
        r->expect_seq = 0;              /* un parziale in corso decade   */
        *out_buf  = pl;
        *out_size = sz;
        return (pl != NULL && sz > 0);
    }

    uint32_t seq   = msg->arg1;
    uint32_t nsegs = msg->arg2;
    uint32_t total = msg->arg3;

    if (pl == NULL || sz == 0 || total == 0 ||
        total > DOBUI_CMDBUF_MAX_BYTES ||
        nsegs != (total + DOBUI_CMDBUF_SEG_BYTES - 1u) / DOBUI_CMDBUF_SEG_BYTES)
    {
        r->expect_seq = 0;              /* malformato: scarta il parziale */
        return false;
    }

    if (seq == 0)
    {
        /* Nuovo frame: (ri)dimensiona l'accumulo e riparti. */
        if (total > r->cap)
        {
            uint8_t *nb = (uint8_t *)realloc(r->buf, total);
            if (!nb)
            {
                r->expect_seq = 0;      /* OOM: questo frame si perde    */
                return false;
            }
            r->buf = nb;
            r->cap = total;
        }
        r->bytes       = 0;
        r->total_segs  = nsegs;
        r->total_bytes = total;
    }
    else if (seq != r->expect_seq || r->total_segs != nsegs
                                  || r->total_bytes != total)
    {
        r->expect_seq = 0;              /* buco o frame mischiato        */
        return false;
    }

    if (r->buf == NULL || r->bytes + sz > r->total_bytes)
    {
        r->expect_seq = 0;
        return false;
    }

    memcpy(r->buf + r->bytes, pl, sz);
    r->bytes      += sz;
    r->expect_seq  = seq + 1;

    if (r->expect_seq == r->total_segs && r->bytes == r->total_bytes)
    {
        r->expect_seq = 0;              /* pronto per il prossimo frame  */
        *out_buf  = r->buf;
        *out_size = r->total_bytes;
        return true;
    }
    return false;                       /* segmento intermedio assorbito */
}

/* ================= decoder (unica fonte di verita') =================== */

void cmdbuf_decode(const uint8_t *buf, uint32_t size,
                   const cmdbuf_visitor_t *v)
{
    if (size <= DOBUI_CMDBUF_HDR_SIZE) return;
    const uint8_t *p   = buf + DOBUI_CMDBUF_HDR_SIZE;
    const uint8_t *end = buf + size;

    while (p < end)
    {
        uint8_t op = *p++;
        switch (op)
        {
        case DOBUI_OP_FILL_RECT: {
            if ((uint32_t)(end - p) < (DOBUI_REC_FILL_RECT_SZ - 1)) return;
            int16_t  x  = cb_i16(p); p += 2;
            int16_t  y  = cb_i16(p); p += 2;
            uint16_t rw = cb_u16(p); p += 2;
            uint16_t rh = cb_u16(p); p += 2;
            uint32_t c  = cb_u32(p); p += 4;
            if (v->fill_rect) v->fill_rect(v->ctx, x, y, rw, rh, c);
            break;
        }
        case DOBUI_OP_DRAW_RECT: {
            if ((uint32_t)(end - p) < (DOBUI_REC_DRAW_RECT_SZ - 1)) return;
            int16_t  x  = cb_i16(p); p += 2;
            int16_t  y  = cb_i16(p); p += 2;
            uint16_t rw = cb_u16(p); p += 2;
            uint16_t rh = cb_u16(p); p += 2;
            uint32_t c  = cb_u32(p); p += 4;
            if (v->draw_rect) v->draw_rect(v->ctx, x, y, rw, rh, c);
            break;
        }
        case DOBUI_OP_DRAW_PIXEL: {
            if ((uint32_t)(end - p) < (DOBUI_REC_DRAW_PIXEL_SZ - 1)) return;
            int16_t  x = cb_i16(p); p += 2;
            int16_t  y = cb_i16(p); p += 2;
            uint32_t c = cb_u32(p); p += 4;
            if (v->draw_pixel) v->draw_pixel(v->ctx, x, y, c);
            break;
        }
        case DOBUI_OP_DRAW_TEXT:
        case DOBUI_OP_DRAW_TEXT_FIXED: {
            if ((uint32_t)(end - p) < (DOBUI_REC_DRAW_TEXT_HDR - 1)) return;
            int16_t  x   = cb_i16(p); p += 2;
            int16_t  y   = cb_i16(p); p += 2;
            uint32_t fg  = cb_u32(p); p += 4;
            uint32_t bg  = cb_u32(p); p += 4;
            uint16_t len = cb_u16(p); p += 2;
            if ((uint32_t)(end - p) < len) return;
            const uint8_t *text = p;
            p += len;
            int fixed = (op == DOBUI_OP_DRAW_TEXT_FIXED);
            if (v->draw_text) v->draw_text(v->ctx, x, y, fg, bg, text, len, fixed);
            break;
        }
        case DOBUI_OP_BLIT_INLINE: {
            if ((uint32_t)(end - p) < (DOBUI_REC_BLIT_INLINE_HDR - 1)) return;
            int16_t  x  = cb_i16(p); p += 2;
            int16_t  y  = cb_i16(p); p += 2;
            uint16_t sw = cb_u16(p); p += 2;
            uint16_t sh = cb_u16(p); p += 2;
            uint64_t pix_bytes = (uint64_t)sw * (uint64_t)sh * 4ull;
            if ((uint64_t)(end - p) < pix_bytes) return;
            const uint32_t *pixels = (const uint32_t *)p;
            p += pix_bytes;
            if (v->blit_inline) v->blit_inline(v->ctx, x, y, sw, sh, pixels);
            break;
        }
        case DOBUI_OP_BLIT_TEX: {
            if ((uint32_t)(end - p) < (DOBUI_REC_BLIT_TEX_SZ - 1)) return;
            int16_t  x      = cb_i16(p); p += 2;
            int16_t  y      = cb_i16(p); p += 2;
            uint32_t handle = cb_u32(p); p += 4;
            uint16_t sw     = cb_u16(p); p += 2;
            uint16_t sh     = cb_u16(p); p += 2;
            if (v->blit_tex) v->blit_tex(v->ctx, x, y, handle, sw, sh);
            break;
        }
        case DOBUI_OP_BLIT_SHMPANEL: {
            if ((uint32_t)(end - p) < (DOBUI_REC_BLIT_SHMPANEL_SZ - 1)) return;
            int16_t  x   = cb_i16(p); p += 2;
            int16_t  y   = cb_i16(p); p += 2;
            uint16_t sw  = cb_u16(p); p += 2;
            uint16_t sh  = cb_u16(p); p += 2;
            uint16_t by0 = cb_u16(p); p += 2;
            uint16_t brs = cb_u16(p); p += 2;
            if (v->blit_shmpanel)
                v->blit_shmpanel(v->ctx, x, y, sw, sh, by0, brs);
            break;
        }
        default:
            /* Opcode ignoto -> cmdbuf malformato, scarta il resto. */
            return;
        }
    }
}

/* ================= tex pool: ricerca e specchio CPU =================== */

/* Scansione lineare; il pool e' al massimo WIN_TEX_POOL_SIZE entry. */
int tex_pool_find(window_t *w, dv_texture_t handle)
{
    for (uint32_t i = 0; i < w->tex_pool_count; i++)
        if (w->tex_pool[i].handle == handle) return (int)i;
    return -1;
}

/* Specchio CPU di un handle del pool: ritorna i pixel BGRA (e misure)
 * della copia cache mantenuta accanto alla texture, o NULL se
 * l'handle non e' nel pool o lo specchio e' saltato per OOM.
 * E' il verbo delle ricomposizioni software: screenshot e miniature
 * fedeli del Mission Control leggono da qui i raster veri. */
const uint32_t *win_tex_pool_cpu_find(window_t *w, uint32_t handle,
                                      uint16_t *w_out, uint16_t *h_out)
{
    int i = tex_pool_find(w, (dv_texture_t)handle);
    if (i < 0 || !w->tex_pool[i].cpu) return NULL;
    if (w_out) *w_out = w->tex_pool[i].w;
    if (h_out) *h_out = w->tex_pool[i].h;
    return w->tex_pool[i].cpu;
}

/* ================= visitor finestra (bake su surface) ================= */

/* Contesto del replay finestra. Le coordinate del cmdbuf sono
 * relative al CORPO (origine = angolo alto-sinistro interno al
 * chrome); (+1, +1+WIN_HEADER_H) le traduce in coordinate surface
 * coerenti col chrome gia' emesso dal chiamante (win_bake). */
typedef struct {
    window_t *w;
    int       body_x_off;
    int       body_y_off;
    int       body_w;
    int       body_h;
    /* Frazione di reveal typed-text = reveal_num / reveal_den in
     * [0,1]. reveal_den == 0 e' la sentinella di regime: ogni run
     * intera, nessun troncamento. Fissata per-bake da win_bake. */
    uint32_t  reveal_num;
    uint32_t  reveal_den;
} win_replay_ctx_t;

static void win_v_fill_rect(void *ctx, int x, int y, int rw, int rh, uint32_t c)
{
    win_replay_ctx_t *r = (win_replay_ctx_t *)ctx;
    int x0 = x, y0 = y, x1 = x + rw, y1 = y + rh;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > r->body_w) x1 = r->body_w;
    if (y1 > r->body_h) y1 = r->body_h;
    if (x0 >= x1 || y0 >= y1) return;
    dv_rect_t dr = {
        (uint32_t)(x0 + r->body_x_off), (uint32_t)(y0 + r->body_y_off),
        (uint32_t)(x1 - x0),            (uint32_t)(y1 - y0)
    };
    dv_fill_rect(r->w->body_surf, dr, dv_color_from_u32(c));
}

static void win_v_draw_rect(void *ctx, int x, int y, int rw, int rh, uint32_t c)
{
    win_replay_ctx_t *r = (win_replay_ctx_t *)ctx;
    if (rw <= 0 || rh <= 0) return;
    /* 4 fill sottili — economico ed evita un op dedicato di outline. */
    dv_rect_t top    = { (uint32_t)(x      + r->body_x_off),
                         (uint32_t)(y      + r->body_y_off),
                         (uint32_t)rw, 1 };
    dv_rect_t bot    = { (uint32_t)(x      + r->body_x_off),
                         (uint32_t)(y+rh-1 + r->body_y_off),
                         (uint32_t)rw, 1 };
    dv_rect_t left   = { (uint32_t)(x      + r->body_x_off),
                         (uint32_t)(y      + r->body_y_off),
                         1, (uint32_t)rh };
    dv_rect_t right  = { (uint32_t)(x+rw-1 + r->body_x_off),
                         (uint32_t)(y      + r->body_y_off),
                         1, (uint32_t)rh };
    dv_color_t cc = dv_color_from_u32(c);
    dv_fill_rect(r->w->body_surf, top,   cc);
    dv_fill_rect(r->w->body_surf, bot,   cc);
    dv_fill_rect(r->w->body_surf, left,  cc);
    dv_fill_rect(r->w->body_surf, right, cc);
}

static void win_v_draw_pixel(void *ctx, int x, int y, uint32_t c)
{
    win_replay_ctx_t *r = (win_replay_ctx_t *)ctx;
    if (x < 0 || y < 0 || x >= r->body_w || y >= r->body_h) return;
    dv_rect_t dr = {
        (uint32_t)(x + r->body_x_off), (uint32_t)(y + r->body_y_off), 1, 1
    };
    dv_fill_rect(r->w->body_surf, dr, dv_color_from_u32(c));
}

static void win_v_draw_text(void *ctx, int x, int y, uint32_t fg, uint32_t bg,
                            const uint8_t *text, uint32_t len, int fixed)
{
    win_replay_ctx_t *r = (win_replay_ctx_t *)ctx;

    /* Passata di sfondo: un solo fill_rect sull'intera run se bg e'
     * opaco. Le app chiamano DrawText quasi sempre con bg pieno
     * uguale al fill circostante; emettere il bg una volta costa
     * molto meno dei puntini di bg per-glifo. */
    if (len > 0)
    {
        int rw = fixed ? (int)len * DOB_FONT_W
                       : dob_text_width((const char *)text, len);
        int rx = x, ry = y, rh = DOB_FONT_H;
        int x0 = rx, y0 = ry, x1 = rx + rw, y1 = ry + rh;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > r->body_w) x1 = r->body_w;
        if (y1 > r->body_h) y1 = r->body_h;
        if (x0 < x1 && y0 < y1)
        {
            dv_rect_t br = {
                (uint32_t)(x0 + r->body_x_off),
                (uint32_t)(y0 + r->body_y_off),
                (uint32_t)(x1 - x0),
                (uint32_t)(y1 - y0)
            };
            dv_fill_rect(r->w->body_surf, br, dv_color_from_u32(bg));
        }
    }

    /* Passata glifi: array di dv_glyph_t con indici nell'atlante
     * (= byte ASCII). Tetto a 256 glifi per record per non far
     * esplodere lo stack — le etichette realistiche sono molto piu'
     * corte; le run lunghe vanno in piu' chiamate draw_glyphs. */
    if (g_glyph_atlas != DV_HANDLE_NONE && len > 0)
    {
        /* Reveal typed-text: tronca la run ai primi reveal_k glifi.
         * reveal_den == 0 (regime) => run intera. Altrimenti conta i
         * glifi N della run (il decode e' UTF-8 aware: un conteggio a
         * byte sbaglierebbe con gli accenti) e rivela
         *     k = (reveal_num * N) / reveal_den + 1
         * cosi' il primo glifo appare a p=0 (scattante, come un
         * terminale che comincia subito a riempire) e l'ultimo entro
         * p=1. Tutta la matematica e' a 32 bit: N <= 64 KiB e
         * reveal_num <= TYPE_ANIM_MS, il prodotto non trabocca u32 e
         * non si tira dentro nessun helper libgcc a 64 bit. Se si
         * vuole un'altra curva di reveal si cambia il "+ 1" in
         * ceil/round — e' l'unica riga che detta il passo. */
        uint32_t reveal_k = 0xFFFFFFFFu;   /* illimitato (regime) */
        if (r->reveal_den != 0)
        {
            uint32_t total_g = 0, cb = 0;
            while (cb < len)
            {
                uint8_t gg;
                cb += dob_font_decode(text, len, cb, &gg);
                total_g++;
            }
            reveal_k = (r->reveal_num * total_g) / r->reveal_den + 1u;
            if (reveal_k > total_g) reveal_k = total_g;
        }

        dv_glyph_t glyphs[256];
        uint32_t   gi = 0;
        dv_color_t fgc = dv_color_from_u32(fg);
        uint32_t   bi = 0;                /* indice byte             */
        int        penx = 0;              /* pen x dentro la run     */
        uint32_t   emitted = 0;           /* glifi rivelati finora   */
        while (bi < len && emitted < reveal_k)
        {
            if (gi == 256)
            {
                dv_draw_glyphs(r->w->body_surf, g_glyph_atlas,
                               glyphs, gi, fgc);
                gi = 0;
            }
            uint8_t gidx;
            bi += dob_font_decode(text, len, bi, &gidx);
            /* In proporzionale sposta il glifo (a cella piena) a
             * sinistra del suo ink-left cosi' l'inchiostro cade sul
             * pen; le colonne vuote della cella sono trasparenti
             * nell'atlante, lo shift e' gratis. In fisso la cella
             * siede sull'origine cella (monospace). */
            int shift = fixed ? 0 : dob_font_left(gidx);
            glyphs[gi].glyph_index = gidx;
            glyphs[gi].x = x + penx - shift + r->body_x_off;
            glyphs[gi].y = y + r->body_y_off;
            gi++;
            emitted++;
            penx += fixed ? DOB_FONT_W : dob_font_advance(gidx);
        }
        if (gi > 0)
            dv_draw_glyphs(r->w->body_surf, g_glyph_atlas,
                           glyphs, gi, fgc);
    }
}

static void win_v_blit_inline(void *ctx, int x, int y, int sw, int sh,
                              const uint32_t *pixels)
{
    win_replay_ctx_t *r = (win_replay_ctx_t *)ctx;

    /* Prossimo slot del round-robin (incapsulato in video.c) cosi' il
     * blit accodato atterra su una texture che nessun altro in questo
     * frame sta per sovrascrivere. */
    uint32_t dim = 0;
    dv_texture_t scratch = video_blit_scratch_next(&dim);
    if (scratch == DV_HANDLE_NONE) return;
    if (sw > (int)dim || sh > (int)dim) return;

    /* Upload nella scratch scelta, poi blit_alpha che la referenzia.
     * La convenzione alpha conserva la semantica legacy di
     * BlitBuffer — pixel sorgente == 0xFF000000 e' trasparente. */
    dv_rect_t sr = { 0, 0, (uint32_t)sw, (uint32_t)sh };
    dv_texture_update_region(scratch, sr,
                             pixels, (uint32_t)sw * 4u);
    dv_point_t dp = { x + r->body_x_off, y + r->body_y_off };
    dv_blit_pixel_alpha((dv_surface_t)scratch, sr, r->w->body_surf, dp);
}

static void win_v_blit_tex(void *ctx, int x, int y, uint32_t handle,
                           int sw, int sh)
{
    win_replay_ctx_t *r = (win_replay_ctx_t *)ctx;
    if (tex_pool_find(r->w, (dv_texture_t)handle) < 0) return;
    dv_rect_t  sr = { 0, 0, (uint32_t)sw, (uint32_t)sh };
    dv_point_t dp = { x + r->body_x_off, y + r->body_y_off };
    dv_blit_pixel_alpha((dv_surface_t)handle, sr, r->w->body_surf, dp);
}

/* Blit del pannello SHM nel corpo: una sola copia CPU (SHM -> body
 * SYSRAM) via dv_texture_update_region — texture e surface
 * condividono lo spazio handle nel driver, e update_region e'
 * esattamente "scrivi questi pixel CPU nel rettangolo", con il
 * damage marcato di suo. Clip manuale: il rettangolo va serrato al
 * corpo E al pannello. */
static void win_v_blit_shmpanel(void *ctx, int x, int y, int sw, int sh,
                                unsigned band_y0, unsigned band_rows)
{
    win_replay_ctx_t *r = (win_replay_ctx_t *)ctx;
    window_t *w = r->w;
    if (w->panel_shm_id < 0 || !w->panel_ptr) return;

    /* La banda del client vale solo se il corpo riflette gia' il
     * pannello E il rettangolo di blit non e' cambiato: altrimenti
     * fuori banda ci sarebbero pixel stantii o il bianco del fill —
     * promozione a copia integrale e risincronizzazione. */
    bool rect_same = w->panel_last_x == (int16_t)x &&
                     w->panel_last_y == (int16_t)y &&
                     w->panel_last_w == (uint16_t)sw &&
                     w->panel_last_h == (uint16_t)sh;
    bool can_band  = w->panel_synced && rect_same;
    w->panel_last_x = (int16_t)x;  w->panel_last_y = (int16_t)y;
    w->panel_last_w = (uint16_t)sw; w->panel_last_h = (uint16_t)sh;
    w->panel_synced = true;

    if (can_band && band_y0 == DOBUI_SHMPANEL_UNCHANGED
                 && band_rows == DOBUI_SHMPANEL_UNCHANGED)
        return;                       /* contenuto invariato: zero copia */

    int sx = 0, sy = 0;
    if (x < 0) { sx = -x; sw += x; x = 0; }
    if (y < 0) { sy = -y; sh += y; y = 0; }

    if (can_band && band_rows > 0 && band_rows != DOBUI_SHMPANEL_UNCHANGED)
    {
        /* Restringi alla banda dichiarata (coordinate pannello). */
        int b0 = (int)band_y0, b1 = (int)band_y0 + (int)band_rows;
        if (b0 < sy) b0 = sy;
        if (b1 > sy + sh) b1 = sy + sh;
        if (b0 >= b1) return;
        y  += b0 - sy;
        sy  = b0;
        sh  = b1 - b0;
    }

    if (sw > (int)w->panel_w - sx) sw = (int)w->panel_w - sx;
    if (sh > (int)w->panel_h - sy) sh = (int)w->panel_h - sy;
    if (x + sw > r->body_w) sw = r->body_w - x;
    if (y + sh > r->body_h) sh = r->body_h - y;
    if (sw <= 0 || sh <= 0) return;

    dv_rect_t dr = { x + r->body_x_off, y + r->body_y_off,
                     (uint32_t)sw, (uint32_t)sh };
    const uint32_t *src = w->panel_ptr + (size_t)sy * w->panel_w + sx;
    dv_texture_update_region((dv_texture_t)w->body_surf, dr,
                             src, (uint32_t)w->panel_w * 4u);
}

/* ================= terra di mezzo: prescan e interrogazioni =========== */

/* Prescan: estrae il rettangolo del PRIMO record SHMPANEL del cmdbuf
 * (coordinate body-content). Serve a win_bake per riempire di bianco
 * SOLO le fasce fuori dal pannello: dentro, i pixel del corpo sono
 * garantiti dal pannello stesso — copia integrale se non
 * sincronizzato, banda valida altrimenti.
 * NOTA il baco che questo sostituisce (inglobato come lezione): la
 * versione precedente saltava il bianco solo se il pannello copriva
 * l'INTERO corpo, cosa che non accade mai (il pannello di un editor
 * parte sotto la ribbon) — il bianco cancellava tutto e il blit a
 * banda ne ricopiava solo una fascia: area di lavoro bianca dopo la
 * prima paint incrementale. */
static void win_v_scan_shmpanel(void *ctx, int x, int y, int sw, int sh,
                                unsigned band_y0, unsigned band_rows)
{
    (void)band_y0; (void)band_rows;
    shm_rect_scan_t *s = (shm_rect_scan_t *)ctx;
    if (s->found) return;                 /* il primo record vince */
    s->found = true;
    s->x = x; s->y = y; s->w = sw; s->h = sh;
}

bool cmdbuf_shmpanel_rect(const uint8_t *buf, uint32_t size,
                          shm_rect_scan_t *out)
{
    out->found = false;
    cmdbuf_visitor_t v = {
        .ctx           = out,
        .blit_shmpanel = win_v_scan_shmpanel,
    };
    cmdbuf_decode(buf, size, &v);
    return out->found;
}

/* cmdbuf_has_text — true sse il cmdbuf contiene almeno una run di
 * testo. Fa da cancello all'effetto typed-text: una finestra dal
 * corpo di sola grafica (un gioco, un visualizzatore di immagini)
 * non ha nulla da "battere", inutile avviare una pompa da 1 s che
 * brucerebbe repaint pieni senza cambiamento visibile. */
static void win_v_scan_text(void *ctx, int x, int y, uint32_t fg, uint32_t bg,
                            const uint8_t *text, uint32_t len, int fixed)
{
    (void)x; (void)y; (void)fg; (void)bg; (void)text; (void)fixed;
    if (len > 0) *(bool *)ctx = true;
}

bool cmdbuf_has_text(const uint8_t *buf, uint32_t size)
{
    bool found = false;
    /* L'init designata azzera ogni callback non nominato e il decoder
     * guarda ogni puntatore: qui gira solo draw_text. */
    cmdbuf_visitor_t v = {
        .ctx       = &found,
        .draw_text = win_v_scan_text,
    };
    cmdbuf_decode(buf, size, &v);
    return found;
}

/* ================= logica ad alto livello: i due replay =============== */

/* Replay del cmdbuf di una finestra in draw diretti sulla surface del
 * corpo. reveal_num/reveal_den: frazione typed-text (den=0: regime). */
void win_replay_cmdbuf(window_t *w, const uint8_t *buf, uint32_t size,
                       uint32_t reveal_num, uint32_t reveal_den)
{
    win_replay_ctx_t rc = {
        .w          = w,
        .body_x_off = 1,
        .body_y_off = 1 + WIN_HEADER_H,
        .body_w     = w->width,
        .body_h     = w->height,
        .reveal_num = reveal_num,
        .reveal_den = reveal_den,
    };
    cmdbuf_visitor_t v = {
        .ctx           = &rc,
        .fill_rect     = win_v_fill_rect,
        .draw_rect     = win_v_draw_rect,
        .draw_pixel    = win_v_draw_pixel,
        .draw_text     = win_v_draw_text,
        .blit_inline   = win_v_blit_inline,
        .blit_tex      = win_v_blit_tex,
        .blit_shmpanel = win_v_blit_shmpanel,
    };
    cmdbuf_decode(buf, size, &v);
}

/* ================= visitor widget (raster software su SHM) ============ */

typedef struct {
    uint32_t *fb;
    int       fb_w;
    int       fb_h;
} widget_replay_ctx_t;

static void widget_v_fill_rect(void *ctx, int x, int y, int w, int h, uint32_t c)
{
    widget_replay_ctx_t *r = (widget_replay_ctx_t *)ctx;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > r->fb_w) x1 = r->fb_w;
    int y1 = y + h; if (y1 > r->fb_h) y1 = r->fb_h;
    for (int yy = y0; yy < y1; yy++)
    {
        uint32_t *row = &r->fb[yy * r->fb_w];
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

static void widget_v_draw_rect(void *ctx, int x, int y, int w, int h, uint32_t c)
{
    widget_replay_ctx_t *r = (widget_replay_ctx_t *)ctx;
    if (w <= 0 || h <= 0) return;
    for (int xx = x; xx < x + w; xx++)
    {
        if (xx < 0 || xx >= r->fb_w) continue;
        if (y >= 0 && y < r->fb_h)            r->fb[y * r->fb_w + xx] = c;
        if (h > 1 && y+h-1 >= 0 && y+h-1 < r->fb_h)
            r->fb[(y + h - 1) * r->fb_w + xx] = c;
    }
    for (int yy = y + 1; yy < y + h - 1; yy++)
    {
        if (yy < 0 || yy >= r->fb_h) continue;
        if (x >= 0 && x < r->fb_w)            r->fb[yy * r->fb_w + x] = c;
        if (w > 1 && x+w-1 >= 0 && x+w-1 < r->fb_w)
            r->fb[yy * r->fb_w + (x + w - 1)] = c;
    }
}

static void widget_v_draw_pixel(void *ctx, int x, int y, uint32_t c)
{
    widget_replay_ctx_t *r = (widget_replay_ctx_t *)ctx;
    if (x >= 0 && x < r->fb_w && y >= 0 && y < r->fb_h)
        r->fb[y * r->fb_w + x] = c;
}

static void widget_v_draw_text(void *ctx, int x, int y, uint32_t fg, uint32_t bg,
                               const uint8_t *text, uint32_t len, int fixed)
{
    widget_replay_ctx_t *r = (widget_replay_ctx_t *)ctx;
    int penx = x;
    uint32_t i = 0;
    while (i < len)
    {
        uint8_t gidx;
        i += dob_font_decode(text, len, i, &gidx);
        int adv = fixed ? DOB_FONT_W : dob_font_advance(gidx);
        int lft = fixed ? 0 : dob_font_left(gidx);
        const uint8_t *glyph = dob_font_data[gidx];
        for (int row = 0; row < DOB_FONT_H; row++)
        {
            int py = y + row;
            if (py < 0 || py >= r->fb_h) continue;
            uint32_t *drow = &r->fb[py * r->fb_w];
            /* Striscia di sfondo sull'avanzamento del glifo, cosi' la
             * run tiene uno sfondo opaco continuo come prima. */
            for (int c = 0; c < adv; c++)
            {
                int px = penx + c;
                if (px >= 0 && px < r->fb_w) drow[px] = bg;
            }
            /* Inchiostro, spostato cosi' la colonna inchiostrata
             * `lft` cade sul pen. */
            uint8_t bits = glyph[row];
            for (int col = 0; col < DOB_FONT_W; col++)
            {
                if (!((bits >> (DOB_FONT_W - 1 - col)) & 1)) continue;
                int px = penx + col - lft;
                if (px >= 0 && px < r->fb_w) drow[px] = fg;
            }
        }
        penx += adv;
    }
}

static void widget_v_blit_inline(void *ctx, int x, int y, int sw, int sh,
                                 const uint32_t *src)
{
    widget_replay_ctx_t *r = (widget_replay_ctx_t *)ctx;
    for (int yy = 0; yy < sh; yy++)
    {
        int dy = y + yy;
        if (dy < 0 || dy >= r->fb_h) continue;
        const uint32_t *srow = &src[yy * sw];
        uint32_t       *drow = &r->fb[dy * r->fb_w];
        for (int xx = 0; xx < sw; xx++)
        {
            int dx = x + xx;
            if (dx < 0 || dx >= r->fb_w) continue;
            /* Semantica legacy di BlitBuffer:
             * sorgente == 0xFF000000 e' trasparente. */
            if (srow[xx] != 0xFF000000u) drow[dx] = srow[xx];
        }
    }
}

/* widget_v_blit_tex deliberatamente assente: i widget non usano il
 * texture pool lato server (nessuna cmdlist per-widget), quindi i
 * record BLIT_TEX sono saltati in silenzio (callback NULL). */

/* Replay del cmdbuf di un widget: raster software nell'SHM, poi
 * specchio nella texture di cache cosi' wpanel_draw puo' blittarla
 * nel backbuf. */
void widget_replay_cmdbuf_to_shm(widget_slot_t *ws,
                                 const uint8_t *buf, uint32_t size)
{
    if (!ws->buffer || ws->width <= 0 || ws->height <= 0) return;

    /* Clear a nero trasparente — il tray compone con sfondo alpha=0
     * per lasciar trasparire il BG del wpanel dove il widget non ha
     * disegnato. */
    for (int i = 0; i < ws->width * ws->height; i++) ws->buffer[i] = 0;

    widget_replay_ctx_t rc = {
        .fb   = ws->buffer,
        .fb_w = ws->width,
        .fb_h = ws->height,
    };
    cmdbuf_visitor_t v = {
        .ctx         = &rc,
        .fill_rect   = widget_v_fill_rect,
        .draw_rect   = widget_v_draw_rect,
        .draw_pixel  = widget_v_draw_pixel,
        .draw_text   = widget_v_draw_text,
        .blit_inline = widget_v_blit_inline,
        .blit_tex    = NULL,
    };
    cmdbuf_decode(buf, size, &v);

    if (ws->cache_tex != DV_HANDLE_NONE)
    {
        dv_rect_t r = { 0, 0, (uint32_t)ws->width, (uint32_t)ws->height };
        dv_texture_update_region(ws->cache_tex, r,
                                 ws->buffer, (uint32_t)ws->width * 4u);
    }
}
