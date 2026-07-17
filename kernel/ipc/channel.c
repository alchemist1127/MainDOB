#include "ipc/channel.h"
#include "krt/reclaim.h"
#include "ipc/port.h"
#include "proc/thread.h"
#include "proc/process.h"
#include "proc/scheduler.h"
#include "proc/percpu.h"
#include "mm/kheap.h"
#include "console/console.h"
#include "lib/string.h"
#include "arch/x86/cpu.h"

/* ======================================================================
 * Allocazione entry messaggio
 *
 * Magazine per-CPU (LIFO) sopra un depot globale: il percorso caldo e'
 * un pop/push sotto solo cli — con IF=0 la CPU e' l'unica a toccare il
 * proprio magazine, nessun lock. Il depot (lock) si tocca solo a
 * magazine vuoto/pieno, a lotti. Pool IRQ separato e preallocato:
 * ipc_post_kernel da timer/IRQ non tocca mai kmalloc ne' blocca.
 * node.next fa da link di freelist: l'entry libera e' fuori da ogni coda.
 * ==================================================================== */

#define IPC_MAG_CAP         64u     /* entry cachate per CPU             */
#define IPC_MAG_BATCH       32u     /* entry mosse per transazione depot */
#define IPC_DEPOT_CACHE_MAX 256u
#define IPC_IRQ_POOL_SIZE   512u

static port_msg_entry_t *s_mag[MAX_CPUS];
static uint32_t          s_mag_n[MAX_CPUS];

static port_msg_entry_t *s_depot;
static uint32_t          s_depot_n;
static spinlock_t        s_depot_lock = SPINLOCK_INIT;

/* Verbo di ritiro (auto-pulizia, vedi krt/reclaim.h): dopo una raffica
 * il depot resta al suo high-water mark; qui le entry oltre il
 * pavimento caldo tornano al kheap. Il pavimento (= un lotto di
 * travaso) garantisce che il prossimo picco non ripaghi il warm-up.
 * Le entry si staccano sotto il lock ma il kfree avviene FUORI: mai
 * heap-lock annidato nel depot-lock. I magazine per-CPU restano
 * intoccati (piccoli, e caldi per definizione). */
static uint32_t ipc_depot_trim(void)
{
    port_msg_entry_t *chain = NULL;
    uint32_t taken = 0;

    spinlock_acquire(&s_depot_lock);
    while (s_depot_n > IPC_MAG_BATCH)
    {
        port_msg_entry_t *e = s_depot;
        s_depot = (port_msg_entry_t *)e->node.next;
        s_depot_n--;
        e->node.next = (list_node_t *)chain;
        chain = e;
        taken++;
    }
    spinlock_release(&s_depot_lock);

    while (chain != NULL)
    {
        port_msg_entry_t *e = chain;
        chain = (port_msg_entry_t *)e->node.next;
        kfree(e);
    }
    return taken * (uint32_t)sizeof(port_msg_entry_t);
}

static port_msg_entry_t  s_irq_pool[IPC_IRQ_POOL_SIZE];
static port_msg_entry_t *s_irq_freelist;
static spinlock_t        s_irq_lock = SPINLOCK_INIT;
static uint32_t          s_irq_drops;

static port_msg_entry_t *entry_alloc(void)
{
    uint32_t fl  = irq_save();          /* blocca la CPU su questo core */
    uint32_t cpu = this_cpu()->cpu_index;

    port_msg_entry_t *e = s_mag[cpu];
    if (e != NULL)                      /* percorso caldo: pop locale   */
    {
        s_mag[cpu] = (port_msg_entry_t *)e->node.next;
        s_mag_n[cpu]--;
        irq_restore(fl);
        return e;
    }

    /* Magazine vuoto: ricarica un lotto dal depot. Sotto cli si resta
     * su questa CPU, quindi il magazine non ha altri toccatori. */
    spinlock_acquire(&s_depot_lock);
    uint32_t moved = 0;
    while (moved < IPC_MAG_BATCH && s_depot != NULL)
    {
        port_msg_entry_t *d = s_depot;
        s_depot = (port_msg_entry_t *)d->node.next;
        s_depot_n--;
        d->node.next = (list_node_t *)s_mag[cpu];
        s_mag[cpu] = d;
        s_mag_n[cpu]++;
        moved++;
    }
    spinlock_release(&s_depot_lock);

    e = s_mag[cpu];
    if (e != NULL)
    {
        s_mag[cpu] = (port_msg_entry_t *)e->node.next;
        s_mag_n[cpu]--;
        irq_restore(fl);
        return e;
    }

    /* Depot vuoto: kmalloc fuori da cli (fuori dal percorso caldo). */
    irq_restore(fl);
    return (port_msg_entry_t *)kmalloc(sizeof(port_msg_entry_t));
}

