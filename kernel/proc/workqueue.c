#include "proc/workqueue.h"
#include "arch/x86/cpu.h"
#include "sync/spinlock.h"
#include "console/console.h"

/* Facility di lavoro BEST-EFFORT differito: gli IRQ accodano, idle
 * esegue in contesto thread. Prodotta e drenata da TUTTE le CPU, quindi
 * MPMC: irq_save (solo CPU locale) non bastava — serve uno spinlock che
 * possieda ring e indici. Il lavoro di RECUPERO critico (teardown di
 * processo, respawn) NON passa piu' di qui: usa i percorsi intrusivi
 * non-perdibili (teardown_q, respawn_pending). Cosi' lo scarto a coda
 * piena resta un evento benigno di lavoro best-effort. */

#define WQ_CAP 64u

typedef struct
{
    work_fn_t fn;
    void     *arg;
} work_item_t;

static work_item_t s_items[WQ_CAP];
static uint32_t    s_head;
static uint32_t    s_tail;
static uint32_t    s_dropped;
static spinlock_t  s_lock = SPINLOCK_INIT;

void workqueue_init(void)
{
    s_head = s_tail = 0;
    kprintf("[WKQ ] Workqueue pronta (cap=%u).\n", WQ_CAP);
}

bool workqueue_add(work_fn_t fn, void *arg)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_lock);
    uint32_t next = (s_tail + 1u) % WQ_CAP;

    if (next == s_head)
    {
        s_dropped++;
        spinlock_release_irqrestore(&s_lock, fl);
        kprintf("[WKQ ] piena: item scartato (#%u)\n", s_dropped);
        return false;                   /* D3: errore, non panic          */
    }

    s_items[s_tail].fn  = fn;
    s_items[s_tail].arg = arg;
    s_tail = next;
    spinlock_release_irqrestore(&s_lock, fl);
    return true;
}

bool workqueue_process_one(void)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_lock);
    if (s_head == s_tail)
    {
        spinlock_release_irqrestore(&s_lock, fl);
        return false;
    }
    work_item_t item = s_items[s_head];
    s_head = (s_head + 1u) % WQ_CAP;
    spinlock_release_irqrestore(&s_lock, fl);

    item.fn(item.arg);                  /* callback FUORI dal lock         */
    return true;
}
