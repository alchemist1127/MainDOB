/* mainDOB SMP — bring-up delle application processor.
 * Avvia ogni CPU non-boot enabled. Ogni AP esegue il trampolino
 * (ap_trampoline.asm), atterra in ap_main, carica il proprio GDT/TSS/
 * base-%gs, punta l'IDTR alla IDT condivisa, porta su il proprio Local
 * APIC, abilita la propria FPU, si unisce all'insieme partecipante del
 * TLB shootdown, attiva gli interrupt e resta in un idle loop
 * indipendente. Una IPI di reschedule (BSP->AP) viene inviata e
 * confermata come self-test end-to-end del meccanismo IPI.
 *
 * Sicurezza cross-core: ogni AP scrive solo nel proprio slot (indicizzato
 * da cpu_index) delle tabelle di check-in; BSP e AP condividono solo
 * stato esplicitamente sincronizzato (i quattro campi patchati nella
 * pagina del trampolino, protetti dal gate di identita' + finestre
 * temporali dell'ack). */

#ifdef MAINDOB_SMP

#include "arch/x86/smp.h"
#include "arch/x86/lapic.h"
#include "arch/x86/gdt.h"
#include "arch/x86/idt.h"
#include "arch/x86/fpu.h"
#include "mm/paging.h"
#include "arch/x86/tsc.h"
#include "arch/x86/tlb.h"
#include "proc/thread.h"
#include "proc/scheduler.h"
#include "acpi/acpi.h"
#include "mm/kheap.h"
#include "mm/pmm.h"
#include "time/clock.h"
#include "lib/string.h"
#include "console/console.h"
#include "proc/percpu.h"
#include "sync/atomic.h"

/* Comandi ICR (dword basso) di INIT-SIPI-SIPI (destinazione fisica,
 * edge, assert):
 *   INIT    : modo di consegna 101 -> 0x4500
 *   STARTUP : modo di consegna 110 -> 0x4600, OR con il vettore-pagina */
#define ICR_INIT     0x00004500u
#define ICR_STARTUP  0x00004600u

#define AP_STACK_PAGES  4u   /* 16 KB di stack kernel per AP */

static volatile uint32_t g_ap_online;

/* Canale di check-in PER SLOT. Scritto dall'AP, letto dalla BSP; lo
 * store di checked_in e' l'ultimo e i campi sono allineati a 32 bit,
 * quindi il TSO di x86 ordina la pubblicazione. Uno slot per round:
 * ogni AP scrive solo il proprio, quindi un check-in tardivo non puo'
 * mai essere attribuito all'AP sbagliata. */
static volatile int32_t  g_ap_self_index[MAX_CPUS];
static volatile int32_t  g_ap_gs_ok[MAX_CPUS];
static volatile uint32_t g_ap_checked_in[MAX_CPUS];

/* Simboli da ap_trampoline.asm. Puntano nel template .rodata; servono
 * solo a misurarne la dimensione e localizzare gli slot di patch. */
extern uint8_t ap_trampoline_start[];
extern uint8_t ap_trampoline_end[];
extern uint8_t ap_param_pd[];
extern uint8_t ap_param_stack[];
extern uint8_t ap_param_apicid[];
extern uint8_t ap_param_ack[];

