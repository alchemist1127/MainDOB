#include "proc/scheduler.h"
#include "proc/wait.h"
#include "proc/process.h"
#include "proc/percpu.h"
#include "arch/x86/cpu.h"
#include "arch/x86/gdt.h"
#include "arch/x86/fpu.h"
#include "krt/reclaim.h"
#include "arch/x86/lapic.h"
#include "time/timer.h"
#include "time/clock.h"
#include "time/event.h"
#include "sync/spinlock.h"
#include "sync/atomic.h"
#include "lib/list.h"
#include "lib/string.h"
#include "console/console.h"
#include "kernel.h"

/* Scheduler pinnato per-CPU — priorita' stretta a 4 livelli.
 *
 * Ogni CPU ha runqueue, quanto e idle propri; un thread vive sulla sua
 * home_cpu e non migra. Le operazioni locali e gli enqueue remoti
 * passano dal lock della CPU bersaglio; un wake cross-core segue con
 * una IPI di reschedule, cosi' il bersaglio rivaluta subito.
 *
 * Un IRQ non switcha mai direttamente: slice scaduta e wake-preemption
 * alzano need_resched della CPU, e l'unico punto di switch da
 * interrupt e' scheduler_preempt_if_needed (post-EOI, o handler della
 * IPI di reschedule). Nessun lock di scheduling e' mai tenuto
 * attraverso un context_switch. */

/* Quanti in ns: il quanto e' una SCADENZA armata (modo eventi) o
 * confrontata al tick (fallback PIT), mai un contatore da scalare. */
static const uint64_t s_quantum_ns[SCHED_NUM_PRIORITIES] =
{
    2u * 1000000ull, 5u * 1000000ull, 10u * 1000000ull, 20u * 1000000ull
};

/* Budget della valvola di equita' RT (vedi sched_cpu_t): monopolio
 * continuo di prio 0 con lavoro affamato sotto, oltre il quale il pick
 * concede UN quanto ai livelli inferiori. 100 ms = 50 quanti RT pieni:
 * nessun RT sano ci arriva mai; un RT rotto costa al sistema al piu'
 * un quanto inferiore ogni 100 ms invece del congelamento perpetuo. */
#define SCHED_RT_BURST_BUDGET_NS   (100ull * 1000000ull)

/* Diagnosi della valvola: rate-limited, la prima volta e' quella che
 * conta (un RT che monopolizza e' un bug del driver, non un evento). */
static void rt_relief_note(thread_t *hog)
{
    static uint32_t n;
    uint32_t k = n++;
    if (k < 4u || (k & 255u) == 0u)
    {
        kprintf("[SCHD] prio 0 (tid %d) monopolizza il core con lavoro "
                "affamato sotto: concesso un quanto inferiore (#%u)\n",
                hog->tid, k + 1u);
    }
}

typedef struct
{
    spinlock_t        lock;         /* runqueue + mask + need_resched   */
    list_t            run_queue[SCHED_NUM_PRIORITIES];
    uint32_t          ready_mask;
    volatile uint64_t slice_deadline; /* fine quanto (ns), UINT64_MAX=idle */
    volatile bool     need_resched;
    volatile bool     in_timer_drain;
    thread_t         *idle;
    bool              online;       /* questa CPU esegue lo scheduler   */

    /* "Questa CPU sta dormendo in hlt". Scritto SOLO da questa CPU (nel
     * blocco idle e in switch_to: single-writer), letto dai waker remoti
     * sotto barriera. E' il segnale AFFIDABILE che sostituisce la lettura
     * in gara di g_cpus[].current: un waker che lo vede alzato DEVE
     * mandare l'IPI di sveglia, perche' una CPU idle non si rivaluta da
     * se'. La correttezza sta nella barriera simmetrica (vedi
     * enqueue_home_and_kick e scheduler_idle_block). */
    volatile bool     cpu_idle;

    /* Priorita' del thread SCELTO per girare su questa CPU
     * (SCHED_NUM_PRIORITIES = idle). Scritta ESCLUSIVAMENTE sotto
     * sc->lock, da ogni punto che decide chi gira: reschedule,
     * scheduler_yield_to, scheduler_set_priority sul running.
     *
     * Perche' la lettura del waker (fuori lock, in
     * enqueue_home_and_kick) e' affidabile: il waker legge DOPO aver
     * chiuso la propria sezione sc->lock di enqueue. Se la decisione
     * remota e' avvenuta PRIMA di quell'enqueue, il release del lock
     * (store su TSO) rende running_prio visibile all'acquire del
     * waker; se avviene DOPO, il pick vede gia' il thread accodato e
     * — se outranks — lo sceglie da se'. In entrambi i casi nessun
     * wake outranking resta senza consegna; al peggio parte una IPI
     * spuria, che il giudice smonta sotto il proprio lock. */
    volatile uint32_t running_prio;

    /* Thread homed su questa CPU. Contatore ADVISORY per il placement
     * (scheduler_pick_cpu_local): inc/dec atomici, letture senza lock.
     * Una gara qui produce al massimo un piazzamento subottimale, mai
     * un errore di correttezza. */
    volatile uint32_t nr_homed;

    /* Profondita' runnable ISTANTANEA (thread nelle run_queue). ESATTA,
     * non advisory: ogni mutazione avviene sotto sc->lock negli stessi
     * punti che toccano le code (enqueue_ready / pop di pick_next /
     * scheduler_remove / riaccodo di set_priority) — costo zero, la
     * cache line e' gia' in scrittura. Il placement la legge senza
     * lock (istantanea comunque): i runnable pesano piu' dei homed
     * dormienti nella metrica di carico (vedi placement_load_of). */
    volatile uint32_t nr_ready;

    /* === Migrazione cooperativa (D) ====================================
     * migrate_req: slot di RICHIESTA. Un core idle che vuole lavoro
     * pubblica qui il proprio indice (CAS da MIGRATE_NONE) e manda una
     * IPI di resched: NON ruba mai dalla coda altrui. E' il donatore —
     * cioe' QUESTA CPU, al proprio prossimo punto di giudizio — a
     * scegliere e trasferire un intero processo. "Solo la home muta le
     * proprie code e ri-homa i propri processi": il pilastro che rende
     * il direct-switch IPC (check home==self sotto IF=0) immune alla
     * migrazione senza alcun lock aggiunto sul percorso caldo.
     * last_pull_ns: rate-limit delle RICHIESTE emesse da questa CPU
     * quando e' lei l'affamata (anti-tempesta di IPI). */
    volatile uint32_t migrate_req;
    uint64_t          last_pull_ns;

    /* Contabilita' del tempo (task manager). last_dispatch_ns: istante
     * in cui current e' salito su questa CPU; idle_time_ns: ozio
     * accumulato. Single-writer (switch_to di questa CPU); i lettori
     * remoti rileggono fino a stabilita' (u64 su i386). */
    uint64_t          last_dispatch_ns;
    uint64_t          idle_time_ns;

    /* === Valvola di equita' RT ==========================================
     * La priorita' 0 e' STRETTA per contratto (prevedibilita' > equita')
     * — ma un thread RT impazzito in busy-loop congelerebbe prio 1..3 su
     * questo core PER SEMPRE, watchdog compresi se i loro callback
     * devono girare qui. La valvola limita il danno senza rompere il
     * contratto: se prio 0 monopolizza il core per rt_burst_ns oltre il
     * budget MENTRE c'e' lavoro pronto sotto, il pick successivo concede
     * UN quanto al migliore dei livelli inferiori, poi l'RT riprende.
     * Costo per l'RT sano: zero (la valvola matura solo se il lavoro
     * sotto resta affamato per l'intero budget). Costo per l'RT rotto:
     * un quanto ogni budget — latenza limitata e sistema che respira,
     * log compreso. Entrambi i campi vivono sotto sc->lock (slice check
     * e pick girano gia' li' o con IRQ spenti sulla propria CPU). */
    uint64_t          rt_burst_ns;  /* monopolio prio-0 con fame sotto  */
    bool              rt_relief;    /* concesso: il pick salta prio 0   */

    /* Isteresi di migrazione. contended_since_ns: istante in cui la
     * coda ha smesso di essere vuota (0 = nessuna contesa in corso);
     * scritto solo da questa CPU nella slice check, letto ADVISORY dai
     * puller remoti (u64 su i386 puo' strapparsi: al peggio una
     * richiesta spuria o mancata, e la donazione resta l'unico giudice
     * — mai un errore di correttezza). last_offer_ns: rate-limit delle
     * offerte del donatore, solo nostro. */
    volatile uint64_t contended_since_ns;
    uint64_t          last_offer_ns;

    /* Proprietario dello stato FPU FISICO di questa CPU: l'ultimo
     * thread il cui contesto FP e' nei registri. Il kernel e' compilato
     * -mno-80387/-mno-sse (non tocca MAI FP), e l'idle men che meno:
     * tra un fxsave e il ritorno dello STESSO thread i registri sono
     * intonsi, e il fxrstor da 512 B e' puro spreco — ed e' il caso
     * piu' comune del mondo event-driven (A -> idle -> A a ogni
     * evento). Save sempre EAGER (lo stato e' persistito a ogni
     * deschedule: la migrazione trova sempre memoria aggiornata);
     * restore SOLO se il proprietario e' un altro. Invalidato dal reap
     * del thread (scheduler_fpu_owner_forget): il riciclo dell'indirizzo
     * da parte dell'allocatore non puo' produrre un falso match. */
    thread_t         *fpu_owner;

    /* Teardown/reclamation instradati su questa CPU (la home dei thread).
     * Un thread morto e' mietuto SOLO dalla sua home-core, nel suo idle:
     * a quel punto ha gia' fatto lo switch via, quindi liberarne lo stack
     * e' sicuro (niente reaper-UAF cross-core). teardown_q raccoglie i
     * process_destroy instradati qui da altri core. Lock dedicato: non
     * tocca la runqueue, nessun ordine di lock da rispettare. */
    spinlock_t        tw_lock;
    list_t            reap_list;    /* cadaveri thread homed qui         */
    list_t            teardown_q;   /* processi da distruggere qui       */
} __attribute__((aligned(64))) sched_cpu_t;
/* aligned(64): ogni elemento di s_cpu[] parte su una cache line
 * propria. Senza, i campi PIU' CALDI del kernel (lock, run_queue,
 * need_resched) di due CPU adiacenti condividono una linea: ogni
 * acquire/enqueue di una invalida la linea dell'altra — false sharing
 * puro sul percorso di scheduling. La 1.0 lo curava esplicitamente;
 * qui era andato perso nella riscrittura. */

