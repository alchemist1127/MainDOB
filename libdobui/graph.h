/* DobUITools — Graph Controls (linegraph / bargraph / piechart)
 *
 * Three read-only (output) controls for plotting numeric data, rendered
 * with the standard DobInterface primitives — no extra dependency, no
 * vprocess, no GPU surface of their own.  Like every other libdobui
 * control they draw into a window (or widget) backing buffer; the
 * compositor (dobinterface) does the hardware work underneath via the
 * dv command list.
 *
 *   - linegraph : the "sali-scendi" strip chart (CPU meter / stock tape).
 *   - bargraph  : vertical OR horizontal bars (one control, an enum picks
 *                 the orientation — the layout logic is identical).
 *   - piechart  : proportional slices, with an optional legend.
 *
 * Rendering backend.  Lines are rasterised in software (Bresenham) and
 * emitted as DrawPixel records; bars and pie slices are emitted as
 * FillRect records.  All of these append to the window's command buffer
 * and are replayed as dv_cmdlist_* primitives on the GPU at Invalidate.
 * The pie's slice membership is decided with an integer atan2 (no FPU,
 * no float), so the controls build and run identically on every video
 * driver.
 *
 * Allocation.  Every control embeds its sample/colour/label storage in
 * the struct (fixed caps below).  No malloc, no destructor — same shape
 * as button/progressbar/etc.  A control can live on the stack or in BSS.
 *
 * Redraw model (matches the rest of libdobui).  The *_Update* helpers
 * mutate state and repaint into the command buffer; the program decides
 * WHEN the result reaches the screen by calling dobui_Invalidate(win)
 * at its own cadence.  So N updates inside one tick cost a single screen
 * flush.  _Draw() does a full repaint from current state (call it once
 * after Init, and after any layout change).
 *
 * Value scaling (linegraph + bargraph).  Values map linearly onto the
 * drawing extent across a declared range [min, max]: a value of 50 on a
 * 0..100 range fills about half.  Two modes, picked by a binary flag:
 *   - clamp     (default): values outside [min, max] are pinned to the
 *                edge — right for bounded series like CPU% (0..100).
 *   - autoscale (flag on) : the range is recomputed from the current
 *                data each repaint — right for an unbounded series like
 *                a stock tape.  (Pie charts are inherently proportional
 *                and ignore range/autoscale entirely.)
 */

#ifndef MAINDOB_DOBUITOOLS_GRAPH_H
#define MAINDOB_DOBUITOOLS_GRAPH_H

#include <dob/types.h>
#include "dobui_common.h"
#include "dobui_theme.h"

/* Shared font metrics (the system 8x16 bitmap font). */
#define DOBGRAPH_FONT_W     8
#define DOBGRAPH_FONT_H     16

/* ======================================================================
 *  Line graph — scrolling "sali-scendi" strip chart
 * ====================================================================== */

/* Max sample slots a line graph can hold.  A wider strip than this would
 * have sub-pixel spacing anyway; bump the macro if a use case needs more
 * (the struct grows by 4 bytes per extra slot). */
#define DOBLG_MAX_SAMPLES   256

#define DOBLG_COL_BG        DOBUI_INSET      /* dark blue panel  */
#define DOBLG_COL_LINE      DOBUI_INPUT      /* yellow trace     */
#define DOBLG_COL_BORDER    DOBUI_RELIEF

typedef struct
{
    uint32_t    win_id;
    int         x, y;
    int         w, h;

    int         n;                          /* resolution: active slots (<= MAX) */
    int         count;                      /* valid samples so far (fill from right) */
    int         samples[DOBLG_MAX_SAMPLES]; /* display order: [n-1] = newest (right) */

    int         range_min;
    int         range_max;
    bool        autoscale;

    bool        visible;
    bool        show_border;

    uint32_t    col_bg;
    uint32_t    col_line;
    uint32_t    col_border;
} dob_linegraph_t;

/* Init a strip chart at (x,y) sized (w,h) with `resolution` sample slots
 * (clamped to DOBLG_MAX_SAMPLES) and the Y value range [range_min,
 * range_max].  Starts empty; samples appear from the right as they are
 * pushed (the task-manager look). */
void doblg_Init(dob_linegraph_t *g, uint32_t win_id,
                int x, int y, int w, int h,
                int resolution, int range_min, int range_max,
                uint32_t bg_color, uint32_t line_color);

void doblg_SetAutoscale(dob_linegraph_t *g, bool on);
void doblg_SetRange(dob_linegraph_t *g, int min, int max);

