/* libdobdoc -- the document object: styles, editing, queries, undo. */

#include "doc_internal.h"
#include <string.h>
#include <stdlib.h>

#define ALL_CF (DD_CF_FAMILY | DD_CF_SIZE | DD_CF_BOLD | DD_CF_ITALIC | DD_CF_UNDERLINE | DD_CF_STRIKE | DD_CF_COLOR | DD_CF_HIGHLIGHT)
#define ALL_PF (DD_PF_ALIGN | DD_PF_INDENT_LEFT | DD_PF_INDENT_RIGHT | DD_PF_INDENT_FIRST | \
                DD_PF_SPACE_BEFORE | DD_PF_SPACE_AFTER | DD_PF_LINE_SPACING | DD_PF_STYLE)
#define UNDO_MAX 500

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* ---- format overlay (apply set fields of ov onto base) ---- */

static void overlay_char(CharFmt *b, const CharFmt *o)
{
    if (o->mask & DD_CF_FAMILY)    { b->family_id  = o->family_id;  b->mask |= DD_CF_FAMILY; }
    if (o->mask & DD_CF_SIZE)      { b->size_twips = o->size_twips; b->mask |= DD_CF_SIZE; }
    if (o->mask & DD_CF_BOLD)      { b->bold       = o->bold;       b->mask |= DD_CF_BOLD; }
    if (o->mask & DD_CF_ITALIC)    { b->italic     = o->italic;     b->mask |= DD_CF_ITALIC; }
    if (o->mask & DD_CF_UNDERLINE) { b->underline  = o->underline;  b->mask |= DD_CF_UNDERLINE; }
    if (o->mask & DD_CF_STRIKE)    { b->strike     = o->strike;     b->mask |= DD_CF_STRIKE; }
    if (o->mask & DD_CF_COLOR)     { b->color      = o->color;      b->mask |= DD_CF_COLOR; }
    if (o->mask & DD_CF_HIGHLIGHT)  { b->highlight  = o->highlight;  b->mask |= DD_CF_HIGHLIGHT; }
}

static void overlay_para(ParaFmt *b, const ParaFmt *o)
{
    if (o->mask & DD_PF_ALIGN)        { b->align        = o->align;        b->mask |= DD_PF_ALIGN; }
    if (o->mask & DD_PF_INDENT_LEFT)  { b->indent_left  = o->indent_left;  b->mask |= DD_PF_INDENT_LEFT; }
    if (o->mask & DD_PF_INDENT_RIGHT) { b->indent_right = o->indent_right; b->mask |= DD_PF_INDENT_RIGHT; }
    if (o->mask & DD_PF_INDENT_FIRST) { b->indent_first = o->indent_first; b->mask |= DD_PF_INDENT_FIRST; }
    if (o->mask & DD_PF_SPACE_BEFORE) { b->space_before = o->space_before; b->mask |= DD_PF_SPACE_BEFORE; }
    if (o->mask & DD_PF_SPACE_AFTER)  { b->space_after  = o->space_after;  b->mask |= DD_PF_SPACE_AFTER; }
    if (o->mask & DD_PF_LINE_SPACING) { b->line_spacing = o->line_spacing; b->mask |= DD_PF_LINE_SPACING; }
    if (o->mask & DD_PF_STYLE)        { b->style_id     = o->style_id;     b->mask |= DD_PF_STYLE; }
}

/* ---- style cascade ---- */

static void resolve_style_char(const df_doc *d, int style_id, CharFmt *out)
{
    *out = d->def_cf;                       /* root: document default (fully set) */
    int chain[16]; int depth = 0;
    int s = style_id;
    while (s >= 0 && s < d->nstyles && depth < 16) { chain[depth++] = s; s = d->styles[s].parent; }
    for (int i = depth - 1; i >= 0; i--) overlay_char(out, &d->styles[chain[i]].cf);
    out->mask = ALL_CF;
}

