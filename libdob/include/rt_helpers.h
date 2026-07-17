#ifndef MAINDOB_RT_HELPERS_H
#define MAINDOB_RT_HELPERS_H

/* rt_helpers.h — Inline utility functions for real-time control loops.
 *
 * All functions are static inline — zero overhead, no IPC,
 * compiled directly into the driver. Include and use.
 *
 * Usage:
 *   #include <rt_helpers.h>
 *   int output = rt_pid_step(&pid_state, setpoint, measured, dt_us);
 *   output = rt_clamp(output, 0, 1000);
 */

#include <DobRT.h>

/* Timing helpers */

/* Busy-wait for N microseconds (TSC-based in kernel).
 * For sub-ms hardware timing in drivers only.
 * General programs should use sleep_ms() which yields to the scheduler. */
static inline void dob_sleep_us(uint32_t us)
{
    syscall1(SYS_SLEEP_US, (int)us);
}

/* Microsecond delta handling unsigned wraparound correctly */
static inline uint32_t rt_elapsed_us(uint32_t start)
{
    return dob_clock_us() - start;  /* Works with wraparound */
}

/* Value clamping and filtering */

static inline int rt_clamp(int value, int min, int max)
{
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

/* Dead band: if value is within ±band of center, snap to center.
 * Eliminates noise jitter around a setpoint. */
static inline int rt_deadband(int value, int center, int band)
{
    int diff = value - center;
    if (diff < 0) diff = -diff;
    return (diff <= band) ? center : value;
}

/* Rate limiter: output cannot change by more than max_delta per step.
 * Protects actuators from sudden command changes. */
static inline int rt_rate_limit(int current, int previous, int max_delta)
{
    int delta = current - previous;
    if (delta > max_delta) return previous + max_delta;
    if (delta < -max_delta) return previous - max_delta;
    return current;
}

/* PID Controller (integer fixed-point) */

/* Fixed-point scale: multiply by 1000 for 3 decimal places.
 * Kp=1500 means Kp=1.500. This avoids floating point entirely. */
#define RT_PID_SCALE    1000

typedef struct
{
    int kp;             /* Proportional gain × RT_PID_SCALE */
    int ki;             /* Integral gain × RT_PID_SCALE */
    int kd;             /* Derivative gain × RT_PID_SCALE */
    int integral;       /* Accumulated integral (scaled) */
    int integral_min;   /* Anti-windup lower bound */
    int integral_max;   /* Anti-windup upper bound */
    int prev_error;     /* Previous error for derivative */
    int output_min;     /* Output clamp lower */
    int output_max;     /* Output clamp upper */
} rt_pid_t;

static inline void rt_pid_init(rt_pid_t *pid, int kp, int ki, int kd,
                               int out_min, int out_max)
{
    pid->kp = kp;
    pid->ki = ki;
    pid->kd = kd;
    pid->integral = 0;
    pid->integral_min = out_min * RT_PID_SCALE;
    pid->integral_max = out_max * RT_PID_SCALE;
    pid->prev_error = 0;
    pid->output_min = out_min;
    pid->output_max = out_max;
}

static inline void rt_pid_reset(rt_pid_t *pid)
{
    pid->integral = 0;
    pid->prev_error = 0;
}

/* One PID step. dt_us = time delta in microseconds.
 * Returns clamped output value. */
static inline int rt_pid_step(rt_pid_t *pid, int setpoint, int measured,
                              uint32_t dt_us)
{
    int error = setpoint - measured;

    /* Integral with anti-windup */
    pid->integral += error * (int)dt_us / 1000;
    if (pid->integral > pid->integral_max)
        pid->integral = pid->integral_max;
    if (pid->integral < pid->integral_min)
        pid->integral = pid->integral_min;

    /* Derivative (avoid division by zero) */
    int derivative = 0;
    if (dt_us > 0)
        derivative = (error - pid->prev_error) * 1000 / (int)dt_us;
    pid->prev_error = error;

    /* PID output (all terms already scaled by RT_PID_SCALE) */
    int output = (pid->kp * error + pid->ki * pid->integral +
                  pid->kd * derivative) / RT_PID_SCALE;

    return rt_clamp(output, pid->output_min, pid->output_max);
}

/* Pattern matching (local, no syscall) */

/* Check if a flag pattern matches — same logic as kernel event_group.
 * Use for local polling without a syscall:
 *   uint32_t flags = dob_event_get_flags(gid);
 *   if (rt_pattern_match(flags, CRIT_BITS, RT_ANY_CLEAR)) { stop(); }
 */
static inline bool rt_pattern_match(uint32_t flags, uint32_t pattern, int mode)
{
    switch (mode)
    {
        case RT_ALL_SET:    return (flags & pattern) == pattern;
        case RT_ANY_SET:    return (flags & pattern) != 0;
        case RT_ALL_CLEAR:  return (flags & pattern) == 0;
        case RT_ANY_CLEAR:  return (flags & pattern) != pattern;
        default:            return false;
    }
}

#endif /* MAINDOB_RT_HELPERS_H */