/* Gira su ogni AP, in meta' alta, sul proprio stack kernel. */
void ap_main(void)
{
    /* Risolve il proprio blocco per-CPU — passa dalla lookup via APIC id
     * (l'unico fallback atteso): il nostro %gs e' ancora il selettore
     * flat del trampolino, diventa quello per-CPU solo sotto. */
    percpu_t *me = percpu_current();
    uint32_t  slot = me->cpu_index;
    g_ap_self_index[slot] = (int32_t)slot;

    /* Carica il GDT/TSS/base-%gs DI QUESTA AP. Da qui this_cpu() prende
     * il fast path %gs su questa AP. */
    gdt_ap_init(slot);

    /* Self-test del fast path: legge %gs:0 direttamente e conferma che
     * risolve al nostro blocco. */
    percpu_t *via_gs;
    __asm__ volatile ("movl %%gs:0, %0" : "=r"(via_gs));
    g_ap_gs_ok[slot] = (via_gs == &g_cpus[slot]) ? 1 : 0;

    /* Punta l'IDTR alla IDT condivisa (la BSP ha gia' riempito ogni
     * gate, inclusi i vettori LAPIC). Poi porta su il Local APIC DI
     * QUESTA CPU (riusando la mappa MMIO + calibrazione della BSP), e
     * abilita la FPU di questa CPU. */
    idt_load_this_cpu();
    lapic_init_ap();
    fpu_init_this_cpu();

    /* Si unisce all'insieme partecipante del TLB shootdown: IDT/LAPIC
     * sono vivi e can ricevere la IPI di shootdown; tlb_ap_online prima
     * la propria generazione servita cosi' non dobbiamo mai un ack per
     * uno shootdown completato prima di unirci. */
    tlb_ap_online();

    /* Interrupt attivi: da qui possiamo ricevere IPI di reschedule (gia'
     * verificate dal self-test sotto) e la nostra LAPIC timer, quando
     * verra' armata (1.1.0.0.7). */
    __asm__ volatile ("sti");

    atomic_inc(&g_ap_online);
    g_ap_checked_in[slot] = 1;

    /* Sync TSC col riferimento (BSP): risponde ai round ping-pong e
     * ratifica l'offset nel proprio slot. DEVE stare qui — dopo il
     * check-in (la BSP guida il sync appena lo vede) e PRIMA dello
     * scheduler: da li' in poi l'offset e' immutabile e ogni lettura
     * del clock su questa CPU e' allineata alla BSP (vedi tsc.c). */
    tsc_smp_sync_ap(slot);

    /* Il flusso di bring-up diventa l'idle thread di questa CPU e l'AP
     * entra nello scheduler: da qui riceve thread pinnati via wake
     * cross-core (IPI di reschedule). Il fallback senza memoria per
     * l'idle e' il vecchio loop indipendente: la CPU resta fuori dallo
     * scheduling ma il sistema vive. */
    thread_t *idle = thread_bootstrap_idle_ap();
    if (idle != NULL)
    {
        scheduler_enter_ap(idle);       /* non ritorna */
    }
    kprintf("[SMP ] AP slot %u senza idle: fuori dallo scheduling.\n", slot);
    for (;;)
    {
        __asm__ volatile ("sti; hlt");
    }
}

/* Offset (fissi per l'intera sessione di bring-up) degli slot di
 * parametro dentro la pagina del trampolino, e i valori che ogni AP
 * condivide (id della BSP da saltare, vettore SIPI). Un solo posto dove
 * calcolarli, passati per valore ai verbi sotto — niente stato di file
 * mutabile in piu' del necessario. */
typedef struct
{
    uint32_t off_pd;
    uint32_t off_stack;
    uint32_t off_apicid;
    uint32_t off_ack;
    uint32_t bsp_id;
    uint8_t  vector;
} trampoline_layout_t;

/* === Verbi: preparazione una tantum ===================================== */

/* Mappa in identity la pagina del trampolino nel PD kernel (mai smontata
 * dopo, vedi commento originale su AP_TRAMPOLINE_PHYS), copia il blob
 * template, e ne patcha il campo costante (PD kernel). Ritorna il
 * layout degli offset, riusato da ogni AP nel loop. */
static trampoline_layout_t prepare_trampoline_page(void)
{
    paging_map_page(AP_TRAMPOLINE_PHYS, AP_TRAMPOLINE_PHYS,
                    PTE_PRESENT | PTE_WRITABLE);

    uint32_t len = (uint32_t)(ap_trampoline_end - ap_trampoline_start);
    memcpy((void *)AP_TRAMPOLINE_PHYS, ap_trampoline_start, len);

    trampoline_layout_t t;
    t.off_pd     = (uint32_t)(ap_param_pd     - ap_trampoline_start);
    t.off_stack  = (uint32_t)(ap_param_stack  - ap_trampoline_start);
    t.off_apicid = (uint32_t)(ap_param_apicid - ap_trampoline_start);
    t.off_ack    = (uint32_t)(ap_param_ack    - ap_trampoline_start);

    *(volatile uint32_t *)(AP_TRAMPOLINE_PHYS + t.off_pd) =
        paging_kernel_directory();

    /* Maschera a 8 bit: i campi apic_id della MADT sono a 8 bit e
     * percpu_smp_init chiavisce la sua mappa su (id & 0xFF). */
    t.bsp_id = lapic_get_id() & 0xFFu;
    t.vector = (uint8_t)(AP_TRAMPOLINE_PHYS >> 12);   /* 0x08 */
    return t;
}

/* === Verbi: le due attese con timeout della sequenza di bring-up ======== */

/* Fase 1: attende l'ACK del trampolino — l'AP ha caricato ESP e gli slot
 * di parametro condivisi sono consumati. Solo dopo (o dopo il timeout)
 * il chiamante puo' ripatcharli per il prossimo giro. */
