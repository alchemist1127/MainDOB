#include "time/timer.h"
#include "time/clock.h"
#include "time/event.h"
#include "sync/spinlock.h"
#include "proc/scheduler.h"         /* scheduler_timer_drain_begin/end */
#include "console/console.h"
#include "kernel.h"
#include "mm/kheap.h"               /* heap_try_grow: storage dinamico  */
#include "lib/string.h"
#include "krt/reclaim.h"

/* Heap binario min di puntatori a timer (storage del chiamante);
 * heap_idx nel timer rende cancel/riarmo O(log n) senza ricerca.
 * s_lock (irqsave) protegge heap, heap_idx, gen e fire_pending; le
 * callback girano col lock mollato, cosi' possono (ri)armare timer. */

/* Capienza: bootstrap STATICO (vivo prima del kheap, zero dipendenze a
 * boot) + crescita geometrica a domanda fino a un tetto. La crescita e'
 * parte del comportamento normale del blocco: un arm che trova l'heap
 * pieno lo raddoppia e riprova — la saturazione visibile (log + degrado
 * a timeout immediato) resta solo per il tetto o l'OOM, mai come primo
 * destino. Un server con migliaia di timeout attivi non degrada. */
#define TIMER_HEAP_BOOT_CAP 1024u   /* 4 KB statici                     */
#define TIMER_HEAP_MAX_CAP  65536u  /* tetto: 256 KB di puntatori       */

static timer_t   *s_boot_heap[TIMER_HEAP_BOOT_CAP];
static timer_t  **s_heap = s_boot_heap;
static uint32_t   s_cap  = TIMER_HEAP_BOOT_CAP;
static uint32_t   s_count;
static spinlock_t s_lock = SPINLOCK_INIT;

/* === Primitive heap (chiamante tiene s_lock) =========================== */

static inline void heap_set(uint32_t i, timer_t *t)
{
    s_heap[i] = t;
    t->heap_idx = (int32_t)i;
}

static void heap_swap(uint32_t i, uint32_t j)
{
    timer_t *a = s_heap[i];
    timer_t *b = s_heap[j];
    heap_set(i, b);
    heap_set(j, a);
}

static void heap_sift_up(uint32_t i)
{
    while (i > 0)
    {
        uint32_t parent = (i - 1) / 2;
        if (s_heap[parent]->deadline_ns <= s_heap[i]->deadline_ns)
        {
            break;
        }
        heap_swap(i, parent);
        i = parent;
    }
}

static void heap_sift_down(uint32_t i)
{
    for (;;)
    {
        uint32_t l = 2 * i + 1;
        uint32_t r = 2 * i + 2;
        uint32_t smallest = i;

        if (l < s_count
            && s_heap[l]->deadline_ns < s_heap[smallest]->deadline_ns)
        {
            smallest = l;
        }
        if (r < s_count
            && s_heap[r]->deadline_ns < s_heap[smallest]->deadline_ns)
        {
            smallest = r;
        }
        if (smallest == i)
        {
            break;
        }
        heap_swap(i, smallest);
        i = smallest;
    }
}

static bool heap_insert(timer_t *t)
{
    if (unlikely(s_count >= s_cap))
    {
        return false;
    }
    uint32_t i = s_count++;
    heap_set(i, t);
    heap_sift_up(i);
    return true;
}

/* Rimuove lo slot i: la coda riempie il buco e risale o scende nella
 * direzione che ripristina l'invariante. */
static void heap_remove_at(uint32_t i)
{
    if (i >= s_count)                   /* cintura: indice fuori heap   */
    {
        return;
    }
    s_heap[i]->heap_idx = -1;

    s_count--;
    if (i == s_count)
    {
        return;                         /* rimossa la coda: finito */
    }

    timer_t *moved = s_heap[s_count];
    heap_set(i, moved);

    if (i > 0 && s_heap[(i - 1) / 2]->deadline_ns > moved->deadline_ns)
    {
        heap_sift_up(i);
    }
    else
    {
        heap_sift_down(i);
    }
}

