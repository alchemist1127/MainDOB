#ifndef MAINDOB_STUBS_DOBVIDEOCONTROL_H
#define MAINDOB_STUBS_DOBVIDEOCONTROL_H

/* DobVideoControl — control plane of any dobVideo driver.
 *
 * Companion to <dob/video.h>.  Where <dob/video.h> is the data plane
 * (fast-path, kernel-style flat surface for processes that hold a
 * vprocess), this header is the traditional IPC stub for the control
 * plane: callable by anyone, including processes that do NOT hold a
 * vprocess (Settings, Calculator, a third-party tool).
 *
 * Scope: every operation that changes system-global display state.
 *   - Mode setting and listing
 *   - Multi-display arrangement (positions, primary, enable/disable)
 *   - Gamma and color-correction
 *   - Scanout source routing per display
 *   - Power state
 *   - GPU reset
 *   - Subscription to control-plane events (so dobinterface and
 *     others can react to changes)
 *
 * Not in scope: any per-vprocess resource (surface, texture, draw
 * call, compute dispatch).  Those live exclusively in the data plane.
 *
 * Service name in the registry: "DobVideoControl".  The driver
 * registers under this name in addition to whatever data-plane
 * registration it performs.  The two paths share the underlying
 * driver process; only the IPC interface differs.
 *
 * Read parity.  A few read-only queries are duplicated between this
 * header and <dob/video.h>: a vprocess can ask the same question via
 * its fast-path, a vprocess-less client asks here.  The control plane
 * is always authoritative; the data plane is a cheap mirror.
 *
 * Usage:
 *   #include <DobVideoControl.h>
 *
 *   dobvc_mode_t modes[16];
 *   uint32_t n = 16;
 *   dobvc_ModeList(0, modes, &n);
 *
 *   dobvc_mode_t target = { .width = 1920, .height = 1080,
 *                           .refresh_hz = 60, .format = DV_FMT_RGBA8888 };
 *   if (dobvc_ModeSet(0, &target) == 0) { ... }
 *
 *   dobvc_Subscribe(my_port, DOBVC_EVENT_MODE_CHANGED |
 *                            DOBVC_EVENT_DISPLAY_HOTPLUG);
 */

#include <dob/types.h>
#include <dob/video.h>      /* for dv_format_t, dv_mode_t, dv_display_info_t,
                             * dv_gamma_ramp_t, dv_power_state_t mirroring */

/* --------------------------------------------------------------------------
 *  Mirrored read types — kept identical to <dob/video.h> so values can
 *  be passed without translation between the two planes.
 * -------------------------------------------------------------------------- */

typedef dv_mode_t            dobvc_mode_t;
typedef dv_display_info_t    dobvc_display_info_t;
typedef dv_gamma_ramp_t      dobvc_gamma_ramp_t;

/* --------------------------------------------------------------------------
 *  Multi-display arrangement
 *
 *  Each display has a logical position in a virtual desktop coordinate
 *  space.  dobvc_DisplayArrange takes an array describing every
 *  display: where it sits, whether it is enabled, whether it is the
 *  primary.  Layouts that overlap or leave gaps are accepted; the
 *  driver does not impose a topology.  Coordinates are in display
 *  pixels.
 * -------------------------------------------------------------------------- */

typedef struct
{
    uint32_t display_id;
    int32_t  origin_x;        /* top-left of this display in virtual desktop */
    int32_t  origin_y;
    bool     enabled;         /* false = blanked / disconnected from desktop */
    bool     primary;         /* exactly one display in the array must be true */
    uint32_t rotation_deg;    /* 0, 90, 180, 270; ignored if not supported */
} dobvc_display_arrangement_t;

/* --------------------------------------------------------------------------
 *  Power state — mirror of dv_power_state_t for the same reason
 *  (cross-plane parity).
 * -------------------------------------------------------------------------- */

typedef enum
{
    DOBVC_POWER_LOW       = 0,
    DOBVC_POWER_BALANCED  = 1,
    DOBVC_POWER_HIGH      = 2,
} dobvc_power_state_t;

/* --------------------------------------------------------------------------
 *  Event subscription — clients ask the driver to post events to a
 *  registry-known port.  The driver delivers a dobvc_event_t per event.
 * -------------------------------------------------------------------------- */

#define DOBVC_EVENT_MODE_CHANGED       (1u << 0)   /* a display's mode was changed */
#define DOBVC_EVENT_DISPLAY_HOTPLUG    (1u << 1)   /* monitor connect / disconnect */
#define DOBVC_EVENT_ARRANGEMENT_CHANGED (1u << 2)  /* multi-display layout edited */
#define DOBVC_EVENT_GAMMA_CHANGED      (1u << 3)
#define DOBVC_EVENT_POWER_CHANGED      (1u << 4)
#define DOBVC_EVENT_GPU_RESET          (1u << 5)

typedef struct
{
    uint32_t code;            /* one of DOBVC_EVENT_* (single bit) */
    uint32_t display_id;      /* relevant display, or 0 if not display-bound */
    uint32_t arg0;            /* code-specific */
    uint32_t arg1;
} dobvc_event_t;

/* ==========================================================================
 *  Control-plane API
 *  All functions return 0 on success or a negative error compatible
 *  with <dob/video.h>'s DV_ERR_*.  Calls go via traditional IPC to the
 *  driver's "DobVideoControl" service.
 * ========================================================================== */

/* ----- Mode: list / get / set ----- */

/* List the modes the given display supports.  out is filled with up
 * to *count entries; on return *count holds the number actually
 * written, which may be smaller than the input. */
