#include "registry.h"
#include "sync/spinlock.h"
#include "proc/wait.h"
#include "proc/process.h"
#include "proc/scheduler.h"
#include "arch/x86/cpu.h"
#include "lib/list.h"
#include "lib/string.h"
#include "proc/thread.h"
#include "mm/kheap.h"

/* Registro dei servizi: nome -> porta IPC. I server si registrano
 * all'avvio; i client risolvono con find o bloccano con wait finche'
 * il nome non appare (event-driven, zero polling).
 *
 * Un solo lock copre tabella e lista dei waiter: il check-then-sleep
 * di registry_wait e' cosi' privo di race. Il record d'attesa vive
 * sullo stack del waiter (stabile finche' e' bloccato): nessuna
 * tabella waiter da saturare. */

typedef struct
{
    char     name[REGISTRY_NAME_MAX];
    uint32_t name_hash;             /* FNV-1a, 0 = slot mai usato */
    uint32_t port_id;
    int32_t  owner_pid;
    bool     active;
} entry_t;

typedef struct
{
    const char  *name;              /* punta al buffer del waiter */
    uint32_t     name_hash;
    wait_queue_t wq;
    list_node_t  node;
} waiter_t;

/* Thread parcheggiati da needs: — record kmalloc'd (a differenza dei
 * waiter, il parcheggiato non sta eseguendo registry_wait: niente
 * stack suo su cui vivere). */
typedef struct
{
    char        name[REGISTRY_NAME_MAX];
    uint32_t    name_hash;
    tid_t       tid;                /* validato al wake: mai puntatori  */
    int32_t     owner_pid;
    list_node_t node;
} parked_t;

static entry_t    s_entries[REGISTRY_MAX];
static uint32_t   s_count;
static list_t     s_waiters;
static list_t     s_parked;
static spinlock_t s_lock = SPINLOCK_INIT;

/* === Verbi (chiamante tiene s_lock, salvo nota) ======================== */

static uint32_t hash_name(const char *s)
{
    uint32_t h = 0x811C9DC5u;       /* FNV-1a: prefiltro per gli strcmp */
    while (*s)
    {
        h ^= (uint8_t)*s++;
        h *= 0x01000193u;
    }
    return h ? h : 1;
}

static entry_t *entry_find(const char *name, uint32_t h)
{
    for (uint32_t i = 0; i < s_count; i++)
    {
        if (s_entries[i].active && s_entries[i].name_hash == h
            && strcmp(s_entries[i].name, name) == 0)
        {
            return &s_entries[i];
        }
    }
    return NULL;
}

static entry_t *entry_alloc(void)
{
    for (uint32_t i = 0; i < s_count; i++)
    {
        if (!s_entries[i].active)
        {
            return &s_entries[i];
        }
    }
    if (s_count < REGISTRY_MAX)
    {
        return &s_entries[s_count++];
    }
    return NULL;
}

/* Raccoglie i waiter del nome dato in `batch` (sganciandoli), da
 * svegliare fuori dal lock. Ritorna quanti. */
static uint32_t collect_name_waiters(const char *name, uint32_t h,
                                     waiter_t **batch, uint32_t cap)
{
    uint32_t n = 0;
    list_node_t *pos, *tmp;
    list_for_each_safe(pos, tmp, &s_waiters)
    {
        waiter_t *w = list_entry(pos, waiter_t, node);
        if (w->name_hash == h && strcmp(w->name, name) == 0)
        {
            list_remove(&w->node);
            batch[n++] = w;
            if (n >= cap)
            {
                break;
            }
        }
    }
    return n;
}

/* === API =============================================================== */

void registry_init(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_count = 0;
    list_init(&s_waiters);
    list_init(&s_parked);
}

/* Stacca dal parcheggio i record che aspettano `name` (chiamante
 * tiene s_lock). I record staccati vanno liberati e i tid sbloccati
 * FUORI dal lock. */
static uint32_t collect_parked(const char *name, uint32_t h,
                               parked_t **batch, uint32_t cap)
{
    uint32_t n = 0;
    list_node_t *it = s_parked.head.next;
    while (it != &s_parked.head && n < cap)
    {
        list_node_t *next = it->next;
        parked_t *p = list_entry(it, parked_t, node);
        if (p->name_hash == h && strcmp(p->name, name) == 0)
        {
            list_remove(&p->node);
            batch[n++] = p;
        }
        it = next;
    }
    return n;
}

/* Drena e sveglia TUTTI gli interessati a `name`: i collettori
 * staccano a LOTTI sotto s_lock, i wake avvengono fuori dal lock — e a
 * lotto pieno si ripete finche' le liste non sono asciutte. Fermarsi al
 * primo lotto (com'era) lasciava gli eccedenti — il waiter oltre il
 * sedicesimo, il parcheggiato oltre l'ottavo sullo stesso nome — in
 * attesa PER SEMPRE: la register di un nome e' un evento una-tantum e
 * nessun altro li avrebbe mai piu' toccati. */