/* === Verbi interni (chiamante tiene s_lock) ============================ */

/* Sgancia `t` se e' nell'heap (armo sostituito o cancel). VERIFICA
 * L'IDENTITA', non solo l'indice: un timer mai passato da timer_init
 * (BSS azzerata -> heap_idx == 0) o con back-index corrotto NON deve
 * toccare l'heap — senza questa guardia, armare un timer azzerato
 * chiamava heap_remove_at(0) e DECAPITAVA la scadenza di qualcun
 * altro (sleep rubati, pacing saltati; col churn giusto, heap con
 * conteggio incoerente fino alla scrittura su puntatore NULL nel
 * drain: il page fault cr2=0 attribuito al processo di turno). */
static void detach_if_armed(timer_t *t)
{
    if (t->heap_idx >= 0 &&
        (uint32_t)t->heap_idx < s_count &&
        s_heap[t->heap_idx] == t)
    {
        heap_remove_at((uint32_t)t->heap_idx);
    }
    t->heap_idx = -1;                   /* back-index coerente comunque */
}

static bool arm_locked(timer_t *t, uint64_t deadline_ns, uint64_t period_ns)
{
    detach_if_armed(t);
    t->gen++;                           /* nuovo episodio: i fire vecchi
                                         * diventano stantii            */
    t->deadline_ns = deadline_ns;
    t->period_ns   = period_ns;
    return heap_insert(t);
}

/* === Crescita dell'heap ================================================= */

/* Verbo esecutivo: raddoppia lo storage. Alloca FUORI dal lock, adotta
 * sotto lock solo se la capienza letta e' ancora quella che ha motivato
 * la chiamata (un altro CPU puo' aver gia' cresciuto: il perdente butta
 * il proprio array). L'array di bootstrap statico non si libera mai.
 * La memcpy sotto irqsave e' rara (log2 volte nella vita del sistema) e
 * limitata dal tetto: accettata a fronte di uno swap atomico semplice. */
static bool heap_try_grow(void)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_lock);
    uint32_t have = s_cap;
    spinlock_release_irqrestore(&s_lock, fl);

    uint32_t want = have * 2u;
    if (want > TIMER_HEAP_MAX_CAP)
    {
        return false;
    }

    timer_t **fresh = (timer_t **)kmalloc(want * sizeof(timer_t *));
    if (fresh == NULL)
    {
        return false;
    }

    timer_t **stale = NULL;
    fl = spinlock_acquire_irqsave(&s_lock);
    if (s_cap == have)
    {
        memcpy(fresh, s_heap, s_count * sizeof(timer_t *));
        stale  = (s_heap == s_boot_heap) ? NULL : s_heap;
        s_heap = fresh;
        s_cap  = want;
        fresh  = NULL;
    }
    spinlock_release_irqrestore(&s_lock, fl);

    if (fresh != NULL)
    {
        kfree(fresh);           /* gara persa: cresciuto da altri       */
    }
    if (stale != NULL)
    {
        kfree(stale);
    }
    return true;
}

/* Verbo di ritiro (auto-pulizia, vedi krt/reclaim.h): quando il picco
 * che aveva fatto crescere l'heap e' passato, lo storage torna verso il
 * bootstrap. Simmetrico alla crescita: alloca (o adotta il boot array)
 * FUORI dal lock, scambia sotto lock solo se la condizione regge ancora
 * (isteresi: si dimezza solo se il contenuto sta comodo in un QUARTO
 * della capienza — mai ritirare cio' che il carico corrente usa, mai
 * oscillare col carico che respira). Ritorna i byte restituiti. */
