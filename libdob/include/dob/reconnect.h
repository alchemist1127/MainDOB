#ifndef MAINDOB_DOB_RECONNECT_H
#define MAINDOB_DOB_RECONNECT_H

/* Shared reconnect helper for all Entry Point stubs.
 *
 * When a server bubble crashes, the kernel relaunches it (if critical)
 * with a new IPC port. Stubs cache the port for performance. This helper
 * detects a dead port (IPC_ERR_DEAD / IPC_ERR_NO_PORT), invalidates
 * the cache, re-discovers the server via the registry, and retries
 * the call — transparently.
 *
 * This is what makes MainDOB's bubble architecture self-healing:
 * crash → relaunch → automatic reconnect, no manual intervention. */

#include <dob/ipc.h>
#include <dob/registry.h>
#include <string.h>

static inline dob_status_t
_dob_call_reconnect(uint32_t *cached_port, const char *service_name,
                    uint32_t timeout_ms, dob_msg_t *msg, dob_msg_t *reply)
{
    /* Ensure we have a port */
    if (!*cached_port)
        *cached_port = dob_registry_wait(service_name, timeout_ms);
    if (!*cached_port)
        return DOB_ERR_NOT_FOUND;

    dob_status_t ret = dob_ipc_call(*cached_port, msg, reply);

    /* Dead port: server crashed. Re-discover and retry once. */
    if (ret == DOB_ERR_DEAD || ret == DOB_ERR_NOT_FOUND)
    {
        *cached_port = 0;
        *cached_port = dob_registry_wait(service_name, timeout_ms);
        if (!*cached_port)
            return DOB_ERR_NOT_FOUND;
        memset(reply, 0, sizeof(dob_msg_t));
        ret = dob_ipc_call(*cached_port, msg, reply);
    }

    return ret;
}

#endif /* MAINDOB_DOB_RECONNECT_H */
