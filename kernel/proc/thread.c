#include "proc/thread.h"
#include "proc/tasksnap.h"
#include "proc/scheduler.h"
#include "mm/kheap.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "lib/string.h"
#include "console/console.h"
#include "arch/x86/cpu.h"
#include "arch/x86/gdt.h"
#include "sync/mutex.h"
#include "ipc/port.h"
#include "proc/workqueue.h"
#include "proc/process.h"
#include "ipc/channel.h"
#include "kernel.h"
#include "sync/spinlock.h"
#include "arch/x86/tlb.h"

static thread_t  *s_thread_table[MAX_THREADS];
static list_t     s_all_threads;
static tid_t      s_next_tid = 1;

/* Lifecycle-lock: unico dominio che serializza OGNI transizione di vita
 * e morte di un thread, cross-core. Copre: tabella tid + s_all_threads,
 * pool stack kernel, e per-processo threads/thread_count/awaiting_teardown.
 * irq_save da solo basta su UP ma NON su SMP (disabilita gli IRQ solo sul
 * core locale): senza questo lock il claim single-winner del teardown
 * diventa una race -> doppio process_finalize (il PCB double-free). I
 * cadaveri thread NON stanno piu' in una lista globale: vanno sulla
 * reap_list della loro home-core (scheduler.c) e sono mietuti solo da quel
 * core, dopo lo switch via -> niente reaper-UAF cross-core. Regola: il
 * lavoro pesante (kfree, paging, cleanup mutex/ipc/scheduler) gira SEMPRE
 * fuori dal lock; qui dentro solo puntatori e contatori. */
static spinlock_t s_table_lock = SPINLOCK_INIT;

/* Cache di stack kernel (regione con guard gia' smontata): la morte e
 * la nascita di un thread diventano un hand-off di puntatore, zero
 * lavoro di paging sul percorso caldo (idea 1.0, era buona). */
#define KSTACK_POOL_MAX 8u
static uint32_t s_kstack_pool[KSTACK_POOL_MAX];
static uint32_t s_kstack_pool_n;

extern void kernel_thread_entry(void);
extern void user_thread_entry(void);

/* === Verbi stack ========================================================= */

static uint32_t stack_pages_total(void)
{
    return KERNEL_STACK_SIZE / PAGE_SIZE + 1u;  /* + guard                */
}

static uint32_t acquire_stack_region(void)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_table_lock);
    uint32_t base = (s_kstack_pool_n > 0) ? s_kstack_pool[--s_kstack_pool_n]
                                          : 0;
    spinlock_release_irqrestore(&s_table_lock, fl);

    if (base != 0)
    {
        return base;                    /* guard gia' smontata            */
    }

    base = kpages_alloc(stack_pages_total());
    if (base == 0)
    {
        return 0;
    }

    /* La prima pagina diventa guard: overflow -> page fault rumoroso,
     * mai corruzione silenziosa del vicino. */
    uint32_t p = paging_get_physical(base, NULL);
    paging_unmap_page(base);
    tlb_shootdown_page(base);           /* prima che il frame torni al pool */
    if (p != 0)
    {
        pmm_free_frame(p & 0xFFFFF000u);
    }
    return base;
}

static void release_stack_region(uint32_t guard_base)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_table_lock);
    bool pooled = (s_kstack_pool_n < KSTACK_POOL_MAX);
    if (pooled)
    {
        s_kstack_pool[s_kstack_pool_n++] = guard_base;
    }
    spinlock_release_irqrestore(&s_table_lock, fl);

    if (!pooled)
    {
        /* La guard e' gia' senza frame: kpages_free la salta (phys=0). */
        kpages_free(guard_base, stack_pages_total());
    }
}

/* === Verbi contesto ====================================================== */

/* Prepara lo stack iniziale con il layout atteso da context_switch:
 * [edi esi ebx ebp eflags ret->kernel_thread_entry]. */
