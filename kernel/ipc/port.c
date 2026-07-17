#include "ipc/port.h"
#include "ipc/channel.h"
#include "sync/atomic.h"
#include "proc/process.h"
#include "mm/kheap.h"
#include "console/console.h"
#include "lib/string.h"

/* Ciclo di vita delle porte: tabella globale + freelist di id,
 * refcount per la validita' dei puntatori, generazione per-id contro
 * l'ABA, riciclo LIFO degli struct (create/destroy e' un pattern
 * caldo nei server RPC). */

static ipc_port_t *s_table[MAX_PORTS];
static uint32_t    s_freelist[MAX_PORTS];
static uint32_t    s_free_top;
static spinlock_t  s_global_lock = SPINLOCK_INIT;

/* ======================================================================
 * Death-watch: notifica event-driven della morte di una PORTA
 *
 * Un processo watcher registra "quando la porta (id, gen) muore, postami
 * `code` sulla mia `notify_port`". La chiave e' (id, gen), non il pid:
 * cosi' la notifica e' immune al riciclo del pid/della porta — un id
 * riusato ha gen diversa e non fa scattare il watch di un binding
 * vecchio. Sostituisce il poll di dobinterface: la scoperta della morte
 * torna sul ciclo eventi, come vuole il design.
 *
 * Ciclo di vita di un'entry: one-shot (rimossa quando scatta) oppure
 * ripulita quando muore il watcher (deathwatch_cleanup_watcher, dal
 * teardown del processo). Non serve unwatch esplicito: se il target non
 * muore mai, l'entry vive quanto il watcher — limite superiore basso.
 * ==================================================================== */

#define DEATHWATCH_MAX  (2u * MAX_PORTS)

typedef struct
{
    bool     used;
    int32_t  watcher_pid;       /* per la pulizia alla morte del watcher */
    uint32_t notify_port;       /* dove postare la notifica              */
    uint32_t code;              /* codice IPC della notifica             */
    uint32_t target_port;       /* porta osservata                       */
    uint32_t target_gen;        /* incarnazione osservata (anti-ABA)     */
} deathwatch_t;

static deathwatch_t s_dw[DEATHWATCH_MAX];
static spinlock_t   s_dw_lock = SPINLOCK_INIT;

void deathwatch_init(void)
{
    memset(s_dw, 0, sizeof(s_dw));
}

/* Registra un watch. Ritorna 0 (osservando), 1 (target GIA' morto: il
 * chiamante mieta subito), -1 (tabella piena). Deduplica su
 * (notify_port, target_port, target_gen): re-registrare e' un no-op. */
int32_t deathwatch_add(int32_t watcher_pid, uint32_t notify_port,
                       uint32_t code, uint32_t target_port,
                       uint32_t target_gen)
{
    /* Il target e' ancora quella incarnazione? Se no, e' gia' morto tra
     * la cattura della gen e qui: niente da osservare, mieta subito. */
    if (target_gen == 0 || ipc_port_generation(target_port) != target_gen)
    {
        return 1;
    }

    uint32_t fl = spinlock_acquire_irqsave(&s_dw_lock);

    int free_slot = -1;
    for (uint32_t i = 0; i < DEATHWATCH_MAX; i++)
    {
        if (!s_dw[i].used)
        {
            if (free_slot < 0) free_slot = (int)i;
            continue;
        }
        if (s_dw[i].notify_port == notify_port &&
            s_dw[i].target_port == target_port &&
            s_dw[i].target_gen == target_gen)
        {
            spinlock_release_irqrestore(&s_dw_lock, fl);
            return 0;                   /* gia' osservato                 */
        }
    }
    if (free_slot < 0)
    {
        spinlock_release_irqrestore(&s_dw_lock, fl);
        return -1;                      /* tabella piena                  */
    }

    s_dw[free_slot].used        = true;
    s_dw[free_slot].watcher_pid = watcher_pid;
    s_dw[free_slot].notify_port = notify_port;
    s_dw[free_slot].code        = code;
    s_dw[free_slot].target_port = target_port;
    s_dw[free_slot].target_gen  = target_gen;
    spinlock_release_irqrestore(&s_dw_lock, fl);
    return 0;
}