static sched_cpu_t s_cpu[MAX_CPUS];
static bool        s_active;

/* Migrazione cooperativa: parametri. */
#define MIGRATE_NONE            0xFFFFFFFFu
#define PULL_MIN_INTERVAL_NS    MS_TO_NS(5)     /* tra due richieste        */
#define MIGRATION_COOLDOWN_NS   MS_TO_NS(250)   /* per-processo, anti-thrash */

/* Soglia di SOVRACCARICO del bersaglio del pull: si chiede lavoro solo a
 * chi ha ALMENO 2 runnable in coda. Con 1 solo, quel thread girera'
 * comunque entro <= 1 quanto sul suo core: rubarlo compra millisecondi
 * di latenza al prezzo della localita' — ed e' ESATTAMENTE il ping-pong
 * osservato sulle coppie IPC (client/server in alternanza: in ogni
 * istante un core lavora e l'altro, idle, vedeva il peer appena
 * svegliato in coda e lo strappava via, spezzando la coppia; poi i
 * ruoli si invertivano e lo strappava indietro). Con >= 2 in coda c'e'
 * contesa VERA: li' il furto paga. */
#define PULL_MIN_READY          2u

/* Isteresi degli inneschi di migrazione (lezione b132): la contesa
 * ISTANTANEA e' il rumore di fondo di qualunque core che lavora — le
 * cascate IPC producono code a 1-2 per microsecondi, decine di volte
 * al secondo. Migrare su quel segnale = inseguire il rumore. Un
 * innesco vale solo se la contesa PERSISTE (il core e' davvero corto
 * di CPU da un tempo percettibile), e le offerte del donatore sono
 * comunque diradate. */
#define CONTENTION_PERSIST_NS   MS_TO_NS(50)    /* contesa che conta        */
#define DONOR_OFFER_INTERVAL_NS MS_TO_NS(100)   /* tra due offerte          */

extern void context_switch(context_t *old, context_t *new_ctx);

static inline sched_cpu_t *sched_here(void)
{
    return &s_cpu[this_cpu()->cpu_index];
}

static inline sched_cpu_t *sched_of(uint32_t cpu)
{
    return &s_cpu[cpu];
}

/* === Verbi runqueue (chiamante tiene sc->lock) ========================== */

static void enqueue_ready(sched_cpu_t *sc, thread_t *t)
{
    list_push_back(&sc->run_queue[t->priority], &t->sched_node);
    sc->ready_mask |= 1u << t->priority;
    sc->nr_ready++;                     /* esatto: sotto sc->lock        */
}

static void refresh_mask(sched_cpu_t *sc, uint32_t prio)
{
    if (list_empty(&sc->run_queue[prio]))
    {
        sc->ready_mask &= ~(1u << prio);
    }
}

static uint32_t highest_ready_priority(sched_cpu_t *sc)
{
    if (sc->ready_mask == 0)
    {
        return SCHED_NUM_PRIORITIES;
    }
    return (uint32_t)__builtin_ctz(sc->ready_mask);
}

static thread_t *pick_next(sched_cpu_t *sc)
{
    while (sc->ready_mask != 0)
    {
        uint32_t prio = (uint32_t)__builtin_ctz(sc->ready_mask);

        /* Esecuzione della valvola di equita' RT (decisa in
         * scheduler_slice_check): salta il livello 0 UNA volta a favore
         * del migliore sotto. Il flag si consuma comunque — se sotto
         * non c'e' piu' nulla, la concessione decade senza effetti. */
        if (unlikely(sc->rt_relief))
        {
            sc->rt_relief = false;
            uint32_t below = sc->ready_mask & ~1u;
            if (prio == 0 && below != 0)
            {
                prio = (uint32_t)__builtin_ctz(below);
                sc->rt_burst_ns = 0;
            }
        }

        list_node_t *node = list_pop_front(&sc->run_queue[prio]);
        refresh_mask(sc, prio);
        sc->nr_ready--;                 /* ogni pop, anche gli scarti    */

        thread_t *t = list_entry(node, thread_t, sched_node);
        if (unlikely(t->state == THREAD_DEAD))
        {
            continue;                   /* distrutto mentre era READY     */
        }
        return t;
    }
    return NULL;
}

/* Idle non vive nelle runqueue: e' il fallback permanente di ogni CPU.
 * Se manca quando serve, l'invariante strutturale e' rotta: panic
 * legittimo. */
static thread_t *pick_next_or_idle(sched_cpu_t *sc)
{
    thread_t *next = pick_next(sc);
    if (next != NULL)
    {
        return next;
    }
    if (sc->idle != NULL && sc->idle != current_thread)
    {
        return sc->idle;
    }
    return NULL;
}

/* Verbo: lock della runqueue HOME di t, robusto alla migrazione.
 * Leggi home, prendi quel lock, RILEGGI home sotto il lock: se e'
 * cambiata nel frattempo, molla e riprova. Il donatore flippa
 * t->home_cpu tenendo ENTRAMBI i lock sc (vecchia e nuova home),
 * quindi: o acquisiamo il lock vecchio PRIMA del flip (home ancora
 * valida), o DOPO (rilettura diversa -> retry sul lock nuovo, che il
 * donatore tiene fino a fine trasferimento -> ci serializziamo dietro
 * di lui). Convergenza in <= 2 giri. Chiamare con IRQ gia' disattivi
 * (irq_save del chiamante); al ritorno il lock della home VERA di t e'
 * tenuto e home_cpu non puo' cambiare finche' lo teniamo. */
static sched_cpu_t *lock_home_queue(thread_t *t)
{
    for (;;)
    {
        uint32_t     home = t->home_cpu;
        sched_cpu_t *sc   = sched_of(home);
        spinlock_acquire(&sc->lock);
        if (t->home_cpu == home)
        {
            return sc;
        }
        spinlock_release(&sc->lock);    /* migrato sotto di noi: retry  */
    }
}

static void requeue_running_current(sched_cpu_t *sc)
{
    thread_t *t = current_thread;
    if (t->state != THREAD_RUNNING)
    {
        t->cpu_hungry = false;          /* lascia la CPU da bloccato:    */
        return;                         /* conversazione, non compute    */
    }
    t->state = THREAD_READY;
    t->cpu_hungry = true;               /* preempted ancora runnable:    */
    if (!t->is_idle)                    /* voleva girare = compute       */
    {
        enqueue_ready(sc, t);           /* idle resta fuori dalle code    */
    }
}

/* === Verbo switch (IF=0, NESSUN lock di sched tenuto) =================== */

static void switch_to(sched_cpu_t *sc, thread_t *next)
{
    thread_t *prev = current_thread;
    if (next == prev)
    {
        /* Ri-selezione dopo un wake ridondante: ripristina RUNNING cosi'
         * la prossima preemption lo rimette in coda invece di perderlo. */
        if (prev->state == THREAD_READY)
        {
            prev->state = THREAD_RUNNING;
        }
        return;
    }

    current_thread  = next;
    next->state     = THREAD_RUNNING;

    /* Contabilita' del tempo: prev ha occupato la CPU da last_dispatch
     * a ORA. Una sola lettura di clock, riusata sotto per la scadenza
     * del quanto (il clock e' economico dal b115, ma non si spreca).
     * Single-writer: solo questa CPU scrive i propri contatori e il
     * cpu_time_ns dei thread homed qui. */
    uint64_t now = clock_now_ns();
    uint64_t ran = now - sc->last_dispatch_ns;
    if (prev->is_idle)
    {
        sc->idle_time_ns += ran;        /* ozio per-CPU (uso = 1-idle)   */
    }
    else
    {
        prev->cpu_time_ns += ran;       /* lavoro del thread             */
    }
    sc->last_dispatch_ns = now;

#ifdef MAINDOB_SMP
    /* Pubblica chi gira ORA su questa CPU. Se e' un thread reale,
     * cpu_idle scende a false: i waker remoti smettono di mandarle IPI
     * di sveglia, e — soprattutto — si chiude la finestra in cui l'hlt
     * del blocco idle viene interrotto da una IPI che switcha a un
     * thread reale SENZA che il blocco idle possa azzerare il flag da
     * se' (resterebbe true mentre un thread reale gira). Qui il flag
     * segue sempre la verita'. Single-writer: solo questa CPU scrive il
     * proprio cpu_idle. */
    sc->cpu_idle = next->is_idle;
#endif

    sc->slice_deadline = next->is_idle
                       ? UINT64_MAX
                       : now + s_quantum_ns[next->priority];
    time_event_refresh();               /* arma la fine quanto (modo eventi) */

    gdt_set_kernel_stack(next->kernel_stack_top);

    /* FPU: save EAGER (mai perdere stato), restore LAZY per
     * proprietario (vedi fpu_owner nella sched_cpu_t). Idle non tocca
     * mai FP: fuori da entrambe le operazioni per costruzione. */
    if (!prev->is_idle)
    {
        fpu_save(fpu_area(prev));
        sc->fpu_owner = prev;           /* registri == memoria di prev  */
    }
    if (!next->is_idle && sc->fpu_owner != next)
    {
        fpu_restore(fpu_area(next));
        sc->fpu_owner = next;
    }

    /* Il CR3 e' STATO del thread, non identita' del suo owner: la fase
     * in-driver del boomerang (int 0x85) presta al thread del chiamante
     * l'address space del driver video, e il thread puo' essere
     * prelazionato LI' DENTRO (quanto scaduto, wake di un pari grado) o
     * bloccarsi legittimamente (sbrk del driver, contesa heap). Senza
     * questo salvataggio, il resume ricaricava la directory del
     * PROPRIETARIO: dispatch ripreso con l'AS del chiamante e EIP nel
     * codice del driver — stesso layout di base, quindi il fetch non
     * fault-a e l'esecuzione prosegue nel testo del processo SBAGLIATO
     * fino alla prima lettura di un indirizzo solo-del-driver (il
     * fault [VID] deterministico), o peggio scrive e corrompe. Per i
     * thread ordinari il CR3 vivo E' quello dell'owner: invariante. */
    prev->context.cr3 = cpu_read_cr3();

    this_cpu()->loaded_cr3 = next->context.cr3;
    context_switch(&prev->context, &next->context);
}