static void seed_initial_context(thread_t *t)
{
    uint32_t *sp = (uint32_t *)t->kernel_stack_top;

    *--sp = (uint32_t)kernel_thread_entry;      /* ret di context_switch  */
    *--sp = 0x00000002;                         /* EFLAGS: IF=0, bit1=1   */
    *--sp = 0;                                  /* ebp                    */
    *--sp = 0;                                  /* ebx                    */
    *--sp = 0;                                  /* esi                    */
    *--sp = 0;                                  /* edi                    */

    t->context.esp = (uint32_t)sp;
    t->context.cr3 = cpu_read_cr3();
}

/* Trampolino C chiamato da kernel_thread_entry (context.asm). */
void thread_entry_trampoline(void);
void thread_entry_trampoline(void)
{
    thread_t *self = current_thread;
    self->entry(self->entry_arg);
    thread_exit();
}

/* === Verbi tabella ======================================================= */

/* Chiamante tiene s_table_lock. */
static tid_t claim_free_tid(void)
{
    for (tid_t probe = 0; probe < MAX_THREADS; probe++)
    {
        tid_t cand = (tid_t)((s_next_tid + probe) % MAX_THREADS);
        if (cand != 0 && s_thread_table[cand] == NULL)
        {
            s_next_tid = cand + 1;
            return cand;
        }
    }
    return -1;
}

/* Nascita atomica: riserva il tid, pubblica il thread nella tabella + lista
 * globale e, se ha un processo proprietario non-idle, lo aggancia alla lista
 * thread del processo con thread_count++ — tutto in UNA sezione di lock. Cosi'
 * un teardown concorrente o vede il thread interamente agganciato o non lo
 * vede affatto: niente orfani, niente conteggio incoerente. Ritorna il tid, o
 * -1 se la tabella e' piena. (Fonde i due passi non-atomici del 1.1.) */
static tid_t thread_publish(thread_t *t)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_table_lock);
    tid_t tid = claim_free_tid();
    if (tid >= 0)
    {
        t->tid = tid;
        s_thread_table[tid] = t;
        list_push_back(&s_all_threads, &t->global_node);
        if (t->owner != NULL && !t->is_idle)
        {
            /* Home AUTORITATIVA dei thread utente: riletta dal processo
             * QUI, sotto il lifecycle-lock — lo stesso lock sotto cui il
             * donatore della migrazione flippa proc->home_cpu e ri-homa
             * la lista threads. Cosi' o questo thread e' gia' in lista
             * quando il donatore la percorre (e viene ri-homato), o la
             * pubblicazione avviene dopo il flip e legge la home NUOVA.
             * Il valore pre-assegnato dal costruttore (fuori lock) era
             * la finestra: thread homed sul core vecchio con processo
             * migrato = due core sullo stesso CR3. I kernel thread
             * (owner PID 0) tengono la CPU del creatore. */
            if (t->owner->pid != 0)
            {
                t->home_cpu = t->owner->home_cpu;
            }
            list_push_back(&t->owner->threads, &t->process_node);
            t->owner->thread_count++;
        }
        if (!t->is_idle)
        {
            /* Bilancio placement: +1 sulla home (advisory, atomico).
             * Idle esclusi: pinnati, permanenti, uno per CPU — solo
             * rumore simmetrico. home_cpu qui e' quella DEFINITIVA
             * (assegnata sopra, sotto questo stesso lock). */
            scheduler_account_thread_homed(t->home_cpu);
        }
    }
    spinlock_release_irqrestore(&s_table_lock, fl);
    return tid;
}

/* Il lifecycle-lock (s_table_lock) e' l'autorita' su nascita/morte dei
 * thread E sulla home dei processi: la migrazione cooperativa flippa
 * home_cpu sotto questo lock, e chiunque debba leggere una home
 * COERENTE col ciclo di vita (router del teardown, donatore) passa da
 * qui. Accessori per scheduler.c: il lock resta privato di questo
 * file. */
uint32_t thread_lifecycle_lock(void)
{
    return spinlock_acquire_irqsave(&s_table_lock);
}

void thread_lifecycle_unlock(uint32_t fl)
{
    spinlock_release_irqrestore(&s_table_lock, fl);
}