/* La porta (target_port, target_gen) sta morendo: notifica e sgancia i
 * watcher. Chiamata da ipc_port_destroy col lock globale GIA' mollato.
 * Raccoglie sotto s_dw_lock, posta FUORI (ipc_post_kernel prende il lock
 * della porta del watcher: mai annidato con s_dw_lock). */
static void deathwatch_fire(uint32_t target_port, uint32_t target_gen)
{
    struct { uint32_t port; uint32_t code; } batch[16];

    for (;;)
    {
        uint32_t n = 0;

        uint32_t fl = spinlock_acquire_irqsave(&s_dw_lock);
        for (uint32_t i = 0; i < DEATHWATCH_MAX && n < 16u; i++)
        {
            if (s_dw[i].used &&
                s_dw[i].target_port == target_port &&
                s_dw[i].target_gen == target_gen)
            {
                batch[n].port = s_dw[i].notify_port;
                batch[n].code = s_dw[i].code;
                n++;
                s_dw[i].used = false;   /* one-shot                       */
            }
        }
        spinlock_release_irqrestore(&s_dw_lock, fl);

        for (uint32_t i = 0; i < n; i++)
        {
            ipc_post_kernel(batch[i].port, batch[i].code,
                            target_port, target_gen, 0);
        }
        if (n < 16u)
        {
            break;
        }
    }
}

/* Sgancia i watch REGISTRATI da un processo che muore (le sue notify non
 * hanno piu' destinatario). Chiamata dal teardown del processo. */
void deathwatch_cleanup_watcher(int32_t watcher_pid)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_dw_lock);
    for (uint32_t i = 0; i < DEATHWATCH_MAX; i++)
    {
        if (s_dw[i].used && s_dw[i].watcher_pid == watcher_pid)
        {
            s_dw[i].used = false;
        }
    }
    spinlock_release_irqrestore(&s_dw_lock, fl);
}

/* ABA: contatore monotono per-id, FUORI dallo struct cosi' sopravvive
 * al riciclo dello slot. Pre-incrementato al create: una porta viva non
 * legge mai gen 0 (0 = "nessuno snapshot"). */
static uint32_t    s_generation[MAX_PORTS];

/* Riciclo struct: create/destroy e' un pattern caldo (server RPC,
 * porte per-finestra di libdobui); il LIFO evita kcalloc + azzeramento
 * a ogni giro, e il piu' recente e' il piu' caldo in cache. */
#define PORT_RECYCLE_MAX  64u
static ipc_port_t *s_recycle[PORT_RECYCLE_MAX];
static uint32_t    s_recycle_top;

/* === Verbi (allocazione struct) ======================================== */

/* Chiamante tiene s_global_lock. */
static ipc_port_t *port_struct_alloc(void)
{
    if (s_recycle_top > 0)
    {
        ipc_port_t *p = s_recycle[--s_recycle_top];
        memset(p, 0, sizeof(*p));
        return p;
    }
    return (ipc_port_t *)kcalloc(1, sizeof(ipc_port_t));
}

/* Chiamante NON tiene s_global_lock (il destroy l'ha gia' mollato). */
static void port_struct_free(ipc_port_t *p)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_global_lock);
    if (s_recycle_top < PORT_RECYCLE_MAX)
    {
        s_recycle[s_recycle_top++] = p;
        spinlock_release_irqrestore(&s_global_lock, fl);
        return;
    }
    spinlock_release_irqrestore(&s_global_lock, fl);
    kfree(p);
}

/* Teardown della porta a referenze zero: libera i messaggi accodati,
 * ricicla lo struct, restituisce l'id. Gira SENZA s_global_lock (fa
 * kfree); a refcount 0 e slot NULL nessun altro percorso puo'
 * raggiungere lo struct — il teardown e' in esclusiva. */
