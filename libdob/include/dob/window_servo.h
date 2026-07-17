#ifndef MAINDOB_DOB_WINDOW_SERVO_H
#define MAINDOB_DOB_WINDOW_SERVO_H

/* MainDOB Window Servo — Pure Geometry Engine
 *
 * Performance-critical window resize and maximize calculations.
 * Zero allocations, zero IPC, zero side effects.
 * Pure integer arithmetic on input coordinates.
 *
 * Usage:
 *   servo_t servo;
 *   servo_result_t result;
 *
 *   servo_begin(&servo, ...);       // start resize drag
 *   servo_update(&servo, mx, my, &result);  // each mouse tick
 *   servo_commit(&servo, &result);  // mouse released — apply
 *
 *   servo_maximize(&servo, ...);    // instant maximize
 *   servo_restore(&servo, &result); // restore from maximize
 */

#include <sys/types.h>

/* Edge identifiers */
#define SERVO_EDGE_LEFT     1
#define SERVO_EDGE_RIGHT    2
#define SERVO_EDGE_TOP      3
#define SERVO_EDGE_BOTTOM   4
#define SERVO_EDGE_TL       5
#define SERVO_EDGE_TR       6
#define SERVO_EDGE_BL       7
#define SERVO_EDGE_BR       8

/* Result of any servo operation */
typedef struct
{
    int x, y, w, h;
} servo_result_t;

/* Servo session state — one per compositor instance */
typedef struct
{
    /* Session active? */
    bool active;

    /* Detected edge for current resize */
    int edge;

    /* Snapshot at drag start */
    int origin_win_x, origin_win_y;
    int origin_w, origin_h;

    /* Desktop bounds */
    int desktop_w, desktop_h;

    /* Constraints */
    int min_size;
    int header_h;
    int grab_zone;

    /* Current target (updated each tick) */
    int target_x, target_y;
    int target_w, target_h;

    /* Pre-maximize snapshot for restore */
    bool has_prev;
    int prev_x, prev_y;
    int prev_w, prev_h;
} servo_t;

/* Initialize servo with environment constraints.
 * Call once at startup. */
void servo_init(servo_t *s, int min_size, int header_h, int grab_zone);

/* Begin a resize drag session.
 * Detects which edge/corner the mouse is on.
 * Returns the detected edge (SERVO_EDGE_*). */
int servo_begin(servo_t *s,
                int win_x, int win_y, int win_w, int win_h,
                int mouse_x, int mouse_y,
                int desktop_w, int desktop_h);

/* Update target geometry during drag.
 * Pure arithmetic — no allocations, no side effects.
 * Writes result to *out. */
void servo_update(servo_t *s, int mouse_x, int mouse_y, servo_result_t *out);

/* Finalize the resize: returns the definitive geometry.
 * Deactivates the session. */
void servo_commit(servo_t *s, servo_result_t *out);

/* Is a resize session active? */
bool servo_is_active(const servo_t *s);

/* Compute maximized geometry.
 * Saves current position for later restore. */
void servo_maximize(servo_t *s,
                    int win_x, int win_y, int win_w, int win_h,
                    int desktop_w, int desktop_h,
                    servo_result_t *out);

/* Restore from maximized state.
 * Returns saved pre-maximize geometry. */
bool servo_restore(servo_t *s, servo_result_t *out);

/* Clear saved pre-maximize state (e.g. after manual move). */
void servo_clear_prev(servo_t *s);

#endif /* MAINDOB_DOB_WINDOW_SERVO_H */