/* === Snapshot per il task manager =======================================
 * Cammina la lista globale dei THREAD sotto il lifecycle-lock e aggrega
 * per processo — mai la proclist: nessun nesting di lock nuovo, e ogni
 * PCB toccato e' vivo per costruzione (i suoi thread sono in tabella,
 * finalize viene dopo il loro detach). Conseguenza accettata: gli
 * zombie senza thread non compaiono (un task manager mostra chi VIVE).
 * I kernel thread confluiscono nella riga aggregata 'kernel' (PID 0);
 * gli idle restano fuori (il loro tempo e' l'ozio per-CPU nell'header). */

/* Verbo: trova o crea la riga del processo `p`. NULL = righe esaurite. */
static dob_tasksnap_row_t *snap_row_of(dob_tasksnap_row_t *rows,
                                       uint32_t *nrows, uint32_t max,
                                       struct process *p)
{
    for (uint32_t i = 0; i < *nrows; i++)
    {
        if (rows[i].pid == p->pid)
        {
            return &rows[i];
        }
    }
    if (*nrows >= max)
    {
        return NULL;
    }
    dob_tasksnap_row_t *r = &rows[(*nrows)++];
    r->pid      = p->pid;
    r->state    = (p->state == PROC_ZOMBIE) ? DOB_TASK_STATE_ZOMBIE
                                            : DOB_TASK_STATE_ALIVE;
    r->home_cpu = p->home_cpu;
    r->pinned   = p->pinned ? 1 : 0;
    r->nthreads = 0;
    r->priority = SCHED_NUM_PRIORITIES - 1;     /* si migliora coi thread */
    /* RAM vera vs aperture device, separate: la VRAM mappata di un
     * driver video NON e' RAM consumata (l'artefatto dei "24 MB" di
     * bga). Letture advisory senza lock, u32 mai strappate. */
    r->mem_pages = p->vm_regions.total_pages
                 - p->vm_regions.device_pages;
    r->dev_pages = p->vm_regions.device_pages;
    r->cpu_ns   = p->cpu_time_dead_ns;          /* i morti, poi i vivi  */
    uint32_t n = 0;
    while (n < DOB_TASKSNAP_NAME_LEN - 1 && p->name[n] != '\0')
    {
        r->name[n] = p->name[n];
        n++;
    }
    while (n < DOB_TASKSNAP_NAME_LEN)
    {
        r->name[n++] = '\0';
    }
    return r;
}

/* Renice di un intero processo (task manager): nuova base_priority per
 * tutti i suoi thread, con effettiva PI-CONSAPEVOLE — lo stesso modello
 * del recompute del mutex: effettiva = min(base, boost dei mutex
 * posseduti). La lettura di m->boost_prio e' senza guard, come nel
 * recompute stesso: advisory, si sana all'unlock successivo. Tutto
 * sotto il lifecycle-lock: thread vivi per costruzione, e la consegna
 * cross-core del cambio la fa scheduler_set_priority (b119). */
int thread_renice_by_pid(pid_t pid, uint32_t prio)
{
    if (pid <= 0 || prio >= SCHED_NUM_PRIORITIES)
    {
        return -1;                      /* PID 0 (kernel) intoccabile    */
    }

    int touched = 0;
    uint32_t fl = spinlock_acquire_irqsave(&s_table_lock);
    list_node_t *n;
    list_for_each(n, &s_all_threads)
    {
        thread_t *t = list_entry(n, thread_t, global_node);
        if (t->is_idle || t->owner == NULL || t->owner->pid != pid)
        {
            continue;
        }
        t->base_priority = prio;

        uint32_t eff = prio;
        for (uint32_t i = 0; i < t->owned_mutex_count; i++)
        {
            struct mutex *m = t->owned_mutexes[i];
            if (m != NULL && m->boost_prio < eff)
            {
                eff = m->boost_prio;    /* il PI vince sul renice        */
            }
        }
        scheduler_set_priority(t, eff);
        touched++;
    }
    spinlock_release_irqrestore(&s_table_lock, fl);
    return touched > 0 ? 0 : -1;
}

