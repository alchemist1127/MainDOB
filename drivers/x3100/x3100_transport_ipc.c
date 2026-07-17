/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * MainDOB x3100 driver — IPC transport for the control plane.
 *
 * Handles only DOBVC_* control opcodes (>= 0x10000): mode list/get/set,
 * display info, subscribe, driver info, capability queries.  These are
 * infrequent (Settings UI, multi-display) and don't need the boomerang's
 * register-only argument shape; the data plane lives in
 * x3100_transport_fast.c + x3100_fast_entry.asm.
 *
 * Single concern: deserialize an incoming dob_msg_t into one DOBVC_OP_*
 * control op's arguments, invoke it, marshal the outputs back.  All driver
 * logic lives in main.c.  Opcodes this driver doesn't implement (gamma,
 * power, scanout, arrangement-set, gpu-reset) return DV_ERR_NOSUPPORT.
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

#include "x3100_state.h"
#include "dobvc_protocol.h"

/* Control-plane functions defined in main.c (block #5). */
int32_t dv_mode_list(uint32_t display_id, dv_mode_t *out, uint32_t *io_count);
int32_t dv_mode_get_current(uint32_t display_id, dv_mode_t *out);
int32_t x3100_internal_mode_set(const dv_mode_t *m);
int32_t dv_display_count(uint32_t *out);
int32_t dv_display_info(uint32_t display_id, dv_display_info_t *out);
int32_t x3100_subscribe(uint32_t a0, uint32_t a1);
int32_t dv_driver_info(dv_driver_info_t *out);
int32_t dv_cap_query(uint64_t *out);
int32_t dv_cap_query_limit(dv_limit_t which, uint64_t *out);
int32_t dv_cap_query_format(dv_format_t fmt, uint32_t *out);
void    x3100_shutdown_for_detach(void);

/* Helper: zero a reply and stash a return code. */
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
        rc = x3100_internal_mode_set((const dv_mode_t *)msg->payload);
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

    case DOBVC_OP_SUBSCRIBE:
        rc = x3100_subscribe(msg->arg0, msg->arg1);
        reply_set(reply, rc); return rc;

    case DOBVC_OP_DRIVER_INFO:
    { static dv_driver_info_t info;
      rc = dv_driver_info(&info);
      reply_set(reply, rc);
      if (rc == DV_OK) { reply->payload = &info; reply->payload_size = sizeof(info); }
      return rc; }

    /* read-only arrangement, gamma, power, scanout, gpu-reset, vcore:
     * not implemented on this driver -> fall through to NOSUPPORT. */
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
    }
    /* gamma / power / scanout / arrangement-set / gpu-reset / vcore:
     * not implemented on this driver. */
    reply_set(reply, DV_ERR_NOSUPPORT);
    return DV_ERR_NOSUPPORT;
}

/* ---------- public entry point: main()'s event loop ---------- */

int x3100_transport_ipc_run(uint32_t port)
{
    for (;;)
    {
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        dob_ipc_receive(port, &msg);

        if (__builtin_expect(msg.code == HOTPLUG_DETACH, 0))
        {
            x3100_shutdown_for_detach();
            return 0;
        }

        x3100_current_caller_pid = msg.sender_pid;

        dob_msg_t reply;
        if (msg.code >= 0x10000)
            dispatch_control(&msg, &reply);
        else
            /* DV_* data-plane ops go through the int 0x85 boomerang only.
             * An IPC call in this range is an obsolete path — refuse so it
             * surfaces clearly instead of falling through silently. */
            reply_set(&reply, DV_ERR_NOSUPPORT);

        dob_ipc_reply(msg.sender_tid, &reply);
    }
}
