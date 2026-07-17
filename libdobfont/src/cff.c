/* libdobfont -- CFF / Type2 outline backend (PostScript-flavour .otf).
 *
 * The other half of "TTF and OTF as they are": most .otf files carry
 * PostScript outlines in a 'CFF ' table rather than TrueType 'glyf'.
 * This module parses the CFF structures (INDEX, DICT, charsets via
 * GID, global/local subroutines, and the CID-keyed FDArray/FDSelect)
 * and runs a Type2 charstring interpreter to emit a font-unit cubic
 * path -- the same df_path the glyf backend produces, so metrics,
 * rasterization and the rest of the engine are unchanged.
 *
 * Advances come from the sfnt 'hmtx' table (as for glyf); the optional
 * width operand at the head of a charstring is detected and skipped but
 * its value is not used. Hints are parsed only to consume their
 * operands and hintmask bytes. Coordinates are mapped to the font's
 * unitsPerEm via the FontMatrix so they share the glyf coordinate
 * space. Deprecated 'seac' accent composition (endchar with 4 args) is
 * not synthesized.
 */

#include "df_internal.h"
#include <string.h>
#include <stdlib.h>

/* ---- CFF INDEX ---- */
typedef struct { uint32_t count, off_size, offarr, data, end; } cff_index;

struct df_cff
{
    df_blob   blob;
    uint32_t  base;        /* offset of the CFF table in the font blob */
    float     scale;       /* charstring units -> font units (FontMatrix.a * upem) */
    bool      is_cid;
    uint16_t  num_glyphs;

    cff_index charstrings; /* one charstring per glyph id */
    cff_index gsubrs;      /* global subroutines */
    cff_index lsubrs;      /* local subroutines (non-CID) */

    uint32_t  fdselect_off;/* CID: GID -> FD map */
    uint32_t  nfd;
    cff_index *fd_lsubrs;  /* CID: local subrs per FD */
};

typedef struct df_cff df_cff;

/* read an off_size-byte big-endian integer */
static uint32_t rd_offN(const df_blob *b, uint32_t pos, uint32_t sz)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < sz; i++) v = (v << 8) | df_rd_u8(b, pos + i);
    return v;
}

static void read_index(const df_blob *b, uint32_t pos, cff_index *ix)
{
    uint32_t count = df_rd_u16(b, pos);
    if (count == 0) {
        ix->count = 0; ix->off_size = 0;
        ix->offarr = pos + 2; ix->data = pos + 2; ix->end = pos + 2;
        return;
    }
    uint32_t osz = df_rd_u8(b, pos + 2);
    ix->count = count; ix->off_size = osz; ix->offarr = pos + 3;
    ix->data = ix->offarr + (count + 1) * osz - 1;          /* offsets are 1-based */
    uint32_t last = rd_offN(b, ix->offarr + count * osz, osz);
    ix->end = ix->data + last;
}

static bool index_get(const df_blob *b, const cff_index *ix, uint32_t i,
                      uint32_t *start, uint32_t *len)
{
    if (i >= ix->count) return false;
    uint32_t o0 = rd_offN(b, ix->offarr + i * ix->off_size, ix->off_size);
    uint32_t o1 = rd_offN(b, ix->offarr + (i + 1) * ix->off_size, ix->off_size);
    *start = ix->data + o0; *len = o1 - o0;
    return true;
}

/* ---- DICT ---- */
typedef struct
{
    bool     has_charstrings; uint32_t charstrings_off;
    bool     has_private;     uint32_t private_size, private_off;
    bool     has_subrs;       uint32_t subrs_off;     /* relative to Private DICT */
    bool     has_fdarray;     uint32_t fdarray_off;
    bool     has_fdselect;    uint32_t fdselect_off;
    bool     has_ros;
    float    fm_a;                                    /* FontMatrix[0] */
} cff_dict;

