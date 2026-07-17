#include "arch/x86/isr.h"
#include "arch/x86/idt.h"
#include "arch/x86/gdt.h"
#include "arch/x86/cpu.h"
#include "proc/process.h"
#include "proc/thread.h"
#include "proc/scheduler.h"
#include "proc/workqueue.h"
#include "ipc/channel.h"
#include "krt/uaccess.h"
#include "mm/kheap.h"
#include "console/console.h"
#include "kernel.h"
#include "syscall/video_boomerang.h"

static isr_handler_t s_handlers[256];
static void (*s_syscall_dispatch)(isr_regs_t *regs);

static const char *const s_exception_names[32] =
{
    "Divide error",         "Debug",                "NMI",
    "Breakpoint",           "Overflow",             "BOUND range",
    "Invalid opcode",       "Device not available", "Double fault",
    "Coprocessor overrun",  "Invalid TSS",          "Segment not present",
    "Stack fault",          "General protection",   "Page fault",
    "Reserved 15",          "x87 FP error",         "Alignment check",
    "Machine check",        "SIMD FP error",        "Virtualization",
    "Control protection",   "Reserved 22",          "Reserved 23",
    "Reserved 24",          "Reserved 25",          "Reserved 26",
    "Reserved 27",          "Hypervisor injection", "VMM communication",
    "Security exception",   "Reserved 31"
};

/* Dichiarazione degli stub assembly (isr_stubs.asm). */
#define DECL_STUB(n) extern void isr_stub_##n(void);
#define STUB_ADDR(n) ((uint32_t)isr_stub_##n)

DECL_STUB(0)  DECL_STUB(1)  DECL_STUB(2)  DECL_STUB(3)
DECL_STUB(4)  DECL_STUB(5)  DECL_STUB(6)  DECL_STUB(7)
DECL_STUB(8)  DECL_STUB(9)  DECL_STUB(10) DECL_STUB(11)
DECL_STUB(12) DECL_STUB(13) DECL_STUB(14) DECL_STUB(15)
DECL_STUB(16) DECL_STUB(17) DECL_STUB(18) DECL_STUB(19)
DECL_STUB(20) DECL_STUB(21) DECL_STUB(22) DECL_STUB(23)
DECL_STUB(24) DECL_STUB(25) DECL_STUB(26) DECL_STUB(27)
DECL_STUB(28) DECL_STUB(29) DECL_STUB(30) DECL_STUB(31)
DECL_STUB(32) DECL_STUB(33) DECL_STUB(34) DECL_STUB(35)
DECL_STUB(36) DECL_STUB(37) DECL_STUB(38) DECL_STUB(39)
DECL_STUB(40) DECL_STUB(41) DECL_STUB(42) DECL_STUB(43)
DECL_STUB(44) DECL_STUB(45) DECL_STUB(46) DECL_STUB(47)

/* === Verbi ============================================================== */

static void install_stub_gates(void)
{
    static const uint32_t stubs[48] =
    {
        STUB_ADDR(0),  STUB_ADDR(1),  STUB_ADDR(2),  STUB_ADDR(3),
        STUB_ADDR(4),  STUB_ADDR(5),  STUB_ADDR(6),  STUB_ADDR(7),
        STUB_ADDR(8),  STUB_ADDR(9),  STUB_ADDR(10), STUB_ADDR(11),
        STUB_ADDR(12), STUB_ADDR(13), STUB_ADDR(14), STUB_ADDR(15),
        STUB_ADDR(16), STUB_ADDR(17), STUB_ADDR(18), STUB_ADDR(19),
        STUB_ADDR(20), STUB_ADDR(21), STUB_ADDR(22), STUB_ADDR(23),
        STUB_ADDR(24), STUB_ADDR(25), STUB_ADDR(26), STUB_ADDR(27),
        STUB_ADDR(28), STUB_ADDR(29), STUB_ADDR(30), STUB_ADDR(31),
        STUB_ADDR(32), STUB_ADDR(33), STUB_ADDR(34), STUB_ADDR(35),
        STUB_ADDR(36), STUB_ADDR(37), STUB_ADDR(38), STUB_ADDR(39),
        STUB_ADDR(40), STUB_ADDR(41), STUB_ADDR(42), STUB_ADDR(43),
        STUB_ADDR(44), STUB_ADDR(45), STUB_ADDR(46), STUB_ADDR(47)
    };

    for (uint8_t v = 0; v < 48; v++)
    {
        idt_set_gate(v, stubs[v], GDT_SEL_KCODE, IDT_FLAG_INT_KERNEL);
    }
}