static void resolve_style_para(const df_doc *d, int style_id, ParaFmt *out)
{
    memset(out, 0, sizeof(*out));
    out->align = DD_ALIGN_LEFT; out->style_id = (int16_t)style_id;
    int chain[16]; int depth = 0;
    int s = style_id;
    while (s >= 0 && s < d->nstyles && depth < 16) { chain[depth++] = s; s = d->styles[s].parent; }
    for (int i = depth - 1; i >= 0; i--) overlay_para(out, &d->styles[chain[i]].pf);
    out->mask = ALL_PF;
}

/* ---- undo machinery ---- */

static void us_free(undo_state *us)
{
    free(us->pc); rl_free(&us->cruns); rl_free(&us->pruns);
    rl_free(&us->sruns); free(us->sections);
    memset(us, 0, sizeof(*us));
}

static dd_result snapshot(const df_doc *d, undo_state *us)
{
    memset(us, 0, sizeof(*us));
    if (pt_snapshot(&d->pt, &us->pc, &us->npc, &us->total)) return DD_ERR_NOMEM;
    if (rl_copy(&d->cruns, &us->cruns)) { us_free(us); return DD_ERR_NOMEM; }
    if (rl_copy(&d->pruns, &us->pruns)) { us_free(us); return DD_ERR_NOMEM; }
    if (rl_copy(&d->sruns, &us->sruns)) { us_free(us); return DD_ERR_NOMEM; }
    us->sections = (PageSetup *)malloc((size_t)d->nsections * sizeof(PageSetup));
    if (!us->sections) { us_free(us); return DD_ERR_NOMEM; }
    memcpy(us->sections, d->sections, (size_t)d->nsections * sizeof(PageSetup));
    us->nsections = d->nsections;
    return DD_OK;
}

static void apply_state(df_doc *d, undo_state *us)
{
    pt_restore(&d->pt, us->pc, us->npc, us->total);
    rl_free(&d->cruns); rl_copy(&us->cruns, &d->cruns);
    rl_free(&d->pruns); rl_copy(&us->pruns, &d->pruns);
    rl_free(&d->sruns); rl_copy(&us->sruns, &d->sruns);
    PageSetup *ns = (PageSetup *)malloc((size_t)us->nsections * sizeof(PageSetup));
    if (ns) {                                           /* keep old table on OOM */
        memcpy(ns, us->sections, (size_t)us->nsections * sizeof(PageSetup));
        free(d->sections);
        d->sections = ns; d->nsections = us->nsections; d->sections_cap = us->nsections;
    }
}

static dd_result stack_push(undo_state **stk, int *n, int *cap, const undo_state *us)
{
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 16;
        undo_state *ns = (undo_state *)realloc(*stk, (size_t)nc * sizeof(undo_state));
        if (!ns) return DD_ERR_NOMEM;
        *stk = ns; *cap = nc;
    }
    (*stk)[(*n)++] = *us;
    return DD_OK;
}

static dd_result begin_edit(df_doc *d)
{
    undo_state us;
    if (snapshot(d, &us)) return DD_ERR_NOMEM;
    if (stack_push(&d->ustack, &d->un, &d->ucap, &us)) { us_free(&us); return DD_ERR_NOMEM; }
    while (d->un > UNDO_MAX) {
        us_free(&d->ustack[0]);
        memmove(&d->ustack[0], &d->ustack[1], (size_t)(d->un - 1) * sizeof(undo_state));
        d->un--;
    }
    for (int i = 0; i < d->rn; i++) us_free(&d->rstack[i]);
    d->rn = 0;
    return DD_OK;
}

bool df_doc_can_undo(const df_doc *d) { return d->un > 0; }
bool df_doc_can_redo(const df_doc *d) { return d->rn > 0; }

