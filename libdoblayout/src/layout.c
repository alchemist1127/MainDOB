/* libdoblayout -- persistent, incremental, queryable layout engine.
 *
 * Content-space, per-paragraph-owned. Each paragraph holds its own line
 * and glyph arrays; a reflow re-shapes only the touched paragraph window
 * and translates everything below. No pagination here.
 */

#include <doblayout/doblayout.h>
#include <string.h>
#include <stdlib.h>

/* one paragraph object: public view + backing arrays it owns */
typedef struct
{
    dl_para   pub;                         /* exposed; pub.lines == lines */
    dl_line  *lines;  uint32_t cap_line;
    dl_glyph *glyphs; uint32_t nglyph, cap_glyph;
} para_t;

struct df_layout
{
    const df_doc     *doc;
    const df_fontset *fonts;
    float             dpi;
    float             page_w, page_h, content_left, content_top, content_w, content_h;

    uint32_t          col_count;           /* newspaper columns, >=1 */
    float             col_w, col_gap;       /* per-column width and gutter, px */

    para_t           *paras; uint32_t npara, cap_para;
    float             content_height;
};

/* shaped glyph, before line breaking */
typedef struct
{
    uint32_t gid; float advance;
    float    px, embolden, slant; uint32_t color, highlight; df_face *face;
    bool     space; uint32_t byte;
    uint8_t  underline, strike;
} shp;

static float t2px(float dpi, int32_t tw) { return (float)tw * dpi / 1440.0f; }

/* ---- utf8 + run resolution + shaping (shared with the old engine) ---- */

static uint32_t utf8_decode(const char *s, uint32_t len, uint32_t *i)
{
    uint8_t c = (uint8_t)s[*i]; uint32_t cp, n;
    if (c < 0x80)              { cp = c;        n = 1; }
    else if ((c >> 5) == 0x6)  { cp = c & 0x1f; n = 2; }
    else if ((c >> 4) == 0xE)  { cp = c & 0x0f; n = 3; }
    else if ((c >> 3) == 0x1E) { cp = c & 0x07; n = 4; }
    else { *i += 1; return c; }
    if (*i + n > len) { *i += 1; return c; }
    for (uint32_t k = 1; k < n; k++) {
        uint8_t cc = (uint8_t)s[*i + k];
        if ((cc & 0xC0) != 0x80) { *i += 1; return c; }
        cp = (cp << 6) | (cc & 0x3f);
    }
    *i += n; return cp;
}

static void resolve_run(df_layout *L, const CharFmt *cf, df_face **face, float *px, float *emb, float *sl)
{
    const char *fam = df_doc_family_name(L->doc, cf->family_id);
    df_query q; q.family = (fam && fam[0]) ? fam : NULL;
    q.weight = cf->bold ? 700 : 400; q.italic = cf->italic != 0; q.codepoint = 0;
    *px = (float)cf->size_twips * L->dpi / 1440.0f;
    df_resolved r;
    if (df_resolve(L->fonts, &q, &r) == DF_OK && r.face) { *face = r.face; *emb = r.embolden_em * (*px); *sl = r.slant; }
    else { *face = NULL; *emb = 0; *sl = 0; }
}