static double cff_atof(const char *s)
{
    double sign = 1, val = 0; int i = 0;
    if (s[i] == '-') { sign = -1; i++; }
    while (s[i] >= '0' && s[i] <= '9') { val = val * 10 + (s[i] - '0'); i++; }
    if (s[i] == '.') { i++; double f = 0.1;
        while (s[i] >= '0' && s[i] <= '9') { val += (s[i] - '0') * f; f *= 0.1; i++; } }
    int e = 0, es = 1;
    if (s[i] == 'E' || s[i] == 'e') { i++;
        if (s[i] == '-') { es = -1; i++; } else if (s[i] == '+') i++;
        while (s[i] >= '0' && s[i] <= '9') { e = e * 10 + (s[i] - '0'); i++; } }
    double p = 1; for (int k = 0; k < e; k++) p *= 10;
    return sign * (es < 0 ? val / p : val * p);
}

static double parse_real(const df_blob *b, uint32_t *pp)
{
    char buf[64]; int n = 0; uint32_t p = *pp; bool done = false;
    while (!done && n < 60) {
        uint8_t by = df_rd_u8(b, p++);
        for (int k = 0; k < 2; k++) {
            uint8_t nib = k == 0 ? (by >> 4) : (by & 0xf);
            if (nib <= 9)       buf[n++] = (char)('0' + nib);
            else if (nib == 0xa) buf[n++] = '.';
            else if (nib == 0xb) buf[n++] = 'E';
            else if (nib == 0xc) { buf[n++] = 'E'; buf[n++] = '-'; }
            else if (nib == 0xe) buf[n++] = '-';
            else if (nib == 0xf) { done = true; break; }
        }
    }
    buf[n] = '\0'; *pp = p;
    return cff_atof(buf);
}

static void dict_parse(const df_blob *b, uint32_t start, uint32_t end, cff_dict *d)
{
    memset(d, 0, sizeof(*d)); d->fm_a = 0.001f;
    double st[48]; int sp = 0;
    uint32_t p = start;
    while (p < end) {
        uint8_t b0 = df_rd_u8(b, p);
        if (b0 <= 21) {                                   /* operator */
            uint32_t op = b0; p++;
            if (b0 == 12) { op = 1200 + df_rd_u8(b, p); p++; }
            switch (op) {
                case 17:   if (sp >= 1) { d->has_charstrings = true; d->charstrings_off = (uint32_t)st[sp-1]; } break;
                case 18:   if (sp >= 2) { d->has_private = true; d->private_size = (uint32_t)st[sp-2]; d->private_off = (uint32_t)st[sp-1]; } break;
                case 19:   if (sp >= 1) { d->has_subrs = true; d->subrs_off = (uint32_t)st[sp-1]; } break;
                case 1207: if (sp >= 1) { d->fm_a = (float)st[0]; } break;                       /* FontMatrix */
                case 1230: d->has_ros = true; break;                                              /* ROS -> CID  */
                case 1236: if (sp >= 1) { d->has_fdarray = true; d->fdarray_off = (uint32_t)st[sp-1]; } break;
                case 1237: if (sp >= 1) { d->has_fdselect = true; d->fdselect_off = (uint32_t)st[sp-1]; } break;
                default: break;
            }
            sp = 0;
        } else {                                          /* operand */
            if (b0 == 28) { int v = (int16_t)((df_rd_u8(b,p+1) << 8) | df_rd_u8(b,p+2)); if (sp < 48) st[sp++] = v; p += 3; }
            else if (b0 == 29) { int32_t v = (int32_t)(((uint32_t)df_rd_u8(b,p+1) << 24) | ((uint32_t)df_rd_u8(b,p+2) << 16) | ((uint32_t)df_rd_u8(b,p+3) << 8) | df_rd_u8(b,p+4)); if (sp < 48) st[sp++] = v; p += 5; }
            else if (b0 == 30) { double v = parse_real(b, (p++, &p)); if (sp < 48) st[sp++] = v; }
            else if (b0 >= 32 && b0 <= 246) { if (sp < 48) st[sp++] = (int)b0 - 139; p++; }
            else if (b0 >= 247 && b0 <= 250) { int v = (b0 - 247) * 256 + df_rd_u8(b,p+1) + 108; if (sp < 48) st[sp++] = v; p += 2; }
            else if (b0 >= 251 && b0 <= 254) { int v = -((int)(b0 - 251)) * 256 - df_rd_u8(b,p+1) - 108; if (sp < 48) st[sp++] = v; p += 2; }
            else p++;
        }
    }
}