int task_snapshot_collect(dob_tasksnap_row_t *rows, uint32_t max)
{
    if (rows == NULL || max == 0)
    {
        return -1;
    }
    uint32_t nrows = 0;

    uint32_t fl = spinlock_acquire_irqsave(&s_table_lock);
    list_node_t *n;
    list_for_each(n, &s_all_threads)
    {
        thread_t *t = list_entry(n, thread_t, global_node);
        if (t->is_idle || t->owner == NULL)
        {
            continue;                   /* ozio: nell'header per-CPU     */
        }
        dob_tasksnap_row_t *r = snap_row_of(rows, &nrows, max, t->owner);
        if (r == NULL)
        {
            break;                      /* righe ABI esaurite: troncato  */
        }
        if (r->nthreads < 255)
        {
            r->nthreads++;
        }
        if (t->priority < r->priority)
        {
            r->priority = (uint8_t)t->priority;
        }
        r->cpu_ns += scheduler_thread_cpu_ns(t);
    }
    spinlock_release_irqrestore(&s_table_lock, fl);
    return (int)nrows;
}

/* Transizione a morte atomica. Un solo chiamante vince (stato != DEAD ->
 * DEAD) e prosegue con estrazione + parcheggio sulla reap_list; gli altri
 * (thread gia' in morte, es. auto-uscito prima che il kill-loop lo raggiunga)
 * vedono false e non lo toccano — niente doppio reap. */
static bool thread_begin_death(thread_t *t)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_table_lock);
    bool won = (t->state != THREAD_DEAD);
    if (won)
    {
        t->state = THREAD_DEAD;
    }
    spinlock_release_irqrestore(&s_table_lock, fl);
    return won;
}

/* Stacca il corpo da tabella + lista globale + lista del processo,
 * decrementa i thread vivi del processo e RIVENDICA il finalize se questo
 * era l'ultimo (count==0 && armato) — tutto in una sezione, cosi' il claim
 * single-winner e' atomico anche se un altro reaper (o process_destroy)
 * gareggia sullo stesso processo. Ritorna il processo da finalizzare FUORI
 * dal lock, o NULL. */
static struct process *reap_detach(thread_t *t)
{
    struct process *to_finalize = NULL;

    uint32_t fl = spinlock_acquire_irqsave(&s_table_lock);
    s_thread_table[t->tid] = NULL;
    scheduler_account_thread_unhomed(t->home_cpu);  /* -1: simmetrico a
                                                     * publish (idle non
                                                     * arrivano mai qui) */
    if (t->owner != NULL)
    {
        /* Il tempo CPU del morto confluisce nel processo: lo snapshot
         * somma vivi + questo accumulatore. Sotto il lifecycle-lock il
         * PCB e' vivo per costruzione (finalize viene dopo il detach). */
        t->owner->cpu_time_dead_ns += t->cpu_time_ns;
    }
    if (list_node_is_linked(&t->global_node))
    {
        list_remove(&t->global_node);
    }
    if (list_node_is_linked(&t->process_node))
    {
        list_remove(&t->process_node);
    }

    struct process *owner = t->owner;
    if (owner != NULL && owner->pid != 0)
    {
        owner->thread_count--;
        if (owner->thread_count == 0 && owner->awaiting_teardown)
        {
            owner->awaiting_teardown = false;   /* claim: un solo vincitore */
            to_finalize = owner;
        }
    }
    spinlock_release_irqrestore(&s_table_lock, fl);
    return to_finalize;
}

/* Stacca dalla lista del processo il primo thread diverso dal chiamante e lo
 * restituisce (o NULL: resta solo il chiamante o nessuno). Il kill-loop di
 * process_destroy lo distrugge FUORI dal lock — nessun annidamento con i lock
 * di scheduler/timer/wait. Averlo gia' staccato qui evita che il reaper o un
 * secondo giro lo ritrovino. */
static thread_t *thread_take_other_from_process(struct process *proc)
{
    thread_t *victim = NULL;

    uint32_t fl = spinlock_acquire_irqsave(&s_table_lock);
    list_node_t *n;
    list_for_each(n, &proc->threads)
    {
        thread_t *t = list_entry(n, thread_t, process_node);
        if (t != current_thread)
        {
            list_remove(&t->process_node);
            victim = t;
            break;
        }
    }
    spinlock_release_irqrestore(&s_table_lock, fl);
    return victim;
}

