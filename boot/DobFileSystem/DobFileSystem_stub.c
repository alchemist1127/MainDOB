/* DobFileSystem stub — prefix-aware routing.
 *
 * Linked into every boot service and program. Hides IPC behind a
 * flat dobfs_* API and routes file operations by path prefix:
 *
 *     /u<digit>/...          →  "floppy"          (SVC_FLOPPY, fixed)
 *     /SYSTEM/...  /DATA/... →  "DobFileSystem"   (SVC_BOOT,   fixed)
 *     anything else          →  dobfs_service_name (SVC_DEFAULT,
 *                                                   rebindable via
 *                                                   dobfs_set_service)
 *
 * SVC_BOOT is hardwired and never rebinds: a satellite attached to a
 * removable backend via set_service() still reaches /SYSTEM and /DATA.
 *
 * fd values from different services can collide, so the stub maps
 * the caller-visible fd to a (service, real_fd) pair via a local
 * table. The caller treats the fd as opaque.
 */

#include <DobFileSystem.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/reconnect.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "dobfs_protocol.h"

/* Service routing */

typedef enum
{
    SVC_DEFAULT = 0,   /* dobfs_service_name — rebindable fallback    */
    SVC_FLOPPY  = 1,   /* "floppy" — fixed                            */
    SVC_BOOT    = 2,   /* "DobFileSystem" — fixed, boot disk FAT32    */
    SVC_COUNT
} service_id_t;

/* Backing storage for the SVC_DEFAULT name.
 *
 * dobfs_set_service() copies into this process-lifetime static rather
 * than caching the caller's pointer: every call site passes either a
 * stack buffer or a pointer into an IPC payload, neither of which
 * survives past the next IPC receive. The published name is also
 * stored into DobConfig for child-process mount inheritance, which
 * a dangling pointer would corrupt as soon as the caller's stack is
 * reused.
 */
static char        dobfs_service_name_buf[64] = "DobFileSystem";
static const char *dobfs_service_name         = dobfs_service_name_buf;

/* True once dobfs_set_service() has been called explicitly by the
 * client (e.g. a satellite DobFiles bound to "cdrom" or "DobArchive").
 * service_id_for_path() consults this when classifying /u<n> paths:
 * by default /u<n> goes to SVC_FLOPPY (the historical, hard-wired
 * removable-media slot), but a satellite that has bound itself to a
 * different service must route /u<n> through SVC_DEFAULT instead, so
 * its READDIR/STAT calls reach the actual mount target.
 *
 * Without this flag, a CD-ROM satellite bound to service "cdrom" would
 * still ship every /u2/READDIR to "floppy", get an empty/no reply, and
 * render a blank file explorer. */
static bool        dobfs_service_explicitly_set = false;

/* One port cache slot per known service. Only SVC_DEFAULT is
 * invalidated by dobfs_set_service(); SVC_FLOPPY and SVC_BOOT are
 * fixed targets and their cached ports survive rebinds. */
static uint32_t service_ports[SVC_COUNT] = { 0, 0, 0 };

void dobfs_set_service(const char *name)
{
    if (!name || !*name) return;
    size_t n = strlen(name);
    if (n >= sizeof(dobfs_service_name_buf)) n = sizeof(dobfs_service_name_buf) - 1;
    memcpy(dobfs_service_name_buf, name, n);
    dobfs_service_name_buf[n] = '\0';
    dobfs_service_name         = dobfs_service_name_buf;
    service_ports[SVC_DEFAULT] = 0;    /* invalidate default cache */
    dobfs_service_explicitly_set = true;
}

/* Returns the name currently bound as SVC_DEFAULT. Pointer remains
 * valid for the lifetime of the process (it's either the hardcoded
 * default "DobFileSystem" or a string set by dobfs_set_service,
 * which the caller guarantees outlives the stub). Used by spawners
 * that need to propagate their own mount to a child process. */
const char *dobfs_get_service(void)
{
    return dobfs_service_name;
}

static const char *
service_name_for(service_id_t sid)
{
    switch (sid)
    {
        case SVC_FLOPPY: return "floppy";
        case SVC_BOOT:   return "DobFileSystem";
        default:         return dobfs_service_name;
    }
}

