/* BGA driver — IPC transport for the control plane.
 *
 * Handles only DOBVC_* control opcodes (>= 0x10000): mode_list,
 * mode_set, display_info, subscribe, driver_info, gpu_reset,
 * arrangement.  These are infrequent (Settings UI, gamma, multi-
 * display) and don't need the fast path's register-only argument
 * shape; the data plane lives in bga_transport_fast.c + bga_fast_entry.asm.
 *
 * Single concern: deserialize an incoming dob_msg_t into the argument
 * tuple of one DOBVC_OP_* control op, invoke it, marshal the outputs
 * back into the reply.  All driver logic lives in main.c.
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <dob/ipc.h>
#include <dob/server.h>
#include <dob/registry.h>
#include <dob/hotplug.h>
#include <dob/video.h>
#include <DobVideoControl.h>

#include "bga_state.h"
#include "dobvc_protocol.h"

/* Helper: flag-zero a reply and stash a return code. */
static inline void reply_set(dob_msg_t *r, int32_t rc)
{
    memset(r, 0, sizeof(*r));
    r->code = (uint32_t)rc;
}

/* ---------- control-plane dispatch (DOBVC_OP_*) ---------- */

static int dispatch_control(dob_msg_t *msg, dob_msg_t *reply)
{
    int32_t rc;
    switch (msg->code)
    {
    case DOBVC_OP_MODE_LIST:
    {
        static dv_mode_t buf[MAX_MODE_LIST];
        uint32_t n = MAX_MODE_LIST;
        rc = dv_mode_list(msg->arg0, buf, &n);
        reply_set(reply, rc);
        if (rc == DV_OK)
        {
            reply->payload = buf;
            reply->payload_size = n * sizeof(dv_mode_t);
            reply->arg0 = n;
        }
        return rc;
    }
    case DOBVC_OP_MODE_GET:
    {
        static dv_mode_t m;
        rc = dv_mode_get_current(msg->arg0, &m);
        reply_set(reply, rc);
        if (rc == DV_OK) { reply->payload = &m; reply->payload_size = sizeof(m); }
        return rc;
    }
    case DOBVC_OP_MODE_SET:
        if (!msg->payload || msg->payload_size < sizeof(dv_mode_t))
            { reply_set(reply, DV_ERR_INVAL); return DV_ERR_INVAL; }
        rc = bga_internal_mode_set((const dv_mode_t *)msg->payload);
        reply_set(reply, rc); return rc;

    case DOBVC_OP_DISPLAY_COUNT:
    { uint32_t n; rc = dv_display_count(&n);
      reply_set(reply, rc); if (rc == DV_OK) reply->arg0 = n; return rc; }
    case DOBVC_OP_DISPLAY_INFO:
    { static dv_display_info_t info;
      rc = dv_display_info(msg->arg0, &info);
      reply_set(reply, rc);
      if (rc == DV_OK) { reply->payload = &info; reply->payload_size = sizeof(info); }
      return rc; }

    case DOBVC_OP_SUBSCRIBE:    rc = bga_subscribe(msg->arg0, msg->arg1);
                                reply_set(reply, rc); return rc;

    case DOBVC_OP_DRIVER_INFO:
    { static dv_driver_info_t info;
      rc = dv_driver_info(&info);
      reply_set(reply, rc);
      if (rc == DV_OK) { reply->payload = &info; reply->payload_size = sizeof(info); }
      return rc; }

    case DOBVC_OP_GPU_RESET:    bga_gpu_reset_full();
                                reply_set(reply, DV_OK); return DV_OK;

    /* read-only arrangement / EDID stubs */
    case DOBVC_OP_ARRANGEMENT_GET:
    {
        static dobvc_display_arrangement_t arr;
        arr.display_id = 0; arr.origin_x = 0; arr.origin_y = 0;
        arr.enabled = true; arr.primary = true; arr.rotation_deg = 0;
        reply_set(reply, DV_OK);
        reply->payload = &arr; reply->payload_size = sizeof(arr); reply->arg0 = 1;
        return DV_OK;
    }

    /* capability / vcore introspection (cold-path mirror of the
     * dv_cap_* / dv_vcore_* data-plane reads, for vprocess-less
     * clients).  Marshalling matches the former dispatch_data(). */
    case DOBVC_OP_CAP_QUERY:
    {
        static uint64_t caps;
        rc = dv_cap_query(&caps);
        reply_set(reply, rc);
        if (rc == DV_OK) { reply->payload = &caps; reply->payload_size = sizeof(caps); }
        return rc;
    }
    case DOBVC_OP_CAP_QUERY_LIMIT:
    {
        uint64_t v;
        rc = dv_cap_query_limit((dv_limit_t)msg->arg0, &v);
        reply_set(reply, rc);
        if (rc == DV_OK)
        {
            reply->arg0 = (uint32_t)(v & 0xFFFFFFFFu);
            reply->arg1 = (uint32_t)(v >> 32);
        }
        return rc;
    }
    case DOBVC_OP_CAP_QUERY_FORMAT:
    {
        uint32_t flags;
        rc = dv_cap_query_format((dv_format_t)msg->arg0, &flags);
        reply_set(reply, rc);
        if (rc == DV_OK) reply->arg0 = flags;
        return rc;
    }
    case DOBVC_OP_VCORE_COUNT:
    {
        uint32_t n;
        rc = dv_vcore_count(&n);
        reply_set(reply, rc);
        if (rc == DV_OK) reply->arg0 = n;
        return rc;
    }
    case DOBVC_OP_VCORE_INFO:
    {
        static dv_vcore_info_t info;
        rc = dv_vcore_info(msg->arg0, &info);
        reply_set(reply, rc);
        if (rc == DV_OK) { reply->payload = &info; reply->payload_size = sizeof(info); }
        return rc;
    }
    }
    reply_set(reply, DV_ERR_NOSUPPORT);
    return DV_ERR_NOSUPPORT;
}

/* ---------- public entry point: main()'s event loop ---------- */

int bga_transport_ipc_run(uint32_t port)
{
    for (;;)
    {
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        dob_ipc_receive(port, &msg);

        if (__builtin_expect(msg.code == HOTPLUG_DETACH, 0))
        {
            extern void bga_shutdown_for_detach(void);
            bga_shutdown_for_detach();
            return 0;
        }

        bga_current_caller_pid = msg.sender_pid;

        dob_msg_t reply;
        if (msg.code >= 0x10000)
        {
            dispatch_control(&msg, &reply);
        }
        else
        {
            /* DV_* data-plane ops are handled exclusively via the
             * int 0x85 boomerang.  An IPC call into this range means
             * a client is using an obsolete code path — refuse and
             * let it surface as a clear error instead of falling
             * through silently. */
            reply_set(&reply, DV_ERR_NOSUPPORT);
        }

        dob_ipc_reply(msg.sender_tid, &reply);
    }
}