static shp *shape_para(df_layout *L, uint32_t pstart, uint32_t plen, uint32_t *out_n, bool *err)
{
    *out_n = 0;
    char *txt = (char *)malloc(plen ? plen : 1);
    if (!txt) { *err = true; return NULL; }
    df_doc_get_text(L->doc, pstart, plen, txt, plen);

    shp *arr = NULL; uint32_t n = 0, cap = 0, i = 0;
    while (i < plen) {
        uint32_t byte_off = pstart + i;
        uint32_t cp = utf8_decode(txt, plen, &i);
        if (cp == '\n' || cp == '\r') continue;

        CharFmt cf; df_doc_char_fmt_at_para(L->doc, pstart, byte_off, &cf);
        df_face *face; float px, emb, sl;
        resolve_run(L, &cf, &face, &px, &emb, &sl);

        uint32_t gid = face ? df_map_codepoint(face, cp) : 0;
        if (face && gid == 0) {
            const char *fam = df_doc_family_name(L->doc, cf.family_id);
            df_query q; q.family = (fam && fam[0]) ? fam : NULL;
            q.weight = cf.bold ? 700 : 400; q.italic = cf.italic != 0; q.codepoint = cp;
            df_resolved r;
            if (df_resolve(L->fonts, &q, &r) == DF_OK && r.face && r.gid) {
                face = r.face; gid = r.gid; emb = r.embolden_em * px; sl = r.slant;
            }
        }
        float adv = 0;
        if (face) { df_gmetrics gm; if (df_glyph_metrics(face, gid, px, &gm) == DF_OK) adv = gm.advance; }

        if (n == cap) {
            uint32_t nc = cap ? cap * 2 : 32;
            shp *na = (shp *)realloc(arr, (size_t)nc * sizeof(shp));
            if (!na) { free(arr); free(txt); *err = true; *out_n = 0; return NULL; }
            arr = na; cap = nc;
        }
        arr[n].gid = gid; arr[n].advance = adv; arr[n].px = px;
        arr[n].embolden = emb; arr[n].slant = sl; arr[n].color = cf.color; arr[n].highlight = cf.highlight;
        arr[n].face = face; arr[n].space = (cp == 0x20); arr[n].byte = byte_off;
        arr[n].underline = cf.underline; arr[n].strike = cf.strike;
        n++;
    }
    free(txt);
    *out_n = n;
    return arr;
}

/* ---- per-paragraph backing arrays ---- */

static bool pl_reserve(para_t *P, uint32_t need)
{
    if (need <= P->cap_line) return true;
    uint32_t nc = P->cap_line ? P->cap_line * 2 : 8; while (nc < need) nc *= 2;
    dl_line *n = (dl_line *)realloc(P->lines, (size_t)nc * sizeof(dl_line));
    if (!n) return false; P->lines = n; P->cap_line = nc; return true;
}
static bool pg_reserve(para_t *P, uint32_t need)
{
    if (need <= P->cap_glyph) return true;
    uint32_t nc = P->cap_glyph ? P->cap_glyph * 2 : 32; while (nc < need) nc *= 2;
    dl_glyph *n = (dl_glyph *)realloc(P->glyphs, (size_t)nc * sizeof(dl_glyph));
    if (!n) return false; P->glyphs = n; P->cap_glyph = nc; return true;
}

static void para_free(para_t *P) { free(P->lines); free(P->glyphs); memset(P, 0, sizeof(*P)); }

/* lay out paragraph `index` (doc bytes [pstart,pstart+plen)) starting at
 * content-y top_y, into P. Returns false on OOM. */