/* === API ================================================================== */

void thread_init(void)
{
    list_init(&s_all_threads);
    memset(s_thread_table, 0, sizeof(s_thread_table));
    kprintf("[THRD] Sottosistema thread pronto (max %u).\n",
            (uint32_t)MAX_THREADS);
}

extern void scheduler_sleep_timer_fire(void *arg);

void thread_bootstrap_idle(void)
{
    thread_t *boot = (thread_t *)kcalloc(1, sizeof(thread_t));
    KASSERT(boot != NULL);

    boot->tid           = 0;
    boot->state         = THREAD_RUNNING;
    boot->priority      = SCHED_IDLE_PRIO;
    boot->base_priority = SCHED_IDLE_PRIO;
    boot->is_idle       = true;
    boot->context.cr3   = cpu_read_cr3();

    timer_init(&boot->sleep_timer, scheduler_sleep_timer_fire, boot);
    list_node_init(&boot->sched_node);
    list_node_init(&boot->global_node);

    boot->home_cpu = 0;
    this_cpu()->loaded_cr3 = boot->context.cr3;
    s_thread_table[0] = boot;
    list_push_back(&s_all_threads, &boot->global_node);
    current_thread = boot;
    scheduler_adopt_idle(boot);         /* fallback permanente            */

    kprintf("[THRD] Boot thread = idle (TID 0, prio %u).\n", SCHED_IDLE_PRIO);
}

/* Idle della AP chiamante: il flusso di controllo gia' in esecuzione
 * sull'AP diventa un thread (come il boot thread della BSP), con un tid
 * reale e lo stack del bring-up. Reso current di questa CPU. */
thread_t *thread_bootstrap_idle_ap(void)
{
    thread_t *idle = (thread_t *)kcalloc(1, sizeof(thread_t));
    if (idle == NULL)
    {
        return NULL;
    }

    idle->state         = THREAD_RUNNING;
    idle->priority      = SCHED_IDLE_PRIO;
    idle->base_priority = SCHED_IDLE_PRIO;
    idle->is_idle       = true;
    idle->home_cpu      = (uint8_t)this_cpu()->cpu_index;
    idle->context.cr3   = cpu_read_cr3();

    timer_init(&idle->sleep_timer, scheduler_sleep_timer_fire, idle);
    list_node_init(&idle->sched_node);
    list_node_init(&idle->global_node);
    list_node_init(&idle->process_node);

    if (thread_publish(idle) < 0)
    {
        kfree(idle);
        return NULL;
    }

    this_cpu()->loaded_cr3 = idle->context.cr3;
    current_thread = idle;
    return idle;
}

thread_t *thread_create_kernel(void (*entry)(void *), void *arg,
                               uint32_t priority)
{
    if (priority >= SCHED_NUM_PRIORITIES)
    {
        return NULL;
    }

    thread_t *t = (thread_t *)kcalloc(1, sizeof(thread_t));
    if (t == NULL)
    {
        return NULL;
    }

    uint32_t guard = acquire_stack_region();
    if (guard == 0)
    {
        kfree(t);
        return NULL;
    }

    t->state              = THREAD_READY;
    t->priority           = priority;
    t->base_priority      = priority;
    t->kernel_stack_guard = guard;
    t->kernel_stack_top   = guard + PAGE_SIZE + KERNEL_STACK_SIZE;
    t->entry              = entry;
    t->entry_arg          = arg;
    t->owner              = process_get_by_pid(0);
    t->home_cpu           = (uint8_t)this_cpu()->cpu_index;

    timer_init(&t->sleep_timer, scheduler_sleep_timer_fire, t);
    list_node_init(&t->sched_node);
    list_node_init(&t->global_node);
    list_node_init(&t->process_node);
    fpu_seed_thread_area(fpu_area(t));  /* PRIMA del dispatch (vedi fpu.c)*/
    seed_initial_context(t);

    if (thread_publish(t) < 0)
    {
        release_stack_region(guard);
        kfree(t);
        kprintf("[THRD] TID esauriti\n");
        return NULL;
    }

    scheduler_add(t);
    return t;
}