dd_result df_doc_undo(df_doc *d)
{
    if (d->un == 0) return DD_ERR_RANGE;
    undo_state cur;
    if (snapshot(d, &cur)) return DD_ERR_NOMEM;
    if (stack_push(&d->rstack, &d->rn, &d->rcap, &cur)) { us_free(&cur); return DD_ERR_NOMEM; }
    undo_state us = d->ustack[--d->un];
    apply_state(d, &us);
    us_free(&us);
    return DD_OK;
}

dd_result df_doc_redo(df_doc *d)
{
    if (d->rn == 0) return DD_ERR_RANGE;
    undo_state cur;
    if (snapshot(d, &cur)) return DD_ERR_NOMEM;
    if (stack_push(&d->ustack, &d->un, &d->ucap, &cur)) { us_free(&cur); return DD_ERR_NOMEM; }
    undo_state us = d->rstack[--d->rn];
    apply_state(d, &us);
    us_free(&us);
    return DD_OK;
}

/* Append a page setup to the section table, returning its index (or -1). */
int sect_add(df_doc *d, const PageSetup *p)
{
    if (d->nsections == d->sections_cap) {
        int nc = d->sections_cap ? d->sections_cap * 2 : 4;
        PageSetup *np = (PageSetup *)realloc(d->sections, (size_t)nc * sizeof(PageSetup));
        if (!np) return -1;
        d->sections = np; d->sections_cap = nc;
    }
    d->sections[d->nsections] = *p;
    return d->nsections++;
}

/* ---- lifecycle ---- */

df_doc *df_doc_create(void)
{
    df_doc *d = (df_doc *)calloc(1, sizeof(df_doc));
    if (!d) return NULL;

    pt_init(&d->pt);
    rl_init(&d->cruns, sizeof(CharFmt));
    rl_init(&d->pruns, sizeof(ParaFmt));

    CharFmt zc; memset(&zc, 0, sizeof zc);     /* empty overrides for the initial run */
    ParaFmt zp; memset(&zp, 0, sizeof zp); zp.align = DD_ALIGN_LEFT; zp.style_id = -1;
    rl_reset_single(&d->cruns, 0, &zc);
    rl_reset_single(&d->pruns, 0, &zp);

    memset(&d->def_cf, 0, sizeof d->def_cf);
    d->def_cf.mask = ALL_CF;
    d->def_cf.family_id = 0;
    d->def_cf.size_twips = 220;                /* 11 pt */
    d->def_cf.color = 0xFF000000u;             /* opaque black */
    d->def_cf.highlight = 0;                   /* no highlight */

    /* One section spanning the whole (initially empty) document.  sections[0]
     * holds the A4 default; sruns is a single index-0 run. */
    rl_init(&d->sruns, sizeof(uint32_t));
    PageSetup defpage; memset(&defpage, 0, sizeof defpage);
    defpage.width = 11906; defpage.height = 16838;     /* A4 */
    defpage.margin_left = defpage.margin_right = 1440;
    defpage.margin_top  = defpage.margin_bottom = 1440;
    defpage.bg_color = 0xFFFFFFFFu;                     /* opaque white paper */
    defpage.columns = 1; defpage.column_gap = 720;      /* single column, 0.5in gutter */
    if (sect_add(d, &defpage) < 0) { df_doc_destroy(d); return NULL; }
    uint32_t zero_idx = 0;
    rl_reset_single(&d->sruns, 0, &zero_idx);

    /* family id 0 reserved = "" */
    d->fam = (char **)malloc(sizeof(char *));
    if (d->fam) { d->fam[0] = dup_str(""); d->nfam = 1; d->fam_cap = 1; }

    /* style 0 = "Normal" (inherits everything) */
    CharFmt nc; memset(&nc, 0, sizeof nc);
    ParaFmt np; memset(&np, 0, sizeof np); np.style_id = -1;
    df_doc_style_define(d, "Normal", -1, &nc, &np);

    return d;
}