/* Estrae il prossimo da eseguire sotto sc->lock e switcha col lock
 * MOLLATO. `must_leave` distingue il cedere volontario (resta RUNNING
 * se non c'e' di meglio) dall'obbligo di lasciare la CPU (blocco). */
static void reschedule(sched_cpu_t *sc, bool must_leave)
{
    uint32_t fl = spinlock_acquire_irqsave(&sc->lock);

    thread_t *next = must_leave ? pick_next_or_idle(sc) : pick_next(sc);
    if (next == NULL)
    {
        spinlock_release_irqrestore(&sc->lock, fl);
        if (must_leave)
        {
            kpanic("scheduler: nemmeno idle disponibile (cpu %u)",
                   this_cpu()->cpu_index);
        }
        return;
    }
    requeue_running_current(sc);

    /* Pubblica chi e' stato SCELTO, ancora sotto il lock: e' la meta'
     * "decisore" del protocollo con enqueue_home_and_kick (vedi il
     * commento di running_prio nella sched_cpu_t). */
    sc->running_prio = next->is_idle ? SCHED_NUM_PRIORITIES
                                     : next->priority;
    spinlock_release(&sc->lock);        /* IF resta 0 fino a dopo lo switch */

    switch_to(sc, next);
    irq_restore(fl);
}

/* Dimentica un thread come proprietario FPU su ogni CPU: chiamato dal
 * reap PRIMA che la struct torni all'allocatore. Senza, il riciclo
 * dell'indirizzo per un thread nuovo produrrebbe un falso match e il
 * nuovo erediterebbe i registri FP del morto (skip di un restore
 * dovuto). Percorso freddo: la scansione di MAX_CPUS non costa. */
void scheduler_fpu_owner_forget(thread_t *t)
{
    for (uint32_t c = 0; c < MAX_CPUS; c++)
    {
        if (s_cpu[c].fpu_owner == t)
        {
            s_cpu[c].fpu_owner = NULL;
        }
    }
}

/* === API ================================================================= */

void scheduler_init(void)
{
    for (uint32_t c = 0; c < MAX_CPUS; c++)
    {
        spinlock_init(&s_cpu[c].lock);
        for (uint32_t p = 0; p < SCHED_NUM_PRIORITIES; p++)
        {
            list_init(&s_cpu[c].run_queue[p]);
        }
        s_cpu[c].ready_mask   = 0;
        s_cpu[c].online       = false;
        s_cpu[c].running_prio = SCHED_NUM_PRIORITIES;   /* "gira l'idle" */
        s_cpu[c].nr_homed     = 0;
        s_cpu[c].nr_ready     = 0;
        s_cpu[c].migrate_req  = MIGRATE_NONE;
        s_cpu[c].last_pull_ns = 0;
        s_cpu[c].last_dispatch_ns = 0;
        s_cpu[c].idle_time_ns     = 0;

        spinlock_init(&s_cpu[c].tw_lock);
        list_init(&s_cpu[c].reap_list);
        list_init(&s_cpu[c].teardown_q);
    }
    s_active = false;
    kprintf("[SCHD] Scheduler per-CPU pronto (quanti %u/%u/%u/%u ms).\n",
            (uint32_t)ns_to_ms(s_quantum_ns[0]),
            (uint32_t)ns_to_ms(s_quantum_ns[1]),
            (uint32_t)ns_to_ms(s_quantum_ns[2]),
            (uint32_t)ns_to_ms(s_quantum_ns[3]));
}

/* Adotta `t` come idle della CPU chiamante. */
void scheduler_adopt_idle(thread_t *t)
{
    sched_cpu_t *sc = sched_here();
    t->home_cpu = (uint8_t)this_cpu()->cpu_index;
    sc->idle    = t;
}

void scheduler_start(void)
{
    sched_cpu_t *sc = sched_here();
    if (sc->idle == NULL)
    {
        kpanic("scheduler_start: nessun thread idle adottato");
    }
    sc->online = true;
    sc->last_dispatch_ns = clock_now_ns();  /* ancoraggio contabilita'  */
    s_active   = true;
#ifdef MAINDOB_SMP
    /* La IPI di reschedule diventa il punto di switch remoto. La
     * sorgente di slicing (evento LAPIC o tick PIT) e' del nucleo
     * eventi: time_event_try_enable, dopo lapic_init. */
    lapic_register_resched_callback(scheduler_preempt_if_needed);
#endif
    kprintf("[SCHD] Scheduler avviato (BSP).\n");
}

/* Ingresso di una AP: adotta il proprio idle (gia' current) e diventa
 * online. Da quel momento riceve thread via wake cross-core; il
 * ritorno e' il suo idle loop. Chiamare con IF gia' attivi. */
void scheduler_enter_ap(thread_t *idle)
{
    scheduler_adopt_idle(idle);
    sched_here()->online = true;
    sched_here()->last_dispatch_ns = clock_now_ns();

    /* In modo eventi la fine quanto di questa CPU e' armata al primo
     * dispatch (switch_to -> time_event_refresh): qui non c'e' nulla
     * da avviare — la AP dorme finche' un evento non la riguarda. */
    time_event_refresh();

    for (;;)
    {
        /* Stesso blocco idle del BSP: pubblica cpu_idle sotto barriera,
         * rilegge il lavoro, dorme fuso. Se c'e' lavoro, un giro di
         * scheduling. Prima l'AP era un hlt nudo che non guardava
         * nemmeno la runqueue: con l'IPI in gara poteva dormire col
         * lavoro accodato. Ora il flag rende la sua sveglia affidabile. */
        if (scheduler_idle_block())
        {
            scheduler_yield();
        }
    }
}

bool scheduler_is_running(void)
{
    return s_active;
}

bool scheduler_cpu_online(uint32_t cpu)
{
    return cpu < MAX_CPUS && s_cpu[cpu].online;
}

/* Enqueue sulla home del thread + consegna dell'evento. Verbo comune di
 * add/unblock. Chiamare con t READY e fuori dalle code.
 *
 * Questo e' un blocco puramente ESECUTIVO: accoda e SEGNALA, non decide
 * policy. 'need_resched' significa "c'e' nuovo lavoro, rivaluta", NON
 * "prelaziona per forza": chi decide se davvero switchare e' il giudice
 * (scheduler_preempt_if_needed), che legge il PROPRIO current sotto il
 * PROPRIO lock. Cosi' qui non serve — e non si fa — nessuna lettura in
 * gara del current di un altro core (la vecchia euristica 'outranks',
 * radice dei wake persi).
 *
 * Consegna dell'evento:
 *  - bersaglio = questa CPU: niente da fare. Stiamo girando in un
 *    contesto kernel che uscira' a breve (epilogo IRQ/syscall) o verra'
 *    interrotto dalla scadenza del quanto: preempt_if_needed consumera'
 *    need_resched li'.
 *  - bersaglio remoto IDLE (SMP): IPI di sveglia OBBLIGATORIA — una CPU
 *    in hlt non si rivaluta da se'. L'idle-ness e' letta dal flag
 *    affidabile cpu_idle, non da current in gara.
 *  - bersaglio remoto ATTIVO ma OUTRANKED (il risvegliato batte
 *    STRETTAMENTE running_prio): IPI. In modo eventi una CPU occupata
 *    non riceve interrupt fino alla scadenza del proprio quanto: senza
 *    IPI un wake ad alta priorita' aspetterebbe fino a 20 ms — la
 *    radice della latenza IPC cross-core. running_prio e' la priorita'
 *    DECISA sotto il lock del bersaglio (vedi sched_cpu_t): letta qui
 *    dopo la nostra sezione di enqueue sullo stesso lock, o e' fresca,
 *    o il decisore ha gia' visto il nostro thread in coda. Una IPI
 *    spuria e' innocua: il giudice rivaluta sotto il proprio lock.
 *  - bersaglio remoto ATTIVO a pari o miglior grado: nessuna IPI. Per
 *    policy il wake non prelaziona i pari: il round-robin di fine
 *    quanto lo raccoglie entro <= 1 quanto, mai starvation. */