static bool build_para(df_layout *L, para_t *P, uint32_t index,
                       uint32_t pstart, uint32_t plen, float top_y,
                       float content_left, float wrap_w)
{
    /* reset arrays but keep capacity */
    P->nglyph = 0;
    uint32_t nline = 0;
    memset(&P->pub, 0, sizeof(P->pub));
    P->pub.index = index; P->pub.byte_start = pstart; P->pub.byte_len = plen; P->pub.y = top_y;

    ParaFmt pf; df_doc_para_fmt_resolved(L->doc, index, &pf);
    float indL = t2px(L->dpi, pf.indent_left);
    float indR = t2px(L->dpi, pf.indent_right);
    float indF = t2px(L->dpi, pf.indent_first);
    float sp_before = t2px(L->dpi, (int32_t)pf.space_before);
    float sp_after  = t2px(L->dpi, (int32_t)pf.space_after);
    float line_spacing = t2px(L->dpi, (int32_t)pf.line_spacing);

    float avail = wrap_w - indL - indR; if (avail < 1.0f) avail = wrap_w;

    bool err = false; uint32_t n = 0;
    shp *sv = shape_para(L, pstart, plen, &n, &err);
    if (err) { free(sv); return false; }

    float pen_y = top_y + sp_before;
    uint32_t i = 0; bool first_line = true;

    for (;;) {
        uint32_t start = i;
        float line_avail = avail - (first_line ? indF : 0.0f);
        if (line_avail < 1.0f) line_avail = avail;

        float width = 0; int last_break = -1; uint32_t j = i;
        for (; j < n; j++) {
            float w = sv[j].advance;
            if (width + w > line_avail && j > start && last_break >= 0) break;
            width += w;
            if (sv[j].space) last_break = (int)j;
        }
        uint32_t line_end;
        if (j < n && last_break >= 0) { line_end = (uint32_t)last_break + 1; i = line_end; }
        else                         { line_end = j; i = j; }

        uint32_t vis_end = line_end; while (vis_end > start && sv[vis_end - 1].space) vis_end--;
        float lw = 0, asc = 0, desc = 0, gap = 0; uint32_t ngaps = 0;
        for (uint32_t k = start; k < line_end; k++) {
            if (sv[k].face) {
                df_vmetrics vm;
                if (df_face_vmetrics(sv[k].face, sv[k].px, &vm) == DF_OK) {
                    if (vm.ascent > asc) asc = vm.ascent;
                    if (-vm.descent > desc) desc = -vm.descent;
                    if (vm.line_gap > gap) gap = vm.line_gap;
                }
            }
            if (k < vis_end) { lw += sv[k].advance; if (sv[k].space) ngaps++; }
        }
        if (line_end == start) {                 /* empty line/paragraph */
            CharFmt cf; df_doc_char_fmt_at(L->doc, pstart, &cf);
            df_face *f; float px, e, s; resolve_run(L, &cf, &f, &px, &e, &s);
            if (f) { df_vmetrics vm; if (df_face_vmetrics(f, px, &vm) == DF_OK) { asc = vm.ascent; desc = -vm.descent; gap = vm.line_gap; } }
        }
        float line_h = (line_spacing > 0.0f) ? line_spacing : (asc + desc + gap);
        if (line_h < 1.0f) line_h = (asc + desc > 0.0f) ? (asc + desc) : t2px(L->dpi, 240);

        bool last_line = (i >= n);
        float baseline = pen_y + asc;

        float ll = content_left + indL + (first_line ? indF : 0.0f);
        float la = line_avail;
        float x0; float extra_per_gap = 0;
        switch (pf.align) {
            case DD_ALIGN_CENTER: x0 = ll + (la - lw) * 0.5f; break;
            case DD_ALIGN_RIGHT:  x0 = content_left + wrap_w - indR - lw; break;
            case DD_ALIGN_JUSTIFY:x0 = ll; if (!last_line && ngaps > 0 && la > lw) extra_per_gap = (la - lw) / (float)ngaps; break;
            default:              x0 = ll; break;
        }

        uint32_t first_g = P->nglyph;
        float penx = x0;
        for (uint32_t k = start; k < line_end; k++) {
            float adv = sv[k].advance + ((extra_per_gap > 0 && sv[k].space) ? extra_per_gap : 0);
            if (!pg_reserve(P, P->nglyph + 1)) { free(sv); return false; }
            dl_glyph *g = &P->glyphs[P->nglyph++];
            g->gid = sv[k].gid; g->x = penx; g->advance = adv;
            g->px_size = sv[k].px; g->embolden = sv[k].embolden; g->slant = sv[k].slant;
            g->color = sv[k].color; g->highlight = sv[k].highlight; g->face = sv[k].face; g->byte = sv[k].byte;
            g->underline = sv[k].underline; g->strike = sv[k].strike;
            penx += adv;
        }

        if (!pl_reserve(P, nline + 1)) { free(sv); return false; }
        dl_line *ln = &P->lines[nline++];
        ln->byte_start = (start < n) ? sv[start].byte : pstart;
        ln->byte_len   = ((line_end < n) ? sv[line_end].byte : (pstart + plen)) - ln->byte_start;
        ln->x = x0; ln->y = pen_y; ln->width = lw;
        ln->ascent = asc; ln->descent = desc; ln->height = line_h;
        ln->glyphs = NULL; ln->glyph_count = P->nglyph - first_g;

        pen_y += line_h;
        first_line = false;
        if (i >= n) break;
    }
    free(sv);

    /* finalize glyph slices (arrays are stable now) */
    uint32_t off = 0;
    for (uint32_t k = 0; k < nline; k++) { P->lines[k].glyphs = P->glyphs + off; off += P->lines[k].glyph_count; }

    pen_y += sp_after;
    P->pub.height = pen_y - top_y;
    P->pub.lines = P->lines; P->pub.line_count = nline;
    return true;
}

/* ---- build / destroy ---- */

static bool grow_paras(df_layout *L, uint32_t need)
{
    if (need <= L->cap_para) return true;
    uint32_t nc = L->cap_para ? L->cap_para * 2 : 8; while (nc < need) nc *= 2;
    para_t *n = (para_t *)realloc(L->paras, (size_t)nc * sizeof(para_t));
    if (!n) return false;
    memset(n + L->cap_para, 0, (size_t)(nc - L->cap_para) * sizeof(para_t));
    L->paras = n; L->cap_para = nc; return true;
}

