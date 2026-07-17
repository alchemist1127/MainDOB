/* DobUITools — Graph Controls Implementation
 *
 * See graph.h for the model.  All rendering goes through the standard
 * DobInterface primitives, which the client stub batches into the
 * window's command buffer and dobinterface replays on the GPU.
 *
 *   linegraph : polyline via Bresenham -> DrawPixel records.
 *   bargraph  : one FillRect per bar.
 *   piechart  : per-pixel slice test (integer atan2) with run-length
 *               FillRect emission, plus an optional legend.
 *
 * No FPU / float anywhere: angles use an integer arctangent table.
 */

#include "graph.h"
#include <DobInterface.h>
#include <string.h>
#include <stdio.h>

/* ======================================================================
 *  Shared helpers
 * ====================================================================== */

/* Map v in [lo,hi] linearly onto [0,extent], clamped to the ends.
 *
 * 32-bit arithmetic (long is 32-bit on this target): exact while
 * |v-lo|*extent stays under 2^31, which covers any realistic graph
 * (e.g. a 0..100000 range against a 4096-px extent leaves wide margin).
 * For an extreme range, pre-scale the values you push (plot in
 * thousands, etc.). */
static int scale_extent(int v, int lo, int hi, int extent)
{
    if (extent <= 0) return 0;
    if (hi <= lo)    return 0;
    if (v <= lo)     return 0;
    if (v >= hi)     return extent;
    return (int)((long)(v - lo) * extent / (hi - lo));
}

/* Bresenham line into a window/widget surface, one DrawPixel per pixel.
 * (A run-length variant could emit fewer records for steep/shallow
 * segments; per-pixel is kept for clarity — line graphs are small.) */
