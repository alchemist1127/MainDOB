#ifndef MAINDOB_DOB_SPAWN_H
#define MAINDOB_DOB_SPAWN_H

/* spawn_file() — launch a .mdl with optional argv.
 *
 * Reads the file via DobFileSystem and spawns a process from its bytes.
 * The heavy work runs in a background thread (dob_thread_spawn), so the
 * caller returns immediately:
 *   1  = submission accepted, work deferred
 *   -1 = submission failed (OOM, path too long, argv too large)
 *
 * argv parameter
 * --------------
 *   NULL   — no arguments. The child sees argc=0, argv[0]=NULL.
 *   array  — NULL-terminated array of C strings, e.g.:
 *                const char *av[] = { "--uninstall", bubble, NULL };
 *                spawn_file("/path/to/bin.mdl", av);
 *            The child receives argc=2 and argv[0]="--uninstall",
 *            argv[1]=bubble at main() entry.
 *
 * Note: argv[0] is NOT conventionally the program name on MainDOB —
 * it is the first real argument. Callers should not emulate the
 * Unix convention of duplicating the binary name as argv[0]; there
 * is no interpreter-shebang path that would need it.
 *
 * Total argv payload (argc count + all strings including NULs + 8-byte
 * header) must stay under 4096 bytes. That limit is enforced in the
 * kernel too; userspace gets a fast -1 when it would be exceeded.
 *
 * spawn_file does not return the new PID — by design, it doesn't
 * exist yet when we return. If you need the PID (e.g. to promote
 * to driver) use spawn_file_driver() which performs make_driver()
 * inside the worker, or spawn_file_sync() for the boot-time case
 * where you can afford the stall.
 */

#include <unistd.h>
#include <string.h>
#include <DobFileSystem.h>
#include <stdlib.h>
#include <dob/thread.h>

/* Mirror of kernel's USER_ARGV_MAX_BLOB_BYTES. Duplicated rather than
 * shared because the kernel header pulls in structs we don't want in
 * userspace; the build will still reject any value over this cap. */
#define SPAWN_ARGV_MAX_BLOB   4096

/* ---------------------------------------------------------------------
 *  argv blob serialisation (internal)
 * ---------------------------------------------------------------------
 *
 * Blob wire format expected by the kernel (see sys_spawn_data):
 *     [uint32_t argc]
 *     [uint32_t blob_size]   — total bytes including this header
 *     [argc strings, each NUL-terminated, packed]
 *
 * _spawn_build_blob() produces this buffer from a NULL-terminated
 * argv array. The caller owns the returned buffer and must free it.
 *
 * Returns 0 on success (even when argv is empty/NULL — in which case
 * *out_blob is set to NULL and *out_size to 0, which the kernel treats
 * as "no arguments"). Returns -1 on OOM or when the serialised form
 * would exceed the cap.
 */
static inline int _spawn_build_blob(const char *const argv[],
                                    void **out_blob, uint32_t *out_size)
{
    *out_blob = NULL;
    *out_size = 0;

    if (!argv || !argv[0]) return 0;

    /* First pass: count argc and total string bytes (including NULs). */
    uint32_t argc = 0;
    uint32_t strings_bytes = 0;
    for (int i = 0; argv[i]; i++)
    {
        argc++;
        strings_bytes += (uint32_t)strlen(argv[i]) + 1;
    }

    uint32_t total = 8 + strings_bytes;
    if (total > SPAWN_ARGV_MAX_BLOB) return -1;

    uint8_t *blob = (uint8_t *)malloc(total);
    if (!blob) return -1;

    ((uint32_t *)blob)[0] = argc;
    ((uint32_t *)blob)[1] = total;

    /* Second pass: copy each string including its NUL terminator. */
    uint8_t *p = blob + 8;
    for (uint32_t i = 0; i < argc; i++)
    {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        memcpy(p, argv[i], len);
        p += len;
    }

    *out_blob = blob;
    *out_size = total;
    return 0;
}

/* Synchronous spawn. Reads the .mdl from disk, serialises argv, and
 * calls into the kernel. Returns PID on success, -1 on failure.
 *
 * Mostly used by the boot path and from within async workers — regular
 * code should prefer spawn_file() so the disk I/O doesn't block the UI
 * thread. */
