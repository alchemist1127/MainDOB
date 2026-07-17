/* DobArchive client stub — IPC marshalling for the native archive API.
 *
 * Each call resolves the target service's port through the registry
 * (cached per service name), serialises its arguments into a dob_msg_t,
 * issues a synchronous call, and unpacks the reply. Mirrors the style
 * of DobFileSystem_stub.c without the service-routing layer: the
 * caller always names the archive instance explicitly.
 */

#include "DobArchive.h"
#include <dob/ipc.h>
#include <dob/registry.h>
#include <string.h>

/*  *  Service port cache
 *
 *  archiveN services are short-lived compared to DobFileSystem, so a
 *  miss is likely the first time any given program calls us. We cache
 *  the last successful (name, port) pair only — callers typically
 *  talk to one archive at a time.
 */

static char     cached_name[16] = {0};
static uint32_t cached_port     = 0;

static uint32_t
resolve_port(const char *service)
{
    if (!service || !service[0]) return 0;
    if (cached_port && strcmp(cached_name, service) == 0)
        return cached_port;

    uint32_t port = dob_registry_find(service);
    if (port)
    {
        strncpy(cached_name, service, sizeof(cached_name) - 1);
        cached_name[sizeof(cached_name) - 1] = '\0';
        cached_port = port;
    }
    return port;
}

/* API */

int
dobarchive_Count(const char *service, uint32_t *count_out, uint8_t *format_out)
{
    uint32_t port = resolve_port(service);
    if (!port) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = ARCHIVE_COUNT;

    if (dob_ipc_call(port, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0)                    return (int)reply.code;

    if (count_out)  *count_out  = reply.arg0;
    if (format_out) *format_out = (uint8_t)reply.arg1;
    return 0;
}

int
dobarchive_GetFormat(const char *service)
{
    uint32_t port = resolve_port(service);
    if (!port) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = ARCHIVE_GET_FORMAT;

    if (dob_ipc_call(port, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0)                    return (int)reply.code;
    return (int)reply.arg0;
}

int
dobarchive_EntryInfo(const char *service, uint32_t index,
                     archive_entry_info_t *info)
{
    if (!info) return -1;
    uint32_t port = resolve_port(service);
    if (!port) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = ARCHIVE_ENTRY_INFO;
    msg.arg0 = index;

    if (dob_ipc_call(port, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0)                    return (int)reply.code;
    if (!reply.payload || reply.payload_size < sizeof(*info)) return -1;

    memcpy(info, reply.payload, sizeof(*info));
    return 0;
}

int
dobarchive_ReadEntry(const char *service, uint32_t index,
                     uint32_t offset, void *buf, uint32_t max)
{
    if (!buf) return -1;
    uint32_t port = resolve_port(service);
    if (!port) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = ARCHIVE_READ_ENTRY;
    msg.arg0 = index;
    msg.arg1 = offset;
    msg.arg2 = max;

    if (dob_ipc_call(port, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0)                    return (int)reply.code;

    uint32_t n = reply.arg0;
    if (n > max) n = max;
    if (n && reply.payload && reply.payload_size >= n)
        memcpy(buf, reply.payload, n);
    return (int)n;
}

int
dobarchive_ExtractTo(const char *service, uint32_t index,
                     const char *dest_path)
{
    if (!dest_path) return -1;
    uint32_t port = resolve_port(service);
    if (!port) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code         = ARCHIVE_EXTRACT_TO;
    msg.arg0         = index;
    msg.payload      = (void *)dest_path;
    msg.payload_size = (uint32_t)strlen(dest_path) + 1;

    if (dob_ipc_call(port, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0)                    return (int)reply.code;
    return (int)reply.arg0;
}

int
dobarchive_Close(const char *service)
{
    uint32_t port = resolve_port(service);
    if (!port) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = ARCHIVE_CLOSE;

    dob_status_t st = dob_ipc_call(port, &msg, &reply);
    /* Invalidate cache either way — the server either exited or is
     * about to, and a stale port will fail on the next call anyway. */
    cached_name[0] = '\0';
    cached_port    = 0;
    return (st == DOB_OK) ? 0 : -1;
}
