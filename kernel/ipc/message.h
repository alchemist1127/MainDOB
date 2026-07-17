#ifndef MAINDOB_IPC_MESSAGE_H
#define MAINDOB_IPC_MESSAGE_H

#include "lib/types.h"

/* Messaggio IPC. Il layout e' ABI userspace (le syscall lo copiano
 * per intero da/verso l'utente): non toccare ordine, tipi o dimensioni
 * dei campi. */

/* Tipi di messaggio */
#define IPC_MSG_REQUEST     1       /* richiesta sincrona (attende reply) */
#define IPC_MSG_REPLY       2       /* risposta a una richiesta           */
#define IPC_MSG_NOTIFY      3       /* notifica asincrona (bitmask)       */
#define IPC_MSG_SIGNAL      4       /* segnale di sistema / post async    */

/* Codici errore IPC (ABI: l'userspace li confronta) */
#define IPC_OK              0
#define IPC_ERR_NO_PORT     (-1)
#define IPC_ERR_PORT_FULL   (-2)
/* slot -3 riservato (fu IPC_ERR_TIMEOUT: il layer IPC non espone piu'
 * timeout) */
#define IPC_ERR_DENIED      (-4)
#define IPC_ERR_INVALID     (-5)
#define IPC_ERR_DEAD        (-6)    /* processo/porta di destinazione morti */
#define IPC_ERR_NO_MEMORY   (-7)
#define IPC_ERR_EMPTY       (-8)    /* nessun messaggio (receive_nowait)  */

typedef struct
{
    uint32_t type;              /* IPC_MSG_REQUEST / REPLY / NOTIFY / SIGNAL */
    pid_t    sender_pid;        /* impostato dal kernel                      */
    tid_t    sender_tid;
    uint32_t code;              /* codice operazione / risposta              */
    uint32_t arg0, arg1, arg2, arg3;    /* argomenti veloci                  */
    uint32_t payload_size;      /* dimensione payload aggiuntivo             */
    void    *payload;           /* puntatore al payload                      */
} ipc_message_t;

#endif /* MAINDOB_IPC_MESSAGE_H */