static void read_metrics(df_layout *L)
{
    PageSetup ps; df_doc_get_page(L->doc, &ps);
    L->page_w = t2px(L->dpi, (int32_t)ps.width);
    L->page_h = t2px(L->dpi, (int32_t)ps.height);
    L->content_left = t2px(L->dpi, (int32_t)ps.margin_left);
    L->content_top  = t2px(L->dpi, (int32_t)ps.margin_top);
    L->content_w = t2px(L->dpi, (int32_t)(ps.width  - ps.margin_left - ps.margin_right));
    L->content_h = t2px(L->dpi, (int32_t)(ps.height - ps.margin_top  - ps.margin_bottom));
    if (L->content_w < 1.0f) L->content_w = 1.0f;

    L->col_count = ps.columns ? ps.columns : 1;
    if (L->col_count > 6) L->col_count = 6;
    L->col_gap = t2px(L->dpi, (int32_t)ps.column_gap);
    if (L->col_count > 1) {
        float tot = (float)(L->col_count - 1) * L->col_gap;
        L->col_w = (L->content_w - tot) / (float)L->col_count;
        if (L->col_w < 1.0f) L->col_w = 1.0f;
    } else {
        L->col_w = L->content_w;
    }
}

/* Newspaper column flow.  build_para lays every line out as if in a single
 * column of width col_w (content_left-based).  Here we re-assign each line,
 * in document order, into a (page, column) cell.  Pages are uniform bands of
 * height content_h; the N columns of a page share that y-band, shifted in x
 * by col*(col_w+col_gap).  Glyphs store absolute x, so they shift too. */
static void flow_columns(df_layout *L)
{
    if (L->col_count <= 1) return;
    float ch = L->content_h; if (ch < 1.0f) return;
    float step = L->col_w + L->col_gap;
    uint32_t N = L->col_count, page = 0, col = 0;
    float band_top = 0.0f, y = 0.0f, max_y = 0.0f;
    for (uint32_t pi = 0; pi < L->npara; pi++) {
        para_t *P = &L->paras[pi];
        for (uint32_t li = 0; li < P->pub.line_count; li++) {
            dl_line *ln = &P->lines[li];
            float h = ln->height;
            if (y > band_top && (y + h) > band_top + ch) {
                if (col + 1 < N) col++;
                else { page++; col = 0; band_top = (float)page * ch; }
                y = band_top;
            }
            float dx = (float)col * step;
            ln->x += dx;
            dl_glyph *gs = (dl_glyph *)ln->glyphs;
            for (uint32_t g = 0; g < ln->glyph_count; g++) gs[g].x += dx;
            ln->y = y;
            y += h;
            if (y > max_y) max_y = y;
        }
    }
    L->content_height = max_y;
}

static void layout_all(df_layout *L)
{
    for (uint32_t i = 0; i < L->npara; i++) para_free(&L->paras[i]);
    L->npara = 0;

    read_metrics(L);
    bool multi = (df_doc_section_count(L->doc) > 1);
    uint32_t paras = df_doc_para_count(L->doc);
    if (!grow_paras(L, paras ? paras : 1)) return;

    float y = 0;
    for (uint32_t pi = 0; pi < paras; pi++) {
        uint32_t s, len; df_doc_para_range(L->doc, pi, &s, &len);
        float cl, ww;
        if (multi) {                                   /* per-section margins (single column) */
            PageSetup sp; df_doc_section_page_at(L->doc, s, &sp);
            cl = t2px(L->dpi, (int32_t)sp.margin_left);
            ww = t2px(L->dpi, (int32_t)(sp.width - sp.margin_left - sp.margin_right));
            if (ww < 1.0f) ww = 1.0f;
        } else {                                       /* preserve single-section + columns path */
            cl = L->content_left; ww = L->col_w;
        }
        if (!build_para(L, &L->paras[pi], pi, s, len, y, cl, ww)) break;
        y += L->paras[pi].pub.height;
        L->npara = pi + 1;
    }
    L->content_height = y;
    if (!multi) flow_columns(L);                       /* per-section columns handled later */
}

