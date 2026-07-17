#ifndef MAINDOB_DOB_IPC_H
#define MAINDOB_DOB_IPC_H

#include <dob/types.h>

/* IPC message structure (mirrors kernel ipc_message_t) */
typedef struct
{
    uint32_t type;
    pid_t    sender_pid;
    tid_t    sender_tid;
    uint32_t code;
    uint32_t arg0, arg1, arg2, arg3;
    uint32_t payload_size;
    void    *payload;
} dob_msg_t;

/* Send synchronous message and wait for reply */
dob_status_t dob_ipc_call(uint32_t port_id, dob_msg_t *msg, dob_msg_t *reply);

/* Receive a message from port */
dob_status_t dob_ipc_receive(uint32_t port_id, dob_msg_t *msg);

/* Non-blocking receive: returns DOB_ERR_NOT_FOUND if no messages pending */
dob_status_t dob_ipc_receive_nowait(uint32_t port_id, dob_msg_t *msg);

/* Async fire-and-forget send: enqueue message, return immediately */
dob_status_t dob_ipc_post(uint32_t port_id, dob_msg_t *msg);

/* Current generation of the live port at `port_id` (0 = no live port).
 * Capture this once at bind time; a later dob_ipc_post_checked using
 * the captured value refuses to deliver if the id has been recycled. */
uint32_t dob_ipc_port_generation(uint32_t port_id);

/* Generation-checked async send: delivers only if the port still
 * carries `gen`. A recycled id returns DOB_ERR_DEAD instead of
 * delivering to whoever now owns that id — the event-driven signal a
 * caller uses to notice its peer died (no polling). */
dob_status_t dob_ipc_post_checked(uint32_t port_id, uint32_t gen,
                                  dob_msg_t *msg);

/* Register an event-driven death-watch: when the port (target_port,
 * target_gen) dies, the kernel posts `code` on `notify_port` with
 * arg0=target_port, arg1=target_gen. Keyed by (id, gen) -> immune to id
 * recycling. Returns 0 (watching), 1 (target already dead: act now),
 * -1 (error). Replaces liveness polling. */
int32_t dob_port_watch(uint32_t target_port, uint32_t target_gen,
                       uint32_t notify_port, uint32_t code);

/* Reply to a message */
dob_status_t dob_ipc_reply(tid_t sender_tid, dob_msg_t *reply);

/* Send async notification */
dob_status_t dob_ipc_notify(uint32_t port_id, uint32_t bits);

/* Wait for notification */
uint32_t dob_ipc_wait_notify(uint32_t port_id);

/* Create an IPC port */
int32_t dob_ipc_port_create(void);

#endif /* MAINDOB_DOB_IPC_H */