static inline pid_t spawn_file_sync(const char *path, const char *const argv[])
{
    dobfs_stat_t st;
    if (dobfs_Stat(path, &st) < 0) return -1;
    /* Cap at 32 MB. For truly large binaries a streaming ELF loader would
     * be proper; this covers every realistic .mdl. */
    if (st.size == 0 || st.size > 32 * 1024 * 1024) return -1;

    int fd = dobfs_Open(path, FS_READ);
    if (fd < 0) return -1;

    void *buf = malloc(st.size);
    if (!buf) { dobfs_Close(fd); return -1; }

    uint32_t total = 0;
    while (total < st.size)
    {
        int got = dobfs_Read(fd, (char *)buf + total, st.size - total);
        if (got <= 0) break;
        total += (uint32_t)got;
    }
    dobfs_Close(fd);

    if (total == 0) { free(buf); return -1; }

    /* Build the argv blob. Pre-kernel failure (OOM, oversized) aborts
     * the spawn cleanly without involving process_create. */
    void    *blob = NULL;
    uint32_t blob_size = 0;
    if (_spawn_build_blob(argv, &blob, &blob_size) < 0)
    {
        free(buf);
        return -1;
    }

    /* Derive a name hint from the path: basename minus ".mdl" extension.
     * Without this the kernel hardcodes home_dir = /SYSTEM/PROGRAMS/spawned/
     * for every child, which makes config_area_allowed's per-bubble
     * whitelist unreachable (no amount of driver promotion overcomes it). */
    char hint[64];
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') base = p + 1;
    uint32_t i = 0;
    while (base[i] && i < sizeof(hint) - 1) { hint[i] = base[i]; i++; }
    hint[i] = '\0';
    /* Strip .mdl extension if present */
    if (i >= 4 && hint[i-4] == '.' &&
        (hint[i-3] == 'm' || hint[i-3] == 'M') &&
        (hint[i-2] == 'd' || hint[i-2] == 'D') &&
        (hint[i-1] == 'l' || hint[i-1] == 'L'))
        hint[i-4] = '\0';

    pid_t pid = spawn_from_data_named_argv(buf, total, hint, blob);
    free(buf);
    free(blob);
    return pid;
}

/* ---------------------------------------------------------------------
 *  Async worker plumbing
 * ---------------------------------------------------------------------
 *
 * dob_thread_spawn takes a single void* argument. We marshal path + blob
 * + flags through a heap-allocated request struct; the worker owns it
 * and frees all fields on exit.
 *
 * path and blob are both malloc'd by the submitter and transferred to
 * the worker, so there is no ownership ambiguity: the worker always
 * frees them, even on early failure inside spawn_file_sync (spawn_file
 * must not free its copy if dob_thread_spawn succeeded — the worker
 * becomes responsible). */
typedef struct _spawn_req
{
    char    *path;            /* malloc'd */
    void    *blob;             /* malloc'd or NULL */
    uint32_t blob_size;        /* 0 when blob is NULL */
    bool     promote_driver;   /* when true, make_driver(pid) after spawn */
} _spawn_req_t;

static inline void _spawn_worker(void *arg)
{
    _spawn_req_t *req = (_spawn_req_t *)arg;

    /* We deliberately call the low-level kernel entry instead of
     * spawn_file_sync() to avoid re-serialising the blob (we already
     * built it in spawn_file/spawn_file_driver). That means this
     * worker is a partial duplicate of spawn_file_sync's disk/ELF
     * logic — kept short and similar so any future refactor can
     * unify them without surprises. */
    dobfs_stat_t st;
    if (dobfs_Stat(req->path, &st) < 0)
    {
        syscall2(SYS_DEBUG_PRINT, (int)"[swk] stat fail\n", 16);
        goto done;
    }
    if (st.size == 0 || st.size > 32 * 1024 * 1024)
    {
        syscall2(SYS_DEBUG_PRINT, (int)"[swk] bad size\n", 15);
        goto done;
    }

    int fd = dobfs_Open(req->path, FS_READ);
    if (fd < 0)
    {
        syscall2(SYS_DEBUG_PRINT, (int)"[swk] open fail\n", 16);
        goto done;
    }

    void *buf = malloc(st.size);
    if (!buf)
    {
        char dbg[64];
        int n = 0;
        const char *p = "[swk] malloc fail size=";
        while (*p) dbg[n++] = *p++;
        uint32_t v = st.size;
        char tmp[12]; int tn = 0;
        if (v == 0) tmp[tn++] = '0';
        while (v) { tmp[tn++] = '0' + (v % 10); v /= 10; }
        while (tn--) dbg[n++] = tmp[tn];
        dbg[n++] = '\n'; dbg[n] = '\0';
        syscall2(SYS_DEBUG_PRINT, (int)dbg, n);
        dobfs_Close(fd); goto done;
    }

    uint32_t total = 0;
    while (total < st.size)
    {
        int got = dobfs_Read(fd, (char *)buf + total, st.size - total);
        if (got <= 0) break;
        total += (uint32_t)got;
    }
    dobfs_Close(fd);
    if (total == 0)
    {
        syscall2(SYS_DEBUG_PRINT, (int)"[swk] total=0\n", 14);
        free(buf); goto done;
    }

    /* Derive name hint as in spawn_file_sync. */
    char hint[64];
    const char *base = req->path;
    for (const char *p = req->path; *p; p++)
        if (*p == '/') base = p + 1;
    uint32_t i = 0;
    while (base[i] && i < sizeof(hint) - 1) { hint[i] = base[i]; i++; }
    hint[i] = '\0';
    if (i >= 4 && hint[i-4] == '.' &&
        (hint[i-3] == 'm' || hint[i-3] == 'M') &&
        (hint[i-2] == 'd' || hint[i-2] == 'D') &&
        (hint[i-1] == 'l' || hint[i-1] == 'L'))
        hint[i-4] = '\0';

    pid_t pid = spawn_from_data_named_argv(buf, total, hint, req->blob);

    /* Transient-failure retry.  thread_create can fail under
     * spawn-burst load when the kernel's 256-byte slab class is
     * temporarily full of zombie thread_t structs awaiting the
     * idle workqueue.  A short sleep lets idle run, drain its
     * queue (kfree each dead thread_t), and reopen slots in the
     * slab.  One retry is enough; if it still fails the cause is
     * something we can't fix here (ELF malformed, OOM, etc). */
    if (pid < 0)
    {
        syscall2(SYS_DEBUG_PRINT,
                 (int)"[swk] spawn -1, sleeping 50ms before retry\n", 43);
        sleep_ms(50);
        pid = spawn_from_data_named_argv(buf, total, hint, req->blob);
    }

    free(buf);

    if (pid > 0 && req->promote_driver)
        make_driver(pid);

done:
    free(req->path);
    free(req->blob);
    free(req);
}

