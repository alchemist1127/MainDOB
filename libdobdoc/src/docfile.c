/* libdobdoc -- file format (.dobw).
 *
 * Serialises the document MODEL -- content, formatting, styles -- not its
 * layout. A flat little-endian chunked container: a "DOBW" header then a
 * sequence of (tag, length, payload) chunks. Length-prefixing lets a
 * reader skip chunks it doesn't know, so the format can grow.
 *
 * Chunks: PAGE (geometry), DCHF (default char fmt), FAMS (family names),
 * STYL (named styles), TEXT (UTF-8 bytes), CRUN (char runs), PRUN (para
 * runs). On load, run totals are reconciled to the text length so a file
 * written by another tool still yields a consistent model.
 *
 * Extensibility (v3+): CharFmt/ParaFmt are stored as self-describing records
 * -- the chunk emits the record byte-size, the reader takes the fields it
 * knows and skips any tail a newer writer appended, so formatting can gain
 * fields with NO version bump. Whole new chunk types are skipped by older
 * readers (length-prefixing). Only ever APPEND fields; keep order stable.
 * Pre-v3 files use a different record layout and are not read.
 */

#include "doc_internal.h"
#include <string.h>
#include <stdlib.h>

#define ALL_CF (DD_CF_FAMILY | DD_CF_SIZE | DD_CF_BOLD | DD_CF_ITALIC | DD_CF_UNDERLINE | DD_CF_STRIKE | DD_CF_COLOR | DD_CF_HIGHLIGHT)
#define ALL_PF (DD_PF_ALIGN | DD_PF_INDENT_LEFT | DD_PF_INDENT_RIGHT | DD_PF_INDENT_FIRST | \
                DD_PF_SPACE_BEFORE | DD_PF_SPACE_AFTER | DD_PF_LINE_SPACING | DD_PF_STYLE)

/* ====================================================================
 *  writer
 * ==================================================================== */

typedef struct { uint8_t *p; uint32_t len, cap; bool err; } bb;

static void bb_init(bb *b) { b->p = NULL; b->len = 0; b->cap = 0; b->err = false; }

static void bb_ensure(bb *b, uint32_t need)
{
    if (b->err) return;
    if (b->len + need <= b->cap) return;
    uint32_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + need) nc *= 2;
    uint8_t *np = (uint8_t *)realloc(b->p, nc);
    if (!np) { b->err = true; return; }
    b->p = np; b->cap = nc;
}
static void bb_u8(bb *b, uint8_t v)  { bb_ensure(b, 1); if (b->err) return; b->p[b->len++] = v; }
static void bb_u16(bb *b, uint16_t v){ bb_u8(b, v & 0xff); bb_u8(b, (v >> 8) & 0xff); }
static void bb_u32(bb *b, uint32_t v){ bb_u8(b, v & 0xff); bb_u8(b, (v >> 8) & 0xff); bb_u8(b, (v >> 16) & 0xff); bb_u8(b, (v >> 24) & 0xff); }
static void bb_i32(bb *b, int32_t v) { bb_u32(b, (uint32_t)v); }
static void bb_bytes(bb *b, const void *d, uint32_t n) { bb_ensure(b, n); if (b->err) return; memcpy(b->p + b->len, d, n); b->len += n; }

static void bb_chunk(bb *b, const char *tag, const bb *payload)
{
    bb_bytes(b, tag, 4);
    bb_u32(b, payload->len);
    bb_bytes(b, payload->p, payload->len);
}

/* CharFmt / ParaFmt are written as self-describing records: each chunk that
 * stores them first emits the record size, so a reader takes the fields it
 * knows and skips the rest. New fields can be appended forever without a
 * format-version bump. Keep the field ORDER stable and only ever APPEND. */

static void put_charfmt(bb *b, const CharFmt *c)
{
    bb_u16(b, c->mask); bb_u16(b, c->family_id); bb_u32(b, c->size_twips);
    bb_u8(b, c->bold); bb_u8(b, c->italic); bb_u8(b, c->underline); bb_u8(b, c->strike);
    bb_u32(b, c->color);
    bb_u32(b, c->highlight);
    /* APPEND new CharFmt fields below this line only. */
}
static void put_parafmt(bb *b, const ParaFmt *p)
{
    bb_u16(b, p->mask); bb_u8(b, p->align); bb_u8(b, 0);
    bb_i32(b, p->indent_left); bb_i32(b, p->indent_right); bb_i32(b, p->indent_first);
    bb_u32(b, p->space_before); bb_u32(b, p->space_after); bb_u32(b, p->line_spacing);
    bb_u16(b, (uint16_t)p->style_id);
    /* APPEND new ParaFmt fields below this line only. */
}