void thread_exit(void)
{
    thread_t *self = current_thread;

    /* Un solo sito parcheggia il corpo: transizione atomica a DEAD e poi il
     * push sulla reap_list della home-core (questo stesso core). Verra'
     * mietuto dal suo idle, DOPO lo switch via -> nessun UAF. (self e'
     * running, non in runqueue: nessuna estrazione.) */
    (void)thread_begin_death(self);
    scheduler_queue_reap(self);

    scheduler_block_current(THREAD_DEAD);       /* non ritorna            */
    kpanic("thread_exit: ritorno impossibile (TID %d)", self->tid);
}

/* Distruzione di un thread DIVERSO dal chiamante. Invariante di questo
 * kernel: i thread di un processo condividono la home CPU e non migrano, e
 * process_destroy gira sulla home del processo — quindi il bersaglio non e'
 * MAI in esecuzione su un altro core (un solo thread gira per core), il che
 * rende sicura l'estrazione dallo scheduler qui. */
void thread_destroy(thread_t *t)
{
    if (t == NULL || t->tid == 0)
    {
        kprintf("[THRD] destroy TID 0 negato\n");
        return;
    }
    if (t == current_thread)
    {
        kprintf("[THRD] destroy di se stessi: usare thread_exit\n");
        return;
    }

    /* Vince un solo distruttore. Se t era gia' in morte (auto-uscito prima
     * che il kill-loop lo raggiungesse) e' gia' nel cimitero: non toccarlo,
     * o scheduler_remove ne corromperebbe la lista e il reaper lo mieterebbe
     * due volte. */
    if (!thread_begin_death(t))
    {
        return;
    }

    timer_cancel(&t->sleep_timer);
    if (t->blocked_on != NULL)
    {
        wait_queue_remove_thread(t->blocked_on, t);
    }
    scheduler_remove(t);
    scheduler_queue_reap(t);            /* reap sulla home-core di t     */
}

/* Uccide tutti i thread del processo TRANNE il chiamante (che sta chiudendo il
 * proprio processo). Ogni vittima e' staccata dalla lista del processo sotto
 * il lifecycle-lock e distrutta FUORI dal lock, una alla volta: nessun
 * annidamento con i lock di scheduler/timer/wait, e un thread gia' staccato
 * non puo' essere ripescato da un secondo giro o dal reaper. */
void thread_kill_others_of(struct process *proc)
{
    for (;;)
    {
        thread_t *victim = thread_take_other_from_process(proc);
        if (victim == NULL)
        {
            break;
        }
        thread_destroy(victim);
    }
}

thread_t *thread_get_by_tid(tid_t tid)
{
    if (tid < 0 || tid >= MAX_THREADS)
    {
        return NULL;
    }
    return s_thread_table[tid];
}

/* Boost di priority-inheritance per TID, con CATENA TRANSITIVA: alza a
 * 'prio' l'owner risolto per TID e, se quello e' a sua volta BLOCCATO
 * su un mutex, risale al suo owner (waiting_owner_tid) e cosi' via —
 * A(alta) -> M1(B) -> M2(C): senza catena C restava basso e
 * l'inversione persisteva attraverso B, tipicamente su un ALTRO core.
 *
 * Sicurezza (la catena della 1.1 iniziale fu rimossa per UAF): tutto
 * sotto s_table_lock, lo STESSO lock che reap_detach prende per
 * azzerare la entry PRIMA del kfree(t) — nessun thread della catena
 * puo' essere liberato mentre camminiamo. E si cammina di SOLO TID in
 * TID (waiting_owner_tid, valore sul thread): nessun mutex altrui
 * viene mai dereferenziato. Un TID stantio (owner cambiato sotto di
 * noi) al peggio boosta il thread sbagliato: inflazione temporanea
 * sanata dal recompute al suo unlock, mai un puntatore morto.
 *
 * Profondita' limitata: catene reali sono corte; il limite taglia
 * anche un eventuale ciclo di deadlock senza girare per sempre.
 * Il nesting s_table_lock -> sc->lock (dentro scheduler_set_priority)
 * non ha inverso: lo scheduler non prende mai s_table_lock sotto il
 * proprio sc->lock. */