/* Classify a path by its first few bytes:
 *   "/u<digit>[/...]"            → SVC_FLOPPY by default,
 *                                   SVC_DEFAULT if dobfs_set_service()
 *                                   was called (satellite bound to a
 *                                   specific removable-media service
 *                                   like "cdrom" — it must reach that
 *                                   service, not the floppy).
 *   "/SYSTEM[/...]" "/DATA[/...]"→ SVC_BOOT   (hardwired boot disk)
 *   anything else                → SVC_DEFAULT (rebindable)
 *
 * The boot-disk slot (SVC_BOOT) stays hard-wired regardless of any
 * set_service call: a satellite that needs /SYSTEM access must always
 * reach it through the canonical "DobFileSystem" service, never
 * through whatever removable backend it happens to be displaying. */
static service_id_t
service_id_for_path(const char *path)
{
    if (!path || path[0] != '/') return SVC_DEFAULT;

    /* /u<digit>(/|\0) → floppy by default, SVC_DEFAULT if rebound */
    if (path[1] == 'u'
     && path[2] >= '0' && path[2] <= '9'
     && (path[3] == '\0' || path[3] == '/'))
        return dobfs_service_explicitly_set ? SVC_DEFAULT : SVC_FLOPPY;

    /* /SYSTEM(/|\0) → boot */
    if (path[1] == 'S' && path[2] == 'Y' && path[3] == 'S'
     && path[4] == 'T' && path[5] == 'E' && path[6] == 'M'
     && (path[7] == '\0' || path[7] == '/'))
        return SVC_BOOT;

    /* /DATA(/|\0) → boot */
    if (path[1] == 'D' && path[2] == 'A' && path[3] == 'T' && path[4] == 'A'
     && (path[5] == '\0' || path[5] == '/'))
        return SVC_BOOT;

    return SVC_DEFAULT;
}

/* One-shot lazy mount inheritance.
 *
 * When a program is spawned from inside a mounted DobFiles satellite
 * (e.g. an archive view or a floppy window), it needs to see the
 * same filesystem as the window that launched it — otherwise a
 * double-click on "/readme.txt" inside an archive lands on the boot
 * disk's nonexistent /readme.txt.
 *
 * The launching code (DobFiles satellite in open_selected) publishes
 * its own SVC_DEFAULT name into DobConfig under the key
 * "spawn_default_service" just before spawning. The first time this
 * process asks the stub for SVC_DEFAULT, we pick up that key,
 * rebind, and clear it so the next spawn starts clean.
 *
 * Inherited names live in this static buffer for the process
 * lifetime so dobfs_service_name can point into it safely. */
static char     inherited_service_buf[64];
static bool     mount_inherit_attempted = false;

static void try_inherit_default_mount(void);

/* Forward refs — definitions live further down with the rest of the
 * REJECTED-message plumbing. call_via and vfd_call need them here. */
#define LAST_REJECTION_CAP 192
static char g_last_rejection[LAST_REJECTION_CAP];
static void capture_rejection(const dob_msg_t *reply);

static dob_status_t
call_via(service_id_t sid, dob_msg_t *msg, dob_msg_t *reply)
{
    if (sid == SVC_DEFAULT && !mount_inherit_attempted)
        try_inherit_default_mount();

    dob_status_t st = _dob_call_reconnect(&service_ports[sid],
                                          service_name_for(sid),
                                          10000, msg, reply);
    if (st == DOB_OK) capture_rejection(reply);
    else              g_last_rejection[0] = '\0';
    return st;
}

/* Implemented below call_via to keep the hot path small. Declared
 * here to satisfy the forward reference. This function is called
 * at most once per process and is otherwise inert.
 *
 * We talk to the "config" service directly via IPC rather than
 * linking DobConfig_stub, because DobConfig itself links *us* —
 * pulling DobConfig_stub into DobFileSystem_stub would create a
 * circular module dependency and break the link of boot-critical
 * modules like config.mdl and DobFileSystem.mdl. The IPC
 * wire format is identical to what DobConfig_stub emits: opcode
 * CONFIG_CMD_GET (1) with payload=key, opcode CONFIG_CMD_SET (2)
 * with payload="key\0value". */

#define CONFIG_CMD_GET 1
#define CONFIG_CMD_SET 2

static uint32_t cached_config_port = 0;

static uint32_t
resolve_config_port(void)
{
    if (cached_config_port) return cached_config_port;
    uint32_t p = dob_registry_find("config");
    if (p) cached_config_port = p;
    return p;
}

