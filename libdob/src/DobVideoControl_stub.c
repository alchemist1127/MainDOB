/* DobVideoControl — client stub for the dobVideo control plane.
 *
 * Companion to <DobVideoControl.h>.  Every function here marshals its
 * arguments into a dob_msg_t, sends it via traditional IPC to the
 * driver's "DobVideoControl" service, and unmarshals the reply.
 *
 * This is the cold-path counterpart of libdob/src/video.c (the data
 * plane, reached via the int 0x85 boomerang).  Use this stub from any
 * process that does NOT hold a vprocess: Settings, diagnostic tools,
 * third-party apps.  A vprocess uses its fast-path instead.
 *
 * Wire format lives in drivers/<driver>/dobvc_protocol.h (DOBVC_OP_*),
 * mirrored across every dobVideo driver (bga, mach64, ...) and shared
 * with the driver-side IPC dispatcher (bga_transport_ipc.c,
 * mach64_transport_ipc.c, ...).  No application code includes that
 * header — only this stub and the drivers do.
 *
 * Return value convention: every function returns 0 (DV_OK) on
 * success or a negative DV_ERR_* code.  When IPC itself fails (driver
 * absent, port dead) the function returns DV_ERR_NOTREADY.
 */

#include <DobVideoControl.h>
#include <dobvc_protocol.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/reconnect.h>
#include <string.h>

/* Cached service port; (re)discovered transparently by the reconnect
 * helper if the driver bubble crashes and is relaunched. */
static uint32_t vc_port = 0;

#define IPC_CALL(m, r) _dob_call_reconnect(&vc_port, "DobVideoControl", \
                                           3000, (m), (r))

/* Send one control op, return the driver's int32 status (or
 * DV_ERR_NOTREADY if the call never reached the driver). */
static int vc_call(dob_msg_t *msg, dob_msg_t *reply)
{
    memset(reply, 0, sizeof(*reply));
    if (IPC_CALL(msg, reply) != DOB_OK)
        return DV_ERR_NOTREADY;
    return (int)(int32_t)reply->code;
}

/* Copy a reply payload into `out` iff the call succeeded and the
 * driver returned at least `size` bytes. */
static int copy_out(int rc, const dob_msg_t *reply, void *out, uint32_t size)
{
    if (rc == DV_OK)
    {
        if (!reply->payload || reply->payload_size < size)
            return DV_ERR_INVAL;
        memcpy(out, reply->payload, size);
    }
    return rc;
}

/* ===================================================================
 *  Mode: list / get / set
 * =================================================================== */

int dobvc_ModeList(uint32_t display_id, dobvc_mode_t *out, uint32_t *count)
{
    if (!out || !count) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_MODE_LIST;
    msg.arg0 = display_id;

    int rc = vc_call(&msg, &reply);
    if (rc == DV_OK)
    {
        uint32_t avail = reply.arg0;                /* modes driver wrote */
        uint32_t n = (avail < *count) ? avail : *count;
        if (reply.payload && n > 0)
            memcpy(out, reply.payload, n * sizeof(dobvc_mode_t));
        *count = n;
    }
    else
    {
        *count = 0;
    }
    return rc;
}

int dobvc_ModeGet(uint32_t display_id, dobvc_mode_t *out)
{
    if (!out) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_MODE_GET;
    msg.arg0 = display_id;

    return copy_out(vc_call(&msg, &reply), &reply, out, sizeof(*out));
}

int dobvc_ModeSet(uint32_t display_id, const dobvc_mode_t *mode)
{
    if (!mode) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code         = DOBVC_OP_MODE_SET;
    msg.arg0         = display_id;
    msg.payload      = (void *)mode;
    msg.payload_size = sizeof(*mode);

    return vc_call(&msg, &reply);
}

/* ===================================================================
 *  Display: enumerate
 * =================================================================== */

int dobvc_DisplayCount(uint32_t *out_count)
{
    if (!out_count) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_DISPLAY_COUNT;

    int rc = vc_call(&msg, &reply);
    if (rc == DV_OK) *out_count = reply.arg0;
    return rc;
}

int dobvc_DisplayInfo(uint32_t display_id, dobvc_display_info_t *out)
{
    if (!out) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_DISPLAY_INFO;
    msg.arg0 = display_id;

    return copy_out(vc_call(&msg, &reply), &reply, out, sizeof(*out));
}

int dobvc_DisplayEDID(uint32_t display_id, void *buf, uint32_t buf_size)
{
    if (!buf || buf_size == 0) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_DISPLAY_EDID;
    msg.arg0 = display_id;

    int rc = vc_call(&msg, &reply);
    if (rc != DV_OK)
        return rc;

    /* Success: copy what fits, return the byte count written. */
    uint32_t n = reply.payload_size;
    if (n > buf_size) n = buf_size;
    if (reply.payload && n > 0)
        memcpy(buf, reply.payload, n);
    return (int)n;
}

/* ===================================================================
 *  Multi-display arrangement
 * =================================================================== */

int dobvc_ArrangementGet(dobvc_display_arrangement_t *out, uint32_t *count)
{
    if (!out || !count) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_ARRANGEMENT_GET;

    int rc = vc_call(&msg, &reply);
    if (rc == DV_OK)
    {
        uint32_t avail = reply.arg0;
        uint32_t n = (avail < *count) ? avail : *count;
        if (reply.payload && n > 0)
            memcpy(out, reply.payload, n * sizeof(*out));
        *count = n;
    }
    else
    {
        *count = 0;
    }
    return rc;
}

