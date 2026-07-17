/* MainDOB Window Servo — Pure Geometry Engine
 *
 * Zero allocations. Zero IPC. Zero side effects.
 * Every function is pure integer arithmetic on input coordinates.
 */

#include <dob/window_servo.h>
#include <string.h>

/* Internals */

static int servo_clamp(int val, int lo, int hi)
{
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

static int servo_detect_edge(servo_t *s, int mx, int my)
{
    int grab = s->grab_zone;
    int wx0 = s->origin_win_x;
    int wy0 = s->origin_win_y;
    int wx1 = wx0 + s->origin_w;
    int wy1 = wy0 + s->origin_h + s->header_h;

    bool near_l = (mx < wx0 + grab);
    bool near_r = (mx >= wx1 - grab);
    bool near_t = (my < wy0 + grab);
    bool near_b = (my >= wy1 - grab);

    if (near_t && near_l) return SERVO_EDGE_TL;
    if (near_t && near_r) return SERVO_EDGE_TR;
    if (near_b && near_l) return SERVO_EDGE_BL;
    if (near_b && near_r) return SERVO_EDGE_BR;
    if (near_l)           return SERVO_EDGE_LEFT;
    if (near_r)           return SERVO_EDGE_RIGHT;
    if (near_t)           return SERVO_EDGE_TOP;
    if (near_b)           return SERVO_EDGE_BOTTOM;

    return SERVO_EDGE_BR;
}

/* Core geometry calculation.
 * Given the edge and mouse position, compute new x, y, w, h.
 * Applies min_size and desktop bounds clamping. */
static void servo_compute(servo_t *s, int mx, int my)
{
    int fix_x = s->origin_win_x;
    int fix_r = s->origin_win_x + s->origin_w;
    int fix_y = s->origin_win_y;
    int fix_b = s->origin_win_y + s->origin_h + s->header_h;

    int new_w = s->origin_w;
    int new_h = s->origin_h;
    int new_x = s->origin_win_x;
    int new_y = s->origin_win_y;

    switch (s->edge)
    {
        case SERVO_EDGE_RIGHT:
            new_w = mx - fix_x;
            break;

        case SERVO_EDGE_LEFT:
            new_w = fix_r - mx;
            new_x = mx;
            break;

        case SERVO_EDGE_BOTTOM:
            new_h = my - fix_y - s->header_h;
            break;

        case SERVO_EDGE_TOP:
            new_h = fix_b - my - s->header_h;
            new_y = my;
            break;

        case SERVO_EDGE_BR:
            new_w = mx - fix_x;
            new_h = my - fix_y - s->header_h;
            break;

        case SERVO_EDGE_BL:
            new_w = fix_r - mx;
            new_h = my - fix_y - s->header_h;
            new_x = fix_r - new_w;
            break;

        case SERVO_EDGE_TR:
            new_w = mx - fix_x;
            new_h = fix_b - my - s->header_h;
            new_y = fix_b - new_h - s->header_h;
            break;

        case SERVO_EDGE_TL:
            new_w = fix_r - mx;
            new_h = fix_b - my - s->header_h;
            new_x = fix_r - new_w;
            new_y = fix_b - new_h - s->header_h;
            break;
    }

    /* Clamp dimensions */
    if (new_w < s->min_size) new_w = s->min_size;
    if (new_h < s->min_size) new_h = s->min_size;
    if (new_w > s->desktop_w) new_w = s->desktop_w;
    if (new_h > s->desktop_h - s->header_h) new_h = s->desktop_h - s->header_h;

    /* Recalculate position after clamping for left/top edges */
    switch (s->edge)
    {
        case SERVO_EDGE_LEFT:
        case SERVO_EDGE_TL:
        case SERVO_EDGE_BL:
            new_x = fix_r - new_w;
            break;
    }

    switch (s->edge)
    {
        case SERVO_EDGE_TOP:
        case SERVO_EDGE_TL:
        case SERVO_EDGE_TR:
            new_y = fix_b - new_h - s->header_h;
            break;
    }

    /* Clamp position to desktop bounds */
    new_x = servo_clamp(new_x, 0, s->desktop_w - new_w);
    new_y = servo_clamp(new_y, 0, s->desktop_h - new_h - s->header_h);

    s->target_x = new_x;
    s->target_y = new_y;
    s->target_w = new_w;
    s->target_h = new_h;
}

static void servo_fill_result(servo_t *s, servo_result_t *out)
{
    out->x = s->target_x;
    out->y = s->target_y;
    out->w = s->target_w;
    out->h = s->target_h;
}

/* Public API */

void servo_init(servo_t *s, int min_size, int header_h, int grab_zone)
{
    memset(s, 0, sizeof(*s));
    s->min_size = min_size;
    s->header_h = header_h;
    s->grab_zone = grab_zone;
}

int servo_begin(servo_t *s,
                int win_x, int win_y, int win_w, int win_h,
                int mouse_x, int mouse_y,
                int desktop_w, int desktop_h)
{
    s->active = true;

    s->origin_win_x = win_x;
    s->origin_win_y = win_y;
    s->origin_w = win_w;
    s->origin_h = win_h;

    s->desktop_w = desktop_w;
    s->desktop_h = desktop_h;

    /* Start target at current geometry */
    s->target_x = win_x;
    s->target_y = win_y;
    s->target_w = win_w;
    s->target_h = win_h;

    s->edge = servo_detect_edge(s, mouse_x, mouse_y);
    return s->edge;
}

void servo_update(servo_t *s, int mouse_x, int mouse_y, servo_result_t *out)
{
    if (!s->active)
    {
        out->x = 0;
        out->y = 0;
        out->w = 0;
        out->h = 0;
        return;
    }

    servo_compute(s, mouse_x, mouse_y);
    servo_fill_result(s, out);
}

void servo_commit(servo_t *s, servo_result_t *out)
{
    servo_fill_result(s, out);
    s->active = false;
}

bool servo_is_active(const servo_t *s)
{
    return s->active;
}

void servo_maximize(servo_t *s,
                    int win_x, int win_y, int win_w, int win_h,
                    int desktop_w, int desktop_h,
                    servo_result_t *out)
{
    /* Save current geometry for restore */
    s->has_prev = true;
    s->prev_x = win_x;
    s->prev_y = win_y;
    s->prev_w = win_w;
    s->prev_h = win_h;

    /* Fullscreen geometry */
    out->x = 0;
    out->y = 0;
    out->w = desktop_w;
    out->h = desktop_h - s->header_h;
}

bool servo_restore(servo_t *s, servo_result_t *out)
{
    if (!s->has_prev)
        return false;

    out->x = s->prev_x;
    out->y = s->prev_y;
    out->w = s->prev_w;
    out->h = s->prev_h;

    s->has_prev = false;
    return true;
}

void servo_clear_prev(servo_t *s)
{
    s->has_prev = false;
}
