#include "sync/event_group.h"
#include "proc/scheduler.h"
#include "arch/x86/cpu.h"
#include "lib/string.h"

/* Ogni waiter porta un record sul proprio stack nella lista del
 * gruppo, con una wait-queue privata per il sonno: il wake mirato
 * scorre i record, non tocca thread_t ne' gli interni dello scheduler.
 * Il pulse sveglia con lo snapshot a bit alti e azzera subito: i nuovi
 * arrivati non vedono il fronte. */

typedef struct
{
    uint32_t     pattern;
    uint8_t      mode;
    volatile bool signaled;         /* scritto dal waker, riletto nel
                                     * ciclo prepare-first             */
    int          result;            /* WAIT_OK / WAIT_CANCELLED        */
    uint32_t     result_flags;      /* snapshot flags al match         */
    wait_queue_t wq;                /* sonno privato di questo waiter  */
    list_node_t  node;              /* link in group->waiters          */
} event_waiter_t;

typedef struct
{
    bool         used;
    bool         poisoned;
    pid_t        owner_pid;
    uint32_t     flags;
    spinlock_t   lock;              /* protegge tutto lo stato + la
                                     * lista waiters                   */
    list_t       waiters;
} event_group_t;

static event_group_t s_groups[MAX_EVENT_GROUPS];

/* === Verbi ============================================================= */

static bool pattern_match(uint32_t flags, uint32_t pattern, uint8_t mode)
{
    switch (mode)
    {
        case RT_ALL_SET:    return (flags & pattern) == pattern;
        case RT_ANY_SET:    return (flags & pattern) != 0;
        case RT_ALL_CLEAR:  return (flags & pattern) == 0;
        case RT_ANY_CLEAR:  return (flags & pattern) != pattern;
        default:            return false;
    }
}

static event_group_t *group_of(int32_t gid)
{
    if (gid < 0 || gid >= MAX_EVENT_GROUPS)
    {
        return NULL;
    }
    return &s_groups[gid];
}

/* Marca e sgancia (sotto g->lock) i waiter che combaciano con lo
 * snapshot; li sveglia FUORI dal lock. Una passata O(N). Se
 * `cancel_all` e' true ignora i pattern e cancella tutti. */
static void wake_waiters(event_group_t *g, uint32_t flags_snapshot,
                         bool cancel_all)
{
    event_waiter_t *batch[16];

    for (;;)
    {
        uint32_t n = 0;

        uint32_t fl = spinlock_acquire_irqsave(&g->lock);
        list_node_t *pos, *tmp;
        list_for_each_safe(pos, tmp, &g->waiters)
        {
            event_waiter_t *w = list_entry(pos, event_waiter_t, node);
            if (cancel_all
                || pattern_match(flags_snapshot, w->pattern, w->mode))
            {
                list_remove(&w->node);
                w->result       = cancel_all ? WAIT_CANCELLED : WAIT_OK;
                w->result_flags = flags_snapshot;
                w->signaled     = true;
                batch[n++] = w;
                if (n >= ARRAY_SIZE(batch))
                {
                    break;
                }
            }
        }
        spinlock_release_irqrestore(&g->lock, fl);

        /* Wake fuori da g->lock (niente inversione con wq->lock). Il
         * record sta sullo stack del waiter, che resta bloccato o in
         * timeout — e in timeout si auto-sgancia sotto g->lock, quindi
         * non puo' sparire tra il nostro unlink e questo wake. */
        for (uint32_t i = 0; i < n; i++)
        {
            wait_queue_wake_all(&batch[i]->wq);
        }

        if (n < ARRAY_SIZE(batch))
        {
            break;
        }
    }
}

/* Corpo comune di set/clear/pulse: applica la mutazione sotto lock,
 * cattura lo snapshot giusto e sveglia i combacianti. */