static bool wait_trampoline_ack(uint32_t off_ack)
{
    uint64_t deadline = clock_now_ns() + 100ull * 1000 * 1000;
    while (*(volatile uint32_t *)(AP_TRAMPOLINE_PHYS + off_ack) == 0 &&
           clock_now_ns() < deadline)
    {
        __asm__ volatile ("pause");
    }
    return *(volatile uint32_t *)(AP_TRAMPOLINE_PHYS + off_ack) != 0;
}

/* Fase 2: attende (con limite) che l'AP raggiunga ap_main e si registri
 * sul PROPRIO flag di slot. */
static bool wait_checkin(uint32_t ap_slot)
{
    uint64_t deadline = clock_now_ns() + 100ull * 1000 * 1000;
    while (g_ap_checked_in[ap_slot] == 0 && clock_now_ns() < deadline)
    {
        __asm__ volatile ("pause");
    }
    return g_ap_checked_in[ap_slot] != 0;
}

/* === Verbi: validazione + self-test dopo un check-in riuscito =========== */

static void report_checkin(uint8_t apic_id, uint32_t ap_slot)
{
    if ((int32_t)ap_slot != g_ap_self_index[ap_slot])
    {
        kprintf("[SMP ] AP %u registrata ma this_cpu ha slot=%d, "
                "atteso %u!\n", apic_id, g_ap_self_index[ap_slot], ap_slot);
        return;
    }
    if (g_ap_gs_ok[ap_slot] != 1)
    {
        kprintf("[SMP ] AP %u registrata (slot %d) ma il fast path %%gs "
                "e' FALLITO (mismatch gs:0)!\n", apic_id,
                g_ap_self_index[ap_slot]);
        return;
    }
    kprintf("[SMP ] AP %u registrata (slot %d, idle indipendente; "
            "GDT+GS+IDT+LAPIC+FPU per-CPU vivi).\n",
            apic_id, g_ap_self_index[ap_slot]);
}

/* Self-test cross-core: l'AP e' in idle con interrupt attivi. Le si
 * invia una IPI di reschedule e si conferma che il suo handler gira (il
 * contatore globale avanza) — prova la consegna BSP->AP e che
 * IDT/LAPIC/handler dell'AP sono vivi. */
static void selftest_resched_ipi(uint8_t apic_id)
{
    uint32_t rc_before = lapic_resched_count();
    lapic_send_resched_ipi(apic_id);

    uint64_t deadline = clock_now_ns() + 50ull * 1000 * 1000;
    while (lapic_resched_count() == rc_before && clock_now_ns() < deadline)
    {
        __asm__ volatile ("pause");
    }

    if (lapic_resched_count() != rc_before)
    {
        kprintf("[SMP ] AP %u: IPI di reschedule cross-core OK.\n", apic_id);
    }
    else
    {
        kprintf("[SMP ] AP %u: IPI di reschedule cross-core NON "
                "confermata (timeout)!\n", apic_id);
    }
}

/* === Verbo: bring-up di UNA AP =========================================== */

/* Risolve lo slot, alloca lo stack, pubblica i parametri di questo
 * round, spara INIT-SIPI-SIPI, e attende le due fasi. Ritorna solo dopo
 * aver loggato l'esito (successo, timeout, o slot non disponibile) —
 * l'orchestratore non deve saper nulla dei dettagli del protocollo. */