void df_doc_destroy(df_doc *d)
{
    if (!d) return;
    pt_free(&d->pt);
    rl_free(&d->cruns); rl_free(&d->pruns); rl_free(&d->sruns);
    free(d->sections);
    for (int i = 0; i < d->un; i++) us_free(&d->ustack[i]);
    for (int i = 0; i < d->rn; i++) us_free(&d->rstack[i]);
    free(d->ustack); free(d->rstack);
    for (int i = 0; i < d->nfam; i++) free(d->fam[i]);
    free(d->fam);
    free(d->styles);
    free(d);
}

/* ---- family interning ---- */

uint16_t df_doc_family_intern(df_doc *d, const char *name)
{
    if (!name || !name[0]) return 0;
    for (int i = 0; i < d->nfam; i++)
        if (d->fam[i] && strcmp(d->fam[i], name) == 0) return (uint16_t)i;
    if (d->nfam == d->fam_cap) {
        int nc = d->fam_cap ? d->fam_cap * 2 : 8;
        char **nf = (char **)realloc(d->fam, (size_t)nc * sizeof(char *));
        if (!nf) return 0;
        d->fam = nf; d->fam_cap = nc;
    }
    d->fam[d->nfam] = dup_str(name);
    return (uint16_t)(d->nfam++);
}

const char *df_doc_family_name(const df_doc *d, uint16_t id)
{
    if (id < d->nfam && d->fam[id]) return d->fam[id];
    return "";
}

/* ---- defaults & page ---- */

void df_doc_get_default_char(const df_doc *d, CharFmt *out) { *out = d->def_cf; }
void df_doc_set_default_char(df_doc *d, const CharFmt *cf)  { d->def_cf = *cf; d->def_cf.mask = ALL_CF; }
void df_doc_get_page(const df_doc *d, PageSetup *out)
{
    const uint32_t *si = (const uint32_t *)rl_payload_at(&d->sruns, 0);
    *out = d->sections[*si];                              /* section 0 */
}
/* Whole-document set: apply the geometry to every section (boundaries kept). */
void df_doc_set_page(df_doc *d, const PageSetup *p)
{
    for (int i = 0; i < d->nsections; i++) d->sections[i] = *p;
}

uint32_t df_doc_section_count(const df_doc *d) { return d->sruns.n; }

uint32_t df_doc_section_at(const df_doc *d, uint32_t pos)
{
    uint32_t idx, within; rl_find(&d->sruns, pos, &idx, &within);
    if (idx >= d->sruns.n) idx = d->sruns.n ? d->sruns.n - 1 : 0;  /* pos==total -> last */
    return idx;
}

dd_result df_doc_section_range(const df_doc *d, uint32_t ordinal,
                               uint32_t *start, uint32_t *len)
{
    if (ordinal >= d->sruns.n) return DD_ERR_RANGE;
    uint32_t s = 0;
    for (uint32_t i = 0; i < ordinal; i++) s += d->sruns.len[i];
    if (start) *start = s;
    if (len)   *len   = d->sruns.len[ordinal];
    return DD_OK;
}

dd_result df_doc_section_page(const df_doc *d, uint32_t ordinal, PageSetup *out)
{
    if (ordinal >= d->sruns.n) return DD_ERR_RANGE;
    *out = d->sections[*(const uint32_t *)rl_payload(&d->sruns, ordinal)];
    return DD_OK;
}

void df_doc_section_page_at(const df_doc *d, uint32_t pos, PageSetup *out)
{
    const uint32_t *si = (const uint32_t *)rl_payload_at(&d->sruns, pos);
    *out = d->sections[*si];
}

/* ---- styles ---- */

int df_doc_style_define(df_doc *d, const char *name, int parent,
                        const CharFmt *cf, const ParaFmt *pf)
{
    if (d->nstyles == d->styles_cap) {
        int nc = d->styles_cap ? d->styles_cap * 2 : 8;
        Style *ns = (Style *)realloc(d->styles, (size_t)nc * sizeof(Style));
        if (!ns) return -1;
        d->styles = ns; d->styles_cap = nc;
    }
    Style *s = &d->styles[d->nstyles];
    memset(s, 0, sizeof(*s));
    if (name) { size_t k = 0; while (name[k] && k + 1 < sizeof(s->name)) { s->name[k] = name[k]; k++; } s->name[k] = '\0'; }
    s->parent = (int16_t)parent;
    if (cf) s->cf = *cf;
    if (pf) s->pf = *pf;
    return d->nstyles++;
}