static port_msg_entry_t *entry_alloc_irq(void)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_irq_lock);
    port_msg_entry_t *e = s_irq_freelist;
    uint32_t drop_n = 0;
    if (e != NULL)
    {
        s_irq_freelist = (port_msg_entry_t *)e->node.next;
    }
    else
    {
        drop_n = ++s_irq_drops;         /* pool esaurito: drop contato */
    }
    spinlock_release_irqrestore(&s_irq_lock, fl);

    /* Un drop qui NON e' statistica: e' un post kernel->user PERSO —
     * tipicamente la scadenza di un timer userspace. Il sintomo a valle
     * e' subdolo (GUI che "salta", FSM driver che si incantano, eventi
     * in ritardo di un giro) e senza questa riga era INVISIBILE: il
     * contatore esisteva ma non parlava mai. Rate-limited, fuori lock. */
    if (drop_n != 0 && (drop_n <= 4u || (drop_n & 255u) == 0u))
    {
        kprintf("[IPC ] POOL IRQ ESAURITO: post kernel perso "
                "(drop #%u) - consumer lenti o pool sottodimensionato\n",
                drop_n);
    }
    return e;
}

void ipc_msg_entry_free(void *ep)
{
    port_msg_entry_t *e = (port_msg_entry_t *)ep;
    if (e == NULL)
    {
        return;
    }

    /* Le entry del pool IRQ tornano SEMPRE alla loro freelist
     * (riconosciute per range di storage). */
    if (e >= &s_irq_pool[0] && e < &s_irq_pool[IPC_IRQ_POOL_SIZE])
    {
        uint32_t fl = spinlock_acquire_irqsave(&s_irq_lock);
        e->node.next = (list_node_t *)s_irq_freelist;
        s_irq_freelist = e;
        spinlock_release_irqrestore(&s_irq_lock, fl);
        return;
    }

    uint32_t fl  = irq_save();
    uint32_t cpu = this_cpu()->cpu_index;

    if (s_mag_n[cpu] < IPC_MAG_CAP)     /* percorso caldo: push locale  */
    {
        e->node.next = (list_node_t *)s_mag[cpu];
        s_mag[cpu] = e;
        s_mag_n[cpu]++;
        irq_restore(fl);
        return;
    }

    /* Magazine pieno: travasa un lotto al depot (le entry migrano
     * liberamente tra CPU: alloc su un core, free su un altro). */
    spinlock_acquire(&s_depot_lock);
    for (uint32_t i = 0; i < IPC_MAG_BATCH && s_depot_n < IPC_DEPOT_CACHE_MAX;
         i++)
    {
        port_msg_entry_t *m = s_mag[cpu];
        s_mag[cpu] = (port_msg_entry_t *)m->node.next;
        s_mag_n[cpu]--;
        m->node.next = (list_node_t *)s_depot;
        s_depot = m;
        s_depot_n++;
    }
    bool room = (s_mag_n[cpu] < IPC_MAG_CAP);
    spinlock_release(&s_depot_lock);

    if (room)
    {
        e->node.next = (list_node_t *)s_mag[cpu];
        s_mag[cpu] = e;
        s_mag_n[cpu]++;
        irq_restore(fl);
        return;
    }
    irq_restore(fl);
    kfree(e);                           /* depot pieno: torna all'heap  */
}

/* ======================================================================
 * Reply pendenti
 *
 * Il sender sincrono blocca con una pending_reply_t sul proprio stack
 * kernel, indicizzata per tid. Lock hashato per-TID: ipc_reply_staged
 * dereferenzia quello stack da un altro contesto, e ipc_cleanup_thread
 * (eseguito prima che il reaper liberi lo stack) prende lo stesso lock
 * — il teardown attende ogni reply in volo.
 * ==================================================================== */