/* On-disk record size, kept in lock-step with put_* by serialising a zero
 * record, so it can never drift from the writer. */
static uint16_t charfmt_rec_size(void)
{
    bb t; bb_init(&t); CharFmt z; memset(&z, 0, sizeof z);
    put_charfmt(&t, &z); uint16_t sz = (uint16_t)t.len; free(t.p); return sz;
}
static uint16_t parafmt_rec_size(void)
{
    bb t; bb_init(&t); ParaFmt z; memset(&z, 0, sizeof z);
    put_parafmt(&t, &z); uint16_t sz = (uint16_t)t.len; free(t.p); return sz;
}

/* PageSetup as a self-describing record (9 u32 fields). */
static void put_pagerec(bb *b, const PageSetup *p)
{
    bb_u32(b, p->width); bb_u32(b, p->height);
    bb_u32(b, p->margin_left); bb_u32(b, p->margin_right);
    bb_u32(b, p->margin_top); bb_u32(b, p->margin_bottom);
    bb_u32(b, p->bg_color); bb_u32(b, p->columns); bb_u32(b, p->column_gap);
}
static uint16_t pagerec_size(void) { return 9u * 4u; }   /* 36 bytes */

dd_result df_doc_serialize(const df_doc *d, uint8_t **out, uint32_t *size)
{
    bb b; bb_init(&b);
    bb_bytes(&b, "DOBW", 4); bb_u16(&b, 3); bb_u16(&b, 0);

    { bb c; bb_init(&c);
      const PageSetup *p0 = &d->sections[0];            /* section 0 == legacy single page */
      bb_u32(&c, p0->width); bb_u32(&c, p0->height);
      bb_u32(&c, p0->margin_left); bb_u32(&c, p0->margin_right);
      bb_u32(&c, p0->margin_top); bb_u32(&c, p0->margin_bottom);
      bb_u32(&c, p0->bg_color);
      bb_u32(&c, p0->columns); bb_u32(&c, p0->column_gap);
      bb_chunk(&b, "PAGE", &c); free(c.p); }

    { bb c; bb_init(&c); bb_u16(&c, charfmt_rec_size()); put_charfmt(&c, &d->def_cf); bb_chunk(&b, "DCHF", &c); free(c.p); }

    { bb c; bb_init(&c);
      bb_u16(&c, (uint16_t)d->nfam);
      for (int i = 0; i < d->nfam; i++) {
          const char *nm = d->fam[i] ? d->fam[i] : "";
          uint16_t l = (uint16_t)strlen(nm);
          bb_u16(&c, l); bb_bytes(&c, nm, l);
      }
      bb_chunk(&b, "FAMS", &c); free(c.p); }

    { bb c; bb_init(&c);
      bb_u16(&c, (uint16_t)d->nstyles);
      bb_u16(&c, charfmt_rec_size()); bb_u16(&c, parafmt_rec_size());
      for (int i = 0; i < d->nstyles; i++) {
          const Style *s = &d->styles[i];
          bb_bytes(&c, s->name, 48); bb_u16(&c, (uint16_t)s->parent);
          put_charfmt(&c, &s->cf); put_parafmt(&c, &s->pf);
      }
      bb_chunk(&b, "STYL", &c); free(c.p); }

    { uint32_t tl = pt_total(&d->pt); bb c; bb_init(&c);
      if (tl) {
          char *tmp = (char *)malloc(tl);
          if (tmp) { pt_get_text(&d->pt, 0, tl, tmp, tl); bb_bytes(&c, tmp, tl); free(tmp); }
          else c.err = true;
      }
      bb_chunk(&b, "TEXT", &c); if (c.err) b.err = true; free(c.p); }

    { bb c; bb_init(&c);
      bb_u32(&c, d->cruns.n); bb_u16(&c, charfmt_rec_size());
      for (uint32_t i = 0; i < d->cruns.n; i++) { bb_u32(&c, d->cruns.len[i]); put_charfmt(&c, (CharFmt *)rl_payload(&d->cruns, i)); }
      bb_chunk(&b, "CRUN", &c); free(c.p); }

    { bb c; bb_init(&c);
      bb_u32(&c, d->pruns.n); bb_u16(&c, parafmt_rec_size());
      for (uint32_t i = 0; i < d->pruns.n; i++) { bb_u32(&c, d->pruns.len[i]); put_parafmt(&c, (ParaFmt *)rl_payload(&d->pruns, i)); }
      bb_chunk(&b, "PRUN", &c); free(c.p); }

    /* Sections: the page table, then the index runs (len, section index). */
    { bb c; bb_init(&c);
      bb_u16(&c, pagerec_size()); bb_u32(&c, (uint32_t)d->nsections);
      for (int i = 0; i < d->nsections; i++) put_pagerec(&c, &d->sections[i]);
      bb_u32(&c, d->sruns.n);
      for (uint32_t i = 0; i < d->sruns.n; i++) {
          bb_u32(&c, d->sruns.len[i]);
          bb_u32(&c, *(uint32_t *)rl_payload(&d->sruns, i));
      }
      bb_chunk(&b, "SECT", &c); free(c.p); }

    if (b.err) { free(b.p); return DD_ERR_NOMEM; }
    *out = b.p; *size = b.len;
    return DD_OK;
}