#define PI_CHAIN_MAX_DEPTH 8

void thread_boost_by_tid(tid_t tid, uint32_t prio)
{
    if (tid <= 0 || tid >= MAX_THREADS)
    {
        return;
    }
    uint32_t fl = spinlock_acquire_irqsave(&s_table_lock);

    tid_t cursor = tid;
    for (int depth = 0; depth < PI_CHAIN_MAX_DEPTH; depth++)
    {
        if (cursor <= 0 || cursor >= MAX_THREADS)
        {
            break;
        }
        thread_t *owner = s_thread_table[cursor];
        if (owner == NULL || prio >= owner->priority)
        {
            break;                      /* sparito, o gia' abbastanza su */
        }

        scheduler_set_priority(owner, prio);

        /* Anello successivo: solo se l'owner e' a sua volta in attesa
         * di un mutex. Il check su waiting_on_mutex e' di NON-nullita'
         * (nessuna dereferenziazione); il TID e' il valore registrato
         * dal waiter sotto il guard del suo mutex. */
        if (owner->state != THREAD_BLOCKED ||
            owner->waiting_on_mutex == NULL)
        {
            break;
        }
        tid_t next = owner->waiting_owner_tid;
        if (next == cursor || next == tid)
        {
            break;                      /* ciclo (deadlock): non girare  */
        }
        cursor = next;
    }

    spinlock_release_irqrestore(&s_table_lock, fl);
}

/* Reclamation di UN cadavere. Chiamata da scheduler_reap_local sulla
 * home-core del thread (contesto idle): rilascia i mutex orfani, smonta le
 * strutture, ricicla lo stack e libera il PCB thread. Ritorna il processo da
 * finalizzare (se questo era l'ultimo thread e reap_detach ha rivendicato il
 * finalize) o NULL — il finalize dell'address space lo fa il chiamante,
 * sicuro perche' e' la home-core e nessun core e' sul CR3. */
struct process *thread_reap_one(thread_t *t)
{
    /* Pesante, FUORI dal lifecycle-lock (prendono lock propri). */
    mutex_release_all_owned(t);
    timer_cancel(&t->sleep_timer);
    ipc_cleanup_thread(t->tid);

    /* Stacco atomico + claim single-winner del teardown del processo. */
    struct process *to_finalize = reap_detach(t);

    if (t->kernel_stack_guard != 0)
    {
        release_stack_region(t->kernel_stack_guard);
    }
    kfree(t);

    return to_finalize;
}

/* Prepara lo stack kernel di un thread utente. Layout (dal fondo):
 *
 *   [SS_user][ESP_user][EFLAGS|IF][CS_user][EIP=entry]   <- frame iret
 *   [arg]                                <- pop ecx in user_thread_entry
 *   [ret=user_thread_entry][EFLAGS=0x2][ebp][ebx][esi][edi]  <- switch
 *
 * L'IF del frame di context_switch resta 0: gli interrupt si accendono
 * solo con l'iretd (che carica EFLAGS|IF=1), atomicamente col salto a
 * ring 3 — la finestra "frame preparato ma IRQ attivi" non esiste.
 * ABI 1:1 col 1.0: arg consegnato in ECX. */
static void seed_user_context(thread_t *t, uint32_t entry, uint32_t arg,
                              uint32_t user_esp)
{
    uint32_t *sp = (uint32_t *)t->kernel_stack_top;

    *--sp = 0x23;                           /* SS  = user data RPL3       */
    *--sp = user_esp;                       /* ESP utente                 */
    *--sp = 0x00000202;                     /* EFLAGS: IF=1, bit1=1       */
    *--sp = 0x1B;                           /* CS  = user code RPL3       */
    *--sp = entry;                          /* EIP                        */

    *--sp = arg;                            /* -> ECX (ABI thread arg)    */

    *--sp = (uint32_t)user_thread_entry;    /* ret di context_switch      */
    *--sp = 0x00000002;                     /* EFLAGS switch: IF=0        */
    *--sp = 0;                              /* ebp                        */
    *--sp = 0;                              /* ebx                        */
    *--sp = 0;                              /* esi                        */
    *--sp = 0;                              /* edi                        */

    t->context.esp = (uint32_t)sp;
    t->context.cr3 = t->owner->page_directory;
}

