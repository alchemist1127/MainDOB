/* libdobfont -- font resolver implementation. */

#include <dobfont/resolver.h>
#include <string.h>
#include <stdlib.h>

#define DF_MAX_FALLBACK 8

typedef struct
{
    char      family[64];
    df_face  *face;
    char      path[256];
    uint32_t  index;
    int       weight;     /* 100..900 */
    bool      italic;
} df_entry;

struct df_fontset
{
    df_entry *e;
    int       n, cap;
    char      fallback[DF_MAX_FALLBACK][64];
    int       nfallback;
};

/* ---- small string helpers (ASCII, no libc niceties) ---- */

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool ieq(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) { if (lower(*a) != lower(*b)) return false; a++; b++; }
    return *a == *b;
}

static void copy_bounded(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return;
    if (src) while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ---- fontset ---- */

df_fontset *df_fontset_create(void)
{
    return (df_fontset *)calloc(1, sizeof(df_fontset));
}

void df_fontset_destroy(df_fontset *set)
{
    if (!set) return;
    free(set->e);          /* faces are NOT closed here -- caller owns them */
    free(set);
}

df_result df_fontset_add(df_fontset *set, const char *family,
                         df_face *face, const char *path, uint32_t face_index)
{
    if (!set || !face) return DF_ERR_INVAL;

    if (set->n == set->cap) {
        int nc = set->cap ? set->cap * 2 : 8;
        df_entry *ne = (df_entry *)realloc(set->e, (size_t)nc * sizeof(df_entry));
        if (!ne) return DF_ERR_NOMEM;
        set->e = ne; set->cap = nc;
    }
    df_entry *en = &set->e[set->n++];
    memset(en, 0, sizeof(*en));
    copy_bounded(en->family, family, sizeof(en->family));
    copy_bounded(en->path,   path,   sizeof(en->path));
    en->face   = face;
    en->index  = face_index;
    en->weight = df_face_weight(face);
    en->italic = df_face_is_italic(face);
    return DF_OK;
}

df_result df_fontset_set_fallback(df_fontset *set,
                                  const char *const *families, int count)
{
    if (!set) return DF_ERR_INVAL;
    if (count > DF_MAX_FALLBACK) count = DF_MAX_FALLBACK;
    set->nfallback = 0;
    for (int i = 0; i < count; i++) {
        copy_bounded(set->fallback[set->nfallback], families[i],
                     sizeof(set->fallback[0]));
        set->nfallback++;
    }
    return DF_OK;
}

/* ---- matching ---- */

/* CSS-ish weight distance with a small directional bias. */
static int weight_dist(int have, int want)
{
    int d = have > want ? have - want : want - have;
    int bias = 0;
    if (want < 400)      { if (have > want) bias = 1; }
    else if (want > 500) { if (have < want) bias = 1; }
    else                 { if (have < want) bias = 1; }
    return d * 2 + bias;
}

/* lower score = better fit for the requested style */
static int style_score(const df_entry *en, const df_query *q)
{
    int s = weight_dist(en->weight, q->weight);
    if (en->italic != q->italic) s += 100000;       /* style mismatch dominates */
    return s;
}

static int fallback_index(const df_fontset *set, const char *family)
{
    for (int i = 0; i < set->nfallback; i++)
        if (ieq(set->fallback[i], family)) return i;
    return -1;
}

static bool covers(df_face *face, uint32_t cp)
{
    return cp == 0 || df_map_codepoint(face, cp) != 0;
}

/* best entry in the requested family (or any, if family is empty/unknown) */
static int pick_best(const df_fontset *set, const df_query *q, bool *family_found)
{
    bool want_family = (q->family && q->family[0]);
    *family_found = false;

    int best = -1, best_score = 0;
    for (int i = 0; i < set->n; i++) {
        if (want_family && !ieq(set->e[i].family, q->family)) continue;
        *family_found = true;
        int sc = style_score(&set->e[i], q);
        if (best < 0 || sc < best_score) { best = i; best_score = sc; }
    }
    if (best >= 0) return best;

    /* requested family not present -> best of everything */
    for (int i = 0; i < set->n; i++) {
        int sc = style_score(&set->e[i], q);
        if (best < 0 || sc < best_score) { best = i; best_score = sc; }
    }
    return best;
}

/* find a covering face, preferring same family > fallback chain > any,
 * and within a tier the closest style */
static int pick_covering(const df_fontset *set, const df_query *q)
{
    int best = -1, best_prio = -1, best_score = 0, best_fb = 0;
    for (int i = 0; i < set->n; i++) {
        if (!covers(set->e[i].face, q->codepoint)) continue;

        int prio, fb = fallback_index(set, set->e[i].family);
        if (q->family && q->family[0] && ieq(set->e[i].family, q->family)) prio = 3;
        else if (fb >= 0)                                                  prio = 2;
        else                                                               prio = 1;

        int sc = style_score(&set->e[i], q);
        bool better =
            best < 0 ||
            prio > best_prio ||
            (prio == best_prio && sc < best_score) ||
            (prio == best_prio && sc == best_score && prio == 2 && fb < best_fb);
        if (better) { best = i; best_prio = prio; best_score = sc; best_fb = (fb < 0 ? 0 : fb); }
    }
    return best;
}

df_result df_resolve(const df_fontset *set, const df_query *q, df_resolved *out)
{
    if (!set || !q || !out) return DF_ERR_INVAL;
    if (set->n == 0)        return DF_ERR_NOGLYPH;
    memset(out, 0, sizeof(*out));

    bool family_found;
    int idx = pick_best(set, q, &family_found);
    if (idx < 0) return DF_ERR_NOGLYPH;

    /* coverage: if the chosen face lacks the code point, walk the chain */
    if (q->codepoint && !covers(set->e[idx].face, q->codepoint)) {
        int cov = pick_covering(set, q);
        if (cov >= 0) idx = cov;          /* else keep idx -> renders .notdef */
    }

    const df_entry *en = &set->e[idx];
    out->face       = en->face;
    out->path       = en->path;
    out->face_index = en->index;
    out->gid        = q->codepoint ? df_map_codepoint(en->face, q->codepoint) : 0;

    /* synthesis relative to what was requested */
    out->slant = (q->italic && !en->italic) ? 0.21f : 0.0f;
    if (en->weight < q->weight - 100) {
        int gap = q->weight - en->weight;            /* still lighter than asked */
        out->embolden_em = ((float)gap / 100.0f) * 0.010f;
    } else {
        out->embolden_em = 0.0f;
    }
    return DF_OK;
}

void df_resolved_raster_req(const df_resolved *r, float px_size, df_raster_req *req)
{
    req->px_size  = px_size;
    req->embolden = r->embolden_em * px_size;
    req->slant    = r->slant;
    req->shift_x  = 0.0f;
}

/* ====================================================================
 *  manifest.dob (type=font) parser
 * ==================================================================== */

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

/* copy until terminator chars, trimming trailing spaces */
static int copy_token(char *dst, size_t cap, const char *p, const char *end,
                      const char *terms)
{
    size_t n = 0;
    while (p < end && !strchr(terms, *p)) {
        if (n + 1 < cap) dst[n++] = *p;
        p++;
    }
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\t')) n--;
    dst[n] = '\0';
    return (int)n;
}

static void parse_faces(df_font_manifest *out, const char *p, const char *end)
{
    while (p < end && out->num_faces < 16) {
        p = skip_ws(p, end);
        char item[160];
        copy_token(item, sizeof(item), p, end, ",\r\n");
        /* advance p past this item + separator */
        while (p < end && *p != ',' && *p != '\r' && *p != '\n') p++;
        if (p < end && *p == ',') p++;

        if (!item[0]) continue;
        df_font_face_spec *fs = &out->faces[out->num_faces];
        char *colon = strchr(item, ':');
        if (colon) {
            *colon = '\0';
            fs->index = (uint32_t)atoi(colon + 1);
        } else {
            fs->index = 0;
        }
        copy_bounded(fs->file, item, sizeof(fs->file));
        out->num_faces++;
    }
}

df_result df_font_manifest_parse(const char *text, uint32_t len,
                                 df_font_manifest *out)
{
    if (!text || !out) return DF_ERR_INVAL;
    memset(out, 0, sizeof(*out));

    const char *p = text, *end = text + len;
    while (p < end) {
        const char *line_end = p;
        while (line_end < end && *line_end != '\n') line_end++;

        const char *q = skip_ws(p, line_end);
        if (q < line_end && *q != '#') {
            const char *eq = q;
            while (eq < line_end && *eq != '=') eq++;
            if (eq < line_end) {
                char key[32];
                copy_token(key, sizeof(key), q, eq, "=");
                const char *val = skip_ws(eq + 1, line_end);
                if (ieq(key, "name"))        copy_token(out->name,   sizeof(out->name),   val, line_end, "\r\n");
                else if (ieq(key, "family")) copy_token(out->family, sizeof(out->family), val, line_end, "\r\n");
                else if (ieq(key, "faces"))  parse_faces(out, val, line_end);
            }
        }
        p = (line_end < end) ? line_end + 1 : end;
    }

    /* default family to name if the manifest only gave a name */
    if (!out->family[0] && out->name[0])
        copy_bounded(out->family, out->name, sizeof(out->family));

    return DF_OK;
}
