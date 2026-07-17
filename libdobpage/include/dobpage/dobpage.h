/* libdobpage -- the page-object engine.
 *
 * This is the "engine" half of DobWrite's view: it turns the layout's one
 * continuous content column into SHEET OBJECTS and draws each sheet's
 * contents -- paper, margins, text, colours -- INTO that sheet's own
 * surface. A sheet is the unit of rendering and of memory.
 *
 * Design (sized for real hardware: Armada E500 / Extensa-class):
 *   - RAM surfaces, no GPU: each live sheet owns a page-sized 0x00RRGGBB
 *     buffer drawn by the CPU and blitted by the app. The video card only
 *     does the final blit.
 *   - Only the visible sheets are live. Buffers come from a small pool and
 *     are recycled as sheets scroll off, so memory is O(visible pages),
 *     not O(document).
 *   - Incremental: dp_notify_edit re-paginates (a cheap walk) and, for the
 *     common case (a single paragraph whose height didn't change), repaints
 *     only the dirty strip of the affected sheet -- the rest of the sheet's
 *     surface is left untouched. Glyphs are rasterized once and cached.
 *
 * Caret and selection are NOT drawn here -- they are transient UI the app
 * paints as an overlay on top of the sheet surfaces. This engine owns the
 * content only. Three coordinate spaces meet here:
 *   - content space: the layout's continuous column (owned by libdoblayout)
 *   - window space:  pixels in the app's window (sheets stacked with gaps,
 *                    centred, offset by the scroll)
 * The engine maps between them so the app can place the overlay and resolve
 * clicks without knowing the pagination.
 */

#ifndef DOBPAGE_DOBPAGE_H
#define DOBPAGE_DOBPAGE_H

#include <doblayout/doblayout.h>

typedef struct dp_engine dp_engine;

/* Create an engine over a (persistent) layout. Pooled surfaces are sized to
 * the layout's page. The layout must outlive the engine. */
dp_engine *dp_create(const df_layout *L);
void       dp_destroy(dp_engine *e);

/* Re-read page geometry and re-paginate (after a full layout rebuild or a
 * page-size change). Resizes the surface pool if the page size changed. */
void dp_relayout(dp_engine *e);

/* The document changed: `u` is the report from df_layout_reflow. Re-paginates
 * and invalidates / strip-repaints the affected sheet surfaces. */
void dp_notify_edit(dp_engine *e, const dl_update *u);

/* Set the paper background colour (opaque 0xFFRRGGBB). Re-renders pages. */
void dp_set_page_bg(dp_engine *e, uint32_t color);

/* Viewport + scrolling (window pixels; scroll is in the stacked "desk"). */
void dp_set_viewport(dp_engine *e, int win_w, int win_h);
void dp_set_scroll(dp_engine *e, int scroll);
void dp_scroll_by(dp_engine *e, int dy);
int  dp_scroll(const dp_engine *e);
int  dp_scroll_max(const dp_engine *e);
/* Adjust scroll so the content-space vertical span is visible (caret-into-view). */
void dp_scroll_to_content(dp_engine *e, float cy_top, float cy_bottom);

/* horizontal pan (for pages wider than the viewport, e.g. when zoomed in) */
int  dp_scroll_x(const dp_engine *e);
int  dp_scroll_x_max(const dp_engine *e);
void dp_set_scroll_x(dp_engine *e, int scroll_x);
void dp_scroll_x_by(dp_engine *e, int dx);
void dp_scroll_to_content_x(dp_engine *e, float cx_left, float cx_right, float cy);

uint32_t dp_page_count(const dp_engine *e);
int      dp_page_w(const dp_engine *e);
int      dp_page_h(const dp_engine *e);

/* Render the currently visible sheets into pooled surfaces and report them.
 * Returns the number written to `out` (at most `max`). Each entry gives the
 * sheet's 0x00RRGGBB buffer and where to blit it in the window. */
typedef struct { int page_index; const uint32_t *buf; int w, h; int win_x, win_y; } dp_view;
int dp_visible_pages(dp_engine *e, dp_view *out, int max);

/* Coordinate mapping for the app's overlay (caret/selection) and for clicks. */
void dp_content_to_window(const dp_engine *e, float cx, float cy, int *wx, int *wy);
void dp_window_to_content(const dp_engine *e, int wx, int wy, float *cx, float *cy);

#endif /* DOBPAGE_DOBPAGE_H */