/* ---- open ---- */
df_result df_cff_open(df_face *f)
{
    if (!f->cff.off) return DF_ERR_BADFONT;

    df_cff *c = (df_cff *)calloc(1, sizeof(df_cff));
    if (!c) return DF_ERR_NOMEM;
    c->blob = f->blob; c->base = f->cff.off; c->num_glyphs = f->num_glyphs;
    c->scale = 0.001f * (float)f->units_per_em;

    uint32_t base = c->base;
    uint8_t  hdrsize = df_rd_u8(&c->blob, base + 2);
    uint32_t p = base + hdrsize;

    cff_index nameidx, topidx, stridx;
    read_index(&c->blob, p, &nameidx); p = nameidx.end;
    read_index(&c->blob, p, &topidx);  p = topidx.end;
    read_index(&c->blob, p, &stridx);  p = stridx.end;
    read_index(&c->blob, p, &c->gsubrs);

    uint32_t ts, tl;
    if (!index_get(&c->blob, &topidx, 0, &ts, &tl)) { free(c); return DF_ERR_BADFONT; }
    cff_dict top; dict_parse(&c->blob, ts, ts + tl, &top);

    c->scale = top.fm_a * (float)f->units_per_em;
    if (!top.has_charstrings) { free(c); return DF_ERR_BADFONT; }
    read_index(&c->blob, base + top.charstrings_off, &c->charstrings);
    c->is_cid = top.has_ros;

    if (!c->is_cid) {
        if (top.has_private) {
            cff_dict priv;
            dict_parse(&c->blob, base + top.private_off,
                       base + top.private_off + top.private_size, &priv);
            if (priv.has_subrs)
                read_index(&c->blob, base + top.private_off + priv.subrs_off, &c->lsubrs);
        }
    } else if (top.has_fdarray && top.has_fdselect) {
        c->fdselect_off = base + top.fdselect_off;
        cff_index fdarr; read_index(&c->blob, base + top.fdarray_off, &fdarr);
        c->nfd = fdarr.count;
        if (c->nfd > 0) {
            c->fd_lsubrs = (cff_index *)calloc(c->nfd, sizeof(cff_index));
            if (!c->fd_lsubrs) { free(c); return DF_ERR_NOMEM; }
            for (uint32_t i = 0; i < c->nfd; i++) {
                uint32_t fs, fl;
                if (!index_get(&c->blob, &fdarr, i, &fs, &fl)) continue;
                cff_dict fd; dict_parse(&c->blob, fs, fs + fl, &fd);
                if (fd.has_private) {
                    cff_dict priv;
                    dict_parse(&c->blob, base + fd.private_off,
                               base + fd.private_off + fd.private_size, &priv);
                    if (priv.has_subrs)
                        read_index(&c->blob, base + fd.private_off + priv.subrs_off,
                                   &c->fd_lsubrs[i]);
                }
            }
        }
    }

    f->cffp = c; f->outline_kind = DF_OUTLINE_CFF; f->has_outlines = true;
    return DF_OK;
}

void df_cff_close(df_face *f)
{
    if (!f->cffp) return;
    free(f->cffp->fd_lsubrs);
    free(f->cffp);
    f->cffp = NULL;
}

