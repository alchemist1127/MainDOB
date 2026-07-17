#ifndef MAINDOB_IPC_PORT_H
#define MAINDOB_IPC_PORT_H

#include "lib/types.h"
#include "lib/list.h"
#include "ipc/message.h"
#include "proc/wait.h"
#include "sync/spinlock.h"
#include "krt/refcount.h"

/* Porta IPC.
 *   - Vita a refcount (krt/refcount.h): il puntatore di ipc_port_get
 *     resta valido fino a ipc_port_put anche con un destroy concorrente.
 *   - Generazione per-id fuori dallo struct: sopravvive al riciclo
 *     dello slot, cosi' un sender distingue "stesso id" da "stessa
 *     porta" (difesa ABA).
 *   - pending_senders: il destroy sveglia tutti i sender sincroni in
 *     attesa (crash del server -> i client ricevono IPC_ERR_DEAD). */

#define PORT_QUEUE_MAX  256u    /* messaggi accodabili per porta: dimensionato
                                 * sui burst di fan-out eventi (dobinterface,
                                 * hotplug) del sistema reale */
#define MAX_PORTS       4096u   /* id validi: 1..MAX_PORTS-1 (0 = invalido,
                                 * e' il "non trovato" del registry) */

typedef enum
{
    PORT_ACTIVE,
    PORT_CLOSED
} port_state_t;

typedef struct
{
    list_node_t    node;
    ipc_message_t  msg;
    bool           payload_owned;   /* true: l'entry possiede msg.payload
                                     * (kfree al cleanup)                 */
} port_msg_entry_t;

typedef struct ipc_port
{
    uint32_t        id;
    pid_t           owner_pid;
    port_state_t    state;
    spinlock_t      lock;
    uint32_t        generation;     /* copia della gen per-id al create   */
    refcount_t      refcount;       /* B1 (vedi sopra); nasce a 1 = slot  */

    list_t          msg_queue;      /* coda messaggi in ingresso          */
    uint32_t        msg_count;
    wait_queue_t    recv_waiters;

    list_t          pending_senders;    /* pending_reply_t verso questa
                                         * porta (sotto port->lock)       */

    volatile uint32_t notify_bits;      /* notifiche async (bitmask OR)   */
    wait_queue_t    notify_waiters;

    list_node_t     owner_node;     /* link in process->owned_ports:
                                     * cleanup O(porte del processo)      */
} ipc_port_t;

void        ipc_port_init(void);

/* Crea una porta. Ritorna l'id (>0) o un errore IPC_ERR_* negativo. */
int32_t     ipc_port_create(pid_t owner_pid);

/* Distrugge la porta: sveglia receiver, notify-waiter e sender sincroni
 * in attesa (che escono con IPC_ERR_DEAD). */
void        ipc_port_destroy(uint32_t port_id);

/* Porta per id, con REFERENZA (B1): valida fino a ipc_port_put.
 * NULL se nessuna porta viva ha quell'id. */
ipc_port_t *ipc_port_get(uint32_t port_id);

/* Come ipc_port_get, ma verifica anche la GENERAZIONE per-id: NULL se
 * la porta e' chiusa O se e' stata riciclata (gen diversa da quella
 * attesa). E' la difesa ABA cablata sul percorso di consegna: un id
 * riusato da un altro processo fallisce qui invece di consegnargli il
 * messaggio di un binding vecchio. Referenza come get: chiudere con put. */
ipc_port_t *ipc_port_get_checked(uint32_t port_id, uint32_t gen);

/* Generazione corrente della porta viva a quell'id (0 = nessuna porta
 * viva: una porta attiva non legge mai gen 0, pre-incrementata al
 * create). Snapshot da conservare per una consegna verificata piu'
 * tardi. */
uint32_t ipc_port_generation(uint32_t port_id);

/* Rilascia la referenza di ipc_port_get. Ultimo put su porta distrutta
 * = teardown reale. NULL e' no-op. */
void        ipc_port_put(ipc_port_t *port);

/* === Death-watch: notifica event-driven di morte di una porta ========= */

/* Init della tabella watch. Prima di qualunque registrazione. */
void        deathwatch_init(void);

/* Registra: alla morte di (target_port, target_gen) posta `code` su
 * `notify_port`. Ritorna 0 (osservando), 1 (target gia' morto: mieta
 * ora), -1 (tabella piena). Deduplicato su (notify_port, target). */
int32_t     deathwatch_add(int32_t watcher_pid, uint32_t notify_port,
                           uint32_t code, uint32_t target_port,
                           uint32_t target_gen);

/* Sgancia tutti i watch registrati da `watcher_pid` (che sta morendo).
 * Chiamata dal teardown del processo. */
void        deathwatch_cleanup_watcher(int32_t watcher_pid);

#endif /* MAINDOB_IPC_PORT_H */