typedef struct
{
    ipc_message_t  *reply_buf;      /* dove il sender vuole la reply      */
    volatile bool   reply_received; /* scritto cross-contesto: mai cache  */
    bool            port_dead;      /* porta distrutta durante l'attesa   */
    uint32_t        target_port_id;
    uint32_t        target_port_gen;    /* snapshot ABA al send           */
    wait_queue_t    reply_wait;
    list_node_t     port_node;      /* link in port->pending_senders      */
} pending_reply_t;

static pending_reply_t *s_pending[MAX_THREADS];

#define PENDING_LOCK_N 256u             /* potenza di 2                   */
static spinlock_t s_pending_lock[PENDING_LOCK_N];

static inline uint32_t pending_lock(tid_t tid)
{
    return spinlock_acquire_irqsave(
        &s_pending_lock[(uint32_t)tid & (PENDING_LOCK_N - 1u)]);
}

static inline void pending_unlock(tid_t tid, uint32_t flags)
{
    spinlock_release_irqrestore(
        &s_pending_lock[(uint32_t)tid & (PENDING_LOCK_N - 1u)], flags);
}

/* ======================================================================
 * Verbi comuni
 * ==================================================================== */

/* Accodamento asincrono unificato (post + post_kernel). Il percorso
 * sincrono NON passa di qui (aggancia pending_senders sotto lo stesso
 * lock). IPC_OK (accodato, receiver svegliato) o IPC_ERR_PORT_FULL
 * (entry e payload posseduto liberati). */
static int32_t enqueue_async(ipc_port_t *port, port_msg_entry_t *entry)
{
    uint32_t flags = spinlock_acquire_irqsave(&port->lock);
    if (unlikely(port->msg_count >= PORT_QUEUE_MAX))
    {
        spinlock_release_irqrestore(&port->lock, flags);

        /* Diagnostica rate-limited: una porta piena e' quasi sempre un
         * consumer che ha perso un wake e dorme, non un consumer lento. */
        static volatile uint32_t s_full_reports;
        uint32_t n = atomic_add_return(&s_full_reports, 1) - 1;
        if (n < 8u || (n & 1023u) == 0u)
        {
            kprintf("[IPC ] porta %u (owner PID %d) PIENA (%u msg) - "
                    "evento perso (occorrenza %u). Receiver addormentato "
                    "o in stallo.\n",
                    port->id, port->owner_pid, PORT_QUEUE_MAX, n + 1);
        }

        if (entry->payload_owned && entry->msg.payload != NULL)
        {
            kfree(entry->msg.payload);
        }
        ipc_msg_entry_free(entry);
        return IPC_ERR_PORT_FULL;
    }
    list_push_back(&port->msg_queue, &entry->node);
    port->msg_count++;
    spinlock_release_irqrestore(&port->lock, flags);

    wait_queue_wake_one(&port->recv_waiters);
    return IPC_OK;
}

/* Estrae il prossimo elemento consumabile dalla porta, sotto
 * port->lock del chiamante NON tenuto (lo prende qui). Ritorna:
 *   IPC_OK        — messaggio o notifica scritti in *msg
 *   IPC_ERR_DEAD  — porta chiusa e coda vuota
 *   IPC_ERR_EMPTY — niente da consumare (porta viva)                  */
static int32_t try_consume(ipc_port_t *port, ipc_message_t *msg,
                           bool *payload_owned)
{
    uint32_t flags = spinlock_acquire_irqsave(&port->lock);

    if (!list_empty(&port->msg_queue))
    {
        list_node_t *node = list_pop_front(&port->msg_queue);
        port->msg_count--;
        spinlock_release_irqrestore(&port->lock, flags);

        port_msg_entry_t *entry = list_entry(node, port_msg_entry_t, node);
        memcpy(msg, &entry->msg, sizeof(ipc_message_t));
        *payload_owned = entry->payload_owned;
        ipc_msg_entry_free(entry);
        return IPC_OK;
    }

    if (port->notify_bits != 0)
    {
        uint32_t bits = port->notify_bits;
        port->notify_bits = 0;
        spinlock_release_irqrestore(&port->lock, flags);

        /* Notifica sintetizzata come messaggio: un solo punto di
         * attesa per il chiamante. */
        memset(msg, 0, sizeof(ipc_message_t));
        msg->type = IPC_MSG_NOTIFY;
        msg->arg0 = bits;
        *payload_owned = false;
        return IPC_OK;
    }

    port_state_t st = port->state;
    spinlock_release_irqrestore(&port->lock, flags);
    return (st != PORT_ACTIVE) ? IPC_ERR_DEAD : IPC_ERR_EMPTY;
}

