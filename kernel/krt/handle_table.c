#include "krt/handle_table.h"
#include "mm/kheap.h"
#include "lib/string.h"

/* === Verbi interni (lock del chiamante) ================================= */

static bool in_range(handle_table_t *t, uint32_t id)
{
    return id > 0 && id < t->capacity;
}

/* === API ================================================================ */

bool handle_table_init(handle_table_t *t, uint32_t capacity, ht_reuse_t reuse)
{
    t->reuse = reuse;
    t->slots      = (void **)kcalloc(capacity, sizeof(void *));
    t->generation = (uint32_t *)kcalloc(capacity, sizeof(uint32_t));
    t->freelist   = (uint32_t *)kcalloc(capacity, sizeof(uint32_t));

    if (t->slots == NULL || t->generation == NULL || t->freelist == NULL)
    {
        handle_table_destroy(t);
        return false;
    }

    /* Slot 0 riservato: id valido parte da 1. Il popolamento del freelist
     * e' funzione della politica cosi' che TUTTE assegnino prima gli id
     * bassi: FIFO consuma dalla testa del ring (ascendente), LIFO dalla
     * cima dello stack (discendente, come lo storico). MONOTONIC non usa
     * il freelist: scandisce slots[] da un cursore, quindi non lo popola. */
    if (reuse == HT_REUSE_FIFO)
    {
        for (uint32_t i = 1; i < capacity; i++)
        {
            t->freelist[i - 1] = i;
        }
    }
    else if (reuse == HT_REUSE_LIFO)
    {
        for (uint32_t i = 1; i < capacity; i++)
        {
            t->freelist[i - 1] = capacity - i;
        }
    }
    t->free_head  = 0;
    t->free_tail  = capacity - 1;   /* prima cella libera del ring      */
    t->free_count = capacity - 1;
    t->next_scan  = 1;              /* MONOTONIC: parte dal primo id valido */
    t->capacity   = capacity;
    spinlock_init(&t->lock);
    return true;
}

void handle_table_destroy(handle_table_t *t)
{
    kfree(t->slots);
    kfree(t->generation);
    kfree(t->freelist);
    t->slots      = NULL;
    t->generation = NULL;
    t->freelist   = NULL;
    t->capacity   = 0;
    t->free_head  = 0;
    t->free_tail  = 0;
    t->free_count = 0;
}

/* === Blocchi esecutivi: presa/ritorno di un id per politica ============= *
 * Chiamante tiene t->lock. free_count e slots[] li gestisce
 * l'orchestratore (comuni); questi toccano solo lo stato della POLITICA. */

static uint32_t id_take_fifo(handle_table_t *t)
{
    uint32_t id = t->freelist[t->free_head];
    t->free_head = (t->free_head + 1) % t->capacity;
    return id;
}

static uint32_t id_take_lifo(handle_table_t *t)
{
    return t->freelist[t->free_count - 1];      /* cima dello stack        */
}

/* Cursore rotante: il primo slot libero da next_scan in avanti, con wrap.
 * Un id appena liberato torna in gioco solo dopo che il cursore ha fatto
 * il giro completo -> ritardo di riuso massimo (semantica next_pid 1.0). */
static uint32_t id_take_monotonic(handle_table_t *t)
{
    uint32_t id = t->next_scan;
    for (uint32_t n = 1; n < t->capacity; n++)
    {
        if (id == 0) id = 1;                    /* id 0 riservato          */
        if (t->slots[id] == NULL)
        {
            t->next_scan = (id + 1u >= t->capacity) ? 1u : id + 1u;
            return id;
        }
        id = (id + 1u >= t->capacity) ? 1u : id + 1u;
    }
    return 0;   /* piena: l'orchestratore ha gia' escluso questo caso      */
}

static void id_return_fifo(handle_table_t *t, uint32_t id)
{
    t->freelist[t->free_tail] = id;             /* in CODA: riuso tardo    */
    t->free_tail = (t->free_tail + 1) % t->capacity;
}

static void id_return_lifo(handle_table_t *t, uint32_t id)
{
    t->freelist[t->free_count] = id;            /* in cima: riuso subito   */
}
/* MONOTONIC non ritorna nulla al freelist: lo slot libero e' slots[id]==NULL,
 * gia' azzerato dall'orchestratore, e il cursore lo ritrova da se'. */

/* Pubblica l'oggetto nello slot: store + nuova incarnazione. */
static void slot_publish(handle_table_t *t, uint32_t id, void *obj)
{
    t->slots[id] = obj;
    t->generation[id]++;
}

/* === Orchestratori: l'algoritmo in chiaro, la policy sceglie il percorso = */

handle_ref_t handle_table_insert(handle_table_t *t, void *obj)
{
    handle_ref_t ref = { 0, 0 };

    uint32_t fl = spinlock_acquire_irqsave(&t->lock);
    if (t->free_count == 0)
    {
        spinlock_release_irqrestore(&t->lock, fl);
        return ref;                     /* tabella piena                  */
    }

    /* La logica ad alto livello sceglie l'id secondo la policy; i blocchi
     * eseguono soltanto. */
    uint32_t id = (t->reuse == HT_REUSE_MONOTONIC) ? id_take_monotonic(t)
                : (t->reuse == HT_REUSE_FIFO)      ? id_take_fifo(t)
                :                                    id_take_lifo(t);
    t->free_count--;
    slot_publish(t, id, obj);

    ref.id  = id;
    ref.gen = t->generation[id];
    spinlock_release_irqrestore(&t->lock, fl);
    return ref;
}

void *handle_table_remove(handle_table_t *t, uint32_t id)
{
    uint32_t fl = spinlock_acquire_irqsave(&t->lock);
    if (!in_range(t, id) || t->slots[id] == NULL)
    {
        spinlock_release_irqrestore(&t->lock, fl);
        return NULL;
    }

    void *obj = t->slots[id];
    t->slots[id] = NULL;                 /* prima: cosi' il monotono lo vede libero */
    if (t->reuse == HT_REUSE_FIFO)
    {
        id_return_fifo(t, id);
    }
    else if (t->reuse == HT_REUSE_LIFO)
    {
        id_return_lifo(t, id);
    }
    /* MONOTONIC: nessun ritorno al freelist */
    t->free_count++;
    spinlock_release_irqrestore(&t->lock, fl);
    return obj;
}

void *handle_table_get(handle_table_t *t, uint32_t id)
{
    uint32_t fl = spinlock_acquire_irqsave(&t->lock);
    void *obj = in_range(t, id) ? t->slots[id] : NULL;
    spinlock_release_irqrestore(&t->lock, fl);
    return obj;
}

void *handle_table_get_checked(handle_table_t *t, handle_ref_t ref)
{
    uint32_t fl = spinlock_acquire_irqsave(&t->lock);
    void *obj = NULL;
    if (in_range(t, ref.id) && t->generation[ref.id] == ref.gen)
    {
        obj = t->slots[ref.id];
    }
    spinlock_release_irqrestore(&t->lock, fl);
    return obj;
}

uint32_t handle_table_generation(handle_table_t *t, uint32_t id)
{
    uint32_t fl = spinlock_acquire_irqsave(&t->lock);
    uint32_t gen = in_range(t, id) ? t->generation[id] : 0;
    spinlock_release_irqrestore(&t->lock, fl);
    return gen;
}
