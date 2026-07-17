#ifndef MAINDOB_PROC_SCHEDULER_H
#define MAINDOB_PROC_SCHEDULER_H

#include "lib/types.h"
#include "proc/thread.h"

#define SCHED_NUM_PRIORITIES 4u
#define SCHED_IDLE_PRIO      (SCHED_NUM_PRIORITIES - 1u)

void scheduler_init(void);
void scheduler_adopt_idle(thread_t *t);
void scheduler_start(void);
bool scheduler_is_running(void);

void scheduler_enter_ap(thread_t *idle);   /* non ritorna: idle della AP */
void scheduler_slice_check(void);

/* Direct-switch: cede la CPU DIRETTAMENTE a `next` (estratto da una
 * wait queue: BLOCKED, fuori dalle runqueue, home su questa CPU),
 * saltando la selezione dello scheduler. `my_state` e' lo stato in cui
 * lascia il chiamante (READY: rimesso in coda; BLOCKED: il chiamante
 * ha gia' pubblicato la propria attesa). Se `next` non e' idoneo
 * (altra CPU, stato inatteso) degrada a unblock normale. */
void scheduler_yield_to(thread_t *next, thread_state_t my_state);

/* Placement: prossima CPU online in round-robin (0 se sole la BSP).
 * Distributore neutro; il default dei processi e' pick_cpu_local. */
uint32_t scheduler_pick_cpu(void);

/* Placement con localita': CPU del creatore, salvo squilibrio oltre lo
 * slack rispetto alla CPU online meno carica (allora quella). Tiene il
 * fast-path IPC same-CPU (direct-switch) e i wake locali. */
uint32_t scheduler_pick_cpu_local(void);

/* Bilancio advisory dei thread homed per CPU (per il placement).
 * Chiamati da thread.c alla nascita (publish) e alla morte (reap). */
void scheduler_account_thread_homed(uint32_t cpu);
void scheduler_account_thread_unhomed(uint32_t cpu);

/* Statistiche per il task manager (letture advisory senza lock). */
bool     scheduler_cpu_idle_ns(uint32_t cpu, uint64_t *out_idle_ns);
uint64_t scheduler_thread_cpu_ns(const struct thread *t);

/* Sezione critica driver: sospende la preemption del thread corrente
 * (need_resched resta pendente e scatta all'uscita). Annidabile. */
void scheduler_enter_critical(void);
void scheduler_exit_critical(void);
uint64_t scheduler_slice_deadline_ns(void);
bool scheduler_cpu_online(uint32_t cpu);

void scheduler_add(thread_t *t);
void scheduler_remove(thread_t *t);

/* Teardown/reclamation instradati sulla home-core (vedi scheduler.c). */
struct process;
void scheduler_queue_reap(thread_t *t);          /* parcheggia un cadavere   */
void scheduler_reap_local(void);                 /* idle: miete i locali     */
bool scheduler_route_process_teardown(struct process *proc); /* true=remoto  */
void scheduler_defer_process_teardown(struct process *proc); /* sempre in idle */
void scheduler_run_local_teardowns(void);        /* idle: teardown locali    */
void scheduler_yield(void);
void scheduler_block_current(thread_state_t reason);
void scheduler_unblock(thread_t *t);
void scheduler_sleep_ns(uint64_t ns);
void scheduler_set_priority(thread_t *t, uint32_t new_prio);

/* Chiamati dal PIT: contabilita' del quanto + consolidamento switch. */
void scheduler_tick(void);
void scheduler_timer_drain_begin(void);
void scheduler_timer_drain_end(void);
/* Invalidazione del proprietario FPU (reap: mai falso match su
 * indirizzo riciclato dall'allocatore). */
void scheduler_fpu_owner_forget(struct thread *t);

/* Hook post-EOI: l'unico punto dove un IRQ puo' produrre uno switch. */
void scheduler_preempt_if_needed(void);

bool scheduler_selftest(void);

/* Idle: lavoro pendente su QUESTA CPU (ready_mask o need_resched)? */
bool scheduler_idle_has_work(void);

/* Blocco idle autonomo: pubblica lo stato idle di questa CPU sotto la
 * barriera simmetrica col waker, rilegge il lavoro e — se non ce n'e' —
 * dorme (sti;hlt fusi). Ritorna true se c'e' lavoro da schedulare (il
 * chiamante fa scheduler_yield), false dopo aver dormito. Unico punto in
 * cui una CPU si addormenta; usato da BSP e AP. */
bool scheduler_idle_block(void);

#endif