static void enqueue_home_and_kick(thread_t *t)
{
    uint32_t fl = irq_save();
    sched_cpu_t *sc = lock_home_queue(t);   /* home VERA, ferma finche'
                                             * teniamo il lock          */
    uint32_t home = t->home_cpu;
    (void)home;                             /* usata solo in build SMP  */
    enqueue_ready(sc, t);
    sc->need_resched = true;
    spinlock_release(&sc->lock);
    irq_restore(fl);

#ifdef MAINDOB_SMP
    if (home != this_cpu()->cpu_index && sc->online)
    {
        /* Barriera store-load SIMMETRICA col blocco idle: pubblichiamo
         * enqueue + need_resched PRIMA di leggere cpu_idle; il blocco
         * idle fa lo speculare (alza cpu_idle, barriera, rilegge il
         * lavoro). Su TSO x86 lo store-buffer riordina store-poi-load:
         * senza la barriera su ENTRAMBI i lati potremmo vedere cpu_idle
         * vecchio (false) mentre l'idle vede need_resched vecchio (assente)
         * — wake perso. Con la barriera doppia, almeno uno dei due vede
         * l'altro. memory_barrier() = lock addl, valido su tutti i P6
         * (Armada E500 senza SSE2 compresa).
         * Se il thread migra di nuovo DOPO il nostro enqueue, questa IPI
         * al piu' e' spuria: la consegna alla nuova home la fa il
         * donatore col proprio kick. */
        memory_barrier();
        if (sc->cpu_idle || t->priority < sc->running_prio)
        {
            lapic_send_resched_ipi(g_cpus[home].apic_id);
        }
    }
#endif
}

void scheduler_add(thread_t *t)
{
    uint32_t fl = irq_save();
    if ((t->state == THREAD_READY || t->state == THREAD_RUNNING) &&
        !list_node_is_linked(&t->sched_node))
    {
        t->state = THREAD_READY;
        enqueue_home_and_kick(t);
    }
    irq_restore(fl);
}

void scheduler_remove(thread_t *t)
{
    uint32_t fl = irq_save();
    sched_cpu_t *sc = lock_home_queue(t);
    if (list_node_is_linked(&t->sched_node))
    {
        uint32_t prio = t->priority;
        list_remove(&t->sched_node);
        refresh_mask(sc, prio);
        sc->nr_ready--;                 /* era in coda: esce dal conteggio */
    }
    spinlock_release(&sc->lock);
    irq_restore(fl);
}

/* === Teardown/reclamation instradati sulla home-core ===================== */

/* Parcheggia un cadavere sulla reap_list della sua home-core. Chiamato da
 * thread_exit/thread_destroy: il thread viene mietuto SOLO da quel core, nel
 * suo idle, cioe' dopo che ha fatto lo switch via -> niente reaper-UAF. In
 * pratica t->home_cpu e' sempre il core corrente (il teardown gira sulla
 * home), ma la funzione gestisce qualunque core per robustezza. */
void scheduler_queue_reap(thread_t *t)
{
    sched_cpu_t *sc = sched_of(t->home_cpu);
    uint32_t fl = spinlock_acquire_irqsave(&sc->tw_lock);
    list_push_back(&sc->reap_list, &t->sched_node);
    spinlock_release_irqrestore(&sc->tw_lock, fl);
}

/* Miete i cadaveri parcheggiati su QUESTO core. Chiamata dall'idle (contesto
 * thread, IRQ on tra un elemento e l'altro): il reap pesante prende i suoi
 * lock fuori dalla cli breve del pop. Se il thread era l'ultimo di un
 * processo in teardown, thread_reap_one ha gia' RIVENDICATO il finalize sotto
 * il lifecycle-lock (ritorna il proc al solo vincitore): finalizziamo qui,
 * sicuri, perche' e' la home-core del PCB e nessun core e' sul suo CR3. */
void scheduler_reap_local(void)
{
    for (;;)
    {
        sched_cpu_t *sc = sched_here();
        uint32_t fl = spinlock_acquire_irqsave(&sc->tw_lock);
        list_node_t *node = list_empty(&sc->reap_list)
                          ? NULL : list_pop_front(&sc->reap_list);
        spinlock_release_irqrestore(&sc->tw_lock, fl);
        if (node == NULL)
        {
            return;
        }

        thread_t *t = list_entry(node, thread_t, sched_node);
        list_node_init(&t->sched_node);
        scheduler_fpu_owner_forget(t);  /* PRIMA che la struct torni
                                         * all'allocatore: mai un falso
                                         * match FPU su indirizzo riciclato */
        struct process *dead = thread_reap_one(t);      /* pesante, in thread.c */
        if (dead != NULL)
        {
            process_finalize(dead);
        }
    }
}

/* Instrada il teardown di un processo sulla sua home-core. Ritorna true se il
 * PCB e' stato messo in coda su un core REMOTO (il chiamante deve tornare:
 * quel core lo distruggera' dal suo idle). Ritorna false se il chiamante deve
 * eseguire il teardown ORA, qui: processo gia' a casa, home offline, o
 * scheduler non ancora attivo (boot/UP). La home-core e' l'unico core che
 * carica il CR3 del processo (tutti i suoi thread girano li'), quindi
 * eseguire kill+reap+finalize li' garantisce che nessun core sia
 * sull'address space quando viene liberato. */
bool scheduler_route_process_teardown(struct process *proc)
{
    if (!s_active)
    {
        return false;                       /* boot/UP: esegui qui       */
    }

    /* La home va letta sotto il LIFECYCLE-LOCK: e' lo stesso lock sotto
     * cui il donatore della migrazione flippa proc->home_cpu. Senza,
     * la finestra era: leggo home vecchia -> migrazione completa ->
     * accodo il PCB sulla teardown_q del core SBAGLIATO -> quel core
     * esegue kill/reap di thread che ORA girano altrove (l'invariante
     * di thread_destroy salta) e finalizza un CR3 potenzialmente vivo
     * sulla nuova home. Sotto il lock: o vediamo la home nuova, o il
     * donatore vede destroy_claimed/queued_for_teardown (li controlla
     * sotto lo stesso lock) e SI RITIRA. Nesting lifecycle -> tw_lock:
     * nuovo ma senza inverso (nessuno prende il lifecycle sotto un
     * tw_lock; reap_local rilascia tw prima di thread_reap_one). */
    uint32_t lfl = thread_lifecycle_lock();

    uint32_t home = proc->home_cpu;
    uint32_t self = this_cpu()->cpu_index;
    if (home >= (uint32_t)MAX_CPUS || !s_cpu[home].online || home == self)
    {
        thread_lifecycle_unlock(lfl);
        return false;                       /* eseguibile qui / a casa   */
    }

    bool kick = false;
    sched_cpu_t *sc = sched_of(home);
    uint32_t fl = spinlock_acquire_irqsave(&sc->tw_lock);
    if (!proc->queued_for_teardown)
    {
        proc->queued_for_teardown = true;   /* accoda esattamente una volta */
        list_push_back(&sc->teardown_q, &proc->teardown_link);
        kick = true;
    }
    spinlock_release_irqrestore(&sc->tw_lock, fl);
    thread_lifecycle_unlock(lfl);

#ifdef MAINDOB_SMP
    if (kick && s_cpu[home].online)
    {
        lapic_send_resched_ipi(g_cpus[home].apic_id);   /* sveglia la home  */
    }
#else
    (void)kick;
#endif
    return true;
}

/* Variante SEMPRE-differita del router: aggancia il PCB alla teardown_q
 * della sua home-core ANCHE quando home==self, cosi' il teardown gira in
 * idle e MAI sincrono. E' il canale non-perdibile per "termina questo
 * processo" invocato da contesti dove non si puo' distruggere adesso
 * (fault handler, callback timer, percorso OOM sotto i lock mm): accoda
 * soltanto (spinlock irqsave), la distruzione avviene dopo in idle.
 * Rimpiazza la vecchia deviazione sulla workqueue perdibile. Idempotente:
 * queued_for_teardown accoda una volta sola, destroy_claimed (gia' preso
 * dal chiamante) impedisce doppi teardown. */
void scheduler_defer_process_teardown(struct process *proc)
{
    uint32_t lfl = thread_lifecycle_lock();

    uint32_t home = proc->home_cpu;
    if (!s_active || home >= (uint32_t)MAX_CPUS || !s_cpu[home].online)
    {
        home = this_cpu()->cpu_index;   /* boot/UP o home invalida: locale */
    }

    bool kick = false;
    sched_cpu_t *sc = sched_of(home);
    uint32_t fl = spinlock_acquire_irqsave(&sc->tw_lock);
    if (!proc->queued_for_teardown)
    {
        proc->queued_for_teardown = true;   /* accoda esattamente una volta */
        list_push_back(&sc->teardown_q, &proc->teardown_link);
        kick = true;
    }
    spinlock_release_irqrestore(&sc->tw_lock, fl);
    thread_lifecycle_unlock(lfl);

#ifdef MAINDOB_SMP
    if (kick && home != this_cpu()->cpu_index && s_cpu[home].online)
    {
        lapic_send_resched_ipi(g_cpus[home].apic_id);   /* sveglia la home  */
    }
#else
    (void)kick;
#endif
}

/* Svuota la coda dei teardown instradati su QUESTO core. Chiamata dall'idle.
 * Ogni process_destroy rieseguito qui trova home==self -> non re-instrada, e
 * il claim atomico destroy_claimed impedisce doppie esecuzioni. */
void scheduler_run_local_teardowns(void)
{
    for (;;)
    {
        sched_cpu_t *sc = sched_here();
        uint32_t fl = spinlock_acquire_irqsave(&sc->tw_lock);
        list_node_t *node = list_empty(&sc->teardown_q)
                          ? NULL : list_pop_front(&sc->teardown_q);
        struct process *proc = NULL;
        if (node != NULL)
        {
            proc = list_entry(node, struct process, teardown_link);
            proc->queued_for_teardown = false;
            list_node_init(&proc->teardown_link);
        }
        spinlock_release_irqrestore(&sc->tw_lock, fl);
        if (proc == NULL)
        {
            return;
        }
        process_destroy_local(proc);        /* gia' rivendicato dal router */
    }
}

