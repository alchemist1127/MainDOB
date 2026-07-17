#ifndef MAINDOB_DOBRT_H
#define MAINDOB_DOBRT_H

/* DobRT — Real-Time Control API for MainDOB
 *
 * Event groups: 32-bit bitmask of conditions with combinatorial waits.
 * Direct syscalls — no server, no IPC overhead.
 *
 * Usage:
 *   #include <DobRT.h>
 *   int gid = dob_event_create();
 *   dob_event_set(gid, BIT_TEMP_OK | BIT_PRESSURE_OK);
 *   uint32_t flags;
 *   int r = dob_event_wait(gid, BIT_TEMP_OK | BIT_PRESSURE_OK,
 *                          RT_ALL_SET, 500, &flags);
 *   if (r == RT_TIMEOUT) { emergency_stop(); }
 */

#include <sys/syscall.h>

/* Wait result codes */
#define RT_OK           0
#define RT_TIMEOUT      1
#define RT_CANCELLED    2

/* Wait modes */
#define RT_ALL_SET      0   /* All specified bits must be 1 */
#define RT_ANY_SET      1   /* At least one specified bit is 1 */
#define RT_ALL_CLEAR    2   /* All specified bits must be 0 */
#define RT_ANY_CLEAR    3   /* At least one specified bit is 0 */

/* Event operations (for dob_event_op) */
#define RT_OP_SET       0
#define RT_OP_CLEAR     1
#define RT_OP_PULSE     2
#define RT_OP_POISON    3
#define RT_OP_RESET     4

/* Event Group API */

/* Create a new event group. Returns group ID (0-31) or -1. */
static inline int dob_event_create(void)
{
    return syscall0(SYS_EVENT_CREATE);
}

/* Set bits (OR). Wakes waiters matching ALL_SET or ANY_SET. */
static inline int dob_event_set(int gid, uint32_t bits)
{
    return syscall3(SYS_EVENT_SETCLEAR, gid, (int)bits, RT_OP_SET);
}

/* Clear bits (AND NOT). Wakes waiters matching ALL_CLEAR or ANY_CLEAR. */
static inline int dob_event_clear(int gid, uint32_t bits)
{
    return syscall3(SYS_EVENT_SETCLEAR, gid, (int)bits, RT_OP_CLEAR);
}

/* Pulse: set + wake + clear atomically. For transient events. */
static inline int dob_event_pulse(int gid, uint32_t bits)
{
    return syscall3(SYS_EVENT_SETCLEAR, gid, (int)bits, RT_OP_PULSE);
}

/* Poison: wake all waiters with RT_CANCELLED. Emergency stop. */
static inline int dob_event_poison(int gid)
{
    return syscall3(SYS_EVENT_SETCLEAR, gid, 0, RT_OP_POISON);
}

/* Reset: clear all flags and un-poison. */
static inline int dob_event_reset(int gid)
{
    return syscall3(SYS_EVENT_SETCLEAR, gid, 0, RT_OP_RESET);
}

/* Non-blocking read of current flags. */
static inline uint32_t dob_event_get_flags(int gid)
{
    return (uint32_t)syscall1(SYS_EVENT_GETFLAGS, gid);
}

/* Wait for pattern to match with optional timeout.
 * mode: RT_ALL_SET, RT_ANY_SET, RT_ALL_CLEAR, RT_ANY_CLEAR
 * timeout_ms: 0 = infinite wait
 * out_flags: receives flag snapshot at wakeup (can be NULL)
 * Returns: RT_OK, RT_TIMEOUT, or RT_CANCELLED */
static inline int dob_event_wait(int gid, uint32_t pattern, int mode,
                                 uint32_t timeout_ms, uint32_t *out_flags)
{
    return syscall5(SYS_EVENT_WAIT, gid, (int)pattern, mode,
                    (int)timeout_ms, (int)out_flags);
}

/* High-resolution timer API */

/* Microsecond timestamp (wraps every ~71 minutes). */
static inline uint32_t dob_clock_us(void)
{
    return (uint32_t)syscall0(SYS_CLOCK_US);
}

#endif /* MAINDOB_DOBRT_H */