static thread_t *create_user_common(struct process *proc, uint32_t entry,
                                    uint32_t arg, uint32_t user_stack_top,
                                    bool suspended);

thread_t *thread_create_user(struct process *proc, uint32_t entry,
                             uint32_t arg, uint32_t user_stack_top)
{
    return create_user_common(proc, entry, arg, user_stack_top, false);
}

thread_t *thread_create_user_suspended(struct process *proc, uint32_t entry,
                                       uint32_t arg, uint32_t user_stack_top)
{
    return create_user_common(proc, entry, arg, user_stack_top, true);
}

static thread_t *create_user_common(struct process *proc, uint32_t entry,
                                    uint32_t arg, uint32_t user_stack_top,
                                    bool suspended)
{
    thread_t *t = (thread_t *)kcalloc(1, sizeof(thread_t));
    if (t == NULL)
    {
        return NULL;
    }

    uint32_t guard = acquire_stack_region();
    if (guard == 0)
    {
        kfree(t);
        return NULL;
    }

    t->state              = THREAD_READY;
    t->priority           = 2;              /* Normal                     */
    t->base_priority      = 2;
    t->kernel_stack_guard = guard;
    t->kernel_stack_top   = guard + PAGE_SIZE + KERNEL_STACK_SIZE;
    t->owner              = proc;
    t->home_cpu           = proc->home_cpu;     /* i thread seguono il
                                                 * processo, non il
                                                 * creatore            */

    timer_init(&t->sleep_timer, scheduler_sleep_timer_fire, t);
    list_node_init(&t->sched_node);
    list_node_init(&t->global_node);
    list_node_init(&t->process_node);
    fpu_seed_thread_area(fpu_area(t));
    seed_user_context(t, entry, arg, user_stack_top);

    if (thread_publish(t) < 0)
    {
        release_stack_region(guard);
        kfree(t);
        return NULL;
    }

    if (suspended)
    {
        t->state = THREAD_BLOCKED;      /* riparte con scheduler_unblock */
        return t;
    }
    scheduler_add(t);
    return t;
}

/* Claim single-winner del teardown dell'address space (fix double-free PCB):
 * se non resta alcun thread vivo, il chiamante finalizza ORA; altrimenti arma
 * e l'ultimo reap finalizzera' esattamente una volta. Sotto lo STESSO
 * lifecycle-lock del claim in reap_detach -> lettura di thread_count e set di
 * awaiting_teardown atomici rispetto al decremento cross-core: e' questo che
 * chiude la finestra del double-free su SMP. */
bool thread_arm_process_teardown(struct process *proc)
{
    uint32_t fl = spinlock_acquire_irqsave(&s_table_lock);
    bool finalize_now = (proc->thread_count == 0);
    proc->awaiting_teardown = !finalize_now;
    spinlock_release_irqrestore(&s_table_lock, fl);
    return finalize_now;
}

bool process_arm_teardown(struct process *proc);
extern void kernel_run_pending_respawns(void);  /* respawn primary non-perdibili */

void idle_entry(void)
{
    scheduler_yield();                  /* kick iniziale                  */

    for (;;)
    {
        scheduler_run_local_teardowns();    /* teardown instradati qui   */
        scheduler_reap_local();             /* miete i cadaveri locali   */
        while (workqueue_process_one())
        {
        }
        /* Respawn dei primary morti: DOPO i teardown, cosi' porte/IRQ/
         * registry del cadavere sono gia' liberati. Non-perdibile. */
        kernel_run_pending_respawns();
        /* Blocco idle autonomo: pubblica cpu_idle sotto barriera,
         * rilegge il lavoro e — se non ce n'e' — dorme con sti;hlt fusi.
         * La correttezza del risveglio non e' piu' una "cintura contro i
         * wake persi": e' l'invariante della barriera simmetrica col
         * waker (enqueue_home_and_kick). Se c'e' lavoro, un giro di
         * scheduling. */
        if (scheduler_idle_block())
        {
            scheduler_yield();
            continue;
        }
    }
}

