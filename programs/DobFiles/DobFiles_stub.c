/* DobFiles Stub — File Dialog (path-only)
 *
 * Each picker call spawns its own dedicated DobFiles instance in
 * "--dialog" mode and rendezvous with it over a service name unique
 * to that call. The dialog is therefore always its own process and
 * window — it never reuses or hides an existing file-explorer window,
 * and it works even when no explorer is open. When the user picks a
 * file/location, the reply contains only the path; the caller then
 * reads/writes the file itself via DobFileSystem.
 *
 * Protocol (stub -> the spawned dialog instance):
 *   Open:  code=10, payload = "extensions\0default_path\0"
 *   Save:  code=11, payload = "default_name\0extensions\0default_path\0"
 *   Reply: arg0 = 0 (ok) or -1 (cancel), payload = path string
 */

#include <DobFiles.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/spawn.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* Each picker call spawns its own one-shot DobFiles "--dialog"
 * instance and rendezvous with it through a service name unique to
 * this (process, call). The name fits KREGISTRY_NAME_MAX (32). */
static void make_dialog_svc(char *buf, uint32_t size)
{
    static int counter = 0;
    snprintf(buf, size, "dfdlg.%d.%d", (int)getpid(), ++counter);
}

/* Spawn a fresh DobFiles in dialog mode and wait for it to register.
 * Returns the new instance's port, or 0 on failure. `svc` receives
 * the unique service name (caller buffer, must be >= 32 bytes).
 *
 * This never touches — and never depends on — an existing file
 * explorer window: the dialog is always its own process. */
static uint32_t spawn_dialog(char *svc, uint32_t svc_size)
{
    make_dialog_svc(svc, svc_size);
    const char *argv[] = { "--dialog", svc, NULL };
    if (spawn_file("/SYSTEM/PROGRAMS/DobFiles/DobFiles.mdl", argv) <= 0)
        return 0;
    return dob_registry_wait(svc, 5000);
}

int dobfiles_PickFile(const char *extensions, const char *default_path,
                      char *out_path, uint32_t path_size)
{
    if (!extensions) extensions = "";
    if (!default_path) default_path = "/DATA";

    char svc[32];
    uint32_t port = spawn_dialog(svc, sizeof(svc));
    if (!port) return -1;

    /* Pack: extensions\0default_path\0 */
    char buf[512];
    uint32_t elen = strlen(extensions);
    uint32_t plen = strlen(default_path);
    uint32_t total = elen + 1 + plen + 1;
    if (total > sizeof(buf)) return -1;

    memcpy(buf, extensions, elen);
    buf[elen] = '\0';
    memcpy(buf + elen + 1, default_path, plen);
    buf[elen + 1 + plen] = '\0';

    dob_msg_t msg = {0}, reply = {0};
    msg.code = 10;  /* FILES_CMD_OPEN_DIALOG */
    msg.payload = buf;
    msg.payload_size = total;

    if (dob_ipc_call(port, &msg, &reply) != DOB_OK)
        return -1;

    if ((int32_t)reply.arg0 < 0)
        return -1;

    /* Reply payload = path string */
    if (reply.payload && reply.payload_size > 0 && out_path && path_size > 0)
    {
        uint32_t copy = reply.payload_size < path_size ? reply.payload_size : path_size - 1;
        memcpy(out_path, reply.payload, copy);
        out_path[copy] = '\0';
    }

    return 0;
}

int dobfiles_PickSavePath(const char *default_name, const char *extensions,
                          const char *default_path,
                          char *out_path, uint32_t path_size)
{
    if (!default_name) default_name = "untitled.txt";
    if (!extensions) extensions = "";
    if (!default_path) default_path = "/DATA";

    char svc[32];
    uint32_t port = spawn_dialog(svc, sizeof(svc));
    if (!port) return -1;

    /* Pack: default_name\0extensions\0default_path\0 */
    char buf[512];
    uint32_t nlen = strlen(default_name);
    uint32_t elen = strlen(extensions);
    uint32_t plen = strlen(default_path);
    uint32_t total = nlen + 1 + elen + 1 + plen + 1;
    if (total > sizeof(buf)) return -1;

    uint32_t pos = 0;
    memcpy(buf + pos, default_name, nlen); pos += nlen; buf[pos++] = '\0';
    memcpy(buf + pos, extensions, elen); pos += elen; buf[pos++] = '\0';
    memcpy(buf + pos, default_path, plen); pos += plen; buf[pos++] = '\0';

    dob_msg_t msg = {0}, reply = {0};
    msg.code = 11;  /* FILES_CMD_SAVE_DIALOG */
    msg.payload = buf;
    msg.payload_size = total;

    if (dob_ipc_call(port, &msg, &reply) != DOB_OK)
        return -1;

    if ((int32_t)reply.arg0 < 0)
        return -1;

    if (reply.payload && reply.payload_size > 0 && out_path && path_size > 0)
    {
        uint32_t copy = reply.payload_size < path_size ? reply.payload_size : path_size - 1;
        memcpy(out_path, reply.payload, copy);
        out_path[copy] = '\0';
    }

    return 0;
}

/* Surface the file explorer on an alternate dobfs service (typically
 * a removable-media driver such as "floppy").
 *
 * The third argument carries the originator's intent — propagated
 * end-to-end from ICON_ACTIVATED down to here so this function
 * never has to *guess* what to do:
 *
 *   hijack_port == 0  → spawn a fresh DobFiles satellite. This is
 *                       the desktop-double-click path: the user
 *                       expects a brand-new file-explorer window.
 *
 *   hijack_port != 0  → IPC FILES_CMD_MOUNT directly to that port.
 *                       The caller (a DobFiles in its Monta view,
 *                       passing its own dobui_port) has explicitly
 *                       asked to be the target of the mount. If the
 *                       call fails (window closed in the meantime,
 *                       port stale) we silently fall back to spawn
 *                       — the user's intent was "see this device",
 *                       a new window is better than nothing.
 *
 * Note: no registry lookup for "DobFiles" anywhere. Routing is
 * fully driven by `hijack_port`. The old behaviour of guessing the
 * primary explorer is gone — every caller now declares its intent
 * (or accepts 0 = spawn by passing it explicitly). */
int dobfiles_OpenMount(const char *service, const char *root_path,
                       uint32_t hijack_port)
{
    if (!service || !*service) return -1;
    if (!root_path || !*root_path) root_path = "/";

    /* Step 1: targeted hijack. */
    if (hijack_port)
    {
        char buf[128];
        size_t slen = strlen(service);
        size_t rlen = strlen(root_path);
        if (slen + 1 + rlen + 1 <= sizeof(buf))
        {
            memcpy(buf, service, slen);
            buf[slen] = '\0';
            memcpy(buf + slen + 1, root_path, rlen);
            buf[slen + 1 + rlen] = '\0';

            dob_msg_t msg = {0}, reply = {0};
            msg.code         = 20;   /* FILES_CMD_MOUNT */
            msg.payload      = buf;
            msg.payload_size = (uint32_t)(slen + 1 + rlen + 1);

            if (dob_ipc_call(hijack_port, &msg, &reply) == DOB_OK
                && (int32_t)reply.arg0 == 0)
            {
                return 0;        /* hijack accepted, done */
            }
            /* Fall through to spawn — the targeted window vanished
             * or refused; satellite is the next-best surface. */
        }
    }

    /* Step 2: spawn a satellite. */
    const char *argv[] = { "--mount", service, root_path, NULL };
    pid_t pid = spawn_file("/SYSTEM/PROGRAMS/DobFiles/DobFiles.mdl", argv);
    if (pid <= 0) return -1;
    return 0;
}