static void port_teardown(ipc_port_t *port)
{
    uint32_t port_id = port->id;

    list_node_t *pos, *tmp;
    list_for_each_safe(pos, tmp, &port->msg_queue)
    {
        port_msg_entry_t *entry = list_entry(pos, port_msg_entry_t, node);
        list_remove(pos);
        if (entry->payload_owned && entry->msg.payload != NULL)
        {
            kfree(entry->msg.payload);
        }
        ipc_msg_entry_free(entry);
    }

    port_struct_free(port);

    uint32_t flags = spinlock_acquire_irqsave(&s_global_lock);
    s_freelist[s_free_top++] = port_id;
    spinlock_release_irqrestore(&s_global_lock, flags);
}

/* === API =============================================================== */

void ipc_port_init(void)
{
    memset(s_table, 0, sizeof(s_table));

    s_free_top = 0;
    for (uint32_t i = MAX_PORTS - 1; i >= 1; i--)
    {
        s_freelist[s_free_top++] = i;   /* id 1..MAX-1; 0 resta invalido */
    }

    kprintf("[IPC]  Porte inizializzate (max %u).\n", MAX_PORTS);
}

int32_t ipc_port_create(pid_t owner_pid)
{
    uint32_t flags = spinlock_acquire_irqsave(&s_global_lock);

    if (s_free_top == 0)
    {
        spinlock_release_irqrestore(&s_global_lock, flags);
        /* Tabella porte esausta: o il sistema e' davvero a 4096 porte
         * vive, o QUALCUNO le perde (crea senza distruggere). In
         * entrambi i casi il fallimento silenzioso si manifestava come
         * app che "impazziscono" senza spiegazione: la riga da' il PID
         * del richiedente e rende visibile il trend. Rate-limited. */
        static volatile uint32_t s_exhaust_reports;
        uint32_t n = atomic_add_return(&s_exhaust_reports, 1) - 1;
        if (n < 4u || (n & 255u) == 0u)
        {
            kprintf("[IPC ] TABELLA PORTE ESAUSTA (%u): create fallita "
                    "per PID %d (occorrenza %u) - possibile leak di "
                    "porte\n", MAX_PORTS, owner_pid, n + 1);
        }
        return IPC_ERR_NO_MEMORY;
    }

    uint32_t id = s_freelist[--s_free_top];

    ipc_port_t *port = port_struct_alloc();
    if (port == NULL)
    {
        s_freelist[s_free_top++] = id;
        spinlock_release_irqrestore(&s_global_lock, flags);
        return IPC_ERR_NO_MEMORY;
    }

    port->id         = id;
    port->owner_pid  = owner_pid;
    port->state      = PORT_ACTIVE;
    port->generation = ++s_generation[id];
    refcount_init(&port->refcount, 1);      /* B1: la referenza dello slot */
    spinlock_init(&port->lock);
    list_init(&port->msg_queue);
    port->msg_count = 0;
    list_init(&port->pending_senders);
    wait_queue_init(&port->recv_waiters);
    port->notify_bits = 0;
    wait_queue_init(&port->notify_waiters);
    list_node_init(&port->owner_node);

    s_table[id] = port;

    /* Aggancio alla lista del proprietario: cleanup O(porte possedute).
     * owner_pid e' il creatore (il processo corrente): vivo per
     * definizione, nessun process_get_ref necessario. */
    process_t *owner = process_get_by_pid(owner_pid);
    if (owner != NULL)
    {
        list_push_back(&owner->owned_ports, &port->owner_node);
    }

    spinlock_release_irqrestore(&s_global_lock, flags);
    return (int32_t)id;
}