static uint32_t timer_heap_trim(void)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_lock);
    uint32_t cap = s_cap, cnt = s_count;
    spinlock_release_irqrestore(&s_lock, fl);

    if (cap <= TIMER_HEAP_BOOT_CAP || cnt > cap / 4u)
    {
        return 0;                       /* a riposo, o ancora sotto uso */
    }
    uint32_t want = cap / 2u;
    if (want < TIMER_HEAP_BOOT_CAP)
    {
        want = TIMER_HEAP_BOOT_CAP;
    }

    timer_t **fresh = (want == TIMER_HEAP_BOOT_CAP)
                    ? s_boot_heap       /* si torna a casa: zero alloc  */
                    : (timer_t **)kmalloc(want * sizeof(timer_t *));
    if (fresh == NULL)
    {
        return 0;                       /* niente memoria: si riprova   */
    }

    timer_t **stale = NULL;
    uint32_t  freed = 0;
    fl = spinlock_acquire_irqsave(&s_lock);
    if (s_cap == cap && s_count <= want / 2u)
    {
        memcpy(fresh, s_heap, s_count * sizeof(timer_t *));
        stale  = s_heap;                /* mai il boot array: cap>BOOT  */
        s_heap = fresh;
        freed  = (cap - want) * (uint32_t)sizeof(timer_t *);
        s_cap  = want;
        fresh  = NULL;
    }
    spinlock_release_irqrestore(&s_lock, fl);

    if (fresh != NULL && fresh != s_boot_heap)
    {
        kfree(fresh);                   /* condizione evaporata          */
    }
    if (stale != NULL)
    {
        kfree(stale);
    }
    return freed;
}

/* Livello logico dell'armo: prova, e se l'heap e' pieno cresce e
 * riprova — il fallimento vero (tetto/OOM) resta visibile al chiamante
 * e nel log. time_event_refresh SOLO fuori dal lock (contratto suo). */
static bool arm_with_growth(timer_t *t, uint64_t deadline_ns,
                            uint64_t period_ns)
{
    for (;;)
    {
        uint32_t flags = spinlock_acquire_irqsave(&s_lock);
        bool ok = arm_locked(t, deadline_ns, period_ns);
        bool is_head = ok && (s_heap[0] == t);
        spinlock_release_irqrestore(&s_lock, flags);

        if (ok)
        {
            if (is_head)
            {
                time_event_refresh();   /* nuova prima scadenza: riarma */
            }
            return true;
        }
        if (!heap_try_grow())
        {
            kprintf("[TMR ] heap pieno (cap=%u, tetto=%u): armo "
                    "rifiutato\n", s_cap, TIMER_HEAP_MAX_CAP);
            return false;
        }
    }
}

/* === API =============================================================== */

void timer_subsystem_init(void)
{
    reclaim_register("timer-heap", timer_heap_trim);

    uint32_t flags = spinlock_acquire_irqsave(&s_lock);
    s_count = 0;
    spinlock_release_irqrestore(&s_lock, flags);
}

/* CONTRATTO: mai su un timer potenzialmente ARMATO — azzera il
 * back-index senza toccare l'heap (la ruota ora si difende comunque:
 * detach_if_armed verifica l'identita'). L'idioma corretto per i pool
 * e' init UNA volta a boot e poi solo arm/cancel: arm_locked gestisce
 * gia' il ri-armo (detach + gen++). */
void timer_init(timer_t *t, timer_fn_t fn, void *arg)
{
    t->deadline_ns  = 0;
    t->period_ns    = 0;
    t->fn           = fn;
    t->arg          = arg;
    t->heap_idx     = -1;
    t->gen          = 0;
    t->fire_pending = 0;
}

bool timer_arm_at(timer_t *t, uint64_t deadline_ns)
{
    return arm_with_growth(t, deadline_ns, 0);
}

bool timer_arm_in(timer_t *t, uint64_t delay_ns)
{
    return timer_arm_at(t, clock_now_ns() + delay_ns);
}

