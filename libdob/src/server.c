#include <dob/server.h>
#include <stdio.h>
#include <dob/registry.h>
#include <string.h>

static dob_handler_fn message_handler = NULL;
static uint32_t server_port = 0;

dob_status_t dob_server_init(const char *name)
{
    message_handler = NULL;

    int32_t port = dob_ipc_port_create();
    if (port <= 0)
        return DOB_ERR_INTERNAL;
    server_port = (uint32_t)port;

    /* Propagate the registration verdict. The registry now refuses a name held
     * by a different LIVE owner (non-OK), so a driver that must win a singleton
     * service can check this return and bow out atomically, instead of a racy
     * check-then-register. Callers that don't care simply ignore it (all but one
     * do). A same-pid re-register or a takeover of a dead owner still yields OK. */
    return dob_registry_register(name, server_port);
}

dob_status_t dob_server_register(dob_handler_fn handler)
{
    message_handler = handler;
    return DOB_OK;
}

static bool exit_requested = false;

void dob_server_loop(void)
{
    dob_msg_t msg, reply;

    for (;;)
    {
        /* Minimal reply reset — handler owns the fields it uses.
         * Clear only the fields a caller could legitimately read after reply
         * (code, arg0..3, payload, payload_size). type/sender_pid/sender_tid
         * are overwritten by the kernel in ipc_reply. */
        reply.code = 0;
        reply.arg0 = 0;
        reply.arg1 = 0;
        reply.arg2 = 0;
        reply.arg3 = 0;
        reply.payload = NULL;
        reply.payload_size = 0;

        dob_status_t ret = dob_ipc_receive(server_port, &msg);
        if (ret != DOB_OK)
            continue;

        if (msg.type == 3)
            continue;

        if (message_handler)
            reply.code = (uint32_t)message_handler(&msg, &reply);
        else
            reply.code = (uint32_t)DOB_ERR_NOT_FOUND;

        if (msg.type == 1)
            dob_ipc_reply(msg.sender_tid, &reply);

        /* Deferred exit: a handler may decide the server must die (e.g.
         * DOBFS_SHUTDOWN when a removable medium disappears), but the
         * caller still deserves its reply. The handler sets the flag;
         * we exit only AFTER the reply has gone out. */
        if (exit_requested)
            return;
    }
}

void dob_server_request_exit(void)
{
    exit_requested = true;
}

/* Register under the first free name in {base, base_1 .. base_7}.
 * The kernel registry is the atomic arbiter (SYS_REG_REGISTER fails on
 * a taken name), so concurrent instances cannot collide — the pattern
 * for multi-controller drivers (e.g. five UHCI functions on ICH8).
 * One port is created and reused across attempts. */
dob_status_t dob_server_init_unique(const char *base, char *out, int cap)
{
    message_handler = NULL;

    int32_t port = dob_ipc_port_create();
    if (port <= 0) return DOB_ERR_INTERNAL;
    server_port = (uint32_t)port;

    char name[32];
    for (int n = 0; n < 8; n++)
    {
        if (n == 0) snprintf(name, sizeof(name), "%s", base);
        else        snprintf(name, sizeof(name), "%s_%d", base, n);
        if (dob_registry_register(name, server_port) == DOB_OK)
        {
            if (out && cap > 0) snprintf(out, (size_t)cap, "%s", name);
            return DOB_OK;
        }
    }
    return DOB_ERR_NO_SPACE;
}

/* Create the server port WITHOUT registering any name. For drivers that
 * must talk (hotplug attach, hardware init) before they can decide which
 * name to claim — e.g. the multi-controller AHCI election, where the name
 * "ahci" must go to the instance whose controller actually has disks,
 * which is only known after init_hardware. Pair with
 * dob_server_claim_name() once the decision is made. */
dob_status_t dob_server_init_noreg(void)
{
    message_handler = NULL;
    int32_t port = dob_ipc_port_create();
    if (port <= 0) return DOB_ERR_INTERNAL;
    server_port = (uint32_t)port;
    return DOB_OK;
}

/* Register `name` for the already-created server port. Atomic in the
 * kernel registry: DOB_OK means we own the name; a non-OK return means a
 * different LIVE process holds it (election lost — pick another name or
 * run unnamed). Callable multiple times to claim additional aliases. */
dob_status_t dob_server_claim_name(const char *name)
{
    if (!server_port) return DOB_ERR_INTERNAL;
    return dob_registry_register(name, server_port);
}

/* Adopt an already-created, already-registered port as the server port
 * (for drivers that need VERIFIED registration with custom handling). */
void dob_server_adopt_port(uint32_t port)
{
    message_handler = NULL;
    server_port = port;
}

uint32_t dob_server_get_port(void)
{
    return server_port;
}