static void report_fatal_exception(isr_regs_t *regs)
{
    const char *name = (regs->vector < 32)
                     ? s_exception_names[regs->vector]
                     : "sconosciuta";

    if (regs->vector == 14)
    {
        kpanic("Eccezione %u (%s) err=0x%x eip=0x%08x cr2=0x%08x",
               regs->vector, name, regs->error_code,
               regs->eip, cpu_read_cr2());
    }
    kpanic("Eccezione %u (%s) err=0x%x eip=0x%08x",
           regs->vector, name, regs->error_code, regs->eip);
}

/* === Gestione delle eccezioni CPU ======================================= *
 *
 * In un microkernel il fault di un processo NON deve fermare il sistema:
 * la CPU isola gia' i processi, e il kernel deve reagire terminando il
 * solo colpevole e proseguendo. Solo un fault del kernel stesso, senza
 * un processo utente a cui imputarlo, e' fatale.
 *
 * Orchestratore fault_handler + verbi subordinati, una responsabilita'
 * ciascuno:
 *   - fault_is_recoverable_kernel_pde: PDE kernel mancante -> no (nel
 *     1.1 le PDE kernel sono pre-allocate e immutabili, quindi non
 *     accade mai; il verbo documenta l'invariante e resta il punto
 *     unico se un domani servisse un fixup).
 *   - fault_kill_current_process: imputa il fault al processo corrente,
 *     lo marca zombie, sveglia chi lo attende, e rimanda la bonifica
 *     completa (porte, IRQ, claim, DMA, timer, watchdog, registry) al
 *     canale intrusivo non-perdibile (teardown_q via
 *     process_destroy_deferred), fuori dal contesto ISR. Poi cede la
 *     CPU: non si torna.
 *   - fault_panic: fault kernel puro, nessun colpevole utente. Fatale.
 * ==================================================================== */

static void fault_log(isr_regs_t *regs, const char *where,
                      process_t *proc, thread_t *t, uint32_t cr2)
{
    const char *name = (regs->vector < 32)
                     ? s_exception_names[regs->vector] : "sconosciuta";
    kprintf("[FAULT] %s: %s in '%s' (PID %d, TID %d)\n",
            where, name,
            proc ? proc->name : "?",
            proc ? proc->pid : -1,
            t ? (int)t->tid : -1);
    if (regs->vector == 14)
    {
        kprintf("[FAULT]   cr2=0x%08x eip=0x%08x %s %s (ring %d)\n",
                cr2, regs->eip,
                (regs->error_code & 0x1) ? "protezione" : "non-presente",
                (regs->error_code & 0x2) ? "scrittura" : "lettura",
                (regs->error_code & 0x4) ? 3 : 0);
    }
    else
    {
        kprintf("[FAULT]   eip=0x%08x err=0x%x\n",
                regs->eip, regs->error_code);
    }
}

/* Termina il processo corrente come colpevole del fault e cede la CPU.
 * Non ritorna: il thread e' morto quando questa funzione cede. */
static void fault_kill_current_process(isr_regs_t *regs, uint32_t cr2,
                                       const char *where)
{
    thread_t  *t    = current_thread;
    process_t *proc = process_current();

    fault_log(regs, where, proc, t, cr2);

    if (proc != NULL && proc->pid > 0)
    {
        /* Termina il colpevole SENZA distruggerlo qui (giriamo sul suo
         * stesso contesto): fissa l'esito e differisce il teardown sul
         * canale intrusivo non-perdibile. -11 = equivalente SIGSEGV.
         * Niente kmalloc, niente workqueue perdibile. */
        process_destroy_deferred(proc, -11);
        kprintf("[FAULT]   processo terminato, il kernel prosegue.\n");
    }

    if (t != NULL)
    {
        ipc_cleanup_thread(t->tid);
        t->state = THREAD_DEAD;             /* yield DEVE lasciare la CPU */
        scheduler_remove(t);
    }
    scheduler_yield();                       /* non ritorna                */
    for (;;)
    {
        __asm__ volatile ("hlt");
    }
}

