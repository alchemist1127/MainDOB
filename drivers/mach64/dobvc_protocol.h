/* DobVideoControl IPC protocol — private wire format.
 *
 * Shared between the driver (drivers/mach64/main.c — and mirrored, with
 * identical opcodes, in drivers/bga/dobvc_protocol.h) and the client
 * stub (libdob/src/DobVideoControl_stub.c).  Public API is in
 * <DobVideoControl.h>; this file is the wire-level details that no
 * client code should ever include.
 *
 * Opcodes start at 0x10000 to stay clear of every dobVideo data-plane
 * opcode (which range up to 0x0F00).  The transport routes by simple
 * range test: msg.code >= 0x10000 is control plane, otherwise data
 * plane.  No overlap, no ambiguity. */

#ifndef MAINDOB_DRIVERS_DOBVC_PROTOCOL_H
#define MAINDOB_DRIVERS_DOBVC_PROTOCOL_H

#include <dob/types.h>

/* Control-plane opcodes (msg.code) */
#define DOBVC_OP_MODE_LIST          0x10000
#define DOBVC_OP_MODE_GET           0x10001
#define DOBVC_OP_MODE_SET           0x10002

#define DOBVC_OP_DISPLAY_COUNT      0x10010
#define DOBVC_OP_DISPLAY_INFO       0x10011
#define DOBVC_OP_DISPLAY_EDID       0x10012

#define DOBVC_OP_ARRANGEMENT_GET    0x10020
#define DOBVC_OP_ARRANGEMENT_SET    0x10021

#define DOBVC_OP_GAMMA_GET          0x10030
#define DOBVC_OP_GAMMA_SET          0x10031
#define DOBVC_OP_GAMMA_RESET        0x10032

#define DOBVC_OP_SCANOUT_GET        0x10040
#define DOBVC_OP_SCANOUT_SET        0x10041

#define DOBVC_OP_POWER_GET          0x10050
#define DOBVC_OP_POWER_SET          0x10051
#define DOBVC_OP_GPU_RESET          0x10052

#define DOBVC_OP_SUBSCRIBE          0x10060
#define DOBVC_OP_UNSUBSCRIBE        0x10061
#define DOBVC_OP_DRIVER_INFO        0x10070

/* Capability / vcore introspection.  These mirror the dv_cap_* and
 * dv_vcore_* read-only data-plane opcodes for clients that do not
 * hold a vprocess (the fast-path boomerang only wires the hot-path
 * ops, not introspection — see mach64_transport_fast.c).  Cold path:
 * queried once at attach time, never per-frame. */
#define DOBVC_OP_CAP_QUERY          0x10080
#define DOBVC_OP_CAP_QUERY_LIMIT    0x10081
#define DOBVC_OP_CAP_QUERY_FORMAT   0x10082

#define DOBVC_OP_VCORE_COUNT        0x10090
#define DOBVC_OP_VCORE_INFO         0x10091

/* For simple ops the wire layout is:
 *   msg.arg0 = display_id (where applicable)
 *   msg.payload = op-specific struct (mode, gamma ramp, arrangement[], ...)
 *   msg.payload_size = byte count
 *   reply.code = 0 on success, negative DV_ERR_* on failure
 *   reply.arg0..arg3 = small return values (count, handle, state, ...)
 *   reply.payload = filled-out struct on read ops */

#endif /* MAINDOB_DRIVERS_DOBVC_PROTOCOL_H */