/* ====================================================================
 *  reader
 * ==================================================================== */

typedef struct { const uint8_t *p; uint32_t len, pos; bool err; } rd;

static uint8_t  rd_u8(rd *r)  { if (r->pos + 1 > r->len) { r->err = true; return 0; } return r->p[r->pos++]; }
static uint16_t rd_u16(rd *r) { uint16_t a = rd_u8(r), b = rd_u8(r); return (uint16_t)(a | (b << 8)); }
static uint32_t rd_u32(rd *r) { uint32_t a = rd_u8(r), b = rd_u8(r), c = rd_u8(r), e = rd_u8(r); return a | (b << 8) | (c << 16) | (e << 24); }
static int32_t  rd_i32(rd *r) { return (int32_t)rd_u32(r); }
static void rd_bytes(rd *r, void *dst, uint32_t n) { if (r->pos + n > r->len) { r->err = true; return; } memcpy(dst, r->p + r->pos, n); r->pos += n; }
static void rd_skip(rd *r, uint32_t n) { if (r->pos + n > r->len) { r->err = true; return; } r->pos += n; }

/* Read a self-describing record: take the fields this build knows that fit in
 * `rec` bytes, leave the rest at their memset-zero default, and skip any tail
 * a newer writer appended. */
static void get_charfmt_sized(rd *r, CharFmt *c, uint32_t rec)
{
    uint32_t end = r->pos + rec;
    if (end > r->len) { r->err = true; return; }
    memset(c, 0, sizeof(*c));
    if (r->pos + 2 <= end) c->mask       = rd_u16(r);
    if (r->pos + 2 <= end) c->family_id  = rd_u16(r);
    if (r->pos + 4 <= end) c->size_twips = rd_u32(r);
    if (r->pos + 1 <= end) c->bold       = rd_u8(r);
    if (r->pos + 1 <= end) c->italic     = rd_u8(r);
    if (r->pos + 1 <= end) c->underline  = rd_u8(r);
    if (r->pos + 1 <= end) c->strike     = rd_u8(r);
    if (r->pos + 4 <= end) c->color      = rd_u32(r);
    if (r->pos + 4 <= end) c->highlight  = rd_u32(r);
    r->pos = end;
}
static void get_pagerec_sized(rd *r, PageSetup *p, uint32_t rec)
{
    uint32_t end = r->pos + rec;
    if (end > r->len) { r->err = true; return; }
    memset(p, 0, sizeof(*p));
    if (r->pos + 4 <= end) p->width        = rd_u32(r);
    if (r->pos + 4 <= end) p->height       = rd_u32(r);
    if (r->pos + 4 <= end) p->margin_left  = rd_u32(r);
    if (r->pos + 4 <= end) p->margin_right = rd_u32(r);
    if (r->pos + 4 <= end) p->margin_top   = rd_u32(r);
    if (r->pos + 4 <= end) p->margin_bottom= rd_u32(r);
    if (r->pos + 4 <= end) p->bg_color     = rd_u32(r);
    if (r->pos + 4 <= end) p->columns      = rd_u32(r);
    if (r->pos + 4 <= end) p->column_gap   = rd_u32(r);
    r->pos = end;
}
static void get_parafmt_sized(rd *r, ParaFmt *p, uint32_t rec)
{
    uint32_t end = r->pos + rec;
    if (end > r->len) { r->err = true; return; }
    memset(p, 0, sizeof(*p));
    if (r->pos + 2 <= end) p->mask         = rd_u16(r);
    if (r->pos + 1 <= end) p->align        = rd_u8(r);
    if (r->pos + 1 <= end) (void)rd_u8(r);                       /* _pad */
    if (r->pos + 4 <= end) p->indent_left  = rd_i32(r);
    if (r->pos + 4 <= end) p->indent_right = rd_i32(r);
    if (r->pos + 4 <= end) p->indent_first = rd_i32(r);
    if (r->pos + 4 <= end) p->space_before = rd_u32(r);
    if (r->pos + 4 <= end) p->space_after  = rd_u32(r);
    if (r->pos + 4 <= end) p->line_spacing = rd_u32(r);
    if (r->pos + 2 <= end) p->style_id     = (int16_t)rd_u16(r);
    r->pos = end;
}