static int32_t mutate_and_wake(int32_t gid, uint32_t bits, uint32_t op)
{
    event_group_t *g = group_of(gid);
    if (g == NULL)
    {
        return -1;
    }

    uint32_t snapshot;
    uint32_t fl = spinlock_acquire_irqsave(&g->lock);
    if (!g->used || g->poisoned)
    {
        spinlock_release_irqrestore(&g->lock, fl);
        return -1;
    }
    switch (op)
    {
        case EVENT_OP_SET:
            g->flags |= bits;
            snapshot = g->flags;
            break;
        case EVENT_OP_CLEAR:
            g->flags &= ~bits;
            snapshot = g->flags;
            break;
        default: /* EVENT_OP_PULSE */
            g->flags |= bits;
            snapshot = g->flags;        /* i waiter vedono i bit ALTI...  */
            g->flags &= ~bits;          /* ...i nuovi arrivati no (fronte) */
            break;
    }
    spinlock_release_irqrestore(&g->lock, fl);

    wake_waiters(g, snapshot, false);
    return 0;
}

/* === API =============================================================== */

void event_group_init_subsystem(void)
{
    for (int i = 0; i < MAX_EVENT_GROUPS; i++)
    {
        s_groups[i].used = false;
        s_groups[i].poisoned = false;
        s_groups[i].flags = 0;
        spinlock_init(&s_groups[i].lock);
        list_init(&s_groups[i].waiters);
    }
}

int32_t event_group_create(pid_t owner)
{
    for (int i = 0; i < MAX_EVENT_GROUPS; i++)
    {
        event_group_t *g = &s_groups[i];
        uint32_t fl = spinlock_acquire_irqsave(&g->lock);
        if (!g->used)
        {
            g->used      = true;
            g->flags     = 0;
            g->poisoned  = false;
            g->owner_pid = owner;
            spinlock_release_irqrestore(&g->lock, fl);
            return i;
        }
        spinlock_release_irqrestore(&g->lock, fl);
    }
    return -1;
}

void event_group_destroy(int32_t gid)
{
    event_group_t *g = group_of(gid);
    if (g == NULL)
    {
        return;
    }

    uint32_t fl = spinlock_acquire_irqsave(&g->lock);
    if (!g->used)
    {
        spinlock_release_irqrestore(&g->lock, fl);
        return;
    }
    g->poisoned = true;
    spinlock_release_irqrestore(&g->lock, fl);

    wake_waiters(g, g->flags, true);    /* tutti fuori, WAIT_CANCELLED */

    fl = spinlock_acquire_irqsave(&g->lock);
    g->used      = false;
    g->flags     = 0;
    g->poisoned  = false;
    g->owner_pid = 0;
    spinlock_release_irqrestore(&g->lock, fl);
}

void event_group_cleanup_process(pid_t pid)
{
    for (int i = 0; i < MAX_EVENT_GROUPS; i++)
    {
        if (s_groups[i].used && s_groups[i].owner_pid == pid)
        {
            event_group_destroy(i);
        }
    }
}

int32_t event_group_set(int32_t gid, uint32_t bits)
{
    return mutate_and_wake(gid, bits, EVENT_OP_SET);
}

int32_t event_group_clear(int32_t gid, uint32_t bits)
{
    return mutate_and_wake(gid, bits, EVENT_OP_CLEAR);
}

int32_t event_group_pulse(int32_t gid, uint32_t bits)
{
    return mutate_and_wake(gid, bits, EVENT_OP_PULSE);
}

int32_t event_group_poison(int32_t gid)
{
    event_group_t *g = group_of(gid);
    if (g == NULL)
    {
        return -1;
    }

    uint32_t fl = spinlock_acquire_irqsave(&g->lock);
    if (!g->used)
    {
        spinlock_release_irqrestore(&g->lock, fl);
        return -1;
    }
    g->poisoned = true;
    spinlock_release_irqrestore(&g->lock, fl);

    wake_waiters(g, g->flags, true);
    return 0;
}

