#ifndef MAINDOB_IPC_CHANNEL_H
#define MAINDOB_IPC_CHANNEL_H

#include "lib/types.h"
#include "ipc/message.h"

/* Canale IPC — send/receive/reply/post/notify sopra le porte.
 *
 * Il payload della reply viaggia in un buffer kernel kmalloc'd
 * attaccato al messaggio: sys_reply lo snapshotta (anti-TOCTOU) e lo
 * consegna con ipc_reply_staged, il sender lo copia nel proprio
 * IPC_BUF e lo libera. Nessuno staging sullo stack del sender: niente
 * finestre di use-after-free cross-core su quello stack.
 *
 * Il round-trip usa wake + yield; il direct-switch arrivera' col
 * milestone per-CPU senza cambiare questa API.
 *
 * Ownership dei payload (contratto con syscall.c):
 *   send_sync : msg->payload PRESTATO dal chiamante (resta suo);
 *               reply->payload in uscita e' kernel-kmalloc'd e passa
 *               di proprieta' al chiamante (kfree dopo la consegna).
 *   post      : su successo il payload viene RUBATO (msg->payload
 *               azzerato); su errore resta del chiamante.
 *   reply_staged: payload_kbuf passa SEMPRE di proprieta' al layer
 *               (liberato internamente su ogni errore).
 *   receive   : *payload_owned true = msg->payload e' kernel-kmalloc'd
 *               e va liberato dal chiamante dopo la consegna. */

/* Invio sincrono: accoda su port_id e blocca fino alla reply.
 * Ritorna IPC_OK o errore (IPC_ERR_DEAD se la porta muore durante
 * l'attesa). */
int32_t ipc_send_sync(uint32_t port_id, ipc_message_t *msg,
                      ipc_message_t *reply);

/* Reply al sender bloccato in send_sync. `payload_kbuf` (kernel,
 * kmalloc'd, puo' essere NULL) passa di proprieta': su successo finisce
 * in reply->payload del sender, su errore e' liberato qui. */
int32_t ipc_reply_staged(tid_t sender_tid, ipc_message_t *reply,
                         void *payload_kbuf, uint32_t payload_size);

/* Ricezione (bloccante). Solo il processo proprietario della porta.
 * Le notifiche pendenti sono sintetizzate come IPC_MSG_NOTIFY. */
int32_t ipc_receive(uint32_t port_id, ipc_message_t *msg,
                    bool *payload_owned);

/* Ricezione non bloccante: IPC_ERR_EMPTY se non c'e' nulla. */
int32_t ipc_receive_nowait(uint32_t port_id, ipc_message_t *msg,
                           bool *payload_owned);

/* Invio asincrono fire-and-forget (vedi ownership sopra). */
int32_t ipc_post(uint32_t port_id, ipc_message_t *msg);

/* Come ipc_post, ma consegna solo se la porta ha ancora la generazione
 * `gen` attesa: un id riciclato da un altro processo -> IPC_ERR_DEAD,
 * mai consegna all'estraneo (difesa ABA sul percorso di consegna). */
int32_t ipc_post_checked(uint32_t port_id, uint32_t gen, ipc_message_t *msg);

/* Notifica asincrona: OR dei bit sulla porta, sveglia notify e recv
 * waiter (cosi' una receive in attesa la vede subito). */
int32_t ipc_notify(uint32_t port_id, uint32_t bits);

/* Attende notifiche sulla porta: ritorna i bit accumulati azzerandoli.
 * 0 se la porta muore. */
uint32_t ipc_wait_notify(uint32_t port_id);

/* Sveglia i sender sincroni bloccati verso questa porta (escono con
 * IPC_ERR_DEAD). La variante a puntatore serve al destroy, il cui slot
 * in tabella e' gia' NULL. */
struct ipc_port;
void ipc_wake_senders_for_port_ptr(struct ipc_port *port);

/* Cleanup IPC di un thread che muore: slot pending azzerato sotto il
 * lock per-TID. Da chiamare PRIMA di liberare lo stack kernel. */
void ipc_cleanup_thread(tid_t tid);

/* Distrugge tutte le porte del processo (crash/exit del server: i
 * client ricevono IPC_ERR_DEAD — la bolla auto-riparante). Dopo il
 * cleanup dei thread, prima di liberare l'IPC buffer. */
void ipc_ports_cleanup_owner(pid_t owner_pid);

/* Post da contesto kernel/IRQ (timer, watchdog): pool preallocato,
 * niente kmalloc, mai blocca. Senza payload. */
int32_t ipc_post_kernel(uint32_t port_id, uint32_t code,
                        uint32_t arg0, uint32_t arg1, uint32_t arg2);

/* Rilascio di una port_msg_entry_t (usata anche dal teardown porta). */
struct port_msg_entry;
void ipc_msg_entry_free(void *entry);

void ipc_init(void);

#endif /* MAINDOB_IPC_CHANNEL_H */