static uint32_t fdselect_lookup(const df_cff *c, uint32_t gid)
{
    uint32_t off = c->fdselect_off;
    uint8_t  fmt = df_rd_u8(&c->blob, off);
    if (fmt == 0) return df_rd_u8(&c->blob, off + 1 + gid);
    if (fmt == 3) {
        uint16_t nr = df_rd_u16(&c->blob, off + 1);
        uint32_t r  = off + 3;
        for (uint16_t i = 0; i < nr; i++) {
            uint16_t first = df_rd_u16(&c->blob, r + i * 3);
            uint16_t next  = df_rd_u16(&c->blob, r + (i + 1) * 3);   /* sentinel after last */
            if (gid >= first && gid < next) return df_rd_u8(&c->blob, r + i * 3 + 2);
        }
    }
    return 0;
}

/* ====================================================================
 *  Type2 charstring interpreter
 * ==================================================================== */

#define T2_MAX_DEPTH 16

typedef struct
{
    const df_cff   *c;
    const cff_index *ls;
    df_path        *p;
    float           scale;
    float           st[64]; int sp;
    float           x, y; bool open;
    int             nstems; bool width_done;
    struct { uint32_t pos, end; } call[T2_MAX_DEPTH]; int depth;
} t2ctx;

#define EMIT(call) do { df_result _r = (call); if (_r) return _r; } while (0)

static df_result t2_moveto(t2ctx *X)
{
    if (X->open) {
        df_seg cl; memset(&cl, 0, sizeof cl); cl.k = DF_CLOSE;
        EMIT(df_path_push(X->p, &cl));
    }
    df_seg s; memset(&s, 0, sizeof s); s.k = DF_MOVE;
    s.x[0] = X->x * X->scale; s.y[0] = X->y * X->scale;
    X->open = true;
    return df_path_push(X->p, &s);
}
static df_result t2_lineto(t2ctx *X)
{
    df_seg s; memset(&s, 0, sizeof s); s.k = DF_LINE;
    s.x[0] = X->x * X->scale; s.y[0] = X->y * X->scale;
    return df_path_push(X->p, &s);
}
static df_result t2_curveto(t2ctx *X, float c1x, float c1y, float c2x, float c2y)
{
    df_seg s; s.k = DF_CUBIC;
    s.x[0] = c1x * X->scale; s.y[0] = c1y * X->scale;
    s.x[1] = c2x * X->scale; s.y[1] = c2y * X->scale;
    s.x[2] = X->x * X->scale; s.y[2] = X->y * X->scale;
    return df_path_push(X->p, &s);
}
static void t2_close(t2ctx *X)
{
    if (!X->open) return;
    df_seg cl; memset(&cl, 0, sizeof cl); cl.k = DF_CLOSE;
    df_path_push(X->p, &cl); X->open = false;
}

static void count_stems(t2ctx *X)
{
    X->width_done = true;       /* a stem op fixes width presence; value unused */
    X->nstems += X->sp / 2;     /* if sp is odd, the leftover leading width is dropped */
    X->sp = 0;
}
/* index of first real arg given the expected count (skips a leading width) */
static int take_width(t2ctx *X, int nargs)
{
    int base = 0;
    if (!X->width_done) { if (X->sp > nargs) base = 1; X->width_done = true; }
    return base;
}

static int subr_bias(uint32_t n)
{
    return n < 1240 ? 107 : (n < 33900 ? 1131 : 32768);
}