void ipc_port_destroy(uint32_t port_id)
{
    if (port_id == 0 || port_id >= MAX_PORTS)
    {
        return;
    }

    uint32_t flags = spinlock_acquire_irqsave(&s_global_lock);
    ipc_port_t *port = s_table[port_id];
    if (port == NULL)
    {
        spinlock_release_irqrestore(&s_global_lock, flags);
        return;
    }

    port->state = PORT_CLOSED;
    bool has_pending = !list_empty(&port->pending_senders);
    uint32_t dead_gen = port->generation;   /* per il death-watch, prima */
                                            /* che lo struct venga riciclato */
    s_table[port_id] = NULL;            /* nessun nuovo get la trova piu' */

    if (list_node_is_linked(&port->owner_node))
    {
        list_remove(&port->owner_node);
    }
    spinlock_release_irqrestore(&s_global_lock, flags);

    /* Notifica event-driven della morte di questa porta ai watcher
     * (chiave (id, gen): anti-ABA). Fuori dal lock globale — posta via
     * ipc_post_kernel (pool IRQ, non bloccante). */
    deathwatch_fire(port_id, dead_gen);

    /* Sveglia subito tutti i bloccati: al risveglio rivedono
     * state == PORT_CLOSED ed escono con errore. Lo struct resta valido
     * perche' teniamo ancora la referenza dello slot. */
    wait_queue_wake_all(&port->recv_waiters);
    wait_queue_wake_all(&port->notify_waiters);
    if (has_pending)
    {
        /* Variante a puntatore obbligatoria: s_table[id] e' gia' NULL,
         * un re-get per id non sveglierebbe nessuno. */
        ipc_wake_senders_for_port_ptr(port);
    }

    /* Rilascio della referenza dello slot: se nessun get e' in corso il
     * teardown avviene ora, altrimenti all'ultimo put — l'id si ricicla
     * solo a struct davvero fermo. */
    if (refcount_dec(&port->refcount))
    {
        port_teardown(port);
    }
}

ipc_port_t *ipc_port_get(uint32_t port_id)
{
    if (port_id == 0 || port_id >= MAX_PORTS)
    {
        return NULL;
    }

    uint32_t flags = spinlock_acquire_irqsave(&s_global_lock);
    ipc_port_t *port = s_table[port_id];
    if (port != NULL && port->state != PORT_CLOSED)
    {
        refcount_inc(&port->refcount);
    }
    else
    {
        port = NULL;
    }
    spinlock_release_irqrestore(&s_global_lock, flags);
    return port;
}

ipc_port_t *ipc_port_get_checked(uint32_t port_id, uint32_t gen)
{
    if (port_id == 0 || port_id >= MAX_PORTS)
    {
        return NULL;
    }

    uint32_t flags = spinlock_acquire_irqsave(&s_global_lock);
    ipc_port_t *port = s_table[port_id];
    /* Stesso slot E stessa incarnazione: un id riciclato ha gen diversa
     * (pre-incrementata a ogni create), quindi qui NULL — il chiamante
     * scopre che il suo binding e' morto invece di parlare a un estraneo. */
    if (port != NULL && port->state != PORT_CLOSED && port->generation == gen)
    {
        refcount_inc(&port->refcount);
    }
    else
    {
        port = NULL;
    }
    spinlock_release_irqrestore(&s_global_lock, flags);
    return port;
}

uint32_t ipc_port_generation(uint32_t port_id)
{
    if (port_id == 0 || port_id >= MAX_PORTS)
    {
        return 0;
    }

    uint32_t flags = spinlock_acquire_irqsave(&s_global_lock);
    ipc_port_t *port = s_table[port_id];
    uint32_t gen = (port != NULL && port->state != PORT_CLOSED)
                 ? port->generation : 0;
    spinlock_release_irqrestore(&s_global_lock, flags);
    return gen;
}

void ipc_port_put(ipc_port_t *port)
{
    if (port == NULL)
    {
        return;
    }
    /* Drop lock-free: gli incrementi avvengono SOLO sotto s_global_lock
     * e SOLO finche' s_table[id] punta qui; il conteggio puo' toccare 0
     * solo dopo che il destroy ha annullato lo slot (che deteneva una
     * referenza). Visto 0, nessuna nuova referenza puo' apparire. */
    if (refcount_dec(&port->refcount))
    {
        port_teardown(port);
    }
}
