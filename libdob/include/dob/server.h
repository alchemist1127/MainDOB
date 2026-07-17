#ifndef MAINDOB_DOB_SERVER_H
#define MAINDOB_DOB_SERVER_H

#include <dob/types.h>
#include <dob/ipc.h>

/* Handler type: receives message, writes reply. Returns status. */
typedef dob_status_t (*dob_handler_fn)(dob_msg_t *msg, dob_msg_t *reply);

/* Initialize server framework. Creates IPC port and registers with registry.
 * name: service name (es. "emailclient"). */
dob_status_t dob_server_init(const char *name);

/* Multi-instance variant: first free of {base, base_1..base_7}; the
 * winning name is written to out. The kernel registry arbitrates. */
dob_status_t dob_server_init_unique(const char *base, char *out, int cap);

/* Split init: create the port now, claim the service name later (after
 * attach/hardware-init decide WHICH name). See server.c for rationale. */
dob_status_t dob_server_init_noreg(void);
dob_status_t dob_server_claim_name(const char *name);
void dob_server_adopt_port(uint32_t port);

/* Register the message handler. Dispatch is by msg->code (opcode); the
 * handler does its own switch. */
dob_status_t dob_server_register(dob_handler_fn handler);

/* Main server loop: receive messages, dispatch to handler, reply.
 * Never returns. */
void dob_server_loop(void);

/* Return this server IPC port ID */
uint32_t dob_server_get_port(void);

/* Ask dob_server_loop to return after replying to the CURRENT message.
 * For servers whose reason to exist can disappear (removable media). */
void dob_server_request_exit(void);

#endif /* MAINDOB_DOB_SERVER_H */