/* ======================================================================
 * API
 * ==================================================================== */

int32_t ipc_send_sync(uint32_t port_id, ipc_message_t *msg,
                      ipc_message_t *reply)
{
    if (msg == NULL || reply == NULL)
    {
        return IPC_ERR_INVALID;
    }

    ipc_port_t *port = ipc_port_get(port_id);
    if (port == NULL)
    {
        return IPC_ERR_NO_PORT;
    }

    int32_t ret;
    thread_t *self = current_thread;
    if (self == NULL || self->owner == NULL
        || self->tid < 0 || self->tid >= MAX_THREADS)
    {
        ret = IPC_ERR_INVALID;
        goto out;
    }
    if (port->state != PORT_ACTIVE)
    {
        ret = IPC_ERR_NO_PORT;
        goto out;
    }

    msg->sender_pid = self->owner->pid;
    msg->sender_tid = self->tid;
    msg->type       = IPC_MSG_REQUEST;

    /* Pending sul NOSTRO stack: stabile finche' siamo bloccati. */
    pending_reply_t pending;
    pending.reply_buf       = reply;
    pending.reply_received  = false;
    pending.port_dead       = false;
    pending.target_port_id  = port_id;
    pending.target_port_gen = port->generation;     /* snapshot ABA */
    wait_queue_init(&pending.reply_wait);
    list_node_init(&pending.port_node);

    port_msg_entry_t *entry = entry_alloc();
    if (entry == NULL)
    {
        ret = IPC_ERR_NO_MEMORY;
        goto out;
    }
    list_node_init(&entry->node);
    memcpy(&entry->msg, msg, sizeof(ipc_message_t));
    entry->payload_owned = false;       /* payload prestato dal chiamante:
                                         * il sender resta bloccato, il
                                         * buffer e' stabile — zero copie */

    s_pending[self->tid] = &pending;

    /* Accoda il messaggio E aggancia il pending, sotto lo stesso lock:
     * o il receiver ci vede in pending_senders, o non ha ancora visto
     * il messaggio. */
    uint32_t flags = spinlock_acquire_irqsave(&port->lock);
    if (unlikely(port->msg_count >= PORT_QUEUE_MAX))
    {
        spinlock_release_irqrestore(&port->lock, flags);
        ipc_msg_entry_free(entry);
        s_pending[self->tid] = NULL;
        ret = IPC_ERR_PORT_FULL;
        goto out;
    }
    list_push_back(&port->msg_queue, &entry->node);
    port->msg_count++;
    list_push_back(&port->pending_senders, &pending.port_node);
    spinlock_release_irqrestore(&port->lock, flags);

    /* Direct-switch (richiesta): se un receiver e' in attesa sulla
     * NOSTRA CPU, gli si cede direttamente il processore — il
     * messaggio e' gia' in coda, il round-trip salta due passaggi
     * dallo scheduler. La nostra attesa (BLOCKED + in reply_wait) e'
     * pubblicata PRIMA dello switch: una reply da un altro core non
     * puo' perdersi. Receiver assente o su altra CPU: wake normale. */
    thread_t *receiver = wait_queue_extract_one(&port->recv_waiters);
    if (receiver != NULL)
    {
        uint32_t hfl = irq_save();
        if (receiver->home_cpu == this_cpu()->cpu_index)
        {
            wait_prepare(&pending.reply_wait, 0);
            scheduler_yield_to(receiver, THREAD_BLOCKED);
            irq_restore(hfl);
            wait_finish();
        }
        else
        {
            irq_restore(hfl);
            scheduler_unblock(receiver);
        }
    }

    /* Attesa della reply: prepare-first + re-check sotto IF=0 — una
     * preemption tra prepare e re-check deschedulerebbe senza requeue,
     * e un wake gia' speso non torna. (Dopo un handoff diretto la
     * reply e' di norma gia' arrivata: il while non entra.) */
    while (!pending.reply_received)
    {
        uint32_t wfl = irq_save();
        wait_prepare(&pending.reply_wait, 0);
        if (pending.reply_received)
        {
            wait_cancel();
            irq_restore(wfl);
            break;
        }
        scheduler_yield();
        irq_restore(wfl);
        wait_finish();
    }

    /* Smontaggio: slot pending sotto il lock per-TID (una reply tardiva
     * concorrente o vede lo slot e finisce prima di noi, o trova NULL). */
    uint32_t lf = pending_lock(self->tid);
    s_pending[self->tid] = NULL;
    pending_unlock(self->tid, lf);

    /* Sgancio da pending_senders: se la porta e' morta ci ha gia'
     * sganciati il wake del destroy. B1 tiene lo struct valido; il
     * check di generazione resta come guardia d'invariante. */
    if (!pending.port_dead && port->generation == pending.target_port_gen)
    {
        uint32_t fl2 = spinlock_acquire_irqsave(&port->lock);
        if (list_node_is_linked(&pending.port_node))
        {
            list_remove(&pending.port_node);
        }
        spinlock_release_irqrestore(&port->lock, fl2);
    }

    ret = pending.port_dead ? IPC_ERR_DEAD : IPC_OK;

out:
    ipc_port_put(port);
    return ret;
}