void scheduler_yield(void)
{
    sched_cpu_t *sc = sched_here();

    /* Cedere e' obbligatorio se lo stato non e' piu' RUNNING/READY
     * (blocco in corso); volontario altrimenti. */
    bool must_leave = (current_thread->state != THREAD_RUNNING &&
                       current_thread->state != THREAD_READY);

    if (!must_leave)
    {
        /* Volontario: cede solo a pari o superiore priorita'. */
        uint32_t fl = spinlock_acquire_irqsave(&sc->lock);
        bool nothing_better =
            highest_ready_priority(sc) > current_thread->priority &&
            current_thread->state == THREAD_RUNNING;
        spinlock_release_irqrestore(&sc->lock, fl);
        if (nothing_better)
        {
            return;
        }
    }

    reschedule(sc, must_leave);
}

void scheduler_block_current(thread_state_t reason)
{
    uint32_t fl = irq_save();
    current_thread->state = reason;
    reschedule(sched_here(), true);
    irq_restore(fl);
}

void scheduler_yield_to(thread_t *next, thread_state_t my_state)
{
    if (next == NULL)
    {
        return;
    }

    uint32_t fl = irq_save();
    sched_cpu_t *sc = sched_here();

    /* Idoneita': deve essere un thread di QUESTA CPU, bloccato e fuori
     * dalle code (appena estratto da una wait queue). Ogni altro caso
     * degrada al percorso normale: mai un'invariante rotta per una
     * scorciatoia. */
    if (next->home_cpu != this_cpu()->cpu_index ||
        next->state != THREAD_BLOCKED ||
        list_node_is_linked(&next->sched_node))
    {
        irq_restore(fl);
        scheduler_unblock(next);
        return;
    }

    timer_cancel(&next->sleep_timer);

    thread_t *self = current_thread;
    self->state = my_state;
    self->cpu_hungry = false;           /* chi passa dal direct-switch
                                         * sta CONVERSANDO per definizione:
                                         * cede la CPU al partner IPC di
                                         * sua volonta' — mai candidarlo
                                         * alla migrazione come compute  */

    /* Sezione decisore UNICA per entrambi gli esiti del chiamante
     * (READY o BLOCKED): l'eventuale requeue di self e la
     * pubblicazione di running_prio stanno sotto lo STESSO sc->lock,
     * come in reschedule — e' cio' che rende affidabile la lettura
     * del waker remoto (protocollo in sched_cpu_t). `next` viene da
     * una wait queue: mai idle, la sua priority e' quella pubblicata. */
    spinlock_acquire(&sc->lock);
    if (my_state == THREAD_READY && !self->is_idle)
    {
        enqueue_ready(sc, self);
    }
    sc->running_prio = next->priority;
    spinlock_release(&sc->lock);

    switch_to(sc, next);
    irq_restore(fl);
}

/* Round-robin cieco sulle CPU online. Non e' piu' il default del
 * placement (vedi scheduler_pick_cpu_local): resta come distributore
 * neutro per chi vuole spargere senza criterio di localita'. */
uint32_t scheduler_pick_cpu(void)
{
    static volatile uint32_t s_rr;

    for (uint32_t probe = 0; probe < MAX_CPUS; probe++)
    {
        uint32_t cand = atomic_add_return(&s_rr, 1) % MAX_CPUS;
        if (s_cpu[cand].online)
        {
            return cand;
        }
    }
    return 0;                           /* solo la BSP e' online */
}

/* === Placement con localita' + bilancio advisory ========================= */

/* Bilancio dei thread homed per CPU. ADVISORY: inc/dec atomici, letture
 * senza lock; una gara costa al massimo un piazzamento subottimale.
 * Aggancio: thread_publish (nascita, idle esclusi) / reap_detach (morte)
 * — stessi punti, stesso lock tabella, simmetria per costruzione. */

void scheduler_account_thread_homed(uint32_t cpu)
{
    if (cpu < MAX_CPUS)
    {
        atomic_inc(&s_cpu[cpu].nr_homed);
    }
}

void scheduler_account_thread_unhomed(uint32_t cpu)
{
    /* La guardia sullo zero e' una cintura contro bug di contabilita':
     * un underflow renderebbe la CPU "infinitamente carica" per sempre. */
    if (cpu < MAX_CPUS && atomic_read(&s_cpu[cpu].nr_homed) != 0)
    {
        atomic_dec(&s_cpu[cpu].nr_homed);
    }
}

/* Esecutivo: carico di placement di una CPU, senza lock (istantanea
 * advisory). nr_homed da solo mentiva: una CPU che ospita dieci server
 * perennemente bloccati sembrava carica quanto una con dieci spinner
 * CPU-bound. I runnable (in coda ORA, si contendono davvero la CPU)
 * pesano PLACEMENT_READY_WEIGHT volte i homed dormienti; i homed
 * restano nel conto come pressione di fondo (prima o poi si svegliano,
 * e le loro sveglie sono lavoro per questa CPU). */
#define PLACEMENT_READY_WEIGHT 3u

static uint32_t placement_load_of(uint32_t cpu)
{
    return atomic_read(&s_cpu[cpu].nr_homed)
         + PLACEMENT_READY_WEIGHT * atomic_read(&s_cpu[cpu].nr_ready);
}

/* Esecutivo: la CPU online meno carica (metrica pesata, senza lock). */
static uint32_t least_loaded_online_cpu(void)
{
    uint32_t best      = 0;
    uint32_t best_load = UINT32_MAX;
    for (uint32_t c = 0; c < MAX_CPUS; c++)
    {
        if (!s_cpu[c].online)
        {
            continue;
        }
        uint32_t load = placement_load_of(c);
        if (load < best_load)
        {
            best      = c;
            best_load = load;
        }
    }
    return best;
}

/* Tolleranza di squilibrio (in unita' della metrica pesata) prima di
 * rinunciare alla localita': con la CPU del creatore entro SLACK dalla
 * meno carica, si resta QUI.
 * Perche' la localita' vale oro: il fast-path IPC (direct-switch di
 * scheduler_yield_to) scatta solo same-CPU, e ogni wake same-CPU evita
 * IPI e latenza cross-core. Il round-robin cieco del passato spargeva
 * client e server su core diversi, trasformando OGNI messaggio in un
 * wake remoto: era la seconda radice del multicore-piu'-lento. */
#define SCHED_PLACEMENT_SLACK 2u

/* Placement di default per un nuovo processo: la CPU del CREATORE, a
 * meno che non sia sproporzionatamente carica rispetto alla migliore
 * (allora la migliore). Prima che lo scheduler sia attivo: la CPU
 * corrente (boot: la BSP). */
uint32_t scheduler_pick_cpu_local(void)
{
    uint32_t self = this_cpu()->cpu_index;
    if (!s_active || !s_cpu[self].online)
    {
        return s_active ? least_loaded_online_cpu() : self;
    }

    uint32_t best = least_loaded_online_cpu();
    if (placement_load_of(self)
        <= placement_load_of(best) + SCHED_PLACEMENT_SLACK)
    {
        return self;                    /* localita': si resta a casa    */
    }
    return best;                        /* troppo squilibrio: si sparge  */
}

/* === Statistiche per il task manager ====================================
 * Letture senza lock di contatori single-writer: gli u64 vengono
 * riletti fino a stabilita' (i386 puo' strapparli). Advisory per
 * definizione: e' una fotografia per un display a 300 ms. */

static uint64_t read_u64_stable(const volatile uint64_t *p)
{
    for (int i = 0; i < 3; i++)
    {
        uint64_t a = *p;
        uint64_t b = *p;
        if (a == b)
        {
            return a;
        }
    }
    return *p;                          /* terzo strappo di fila: amen   */
}

/* Ozio accumulato della CPU `cpu`, INCLUSA la frazione idle in corso
 * (se sta oziando ORA, il tempo da last_dispatch e' gia' ozio: senza,
 * una CPU ferma da secondi risulterebbe "occupata" fino al prossimo
 * switch). Ritorna false se la CPU non e' online. */
bool scheduler_cpu_idle_ns(uint32_t cpu, uint64_t *out_idle_ns)
{
    if (cpu >= (uint32_t)MAX_CPUS || !s_cpu[cpu].online)
    {
        return false;
    }
    uint64_t idle = read_u64_stable(&s_cpu[cpu].idle_time_ns);
    if (s_cpu[cpu].cpu_idle)
    {
        uint64_t since = read_u64_stable(
            (const volatile uint64_t *)&s_cpu[cpu].last_dispatch_ns);
        uint64_t now = clock_now_ns();
        if (now > since)
        {
            idle += now - since;
        }
    }
    *out_idle_ns = idle;
    return true;
}

/* Tempo CPU di un thread, lettura stabile (per lo snapshot). */
uint64_t scheduler_thread_cpu_ns(const struct thread *t)
{
    return read_u64_stable(&t->cpu_time_ns);
}

#ifdef MAINDOB_SMP
/* === Migrazione cooperativa (D) ==========================================
 *
 * ARCHITETTURA (i quattro pilastri, dall'analisi delle race):
 *  1. SOLO LA HOME ri-homa i propri processi. Il core affamato CHIEDE
 *     (migrate_req + IPI), mai ruba: "T e' homed su di me" cambia solo
 *     per azione mia -> il direct-switch IPC (check home==self, IF=0)
 *     resta corretto SENZA lock aggiunti sul percorso caldo.
 *  2. Grana di PROCESSO INTERO con nessun thread RUNNING (garantito:
 *     solo la home esegue i suoi thread e sta eseguendo il donatore,
 *     che appartiene a un ALTRO processo). La vecchia home non
 *     ricarichera' mai piu' quel CR3; la nuova ridiventa l'unica.
 *  3. I lettori di home_cpu sono lock-recheck-retry (lock_home_queue).
 *  4. Esclusioni sotto il gate giusto (lifecycle-lock): pinned (driver
 *     IRQ, driver video/boomerang), teardown in corso o rivendicato,
 *     cadaveri in attesa di reap (la reap_list e' della VECCHIA home),
 *     cooldown anti-thrash, processo del corrente.
 *
 * Serializzazione: ogni donazione gira sotto il lifecycle-lock (due
 * donatori mai concorrenti, e linearizzata con spawn/route/reap che
 * toccano home o liste) + ENTRAMBI gli sc->lock per lo spostamento. */