/* Push one new sample at the right edge; everything scrolls one slot to
 * the left, the oldest sample falls off.  This is the real-time path. */
void doblg_UpdateLast(dob_linegraph_t *g, int value);

/* Replace the whole series with `count` values (the last DOBLG_MAX or
 * `n`, whichever is smaller, are kept), right-aligned/newest-last. */
void doblg_UpdateAll(dob_linegraph_t *g, const int *values, int count);

/* Overwrite the sample at display column `index` (0 = leftmost slot,
 * resolution-1 = rightmost).  Treats the strip as full. */
void doblg_Update(dob_linegraph_t *g, int index, int value);

/* Full repaint from current state into the command buffer. */
void doblg_Draw(dob_linegraph_t *g);

/* ======================================================================
 *  Bar graph — vertical or horizontal bars (same logic, one control)
 * ====================================================================== */

#define DOBBG_MAX_BARS      64
#define DOBBG_DEFAULT_GAP   2               /* px between adjacent bars   */

#define DOBBG_COL_BG        DOBUI_INSET
#define DOBBG_COL_FILL      DOBUI_SUCCESS      /* default bar colour         */

typedef enum
{
    DOB_BAR_VERTICAL = 0,                   /* bars grow up from the bottom */
    DOB_BAR_HORIZONTAL                      /* bars grow right from the left */
} dob_bar_orient_t;

typedef struct
{
    uint32_t        win_id;
    int             x, y;
    int             w, h;

    int             n_bars;                 /* active bars (<= MAX) */
    int             values[DOBBG_MAX_BARS];
    uint32_t        colors[DOBBG_MAX_BARS];

    dob_bar_orient_t orient;
    int             gap;                    /* px between bars */

    int             range_min;
    int             range_max;
    bool            autoscale;              /* on: top = tallest current value */

    bool            visible;

    uint32_t        col_bg;
} dob_bargraph_t;

/* Init `n_bars` bars (clamped to DOBBG_MAX_BARS) with the given
 * orientation and range.  bar_colors may be NULL (all bars take a
 * default colour) or an array of n_bars colours. */
void dobbg_Init(dob_bargraph_t *g, uint32_t win_id,
                int x, int y, int w, int h,
                int n_bars, dob_bar_orient_t orient,
                const uint32_t *bar_colors, uint32_t bg_color,
                int range_min, int range_max);

void dobbg_SetAutoscale(dob_bargraph_t *g, bool on);
void dobbg_SetRange(dob_bargraph_t *g, int min, int max);
void dobbg_SetBarColor(dob_bargraph_t *g, int index, uint32_t color);

void dobbg_Update(dob_bargraph_t *g, int index, int value);
void dobbg_UpdateAll(dob_bargraph_t *g, const int *values, int count);
void dobbg_Draw(dob_bargraph_t *g);

/* ======================================================================
 *  Pie chart — proportional slices with an optional legend
 * ====================================================================== */

#define DOBPIE_MAX_SLICES   32
#define DOBPIE_LABEL_MAX    24

#define DOBPIE_COL_BG          DOBUI_INSET
#define DOBPIE_COL_LEGEND_FG   DOBUI_TEXT

typedef struct
{
    uint32_t    win_id;
    int         x, y;
    int         w, h;

    int         n_slices;                   /* active slices (<= MAX) */
    int         values[DOBPIE_MAX_SLICES];  /* negatives are treated as 0 */
    uint32_t    colors[DOBPIE_MAX_SLICES];
    char        labels[DOBPIE_MAX_SLICES][DOBPIE_LABEL_MAX];

    bool        show_legend;
    bool        visible;

    uint32_t    col_bg;
    uint32_t    col_legend_fg;
} dob_piechart_t;

/* Init `n_slices` slices (clamped to DOBPIE_MAX_SLICES).  slice_colors
 * may be NULL (a built-in palette is used) or an array of n_slices
 * colours.  Labels default empty; set them with dobpie_SetLabel for the
 * legend. */
void dobpie_Init(dob_piechart_t *g, uint32_t win_id,
                 int x, int y, int w, int h,
                 int n_slices, const uint32_t *slice_colors,
                 uint32_t bg_color, bool show_legend);

void dobpie_SetLabel(dob_piechart_t *g, int index, const char *label);
void dobpie_SetSliceColor(dob_piechart_t *g, int index, uint32_t color);

void dobpie_Update(dob_piechart_t *g, int index, int value);
void dobpie_UpdateAll(dob_piechart_t *g, const int *values, int count);
void dobpie_Draw(dob_piechart_t *g);

#endif /* MAINDOB_DOBUITOOLS_GRAPH_H */