int32_t ipc_reply_staged(tid_t sender_tid, ipc_message_t *reply,
                         void *payload_kbuf, uint32_t payload_size)
{
    if (reply == NULL || sender_tid < 0 || sender_tid >= MAX_THREADS)
    {
        if (payload_kbuf != NULL)
        {
            kfree(payload_kbuf);        /* ownership: sempre nostra */
        }
        return IPC_ERR_INVALID;
    }

    /* Lock per-TID per TUTTA la sezione: il pending vive sullo stack
     * del sender e ipc_cleanup_thread (teardown) prende questo stesso
     * lock prima che lo stack venga liberato. */
    uint32_t lf = pending_lock(sender_tid);

    pending_reply_t *p = s_pending[sender_tid];
    if (p == NULL)
    {
        pending_unlock(sender_tid, lf);
        if (payload_kbuf != NULL)
        {
            kfree(payload_kbuf);
        }
        return IPC_ERR_INVALID;
    }

    if (p->reply_buf != NULL)
    {
        reply->type = IPC_MSG_REPLY;
        thread_t *self = current_thread;
        if (self != NULL && self->owner != NULL)
        {
            reply->sender_pid = self->owner->pid;
            reply->sender_tid = self->tid;
        }
        /* Il payload kernel passa al sender: lo consegnera' nel proprio
         * IPC_BUF e fara' lui il kfree (send_deliver_reply_payload). */
        reply->payload      = payload_kbuf;
        reply->payload_size = (payload_kbuf != NULL) ? payload_size : 0;
        memcpy(p->reply_buf, reply, sizeof(ipc_message_t));
    }
    else if (payload_kbuf != NULL)
    {
        kfree(payload_kbuf);
    }

    p->reply_received = true;

    /* Direct-switch (reply): il sender bloccato sulla NOSTRA CPU
     * riparte subito, noi restiamo READY in coda. Estrazione sotto il
     * lock per-TID (il pending vive sullo stack del sender), switch a
     * lock MOLLATO — mai uno spinlock attraverso un context switch. */
    thread_t *sender = wait_queue_extract_one(&p->reply_wait);
    pending_unlock(sender_tid, lf);

    if (sender != NULL)
    {
        if (sender->home_cpu == this_cpu()->cpu_index)
        {
            scheduler_yield_to(sender, THREAD_READY);
        }
        else
        {
            scheduler_unblock(sender);
        }
    }
    return IPC_OK;
}

