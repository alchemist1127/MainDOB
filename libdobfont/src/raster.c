/* libdobfont -- coverage rasterizer.
 *
 * Input: a path already mapped into device space (pixels, y down).
 * Output: an 8-bit coverage buffer (R8) -- the per-pixel alpha that dv
 * blends against the destination in dv_draw_glyphs ("graduated
 * transparency"). Anti-aliasing comes from sampling fractional pixel
 * coverage, nothing else.
 *
 * Method: flatten curves to line edges, then for each pixel row take
 * 4 vertical sub-scanlines; on each, compute exact horizontal coverage
 * of the nonzero-winding interior and accumulate. Exact in x, 4x
 * oversampled in y -- good quality at text sizes, no libm, no float
 * sqrt. Speed can be improved later (active edge table); correctness
 * first.
 */

#include "df_internal.h"
#include <string.h>
#include <stdlib.h>

#define DF_SS 4   /* vertical sub-scanlines per pixel row */

typedef struct { float x0, y0, x1, y1; int dir; } df_edge;

typedef struct { df_edge *e; int n, cap; } df_edges;

static int edges_push(df_edges *es, float x0, float y0, float x1, float y1)
{
    if (y0 == y1) return 0;                 /* horizontal edges contribute nothing */
    int dir = 1;
    if (y1 < y0) { float t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; dir = -1; }
    if (es->n == es->cap) {
        int nc = es->cap ? es->cap * 2 : 128;
        df_edge *ne = (df_edge *)realloc(es->e, (size_t)nc * sizeof(df_edge));
        if (!ne) return -1;
        es->e = ne; es->cap = nc;
    }
    es->e[es->n].x0 = x0; es->e[es->n].y0 = y0;
    es->e[es->n].x1 = x1; es->e[es->n].y1 = y1;
    es->e[es->n].dir = dir; es->n++;
    return 0;
}

/* steps for flattening a curve, from its control-polygon extent */
static int curve_steps(float ext)
{
    int s = (int)(ext / 3.0f) + 1;
    if (s < 2)  s = 2;
    if (s > 64) s = 64;
    return s;
}

static int flatten_quad(df_edges *es, float x0, float y0,
                        float cx, float cy, float x1, float y1)
{
    float ext = df_maxf(df_fabsf(cx - x0) + df_fabsf(x1 - cx),
                        df_fabsf(cy - y0) + df_fabsf(y1 - cy));
    int n = curve_steps(ext);
    float px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1.0f - t;
        float qx = u * u * x0 + 2.0f * u * t * cx + t * t * x1;
        float qy = u * u * y0 + 2.0f * u * t * cy + t * t * y1;
        if (edges_push(es, px, py, qx, qy)) return -1;
        px = qx; py = qy;
    }
    return 0;
}

static int flatten_cubic(df_edges *es, float x0, float y0,
                         float c1x, float c1y, float c2x, float c2y,
                         float x1, float y1)
{
    float ext = df_maxf(df_fabsf(c1x - x0) + df_fabsf(c2x - c1x) + df_fabsf(x1 - c2x),
                        df_fabsf(c1y - y0) + df_fabsf(c2y - c1y) + df_fabsf(y1 - c2y));
    int n = curve_steps(ext);
    float px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1.0f - t;
        float uu = u * u, tt = t * t;
        float qx = uu * u * x0 + 3.0f * uu * t * c1x + 3.0f * u * tt * c2x + tt * t * x1;
        float qy = uu * u * y0 + 3.0f * uu * t * c1y + 3.0f * u * tt * c2y + tt * t * y1;
        if (edges_push(es, px, py, qx, qy)) return -1;
        px = qx; py = qy;
    }
    return 0;
}