static void drain_and_wake(const char *name, uint32_t h)
{
    for (;;)
    {
        waiter_t *wb[16];
        parked_t *pb[8];

        uint32_t flags = spinlock_acquire_irqsave(&s_lock);
        uint32_t nw = collect_name_waiters(name, h, wb, ARRAY_SIZE(wb));
        uint32_t np = collect_parked(name, h, pb, ARRAY_SIZE(pb));
        spinlock_release_irqrestore(&s_lock, flags);

        for (uint32_t i = 0; i < nw; i++)
        {
            wait_queue_wake_all(&wb[i]->wq);
        }
        for (uint32_t i = 0; i < np; i++)
        {
            thread_t *t = thread_get_by_tid(pb[i]->tid);
            if (t != NULL)
            {
                scheduler_unblock(t);   /* il modulo parte SOLO ora */
            }
            kfree(pb[i]);
        }

        if (nw < ARRAY_SIZE(wb) && np < ARRAY_SIZE(pb))
        {
            return;                     /* lotti non pieni: liste asciutte */
        }
    }
}

int32_t registry_register(const char *name, uint32_t port_id,
                          int32_t owner_pid)
{
    if (name == NULL || name[0] == '\0')
    {
        return -1;
    }

    uint32_t h = hash_name(name);

    uint32_t flags = spinlock_acquire_irqsave(&s_lock);

    entry_t *e = entry_find(name, h);
    if (e != NULL)
    {
        /* Nome gia' presente: la ri-registrazione e' rifiutata solo se
         * il detentore e' un ALTRO processo ancora vivo. La liveness va
         * sondata: il cleanup di un processo morto puo' non essere
         * ancora passato, e una voce stantia non deve bloccare il
         * respawn del servizio. */
        if (e->owner_pid != owner_pid
            && process_get_by_pid(e->owner_pid) != NULL)
        {
            spinlock_release_irqrestore(&s_lock, flags);
            return -3;                  /* nome di un processo vivo */
        }
        e->port_id   = port_id;
        e->owner_pid = owner_pid;
    }
    else
    {
        e = entry_alloc();
        if (e == NULL)
        {
            spinlock_release_irqrestore(&s_lock, flags);
            return -2;                  /* tabella piena */
        }
        strlcpy(e->name, name, REGISTRY_NAME_MAX);
        e->name_hash = h;
        e->port_id   = port_id;
        e->owner_pid = owner_pid;
        e->active    = true;
    }

    spinlock_release_irqrestore(&s_lock, flags);

    /* La voce e' installata: chi arriva ADESSO la trova e non parcheggia.
     * Restano solo gli iscritti PRIMA di questo istante: drenali tutti. */
    drain_and_wake(name, h);
    return 0;
}

void registry_unregister(const char *name, int32_t owner_pid)
{
    if (name == NULL)
    {
        return;
    }

    uint32_t h = hash_name(name);
    uint32_t flags = spinlock_acquire_irqsave(&s_lock);
    entry_t *e = entry_find(name, h);
    if (e != NULL && e->owner_pid == owner_pid)     /* solo il detentore */
    {
        e->active = false;
    }
    spinlock_release_irqrestore(&s_lock, flags);
}

uint32_t registry_find(const char *name)
{
    if (name == NULL)
    {
        return 0;
    }

    uint32_t h = hash_name(name);
    uint32_t flags = spinlock_acquire_irqsave(&s_lock);
    entry_t *e = entry_find(name, h);
    uint32_t port = (e != NULL) ? e->port_id : 0;
    spinlock_release_irqrestore(&s_lock, flags);
    return port;
}

uint32_t registry_wait(const char *name, uint32_t timeout_ms)
{
    if (name == NULL)
    {
        return 0;
    }

    uint32_t h = hash_name(name);

    /* Record sul nostro stack: `name` punta al buffer del chiamante,
     * stabile finche' siamo bloccati. */
    waiter_t w;
    w.name      = name;
    w.name_hash = h;
    wait_queue_init(&w.wq);
    list_node_init(&w.node);

    uint32_t flags = spinlock_acquire_irqsave(&s_lock);

    entry_t *e = entry_find(name, h);
    if (e != NULL)
    {
        uint32_t port = e->port_id;
        spinlock_release_irqrestore(&s_lock, flags);
        return port;                    /* gia' registrato: zero attesa */
    }

    /* Aggancio + prepare sotto s_lock: una register concorrente o ci
     * trova in lista (e ci sveglia dopo l'enqueue), o ha scritto la
     * voce prima del nostro check. Nessun wake perso. */
    list_push_back(&s_waiters, &w.node);
    wait_prepare(&w.wq, timeout_ms);
    spinlock_release_irqrestore(&s_lock, flags);

    scheduler_yield();
    wait_finish();

    /* Sveglio (register, cleanup del detentore, o timeout): l'esito
     * giusto in ogni caso e' cio' che la tabella contiene ADESSO —
     * anche a timeout scattato il servizio puo' essere appena arrivato. */
    flags = spinlock_acquire_irqsave(&s_lock);
    if (list_node_is_linked(&w.node))   /* timeout: nessuno ci ha sganciati */
    {
        list_remove(&w.node);
    }
    e = entry_find(name, h);
    uint32_t port = (e != NULL) ? e->port_id : 0;
    spinlock_release_irqrestore(&s_lock, flags);
    return port;
}

