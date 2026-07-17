#include <dob/ipc.h>
#include <sys/syscall.h>

dob_status_t dob_ipc_call(uint32_t port_id, dob_msg_t *msg, dob_msg_t *reply)
{
    return (dob_status_t)syscall3(SYS_SEND, (int)port_id, (int)msg, (int)reply);
}

dob_status_t dob_ipc_receive(uint32_t port_id, dob_msg_t *msg)
{
    return (dob_status_t)syscall2(SYS_RECEIVE, (int)port_id, (int)msg);
}

dob_status_t dob_ipc_receive_nowait(uint32_t port_id, dob_msg_t *msg)
{
    return (dob_status_t)syscall2(SYS_RECEIVE_NOWAIT, (int)port_id, (int)msg);
}

dob_status_t dob_ipc_post(uint32_t port_id, dob_msg_t *msg)
{
    return (dob_status_t)syscall2(SYS_POST, (int)port_id, (int)msg);
}

uint32_t dob_ipc_port_generation(uint32_t port_id)
{
    return (uint32_t)syscall1(SYS_PORT_GEN, (int)port_id);
}

dob_status_t dob_ipc_post_checked(uint32_t port_id, uint32_t gen,
                                  dob_msg_t *msg)
{
    return (dob_status_t)syscall3(SYS_POST_CK, (int)port_id, (int)gen,
                                  (int)msg);
}

int32_t dob_port_watch(uint32_t target_port, uint32_t target_gen,
                       uint32_t notify_port, uint32_t code)
{
    return (int32_t)syscall4(SYS_WATCH_PORT, (int)target_port,
                             (int)target_gen, (int)notify_port, (int)code);
}

dob_status_t dob_ipc_reply(tid_t sender_tid, dob_msg_t *reply)
{
    return (dob_status_t)syscall2(SYS_REPLY, (int)sender_tid, (int)reply);
}

dob_status_t dob_ipc_notify(uint32_t port_id, uint32_t bits)
{
    return (dob_status_t)syscall2(SYS_NOTIFY, (int)port_id, (int)bits);
}

uint32_t dob_ipc_wait_notify(uint32_t port_id)
{
    return (uint32_t)syscall1(SYS_WAIT_NOTIFY, (int)port_id);
}

int32_t dob_ipc_port_create(void)
{
    return (int32_t)syscall0(SYS_PORT_CREATE);
}