static void fix_total(runlist *rl, uint32_t tl, const void *defp)
{
    uint32_t s = rl_total(rl);
    if (s == tl) return;
    if (s < tl) { if (rl->n == 0) rl_reset_single(rl, tl, defp); else rl->len[rl->n - 1] += tl - s; }
    else        { rl_delete(rl, tl, s - tl); }
}

dd_result df_doc_load(const uint8_t *data, uint32_t size, df_doc **out)
{
    rd r; r.p = data; r.len = size; r.pos = 0; r.err = false;

    char magic[4]; rd_bytes(&r, magic, 4);
    if (r.err || memcmp(magic, "DOBW", 4) != 0) return DD_ERR_BADFILE;
    uint16_t ver = rd_u16(&r); (void)rd_u16(&r);   /* version, flags */
    if (ver < 3) return DD_ERR_BADFILE;            /* pre-v3 record layout is not read */

    df_doc *d = df_doc_create();
    if (!d) return DD_ERR_NOMEM;

    bool got_crun = false, got_prun = false, got_sect = false;

    while (r.pos + 8 <= r.len && !r.err) {
        char tag[4]; rd_bytes(&r, tag, 4);
        uint32_t clen = rd_u32(&r);
        if (r.err) break;
        uint32_t chunk_end = r.pos + clen;
        if (chunk_end > r.len) { r.err = true; break; }

        if (memcmp(tag, "PAGE", 4) == 0) {
            PageSetup p0 = d->sections[0];               /* seed defaults for absent fields */
            p0.width  = rd_u32(&r); p0.height = rd_u32(&r);
            p0.margin_left = rd_u32(&r); p0.margin_right = rd_u32(&r);
            p0.margin_top  = rd_u32(&r); p0.margin_bottom = rd_u32(&r);
            if (r.pos + 4 <= chunk_end) p0.bg_color = rd_u32(&r);
            if (r.pos + 4 <= chunk_end) p0.columns = rd_u32(&r);
            if (r.pos + 4 <= chunk_end) p0.column_gap = rd_u32(&r);
            if (!got_sect) d->sections[0] = p0;          /* SECT, if present, is authoritative */
        } else if (memcmp(tag, "DCHF", 4) == 0) {
            uint16_t rec = rd_u16(&r); CharFmt c; get_charfmt_sized(&r, &c, rec); c.mask = ALL_CF; d->def_cf = c;
        } else if (memcmp(tag, "FAMS", 4) == 0) {
            for (int i = 0; i < d->nfam; i++) free(d->fam[i]);
            d->nfam = 0;
            uint16_t cnt = rd_u16(&r);
            for (uint16_t i = 0; i < cnt && !r.err; i++) {
                uint16_t l = rd_u16(&r);
                char *nm = (char *)malloc((size_t)l + 1);
                if (!nm) { r.err = true; break; }
                rd_bytes(&r, nm, l); nm[l] = '\0';
                if (d->nfam == d->fam_cap) {
                    int nc = d->fam_cap ? d->fam_cap * 2 : 8;
                    char **nf = (char **)realloc(d->fam, (size_t)nc * sizeof(char *));
                    if (!nf) { free(nm); r.err = true; break; }
                    d->fam = nf; d->fam_cap = nc;
                }
                d->fam[d->nfam++] = nm;
            }
        } else if (memcmp(tag, "STYL", 4) == 0) {
            d->nstyles = 0;                         /* Normal is reloaded from the file */
            uint16_t cnt = rd_u16(&r);
            uint16_t cfRec = rd_u16(&r), pfRec = rd_u16(&r);
            for (uint16_t i = 0; i < cnt && !r.err; i++) {
                char name[48]; rd_bytes(&r, name, 48); name[47] = '\0';
                int16_t parent = (int16_t)rd_u16(&r);
                CharFmt cf; get_charfmt_sized(&r, &cf, cfRec);
                ParaFmt pf; get_parafmt_sized(&r, &pf, pfRec);
                df_doc_style_define(d, name, parent, &cf, &pf);
            }
        } else if (memcmp(tag, "TEXT", 4) == 0) {
            if (clen) { if (pt_set_original(&d->pt, r.p + r.pos, clen)) r.err = true; }
            rd_skip(&r, clen);
        } else if (memcmp(tag, "CRUN", 4) == 0) {
            rl_free(&d->cruns); rl_init(&d->cruns, sizeof(CharFmt));
            uint32_t cnt = rd_u32(&r);
            uint16_t rec = rd_u16(&r);
            for (uint32_t i = 0; i < cnt && !r.err; i++) {
                uint32_t l = rd_u32(&r); CharFmt cf; get_charfmt_sized(&r, &cf, rec);
                rl_push(&d->cruns, l, &cf);
            }
            got_crun = true;
        } else if (memcmp(tag, "PRUN", 4) == 0) {
            rl_free(&d->pruns); rl_init(&d->pruns, sizeof(ParaFmt));
            uint32_t cnt = rd_u32(&r);
            uint16_t rec = rd_u16(&r);
            for (uint32_t i = 0; i < cnt && !r.err; i++) {
                uint32_t l = rd_u32(&r); ParaFmt pf; get_parafmt_sized(&r, &pf, rec);
                rl_push(&d->pruns, l, &pf);
            }
            got_prun = true;
        } else if (memcmp(tag, "SECT", 4) == 0) {
            uint16_t rec = rd_u16(&r);
            uint32_t nsec = rd_u32(&r);
            free(d->sections); d->sections = NULL; d->nsections = 0; d->sections_cap = 0;
            for (uint32_t i = 0; i < nsec && !r.err; i++) {
                PageSetup p; get_pagerec_sized(&r, &p, rec); sect_add(d, &p);
            }
            if (d->nsections == 0) {                     /* always keep a valid section 0 */
                PageSetup p; memset(&p, 0, sizeof p);
                p.width = 11906; p.height = 16838;
                p.margin_left = p.margin_right = 1440;
                p.margin_top = p.margin_bottom = 1440;
                p.bg_color = 0xFFFFFFFFu; p.columns = 1; p.column_gap = 720;
                sect_add(d, &p);
            }
            rl_free(&d->sruns); rl_init(&d->sruns, sizeof(uint32_t));
            uint32_t nruns = rd_u32(&r);
            for (uint32_t i = 0; i < nruns && !r.err; i++) {
                uint32_t l = rd_u32(&r), idx = rd_u32(&r);
                if (idx >= (uint32_t)d->nsections) idx = 0;       /* defend bad index */
                rl_push(&d->sruns, l, &idx);
            }
            got_sect = true;
        } else {
            rd_skip(&r, clen);                      /* unknown chunk */
        }

        r.pos = chunk_end;                          /* stay in sync regardless */
    }

    if (r.err) { df_doc_destroy(d); return DD_ERR_BADFILE; }

    uint32_t tl = pt_total(&d->pt);
    CharFmt zc; memset(&zc, 0, sizeof zc);
    ParaFmt zp; memset(&zp, 0, sizeof zp); zp.style_id = -1;
    if (!got_crun || d->cruns.n == 0) rl_reset_single(&d->cruns, tl, &zc);
    if (!got_prun || d->pruns.n == 0) rl_reset_single(&d->pruns, tl, &zp);
    fix_total(&d->cruns, tl, &zc);
    fix_total(&d->pruns, tl, &zp);
    {                                                   /* sections must span the whole text */
        uint32_t zero_idx = 0;
        if (!got_sect || rl_total(&d->sruns) != tl) {
            rl_free(&d->sruns); rl_init(&d->sruns, sizeof(uint32_t));
            rl_reset_single(&d->sruns, tl, &zero_idx);
        }
    }

    *out = d;
    return DD_OK;
}