/* Corpo comune receive/receive_nowait. */
static int32_t receive_common(uint32_t port_id, ipc_message_t *msg,
                              bool *payload_owned, bool blocking)
{
    if (msg == NULL || payload_owned == NULL)
    {
        return IPC_ERR_INVALID;
    }

    ipc_port_t *port = ipc_port_get(port_id);
    if (port == NULL)
    {
        return IPC_ERR_NO_PORT;
    }

    int32_t ret;
    thread_t *self = current_thread;
    if (self != NULL && self->owner != NULL
        && port->owner_pid != self->owner->pid)
    {
        ret = IPC_ERR_DENIED;           /* solo il proprietario riceve */
        goto out;
    }

    for (;;)
    {
        /* PERCORSO VELOCE: roba gia' in coda — un lock, zero traffico
         * di wait-queue. Su un server sotto carico e' il caso comune. */
        ret = try_consume(port, msg, payload_owned);
        if (ret != IPC_ERR_EMPTY)
        {
            goto out;
        }
        if (!blocking)
        {
            goto out;                   /* IPC_ERR_EMPTY al chiamante */
        }

        /* Percorso lento: prepare-first + re-check sotto IF=0. Senza,
         * un send tra check e sleep spende il wake e il receiver dorme
         * per sempre mentre la porta si riempie. */
        uint32_t wfl = irq_save();
        wait_prepare(&port->recv_waiters, 0);

        ret = try_consume(port, msg, payload_owned);
        if (ret != IPC_ERR_EMPTY)
        {
            wait_cancel();
            irq_restore(wfl);
            goto out;
        }

        /* B1: la referenza tiene lo struct valido attraverso il sonno;
         * un destroy mette PORT_CLOSED e sveglia recv_waiters — al giro
         * dopo try_consume risponde IPC_ERR_DEAD. */
        scheduler_yield();
        irq_restore(wfl);
        wait_finish();
    }

out:
    ipc_port_put(port);
    return ret;
}

int32_t ipc_receive(uint32_t port_id, ipc_message_t *msg,
                    bool *payload_owned)
{
    return receive_common(port_id, msg, payload_owned, true);
}

int32_t ipc_receive_nowait(uint32_t port_id, ipc_message_t *msg,
                           bool *payload_owned)
{
    return receive_common(port_id, msg, payload_owned, false);
}

/* Corpo comune di post/post_checked: la porta arriva gia' risolta e
 * referenziata dal chiamante (get o get_checked), che fara' il put.
 * Marca il messaggio come SIGNAL, ruba il payload snapshottato e lo
 * accoda. Non tocca il refcount della porta. */
static int32_t post_common(ipc_port_t *port, ipc_message_t *msg)
{
    if (port->state != PORT_ACTIVE)
    {
        return IPC_ERR_NO_PORT;
    }

    thread_t *self = current_thread;
    if (self == NULL || self->owner == NULL)
    {
        return IPC_ERR_INVALID;
    }

    msg->sender_pid = self->owner->pid;
    msg->sender_tid = self->tid;
    msg->type       = IPC_MSG_SIGNAL;   /* signal = nessuna reply attesa */

    port_msg_entry_t *entry = entry_alloc();
    if (entry == NULL)
    {
        return IPC_ERR_NO_MEMORY;
    }
    list_node_init(&entry->node);
    memcpy(&entry->msg, msg, sizeof(ipc_message_t));
    entry->payload_owned = true;

    if (msg->payload != NULL && msg->payload_size > 0)
    {
        /* Ruba il buffer kernel del chiamante: gia' snapshottato da
         * sys_post, trasferirlo evita kmalloc+memcpy+kfree per
         * messaggio. msg->payload azzerato = ownership trasferita. */
        entry->msg.payload = msg->payload;
        msg->payload = NULL;
    }

    return enqueue_async(port, entry);
}

int32_t ipc_post(uint32_t port_id, ipc_message_t *msg)
{
    if (msg == NULL)
    {
        return IPC_ERR_INVALID;
    }

    ipc_port_t *port = ipc_port_get(port_id);
    if (port == NULL)
    {
        return IPC_ERR_NO_PORT;
    }

    int32_t ret = post_common(port, msg);
    ipc_port_put(port);
    return ret;
}

int32_t ipc_post_checked(uint32_t port_id, uint32_t gen, ipc_message_t *msg)
{
    if (msg == NULL)
    {
        return IPC_ERR_INVALID;
    }

    /* Consegna anti-ABA: se l'id e' stato riciclato da un altro
     * processo, get_checked ritorna NULL (gen diversa) e il chiamante
     * riceve IPC_ERR_DEAD — segnale esatto ed event-driven che il
     * binding vecchio e' morto, senza mai consegnare all'estraneo. */
    ipc_port_t *port = ipc_port_get_checked(port_id, gen);
    if (port == NULL)
    {
        return IPC_ERR_DEAD;
    }

    int32_t ret = post_common(port, msg);
    ipc_port_put(port);
    return ret;
}