static void boot_one_ap(const acpi_cpu_info_t *c, const trampoline_layout_t *t)
{
    /* Risolve lo slot PRIMA e lo valida. Un id non mappato cadrebbe
     * sullo slot 0 — l'AP adotterebbe il blocco per-CPU della BSP: due
     * CPU su un solo percpu_t. Si salta del tutto un'AP simile (mai un
     * SIPI a una CPU a cui non si puo' dare stato privato). */
    uint32_t ap_slot = percpu_slot_of_apic(c->apic_id);
    if (ap_slot == 0 || ap_slot >= (uint32_t)MAX_CPUS)
    {
        kprintf("[SMP ] AP %u: nessuno slot per-CPU (mappa piena o id "
                "non mappato), salto.\n", c->apic_id);
        return;
    }

    /* Stack kernel di questa AP. Il trampolino carica ESP solo dopo che
     * il paging e' acceso, quindi un indirizzo higher-half va bene. */
    uint32_t stk = kpages_alloc(AP_STACK_PAGES);
    if (!stk)
    {
        kprintf("[SMP ] AP %u: allocazione stack fallita, salto.\n",
                c->apic_id);
        return;
    }

    /* Pubblica i parametri di questo round nella pagina condivisa:
     * stack, il gate di identita' (una straggler di un round precedente
     * si parcheggia sul mismatch invece di consumarli), ack azzerato. */
    *(volatile uint32_t *)(AP_TRAMPOLINE_PHYS + t->off_stack) =
        stk + AP_STACK_PAGES * PAGE_SIZE;
    *(volatile uint32_t *)(AP_TRAMPOLINE_PHYS + t->off_apicid) = c->apic_id;
    *(volatile uint32_t *)(AP_TRAMPOLINE_PHYS + t->off_ack)    = 0;

    g_ap_self_index[ap_slot] = -1;
    g_ap_gs_ok[ap_slot]      = -1;
    g_ap_checked_in[ap_slot] = 0;

    /* INIT, poi due SIPI, coi ritardi architetturali. */
    lapic_send_ipi(c->apic_id, ICR_INIT);
    tsc_busy_wait_ns(10ull * 1000 * 1000);                       /* 10 ms */
    lapic_send_ipi(c->apic_id, ICR_STARTUP | t->vector);
    tsc_busy_wait_ns(200ull * 1000);                             /* 200 us*/
    lapic_send_ipi(c->apic_id, ICR_STARTUP | t->vector);

    if (!wait_trampoline_ack(t->off_ack))
    {
        /* Mai partita (o parcheggiata sul gate). Lo stack NON viene
         * liberato deliberatamente: una straggler potrebbe in teoria
         * consumarlo ancora — 16 KB persi una volta battono un
         * use-after-free su uno stack vivo. */
        kprintf("[SMP ] AP %u non partita (nessun ack del trampolino).\n",
                c->apic_id);
        return;
    }

    if (!wait_checkin(ap_slot))
    {
        kprintf("[SMP ] AP %u non si e' registrata (timeout).\n", c->apic_id);
        return;
    }

    /* Sync TSC: la controparte (tsc_smp_sync_ap) e' il primo passo
     * dell'AP dopo il check-in — la finestra di rendezvous e' questa.
     * Timeout su entrambi i lati: un sync mancato degrada a offset 0,
     * mai un boot bloccato. */
    tsc_smp_sync_bsp(ap_slot);

    report_checkin(c->apic_id, ap_slot);
    if (g_ap_self_index[ap_slot] == (int32_t)ap_slot && g_ap_gs_ok[ap_slot] == 1)
    {
        selftest_resched_ipi(c->apic_id);
    }
}

/* === Orchestratore ======================================================= */

void smp_boot_aps(void)
{
    uint32_t total   = acpi_cpu_count();
    uint32_t enabled = acpi_cpu_enabled_count();
    if (enabled <= 1)
    {
        kprintf("[SMP ] Uniprocessore; nessuna AP da avviare.\n");
        return;
    }

    /* L'avvio di un'AP e' impossibile senza Local APIC: INIT-SIPI-SIPI
     * passa dall'ICR del LAPIC. Su hardware legacy senza (CPUF_APIC
     * assente) si degrada a uniprocessore sulla BSP. */
    if (!lapic_available())
    {
        kprintf("[SMP ] Nessun Local APIC; resto uniprocessore "
                "(l'avvio delle AP richiede il LAPIC).\n");
        return;
    }

    /* Costruisce la mappa apic-id -> slot e arma la risoluzione per-CPU
     * PRIMA che qualunque AP parta, cosi' percpu_current() funziona
     * nell'istante in cui un'AP raggiunge ap_main. La BSP resta slot 0. */
    percpu_smp_init();

    /* Registra la BSP come partecipante al TLB shootdown prima che
     * qualunque AP si unisca, cosi' la maschera partecipante e' corretta
     * dall'istante in cui la prima AP la raggiunge. */
    tlb_bsp_online();

    trampoline_layout_t t = prepare_trampoline_page();

    for (uint32_t i = 0; i < total; i++)
    {
        acpi_cpu_info_t c;
        if (!acpi_cpu_get(i, &c) || !c.enabled || c.apic_id == t.bsp_id)
        {
            continue;
        }
        boot_one_ap(&c, &t);
    }

    kprintf("[SMP ] %u di %u AP online.\n",
            atomic_read(&g_ap_online), enabled - 1);
    kprintf("[SMP ] fast path this_cpu vivo; %u fallback durante il "
            "bring-up (atteso ~1 per AP: la propria risoluzione).\n",
            percpu_fallback_count());
}

#else /* !MAINDOB_SMP — build uniprocessore: nessuna AP da avviare */

#include "arch/x86/smp.h"

/* ap_main esiste solo per soddisfare l'extern di ap_trampoline.asm; il
 * trampolino non viene mai copiato ne' eseguito in una build UP. */
void ap_main(void)      { for (;;) __asm__ volatile ("hlt"); }
void smp_boot_aps(void) { /* build uniprocessore: niente da fare */ }

#endif /* MAINDOB_SMP */