static void emit_line(uint32_t win, int x0, int y0, int x1, int y1,
                      uint32_t col)
{
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;)
    {
        dobui_DrawPixel(win, x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Integer arctangent table: ATAN_LUT[i] = round(atan(i/64) in degrees),
 * i = 0..64, so it covers a 0..45 deg span for a ratio of the smaller
 * over the larger coordinate.  Mirrored across octants by iatan2_deg. */
static const int ATAN_LUT[65] = {
     0,  1,  2,  3,  4,  4,  5,  6,  7,  8,  9, 10, 11,
    11, 12, 13, 14, 15, 16, 17, 17, 18, 19, 20, 21, 21,
    22, 23, 24, 24, 25, 26, 27, 27, 28, 29, 29, 30, 31,
    31, 32, 33, 33, 34, 35, 35, 36, 36, 37, 37, 38, 39,
    39, 40, 40, 41, 41, 42, 42, 43, 43, 44, 44, 45, 45
};

/* atan2 in whole degrees, result in [0,360), measured CCW from +x.
 * Pure integer; ~1 deg accuracy, ample for pie slice boundaries. */
static int iatan2_deg(int y, int x)
{
    if (x == 0 && y == 0) return 0;

    int ax = (x < 0) ? -x : x;
    int ay = (y < 0) ? -y : y;

    int deg;   /* magnitude angle in [0,90] for the (ax,ay) corner */
    if (ax >= ay)
    {
        int idx = (ax == 0) ? 64 : (ay * 64 / ax);
        if (idx > 64) idx = 64;
        deg = ATAN_LUT[idx];            /* 0..45 */
    }
    else
    {
        int idx = (ay == 0) ? 64 : (ax * 64 / ay);
        if (idx > 64) idx = 64;
        deg = 90 - ATAN_LUT[idx];       /* 45..90 */
    }

    int a;
    if      (x >= 0 && y >= 0) a = deg;          /* Q1 */
    else if (x <  0 && y >= 0) a = 180 - deg;    /* Q2 */
    else if (x <  0 && y <  0) a = 180 + deg;    /* Q3 */
    else                       a = 360 - deg;    /* Q4 */

    if (a >= 360) a -= 360;
    return a;
}

/* Angle of a pixel offset, clockwise from 12 o'clock (the conventional
 * pie start).  top -> 0, right -> 90, bottom -> 180, left -> 270.
 * Screen y grows downward, so we feed (-dy). */
static int pie_angle(int dx, int dy)
{
    return iatan2_deg(dx, -dy);
}

/* Which slice owns angle `ang`, given cumulative boundaries
 * bound[0..n] ascending from 0 to 360.  Zero-width slices (equal
 * neighbouring boundaries) are skipped automatically. */
static int pie_bucket(int ang, const int *bound, int n)
{
    for (int k = 0; k < n; k++)
        if (ang >= bound[k] && ang < bound[k + 1])
            return k;
    return n - 1;   /* defensive: ang==360 cannot occur (atan2 < 360) */
}

/* Default slice palette when the caller passes no colours. */
static const uint32_t PIE_PALETTE[] = {
    DOBUI_TEXT, DOBUI_INPUT, DOBUI_SUCCESS, DOBUI_DANGER,
    DOBUI_SPECIAL, 0x00FF8800, 0x0000AAFF, 0x0088FF00  /* palette categorica data-viz (eccezione) */
};
#define PIE_PALETTE_N  ((int)(sizeof(PIE_PALETTE) / sizeof(PIE_PALETTE[0])))

/* ======================================================================
 *  Line graph
 * ====================================================================== */

void doblg_Init(dob_linegraph_t *g, uint32_t win_id,
                int x, int y, int w, int h,
                int resolution, int range_min, int range_max,
                uint32_t bg_color, uint32_t line_color)
{
    memset(g, 0, sizeof(*g));
    g->win_id = win_id;
    g->x = x; g->y = y; g->w = w; g->h = h;

    if (resolution < 1) resolution = 1;
    if (resolution > DOBLG_MAX_SAMPLES) resolution = DOBLG_MAX_SAMPLES;
    g->n     = resolution;
    g->count = 0;

    g->range_min = range_min;
    g->range_max = range_max;
    g->autoscale = false;

    g->visible     = true;
    g->show_border = false;

    g->col_bg     = bg_color;
    g->col_line   = line_color;
    g->col_border = DOBLG_COL_BORDER;
}

void doblg_SetAutoscale(dob_linegraph_t *g, bool on) { g->autoscale = on; }

void doblg_SetRange(dob_linegraph_t *g, int min, int max)
{
    g->range_min = min;
    g->range_max = max;
}

/* x pixel of display slot j (0..n-1), stretched across the full width. */
static int lg_slot_x(const dob_linegraph_t *g, int j)
{
    if (g->n <= 1) return g->x;
    return g->x + (int)((long)j * (g->w - 1) / (g->n - 1));
}

/* y pixel for value v given the active range [lo,hi].  Higher value =
 * higher on screen (smaller y).  Flat data (hi<=lo) sits at mid-height. */
static int lg_val_y(const dob_linegraph_t *g, int v, int lo, int hi)
{
    if (g->h <= 1) return g->y;
    if (hi <= lo)  return g->y + (g->h - 1) / 2;
    int up = scale_extent(v, lo, hi, g->h - 1);
    return g->y + (g->h - 1) - up;
}

/* min/max over the currently valid samples (for autoscale). */
static void lg_autorange(const dob_linegraph_t *g, int *lo, int *hi)
{
    int first = g->n - g->count;
    bool any = false;
    int mn = 0, mx = 0;
    for (int j = first; j < g->n; j++)
    {
        int v = g->samples[j];
        if (!any) { mn = mx = v; any = true; }
        else { if (v < mn) mn = v; if (v > mx) mx = v; }
    }
    if (!any) { *lo = g->range_min; *hi = g->range_max; return; }
    *lo = mn;
    *hi = mx;
}

void doblg_Draw(dob_linegraph_t *g)
{
    if (!g->visible) return;

    dobui_FillRect(g->win_id, g->x, g->y, g->w, g->h, g->col_bg);

    int lo, hi;
    if (g->autoscale) lg_autorange(g, &lo, &hi);
    else { lo = g->range_min; hi = g->range_max; }

    int first = g->n - g->count;

    if (g->count == 1)
    {
        int px = lg_slot_x(g, g->n - 1);
        int py = lg_val_y(g, g->samples[g->n - 1], lo, hi);
        dobui_DrawPixel(g->win_id, px, py, g->col_line);
    }
    else if (g->count >= 2)
    {
        for (int j = first; j < g->n - 1; j++)
        {
            int x0 = lg_slot_x(g, j),     y0 = lg_val_y(g, g->samples[j],     lo, hi);
            int x1 = lg_slot_x(g, j + 1), y1 = lg_val_y(g, g->samples[j + 1], lo, hi);
            emit_line(g->win_id, x0, y0, x1, y1, g->col_line);
        }
    }

    if (g->show_border)
        dobui_DrawRect(g->win_id, g->x, g->y, g->w, g->h, g->col_border);
}

void doblg_UpdateLast(dob_linegraph_t *g, int value)
{
    if (g->n <= 0) return;
    /* Scroll left by one slot; newest goes to the rightmost slot. */
    if (g->n > 1)
        memmove(&g->samples[0], &g->samples[1],
                (size_t)(g->n - 1) * sizeof(int));
    g->samples[g->n - 1] = value;
    if (g->count < g->n) g->count++;
    doblg_Draw(g);
}

void doblg_UpdateAll(dob_linegraph_t *g, const int *values, int count)
{
    if (g->n <= 0) return;
    if (count < 0) count = 0;

    int keep = (count < g->n) ? count : g->n;   /* last `keep` values */
    int src0 = count - keep;

    for (int j = 0; j < g->n - keep; j++) g->samples[j] = 0;   /* tidy pad */
    for (int j = 0; j < keep; j++)
        g->samples[g->n - keep + j] = values ? values[src0 + j] : 0;

    g->count = keep;
    doblg_Draw(g);
}

void doblg_Update(dob_linegraph_t *g, int index, int value)
{
    if (index < 0 || index >= g->n) return;
    g->samples[index] = value;
    g->count = g->n;   /* addressing absolute columns = full strip */
    doblg_Draw(g);
}

/* ======================================================================
 *  Bar graph
 * ====================================================================== */

void dobbg_Init(dob_bargraph_t *g, uint32_t win_id,
                int x, int y, int w, int h,
                int n_bars, dob_bar_orient_t orient,
                const uint32_t *bar_colors, uint32_t bg_color,
                int range_min, int range_max)
{
    memset(g, 0, sizeof(*g));
    g->win_id = win_id;
    g->x = x; g->y = y; g->w = w; g->h = h;

    if (n_bars < 1) n_bars = 1;
    if (n_bars > DOBBG_MAX_BARS) n_bars = DOBBG_MAX_BARS;
    g->n_bars = n_bars;

    g->orient = orient;
    g->gap    = DOBBG_DEFAULT_GAP;

    g->range_min = range_min;
    g->range_max = range_max;
    g->autoscale = false;

    g->visible = true;
    g->col_bg  = bg_color;

    for (int i = 0; i < n_bars; i++)
        g->colors[i] = bar_colors ? bar_colors[i] : DOBBG_COL_FILL;
}

void dobbg_SetAutoscale(dob_bargraph_t *g, bool on) { g->autoscale = on; }

void dobbg_SetRange(dob_bargraph_t *g, int min, int max)
{
    g->range_min = min;
    g->range_max = max;
}

void dobbg_SetBarColor(dob_bargraph_t *g, int index, uint32_t color)
{
    if (index >= 0 && index < g->n_bars) g->colors[index] = color;
}

/* Autoscale range for bars: floor stays at range_min (usually 0), the
 * top tracks the tallest current value. */
static void bg_autorange(const dob_bargraph_t *g, int *lo, int *hi)
{
    int mx = g->range_min;
    bool any = false;
    for (int i = 0; i < g->n_bars; i++)
    {
        int v = g->values[i];
        if (!any) { mx = v; any = true; }
        else if (v > mx) mx = v;
    }
    *lo = g->range_min;
    *hi = (mx > *lo) ? mx : (*lo + 1);
}

void dobbg_Draw(dob_bargraph_t *g)
{
    if (!g->visible) return;

    dobui_FillRect(g->win_id, g->x, g->y, g->w, g->h, g->col_bg);
    if (g->n_bars <= 0) return;

    int lo, hi;
    if (g->autoscale) bg_autorange(g, &lo, &hi);
    else { lo = g->range_min; hi = g->range_max; }

    if (g->orient == DOB_BAR_VERTICAL)
    {
        int slot = g->w / g->n_bars; if (slot < 1) slot = 1;
        for (int i = 0; i < g->n_bars; i++)
        {
            int bw = slot - g->gap; if (bw < 1) bw = 1;
            int bx = g->x + i * slot + (slot - bw) / 2;
            int len = scale_extent(g->values[i], lo, hi, g->h);
            if (len > 0)
                dobui_FillRect(g->win_id, bx, g->y + g->h - len, bw, len,
                               g->colors[i]);
        }
    }
    else /* DOB_BAR_HORIZONTAL */
    {
        int slot = g->h / g->n_bars; if (slot < 1) slot = 1;
        for (int i = 0; i < g->n_bars; i++)
        {
            int bh = slot - g->gap; if (bh < 1) bh = 1;
            int by = g->y + i * slot + (slot - bh) / 2;
            int len = scale_extent(g->values[i], lo, hi, g->w);
            if (len > 0)
                dobui_FillRect(g->win_id, g->x, by, len, bh, g->colors[i]);
        }
    }
}

void dobbg_Update(dob_bargraph_t *g, int index, int value)
{
    if (index < 0 || index >= g->n_bars) return;
    g->values[index] = value;
    dobbg_Draw(g);
}

void dobbg_UpdateAll(dob_bargraph_t *g, const int *values, int count)
{
    int m = (count < g->n_bars) ? count : g->n_bars;
    if (m < 0) m = 0;
    for (int i = 0; i < m; i++) g->values[i] = values ? values[i] : 0;
    dobbg_Draw(g);
}

/* ======================================================================
 *  Pie chart
 * ====================================================================== */

void dobpie_Init(dob_piechart_t *g, uint32_t win_id,
                 int x, int y, int w, int h,
                 int n_slices, const uint32_t *slice_colors,
                 uint32_t bg_color, bool show_legend)
{
    memset(g, 0, sizeof(*g));
    g->win_id = win_id;
    g->x = x; g->y = y; g->w = w; g->h = h;

    if (n_slices < 1) n_slices = 1;
    if (n_slices > DOBPIE_MAX_SLICES) n_slices = DOBPIE_MAX_SLICES;
    g->n_slices = n_slices;

    g->show_legend   = show_legend;
    g->visible       = true;
    g->col_bg        = bg_color;
    g->col_legend_fg = DOBPIE_COL_LEGEND_FG;

    for (int i = 0; i < n_slices; i++)
        g->colors[i] = slice_colors ? slice_colors[i]
                                    : PIE_PALETTE[i % PIE_PALETTE_N];
}

void dobpie_SetLabel(dob_piechart_t *g, int index, const char *label)
{
    if (index < 0 || index >= g->n_slices || !label) return;
    strncpy(g->labels[index], label, DOBPIE_LABEL_MAX - 1);
    g->labels[index][DOBPIE_LABEL_MAX - 1] = '\0';
}

void dobpie_SetSliceColor(dob_piechart_t *g, int index, uint32_t color)
{
    if (index >= 0 && index < g->n_slices) g->colors[index] = color;
}

void dobpie_Draw(dob_piechart_t *g)
{
    if (!g->visible) return;

    dobui_FillRect(g->win_id, g->x, g->y, g->w, g->h, g->col_bg);
    if (g->n_slices <= 0) return;

    long total = 0;
    for (int i = 0; i < g->n_slices; i++)
        if (g->values[i] > 0) total += g->values[i];

    /* Split the box: pie on the left, legend (if any) on the right.
     * Legend width tracks the box (~40%) but is bounded so the pie keeps
     * at least 40px — the "adaptable pie/legend ratio". */
    int legend_w = 0;
    if (g->show_legend)
    {
        legend_w = g->w * 2 / 5;
        if (legend_w < 56) legend_w = 56;
        if (legend_w > g->w - 40) legend_w = g->w - 40;
        if (legend_w < 0) legend_w = 0;
    }
    int pie_area_w = g->w - legend_w;
    if (pie_area_w < 1) pie_area_w = 1;

    int side = (pie_area_w < g->h) ? pie_area_w : g->h;
    int r = side / 2 - 2; if (r < 1) r = 1;
    int cx = g->x + pie_area_w / 2;
    int cy = g->y + g->h / 2;

    if (total > 0)
    {
        /* Cumulative slice boundaries in degrees, 0..360 clockwise. */
        int bound[DOBPIE_MAX_SLICES + 1];
        long acc = 0;
        bound[0] = 0;
        for (int i = 0; i < g->n_slices; i++)
        {
            int v = g->values[i]; if (v < 0) v = 0;
            acc += v;
            bound[i + 1] = (int)(360L * acc / total);
        }
        bound[g->n_slices] = 360;   /* close the circle despite rounding */

        int r2 = r * r;
        for (int py = cy - r; py <= cy + r; py++)
        {
            int dy = py - cy;
            int run_start = 0;
            int run_slice = -2;     /* sentinel: forces a flush on change */

            /* One column past the right edge acts as a sentinel so the
             * final run is flushed inside the loop. */
            for (int px = cx - r; px <= cx + r + 1; px++)
            {
                int slice;
                if (px <= cx + r)
                {
                    int dx = px - cx;
                    if (dx * dx + dy * dy <= r2)
                        slice = pie_bucket(pie_angle(dx, dy), bound, g->n_slices);
                    else
                        slice = -1;     /* outside the circle */
                }
                else
                {
                    slice = -3;         /* end sentinel */
                }

                if (slice != run_slice)
                {
                    if (run_slice >= 0)
                        dobui_FillRect(g->win_id, run_start, py,
                                       px - run_start, 1, g->colors[run_slice]);
                    run_start = px;
                    run_slice = slice;
                }
            }
        }
    }

    /* Legend: a colour swatch + label + percentage per slice, clipped to
     * the box height. */
    if (g->show_legend && legend_w > 0)
    {
        int lx  = g->x + pie_area_w + 4;
        int ly  = g->y + 4;
        int rh  = DOBGRAPH_FONT_H + 4;
        int sw  = 10;

        for (int i = 0; i < g->n_slices; i++)
        {
            if (ly + DOBGRAPH_FONT_H > g->y + g->h) break;   /* no room */

            dobui_FillRect(g->win_id, lx, ly + (DOBGRAPH_FONT_H - sw) / 2,
                           sw, sw, g->colors[i]);

            int v = (g->values[i] > 0) ? g->values[i] : 0;
            int pct = (total > 0) ? (int)((long)v * 100 / total) : 0;

            char buf[DOBPIE_LABEL_MAX + 8];
            if (g->labels[i][0])
                snprintf(buf, sizeof(buf), "%s %d%%", g->labels[i], pct);
            else
                snprintf(buf, sizeof(buf), "%d%%", pct);

            dobui_DrawText(g->win_id, lx + sw + 4, ly, buf,
                           g->col_legend_fg, g->col_bg);
            ly += rh;
        }
    }
}

void dobpie_Update(dob_piechart_t *g, int index, int value)
{
    if (index < 0 || index >= g->n_slices) return;
    g->values[index] = value;
    dobpie_Draw(g);
}

void dobpie_UpdateAll(dob_piechart_t *g, const int *values, int count)
{
    int m = (count < g->n_slices) ? count : g->n_slices;
    if (m < 0) m = 0;
    for (int i = 0; i < m; i++) g->values[i] = values ? values[i] : 0;
    dobpie_Draw(g);
}