int dobvc_ArrangementSet(const dobvc_display_arrangement_t *arr, uint32_t count)
{
    if (!arr || count == 0) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code         = DOBVC_OP_ARRANGEMENT_SET;
    msg.arg0         = count;
    msg.payload      = (void *)arr;
    msg.payload_size = count * sizeof(*arr);

    return vc_call(&msg, &reply);
}

/* ===================================================================
 *  Gamma
 * =================================================================== */

int dobvc_GammaGet(uint32_t display_id, dobvc_gamma_ramp_t *out)
{
    if (!out) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_GAMMA_GET;
    msg.arg0 = display_id;

    return copy_out(vc_call(&msg, &reply), &reply, out, sizeof(*out));
}

int dobvc_GammaSet(uint32_t display_id, const dobvc_gamma_ramp_t *r)
{
    if (!r) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code         = DOBVC_OP_GAMMA_SET;
    msg.arg0         = display_id;
    msg.payload      = (void *)r;
    msg.payload_size = sizeof(*r);

    return vc_call(&msg, &reply);
}

int dobvc_GammaReset(uint32_t display_id)
{
    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_GAMMA_RESET;
    msg.arg0 = display_id;

    return vc_call(&msg, &reply);
}

/* ===================================================================
 *  Scanout routing
 * =================================================================== */

int dobvc_ScanoutGet(uint32_t display_id, uint32_t *out_surface_handle)
{
    if (!out_surface_handle) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_SCANOUT_GET;
    msg.arg0 = display_id;

    int rc = vc_call(&msg, &reply);
    if (rc == DV_OK) *out_surface_handle = reply.arg0;
    return rc;
}

int dobvc_ScanoutSet(uint32_t display_id, uint32_t surface_handle,
                     int32_t x_offset, int32_t y_offset)
{
    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_SCANOUT_SET;
    msg.arg0 = display_id;
    msg.arg1 = surface_handle;
    msg.arg2 = (uint32_t)x_offset;
    msg.arg3 = (uint32_t)y_offset;

    return vc_call(&msg, &reply);
}

/* ===================================================================
 *  Power state and reset
 * =================================================================== */

int dobvc_PowerStateGet(dobvc_power_state_t *out)
{
    if (!out) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_POWER_GET;

    int rc = vc_call(&msg, &reply);
    if (rc == DV_OK) *out = (dobvc_power_state_t)reply.arg0;
    return rc;
}

int dobvc_PowerStateSet(dobvc_power_state_t st)
{
    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_POWER_SET;
    msg.arg0 = (uint32_t)st;

    return vc_call(&msg, &reply);
}

int dobvc_GPUReset(uint32_t scope)
{
    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_GPU_RESET;
    msg.arg0 = scope;

    return vc_call(&msg, &reply);
}

/* ===================================================================
 *  Event subscription
 * =================================================================== */

int dobvc_Subscribe(uint32_t port, uint32_t event_mask)
{
    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_SUBSCRIBE;
    msg.arg0 = port;
    msg.arg1 = event_mask;

    return vc_call(&msg, &reply);
}

int dobvc_Unsubscribe(void)
{
    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_UNSUBSCRIBE;

    return vc_call(&msg, &reply);
}

/* ===================================================================
 *  Driver identity
 * =================================================================== */

int dobvc_DriverInfo(dv_driver_info_t *out)
{
    if (!out) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_DRIVER_INFO;

    return copy_out(vc_call(&msg, &reply), &reply, out, sizeof(*out));
}

/* ===================================================================
 *  Capability / vcore introspection
 * =================================================================== */

int dobvc_CapQuery(uint64_t *out_capabilities)
{
    if (!out_capabilities) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_CAP_QUERY;

    return copy_out(vc_call(&msg, &reply), &reply,
                    out_capabilities, sizeof(*out_capabilities));
}

int dobvc_CapQueryLimit(uint32_t which, uint64_t *out_value)
{
    if (!out_value) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_CAP_QUERY_LIMIT;
    msg.arg0 = which;

    int rc = vc_call(&msg, &reply);
    if (rc == DV_OK)
        *out_value = ((uint64_t)reply.arg1 << 32) | reply.arg0;
    return rc;
}

int dobvc_CapQueryFormat(uint32_t fmt, uint32_t *out_usage_flags)
{
    if (!out_usage_flags) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_CAP_QUERY_FORMAT;
    msg.arg0 = fmt;

    int rc = vc_call(&msg, &reply);
    if (rc == DV_OK) *out_usage_flags = reply.arg0;
    return rc;
}

int dobvc_VcoreCount(uint32_t *out_count)
{
    if (!out_count) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_VCORE_COUNT;

    int rc = vc_call(&msg, &reply);
    if (rc == DV_OK) *out_count = reply.arg0;
    return rc;
}

int dobvc_VcoreInfo(uint32_t index, dv_vcore_info_t *out)
{
    if (!out) return DV_ERR_INVAL;

    dob_msg_t msg = {0}, reply;
    msg.code = DOBVC_OP_VCORE_INFO;
    msg.arg0 = index;

    return copy_out(vc_call(&msg, &reply), &reply, out, sizeof(*out));
}