/* MainDOB libc has no strdup — inline the equivalent. */
static inline char *_spawn_dup_path(const char *p)
{
    if (!p) return NULL;
    size_t n = strlen(p) + 1;
    char *c = (char *)malloc(n);
    if (!c) return NULL;
    memcpy(c, p, n);
    return c;
}

/* Build a request struct for the async workers. Returns NULL on any
 * allocation failure; on success the caller hands the pointer to
 * dob_thread_spawn and forgets about it (worker owns it). */
static inline _spawn_req_t *_spawn_make_req(const char *path,
                                            const char *const argv[],
                                            bool promote_driver)
{
    _spawn_req_t *req = (_spawn_req_t *)malloc(sizeof(*req));
    if (!req) return NULL;
    req->path            = _spawn_dup_path(path);
    req->blob            = NULL;
    req->blob_size       = 0;
    req->promote_driver  = promote_driver;
    if (!req->path) { free(req); return NULL; }
    if (_spawn_build_blob(argv, &req->blob, &req->blob_size) < 0)
    {
        free(req->path);
        free(req);
        return NULL;
    }
    return req;
}

/* Async fire-and-forget. Returns 1 if the worker was submitted,
 * -1 on immediate failure (OOM, null path, argv too large). The actual
 * spawn may still fail later asynchronously — errors only go to the
 * serial log.
 *
 * argv may be NULL for an argument-less spawn. */
static inline int spawn_file(const char *path, const char *const argv[])
{
    _spawn_req_t *req = _spawn_make_req(path, argv, false);
    if (!req) return -1;
    int tid = dob_thread_spawn(_spawn_worker, req);
    if (tid < 0)
    {
        free(req->path);
        free(req->blob);
        free(req);
        return -1;
    }
    return 1;
}

/* Same as spawn_file, but promotes the new process to driver status
 * after the ELF load completes. Driver promotion happens on the worker
 * thread, so the caller never sees the intermediate PID. Only the
 * caller's own process privileges matter — make_driver requires the
 * caller to be a driver itself. */
static inline int spawn_file_driver(const char *path, const char *const argv[])
{
    _spawn_req_t *req = _spawn_make_req(path, argv, true);
    if (!req) return -1;
    int tid = dob_thread_spawn(_spawn_worker, req);
    if (tid < 0)
    {
        free(req->path);
        free(req->blob);
        free(req);
        return -1;
    }
    return 1;
}

/* Same as spawn_file_driver but takes an already-serialised argv blob
 * instead of an argv array.
 *
 * Used by code that receives the blob over IPC (notably the
 * GUI_SPAWN_DRIVER handler in dobinterface, which forwards a blob
 * built by another process via dobui_SpawnDriver). Deserialising the
 * blob into a char* array just to re-serialise it inside _spawn_worker
 * would be pointless — this entry point lets the blob pass through
 * untouched.
 *
 * The blob is copied: the caller keeps ownership of its own buffer.
 * A NULL blob (or blob_size == 0) means "no arguments", identical to
 * passing argv=NULL to spawn_file_driver. */
static inline int spawn_file_driver_with_blob(const char *path,
                                              const void *blob,
                                              uint32_t blob_size)
{
    _spawn_req_t *req = (_spawn_req_t *)malloc(sizeof(*req));
    if (!req) return -1;
    req->path           = _spawn_dup_path(path);
    req->blob           = NULL;
    req->blob_size      = 0;
    req->promote_driver = true;
    if (!req->path) { free(req); return -1; }

    if (blob && blob_size > 0)
    {
        req->blob = malloc(blob_size);
        if (!req->blob) { free(req->path); free(req); return -1; }
        memcpy(req->blob, blob, blob_size);
        req->blob_size = blob_size;
    }

    int tid = dob_thread_spawn(_spawn_worker, req);
    if (tid < 0)
    {
        free(req->path);
        free(req->blob);
        free(req);
        return -1;
    }
    return 1;
}

#endif