df_layout *df_layout_create(const df_doc *doc, const df_fontset *fonts, const dl_opts *opts)
{
    df_layout *L = (df_layout *)calloc(1, sizeof(df_layout));
    if (!L) return NULL;
    L->doc = doc; L->fonts = fonts;
    L->dpi = (opts && opts->dpi > 0) ? opts->dpi : 96.0f;
    layout_all(L);
    return L;
}

void df_layout_destroy(df_layout *L)
{
    if (!L) return;
    for (uint32_t i = 0; i < L->npara; i++) para_free(&L->paras[i]);
    free(L->paras); free(L);
}

void df_layout_rebuild(df_layout *L) { if (L) layout_all(L); }

/* ---- incremental reflow ---- */

static uint32_t para_index_at(const df_layout *L, uint32_t pos)
{
    uint32_t a = 0;
    while (a + 1 < L->npara && L->paras[a + 1].pub.byte_start <= pos) a++;
    return a;
}

void df_layout_reflow(df_layout *L, uint32_t pos, uint32_t old_len, uint32_t new_len, dl_update *out)
{
    dl_update u; memset(&u, 0, sizeof u);
    if (!L || L->npara == 0) { layout_all(L); if (out) *out = u; return; }

    if (L->col_count > 1 || df_doc_section_count(L->doc) > 1) {
        /* the incremental splice assumes single-column, single-section vertical
         * stacking; column flow and per-section geometry re-thread everything,
         * so just rebuild. */
        layout_all(L);
        if (out) { u.para_count_new = L->npara; u.dirty_y1 = L->content_height; *out = u; }
        return;
    }

    int delta = (int)new_len - (int)old_len;

    /* old paragraph window touched by [pos, pos+old_len] */
    uint32_t a = para_index_at(L, pos);
    uint32_t b = para_index_at(L, pos + old_len);
    if (b < a) b = a;

    /* corresponding new-doc paragraph window */
    uint32_t Pn = df_doc_para_count(L->doc);
    uint32_t paraA_start = L->paras[a].pub.byte_start;
    long end_old = (long)L->paras[b].pub.byte_start + (long)L->paras[b].pub.byte_len;
    long end_new = end_old + delta;
    uint32_t doc_len = df_doc_length(L->doc);
    if (end_new < 0) end_new = 0;
    if (end_new > (long)doc_len) end_new = (long)doc_len;

    uint32_t na = df_doc_para_at(L->doc, paraA_start);
    uint32_t nb = df_doc_para_at(L->doc, end_new > 0 ? (uint32_t)(end_new - 1) : 0);

    /* fall back to full rebuild if the window looks degenerate */
    if (Pn == 0 || na >= Pn || nb >= Pn || nb < na || a >= L->npara || b >= L->npara) {
        layout_all(L);
        if (out) { u.para_count_new = L->npara; u.dirty_y1 = L->content_height; *out = u; }
        return;
    }

    uint32_t oldcount = b - a + 1;
    uint32_t newcount = nb - na + 1;

    float win_top = L->paras[a].pub.y;
    float old_win_h = (L->paras[b].pub.y + L->paras[b].pub.height) - win_top;

    /* build replacement paragraphs into a temp array */
    para_t *tmp = (para_t *)calloc(newcount, sizeof(para_t));
    if (!tmp) { layout_all(L); if (out) { u.dirty_y1 = L->content_height; *out = u; } return; }
    float y = win_top; bool ok = true;
    for (uint32_t k = 0; k < newcount && ok; k++) {
        uint32_t s, len; df_doc_para_range(L->doc, na + k, &s, &len);
        ok = build_para(L, &tmp[k], na + k, s, len, y, L->content_left, L->col_w);
        y += tmp[k].pub.height;
    }
    if (!ok) { for (uint32_t k = 0; k < newcount; k++) para_free(&tmp[k]); free(tmp);
               layout_all(L); if (out) { u.dirty_y1 = L->content_height; *out = u; } return; }
    float new_win_h = y - win_top;
    float yshift = new_win_h - old_win_h;

    /* splice: paras[a..b] (oldcount) -> tmp (newcount) */
    for (uint32_t k = a; k <= b; k++) para_free(&L->paras[k]);

    uint32_t tail = L->npara - (b + 1);          /* paragraphs after the window */
    uint32_t old_npara = L->npara;               /* count before the splice */
    uint32_t new_npara = L->npara - oldcount + newcount;
    if (!grow_paras(L, new_npara)) { free(tmp); layout_all(L); if (out) { u.dirty_y1 = L->content_height; *out = u; } return; }

    /* move the tail to its new position (memmove of the structs; heap arrays they
     * point to do not move, so their line/glyph pointers stay valid) */
    if (tail) memmove(&L->paras[a + newcount], &L->paras[b + 1], (size_t)tail * sizeof(para_t));
    /* drop in the rebuilt window (transfer ownership of tmp's arrays) */
    memcpy(&L->paras[a], tmp, (size_t)newcount * sizeof(para_t));
    free(tmp);
    L->npara = new_npara;

    /* A shrinking splice leaves the slots above the new count holding stale
     * COPIES of the moved tail's structs -- their glyph/line pointers now alias
     * live buffers. Zero them so a later rebuild never reuses (and frees, then
     * writes through) an aliased buffer. Keeps "slots >= npara are cleared". */
    if (new_npara < old_npara)
        memset(&L->paras[new_npara], 0, (size_t)(old_npara - new_npara) * sizeof(para_t));

    /* fix up the shifted tail: reindex, shift byte offsets and y */
    int idx_delta = (int)newcount - (int)oldcount;
    for (uint32_t k = a + newcount; k < L->npara; k++) {
        para_t *P = &L->paras[k];
        P->pub.index = (uint32_t)((int)P->pub.index + idx_delta);
        P->pub.byte_start = (uint32_t)((long)P->pub.byte_start + delta);
        P->pub.y += yshift;
        for (uint32_t li = 0; li < P->pub.line_count; li++) {
            P->lines[li].byte_start = (uint32_t)((long)P->lines[li].byte_start + delta);
            P->lines[li].y += yshift;
        }
    }
    L->content_height += yshift;

    u.first_para = a; u.para_count_old = oldcount; u.para_count_new = newcount;
    u.height_delta = yshift; u.dirty_y0 = win_top; u.dirty_y1 = win_top + new_win_h;
    if (out) *out = u;
}

