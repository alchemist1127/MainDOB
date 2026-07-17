#include "krt/reclaim.h"
#include "time/clock.h"
#include "time/units.h"
#include "sync/spinlock.h"
#include "console/console.h"

/* Coordinatore di auto-pulizia — vedi il contratto in reclaim.h. */

#define RECLAIM_MAX_VERBS   8u
#define RECLAIM_PERIOD_NS   MS_TO_NS(2000)  /* un giro ogni 2 s di quiete */

typedef struct
{
    const char  *name;
    reclaim_fn_t fn;
} reclaim_verb_t;

static reclaim_verb_t s_verb[RECLAIM_MAX_VERBS];
static uint32_t       s_verb_n;             /* scritto solo a boot      */

static spinlock_t     s_gate = SPINLOCK_INIT;
static uint64_t       s_last_run_ns;        /* sotto s_gate             */

void reclaim_register(const char *name, reclaim_fn_t fn)
{
    if (fn == NULL || s_verb_n >= RECLAIM_MAX_VERBS)
    {
        kprintf("[RCLM] registro pieno o verbo nullo: '%s' ignorato\n",
                name != NULL ? name : "?");
        return;
    }
    s_verb[s_verb_n].name = name;
    s_verb[s_verb_n].fn   = fn;
    s_verb_n++;
}

void reclaim_idle_consider(void)
{
    /* Cancello: UNA CPU per periodo, in tutto il sistema. trylock —
     * se un'altra CPU idle sta gia' valutando, non c'e' nulla da
     * aggiungere: si dorme e basta. */
    if (!spinlock_try_acquire(&s_gate))
    {
        return;
    }
    uint64_t now = clock_now_ns();
    bool due = (now - s_last_run_ns >= RECLAIM_PERIOD_NS);
    if (due)
    {
        s_last_run_ns = now;
    }
    spinlock_release(&s_gate);
    if (!due)
    {
        return;
    }

    /* Il giro: fuori dal cancello (i verbi prendono i PROPRI lock e
     * possono chiamare kfree; il cancello serve solo al rate-limit).
     * Il registro e' in sola lettura dopo il boot: passeggiata nuda. */
    uint32_t total = 0;
    for (uint32_t i = 0; i < s_verb_n; i++)
    {
        total += s_verb[i].fn();
    }
    if (total != 0)
    {
        /* UNA riga, solo quando c'e' stato davvero un ritiro: il log
         * dell'eterno silenzio e' il silenzio. */
        kprintf("[RCLM] auto-pulizia: %u KB ritirati\n",
                (total + 1023u) / 1024u);
    }
}