int32_t ipc_notify(uint32_t port_id, uint32_t bits)
{
    ipc_port_t *port = ipc_port_get(port_id);
    if (port == NULL)
    {
        return IPC_ERR_NO_PORT;
    }

    int32_t ret;
    if (port->state != PORT_ACTIVE)
    {
        ret = IPC_ERR_NO_PORT;
        goto out;
    }

    uint32_t flags = spinlock_acquire_irqsave(&port->lock);
    port->notify_bits |= bits;
    spinlock_release_irqrestore(&port->lock, flags);

    /* Entrambe le code: una receive in attesa raccoglie le notifiche
     * accanto ai messaggi normali. */
    wait_queue_wake_all(&port->notify_waiters);
    wait_queue_wake_all(&port->recv_waiters);
    ret = IPC_OK;

out:
    ipc_port_put(port);
    return ret;
}

uint32_t ipc_wait_notify(uint32_t port_id)
{
    ipc_port_t *port = ipc_port_get(port_id);
    if (port == NULL)
    {
        return 0;
    }

    uint32_t result;
    for (;;)
    {
        /* Percorso veloce: bit gia' presenti / porta morta. */
        uint32_t ffl = spinlock_acquire_irqsave(&port->lock);
        uint32_t bits = port->notify_bits;
        port_state_t st = port->state;
        if (bits != 0)
        {
            port->notify_bits = 0;
        }
        spinlock_release_irqrestore(&port->lock, ffl);
        if (bits != 0)
        {
            result = bits;
            goto out;
        }
        if (st != PORT_ACTIVE)
        {
            result = 0;
            goto out;
        }

        /* Prepare-first (un notify perso strandera' i consumer di IRQ
         * inoltrati — l'input! — per sempre). */
        uint32_t wfl = irq_save();
        wait_prepare(&port->notify_waiters, 0);

        uint32_t flags = spinlock_acquire_irqsave(&port->lock);
        bits = port->notify_bits;
        st   = port->state;
        if (bits != 0)
        {
            port->notify_bits = 0;
        }
        spinlock_release_irqrestore(&port->lock, flags);

        if (bits != 0 || st != PORT_ACTIVE)
        {
            wait_cancel();
            irq_restore(wfl);
            result = bits;              /* 0 se la porta e' morta */
            goto out;
        }

        scheduler_yield();
        irq_restore(wfl);
        wait_finish();
    }

out:
    ipc_port_put(port);
    return result;
}

/* ======================================================================
 * Wake dei sender (destroy) + cleanup
 * ==================================================================== */

void ipc_wake_senders_for_port_ptr(struct ipc_port *port)
{
    if (port == NULL)
    {
        return;
    }

    /* Drenaggio a lotti senza tetto fisso: con molti sender in coda,
     * un cap lascerebbe i rimanenti marcati "dead" ma mai svegliati.
     * Ogni pending vive sullo stack del suo sender, che resta bloccato
     * (non distrutto) finche' e' nella lista: i puntatori reggono. */
    #define WAKE_BATCH 32u
    pending_reply_t *batch[WAKE_BATCH];

    for (;;)
    {
        uint32_t n = 0;

        uint32_t fl = spinlock_acquire_irqsave(&port->lock);
        list_node_t *pos, *tmp;
        list_for_each_safe(pos, tmp, &port->pending_senders)
        {
            pending_reply_t *p = list_entry(pos, pending_reply_t, port_node);
            if (!p->reply_received)
            {
                p->port_dead = true;
                p->reply_received = true;
                list_remove(&p->port_node);
                batch[n++] = p;
                if (n >= WAKE_BATCH)
                {
                    break;
                }
            }
        }
        spinlock_release_irqrestore(&port->lock, fl);

        if (n == 0)
        {
            break;
        }

        /* Wake FUORI da port->lock: evita l'inversione con wq->lock. */
        for (uint32_t i = 0; i < n; i++)
        {
            wait_queue_wake_all(&batch[i]->reply_wait);
        }

        if (n < WAKE_BATCH)
        {
            break;
        }
    }
    #undef WAKE_BATCH
}