/* Verbo (chiamante tiene lifecycle-lock + sc->lock del donatore): il
 * processo `p` puo' essere donato ORA? Tutte le esclusioni del
 * pilastro 4 in un solo posto. */
static bool donation_candidate_ok(struct process *p, uint64_t now)
{
    if (p == NULL || p->pid == 0 || p->pinned)
    {
        return false;
    }
    if (current_thread != NULL && p == current_thread->owner)
    {
        return false;                   /* ha un thread RUNNING: noi     */
    }
    if (atomic_read(&p->destroy_claimed) != 0 ||
        p->queued_for_teardown || p->awaiting_teardown)
    {
        return false;                   /* teardown in volo: mani giu'   */
    }
    if (p->last_migration_ns != 0 &&
        now - p->last_migration_ns < MIGRATION_COOLDOWN_NS)
    {
        return false;                   /* cooldown anti-thrash          */
    }

    /* Nessun cadavere pendente: il reap e' guidato dalla membership
     * nella reap_list della VECCHIA home, non da home_cpu — un DEAD
     * migrato verrebbe mietuto (e magari il processo finalizzato) dal
     * core sbagliato. RUNNING come cintura: escluso gia' sopra. */
    list_node_t *n;
    list_for_each(n, &p->threads)
    {
        thread_t *t = list_entry(n, thread_t, process_node);
        if (t->state == THREAD_DEAD || t->state == THREAD_RUNNING)
        {
            return false;
        }
    }
    return true;
}

/* Verbo (chiamante tiene lifecycle-lock + ENTRAMBI gli sc->lock):
 * trasferisce tutti i thread di `p` da `from` a `to` e flippa la home
 * del processo. I READY escono dalle code vecchie ed entrano nelle
 * nuove nella stessa sezione; i BLOCKED/SLEEPING cambiano solo
 * home_cpu (i loro waker convergono via lock_home_queue). Ritorna la
 * miglior priorita' accodata (per il kick), SCHED_NUM_PRIORITIES se
 * nessun READY mosso. */
static uint32_t donation_move_threads(struct process *p, sched_cpu_t *from,
                                      sched_cpu_t *to, uint32_t new_cpu)
{
    uint32_t best = SCHED_NUM_PRIORITIES;

    list_node_t *n;
    list_for_each(n, &p->threads)
    {
        thread_t *t = list_entry(n, thread_t, process_node);
        bool queued = list_node_is_linked(&t->sched_node);
        if (queued)
        {
            list_remove(&t->sched_node);
            refresh_mask(from, t->priority);
            from->nr_ready--;
        }

        scheduler_account_thread_unhomed(t->home_cpu);
        t->home_cpu = (uint8_t)new_cpu;
        scheduler_account_thread_homed(new_cpu);

        if (queued)
        {
            enqueue_ready(to, t);
            if (t->priority < best)
            {
                best = t->priority;
            }
        }
    }
    p->home_cpu = (uint8_t)new_cpu;
    return best;
}

/* IL DONATORE. Gira sulla home (questa CPU), dal giudice, con IRQ gia'
 * disattivi dal chiamante (nessuna rientranza sullo stesso core).
 * Sceglie dal PROPRIO stato un intero processo caldo-in-coda e lo
 * trasferisce al richiedente; a vuoto o a segno, consuma la richiesta
 * (il richiedente ritentera' dopo il suo rate-limit). */
static void scheduler_donate(sched_cpu_t *sc)
{
    uint32_t req = sc->migrate_req;
    if (req >= (uint32_t)MAX_CPUS || req == this_cpu()->cpu_index ||
        !s_cpu[req].online)
    {
        sc->migrate_req = MIGRATE_NONE;
        return;
    }
    sched_cpu_t *to  = sched_of(req);
    uint64_t     now = clock_now_ns();

    uint32_t lfl = thread_lifecycle_lock();

    /* Ordine fra i due sc->lock: per indirizzo, uniforme. (I donatori
     * sono comunque gia' serializzati fra loro dal lifecycle-lock: qui
     * e' igiene, non necessita'.) */
    sched_cpu_t *first  = (sc < to) ? sc : to;
    sched_cpu_t *second = (sc < to) ? to : sc;
    spinlock_acquire(&first->lock);
    spinlock_acquire(&second->lock);

    /* Rivalutazione sotto il PROPRIO lock: tra la richiesta e questo
     * momento il sovraccarico puo' essere evaporato (il runnable in
     * coda ha gia' girato). Una richiesta stantia non deve strappare
     * l'ultimo thread in coda: stessa soglia del richiedente.
     *
     * Candidato: il primo READY *COMPUTE* (cpu_hungry) dalla priorita'
     * piu' alta il cui processo passa le esclusioni. Il filtro e' la
     * lezione della b132: durante le cascate IPC anche i partner della
     * catena sono momentaneamente in coda DA SVEGLI — l'assunzione
     * "i partner sono bloccati, mai scelti" cade proprio negli unici
     * istanti in cui la donazione scatta, e migrarli spezza la
     * localita' del direct-switch: ogni hop diventa enqueue+IPI+switch
     * pieno (il ping-pong di carico e lo scatto visti sul CQ62). Un
     * thread che si blocca entro il proprio quanto non e' MAI
     * cpu_hungry: le conversazioni restano a casa. Nessun compute in
     * coda -> NESSUNA migrazione: spargere una catena chiacchierona
     * non compra alcun parallelismo, costa solo localita'. */
    struct process *victim = NULL;
    for (uint32_t prio = 0;
         sc->nr_ready >= PULL_MIN_READY &&
         prio < SCHED_NUM_PRIORITIES && victim == NULL;
         prio++)
    {
        list_node_t *n;
        list_for_each(n, &sc->run_queue[prio])
        {
            thread_t *t = list_entry(n, thread_t, sched_node);
            if (t->state == THREAD_READY && t->cpu_hungry &&
                donation_candidate_ok(t->owner, now))
            {
                victim = t->owner;
                break;
            }
        }
    }

    uint32_t kicked_prio = SCHED_NUM_PRIORITIES;
    pid_t    moved_pid   = 0;
    char     moved_name[PROCESS_NAME_MAX];
    if (victim != NULL)
    {
        kicked_prio = donation_move_threads(victim, sc, to, req);
        victim->last_migration_ns = now;
        to->need_resched = true;

        /* Nome/PID copiati ORA: fuori dal lifecycle-lock il PCB puo'
         * essere finalizzato dalla NUOVA home in qualsiasi momento. */
        moved_pid = victim->pid;
        memcpy(moved_name, victim->name, sizeof(moved_name));
    }

    spinlock_release(&second->lock);
    spinlock_release(&first->lock);
    sc->migrate_req = MIGRATE_NONE;     /* richiesta consumata           */
    thread_lifecycle_unlock(lfl);

    if (victim != NULL)
    {
        kprintf("[SCHD] migrato PID %d '%s': core %u -> %u (pull).\n",
                moved_pid, moved_name, this_cpu()->cpu_index, req);

        /* Consegna: il richiedente e' quasi certamente in hlt. Stesso
         * protocollo a barriera dell'enqueue remoto. */
        memory_barrier();
        if (to->cpu_idle || kicked_prio < to->running_prio)
        {
            lapic_send_resched_ipi(g_cpus[req].apic_id);
        }
    }
}

/* IL RICHIEDENTE. Dal blocco idle, quando questa CPU non ha nulla:
 * sceglie il core online piu' carico che abbia ALMENO un runnable in
 * coda (nr_ready >= 1: senza, non c'e' nulla di donabile), pubblica la
 * richiesta (CAS: una sola pendente per vittima) e la pungola con una
 * IPI. Rate-limited per non tempestare. */
static void idle_try_pull(sched_cpu_t *me)
{
    if (!s_active)
    {
        return;
    }
    uint64_t now = clock_now_ns();
    if (now - me->last_pull_ns < PULL_MIN_INTERVAL_NS)
    {
        return;
    }

    uint32_t self        = this_cpu()->cpu_index;
    uint32_t victim      = MIGRATE_NONE;
    uint32_t victim_load = 0;
    for (uint32_t c = 0; c < MAX_CPUS; c++)
    {
        if (c == self || !s_cpu[c].online)
        {
            continue;
        }
        /* Due firme di sovraccarico, non una:
         *   - PROFONDO: coda >= PULL_MIN_READY (la firma storica);
         *   - INCEPPATO: almeno UN runnable in coda mentre un thread
         *     reale occupa il core — MA solo se la contesa PERSISTE da
         *     CONTENTION_PERSIST_NS (lezione b132: la coda a 1 e' il
         *     rumore delle cascate IPC, decine di volte al secondo;
         *     inseguirla migra conversazioni e spezza la localita' del
         *     direct-switch). contended_since_ns e running_prio sono
         *     advisory fuori lock (u64 strappabile su i386): al peggio
         *     una richiesta spuria — e la donazione, che ora migra
         *     SOLO compute (cpu_hungry), resta l'unico giudice. */
        uint32_t rd     = atomic_read(&s_cpu[c].nr_ready);
        uint64_t since  = s_cpu[c].contended_since_ns;
        bool     deep   = (rd >= PULL_MIN_READY);
        bool     jammed = (rd >= 1u &&
                           s_cpu[c].running_prio
                               < (uint32_t)SCHED_NUM_PRIORITIES &&
                           since != 0 &&
                           now - since >= CONTENTION_PERSIST_NS);
        if (!deep && !jammed)
        {
            continue;                   /* non sovraccarico: mani giu'   */
        }
        uint32_t load = placement_load_of(c);
        if (load > victim_load)
        {
            victim_load = load;
            victim      = c;
        }
    }
    if (victim == MIGRATE_NONE)
    {
        return;                         /* nessuno e' davvero in coda    */
    }

    me->last_pull_ns = now;
    if (atomic_cas(&s_cpu[victim].migrate_req, MIGRATE_NONE, self))
    {
        lapic_send_resched_ipi(g_cpus[victim].apic_id);
    }
}
#endif /* MAINDOB_SMP */

