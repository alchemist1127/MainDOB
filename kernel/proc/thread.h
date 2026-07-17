#ifndef MAINDOB_PROC_THREAD_H
#define MAINDOB_PROC_THREAD_H

#include "lib/types.h"
#include "lib/list.h"
#include "time/timer.h"
#include "proc/wait.h"
#include "arch/x86/fpu.h"
#include "proc/percpu.h"

#define MAX_THREADS         1024
#define KERNEL_STACK_SIZE   8192u   /* + 1 pagina guard non mappata       */
#define THREAD_MAX_OWNED_MUTEXES 16

typedef int32_t tid_t;

typedef enum
{
    THREAD_READY = 0,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_SLEEPING,
    THREAD_DEAD
} thread_state_t;

typedef struct
{
    uint32_t esp;               /* stack pointer salvato da context.asm   */
    uint32_t cr3;               /* address space (kernel PD in fase 2)    */
} context_t;

typedef struct thread
{
    tid_t           tid;
    volatile thread_state_t state;
    uint32_t        priority;       /* 0=RT .. 3=idle                     */
    uint32_t        base_priority;
    bool            is_idle;
    uint8_t         home_cpu;       /* CPU su cui il thread e' pinnato    */
    uint8_t         crit_depth;     /* sezioni critiche driver annidate   */

    context_t       context;
    uint32_t        kernel_stack_guard;   /* base regione (pagina guard)  */
    uint32_t        kernel_stack_top;

    /* Wait/sleep */
    timer_t         sleep_timer;
    wait_queue_t   *blocked_on;
    struct thread  *wq_next;
    int             wait_result;

    /* Mutex posseduti (rilascio orfani + priority inheritance) */
    struct mutex   *owned_mutexes[THREAD_MAX_OWNED_MUTEXES];
    uint32_t        owned_mutex_count;
    uint32_t        untracked_mutexes;    /* oltre il limite: solo log    */
    struct mutex   *waiting_on_mutex;

    struct process *owner;
    list_node_t     process_node;   /* link nella lista thread del processo */
    list_node_t     sched_node;
    list_node_t     global_node;

    void          (*entry)(void *);
    void           *entry_arg;

    /* Stato FPU/SSE per-thread (eager). Grezzo +ALIGN perche' kcalloc
     * non garantisce 16 byte: usare fpu_area(). */
    uint8_t         fpu_raw[FPU_AREA_SIZE + FPU_AREA_ALIGN];

    /* PI transitivo SENZA puntatori attraverso finestre di free: il
     * TID (valore, mai puntatore) dell'owner del mutex su cui questo
     * thread e' bloccato. Scritto dal waiter sotto m->guard (dove
     * owner_tid e' gia' in mano), azzerato (-1) all'uscita dall'attesa.
     * La catena di boost cammina SOLO su thread risolti per TID sotto
     * il lifecycle-lock (che blocca il reap): nessun mutex altrui viene
     * mai dereferenziato — era la ragione per cui la catena della 1.1
     * iniziale fu rimossa. Un valore stantio (owner cambiato sotto di
     * noi) al peggio boosta il thread sbagliato: inflazione temporanea,
     * sanata dal recompute al suo prossimo unlock — mai un UAF. */
    tid_t           waiting_owner_tid;

    /* Tempo CPU accumulato (ns). Scritto SOLO da switch_to sulla home
     * del thread, al momento in cui LASCIA la CPU (single-writer). I
     * lettori (snapshot del task manager) leggono senza lock: un u64
     * puo' strapparsi su i386, quindi lo snapshot rilegge fino a
     * stabilita' — un campione sporco al peggio dura un poll. */
    uint64_t        cpu_time_ns;

    /* Firma comportamentale per la migrazione: true se l'ultima volta
     * che ha lasciato la CPU era ANCORA runnable (preempted volendo
     * girare = compute), false se si e' bloccato da solo (IPC/attese =
     * conversazione). La donazione migra SOLO i compute: spostare un
     * partner IPC spezza la localita' del direct-switch e trasforma
     * ogni hop in enqueue+IPI+switch pieno — il ping-pong di carico
     * visto sul CQ62 in b132. Scritto solo dalla home (requeue/yield),
     * letto dalla donazione sotto i lock di sched: mai in gara. */
    bool            cpu_hungry;
    /* NOTA LAYOUT: campi in CODA alla struct — video_boomerang.asm
     * deriva offset fissi dei campi precedenti (owner a 164, assert
     * statico in driver.c): mai inserire campi prima di owner. */
} thread_t;

static inline void *fpu_area(thread_t *t)
{
    return (void *)ALIGN_UP((uint32_t)t->fpu_raw, FPU_AREA_ALIGN);
}

void      thread_init(void);
void      thread_bootstrap_idle(void);
/* Idle della AP chiamante: creato, registrato e reso current. */
thread_t *thread_bootstrap_idle_ap(void);
thread_t *thread_create_kernel(void (*entry)(void *), void *arg,
                               uint32_t priority);
/* Thread utente (ring 3) per un processo dato. */
thread_t *thread_create_user(struct process *proc, uint32_t entry,
                             uint32_t arg, uint32_t user_stack_top);
/* Come sopra ma NON schedulato: nasce BLOCKED, riparte solo con
 * scheduler_unblock (parcheggio needs: di Startup_modules). */
thread_t *thread_create_user_suspended(struct process *proc, uint32_t entry,
                                       uint32_t arg, uint32_t user_stack_top);
bool     thread_arm_process_teardown(struct process *proc);
void      thread_exit(void) __attribute__((noreturn));
void      thread_destroy(thread_t *t);
/* Uccide tutti i thread del processo tranne il chiamante (teardown). */
void      thread_kill_others_of(struct process *proc);
thread_t *thread_get_by_tid(tid_t tid);
void      thread_boost_by_tid(tid_t tid, uint32_t prio);
/* Reclamation di un cadavere (chiamata da scheduler_reap_local sulla
 * home-core). Ritorna il processo da finalizzare o NULL. */
struct process *thread_reap_one(thread_t *t);
void      idle_entry(void) __attribute__((noreturn));

/* Il thread in esecuzione sulla CPU corrente (per-CPU via %gs). */
#define current_thread (this_cpu()->current)

/* Lifecycle-lock (vedi thread.c): autorita' su nascita/morte dei thread
 * e sulla home dei processi. Usato da scheduler.c per la migrazione
 * cooperativa e dal router del teardown. */
uint32_t thread_lifecycle_lock(void);
void     thread_lifecycle_unlock(uint32_t fl);

#endif