/* ---- queries ---- */

/* ---- sections (pixel geometry for the page engine) ---- */
uint32_t df_layout_section_count(const df_layout *L) { return L ? df_doc_section_count(L->doc) : 1; }

uint32_t df_layout_para_section(const df_layout *L, uint32_t para_index)
{
    if (!L || para_index >= L->npara) return 0;
    return df_doc_section_at(L->doc, L->paras[para_index].pub.byte_start);
}

void df_layout_section_px(const df_layout *L, uint32_t ordinal,
                          float *page_w, float *page_h, float *content_top,
                          float *content_left, float *content_w, float *content_h)
{
    PageSetup ps;
    if (!L || df_doc_section_page(L->doc, ordinal, &ps) != DD_OK) { if (L) df_doc_get_page(L->doc, &ps); else memset(&ps, 0, sizeof ps); }
    float d = L ? L->dpi : 96.0f;
    if (page_w)       *page_w       = t2px(d, (int32_t)ps.width);
    if (page_h)       *page_h       = t2px(d, (int32_t)ps.height);
    if (content_top)  *content_top  = t2px(d, (int32_t)ps.margin_top);
    if (content_left) *content_left = t2px(d, (int32_t)ps.margin_left);
    if (content_w) { float w = t2px(d, (int32_t)(ps.width  - ps.margin_left - ps.margin_right)); *content_w = w < 1.0f ? 1.0f : w; }
    if (content_h) { float h = t2px(d, (int32_t)(ps.height - ps.margin_top  - ps.margin_bottom)); *content_h = h < 1.0f ? 1.0f : h; }
}

uint32_t df_layout_section_bg(const df_layout *L, uint32_t ordinal)
{
    PageSetup ps;
    if (!L || df_doc_section_page(L->doc, ordinal, &ps) != DD_OK) return 0xFFFFFFFFu;
    return ps.bg_color ? ps.bg_color : 0xFFFFFFFFu;
}

uint32_t       df_layout_para_count(const df_layout *L) { return L->npara; }
const dl_para *df_layout_para(const df_layout *L, uint32_t i) { return i < L->npara ? &L->paras[i].pub : NULL; }
float          df_layout_content_height(const df_layout *L) { return L->content_height; }
float          df_layout_content_width(const df_layout *L) { return L->content_w; }
uint32_t       df_layout_columns(const df_layout *L) { return L ? L->col_count : 1; }