/* build the edge list from the path */
static int build_edges(const df_path *p, df_edges *es)
{
    float cpx = 0, cpy = 0, csx = 0, csy = 0;
    bool open = false;

    for (uint32_t i = 0; i < p->count; i++) {
        const df_seg *s = &p->seg[i];
        switch (s->k) {
            case DF_MOVE:
                if (open && (cpx != csx || cpy != csy))
                    if (edges_push(es, cpx, cpy, csx, csy)) return -1;
                cpx = csx = s->x[0]; cpy = csy = s->y[0]; open = true;
                break;
            case DF_LINE:
                if (edges_push(es, cpx, cpy, s->x[0], s->y[0])) return -1;
                cpx = s->x[0]; cpy = s->y[0];
                break;
            case DF_QUAD:
                if (flatten_quad(es, cpx, cpy, s->x[0], s->y[0], s->x[1], s->y[1])) return -1;
                cpx = s->x[1]; cpy = s->y[1];
                break;
            case DF_CUBIC:
                if (flatten_cubic(es, cpx, cpy, s->x[0], s->y[0],
                                  s->x[1], s->y[1], s->x[2], s->y[2])) return -1;
                cpx = s->x[2]; cpy = s->y[2];
                break;
            case DF_CLOSE:
                if (cpx != csx || cpy != csy)
                    if (edges_push(es, cpx, cpy, csx, csy)) return -1;
                cpx = csx; cpy = csy;
                break;
        }
    }
    if (open && (cpx != csx || cpy != csy))
        if (edges_push(es, cpx, cpy, csx, csy)) return -1;
    return 0;
}

/* add exact horizontal coverage of [xa,xb) to a pixel row */
static void add_span(float *acc, int w, float xa, float xb, float weight)
{
    if (xb <= xa) return;
    if (xa < 0.0f) xa = 0.0f;
    if (xb > (float)w) xb = (float)w;
    if (xb <= xa) return;

    int ix = (int)df_floorf(xa);
    if (ix < 0) ix = 0;
    for (; ix < w && (float)ix < xb; ix++) {
        float l = df_maxf(xa, (float)ix);
        float r = df_minf(xb, (float)(ix + 1));
        float c = r - l;
        if (c > 0.0f) acc[ix] += c * weight;
    }
}

df_result df_raster_fill(const df_path *p, int w, int h, uint8_t *cover)
{
    memset(cover, 0, (size_t)w * (size_t)h);
    if (w <= 0 || h <= 0) return DF_OK;

    df_edges es = { NULL, 0, 0 };
    if (build_edges(p, &es)) { free(es.e); return DF_ERR_NOMEM; }
    if (es.n == 0) { free(es.e); return DF_OK; }

    float *acc = (float *)malloc((size_t)w * sizeof(float));
    float *xs  = (float *)malloc((size_t)es.n * sizeof(float));
    int   *dr  = (int   *)malloc((size_t)es.n * sizeof(int));
    if (!acc || !xs || !dr) { free(acc); free(xs); free(dr); free(es.e); return DF_ERR_NOMEM; }

    const float invss = 1.0f / (float)DF_SS;

    for (int py = 0; py < h; py++) {
        memset(acc, 0, (size_t)w * sizeof(float));

        for (int s = 0; s < DF_SS; s++) {
            float sy = (float)py + ((float)s + 0.5f) * invss;

            int cnt = 0;
            for (int k = 0; k < es.n; k++) {
                const df_edge *e = &es.e[k];
                if (sy < e->y0 || sy >= e->y1) continue;
                float t = (sy - e->y0) / (e->y1 - e->y0);
                xs[cnt] = e->x0 + t * (e->x1 - e->x0);
                dr[cnt] = e->dir;
                cnt++;
            }
            /* insertion sort crossings by x (cnt is small per scanline) */
            for (int a = 1; a < cnt; a++) {
                float kx = xs[a]; int kd = dr[a]; int b = a - 1;
                while (b >= 0 && xs[b] > kx) { xs[b + 1] = xs[b]; dr[b + 1] = dr[b]; b--; }
                xs[b + 1] = kx; dr[b + 1] = kd;
            }
            /* nonzero winding sweep */
            int wind = 0; float span_start = 0;
            for (int j = 0; j < cnt; j++) {
                int before = wind; wind += dr[j];
                if (before == 0 && wind != 0)      span_start = xs[j];
                else if (before != 0 && wind == 0) add_span(acc, w, span_start, xs[j], invss);
            }
        }

        uint8_t *row = cover + (size_t)py * (size_t)w;
        for (int px = 0; px < w; px++) {
            float v = acc[px];
            if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
            row[px] = (uint8_t)(v * 255.0f + 0.5f);
        }
    }

    free(acc); free(xs); free(dr); free(es.e);
    return DF_OK;
}
