/* DobTable client stub — IPC marshalling for the DobTable Entry Point.
 *
 * Each call resolves the target instance through the registry (cached
 * per service name, same idiom as DobArchive_stub) and issues a
 * synchronous send.  The strings travel as packed NUL-terminated
 * blobs; nothing here knows about the table widget itself.
 *
 * Spawn is the only call that does work beyond marshalling: it picks
 * a unique service name, launches DobTable.mdl with that name in
 * argv[0], and blocks until DobTable has finished registering. */

#include "DobTable.h"
#include "dobtable_protocol.h"
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/spawn.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#define DOBTABLE_BIN_PATH   "/SYSTEM/PROGRAMS/DobTable/DobTable.mdl"
#define SPAWN_WAIT_MS       5000

/* Service-port cache — last successful (name, port) pair.  A program
 * usually addresses one DobTable instance at a time; if it juggles
 * multiple, the cache misses harmlessly and re-resolves through the
 * registry (single syscall, no I/O). */
static char     cached_name[24] = {0};
static uint32_t cached_port     =  0;

static uint32_t resolve_port(const char *service)
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

/* Spawn — picks a unique service name, launches DobTable, waits for
 * registration. Service-name format: "tbl_<pid>_<seq>".  pid varies
 * across processes; seq is a per-process static counter, so even a
 * single process spawning many DobTables in a row gets distinct names. */

int dobtable_Spawn(char *out_service, int out_cap)
{
    if (!out_service || out_cap < 24) return -1;

    static uint32_t seq = 0;
    seq++;

    char service[24];
    snprintf(service, sizeof(service), "tbl_%d_%u",
             (int)getpid(), (unsigned)seq);

    /* spawn_file is fire-and-forget; the new process is started
     * asynchronously by a worker thread. */
    const char *argv[] = { service, NULL };
    if (spawn_file(DOBTABLE_BIN_PATH, argv) != 1)
        return -1;

    /* Block until the spawned process registers itself. */
    uint32_t port = dob_registry_wait(service, SPAWN_WAIT_MS);
    if (!port) return -1;

    /* Prime the cache so the immediate follow-up calls hit it. */
    strncpy(cached_name, service, sizeof(cached_name) - 1);
    cached_name[sizeof(cached_name) - 1] = '\0';
    cached_port = port;

    /* Hand the name back to the caller. */
    int n = (int)strlen(service);
    if (n >= out_cap) n = out_cap - 1;
    memcpy(out_service, service, (uint32_t)n);
    out_service[n] = '\0';
    return 0;
}

/* Generic send-and-reply helper — every "set" call below is the same
 * skeleton, only msg.code/payload differ. Returns 0 on success, -1 on
 * IPC failure or server-reported error. */
static int call(uint32_t port, dob_msg_t *msg)
{
    dob_msg_t reply = {0};
    if (dob_ipc_call(port, msg, &reply) != DOB_OK) return -1;
    return ((int32_t)reply.arg0 == 0) ? 0 : -1;
}

int dobtable_SetTitle(const char *service, const char *title)
{
    uint32_t port = resolve_port(service);
    if (!port || !title) return -1;

    dob_msg_t msg = {0};
    msg.code         = DOBTABLE_SET_TITLE;
    msg.payload      = (void *)title;
    msg.payload_size = strlen(title) + 1;
    return call(port, &msg);
}

int dobtable_SetHeaders(const char *service,
                        const char *key_header,
                        const char *value_header)
{
    uint32_t port = resolve_port(service);
    if (!port) return -1;

    /* Pack: key_header\0value_header\0.  NULL → empty string, which
     * the server treats as "no header for this column". */
    const char *kh = key_header   ? key_header   : "";
    const char *vh = value_header ? value_header : "";
    uint32_t klen = (uint32_t)strlen(kh) + 1;
    uint32_t vlen = (uint32_t)strlen(vh) + 1;

    char buf[2 * DOBTABLE_MAX_HEADER_LEN];
    if (klen + vlen > sizeof(buf)) return -1;

    memcpy(buf,        kh, klen);
    memcpy(buf + klen, vh, vlen);

    dob_msg_t msg = {0};
    msg.code         = DOBTABLE_SET_HEADERS;
    msg.payload      = buf;
    msg.payload_size = klen + vlen;
    return call(port, &msg);
}

int dobtable_AddRows(const char *service,
                     const char **keys,
                     const char **values,
                     int count)
{
    uint32_t port = resolve_port(service);
    if (!port || count < 0) return -1;
    if (count == 0) return 0;
    if (count > DOBTABLE_MAX_ROWS) return -1;

    /* Two-pass to size the buffer — way cheaper than a single
     * worst-case 80 KB allocation for a typical 5-row table. */
    uint32_t need = 0;
    for (int i = 0; i < count; i++)
    {
        const char *k = keys   ? keys[i]   : NULL;
        const char *v = values ? values[i] : NULL;
        if (!k) k = "";
        if (!v) v = "";
        uint32_t kl = (uint32_t)strlen(k) + 1;
        uint32_t vl = (uint32_t)strlen(v) + 1;
        if (kl > DOBTABLE_MAX_KEY_LEN || vl > DOBTABLE_MAX_VALUE_LEN)
            return -1;
        need += kl + vl;
    }

    char *buf = (char *)malloc(need);
    if (!buf) return -1;

    uint32_t off = 0;
    for (int i = 0; i < count; i++)
    {
        const char *k = keys   ? keys[i]   : NULL;
        const char *v = values ? values[i] : NULL;
        if (!k) k = "";
        if (!v) v = "";
        uint32_t kl = (uint32_t)strlen(k) + 1;
        uint32_t vl = (uint32_t)strlen(v) + 1;
        memcpy(buf + off, k, kl); off += kl;
        memcpy(buf + off, v, vl); off += vl;
    }

    dob_msg_t msg = {0};
    msg.code         = DOBTABLE_ADD_ROWS;
    msg.arg0         = (uint32_t)count;
    msg.payload      = buf;
    msg.payload_size = off;
    int r = call(port, &msg);
    free(buf);
    return r;
}

int dobtable_Clear(const char *service)
{
    uint32_t port = resolve_port(service);
    if (!port) return -1;
    dob_msg_t msg = {0};
    msg.code = DOBTABLE_CLEAR;
    return call(port, &msg);
}

int dobtable_Show(const char *service)
{
    uint32_t port = resolve_port(service);
    if (!port) return -1;
    dob_msg_t msg = {0};
    msg.code = DOBTABLE_SHOW;
    return call(port, &msg);
}

int dobtable_Close(const char *service)
{
    uint32_t port = resolve_port(service);
    if (!port) return -1;

    dob_msg_t msg = {0};
    msg.code = DOBTABLE_CLOSE;
    int r = call(port, &msg);

    /* Invalidate the cache — the port is dead now. */
    if (cached_port == port)
    {
        cached_port    = 0;
        cached_name[0] = '\0';
    }
    return r;
}