int df_doc_style_count(const df_doc *d) { return d->nstyles; }

dd_result df_doc_style_get(const df_doc *d, int id, Style *out)
{
    if (id < 0 || id >= d->nstyles) return DD_ERR_RANGE;
    *out = d->styles[id];
    return DD_OK;
}

/* ---- paragraphs ---- */

uint32_t df_doc_para_count(const df_doc *d)
{
    uint32_t total = pt_total(&d->pt), nl = 0;
    for (uint32_t i = 0; i < total; i++) if (pt_byte_at(&d->pt, i) == '\n') nl++;
    return nl + 1;
}

uint32_t df_doc_para_at(const df_doc *d, uint32_t pos)
{
    uint32_t total = pt_total(&d->pt);
    if (pos > total) pos = total;
    uint32_t nl = 0;
    for (uint32_t i = 0; i < pos; i++) if (pt_byte_at(&d->pt, i) == '\n') nl++;
    return nl;
}

dd_result df_doc_para_range(const df_doc *d, uint32_t index, uint32_t *start, uint32_t *len)
{
    uint32_t total = pt_total(&d->pt);
    uint32_t cur = 0, s = 0;
    for (uint32_t p = 0; p < total; p++) {
        if (pt_byte_at(&d->pt, p) == '\n') {
            if (cur == index) { *start = s; *len = p + 1 - s; return DD_OK; }
            cur++; s = p + 1;
        }
    }
    if (cur == index) { *start = s; *len = total - s; return DD_OK; }   /* last paragraph */
    return DD_ERR_RANGE;
}

void df_doc_para_fmt_resolved(const df_doc *d, uint32_t index, ParaFmt *resolved)
{
    uint32_t start = 0, len = 0;
    if (df_doc_para_range(d, index, &start, &len) != DD_OK) { resolve_style_para(d, 0, resolved); return; }

    ParaFmt *direct = (ParaFmt *)rl_payload_at(&d->pruns, start);
    int style = (direct->mask & DD_PF_STYLE) ? direct->style_id : 0;
    resolve_style_para(d, style, resolved);
    overlay_para(resolved, direct);     /* direct overrides win over the style */
    resolved->mask = ALL_PF;
}

/* ---- queries ---- */

uint32_t df_doc_length(const df_doc *d) { return pt_total(&d->pt); }

uint32_t df_doc_get_text(const df_doc *d, uint32_t pos, uint32_t n, char *buf, uint32_t cap)
{
    return pt_get_text(&d->pt, pos, n, buf, cap);
}

void df_doc_char_fmt_at(const df_doc *d, uint32_t pos, CharFmt *resolved)
{
    uint32_t pi = df_doc_para_at(d, pos);
    uint32_t pstart = 0, plen = 0;
    df_doc_para_range(d, pi, &pstart, &plen);

    ParaFmt *pdirect = (ParaFmt *)rl_payload_at(&d->pruns, pstart);
    int style = (pdirect->mask & DD_PF_STYLE) ? pdirect->style_id : 0;

    resolve_style_char(d, style, resolved);
    overlay_char(resolved, (CharFmt *)rl_payload_at(&d->cruns, pos));   /* direct char wins */
    resolved->mask = ALL_CF;
}

/* Same result as df_doc_char_fmt_at, but for a caller that already knows the
 * byte start of the paragraph containing `pos` (e.g. the layout shaper, which
 * processes one paragraph at a time). This skips df_doc_para_at -- an O(pos)
 * newline scan from byte 0 -- and df_doc_para_range -- an O(total) scan of the
 * whole document. Those two were the only reason df_doc_char_fmt_at was O(doc
 * length); calling it per character made shaping O(n^2). `pos` must lie inside
 * the paragraph that starts at `para_start`. */