static df_result run_charstring(t2ctx *X, uint32_t start, uint32_t end)
{
    const df_blob *b = &X->c->blob;
    X->call[0].pos = start; X->call[0].end = end; X->depth = 0;

    for (;;) {
        if (X->call[X->depth].pos >= X->call[X->depth].end) {
            if (X->depth == 0) break;
            X->depth--; continue;              /* implicit return from subr */
        }
        uint32_t pos = X->call[X->depth].pos;
        uint8_t  b0  = df_rd_u8(b, pos);

        if (b0 >= 32 || b0 == 28) {            /* operand */
            float v; uint32_t adv;
            if (b0 == 28)        { v = (int16_t)((df_rd_u8(b,pos+1) << 8) | df_rd_u8(b,pos+2)); adv = 3; }
            else if (b0 <= 246)  { v = (int)b0 - 139; adv = 1; }
            else if (b0 <= 250)  { v = (b0 - 247) * 256 + df_rd_u8(b,pos+1) + 108; adv = 2; }
            else if (b0 <= 254)  { v = -((int)(b0 - 251)) * 256 - df_rd_u8(b,pos+1) - 108; adv = 2; }
            else { int32_t iv = (int32_t)(((uint32_t)df_rd_u8(b,pos+1) << 24) | ((uint32_t)df_rd_u8(b,pos+2) << 16) | ((uint32_t)df_rd_u8(b,pos+3) << 8) | df_rd_u8(b,pos+4)); v = (float)iv / 65536.0f; adv = 5; }
            if (X->sp < 64) X->st[X->sp++] = v;
            X->call[X->depth].pos = pos + adv;
            continue;
        }

        uint32_t op = b0, adv = 1;
        if (b0 == 12) { op = 1200 + df_rd_u8(b, pos + 1); adv = 2; }
        X->call[X->depth].pos = pos + adv;

        switch (op) {
            case 1: case 3: case 18: case 23:                   /* h/v stem (hm) */
                count_stems(X); break;
            case 19: case 20: {                                 /* hintmask / cntrmask */
                count_stems(X);
                X->call[X->depth].pos += (uint32_t)((X->nstems + 7) / 8);
                break;
            }
            case 21: { int o = take_width(X, 2); X->x += X->st[o]; X->y += X->st[o+1]; EMIT(t2_moveto(X)); X->sp = 0; break; }
            case 22: { int o = take_width(X, 1); X->x += X->st[o]; EMIT(t2_moveto(X)); X->sp = 0; break; }
            case 4:  { int o = take_width(X, 1); X->y += X->st[o]; EMIT(t2_moveto(X)); X->sp = 0; break; }

            case 5:                                             /* rlineto */
                for (int i = 0; i + 2 <= X->sp; i += 2) { X->x += X->st[i]; X->y += X->st[i+1]; EMIT(t2_lineto(X)); }
                X->sp = 0; break;
            case 6:                                             /* hlineto */
                for (int i = 0; i < X->sp; i++) { if (i & 1) X->y += X->st[i]; else X->x += X->st[i]; EMIT(t2_lineto(X)); }
                X->sp = 0; break;
            case 7:                                             /* vlineto */
                for (int i = 0; i < X->sp; i++) { if (i & 1) X->x += X->st[i]; else X->y += X->st[i]; EMIT(t2_lineto(X)); }
                X->sp = 0; break;

            case 8:                                             /* rrcurveto */
                for (int i = 0; i + 6 <= X->sp; i += 6) {
                    float c1x = X->x + X->st[i],   c1y = X->y + X->st[i+1];
                    float c2x = c1x + X->st[i+2],  c2y = c1y + X->st[i+3];
                    X->x = c2x + X->st[i+4]; X->y = c2y + X->st[i+5];
                    EMIT(t2_curveto(X, c1x, c1y, c2x, c2y));
                }
                X->sp = 0; break;

            case 24: {                                          /* rcurveline */
                int i = 0;
                while (i + 6 <= X->sp - 2) {
                    float c1x = X->x + X->st[i],  c1y = X->y + X->st[i+1];
                    float c2x = c1x + X->st[i+2], c2y = c1y + X->st[i+3];
                    X->x = c2x + X->st[i+4]; X->y = c2y + X->st[i+5];
                    EMIT(t2_curveto(X, c1x, c1y, c2x, c2y)); i += 6;
                }
                X->x += X->st[i]; X->y += X->st[i+1]; EMIT(t2_lineto(X));
                X->sp = 0; break;
            }
            case 25: {                                          /* rlinecurve */
                int i = 0;
                while (i + 2 <= X->sp - 6) { X->x += X->st[i]; X->y += X->st[i+1]; EMIT(t2_lineto(X)); i += 2; }
                float c1x = X->x + X->st[i],  c1y = X->y + X->st[i+1];
                float c2x = c1x + X->st[i+2], c2y = c1y + X->st[i+3];
                X->x = c2x + X->st[i+4]; X->y = c2y + X->st[i+5];
                EMIT(t2_curveto(X, c1x, c1y, c2x, c2y));
                X->sp = 0; break;
            }

            case 26: {                                          /* vvcurveto */
                int i = 0; float dx1 = 0;
                if (X->sp & 1) { dx1 = X->st[0]; i = 1; }
                for (; i + 4 <= X->sp; i += 4) {
                    float c1x = X->x + dx1,        c1y = X->y + X->st[i];
                    float c2x = c1x + X->st[i+1],  c2y = c1y + X->st[i+2];
                    X->x = c2x; X->y = c2y + X->st[i+3];
                    EMIT(t2_curveto(X, c1x, c1y, c2x, c2y)); dx1 = 0;
                }
                X->sp = 0; break;
            }
            case 27: {                                          /* hhcurveto */
                int i = 0; float dy1 = 0;
                if (X->sp & 1) { dy1 = X->st[0]; i = 1; }
                for (; i + 4 <= X->sp; i += 4) {
                    float c1x = X->x + X->st[i],   c1y = X->y + dy1;
                    float c2x = c1x + X->st[i+1],  c2y = c1y + X->st[i+2];
                    X->x = c2x + X->st[i+3]; X->y = c2y;
                    EMIT(t2_curveto(X, c1x, c1y, c2x, c2y)); dy1 = 0;
                }
                X->sp = 0; break;
            }
            case 30: case 31: {                                 /* vhcurveto / hvcurveto */
                int i = 0; bool horiz = (op == 31);
                while (X->sp - i >= 4) {
                    bool last = (X->sp - i == 5);
                    float c1x, c1y, c2x, c2y;
                    if (horiz) {
                        c1x = X->x + X->st[i]; c1y = X->y;
                        c2x = c1x + X->st[i+1]; c2y = c1y + X->st[i+2];
                        X->y = c2y + X->st[i+3];
                        X->x = c2x + (last ? X->st[i+4] : 0);
                    } else {
                        c1x = X->x; c1y = X->y + X->st[i];
                        c2x = c1x + X->st[i+1]; c2y = c1y + X->st[i+2];
                        X->x = c2x + X->st[i+3];
                        X->y = c2y + (last ? X->st[i+4] : 0);
                    }
                    EMIT(t2_curveto(X, c1x, c1y, c2x, c2y));
                    horiz = !horiz; i += 4;
                }
                X->sp = 0; break;
            }

            case 1234: {                                        /* hflex */
                float *s = X->st, sy = X->y;
                float c1x = X->x + s[0], c1y = X->y;
                float c2x = c1x + s[1],  c2y = c1y + s[2];
                X->x = c2x + s[3]; X->y = c2y; EMIT(t2_curveto(X, c1x, c1y, c2x, c2y));
                float c3x = X->x + s[4], c3y = X->y;
                float c4x = c3x + s[5],  c4y = sy;
                X->x = c4x + s[6]; X->y = sy; EMIT(t2_curveto(X, c3x, c3y, c4x, c4y));
                X->sp = 0; break;
            }
            case 1235: {                                        /* flex */
                float *s = X->st;
                float c1x = X->x + s[0], c1y = X->y + s[1];
                float c2x = c1x + s[2],  c2y = c1y + s[3];
                X->x = c2x + s[4]; X->y = c2y + s[5]; EMIT(t2_curveto(X, c1x, c1y, c2x, c2y));
                float c3x = X->x + s[6], c3y = X->y + s[7];
                float c4x = c3x + s[8],  c4y = c3y + s[9];
                X->x = c4x + s[10]; X->y = c4y + s[11]; EMIT(t2_curveto(X, c3x, c3y, c4x, c4y));
                X->sp = 0; break;
            }
            case 1236: {                                        /* hflex1 */
                float *s = X->st, sy = X->y;
                float c1x = X->x + s[0], c1y = X->y + s[1];
                float c2x = c1x + s[2],  c2y = c1y + s[3];
                X->x = c2x + s[4]; X->y = c2y; EMIT(t2_curveto(X, c1x, c1y, c2x, c2y));
                float c3x = X->x + s[5], c3y = X->y;
                float c4x = c3x + s[6],  c4y = c3y + s[7];
                X->x = c4x + s[8]; X->y = sy; EMIT(t2_curveto(X, c3x, c3y, c4x, c4y));
                X->sp = 0; break;
            }
            case 1237: {                                        /* flex1 */
                float *s = X->st, sx = X->x, sy = X->y;
                float dx = s[0]+s[2]+s[4]+s[6]+s[8];
                float dy = s[1]+s[3]+s[5]+s[7]+s[9];
                float c1x = X->x + s[0], c1y = X->y + s[1];
                float c2x = c1x + s[2],  c2y = c1y + s[3];
                X->x = c2x + s[4]; X->y = c2y + s[5]; EMIT(t2_curveto(X, c1x, c1y, c2x, c2y));
                float c3x = X->x + s[6], c3y = X->y + s[7];
                float c4x = c3x + s[8],  c4y = c3y + s[9];
                if (df_fabsf(dx) > df_fabsf(dy)) { X->x = c4x + s[10]; X->y = sy; }
                else                             { X->x = sx;          X->y = c4y + s[10]; }
                EMIT(t2_curveto(X, c3x, c3y, c4x, c4y));
                X->sp = 0; break;
            }

            case 10: {                                          /* callsubr */
                if (X->sp > 0) {
                    int idx = (int)X->st[--X->sp] + subr_bias(X->ls->count);
                    uint32_t ss, sl;
                    if (X->depth + 1 < T2_MAX_DEPTH &&
                        index_get(b, X->ls, (uint32_t)idx, &ss, &sl)) {
                        X->depth++; X->call[X->depth].pos = ss; X->call[X->depth].end = ss + sl;
                    }
                }
                break;
            }
            case 29: {                                          /* callgsubr */
                if (X->sp > 0) {
                    int idx = (int)X->st[--X->sp] + subr_bias(X->c->gsubrs.count);
                    uint32_t ss, sl;
                    if (X->depth + 1 < T2_MAX_DEPTH &&
                        index_get(b, &X->c->gsubrs, (uint32_t)idx, &ss, &sl)) {
                        X->depth++; X->call[X->depth].pos = ss; X->call[X->depth].end = ss + sl;
                    }
                }
                break;
            }
            case 11:                                            /* return */
                if (X->depth > 0) X->depth--;
                break;
            case 14:                                            /* endchar */
                t2_close(X);
                return DF_OK;
            default:
                X->sp = 0; break;                               /* arithmetic / unknown */
        }
    }

    t2_close(X);
    return DF_OK;
}

df_result df_cff_decode(const df_face *f, uint32_t gid, df_path *p)
{
    const df_cff *c = f->cffp;
    if (!c) return DF_OK;

    uint32_t cs_start, cs_len;
    if (!index_get(&c->blob, &c->charstrings, gid, &cs_start, &cs_len)) return DF_OK;

    const cff_index *ls = &c->lsubrs;
    if (c->is_cid && c->fd_lsubrs) {
        uint32_t fd = fdselect_lookup(c, gid);
        if (fd < c->nfd) ls = &c->fd_lsubrs[fd];
    }

    t2ctx X; memset(&X, 0, sizeof X);
    X.c = c; X.ls = ls; X.p = p; X.scale = c->scale;
    return run_charstring(&X, cs_start, cs_len);
}