static void fault_handler(isr_regs_t *regs)
{
    uint32_t cr2 = (regs->vector == 14) ? cpu_read_cr2() : 0;

    /* Fault col boomerang video in volo su QUESTA CPU: il
     * colpevole e' la fase in-driver (dispatcher ring-3 di bga o copie
     * ring-0 in CR3 driver), non il processo corrente — che e' il
     * CHIAMANTE parcheggiato (modello migrating-thread). Il recupero
     * libera g_boomerang_lock, ripristina CR3 e frame del chiamante in
     * *regs con rc d'errore, e l'epilogo dello stub lo fa ripartire.
     * Senza: chiamante ucciso al posto del colpevole + lock leakato ->
     * ogni dv_* successiva spinna a ring 0 con IF=0 -> freeze totale. */
    if (video_boomerang_fault_recover(regs))
    {
        return;
    }

    /* Copia userspace fault-safe: se il #PF ring-0 e' su un'istruzione di
     * copia registrata in .ex_table (TOCTOU: pagina validata sparita a
     * meta'), recupera facendo fallire la copia — invece di uccidere. Deve
     * stare PRIMA del ramo che imputa i fault ring-0 al processo. */
    uint32_t fixup = uaccess_fault_recover(regs->vector, regs->cs, regs->eip);
    if (fixup != 0)
    {
        regs->eip = fixup;
        return;
    }

    /* Fault da ring 3: colpa del processo utente. Terminalo, prosegui.
     * Il #PF codifica il ring nel bit U dell'error code; le altre
     * eccezioni nel RPL di CS. */
    bool from_user = (regs->vector == 14)
                   ? ((regs->error_code & 0x4) != 0)
                   : ((regs->cs & 0x3) == 3);
    if (from_user)
    {
        fault_kill_current_process(regs, cr2, "utente");
        return;                             /* irraggiungibile            */
    }

    /* Fault da ring 0 ma con un processo utente in esecuzione: quasi
     * sempre un puntatore utente non validato passato a una syscall. Il
     * colpevole e' quel processo: terminalo e prosegui, invece di
     * abbattere tutto il sistema per il bug di un driver. */
    if ((regs->cs & 0x3) == 0 && process_current() != NULL &&
        process_current()->pid > 0)
    {
        fault_kill_current_process(regs, cr2, "kernel per conto utente");
        return;                             /* irraggiungibile            */
    }

    /* Fault kernel puro: nessun colpevole utente. Questo, in un
     * microkernel, non deve accadere — ed e' l'unico caso fatale. */
    report_fatal_exception(regs);
}

/* === Dispatcher (chiamato dagli stub) =================================== */

void isr_dispatch(isr_regs_t *regs);

void isr_dispatch(isr_regs_t *regs)
{
    if (regs->vector == 0x80)
    {
        if (s_syscall_dispatch != NULL)
        {
            s_syscall_dispatch(regs);
        }
    }
    else
    {
        isr_handler_t handler = s_handlers[regs->vector];

        if (handler != NULL)
        {
            handler(regs);
        }
        else if (regs->vector < 32)
        {
            fault_handler(regs);
        }
        /* IRQ senza handler: ignorato (l'EOI e' compito del layer irq). */
    }

    /* Epilogo comune: OGNI ingresso nel kernel onora need_resched
     * all'USCITA. Se la preemption vivesse solo nel tick LAPIC e nella
     * IPI di resched, un wake eseguito sulla stessa CPU (reply IPC in
     * syscall, post da un handler IRQ) alzerebbe need_resched senza che
     * nessuno lo guardi fino al tick successivo — che in un sistema
     * tickless quieto e' la prossima scadenza globale (secondi) o MAI
     * ad agenda vuota. Idempotente coi percorsi che gia' preemptano
     * (need_resched si consuma); le guardie s_active/crit_depth/
     * in_timer_drain vivono dentro scheduler_preempt_if_needed. */
    scheduler_preempt_if_needed();
}

/* === API ================================================================ */

void isr_init(void)
{
    install_stub_gates();
    kprintf("[ISR ] Eccezioni 0-31 e IRQ 32-47 agganciati.\n");
}

void isr_register_handler(uint8_t vector, isr_handler_t handler)
{
    s_handlers[vector] = handler;
}

void isr_register_syscall(void (*dispatch)(isr_regs_t *regs))
{
    s_syscall_dispatch = dispatch;
}