void df_doc_char_fmt_at_para(const df_doc *d, uint32_t para_start, uint32_t pos,
                             CharFmt *resolved)
{
    ParaFmt *pdirect = (ParaFmt *)rl_payload_at(&d->pruns, para_start);
    int style = (pdirect->mask & DD_PF_STYLE) ? pdirect->style_id : 0;

    resolve_style_char(d, style, resolved);
    overlay_char(resolved, (CharFmt *)rl_payload_at(&d->cruns, pos));
    resolved->mask = ALL_CF;
}

/* ---- UTF-8 caret helpers ---- */

uint32_t df_doc_next_cp(const df_doc *d, uint32_t pos)
{
    uint32_t total = pt_total(&d->pt);
    if (pos >= total) return total;
    int b = pt_byte_at(&d->pt, pos);
    uint32_t step = (b < 0x80) ? 1 : (b >= 0xF0) ? 4 : (b >= 0xE0) ? 3 : (b >= 0xC0) ? 2 : 1;
    pos += step;
    return pos > total ? total : pos;
}

uint32_t df_doc_prev_cp(const df_doc *d, uint32_t pos)
{
    if (pos == 0) return 0;
    uint32_t q = pos - 1;
    while (q > 0 && (pt_byte_at(&d->pt, q) & 0xC0) == 0x80 && pos - q < 4) q--;
    return q;
}

/* ---- editing ---- */

static void cf_overlay_cb(void *payload, void *ctx) { overlay_char((CharFmt *)payload, (CharFmt *)ctx); }
static void pf_overlay_cb(void *payload, void *ctx) { overlay_para((ParaFmt *)payload, (ParaFmt *)ctx); }

static dd_result do_apply_char(df_doc *d, uint32_t pos, uint32_t n, const CharFmt *cf)
{
    if (!cf || cf->mask == 0 || n == 0) return DD_OK;
    CharFmt c = *cf;
    return rl_apply(&d->cruns, pos, n, cf_overlay_cb, &c);
}

static dd_result do_set_para(df_doc *d, uint32_t pos, uint32_t n, const ParaFmt *pf)
{
    if (!pf || pf->mask == 0) return DD_OK;

    uint32_t total = pt_total(&d->pt);
    if (pos > total) pos = total;
    uint32_t end = (n == 0) ? pos : pos + n - 1;
    if (end > total) end = total ? total - 1 : 0;

    uint32_t p0 = df_doc_para_at(d, pos), p1 = df_doc_para_at(d, end);
    uint32_t s0 = 0, l0 = 0, s1 = 0, l1 = 0;
    df_doc_para_range(d, p0, &s0, &l0);
    df_doc_para_range(d, p1, &s1, &l1);
    uint32_t span = (s1 + l1) - s0;
    if (span == 0) return DD_OK;

    ParaFmt pp = *pf;
    return rl_apply(&d->pruns, s0, span, pf_overlay_cb, &pp);
}

dd_result df_doc_insert(df_doc *d, uint32_t pos, const char *utf8, uint32_t n)
{
    if (pos > pt_total(&d->pt)) return DD_ERR_RANGE;
    if (n == 0) return DD_OK;
    dd_result r = begin_edit(d); if (r) return r;
    r = pt_insert(&d->pt, pos, (const uint8_t *)utf8, n); if (r) return r;
    rl_insert(&d->cruns, pos, n);
    rl_insert(&d->pruns, pos, n);
    rl_insert(&d->sruns, pos, n);
    return DD_OK;
}

