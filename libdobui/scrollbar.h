/* DobUITools — Shared scrollbar geometry
 *
 * A single source of truth for thumb geometry, hit-testing and the
 * cursor -> scroll mapping used by every widget that draws a thumbed
 * scrollbar (dropdown, listview, checked_listview, table, grid).
 *
 * Historically each widget computed the thumb rectangle inline in its
 * draw routine and offered no way to grab it: scrolling was wheel /
 * keys only. Centralising the math here lets the same numbers drive
 * BOTH the rendered thumb and the drag hit-test, so the two can never
 * drift apart, and every widget gains click-and-drag for free.
 *
 * The primitive is one-dimensional and axis-agnostic. For a vertical
 * bar pass the Y axis (off = track top, len = track height, p = mouse
 * y); for a horizontal bar pass the X axis. The cross-axis bound
 * (e.g. "is x inside the vertical bar's column") is the caller's job —
 * it already knows the bar's thickness.
 *
 * `total`   : number of logical items / lines / columns
 * `visible` : how many are shown at once
 * `scroll`  : index of the first visible one (0 .. max_scroll)
 */

#ifndef MAINDOB_DOBUITOOLS_SCROLLBAR_H
#define MAINDOB_DOBUITOOLS_SCROLLBAR_H

#include <dob/types.h>

/* Minimum thumb length in pixels — matches the historical `< 8` clamp
 * every widget used, kept in one place so a future restyle is global. */
#define DOB_SCROLL_MIN_THUMB    8

typedef struct
{
    int off;         /* Track start along the axis (px) */
    int len;         /* Track length along the axis (px) */
    int thumb_off;   /* Thumb start along the axis (px) */
    int thumb_len;   /* Thumb length along the axis (px) */
    int max_scroll;  /* Largest legal value of `scroll` (>= 0) */
} dob_scroll1d_t;

/* Derive thumb geometry from list state. Mirrors the arithmetic the
 * widgets previously inlined in their draw code:
 *   thumb_len = len * visible / total   (floored, min DOB_SCROLL_MIN_THUMB)
 *   thumb_off = off + (len - thumb_len) * scroll / max_scroll */
static inline dob_scroll1d_t
dob_scroll1d(int off, int len, int total, int visible, int scroll)
{
    dob_scroll1d_t g;
    g.off = off;
    g.len = len;

    int max_s = total - visible;
    if (max_s < 0) max_s = 0;
    g.max_scroll = max_s;

    int tl = (total > 0) ? (len * visible / total) : len;
    if (tl < DOB_SCROLL_MIN_THUMB) tl = DOB_SCROLL_MIN_THUMB;
    if (tl > len)                  tl = len;
    g.thumb_len = tl;

    int range = len - tl;
    int denom = (max_s > 0) ? max_s : 1;

    if (scroll < 0)      scroll = 0;
    if (scroll > max_s)  scroll = max_s;

    g.thumb_off = off + ((range > 0) ? (range * scroll / denom) : 0);
    return g;
}

/* True if position `p` (along the axis) falls on the thumb. */
static inline bool dob_scroll1d_hit_thumb(const dob_scroll1d_t *g, int p)
{
    return p >= g->thumb_off && p < g->thumb_off + g->thumb_len;
}

/* True if `p` falls anywhere on the track (thumb included). */
static inline bool dob_scroll1d_hit_track(const dob_scroll1d_t *g, int p)
{
    return p >= g->off && p < g->off + g->len;
}

/* Map a cursor position `p` to a scroll value while dragging. `grab`
 * is the offset of the cursor within the thumb captured at press time
 * (p_press - thumb_off), so the point under the cursor stays put. The
 * result is clamped to [0, max_scroll] and rounded to nearest. */
static inline int dob_scroll1d_from_pos(const dob_scroll1d_t *g,
                                        int p, int grab)
{
    int range = g->len - g->thumb_len;
    if (range <= 0 || g->max_scroll <= 0) return 0;

    int top = p - grab - g->off;          /* desired thumb start in track */
    if (top < 0)     top = 0;
    if (top > range) top = range;

    int s = (top * g->max_scroll + range / 2) / range;   /* round */
    if (s < 0)              s = 0;
    if (s > g->max_scroll)  s = g->max_scroll;
    return s;
}

#endif /* MAINDOB_DOBUITOOLS_SCROLLBAR_H */