bool timer_arm_periodic(timer_t *t, uint64_t period_ns)
{
    return arm_with_growth(t, clock_now_ns() + period_ns, period_ns);
}

void timer_cancel(timer_t *t)
{
    uint32_t flags = spinlock_acquire_irqsave(&s_lock);
    detach_if_armed(t);
    t->gen++;                           /* fire gia' drenati: scartati */
    t->deadline_ns = 0;
    t->period_ns   = 0;
    spinlock_release_irqrestore(&s_lock, flags);
}

uint64_t timer_next_deadline_ns(void)
{
    uint32_t flags = spinlock_acquire_irqsave(&s_lock);
    uint64_t next = (s_count > 0) ? s_heap[0]->deadline_ns : UINT64_MAX;
    spinlock_release_irqrestore(&s_lock, flags);
    return next;
}

/* === Drain (evento LAPIC o tick PIT in fallback) ======================== */

/* Segnala (rate-limited) che un arm e' fallito per heap saturo e che
 * un'attesa temporizzata e' stata degradata a timeout immediato: rende
 * VISIBILE la saturazione dell'heap invece di nasconderla in un hang. */
void timer_note_arm_saturation(void)
{
    static uint32_t n;      /* diagnostica: una race benigna salta una riga */
    uint32_t k = n++;
    if (k < 4u || (k & 1023u) == 0u)
    {
        kprintf("[TMR ] heap timer saturo (%u): attesa temporizzata "
                "degradata a timeout immediato\n", k + 1);
    }
}

void timer_on_tick(void)
{
    uint64_t now = clock_now_ns();

    /* Percorso vuoto/non maturo: un lock, un confronto, fuori. */
    uint32_t flags = spinlock_acquire_irqsave(&s_lock);
    if (s_count == 0 || s_heap[0]->deadline_ns > now)
    {
        spinlock_release_irqrestore(&s_lock, flags);
        return;
    }

    /* Scadenze mature: gli switch chiesti dalle callback si consolidano
     * nel punto unico post-EOI (contratto con lo scheduler). */
    scheduler_timer_drain_begin();

    while (s_count > 0 && s_heap[0]->deadline_ns <= now)
    {
        timer_t *t = s_heap[0];
        uint32_t fire_gen = t->gen;     /* cattura per il check stantio */

        heap_remove_at(0);

        if (t->period_ns != 0)
        {
            /* Riarmo drift-free sulla cadenza originale. Stesso
             * episodio: la gen NON avanza (heap_insert diretto, non
             * arm_locked). Heap pieno qui e' impossibile: uno slot si
             * e' appena liberato. */
            t->deadline_ns += t->period_ns;
            /* Ri-fasatura anti-storm: se dopo un periodo siamo ANCORA
             * indietro (il timer e' rimasto indietro di piu' di un
             * periodo — stallo IRQ, salto d'ancora), NON si recupera
             * sparando N callback in questo stesso drain: si ri-fasa a
             * now+period e si riprende la cadenza. Una sola callback per
             * drain, latenza IRQ limitata. */
            if (t->deadline_ns <= now)
            {
                t->deadline_ns = now + t->period_ns;
            }
            heap_insert(t);
        }
        else
        {
            t->deadline_ns = 0;
        }

        t->fire_pending = 1;
        spinlock_release_irqrestore(&s_lock, flags);

        /* Check anti-stantio: se la gen e' avanzata tra il pop e qui
         * (cancel/riarmo concorrente), l'episodio della callback e'
         * finito — si salta. Su UP con IF=0 la finestra e' chiusa per
         * costruzione; il check resta per l'SMP (costa un confronto). */
        if (t->gen == fire_gen)
        {
            t->fn(t->arg);
        }
        t->fire_pending = 0;

        flags = spinlock_acquire_irqsave(&s_lock);
    }

    scheduler_timer_drain_end();
    spinlock_release_irqrestore(&s_lock, flags);
}