int dobvc_ModeList(uint32_t display_id, dobvc_mode_t *out, uint32_t *count);

/* Get the currently active mode for the given display. */
int dobvc_ModeGet(uint32_t display_id, dobvc_mode_t *out);

/* Set the active mode.  Driver applies it on the next vsync; callers
 * subscribed to DOBVC_EVENT_MODE_CHANGED receive the event after the
 * switch is committed.  Failures: DV_ERR_NOSUPPORT (mode not
 * supported), DV_ERR_BUSY (another switch in flight), DV_ERR_INVAL.
 *
 * Note: changing a display's mode invalidates all surfaces marked
 * SCANOUT for that display.  The driver re-scans on the new mode and
 * notifies subscribers. */
int dobvc_ModeSet(uint32_t display_id, const dobvc_mode_t *mode);

/* ----- Display: enumerate ----- */

/* Number of displays the driver currently knows about (connected or
 * disconnected; check display_info.connected for state). */
int dobvc_DisplayCount(uint32_t *out_count);

/* Per-display info: physical size, name, EDID presence, connected
 * state, current arrangement entry. */
int dobvc_DisplayInfo(uint32_t display_id, dobvc_display_info_t *out);

/* Optional: retrieve the raw EDID block for a display (max 256 bytes;
 * common case is 128).  Returns the byte count actually written, or
 * a negative error.  Drivers without EDID return DV_ERR_NOSUPPORT. */
int dobvc_DisplayEDID(uint32_t display_id, void *buf, uint32_t buf_size);

/* ----- Multi-display arrangement ----- */

/* Get the current arrangement.  *count is in/out as in ModeList. */
int dobvc_ArrangementGet(dobvc_display_arrangement_t *out, uint32_t *count);

/* Apply a new arrangement.  The array describes every display.
 * Exactly one entry must have primary=true.  Disabling all displays
 * is rejected (DV_ERR_INVAL) — must keep at least one active. */
int dobvc_ArrangementSet(const dobvc_display_arrangement_t *arr, uint32_t count);

/* ----- Gamma / palette ----- */

int dobvc_GammaGet(uint32_t display_id, dobvc_gamma_ramp_t *out);
int dobvc_GammaSet(uint32_t display_id, const dobvc_gamma_ramp_t *r);

/* Reset the gamma ramp of the given display to its boot identity
 * values.  Useful as the "OK" handler of a gamma-calibration UI when
 * the user backs out. */
int dobvc_GammaReset(uint32_t display_id);

/* ----- Scanout routing ----- */

/* Which surface (or none) is the active scanout source for the given
 * display.  Setting DV_HANDLE_NONE blanks the display.  The surface
 * must belong to a vprocess that still exists; passing a stale handle
 * fails with DV_ERR_HANDLE.
 *
 * In practice dobinterface holds the scanout source for the desktop
 * and only changes it when the desktop surface itself is replaced.
 * Other clients normally do not call this. */
int dobvc_ScanoutGet(uint32_t display_id, uint32_t *out_surface_handle);
int dobvc_ScanoutSet(uint32_t display_id, uint32_t surface_handle,
                     int32_t  x_offset, int32_t y_offset);

/* ----- Power state and reset ----- */

int dobvc_PowerStateGet(dobvc_power_state_t *out);
int dobvc_PowerStateSet(dobvc_power_state_t st);

/* Reset the GPU.  scope=0 → driver-wide reset (loses all vprocess
 * state, all clients receive DV_EVENT_GPU_RESET via their own data
 * plane).  scope>0 → reset a specific vprocess by handle, preserving
 * the rest.  Only privileged callers may reset driver-wide; per-vproc
 * reset is allowed for the owning process or its parent. */
int dobvc_GPUReset(uint32_t scope);

/* ----- Subscription to events ----- */

/* Subscribe `port` (a registry IPC port belonging to the caller) to
 * the OR-mask of DOBVC_EVENT_* codes.  Subsequent matching events are
 * delivered as dobvc_event_t messages on that port.  Re-calling with
 * a different mask replaces the previous subscription. */
int dobvc_Subscribe(uint32_t port, uint32_t event_mask);

/* Cancel any subscription this caller has. */
int dobvc_Unsubscribe(void);

/* ----- Driver identity ----- */

/* Mirror of dv_driver_info — useful for Settings to display
 * "Adapter: VirtIO GPU 1.4 (vendor 1AF4 device 1050)". */
int dobvc_DriverInfo(dv_driver_info_t *out);

/* ----- Capability / vcore introspection -----
 *
 * Cold-path mirror of the dv_cap_* / dv_vcore_* read-only data-plane
 * queries, for clients that do not hold a vprocess.  A vprocess uses
 * its fast-path; everyone else asks here. */

/* OR-mask of DV_CAP_* the driver supports. */
int dobvc_CapQuery(uint64_t *out_capabilities);

/* Value of one DV_LIMIT_* limit (pass a dv_limit_t value in `which`). */
int dobvc_CapQueryLimit(uint32_t which, uint64_t *out_value);

/* DV_FMT_USE_* role mask for a given dv_format_t. */
int dobvc_CapQueryFormat(uint32_t fmt, uint32_t *out_usage_flags);

/* Number of vcores the driver exposes (0 on a plain framebuffer). */
int dobvc_VcoreCount(uint32_t *out_count);

/* Per-vcore descriptor for vcore `index`. */
int dobvc_VcoreInfo(uint32_t index, dv_vcore_info_t *out);

#endif /* MAINDOB_STUBS_DOBVIDEOCONTROL_H */