void registry_park_or_start(const char *need_name, struct thread *t)
{
    if (t == NULL)
    {
        return;
    }
    if (need_name == NULL || need_name[0] == '\0')
    {
        scheduler_unblock(t);
        return;
    }

    uint32_t h = hash_name(need_name);
    parked_t *p = (parked_t *)kmalloc(sizeof(parked_t));

    uint32_t flags = spinlock_acquire_irqsave(&s_lock);
    if (entry_find(need_name, h) != NULL || p == NULL)
    {
        /* Bisogno gia' soddisfatto (o niente memoria per il record:
         * meglio partire subito che restare parcheggiati per sempre). */
        spinlock_release_irqrestore(&s_lock, flags);
        if (p != NULL)
        {
            kfree(p);
        }
        scheduler_unblock(t);
        return;
    }
    strlcpy(p->name, need_name, REGISTRY_NAME_MAX);
    p->name_hash = h;
    p->tid       = t->tid;
    p->owner_pid = (t->owner != NULL) ? t->owner->pid : -1;
    list_node_init(&p->node);
    list_push_back(&s_parked, &p->node);
    spinlock_release_irqrestore(&s_lock, flags);
}

void registry_cleanup_owner(int32_t owner_pid)
{
    /* Due bonifiche, entrambe a LOTTI RIPETUTI (stessa lezione di
     * drain_and_wake: fermarsi al primo lotto lascia eccedenti che
     * nessun evento futuro tocchera' mai piu').
     *
     * (1) Record di parcheggio del morto: via TUTTI. Un record
     *     superstite conserva un tid ormai stantio — al riciclo del
     *     TID, una register futura sbloccherebbe il thread SBAGLIATO.
     * (2) Voci del morto: TUTTE disattivate (la versione precedente
     *     smetteva di disattivare quando il batch dei waiter si
     *     riempiva: voci attive di un morto = registry_wait futuri
     *     serviti con una porta morta), e i waiter di ciascun nome
     *     svegliati a lotti — al re-check troveranno il servizio
     *     sparito e torneranno 0 invece di dormire per sempre. */

    for (;;)
    {
        parked_t *dead[8];
        uint32_t  ndead = 0;

        uint32_t flags = spinlock_acquire_irqsave(&s_lock);
        list_node_t *it = s_parked.head.next;
        while (it != &s_parked.head && ndead < ARRAY_SIZE(dead))
        {
            list_node_t *next = it->next;
            parked_t *p = list_entry(it, parked_t, node);
            if (p->owner_pid == owner_pid)
            {
                list_remove(&p->node);
                dead[ndead++] = p;
            }
            it = next;
        }
        spinlock_release_irqrestore(&s_lock, flags);

        for (uint32_t i = 0; i < ndead; i++)
        {
            kfree(dead[i]);
        }
        if (ndead < ARRAY_SIZE(dead))
        {
            break;                      /* lotto non pieno: lista asciutta */
        }
    }

    for (;;)
    {
        /* Nome copiato FUORI dal lock: disattivata la voce, lo slot e'
         * riutilizzabile da una register concorrente che ne riscrive il
         * contenuto — il collect dei waiter deve leggere la NOSTRA copia. */
        char     name[REGISTRY_NAME_MAX];
        uint32_t h     = 0;
        bool     found = false;

        uint32_t flags = spinlock_acquire_irqsave(&s_lock);
        for (uint32_t i = 0; i < s_count; i++)
        {
            if (s_entries[i].active && s_entries[i].owner_pid == owner_pid)
            {
                strlcpy(name, s_entries[i].name, sizeof(name));
                h = s_entries[i].name_hash;
                s_entries[i].active = false;
                found = true;
                break;
            }
        }
        spinlock_release_irqrestore(&s_lock, flags);

        if (!found)
        {
            return;                     /* nessun'altra voce del morto */
        }

        for (;;)
        {
            waiter_t *wb[16];
            uint32_t  fl2 = spinlock_acquire_irqsave(&s_lock);
            uint32_t  nw  = collect_name_waiters(name, h, wb,
                                                 ARRAY_SIZE(wb));
            spinlock_release_irqrestore(&s_lock, fl2);

            for (uint32_t i = 0; i < nw; i++)
            {
                wait_queue_wake_all(&wb[i]->wq);
            }
            if (nw < ARRAY_SIZE(wb))
            {
                break;
            }
        }
    }
}
