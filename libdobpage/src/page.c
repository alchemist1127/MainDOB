/* libdobpage -- page-object engine implementation.
 *
 * Paginates the layout's continuous content column into sheets, draws each
 * visible sheet's content into a pooled RAM surface (0x00RRGGBB), and maps
 * between content space and window space. Glyphs are rasterized once and
 * cached. Only visible sheets hold surfaces.
 */

#include <dobpage/dobpage.h>
#include <dobfont/dobfont.h>
#include <string.h>
#include <stdlib.h>

#define POOL    3        /* live sheet surfaces (>= max simultaneously visible) */
#define GAP     16       /* px between sheets on the desk */
#define TOPPAD  16       /* px above the first / below the last sheet */

static int ri(float v) { return (int)(v + (v < 0 ? -0.5f : 0.5f)); }

/* ---- glyph cache (persistent across renders) ---- */

typedef struct { df_face *face; uint32_t gid; int px_i, emb_i, sl_i; df_bitmap bm; } gentry;
typedef struct { gentry *e; int n, cap; } gcache;

static df_bitmap *cache_get(gcache *c, df_face *face, uint32_t gid, float px, float emb, float sl)
{
    int px_i = ri(px), emb_i = ri(emb * 16.0f), sl_i = ri(sl * 256.0f);
    for (int i = 0; i < c->n; i++) {
        gentry *e = &c->e[i];
        if (e->face == face && e->gid == gid && e->px_i == px_i && e->emb_i == emb_i && e->sl_i == sl_i)
            return &e->bm;
    }
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 128;
        gentry *ne = (gentry *)realloc(c->e, (size_t)nc * sizeof(gentry));
        if (!ne) return NULL;
        c->e = ne; c->cap = nc;
    }
    gentry *e = &c->e[c->n];
    e->face = face; e->gid = gid; e->px_i = px_i; e->emb_i = emb_i; e->sl_i = sl_i;
    memset(&e->bm, 0, sizeof(e->bm));
    df_raster_req req; req.px_size = px; req.embolden = emb; req.slant = sl; req.shift_x = 0;
    if (df_rasterize(face, gid, &req, &e->bm) != DF_OK) memset(&e->bm, 0, sizeof(e->bm));
    c->n++;
    return &e->bm;
}
static void cache_free(gcache *c) { for (int i = 0; i < c->n; i++) df_free_bitmap(&c->e[i].bm); free(c->e); c->e = NULL; c->n = c->cap = 0; }

/* ---- engine ---- */

typedef struct { uint32_t *buf; int page; bool valid; uint32_t lru; } slot;

/* Per-page geometry.  A document can have several sections, each with its own
 * paper size, margins and background; a section break forces a new page.  The
 * engine paginates into pages[] and every render / coordinate path reads it. */
typedef struct
{
    float    c_top;            /* content-y of this page's top                 */
    int      page_w, page_h;   /* paper size, px                               */
    float    content_top, content_left, content_hprint;
    uint32_t page_bg;          /* opaque paper colour (0xFFRRGGBB)             */
    int      desk_y;           /* y of this page on the desk (cumulative)      */
} pinfo;

struct dp_engine
{
    const df_layout *L;
    int   page_w, page_h;     /* render scratch (current page) + s0 fallback   */
    uint32_t page_bg;         /* global default paper colour (dp_set_page_bg)  */
    float content_top, content_height;   /* content_top is render scratch      */

    pinfo   *pages;           /* one entry per laid-out page                   */
    uint32_t npages, pages_cap;
    int   max_pw, max_ph;     /* largest page over all sections (pool sizing)  */

    slot  slots[POOL];
    int   pool_pw, pool_ph;   /* size the slot buffers are allocated for       */

    int   win_w, win_h, scroll, scroll_x;
    uint32_t lru_clock;
    gcache cache;
};

/* ---- compositing into a sheet surface ---- */