void ipc_cleanup_thread(tid_t tid)
{
    if (tid < 0 || tid >= MAX_THREADS)
    {
        return;
    }

    /* Lock per-TID su tutto il teardown dello slot: una reply in volo
     * per questo tid tiene lo stesso lock, quindi si blocca qui finche'
     * non finisce — lo stack non viene mai smappato sotto una reply. */
    uint32_t lf = pending_lock(tid);

    pending_reply_t *p = s_pending[tid];
    if (p == NULL)
    {
        pending_unlock(tid, lf);
        return;
    }

    /* Slot a NULL PRIMA di tutto: una reply successiva che prende il
     * lock dopo di noi trova NULL e torna IPC_ERR_INVALID invece di
     * toccare lo struct morente. */
    s_pending[tid] = NULL;
    p->port_dead = true;
    p->reply_received = true;
    /* Nessun wake: il thread sta morendo, non riprendendo. */

    /* Sgancio da pending_senders della porta bersaglio: il pending vive
     * sullo stack morente, un link stantio punterebbe in memoria
     * riciclata. Guardia ABA: si tocca la lista solo se la porta viva a
     * quell'id ha ancora la generazione a cui abbiamo spedito. */
    ipc_port_t *port = ipc_port_get(p->target_port_id);
    if (port != NULL && port->generation == p->target_port_gen)
    {
        uint32_t fl = spinlock_acquire_irqsave(&port->lock);
        if (list_node_is_linked(&p->port_node))
        {
            list_remove(&p->port_node);
        }
        spinlock_release_irqrestore(&port->lock, fl);
    }
    ipc_port_put(port);

    pending_unlock(tid, lf);
}

void ipc_ports_cleanup_owner(pid_t owner_pid)
{
    /* Lista intrusiva del proprietario: O(porte del processo), non
     * O(MAX_PORTS). Un processo tipico ne possiede 0-3. Il destroy
     * sgancia il nodo, quindi iterazione safe; l'id va preso prima
     * (destroy libera lo struct). owner_pid e' il processo IN teardown
     * (chiamati da process_destroy_local prima di handle_table_remove):
     * vivo qui, la reclamation e' differita a refcount 0 -> nessun
     * process_get_ref necessario. */
    process_t *owner = process_get_by_pid(owner_pid);
    if (owner == NULL)
    {
        return;
    }

    list_node_t *pos, *tmp;
    list_for_each_safe(pos, tmp, &owner->owned_ports)
    {
        ipc_port_t *port = list_entry(pos, ipc_port_t, owner_node);
        ipc_port_destroy(port->id);
    }
}

int32_t ipc_post_kernel(uint32_t port_id, uint32_t code,
                        uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
    ipc_port_t *port = ipc_port_get(port_id);
    if (port == NULL)
    {
        return IPC_ERR_NO_PORT;
    }

    int32_t ret;
    if (port->state != PORT_ACTIVE)
    {
        ret = IPC_ERR_NO_PORT;
        goto out;
    }

    port_msg_entry_t *entry = entry_alloc_irq();
    if (entry == NULL)
    {
        ret = IPC_ERR_NO_MEMORY;        /* pool esaurito: drop contato */
        goto out;
    }

    list_node_init(&entry->node);
    memset(&entry->msg, 0, sizeof(ipc_message_t));
    entry->payload_owned  = false;
    entry->msg.type       = IPC_MSG_SIGNAL;
    entry->msg.sender_pid = 0;
    entry->msg.sender_tid = -1;
    entry->msg.code       = code;
    entry->msg.arg0       = arg0;
    entry->msg.arg1       = arg1;
    entry->msg.arg2       = arg2;

    ret = enqueue_async(port, entry);

out:
    ipc_port_put(port);
    return ret;
}

void ipc_init(void)
{
    reclaim_register("ipc-depot", ipc_depot_trim);

    ipc_port_init();
    memset(s_pending, 0, sizeof(s_pending));

    for (uint32_t i = 0; i < PENDING_LOCK_N; i++)
    {
        spinlock_init(&s_pending_lock[i]);
    }

    /* Incatena il pool IRQ in freelist (node.next come link). */
    for (uint32_t i = 0; i < IPC_IRQ_POOL_SIZE - 1; i++)
    {
        s_irq_pool[i].node.next = (list_node_t *)&s_irq_pool[i + 1];
    }
    s_irq_pool[IPC_IRQ_POOL_SIZE - 1].node.next = NULL;
    s_irq_freelist = &s_irq_pool[0];

    for (uint32_t i = 0; i < MAX_CPUS; i++)
    {
        s_mag[i]   = NULL;
        s_mag_n[i] = 0;
    }
    s_depot   = NULL;
    s_depot_n = 0;

    kprintf("[IPC]  Sottosistema IPC inizializzato.\n");
}