dd_result df_doc_insert_fmt(df_doc *d, uint32_t pos, const char *utf8, uint32_t n,
                            const CharFmt *overrides)
{
    if (pos > pt_total(&d->pt)) return DD_ERR_RANGE;
    if (n == 0) return DD_OK;
    dd_result r = begin_edit(d); if (r) return r;
    r = pt_insert(&d->pt, pos, (const uint8_t *)utf8, n); if (r) return r;
    rl_insert(&d->cruns, pos, n);
    rl_insert(&d->pruns, pos, n);
    rl_insert(&d->sruns, pos, n);
    if (overrides && overrides->mask) do_apply_char(d, pos, n, overrides);
    return DD_OK;
}

dd_result df_doc_delete(df_doc *d, uint32_t pos, uint32_t n)
{
    if (pos > pt_total(&d->pt)) return DD_ERR_RANGE;
    if (n == 0) return DD_OK;
    dd_result r = begin_edit(d); if (r) return r;
    r = pt_delete(&d->pt, pos, n); if (r) return r;
    rl_delete(&d->cruns, pos, n);
    rl_delete(&d->pruns, pos, n);
    rl_delete(&d->sruns, pos, n);
    return DD_OK;
}

dd_result df_doc_apply_char_fmt(df_doc *d, uint32_t pos, uint32_t n, const CharFmt *cf)
{
    if (n == 0 || !cf || cf->mask == 0) return DD_OK;
    dd_result r = begin_edit(d); if (r) return r;
    return do_apply_char(d, pos, n, cf);
}

dd_result df_doc_set_para_fmt(df_doc *d, uint32_t pos, uint32_t n, const ParaFmt *pf)
{
    if (!pf || pf->mask == 0) return DD_OK;
    dd_result r = begin_edit(d); if (r) return r;
    return do_set_para(d, pos, n, pf);
}

dd_result df_doc_set_para_style(df_doc *d, uint32_t pos, uint32_t n, int style_id)
{
    ParaFmt pf; memset(&pf, 0, sizeof pf);
    pf.mask = DD_PF_STYLE; pf.style_id = (int16_t)style_id;
    return df_doc_set_para_fmt(d, pos, n, &pf);
}

/* ---- sections (mutating) ---- */

dd_result df_doc_set_section_page_at(df_doc *d, uint32_t pos, const PageSetup *p)
{
    if (pos > pt_total(&d->pt)) return DD_ERR_RANGE;
    dd_result r = begin_edit(d); if (r) return r;
    uint32_t *si = (uint32_t *)rl_payload_at(&d->sruns, pos);
    d->sections[*si] = *p;
    return DD_OK;
}

dd_result df_doc_insert_section_break(df_doc *d, uint32_t pos)
{
    if (pos > pt_total(&d->pt)) return DD_ERR_RANGE;
    uint32_t fidx, fwithin; rl_find(&d->sruns, pos, &fidx, &fwithin);
    if (fwithin == 0) return DD_OK;                      /* already a section boundary */
    dd_result r = begin_edit(d); if (r) return r;
    uint32_t idx;
    r = rl_split_at(&d->sruns, pos, &idx); if (r) return r;
    uint32_t left_si = *(uint32_t *)rl_payload(&d->sruns, idx);   /* split copied left's index */
    PageSetup tmp = d->sections[left_si];                /* copy before sect_add may realloc */
    int ns = sect_add(d, &tmp);
    if (ns < 0) { rl_merge(&d->sruns); return DD_ERR_NOMEM; }     /* undo the redundant split */
    *(uint32_t *)rl_payload(&d->sruns, idx) = (uint32_t)ns;
    return DD_OK;
}

dd_result df_doc_remove_section_break(df_doc *d, uint32_t pos)
{
    uint32_t idx, within; rl_find(&d->sruns, pos, &idx, &within);
    if (within != 0 || idx == 0) return DD_OK;           /* not a boundary / first section */
    dd_result r = begin_edit(d); if (r) return r;
    *(uint32_t *)rl_payload(&d->sruns, idx) = *(uint32_t *)rl_payload(&d->sruns, idx - 1);
    rl_merge(&d->sruns);                                 /* coalesce the now-identical runs */
    return DD_OK;
}