void scheduler_unblock(thread_t *t)
{
    if (t == NULL)
    {
        return;
    }

    timer_cancel(&t->sleep_timer);

    uint32_t fl = irq_save();
    if (t->state != THREAD_BLOCKED && t->state != THREAD_SLEEPING)
    {
        irq_restore(fl);
        return;                         /* wake ridondante                */
    }
    t->state = THREAD_READY;
    enqueue_home_and_kick(t);
    irq_restore(fl);
}

/* Callback del sleep_timer per-thread (contesto IRQ, dentro il drain). */
void scheduler_sleep_timer_fire(void *arg);
void scheduler_sleep_timer_fire(void *arg)
{
    thread_t *t = (thread_t *)arg;
    if (t == NULL)
    {
        return;
    }
    if (t->blocked_on != NULL)
    {
        wait_queue_remove_thread(t->blocked_on, t);
        t->wait_result = WAIT_TIMEOUT;
    }
    scheduler_unblock(t);
}

void scheduler_sleep_ns(uint64_t ns)
{
    if (ns == 0)
    {
        return;
    }

    uint32_t fl = irq_save();

    /* Armo dentro la sezione IF=0: un fire con stato ancora RUNNING
     * sarebbe un wake perso e un sonno eterno. */
    if (!timer_arm_in(&current_thread->sleep_timer, ns))
    {
        irq_restore(fl);
        kprintf("[SCHD] timer heap pieno: sleep degradata a yield\n");
        scheduler_yield();
        return;
    }

    current_thread->state = THREAD_SLEEPING;
    reschedule(sched_here(), true);
    irq_restore(fl);

    timer_cancel(&current_thread->sleep_timer);
}

void scheduler_set_priority(thread_t *t, uint32_t new_prio)
{
    if (t == NULL || new_prio >= SCHED_NUM_PRIORITIES)
    {
        return;
    }

    uint32_t fl = irq_save();
    sched_cpu_t *sc = lock_home_queue(t);
    uint32_t home = t->home_cpu;        /* ferma finche' teniamo il lock */
    uint32_t old = t->priority;
    if (new_prio == old)
    {
        spinlock_release(&sc->lock);
        irq_restore(fl);
        return;
    }

    t->priority = new_prio;

    bool remote_kick = false;
    if (t->state == THREAD_READY && list_node_is_linked(&t->sched_node))
    {
        list_remove(&t->sched_node);
        refresh_mask(sc, old);
        sc->nr_ready--;                 /* compensa il ++ dell'enqueue:
                                         * spostamento di coda, netto 0  */
        enqueue_ready(sc, t);

        /* BOOST di un READY (percorso PI): senza consegna, il boost
         * verso un core remoto occupato restava lettera morta fino al
         * prossimo evento di QUEL core (fino a un quanto) — la stessa
         * latenza uccisa in b114 per i wake, sopravvissuta qui. Stesso
         * rimedio: invito a rivalutare + IPI se il boostato batte
         * strettamente il running remoto (letto sotto QUESTO lock:
         * running_prio e' pubblicato sotto di esso). Il giudice remoto
         * decide comunque sotto il proprio stato: IPI spuria innocua. */
        if (new_prio < old)
        {
            sc->need_resched = true;
            remote_kick = (new_prio < sc->running_prio);
        }
    }
    else if (t == g_cpus[home].current && t->state == THREAD_RUNNING)
    {
        sc->running_prio   = new_prio;  /* pubblicato: siamo sotto sc->lock */
        sc->slice_deadline = clock_now_ns() + s_quantum_ns[new_prio];
        if (highest_ready_priority(sc) < new_prio)
        {
            sc->need_resched = true;    /* declassato: cedi al piu' alto  */
            remote_kick = true;         /* il suo core deve saperlo ORA   */
        }
    }
    spinlock_release(&sc->lock);
    irq_restore(fl);
    (void)remote_kick;                      /* usata solo in build SMP  */

#ifdef MAINDOB_SMP
    if (remote_kick && home != this_cpu()->cpu_index && sc->online)
    {
        /* Stessa barriera simmetrica del wake remoto (vedi
         * enqueue_home_and_kick): pubblica PRIMA, leggi cpu_idle POI. */
        memory_barrier();
        lapic_send_resched_ipi(g_cpus[home].apic_id);
    }
#endif
}

/* === Integrazione IRQ ==================================================== */

/* Verifica di fine quanto: evento del LAPIC (modo eventi) o tick del
 * PIT (fallback) — la logica e' un confronto di scadenza, identica. */
/* === Blocchi logici della slice check =================================== *
 * Ognuno UNA decisione, nominata; slice_check e' solo il coordinatore:
 * legge il clock UNA volta e delega ai blocchi. */

#ifdef MAINDOB_SMP
/* LOGICA: decidere se offrirsi come donatore (lezione b132). Il pull
 * dell'idle valuta il mondo solo QUANDO entra in idle: se questo core
 * si inceppa DOPO che l'altro si e' gia' addormentato in hlt, nessuno
 * rivaluta piu' niente. Ma offrirsi a ogni micro-contesa insegue il
 * rumore delle cascate IPC: l'offerta parte solo se la contesa PERSISTE
 * da CONTENTION_PERSIST_NS, ed e' diradata da DONOR_OFFER_INTERVAL_NS.
 * Stesso slot, stesso CAS e stessa macchina di donazione della
 * richiesta remota — che migra SOLO compute (cpu_hungry): un'offerta
 * con sola conversazione in coda muore li', senza spezzare nulla.
 * cpu_idle e' il segnale pubblicato sotto barriera dal blocco idle. */
static void donor_offer_consider(sched_cpu_t *sc, uint64_t now)
{
    if (sc->ready_mask == 0)
    {
        sc->contended_since_ns = 0;     /* coda vuota: contesa finita   */
        return;
    }
    if (sc->contended_since_ns == 0)
    {
        sc->contended_since_ns = now;   /* contesa appena iniziata      */
        return;
    }
    if (now - sc->contended_since_ns < CONTENTION_PERSIST_NS ||
        now - sc->last_offer_ns      < DONOR_OFFER_INTERVAL_NS ||
        sc->migrate_req != MIGRATE_NONE)
    {
        return;
    }
    uint32_t self = this_cpu()->cpu_index;
    for (uint32_t c = 0; c < MAX_CPUS; c++)
    {
        if (c != self && s_cpu[c].online && s_cpu[c].cpu_idle)
        {
            sc->last_offer_ns = now;
            atomic_cas(&sc->migrate_req, MIGRATE_NONE, c);
            return;
        }
    }
}
#endif

/* LOGICA: contabilita' della valvola di equita' RT (vedi sched_cpu_t).
 * Il rinnovo del quanto a prio 0 CON lavoro pronto sotto e' esattamente
 * il monopolio da misurare. Maturato il budget, la concessione e'
 * decisa QUI; il pick la esegue e basta. */
static void rt_valve_account(sched_cpu_t *sc)
{
    if (current_thread->priority == 0 && sc->ready_mask != 0)
    {
        sc->rt_burst_ns += s_quantum_ns[0];
        if (sc->rt_burst_ns >= SCHED_RT_BURST_BUDGET_NS && !sc->rt_relief)
        {
            sc->rt_relief    = true;
            sc->need_resched = true;
            rt_relief_note(current_thread);
        }
    }
    else
    {
        sc->rt_burst_ns = 0;            /* niente fame sotto: si azzera */
    }
}

void scheduler_slice_check(void)
{
    sched_cpu_t *sc = sched_here();
    if (!s_active || current_thread == NULL || current_thread->is_idle)
    {
        return;
    }

    uint64_t now = clock_now_ns();      /* UNICA lettura del clock: la
                                         * riusano offerta, confronto
                                         * scadenza e rinnovo del quanto */
#ifdef MAINDOB_SMP
    donor_offer_consider(sc, now);
#endif

    if (now < sc->slice_deadline)
    {
        return;                         /* quanto ancora in corso        */
    }
    if (highest_ready_priority(sc) <= current_thread->priority)
    {
        sc->need_resched = true;        /* pari grado: round-robin       */
        return;
    }

    /* Quanto scaduto ma NESSUNO di pari o miglior grado da far girare:
     * il quanto si RINNOVA. Senza rinnovo la scadenza resta nel PASSATO
     * per sempre e time_event_refresh riarma "subito" a ogni giro
     * (delta 0 -> init_count 1): tempesta perpetua di auto-interrupt su
     * ogni CPU con un thread CPU-bound solitario — la CPU vive nell'ISR
     * e tutto il resto (IPC, input, GUI) muore di fame dietro di lei.
     * Il rinnovo e' anche la rete di sicurezza per i wake REMOTI a pari
     * grado (enqueue senza IPI ne' need_resched: per design non
     * prelazionano): al prossimo fine-quanto questo check li vede e fa
     * round-robin — latenza massima un quanto, mai starvation. */
    sc->slice_deadline = now + s_quantum_ns[current_thread->priority];

    rt_valve_account(sc);
}

void scheduler_tick(void)
{
    scheduler_slice_check();
}

