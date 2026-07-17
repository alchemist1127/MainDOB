#ifndef MAINDOB_PROC_PERCPU_H
#define MAINDOB_PROC_PERCPU_H

#include "lib/types.h"
#include "arch/x86/gdt.h"   /* tss_entry_t */

struct thread;

/* Numero massimo di CPU logiche per cui si prenota stato per-CPU. */
#define MAX_CPUS 32

/* Blocco di stato per-CPU: tutto cio' che il kernel tiene separato per
 * ogni CPU logica. Un'istanza per CPU, in g_cpus[].
 *
 * `self` all'offset 0 e' deliberato: con GS = GDT_SEL_PERCPU caricato,
 * this_cpu() legge [gs:0] e ottiene questo puntatore direttamente
 * (fast path, vedi sotto).
 *
 * `current` e' il thread in esecuzione su QUESTA CPU: e' la
 * definizione di current_thread (proc/thread.h) sotto scheduler
 * pinnato per-CPU.
 *
 * `tss` e' il Task State Segment DI QUESTA CPU — il suo stack ring0
 * (esp0) viene ricaricato ad ogni dispatch, quindi serve uno per CPU. */
typedef struct percpu
{
    struct percpu  *self;        /* offset 0: punta a se stesso (per %gs) */
    struct thread  *current;     /* offset 4: thread corrente della CPU   */
    uint32_t        cpu_index;   /* 0 = boot CPU (BSP)                    */
    uint8_t         apic_id;     /* Local APIC ID di questa CPU           */
    tss_entry_t     tss;         /* TSS di questa CPU (stack ring0 qui)   */
    uint32_t        loaded_cr3;  /* CR3 attualmente caricato su questa CPU
                                  * (pubblicato dallo scheduler prima di
                                  * ogni switch): base dello shootdown
                                  * mirato per address-space              */
} percpu_t;

/* Tutti i blocchi per-CPU. g_cpus[0] e' la BSP. Azzerato in BSS (stato
 * iniziale corretto: self/current NULL, cpu_index 0, tss {0}), quindi
 * this_cpu() e' gia' usabile da gdt_init() prima di ogni init SMP. */
extern percpu_t g_cpus[MAX_CPUS];

#ifdef MAINDOB_SMP

/* Build SMP: il blocco della CPU su cui gira questo codice, risolto
 * mappando l'id del Local APIC corrente sullo slot in g_cpus[]. Prima
 * che l'SMP sia acceso (percpu_smp_init non ancora chiamata — e il
 * LAPIC forse non mappato) ritorna in sicurezza il blocco della BSP,
 * dato che allora gira solo lei. Definita in percpu.c. */
percpu_t *percpu_current(void);

void percpu_smp_init(void);

/* Sentinella per un id APIC che percpu_smp_init non ha mappato (piu' CPU
 * di MAX_CPUS, o un id che ACPI non ha mai riportato). Ogni valore
 * >= MAX_CPUS significa "nessuno slot"; i chiamanti DEVONO controllare
 * prima di indicizzare g_cpus. */
#define PERCPU_SLOT_INVALID 0xFFu

uint32_t percpu_slot_of_apic(uint8_t apic_id);

/* Quante volte this_cpu() e' ricaduta sulla lookup via LAPIC-id invece
 * del fast path %gs. Diagnostica di bring-up. */
uint32_t percpu_fallback_count(void);

static inline percpu_t *this_cpu(void)
{
    /* Fast path: se GS contiene il selettore per-CPU, [gs:0] e' il
     * self-pointer di QUESTA CPU. Si controlla il SELETTORE, non
     * [gs:0] direttamente: leggere il selettore (movw %gs) non fa mai
     * fault, mentre [gs:0] con un GS ancora flat (0x10, es. appena
     * entrati in un handler prima che il per-CPU GS sia caricato)
     * dereferenzierebbe l'indirizzo virtuale 0 — non mappato — e
     * fault. Se GS non e' GDT_SEL_PERCPU si ricade sulla lookup via
     * APIC id, autoritativa e sempre corretta. */
    uint16_t gs_sel;
    __asm__ volatile ("movw %%gs, %0" : "=r"(gs_sel));
    if (gs_sel == GDT_SEL_PERCPU)
    {
        percpu_t *p;
        __asm__ volatile ("movl %%gs:0, %0" : "=r"(p));
        if (p >= &g_cpus[0] && p < &g_cpus[MAX_CPUS] && p->self == p)
        {
            return p;
        }
    }
    return percpu_current();
}

#else /* !MAINDOB_SMP */

/* Build UP: l'unico blocco della BSP a indirizzo costante — zero
 * indirezione, stesso codegen dei globali bare che sostituisce. */
static inline percpu_t *this_cpu(void)
{
    return &g_cpus[0];
}

#endif /* MAINDOB_SMP */

#endif