static uint32_t blend_argb(uint32_t dst, uint8_t sr, uint8_t sg, uint8_t sb, uint32_t a)
{
    uint8_t dr = (dst >> 16) & 0xff, dg = (dst >> 8) & 0xff, db = dst & 0xff;
    uint32_t r = (sr * a + dr * (255 - a)) / 255;
    uint32_t g = (sg * a + dg * (255 - a)) / 255;
    uint32_t b = (sb * a + db * (255 - a)) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void fill_white(uint32_t *buf, int w, int y0, int y1, uint32_t bg)
{
    for (int y = y0; y < y1; y++) { uint32_t *row = buf + (size_t)y * w; for (int x = 0; x < w; x++) row[x] = bg; }
}

/* draw one glyph (R8 coverage * colour) into buf, clipped to [clip_y0,clip_y1) */
static void blit_glyph(dp_engine *e, uint32_t *buf, const df_bitmap *bm,
                       float gx, float baseline, uint32_t color, int clip_y0, int clip_y1)
{
    int ox = ri(gx) + bm->left, oy = ri(baseline) - bm->top;
    uint8_t sr = (uint8_t)(color >> 16), sg = (uint8_t)(color >> 8), sb = (uint8_t)color;
    uint32_t ca = (color >> 24) & 0xff;
    for (int r = 0; r < bm->h; r++) {
        int dy = oy + r;
        if (dy < clip_y0 || dy >= clip_y1 || dy < 0 || dy >= e->page_h) continue;
        const uint8_t *crow = bm->cover + (size_t)r * bm->w;
        uint32_t *drow = buf + (size_t)dy * e->page_w;
        for (int c = 0; c < bm->w; c++) {
            uint8_t cov = crow[c]; if (!cov) continue;
            int dx = ox + c; if (dx < 0 || dx >= e->page_w) continue;
            uint32_t a = (ca * cov + 127) / 255; if (!a) continue;
            drow[dx] = blend_argb(drow[dx], sr, sg, sb, a);
        }
    }
}

/* solid horizontal line (underline / strike) in colour, clipped to [clip_y0,clip_y1) */
static void draw_hline(dp_engine *e, uint32_t *buf, float x0, float x1, int y, int thick,
                       uint32_t color, int clip_y0, int clip_y1)
{
    int ix0 = ri(x0), ix1 = ri(x1);
    if (ix0 < 0) ix0 = 0; if (ix1 > e->page_w) ix1 = e->page_w;
    uint8_t sr = (uint8_t)(color >> 16), sg = (uint8_t)(color >> 8), sb = (uint8_t)color;
    uint32_t a = (color >> 24) & 0xff;
    for (int yy = y; yy < y + thick; yy++) {
        if (yy < clip_y0 || yy >= clip_y1 || yy < 0 || yy >= e->page_h) continue;
        uint32_t *row = buf + (size_t)yy * e->page_w;
        for (int x = ix0; x < ix1; x++) row[x] = blend_argb(row[x], sr, sg, sb, a);
    }
}

/* solid filled rect (text highlight) in colour, clipped to [clip_y0,clip_y1) */
static void fill_rect(dp_engine *e, uint32_t *buf, float fx0, float fx1, int y0, int y1,
                      uint32_t color, int clip_y0, int clip_y1)
{
    uint32_t a = (color >> 24) & 0xff; if (!a) return;
    int ix0 = ri(fx0), ix1 = ri(fx1);
    if (ix0 < 0) ix0 = 0; if (ix1 > e->page_w) ix1 = e->page_w;
    uint8_t sr = (uint8_t)(color >> 16), sg = (uint8_t)(color >> 8), sb = (uint8_t)color;
    for (int yy = y0; yy < y1; yy++) {
        if (yy < clip_y0 || yy >= clip_y1 || yy < 0 || yy >= e->page_h) continue;
        uint32_t *row = buf + (size_t)yy * e->page_w;
        for (int x = ix0; x < ix1; x++) row[x] = blend_argb(row[x], sr, sg, sb, a);
    }
}

/* render page `p` content into `buf`, repainting only surface rows [sy0,sy1). */
static void render_region(dp_engine *e, uint32_t *buf, int p, int sy0, int sy1)
{
    const pinfo *pg = &e->pages[p];
    e->page_w = pg->page_w; e->page_h = pg->page_h;          /* scratch for blit helpers */
    e->content_top = pg->content_top; e->page_bg = pg->page_bg;
    if (sy0 < 0) sy0 = 0; if (sy1 > e->page_h) sy1 = e->page_h; if (sy1 <= sy0) return;
    fill_white(buf, e->page_w, sy0, sy1, e->page_bg);

    float top = pg->c_top;
    float bot = (p + 1 < (int)e->npages) ? e->pages[p + 1].c_top : e->content_height;
    bool cols = df_layout_columns(e->L) > 1;        /* column flow breaks y-monotonicity */
    uint32_t npar = df_layout_para_count(e->L);
    for (uint32_t pi = 0; pi < npar; pi++) {
        const dl_para *pp = df_layout_para(e->L, pi);
        /* The paragraph-level y window is a fast cull that assumes paragraphs
         * stack monotonically downward.  Column flow re-threads each line into
         * a (page,column) cell and leaves pp->y as the now-meaningless single-
         * column position, so this cull would skip/stop at column-1 paragraphs
         * (the bug where the right column rendered blank).  Gate it on single-
         * column layout; otherwise the per-line y test below does the work. */
        if (!cols) {
            if (pp->y + pp->height <= top) continue;
            if (pp->y >= bot) break;
        }
        for (uint32_t li = 0; li < pp->line_count; li++) {
            const dl_line *ln = &pp->lines[li];
            if (ln->y + ln->height <= top || ln->y >= bot) continue;
            float baseline = e->content_top + (ln->y - top) + ln->ascent;
            int line_top = ri(e->content_top + (ln->y - top));
            if (line_top >= sy1) { if (cols) continue; else break; }  /* y order monotone only w/o columns */
            if (line_top + ri(ln->height) < sy0) continue;
            int uline_y  = ri(baseline) + (int)(ln->ascent * 0.12f) + 1;
            int strike_y = ri(baseline) - (int)(ln->ascent * 0.30f);
            int lthick = (int)(ln->ascent / 12.0f); if (lthick < 1) lthick = 1;
            for (uint32_t g = 0; g < ln->glyph_count; g++) {
                const dl_glyph *gl = &ln->glyphs[g];
                if ((gl->highlight >> 24) != 0)
                    fill_rect(e, buf, gl->x, gl->x + gl->advance, line_top, ri(baseline + ln->descent), gl->highlight, sy0, sy1);
                if (gl->underline) draw_hline(e, buf, gl->x, gl->x + gl->advance, uline_y, lthick, gl->color, sy0, sy1);
                if (gl->strike)    draw_hline(e, buf, gl->x, gl->x + gl->advance, strike_y, lthick, gl->color, sy0, sy1);
                if (!gl->face) continue;
                df_bitmap *bm = cache_get(&e->cache, gl->face, gl->gid, gl->px_size, gl->embolden, gl->slant);
                if (!bm || !bm->cover || bm->w <= 0 || bm->h <= 0) continue;
                blit_glyph(e, buf, bm, gl->x, baseline, gl->color, sy0, sy1);
            }
        }
    }
}

static void render_full(dp_engine *e, uint32_t *buf, int p) { render_region(e, buf, p, 0, e->pages[p].page_h); }

/* ---- pagination ---- */

static bool pages_reserve(dp_engine *e, uint32_t need)
{
    if (need <= e->pages_cap) return true;
    uint32_t nc = e->pages_cap ? e->pages_cap * 2 : 16; while (nc < need) nc *= 2;
    pinfo *n = (pinfo *)realloc(e->pages, (size_t)nc * sizeof(pinfo));
    if (!n) return false; e->pages = n; e->pages_cap = nc; return true;
}

/* Walk the layout's paragraphs into pages.  A section change forces a fresh
 * page (Word "Next Page" breaks); within a section, pages fill to that
 * section's printable height.  Each page records its section's paper size,
 * margins, background and cumulative desk position. */
static void recompute_pagination(dp_engine *e)
{
    e->content_height = df_layout_content_height(e->L);
    e->npages = 0; e->max_pw = 1; e->max_ph = 1;

    uint32_t npar = df_layout_para_count(e->L);

    /* Multi-column (single-section): flow_columns folded the lines into uniform
     * vertical bands of height content_h, columns sharing each band (distinguished
     * only by x). Paginate as uniform content_h bands -- the line-overflow walk
     * below assumes a single monotonic column and would mis-page columns. */
    if (df_layout_section_count(e->L) <= 1 && df_layout_columns(e->L) > 1) {
        float pw, ph, ctop, cleft, cw, ch;
        df_layout_section_px(e->L, 0, &pw, &ph, &ctop, &cleft, &cw, &ch); (void)cw;
        uint32_t bg = df_layout_section_bg(e->L, 0);
        int ipw = ri(pw) < 1 ? 1 : ri(pw), iph = ri(ph) < 1 ? 1 : ri(ph);
        float band = ch < 1.0f ? 1.0f : ch;
        uint32_t np = (uint32_t)(e->content_height / band);
        if ((float)np * band < e->content_height - 0.01f) np++;
        if (np < 1) np = 1;
        if (!pages_reserve(e, np)) return;
        for (uint32_t p = 0; p < np; p++) {
            pinfo *pg = &e->pages[p];
            pg->c_top = (float)p * band;
            pg->page_w = ipw; pg->page_h = iph;
            pg->content_top = ctop; pg->content_left = cleft;
            pg->content_hprint = band; pg->page_bg = bg;
        }
        e->npages = np; e->max_pw = ipw; e->max_ph = iph;
        int dy = TOPPAD;
        for (uint32_t p = 0; p < np; p++) { e->pages[p].desk_y = dy; dy += iph + GAP; }
        return;
    }

    if (npar == 0) {                                   /* empty doc: one section-0 page */
        if (!pages_reserve(e, 1)) return;
        float pw, ph, ctop, cleft, cw, ch;
        df_layout_section_px(e->L, 0, &pw, &ph, &ctop, &cleft, &cw, &ch); (void)cw;
        pinfo *pg = &e->pages[0];
        pg->c_top = 0.0f;
        pg->page_w = ri(pw) < 1 ? 1 : ri(pw);
        pg->page_h = ri(ph) < 1 ? 1 : ri(ph);
        pg->content_top = ctop; pg->content_left = cleft;
        pg->content_hprint = ch < 1.0f ? 1.0f : ch;
        pg->page_bg = df_layout_section_bg(e->L, 0);
        pg->desk_y = TOPPAD;
        e->npages = 1; e->max_pw = pg->page_w; e->max_ph = pg->page_h;
        return;
    }

    uint32_t pages = 0;
    int first_on_page = 0;
    uint32_t cur_sec = 0;
    float cur_top = 0.0f, cur_hprint = 1.0f;

    for (uint32_t pi = 0; pi < npar; pi++) {
        const dl_para *pp = df_layout_para(e->L, pi);
        uint32_t sec = df_layout_para_section(e->L, pi);

        if (pages == 0 || sec != cur_sec) {            /* first page, or forced section break */
            float pw, ph, ctop, cleft, cw, ch;
            df_layout_section_px(e->L, sec, &pw, &ph, &ctop, &cleft, &cw, &ch); (void)cw;
            if (!pages_reserve(e, pages + 1)) return;
            pinfo *pg = &e->pages[pages];
            pg->c_top = pp->y;
            pg->page_w = ri(pw) < 1 ? 1 : ri(pw);
            pg->page_h = ri(ph) < 1 ? 1 : ri(ph);
            pg->content_top = ctop; pg->content_left = cleft;
            pg->content_hprint = ch < 1.0f ? 1.0f : ch;
            pg->page_bg = df_layout_section_bg(e->L, sec);
            if (pg->page_w > e->max_pw) e->max_pw = pg->page_w;
            if (pg->page_h > e->max_ph) e->max_ph = pg->page_h;
            cur_sec = sec; cur_top = pp->y; cur_hprint = pg->content_hprint;
            pages++; first_on_page = 1;
        }

        for (uint32_t li = 0; li < pp->line_count; li++) {
            const dl_line *ln = &pp->lines[li];
            if (!first_on_page && (ln->y + ln->height) > cur_top + cur_hprint) {
                if (!pages_reserve(e, pages + 1)) return;
                e->pages[pages] = e->pages[pages - 1];   /* continuation: same section */
                e->pages[pages].c_top = ln->y;
                cur_top = ln->y;
                pages++; first_on_page = 1;
            }
            first_on_page = 0;
        }
    }
    e->npages = pages;

    int dy = TOPPAD;                                   /* cumulative desk layout */
    for (uint32_t p = 0; p < pages; p++) { e->pages[p].desk_y = dy; dy += e->pages[p].page_h + GAP; }
}

static int page_at_content_y(const dp_engine *e, float cy)
{
    if (e->npages == 0) return 0;
    for (uint32_t p = 0; p < e->npages; p++) {
        float bot = (p + 1 < e->npages) ? e->pages[p + 1].c_top : e->content_height;
        if (cy < bot) return (int)p;
    }
    return (int)e->npages - 1;
}

/* ---- pool ---- */

static void pool_free_buffers(dp_engine *e)
{
    for (int i = 0; i < POOL; i++) { free(e->slots[i].buf); e->slots[i].buf = NULL; e->slots[i].page = -1; e->slots[i].valid = false; }
}

static void ensure_pool_size(dp_engine *e)
{
    if (e->pool_pw == e->max_pw && e->pool_ph == e->max_ph) return;
    pool_free_buffers(e);
    e->pool_pw = e->max_pw; e->pool_ph = e->max_ph;
}

static int desk_page_top(const dp_engine *e, int p)
{
    return (p >= 0 && (uint32_t)p < e->npages) ? e->pages[p].desk_y : TOPPAD;
}
static int win_x_p(const dp_engine *e, int p)
{
    int pw = (p >= 0 && (uint32_t)p < e->npages) ? e->pages[p].page_w : e->max_pw;
    int x = (e->win_w - pw) / 2; if (x < 0) x = 0;
    return x - e->scroll_x;
}

static bool page_visible(const dp_engine *e, int p)
{
    int ph = (p >= 0 && (uint32_t)p < e->npages) ? e->pages[p].page_h : e->max_ph;
    int wy = desk_page_top(e, p) - e->scroll;
    return (wy + ph > 0) && (wy < e->win_h);
}

/* find or assign a slot for page p (allocating/evicting as needed) */
static slot *slot_for(dp_engine *e, int p)
{
    for (int i = 0; i < POOL; i++) if (e->slots[i].page == p && e->slots[i].buf) return &e->slots[i];

    int victim = -1; uint32_t best = 0xFFFFFFFFu;
    for (int i = 0; i < POOL; i++) {
        if (!e->slots[i].buf || e->slots[i].page < 0) { victim = i; break; }
        if (page_visible(e, e->slots[i].page)) continue;        /* never evict a visible sheet */
        if (e->slots[i].lru < best) { best = e->slots[i].lru; victim = i; }
    }
    if (victim < 0) victim = 0;
    slot *s = &e->slots[victim];
    if (!s->buf) s->buf = (uint32_t *)malloc((size_t)e->max_pw * e->max_ph * sizeof(uint32_t));
    s->page = p; s->valid = false;
    return s;
}

/* ---- public ---- */

dp_engine *dp_create(const df_layout *L)
{
    dp_engine *e = (dp_engine *)calloc(1, sizeof(dp_engine));
    if (!e) return NULL;
    e->L = L;
    e->page_bg = 0xFFFFFFFFu;
    for (int i = 0; i < POOL; i++) e->slots[i].page = -1;
    dp_relayout(e);
    return e;
}

void dp_destroy(dp_engine *e)
{
    if (!e) return;
    pool_free_buffers(e);
    cache_free(&e->cache);
    free(e->pages);
    free(e);
}

void dp_relayout(dp_engine *e)
{
    recompute_pagination(e);          /* builds pages[] (geometry) and max_pw/ph */
    ensure_pool_size(e);              /* (re)allocates buffers to the largest page */
    /* content height may have shrunk (smaller page / fewer pages); keep the
     * scroll within the new range so the sheet stays on screen. */
    dp_set_scroll(e, e->scroll);
    dp_set_scroll_x(e, e->scroll_x);
    for (int i = 0; i < POOL; i++) e->slots[i].valid = false;
}

void dp_notify_edit(dp_engine *e, const dl_update *u)
{
    recompute_pagination(e);
    ensure_pool_size(e);              /* a section edit can change paper sizes */
    if (!u) { for (int i = 0; i < POOL; i++) e->slots[i].valid = false; return; }

    /* Invalidate the edited sheet and every sheet below it (their content
     * shifted / re-paginated). Only the VISIBLE invalidated sheets are
     * actually re-rendered, lazily, in dp_visible_pages -- so cost stays
     * O(visible pages). A correct full sheet re-render beats a fragile
     * partial-strip update. */
    int first = page_at_content_y(e, u->dirty_y0);
    for (int i = 0; i < POOL; i++)
        if (e->slots[i].page >= first) e->slots[i].valid = false;
}

void dp_set_page_bg(dp_engine *e, uint32_t color)
{
    e->page_bg = color;                                /* global default */
    for (uint32_t p = 0; p < e->npages; p++) e->pages[p].page_bg = color;
    for (int i = 0; i < POOL; i++) e->slots[i].valid = false;
}

void dp_set_viewport(dp_engine *e, int win_w, int win_h) { e->win_w = win_w; e->win_h = win_h; dp_set_scroll_x(e, e->scroll_x); }

int dp_scroll_max(const dp_engine *e)
{
    int desk = TOPPAD;
    if (e->npages > 0) desk = e->pages[e->npages - 1].desk_y + e->pages[e->npages - 1].page_h + TOPPAD;
    int m = desk - e->win_h; return m < 0 ? 0 : m;
}
void dp_set_scroll(dp_engine *e, int scroll) { int m = dp_scroll_max(e); if (scroll < 0) scroll = 0; if (scroll > m) scroll = m; e->scroll = scroll; }
void dp_scroll_by(dp_engine *e, int dy) { dp_set_scroll(e, e->scroll + dy); }
int  dp_scroll(const dp_engine *e) { return e->scroll; }

void dp_scroll_to_content(dp_engine *e, float cy_top, float cy_bottom)
{
    int p = page_at_content_y(e, cy_top);
    int wtop = desk_page_top(e, p) + ri(e->pages[p].content_top + (cy_top    - e->pages[p].c_top));
    int wbot = desk_page_top(e, p) + ri(e->pages[p].content_top + (cy_bottom - e->pages[p].c_top));
    if (wtop < e->scroll) dp_set_scroll(e, wtop);
    else if (wbot > e->scroll + e->win_h) dp_set_scroll(e, wbot - e->win_h);
}

/* ---- horizontal pan (needed when a page is wider than the viewport, e.g. zoomed in) ---- */
int dp_scroll_x_max(const dp_engine *e) { int m = e->max_pw - e->win_w; return m < 0 ? 0 : m; }
void dp_set_scroll_x(dp_engine *e, int sx) { int m = dp_scroll_x_max(e); if (sx < 0) sx = 0; if (sx > m) sx = m; e->scroll_x = sx; }
void dp_scroll_x_by(dp_engine *e, int dx) { dp_set_scroll_x(e, e->scroll_x + dx); }
int  dp_scroll_x(const dp_engine *e) { return e->scroll_x; }

/* pan horizontally so the content x-range [cx_left, cx_right] on the page at cy
 * is inside the viewport (no-op when the page fits: scroll_x is clamped to 0). */
void dp_scroll_to_content_x(dp_engine *e, float cx_left, float cx_right, float cy)
{
    int p = page_at_content_y(e, cy);
    int pw = (p >= 0 && (uint32_t)p < e->npages) ? e->pages[p].page_w : e->max_pw;
    int base = (e->win_w - pw) / 2; if (base < 0) base = 0;
    int margin = 12;
    int wl = base + ri(cx_left)  - e->scroll_x;
    int wr = base + ri(cx_right) - e->scroll_x;
    if (wl < margin)                 dp_set_scroll_x(e, e->scroll_x + (wl - margin));
    else if (wr > e->win_w - margin) dp_set_scroll_x(e, e->scroll_x + (wr - (e->win_w - margin)));
}

uint32_t dp_page_count(const dp_engine *e) { return e->npages; }
int dp_page_w(const dp_engine *e) { return e->max_pw; }
int dp_page_h(const dp_engine *e) { return e->max_ph; }

int dp_visible_pages(dp_engine *e, dp_view *out, int max)
{
    int count = 0;
    for (uint32_t p = 0; p < e->npages && count < max; p++) {
        if (!page_visible(e, (int)p)) continue;
        slot *s = slot_for(e, (int)p);
        if (!s->buf) continue;
        if (!s->valid) { render_full(e, s->buf, (int)p); s->valid = true; }
        s->lru = ++e->lru_clock;
        out[count].page_index = (int)p;
        out[count].buf = s->buf;
        out[count].w = e->pages[p].page_w; out[count].h = e->pages[p].page_h;
        out[count].win_x = win_x_p(e, (int)p);
        out[count].win_y = e->pages[p].desk_y - e->scroll;
        count++;
    }
    return count;
}

void dp_content_to_window(const dp_engine *e, float cx, float cy, int *wx, int *wy)
{
    int p = page_at_content_y(e, cy);
    if (wx) *wx = win_x_p(e, p) + ri(cx);
    if (wy) *wy = desk_page_top(e, p) - e->scroll + ri(e->pages[p].content_top + (cy - e->pages[p].c_top));
}

void dp_window_to_content(const dp_engine *e, int wx, int wy, float *cx, float *cy)
{
    int desk_y = wy + e->scroll;
    int p = 0; int best = 0x7FFFFFFF;
    for (uint32_t k = 0; k < e->npages; k++) {
        int top = desk_page_top(e, (int)k), bot = top + e->pages[k].page_h;
        int d = (desk_y < top) ? top - desk_y : (desk_y > bot ? desk_y - bot : 0);
        if (d < best) { best = d; p = (int)k; if (d == 0) break; }
    }
    float c_top = e->pages[p].c_top;
    float c_bot = (p + 1 < (int)e->npages) ? e->pages[p + 1].c_top : e->content_height;
    float yc = c_top + ((float)(desk_y - desk_page_top(e, p)) - e->pages[p].content_top);
    if (yc < c_top) yc = c_top;
    if (yc > c_bot) yc = c_bot;
    if (cx) *cx = (float)(wx - win_x_p(e, p));
    if (cy) *cy = yc;
}