int32_t event_group_reset(int32_t gid)
{
    event_group_t *g = group_of(gid);
    if (g == NULL)
    {
        return -1;
    }

    uint32_t fl = spinlock_acquire_irqsave(&g->lock);
    if (!g->used)
    {
        spinlock_release_irqrestore(&g->lock, fl);
        return -1;
    }
    g->flags    = 0;
    g->poisoned = false;
    spinlock_release_irqrestore(&g->lock, fl);
    return 0;
}

uint32_t event_group_get_flags(int32_t gid)
{
    event_group_t *g = group_of(gid);
    if (g == NULL)
    {
        return 0;
    }
    return g->flags;                    /* lettura uint32 atomica su x86 */
}

int event_group_wait(int32_t gid, uint32_t pattern, uint8_t mode,
                     uint32_t timeout_ms, uint32_t *out_flags)
{
    event_group_t *g = group_of(gid);
    if (g == NULL)
    {
        return WAIT_TIMEOUT;            /* gid invalido */
    }

    /* Record sul nostro stack, come il pending_reply dell'IPC:
     * stabile finche' siamo bloccati o in fase di auto-sgancio. */
    event_waiter_t w;
    w.pattern  = pattern;
    w.mode     = mode;
    w.signaled = false;
    w.result   = WAIT_TIMEOUT;
    w.result_flags = 0;
    wait_queue_init(&w.wq);
    list_node_init(&w.node);

    uint32_t fl = spinlock_acquire_irqsave(&g->lock);
    if (!g->used)
    {
        spinlock_release_irqrestore(&g->lock, fl);
        return WAIT_TIMEOUT;
    }
    if (g->poisoned)
    {
        if (out_flags != NULL)
        {
            *out_flags = g->flags;
        }
        spinlock_release_irqrestore(&g->lock, fl);
        return WAIT_CANCELLED;
    }
    if (pattern_match(g->flags, pattern, mode))
    {
        if (out_flags != NULL)          /* match immediato: zero attesa */
        {
            *out_flags = g->flags;
        }
        spinlock_release_irqrestore(&g->lock, fl);
        return WAIT_OK;
    }

    list_push_back(&g->waiters, &w.node);   /* visibili PRIMA di dormire */
    spinlock_release_irqrestore(&g->lock, fl);

    /* Prepare-first + re-check su w.signaled sotto IF=0 (schema
     * anti-lost-wakeup: un set concorrente tra l'aggancio sopra e il
     * prepare ci ha gia' marcati — il re-check lo vede). */
    int result;
    for (;;)
    {
        uint32_t wfl = irq_save();
        wait_prepare(&w.wq, timeout_ms);
        if (w.signaled)
        {
            wait_cancel();
            irq_restore(wfl);
            result = w.result;
            break;
        }
        scheduler_yield();
        irq_restore(wfl);

        int wr = wait_finish();
        if (w.signaled)
        {
            result = w.result;          /* il waker ha gia' deciso */
            break;
        }
        if (wr == WAIT_TIMEOUT)
        {
            /* Timeout: auto-sgancio sotto g->lock. Se nel frattempo un
             * waker ci ha marcati (signaled) ha gia' fatto lui
             * l'unlink: vince il suo esito. */
            uint32_t tfl = spinlock_acquire_irqsave(&g->lock);
            bool raced = w.signaled;
            if (!raced && list_node_is_linked(&w.node))
            {
                list_remove(&w.node);
            }
            spinlock_release_irqrestore(&g->lock, tfl);

            result = raced ? w.result : WAIT_TIMEOUT;
            if (!raced)
            {
                w.result_flags = g->flags;
            }
            break;
        }
        /* Wake spurio: si rientra nel ciclo (siamo ancora in lista). */
    }

    if (out_flags != NULL)
    {
        *out_flags = w.result_flags;
    }
    return result;
}