/* Fine quanto di QUESTA CPU per il nucleo eventi (UINT64_MAX = idle o
 * scheduler spento: nulla da armare). */
uint64_t scheduler_slice_deadline_ns(void)
{
    if (!s_active || current_thread == NULL || current_thread->is_idle)
    {
        return UINT64_MAX;
    }
    return sched_here()->slice_deadline;
}

void scheduler_timer_drain_begin(void)
{
    sched_here()->in_timer_drain = true;
}

void scheduler_timer_drain_end(void)
{
    sched_here()->in_timer_drain = false;
    /* Lo switch vero avviene in scheduler_preempt_if_needed (post-EOI). */
}

/* IL GIUDICE. Unico punto in cui si decide una prelazione da interrupt
 * (post-EOI, handler della IPI di resched, uscita da sezione critica).
 *
 * 'need_resched' e' un invito a RIVALUTARE, non un ordine di switchare:
 * l'esecutivo (enqueue_home_and_kick) lo alza a ogni nuovo lavoro senza
 * sapere se batte il corrente. La decisione vera si prende QUI, con il
 * PROPRIO current sotto il PROPRIO lock — dato posseduto, mai in gara con
 * un altro core. Si prelaziona solo se un thread pronto batte
 * STRETTAMENTE in grado il corrente, o se il corrente e' idle (deve
 * cedere a qualunque lavoro). A pari grado NON si prelaziona: il
 * round-robin scatta alla fine del quanto (scheduler_slice_check), non
 * qui. Senza questa guardia, need_resched incondizionato farebbe
 * degradare un thread alto-priorita' per un risveglio piu' basso. */
void scheduler_preempt_if_needed(void)
{
    sched_cpu_t *sc = sched_here();
    if (!s_active || !sc->online || sc->in_timer_drain)
    {
        return;
    }

#ifdef MAINDOB_SMP
    /* Donazione PRIMA del giudizio: la richiesta arriva spesso con una
     * IPI nuda (need_resched locale spento) — controllarla dopo il gate
     * di need_resched la perderebbe. IF spento dal check al ritorno:
     * niente rientranza dello stesso donatore su questo core (il
     * lifecycle-lock non e' rientrante). */
    if (unlikely(sc->migrate_req != MIGRATE_NONE))
    {
        uint32_t dfl = irq_save();
        if (sc->migrate_req != MIGRATE_NONE)
        {
            scheduler_donate(sc);
        }
        irq_restore(dfl);
    }
#endif

    if (!sc->need_resched)
    {
        return;
    }
    if (current_thread != NULL && current_thread->crit_depth > 0)
    {
        return;                     /* need_resched resta: scatta all'uscita */
    }

    /* IF spento dalla DECISIONE fino allo switch: nessuna preemption
     * intermedia puo' rendere stantia la scelta 'preempt' (finestra che
     * altrimenti, tra release del lock e reschedule, permetterebbe a un
     * thread di girare e poi a reschedule(false) di degradare il
     * corrente). reschedule gestisce IF al proprio interno; il nostro
     * irq_restore chiude il tutto. */
    uint32_t fl = irq_save();
    spinlock_acquire(&sc->lock);

    /* Due soglie, una sola decisione:
     *  - WAKE (quanto ancora vivo): si prelaziona solo per un pronto che
     *    batte STRETTAMENTE il corrente. A pari grado NO: il corrente si
     *    tiene il quanto che sta usando.
     *  - FINE QUANTO (scadenza gia' passata): round-robin, si cede a un
     *    pronto di grado PARI o migliore. E' la stessa soglia '<=' con cui
     *    scheduler_slice_check ha alzato need_resched; senza distinguerla,
     *    il gate stretto del wake ucciderebbe il time-slicing tra pari.
     *  - Idle: cede a qualunque lavoro. */
    bool preempt;
    if (current_thread == NULL || current_thread->is_idle)
    {
        preempt = true;
    }
    else
    {
        uint32_t hi = highest_ready_priority(sc);
        bool quantum_expired = clock_now_ns() >= sc->slice_deadline;
        preempt = (hi < current_thread->priority) ||
                  (quantum_expired && hi <= current_thread->priority);
    }
    sc->need_resched = false;       /* invito consumato in ogni caso */
    spinlock_release(&sc->lock);

    /* Se nulla batte il corrente: flag consumato, si resta. Il thread
     * appena svegliato e' comunque in coda; girera' al fine-quanto del
     * corrente (latenza massima un quanto, mai starvation). Se invece
     * qualcosa outranks, reschedule(false) lo raccoglie: pick_next pesca
     * il piu' alto pronto (che qui e' garantito battere il corrente),
     * requeue del corrente, switch. */
    if (preempt)
    {
        reschedule(sc, false);
    }
    irq_restore(fl);
}

void scheduler_enter_critical(void)
{
    uint32_t fl = irq_save();
    if (current_thread->crit_depth < 255)
    {
        current_thread->crit_depth++;
    }
    irq_restore(fl);
}

void scheduler_exit_critical(void)
{
    uint32_t fl = irq_save();
    if (current_thread->crit_depth > 0)
    {
        current_thread->crit_depth--;
    }
    bool run_pending = (current_thread->crit_depth == 0);
    irq_restore(fl);

    if (run_pending)
    {
        scheduler_preempt_if_needed();  /* preemption rimandata: ora   */
    }
}

/* === Self-test ============================================================ */

static volatile uint32_t s_test_a;
static volatile uint32_t s_test_b;

static void selftest_worker_a(void *arg UNUSED)
{
    for (int i = 0; i < 5; i++)
    {
        s_test_a++;
        scheduler_sleep_ns(MS_TO_NS(2));
    }
}

static void selftest_worker_b(void *arg UNUSED)
{
    for (int i = 0; i < 5; i++)
    {
        s_test_b++;
        scheduler_sleep_ns(MS_TO_NS(3));
    }
}

bool scheduler_selftest(void)
{
    s_test_a = 0;
    s_test_b = 0;

    thread_t *a = thread_create_kernel(selftest_worker_a, NULL, 2);
    thread_t *b = thread_create_kernel(selftest_worker_b, NULL, 2);
    if (a == NULL || b == NULL)
    {
        return false;
    }

    uint64_t deadline = clock_now_ms() + 500;
    while ((s_test_a < 5 || s_test_b < 5) && clock_now_ms() < deadline)
    {
        scheduler_yield();
        __asm__ volatile ("sti; hlt");  /* lascia girare tick e worker    */
    }

    return (s_test_a == 5 && s_test_b == 5);
}

/* === Blocco idle ========================================================= */

/* Idle: c'e' lavoro locale che giustifica un giro di scheduling invece
 * di dormire? Lettura racy senza lock (falso positivo = uno yield in
 * piu', innocuo). */
bool scheduler_idle_has_work(void)
{
    sched_cpu_t *sc = sched_here();
    return sc->ready_mask != 0 || sc->need_resched;
}

/* Blocco idle AUTONOMO — l'unico punto in cui una CPU si addormenta.
 * Condiviso da BSP (idle thread) e AP (scheduler_enter_ap). Ritorna true
 * se c'e' lavoro da schedulare (il chiamante fa scheduler_yield), false
 * dopo aver dormito (svegliato da un evento: IPI, IRQ, scadenza).
 *
 * Correttezza del risveglio, un lato della barriera simmetrica:
 *   1. cpu_idle = true    -> "sto per dormire", visibile ai waker
 *   2. memory_barrier()   -> pubblica cpu_idle PRIMA di rileggere il lavoro
 *   3. rilettura del lavoro (ready_mask | need_resched)
 * Il waker (enqueue_home_and_kick) fa lo speculare: accoda + need_resched,
 * barriera, legge cpu_idle. Su TSO x86 lo store-buffer riordina
 * store-poi-load: con la barriera su ENTRAMBI i lati, di due CPU che
 * corrono almeno una vede lo store dell'altra -> niente wake perso.
 *
 * Se dopo il check non c'e' lavoro, 'sti;hlt' FUSI: un evento nella
 * finestra (IPI/IRQ/scadenza gia' pendente) e' preso DENTRO l'hlt, non
 * perso; l'sti esplicito immunizza da un IF=0 trapelato. Il flag NON
 * viene azzerato qui all'uscita: ci pensa switch_to (che segue sempre la
 * verita' su chi gira), cosi' se l'hlt viene interrotto da una IPI che
 * switcha a un thread reale, cpu_idle scende comunque a false. */
bool scheduler_idle_block(void)
{
    sched_cpu_t *sc = sched_here();

#ifdef MAINDOB_SMP
    sc->cpu_idle = true;
    memory_barrier();
#endif

    if (sc->ready_mask != 0 || sc->need_resched)
    {
#ifdef MAINDOB_SMP
        sc->cpu_idle = false;       /* niente sonno: torniamo attivi */
#endif
        return true;
    }

#ifdef MAINDOB_SMP
    /* Nulla da fare: PRIMA di dormire, chiedi lavoro a chi ne ha
     * (pilastro 1: si chiede, non si ruba). cpu_idle e' GIA' alzato
     * sotto barriera: se la donazione completa tra questa richiesta e
     * l'hlt, il kick del donatore vede cpu_idle=true -> IPI garantita,
     * e sti;hlt fusi la prendono anche se gia' pendente — nessun
     * risveglio perso. */
    idle_try_pull(sc);
#endif

    /* Prima di dormire: auto-pulizia opportunistica (krt/reclaim).
     * Contesto perfetto — thread idle, nessun lavoro pronto — e costo
     * a vuoto di una lettura di clock: il sistema quieto si riporta
     * allo stato di riposo invece di custodire per sempre le cicatrici
     * dell'ultimo picco. */
    reclaim_idle_consider();

    __asm__ volatile ("sti; hlt");
    return false;
}