void df_layout_page_metrics(const df_layout *L, float *page_w, float *page_h,
                            float *margin_left, float *margin_top, float *content_w, float *content_h)
{
    if (page_w) *page_w = L->page_w; if (page_h) *page_h = L->page_h;
    if (margin_left) *margin_left = L->content_left; if (margin_top) *margin_top = L->content_top;
    if (content_w) *content_w = L->content_w; if (content_h) *content_h = L->content_h;
}

static uint32_t para_containing(const df_layout *L, uint32_t byte)
{
    uint32_t a = 0;
    while (a + 1 < L->npara && L->paras[a + 1].pub.byte_start <= byte) a++;
    return a;
}

bool df_layout_locate(const df_layout *L, uint32_t byte, dl_locus *out)
{
    if (!L || L->npara == 0) return false;
    uint32_t pi = para_containing(L, byte);
    const para_t *P = &L->paras[pi];

    uint32_t li = 0;
    for (uint32_t k = 0; k < P->pub.line_count; k++) {
        const dl_line *ln = &P->lines[k];
        if (byte < ln->byte_start + ln->byte_len || k + 1 == P->pub.line_count) { li = k; break; }
        li = k;
    }
    const dl_line *ln = &P->lines[li];

    float x = ln->x;
    for (uint32_t g = 0; g < ln->glyph_count; g++) {
        if (ln->glyphs[g].byte >= byte) { x = ln->glyphs[g].x; goto done; }
        x = ln->glyphs[g].x + ln->glyphs[g].advance;
    }
done:
    out->para = pi; out->line = li; out->x = x;
    out->y_top = ln->y; out->y_bottom = ln->y + ln->height;
    return true;
}

uint32_t df_layout_hit(const df_layout *L, float cx, float cy)
{
    if (!L || L->npara == 0) return 0;

    if (L->col_count > 1) {
        /* columns: lines at one y-band sit in different x columns, so the
         * nearest line must be found in 2D (y dominates, x breaks ties). */
        const dl_line *bestL = NULL; float bestd = 1e30f;
        for (uint32_t pk = 0; pk < L->npara; pk++) {
            const para_t *P = &L->paras[pk];
            for (uint32_t lk = 0; lk < P->pub.line_count; lk++) {
                const dl_line *ln = &P->lines[lk];
                float dy = (cy < ln->y) ? (ln->y - cy) : (cy > ln->y + ln->height ? cy - (ln->y + ln->height) : 0.0f);
                float x1 = ln->x + ln->width;
                float dx = (cx < ln->x) ? (ln->x - cx) : (cx > x1 ? cx - x1 : 0.0f);
                float d = dy * 4.0f + dx;
                if (d < bestd) { bestd = d; bestL = ln; }
            }
        }
        if (!bestL) return 0;
        for (uint32_t g = 0; g < bestL->glyph_count; g++)
            if (cx < bestL->glyphs[g].x + bestL->glyphs[g].advance * 0.5f) return bestL->glyphs[g].byte;
        return bestL->byte_start + bestL->byte_len;
    }

    /* nearest paragraph in y */
    uint32_t pi = 0; float best = 1e30f;
    for (uint32_t k = 0; k < L->npara; k++) {
        const dl_para *p = &L->paras[k].pub;
        float d = (cy < p->y) ? (p->y - cy) : (cy > p->y + p->height ? cy - (p->y + p->height) : 0.0f);
        if (d < best) { best = d; pi = k; if (d == 0) break; }
    }
    const para_t *P = &L->paras[pi];

    /* nearest line in y */
    uint32_t li = 0; best = 1e30f;
    for (uint32_t k = 0; k < P->pub.line_count; k++) {
        const dl_line *ln = &P->lines[k];
        float d = (cy < ln->y) ? (ln->y - cy) : (cy > ln->y + ln->height ? cy - (ln->y + ln->height) : 0.0f);
        if (d < best) { best = d; li = k; if (d == 0) break; }
    }
    const dl_line *ln = &P->lines[li];

    for (uint32_t g = 0; g < ln->glyph_count; g++) {
        if (cx < ln->glyphs[g].x + ln->glyphs[g].advance * 0.5f) return ln->glyphs[g].byte;
    }
    uint32_t e = ln->byte_start + ln->byte_len;
    return e;
}