static int
config_get_raw(const char *key, char *value_out, uint32_t max_len)
{
    uint32_t port = resolve_config_port();
    if (!port || !key || !value_out || max_len == 0) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code         = CONFIG_CMD_GET;
    msg.payload      = (void *)key;
    msg.payload_size = (uint32_t)strlen(key) + 1;

    if (dob_ipc_call(port, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0)                    return -1;

    if (!reply.payload || reply.payload_size == 0)
    {
        value_out[0] = '\0';
        return 0;
    }
    uint32_t n = reply.payload_size;
    if (n >= max_len) n = max_len - 1;
    memcpy(value_out, reply.payload, n);
    value_out[n] = '\0';
    return 0;
}

static int
config_set_raw(const char *key, const char *value)
{
    uint32_t port = resolve_config_port();
    if (!port || !key || !value) return -1;

    uint32_t klen = (uint32_t)strlen(key);
    uint32_t vlen = (uint32_t)strlen(value);
    char buf[256];
    if (klen + 1 + vlen + 1 > sizeof(buf)) return -1;

    memcpy(buf, key, klen);
    buf[klen] = '\0';
    memcpy(buf + klen + 1, value, vlen);
    buf[klen + 1 + vlen] = '\0';

    dob_msg_t msg = {0}, reply = {0};
    msg.code         = CONFIG_CMD_SET;
    msg.payload      = buf;
    msg.payload_size = klen + 1 + vlen + 1;

    if (dob_ipc_call(port, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0)                    return -1;
    return 0;
}

static void
try_inherit_default_mount(void)
{
    mount_inherit_attempted = true;

    /* Only take the handoff if the caller hasn't already bound
     * explicitly — an explicit dobfs_set_service() wins over any
     * background inheritance. */
    if (dobfs_service_name &&
        (dobfs_service_name[0] != 'D' || strcmp(dobfs_service_name, "DobFileSystem") != 0))
        return;

    int rc = config_get_raw("spawn_default_service",
                            inherited_service_buf, sizeof(inherited_service_buf));
    if (rc != 0) return;
    if (inherited_service_buf[0] == '\0') return;

    /* Consume immediately so a sibling spawn (say, two programs
     * launched in quick succession) doesn't inherit a stale value. */
    config_set_raw("spawn_default_service", "");

    dobfs_service_name         = inherited_service_buf;
    service_ports[SVC_DEFAULT] = 0;
}

/*  *  Virtual fd table
 *
 *  Each vfd remembers (real_fd, port, service_name) so Read/Write/
 *  Close/Seek don't need to re-route — they call straight to the
 *  port that opened the underlying real_fd. The cached port is
 *  invalidated and rediscovered on death via _dob_call_reconnect.
 */

#define VFD_MAX     32
#define VFD_INVALID (-1)

typedef struct
{
    int      in_use;
    int      real_fd;
    uint32_t port;
    char     name[64];
} vfd_entry_t;

static vfd_entry_t vfds[VFD_MAX];

static int vfd_alloc(int real_fd, uint32_t port, const char *name)
{
    for (int i = 0; i < VFD_MAX; i++)
    {
        if (vfds[i].in_use) continue;
        vfds[i].in_use  = 1;
        vfds[i].real_fd = real_fd;
        vfds[i].port    = port;
        size_t n = strlen(name);
        if (n >= sizeof(vfds[i].name)) n = sizeof(vfds[i].name) - 1;
        memcpy(vfds[i].name, name, n);
        vfds[i].name[n] = '\0';
        return i;
    }
    return VFD_INVALID;
}

static vfd_entry_t *vfd_get(int vfd)
{
    if (vfd < 0 || vfd >= VFD_MAX) return 0;
    if (!vfds[vfd].in_use)         return 0;
    return &vfds[vfd];
}

static void vfd_free(int vfd)
{
    if (vfd < 0 || vfd >= VFD_MAX) return;
    vfds[vfd].in_use = 0;
}

/* Last DOB_ERR_REJECTED message, sticky until the next reply is
 * captured. Populated by capture_rejection() whenever a driver
 * returns REJECTED with a payload; cleared on every successful (or
 * non-REJECTED-error) call. Process-local — single-threaded so no
 * locking needed.
 *
 * Public access via dobfs_last_rejection(); intended use is:
 *     int n = dobfs_Write(fd, buf, len);
 *     if (n < 0) {
 *         const char *why = dobfs_last_rejection();
 *         if (*why) dobpopup_Error("File", why);
 *         else      ... generic error ...
 *     }
 * Empty string means "the last call wasn't a REJECTED, so the
 * stored reason is stale and meaningless".
 *
 * Both the buffer (g_last_rejection) and LAST_REJECTION_CAP are
 * forward-declared above call_via so the hot dispatch paths can
 * reach them; only the function bodies live here. */

static void capture_rejection(const dob_msg_t *reply)
{
    g_last_rejection[0] = '\0';
    if ((int32_t)reply->code != DOB_ERR_REJECTED) return;
    if (!reply->payload || reply->payload_size == 0) return;
    uint32_t n = reply->payload_size;
    if (n > LAST_REJECTION_CAP - 1) n = LAST_REJECTION_CAP - 1;
    memcpy(g_last_rejection, reply->payload, n);
    /* Defensive NUL: payload might already be terminated, but we
     * don't trust the sender to have done it within the announced
     * size — terminate at the truncation point too. */
    g_last_rejection[n] = '\0';
    /* If a NUL appears earlier inside the copy, strlen will stop
     * there — no need to scan & enforce. */
}

const char *dobfs_last_rejection(void)
{
    return g_last_rejection;
}

/* Per-vfd dispatch — the reconnect helper updates port in place. */
static dob_status_t vfd_call(vfd_entry_t *e, dob_msg_t *msg, dob_msg_t *reply)
{
    dob_status_t st = _dob_call_reconnect(&e->port, e->name, 10000, msg, reply);
    if (st == DOB_OK) capture_rejection(reply);
    else              g_last_rejection[0] = '\0';
    return st;
}

/* Public API */

/* Open routed to an explicit service, bypassing path-prefix routing.
 * Used by cross-service operations (e.g. copy from CD to HDD) where
 * the caller already knows which backend owns the source. */
int dobfs_OpenOn(const char *service, const char *path, uint32_t flags)
{
    if (!service || !*service || !path) return -1;

    uint32_t port = 0;   /* let _dob_call_reconnect resolve it */

    dob_msg_t msg = {0}, reply = {0};
    msg.code         = DOBFS_OPEN;
    msg.arg0         = flags;
    msg.payload      = (void *)path;
    msg.payload_size = strlen(path) + 1;

    if (_dob_call_reconnect(&port, service, 10000, &msg, &reply) != DOB_OK)
    {
        g_last_rejection[0] = '\0';
        return -1;
    }
    capture_rejection(&reply);
    if ((int32_t)reply.code < 0) return -1;

    int real_fd = (int)reply.arg0;
    int vfd     = vfd_alloc(real_fd, port, service);
    if (vfd == VFD_INVALID)
    {
        dob_msg_t cm = {0}, cr = {0};
        cm.code = DOBFS_CLOSE;
        cm.arg0 = (uint32_t)real_fd;
        _dob_call_reconnect(&port, service, 10000, &cm, &cr);
        return -1;
    }
    return vfd;
}

int dobfs_Open(const char *path, uint32_t flags)
{
    if (!path) return -1;

    /* Service-qualified path "service:/realpath" — a ':' before the first
     * '/', with '/' immediately after it. Lets a cross-process handoff
     * (e.g. a save dialog mounted on a removable volume) name the target
     * FS service explicitly: a plain "/file" would otherwise resolve to
     * THIS process's SVC_DEFAULT (its own boot FS). FAT names can't hold
     * a ':' and real paths always start with '/', so this never collides
     * with a normal path. The vfd->(service,real_fd) map set up by
     * dobfs_OpenOn then carries every later Write/Close to the right FS. */
    {
        const char *colon = strchr(path, ':');
        const char *slash = strchr(path, '/');
        if (colon && colon != path && colon[1] == '/'
            && (!slash || colon < slash))
        {
            char svc[64];
            size_t n = (size_t)(colon - path);
            if (n >= sizeof(svc)) n = sizeof(svc) - 1;
            memcpy(svc, path, n);
            svc[n] = '\0';
            return dobfs_OpenOn(svc, colon + 1, flags);
        }
    }

    service_id_t sid = service_id_for_path(path);
    if (sid == SVC_DEFAULT && !mount_inherit_attempted)
        try_inherit_default_mount();

    const char *name = service_name_for(sid);
    return dobfs_OpenOn(name, path, flags);
}

int dobfs_Read(int vfd, void *buf, uint32_t max)
{
    if (!buf) return -1;
    vfd_entry_t *e = vfd_get(vfd);
    if (!e) return -1;
    if (max == 0) return 0;

    /* Transport virtualisation: some backends (floppy) cap a single
     * READ reply at one sector. Loop until we either fill the buffer,
     * hit real EOF (reply.arg0 == 0), or hit an error. POSIX-style
     * partial-then-error behaviour: on error after some bytes we
     * return the bytes; the next call will see the error fresh. */

    uint8_t *out   = (uint8_t *)buf;
    uint32_t total = 0;

    while (total < max)
    {
        dob_msg_t msg = {0}, reply = {0};
        msg.code = DOBFS_READ;
        msg.arg0 = (uint32_t)e->real_fd;
        msg.arg1 = max - total;

        if (vfd_call(e, &msg, &reply) != DOB_OK)
            return total > 0 ? (int)total : -1;
        if ((int32_t)reply.code < 0)
            return total > 0 ? (int)total : -1;

        int bytes = (int)reply.arg0;
        if (bytes <= 0) break;          /* EOF */

        uint32_t copy = (uint32_t)bytes;
        if (copy > max - total) copy = max - total;
        if (reply.payload && reply.payload_size > 0)
            memcpy(out + total, reply.payload, copy);
        total += copy;
    }

    return (int)total;
}

int dobfs_Write(int vfd, const void *buf, uint32_t len)
{
    if (!buf) return -1;
    vfd_entry_t *e = vfd_get(vfd);
    if (!e) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code         = DOBFS_WRITE;
    msg.arg0         = (uint32_t)e->real_fd;
    msg.payload      = (void *)buf;
    msg.payload_size = len;

    if (vfd_call(e, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0)             return -1;
    return (int)reply.arg0;
}

int dobfs_Close(int vfd)
{
    vfd_entry_t *e = vfd_get(vfd);
    if (!e) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = DOBFS_CLOSE;
    msg.arg0 = (uint32_t)e->real_fd;

    dob_status_t st = vfd_call(e, &msg, &reply);
    vfd_free(vfd);
    return st == DOB_OK ? (int)reply.code : -1;
}

/* Reposition an open fd's read/write cursor. Returns the new absolute
 * position (>= 0) on success, -1 on error. */
int64_t dobfs_Seek(int vfd, int64_t offset, int whence)
{
    vfd_entry_t *e = vfd_get(vfd);
    if (!e) return -1;

    dob_msg_t msg = {0}, reply = {0};
    msg.code = DOBFS_SEEK;
    msg.arg0 = (uint32_t)e->real_fd;
    msg.arg1 = (uint32_t)(uint64_t)offset;            /* offset low 32  */
    msg.arg2 = (uint32_t)whence;
    msg.arg3 = (uint32_t)((uint64_t)offset >> 32);    /* offset high 32 */

    if (vfd_call(e, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0)             return -1;
    return (int64_t)(((uint64_t)reply.arg1 << 32) | (uint64_t)reply.arg0);
}

int dobfs_Stat(const char *path, dobfs_stat_t *out)
{
    if (!path || !out) return -1;

    /* Service-qualified path "service:/realpath" — see dobfs_Open. */
    {
        const char *colon = strchr(path, ':');
        const char *slash = strchr(path, '/');
        if (colon && colon != path && colon[1] == '/'
            && (!slash || colon < slash))
        {
            char svc[64];
            size_t n = (size_t)(colon - path);
            if (n >= sizeof(svc)) n = sizeof(svc) - 1;
            memcpy(svc, path, n);
            svc[n] = '\0';
            return dobfs_StatOn(svc, colon + 1, out);
        }
    }

    service_id_t sid = service_id_for_path(path);

    dob_msg_t msg = {0}, reply = {0};
    msg.code         = DOBFS_STAT;
    msg.payload      = (void *)path;
    msg.payload_size = strlen(path) + 1;

    if (call_via(sid, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0)               return -1;

    memset(out, 0, sizeof(dobfs_stat_t));
    out->size = ((uint64_t)reply.arg2 << 32) | (uint64_t)reply.arg0;  /* size_hi:size_lo */
    out->type = reply.arg1 ? FS_TYPE_DIR : FS_TYPE_FILE;
    return 0;
}

/* Stat routed to an explicit service. Mirror of dobfs_OpenOn. */
int dobfs_StatOn(const char *service, const char *path, dobfs_stat_t *out)
{
    if (!service || !*service || !path || !out) return -1;

    uint32_t port = 0;
    dob_msg_t msg = {0}, reply = {0};
    msg.code         = DOBFS_STAT;
    msg.payload      = (void *)path;
    msg.payload_size = strlen(path) + 1;

    if (_dob_call_reconnect(&port, service, 10000, &msg, &reply) != DOB_OK)
    {
        g_last_rejection[0] = '\0';
        return -1;
    }
    capture_rejection(&reply);
    if ((int32_t)reply.code < 0)               return -1;

    memset(out, 0, sizeof(dobfs_stat_t));
    out->size = ((uint64_t)reply.arg2 << 32) | (uint64_t)reply.arg0;  /* size_hi:size_lo */
    out->type = reply.arg1 ? FS_TYPE_DIR : FS_TYPE_FILE;
    return 0;
}

int dobfs_List(const char *path, dobfs_dirent_t *entries, uint32_t max, uint32_t *count)
{
    if (!path || !entries || !count) return -1;

    service_id_t sid = service_id_for_path(path);

    dob_msg_t msg = {0}, reply = {0};
    msg.code         = DOBFS_READDIR;
    msg.payload      = (void *)path;
    msg.payload_size = strlen(path) + 1;

    if (call_via(sid, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0) return (int)reply.code;

    *count = 0;
    if (!reply.payload || reply.payload_size == 0)
        return 0;

    char dirbuf[4096];
    uint32_t copy_len = reply.payload_size;
    if (copy_len >= sizeof(dirbuf)) copy_len = sizeof(dirbuf) - 1;
    memcpy(dirbuf, reply.payload, copy_len);
    dirbuf[copy_len] = '\0';

    /* Parse "name\tD|F\tsize\n" per line — legacy DobFileSystem format.
     * The floppy service returns plain "name\n" lines (no type, no
     * size); the missing-tab branch below treats them as regular
     * files with size 0. */
    const char *p   = dirbuf;
    const char *end = dirbuf + copy_len;

    /* Trim any trailing NUL terminators a server may have folded
     * into payload_size. Two of the three readdir servers in tree
     * (DobArchive, floppy) publish `o + 1` so that a NUL rides along
     * at end-1; without this trim the parser sees that byte as a
     * one-char line whose nlen==1 slips past the `nlen == 0` guard
     * below, producing a phantom empty entry at the top of every
     * satellite listing. Trim once here and the parser stays honest
     * regardless of which server is talking. */
    while (end > p && end[-1] == '\0') end--;

    while (p < end && *count < max)
    {
        const char *nl = p;
        while (nl < end && *nl != '\n') nl++;
        if (nl == p) { p = nl + 1; continue; }

        const char *tab1 = p;
        while (tab1 < nl && *tab1 != '\t') tab1++;

        uint32_t nlen = (uint32_t)(tab1 - p);
        if (nlen == 0 || nlen >= 256) { p = nl + 1; continue; }

        memset(&entries[*count], 0, sizeof(dobfs_dirent_t));
        memcpy(entries[*count].name, p, nlen);
        entries[*count].name[nlen] = '\0';

        if (tab1 < nl)
        {
            const char *type_ptr = tab1 + 1;
            entries[*count].type = (*type_ptr == 'D') ? FS_TYPE_DIR : FS_TYPE_FILE;

            const char *tab2 = type_ptr;
            while (tab2 < nl && *tab2 != '\t') tab2++;
            if (tab2 < nl)
            {
                const char *sp = tab2 + 1;
                uint32_t sz = 0;
                while (sp < nl && *sp >= '0' && *sp <= '9')
                    sz = sz * 10 + (*sp++ - '0');
                entries[*count].size = sz;
            }
        }
        else
        {
            entries[*count].type = FS_TYPE_FILE;
        }

        (*count)++;
        p = nl + 1;
    }

    return 0;
}

int dobfs_Mkdir(const char *path)
{
    if (!path) return -1;
    service_id_t sid = service_id_for_path(path);

    dob_msg_t msg = {0}, reply = {0};
    msg.code         = DOBFS_MKDIR;
    msg.payload      = (void *)path;
    msg.payload_size = strlen(path) + 1;
    if (call_via(sid, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0)               return (int)reply.code;
    return 0;
}

/* Mirror of dobfs_Mkdir routed to an explicit service. Used by
 * cross-service writers (DobLiveSetup, future migrations) that need
 * to mkdir on a backend whose path prefixes don't match its identity
 * — e.g. the install target's "/SYSTEM/PROGRAMS" lives under the
 * dobfs_<id> mount even though service_id_for_path would dispatch
 * the same string to the root mount. */
int dobfs_MkdirOn(const char *service, const char *path)
{
    if (!service || !*service || !path) return -1;
    uint32_t port = 0;
    dob_msg_t msg = {0}, reply = {0};
    msg.code         = DOBFS_MKDIR;
    msg.payload      = (void *)path;
    msg.payload_size = strlen(path) + 1;
    if (_dob_call_reconnect(&port, service, 10000, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0)                                            return (int)reply.code;
    return 0;
}

int dobfs_Unlink(const char *path)
{
    if (!path) return -1;
    service_id_t sid = service_id_for_path(path);

    dob_msg_t msg = {0}, reply = {0};
    msg.code         = DOBFS_UNLINK;
    msg.payload      = (void *)path;
    msg.payload_size = strlen(path) + 1;
    if (call_via(sid, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0)               return (int)reply.code;
    return 0;
}

int dobfs_Rename(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path) return -1;

    /* Cross-filesystem rename is not supported — both paths must
     * belong to the same service. */
    service_id_t os = service_id_for_path(old_path);
    service_id_t ns = service_id_for_path(new_path);
    if (os != ns) return -1;

    uint32_t olen = strlen(old_path);
    uint32_t nlen = strlen(new_path);
    char buf[512];
    if (olen + 1 + nlen + 1 > sizeof(buf)) return -1;
    memcpy(buf, old_path, olen);
    buf[olen] = '\0';
    memcpy(buf + olen + 1, new_path, nlen);
    buf[olen + 1 + nlen] = '\0';

    dob_msg_t msg = {0}, reply = {0};
    msg.code         = DOBFS_RENAME;
    msg.payload      = buf;
    msg.payload_size = olen + 1 + nlen + 1;
    if (call_via(os, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0)              return (int)reply.code;
    return 0;
}

/* === Diagnostic / introspection helpers === */

/* Query a specific dobfs_* instance: who is it mounted on, which
 * provider, which partition. Used by DobDisk and DobLiveSetup to
 * map mounted partitions back to disk indices, and to detect the
 * root mount. Caller passes the service name ("DobFileSystem" for
 * the root mount, "dobfs_<token>" for a secondary).
 *
 * Non-blocking by design — these are probes from event handlers
 * (UI selection-change, etc) where the service often legitimately
 * doesn't exist (partition not mounted). dob_registry_find returns
 * 0 immediately when the service isn't registered; we surface that
 * as -1 to the caller. */
int dobfs_GetMountedOn(const char *service, dobfs_mounted_info_t *out)
{
    if (!service || !*service || !out) return -1;
    uint32_t port = dob_registry_find(service);
    if (!port) return -1;
    dob_msg_t msg = {0}, reply = {0};
    msg.code = DOBFS_GET_MOUNTED;
    if (dob_ipc_call(port, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0) return -1;
    if (!reply.payload || reply.payload_size < sizeof(*out)) return -1;
    memcpy(out, reply.payload, sizeof(*out));
    return 0;
}

/* Query disk-usage figures of a mounted instance. Non-blocking,
 * same rationale as dobfs_GetMountedOn. */
int dobfs_DFOn(const char *service, dobfs_df_info_t *out)
{
    if (!service || !*service || !out) return -1;
    uint32_t port = dob_registry_find(service);
    if (!port) return -1;
    dob_msg_t msg = {0}, reply = {0};
    msg.code = DOBFS_DF;
    if (dob_ipc_call(port, &msg, &reply) != DOB_OK) return -1;
    if ((int32_t)reply.code < 0) return -1;
    if (!reply.payload || reply.payload_size < sizeof(*out)) return -1;
    memcpy(out, reply.payload, sizeof(*out));
    return 0;
}
