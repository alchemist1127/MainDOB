/* Local APIC — init, calibrazione, arming. Registri MMIO su
 * IA32_APIC_BASE (~0xFEE00000, mappati PTE_CACHE_DISABLE), accessi solo
 * via helper a 32 bit naturalmente allineati (una scrittura disallineata
 * corromperebbe i registri vicini). La parte one-shot va calibrata
 * (divisore 16... qui usiamo divisore 1, conteggio da 0xFFFFFFFF, ~10ms
 * di finestra TSC -> lapic_bus_hz); la parte TSC-deadline arma con una
 * sola scrittura MSR. */

#include "arch/x86/lapic.h"
#include "arch/x86/cpu.h"
#include "arch/x86/cpu_features.h"
#include "arch/x86/msr.h"
#include "arch/x86/tsc.h"
#include "arch/x86/idt.h"
#include "arch/x86/gdt.h"
#include "arch/x86/isr.h"
#include "mm/mmio_map.h"
#include "mm/pmm.h"        /* PAGE_SIZE */
#include "console/console.h"
#include "kernel.h"

#ifdef MAINDOB_SMP
#include "arch/x86/tlb.h"
#endif

/* === Offset dei registri (dalla base MMIO) === */
#define LAPIC_REG_ID            0x020
#define LAPIC_REG_TPR            0x080   /* Task Priority                */
#define LAPIC_REG_EOI            0x0B0
#define LAPIC_REG_SVR            0x0F0   /* Spurious Interrupt Vector    */
#define LAPIC_REG_LVT_CMCI       0x2F0
#define LAPIC_REG_ICR_LO         0x300
#define LAPIC_REG_ICR_HI         0x310
#define LAPIC_REG_LVT_TIMER      0x320
#define LAPIC_REG_LVT_THERMAL    0x330
#define LAPIC_REG_LVT_PERFCNT    0x340
#define LAPIC_REG_LVT_LINT0      0x350
#define LAPIC_REG_LVT_LINT1      0x360
#define LAPIC_REG_LVT_ERROR      0x370
#define LAPIC_REG_TIMER_INIT     0x380
#define LAPIC_REG_TIMER_CUR      0x390
#define LAPIC_REG_TIMER_DCR      0x3E0

/* === Campi di bit === */
#define LAPIC_SVR_ENABLE           (1u << 8)

#define LAPIC_LVT_DELIVERY_FIXED   (0u << 8)
#define LAPIC_LVT_DELIVERY_NMI     (4u << 8)
#define LAPIC_LVT_DELIVERY_EXTINT  (7u << 8)
#define LAPIC_LVT_MASKED           (1u << 16)
#define LAPIC_LVT_TIMER_ONE_SHOT   (0u << 17)
#define LAPIC_LVT_TIMER_TSC_DEADLN (2u << 17)

/* Divisore 1 (bit 3,1,0 = 1011): dodge quirk degli emulatori con
 * divisori non banali, semantica piu' semplice. */
#define LAPIC_DCR_DIV_1  0xBu

#define LAPIC_DEFAULT_PHYS_BASE  0xFEE00000u

/* x2APIC: un registro MMIO xAPIC all'offset `off` mappa su MSR
 * 0x800 + (off>>4) (eccetto l'ICR, gestito a parte). */
#define X2APIC_MSR(off)  (0x800u + ((off) >> 4))

/* === Stato === */
static volatile uint32_t *s_lapic_mmio;
static uint64_t           s_bus_hz;
static bool               s_use_tscd;
static bool               s_x2apic;

static uint64_t s_bus_cycles_per_ns_q32;
static uint64_t s_max_arm_delta_ns = 1000000000ull;
#define LAPIC_TSCD_MAX_ARM_DELTA_NS  (3600ull * 1000000000ull)

/* === Accesso ai registri === */

static inline uint32_t lapic_read(uint32_t off)
{
    if (s_x2apic) return (uint32_t)rdmsr(X2APIC_MSR(off));
    return s_lapic_mmio[off / 4];
}

static inline void lapic_write(uint32_t off, uint32_t val)
{
    if (s_x2apic) { wrmsr(X2APIC_MSR(off), (uint64_t)val); return; }
    s_lapic_mmio[off / 4] = val;
}

/* Calibrazione della frequenza di bus (solo percorso one-shot). Usa il
 * TSC gia' calibrato come orologio: attesa attiva ~10ms su RDTSC,
 * lettura del conteggio corrente LAPIC a inizio/fine, scala per il
 * divisore. Il chiamante DEVE avere gli interrupt disattivati. */
static uint64_t calibrate_bus_hz(void)
{
    lapic_write(LAPIC_REG_TIMER_DCR, LAPIC_DCR_DIV_1);
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED | LAPIC_LVT_TIMER_ONE_SHOT);
    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFFu);

    const uint64_t window_ns = 10000000ull;   /* 10 ms                   */
    uint64_t start_ns = tsc_now_ns();
    uint64_t deadline  = start_ns + window_ns;
    while (tsc_now_ns() < deadline)
    {
        __asm__ volatile ("pause");
    }

    uint32_t cur = lapic_read(LAPIC_REG_TIMER_CUR);
    uint64_t actual_ns = tsc_now_ns() - start_ns;

    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED | LAPIC_LVT_TIMER_ONE_SHOT);
    lapic_write(LAPIC_REG_TIMER_INIT, 0);

    if (cur == 0)
    {
        kpanic("lapic: il timer ha raggiunto il conteggio terminale nella "
               "calibrazione di 10ms (cur=0). Bus troppo veloce o MMIO "
               "LAPIC rotta.");
    }

    uint64_t delta_counts = 0xFFFFFFFFu - cur;
    return (delta_counts * 1000000000ull) / actual_ns;
}

/* === API pubblica === */

void lapic_eoi(void)
{
    lapic_write(LAPIC_REG_EOI, 0);
}

uint32_t lapic_get_id(void)
{
    if (s_x2apic) return (uint32_t)rdmsr(MSR_X2APIC_APICID);
    return lapic_read(LAPIC_REG_ID) >> 24;
}

bool lapic_available(void)
{
    return s_x2apic || (s_lapic_mmio != NULL);
}

/* Modo fisso, livello asserito (bit 14) — combacia con la codifica
 * INIT/STARTUP di smp.c (0x4500/0x4600), modo di consegna 000. */
#define ICR_FIXED 0x00004000u

void lapic_send_ipi(uint8_t apic_id, uint32_t icr_lo)
{
    /* Cintura no-LAPIC (Armada-class): oggi nessun percorso arriva qui
     * senza LAPIC (smp_boot_aps e' no-op, i kick vanno solo a CPU
     * online != questa), ma una deref di s_lapic_mmio NULL sarebbe un
     * fault muto a ring 0 — il costo della guardia e' un confronto. */
    if (!lapic_available()) return;

    if (s_x2apic)
    {
        /* x2APIC: una sola scrittura MSR a 64 bit porta la destinazione
         * a 32 bit nella dword alta. Auto-sincronizzante — niente bit di
         * stato consegna da attendere (rimosso in x2APIC). */
        wrmsr(MSR_X2APIC_ICR, ((uint64_t)apic_id << 32) | (uint64_t)icr_lo);
        return;
    }

    /* xAPIC: ICR_HI (destinazione) e ICR_LO (comando, la cui scrittura
     * spara davvero) sono due registri MMIO separati. La coppia DEVE
     * essere atomica rispetto agli interrupt: un handler che interpone
     * un'altra IPI tra le due scritture sovrascriverebbe ICR_HI e la
     * NOSTRA IPI finirebbe sulla CPU sbagliata. cli/restore, non un
     * lock: ogni core ha il proprio LAPIC/ICR, quindi non c'e' nulla da
     * serializzare fra core — solo da proteggere dall'interposizione di
     * un IRQ locale. */
    uint32_t flags;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags));
    lapic_write(LAPIC_REG_ICR_HI, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LO, icr_lo);

    /* Il bit di stato consegna si azzera in microsecondi. Se la
     * destinazione non lo accetta mai (APIC inesistente o incastrata),
     * uno spin infinito qui SAREBBE un blocco totale di questo core con
     * IF=0: si limita l'attesa e si degrada con un log — ogni
     * consumatore di IPI tollera una IPI persa. */
    uint32_t spins = 0;
    while (lapic_read(LAPIC_REG_ICR_LO) & (1u << 12))
    {
        __asm__ volatile ("pause");
        if (spins++ > (1u << 24))
        {
            kprintf("[APIC] IPI a apic_id %u mai accettata "
                    "(ICR_LO %08x) - scartata.\n", apic_id, icr_lo);
            break;
        }
    }
    __asm__ volatile ("push %0; popf" : : "r"(flags) : "memory", "cc");
}

/* === Dispatch di arming ===
 * Le due implementazioni e il puntatore a funzione stanno sotto la
 * logica di init, cosi' la sequenza di boot si legge dall'alto in basso. */
typedef void (*lapic_arm_fn_t)(uint64_t deadline_ns,
                               uint64_t now_cycles, uint64_t now_ns);
static lapic_arm_fn_t s_arm_fn;
static void lapic_arm_tscd(uint64_t deadline_ns, uint64_t now_cycles, uint64_t now_ns);
static void lapic_arm_oneshot(uint64_t deadline_ns, uint64_t now_cycles, uint64_t now_ns);

static lapic_timer_cb_t s_timer_cb;

void lapic_register_timer_callback(lapic_timer_cb_t cb)
{
    s_timer_cb = cb;
}

static void lapic_timer_irq(isr_regs_t *regs)
{
    (void)regs;
    lapic_eoi();
    if (s_timer_cb) s_timer_cb();
}

bool lapic_timer_usable(void)
{
    return s_arm_fn != NULL && (s_use_tscd || s_bus_hz != 0);
}

bool lapic_timer_is_tsc_deadline(void)
{
    return s_use_tscd;
}

void lapic_timer_arm_ns(uint64_t deadline_ns)
{
    if (s_arm_fn == NULL)
    {
        return;
    }
    /* Coppia (cicli, ns) COERENTE dallo stesso snapshot — e in cicli
     * CORRETTI, come da contratto. Il vecchio tsc_read() qui era il
     * TSC grezzo accanto a ns derivati: innocuo finche' i due mondi
     * coincidevano, sbagliato dall'introduzione dell'offset di sync. */
    tsc_snapshot_t snap;
    tsc_snapshot(&snap);
    s_arm_fn(deadline_ns, snap.cycles, snap.ns);
}

void lapic_timer_disarm(void)
{
    if (s_use_tscd)
    {
        wrmsr(MSR_IA32_TSC_DEADLINE, 0);    /* 0 = disarmato per spec */
    }
    else
    {
        lapic_write(LAPIC_REG_TIMER_INIT, 0);
    }
}

static void lapic_spurious_irq(isr_regs_t *regs)
{
    (void)regs;
    /* Spurious NON richiede EOI per spec. Nessun log: amplificherebbe
     * un bug genuino che ne producesse molti; restiamo silenziosi. */
}

#ifdef MAINDOB_SMP
static void (*s_resched_cb)(void);
static volatile uint32_t s_resched_count;

void lapic_register_resched_callback(void (*cb)(void))
{
    s_resched_cb = cb;
}

uint32_t lapic_resched_count(void)
{
    return s_resched_count;
}

static void lapic_resched_irq(isr_regs_t *regs)
{
    (void)regs;
    lapic_eoi();
    __asm__ volatile ("lock incl %0" : "+m"(s_resched_count) : : "memory");
    if (s_resched_cb) s_resched_cb();
}

void lapic_send_resched_ipi(uint8_t apic_id)
{
    lapic_send_ipi(apic_id, ICR_FIXED | LAPIC_VECTOR_RESCHED);
}

void lapic_send_tlb_ipi(uint8_t apic_id)
{
    lapic_send_ipi(apic_id, ICR_FIXED | LAPIC_VECTOR_TLB);
}

static void lapic_tlb_irq(isr_regs_t *regs)
{
    (void)regs;
    lapic_eoi();
    tlb_on_ipi();
}
#endif /* MAINDOB_SMP */

/* Stub assembly generici (isr_stubs.asm, macro ISR_NOERR — D9: nessun
 * asm dedicato per il LAPIC, si riusa lo stesso generatore di ogni
 * altro vettore). */
extern void isr_stub_240(void);   /* LAPIC_VECTOR_TIMER   */
extern void isr_stub_241(void);   /* LAPIC_VECTOR_RESCHED */
extern void isr_stub_242(void);   /* LAPIC_VECTOR_TLB     */
extern void isr_stub_255(void);   /* LAPIC_VECTOR_SPURIOUS*/

/* Programma il LAPIC della CPU chiamante: SVR software-enable + LVT +
 * TPR. Condivisa da lapic_init (BSP) e lapic_init_ap. Il vettore
 * spurious vuole i 4 bit bassi settati (quirk P6; 0xFF li soddisfa).
 * LINT1=NMI, timer mascherato one-shot, resto mascherato. TPR=0 (si
 * gate con IF globale). lint0_value e' l'unica differenza per ruolo: la
 * BSP passa ExtINT (filo virtuale verso l'8259), un'AP passa MASKED (il
 * PIC e' cablato solo sulla BSP). */
static void lapic_program_local(uint32_t lint0_value)
{
    lapic_write(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | LAPIC_VECTOR_SPURIOUS);

    lapic_write(LAPIC_REG_LVT_LINT0,   lint0_value);
    lapic_write(LAPIC_REG_LVT_LINT1,   LAPIC_LVT_DELIVERY_NMI);
    lapic_write(LAPIC_REG_LVT_TIMER,   LAPIC_LVT_MASKED | LAPIC_LVT_TIMER_ONE_SHOT);
    lapic_write(LAPIC_REG_LVT_ERROR,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_PERFCNT, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_CMCI,    LAPIC_LVT_MASKED);

    lapic_write(LAPIC_REG_TPR, 0);
}

#ifdef MAINDOB_SMP
/* Bring-up per-core per un'AP. La BSP (lapic_init) ha gia': mappato la
 * finestra MMIO LAPIC (fisica, condivisa da tutti i core), registrato i
 * gate IDT (la IDT e' condivisa), scelto e calibrato la modalita' timer
 * (s_arm_fn/s_bus_hz/s_max_arm_delta_ns sono globali e uniformi sul
 * modello). Quindi l'AP si limita ad abilitare via software IL PROPRIO
 * LAPIC e programmare le proprie LVT/TPR — nessun remap MMIO, nessuna
 * ricalibrazione. Deve girare dopo che l'AP e' sul proprio GDT e ha
 * caricato la IDT. */
void lapic_init_ap(void)
{
    uint64_t base = rdmsr(MSR_IA32_APIC_BASE);
    if (!(base & APIC_BASE_GLOBAL_ENABLE))
    {
        base |= APIC_BASE_GLOBAL_ENABLE;
        wrmsr(MSR_IA32_APIC_BASE, base);
    }
    if (s_x2apic && !(base & APIC_BASE_X2APIC))
    {
        base |= APIC_BASE_X2APIC;
        wrmsr(MSR_IA32_APIC_BASE, base);
    }

    lapic_program_local(LAPIC_LVT_MASKED);   /* LINT0 mascherato: il PIC e' della BSP */

    if (!s_use_tscd)
    {
        lapic_write(LAPIC_REG_TIMER_DCR, LAPIC_DCR_DIV_1);
    }
    uint32_t lvt_mode = s_use_tscd ? LAPIC_LVT_TIMER_TSC_DEADLN
                                   : LAPIC_LVT_TIMER_ONE_SHOT;
    lapic_write(LAPIC_REG_LVT_TIMER, lvt_mode | LAPIC_VECTOR_TIMER);
}
#endif

void lapic_init(void)
{
    /* === Passo 1: individua il LAPIC (presenza, poi xAPIC vs x2APIC) ===
     * La presenza e' la prima cosa che il bring-up deve stabilire, non un
     * caso d'eccezione da filtrare a parte. cpu_features_init() ha gia'
     * deciso via CPUID.01h:EDX.9 e ha tentato la resurrezione di un LAPIC
     * solo-BIOS-disabled (scrittura IA32_APIC_BASE + ri-sonda CPUID). Se al
     * termine ne' xAPIC ne' x2APIC risultano presenti, il bring-up non ha
     * oggetto: e' una configurazione legittima (QEMU -cpu ...,-apic, silicio
     * fuse-disabled), non un errore. Il kernel resta in modo PIC 8259 — che
     * raggiunge il core dal pin INTR, quindi senza il virtual-wire LINT0 dei
     * passi seguenti — e lo stato lasciato qui (s_lapic_mmio=NULL,
     * s_x2apic=false) e' cio' che rende lapic_available() falso: da li'
     * tick_source_select() scende sul PIT one-shot e smp_boot_aps() resta
     * uniprocessore, ciascuno per decisione propria. Distinguere qui evita
     * anche di mappare una finestra MMIO morta e far leggere cur=0 alla
     * calibrazione (kpanic a torto). */
    s_x2apic = cpu_has(CPUF_X2APIC);
    if (!cpu_has(CPUF_APIC) && !s_x2apic)
    {
        s_lapic_mmio = NULL;
        kprintf("[APIC] Nessun Local APIC (CPUID): modo PIC 8259, tick su PIT.\n");
        return;
    }

    /* LAPIC presente: rileva la base e affina la scelta xAPIC/x2APIC con il
     * bit dell'MSR (la vista CPUID sopra ne e' il floor), poi abilita. */
    uint64_t base = rdmsr(MSR_IA32_APIC_BASE);
    s_x2apic = (base & APIC_BASE_X2APIC) || s_x2apic;

    uint32_t phys_base = 0;

    if (!(base & APIC_BASE_GLOBAL_ENABLE))
    {
        base |= APIC_BASE_GLOBAL_ENABLE;
        wrmsr(MSR_IA32_APIC_BASE, base);
    }

    if (s_x2apic)
    {
        if (!(base & APIC_BASE_X2APIC))
        {
            base |= APIC_BASE_X2APIC;
            wrmsr(MSR_IA32_APIC_BASE, base);
        }
    }
    else
    {
        phys_base = (uint32_t)(base & APIC_BASE_ADDR_MASK);
        if (phys_base == 0) phys_base = LAPIC_DEFAULT_PHYS_BASE;

        uint32_t virt, pages;
        void *w = mmio_map(phys_base & 0xFFFFF000u, PAGE_SIZE, true,
                          &virt, &pages);
        if (!w)
        {
            kpanic("lapic: spazio virtuale kernel esaurito");
        }
        s_lapic_mmio = (volatile uint32_t *)w;
    }

    /* === Passo 2: registra i gate IDT — PRIMA di abilitare via
     * software, cosi' un LAPIC IRQ imprevisto non puo' mai atterrare
     * su un vettore senza handler. === */
    idt_set_gate(LAPIC_VECTOR_TIMER,    (uint32_t)isr_stub_240,
                GDT_SEL_KCODE, IDT_FLAG_INT_KERNEL);
    idt_set_gate(LAPIC_VECTOR_RESCHED,  (uint32_t)isr_stub_241,
                GDT_SEL_KCODE, IDT_FLAG_INT_KERNEL);
#ifdef MAINDOB_SMP
    idt_set_gate(LAPIC_VECTOR_TLB,      (uint32_t)isr_stub_242,
                GDT_SEL_KCODE, IDT_FLAG_INT_KERNEL);
#endif
    idt_set_gate(LAPIC_VECTOR_SPURIOUS, (uint32_t)isr_stub_255,
                GDT_SEL_KCODE, IDT_FLAG_INT_KERNEL);

    isr_register_handler(LAPIC_VECTOR_TIMER,    lapic_timer_irq);
    isr_register_handler(LAPIC_VECTOR_SPURIOUS, lapic_spurious_irq);
#ifdef MAINDOB_SMP
    isr_register_handler(LAPIC_VECTOR_RESCHED,  lapic_resched_irq);
    isr_register_handler(LAPIC_VECTOR_TLB,      lapic_tlb_irq);
#endif

    /* === Passi 3-4: software-enable (SVR) + LVT + TPR ===
     * La BSP porta LINT0 in modalita' virtual-wire ExtINT cosi' il PIC
     * 8259 legacy la raggiunge (condiviso con lapic_program_local). */
    lapic_program_local(LAPIC_LVT_DELIVERY_EXTINT);

    /* === Passo 5: scegli la modalita' e calibra === */
    /* Politica: SEMPRE one-shot sul clock di bus, mai TSC-deadline. Il
     * one-shot conta su un clock che l'emulatore fa avanzare anche a
     * vCPU ferma in hlt; la TSC-deadline e' espressa nel TSC LOCALE
     * della vCPU — se quello non avanza o e' sfasato (QEMU TCG), la
     * deadline non scatta MAI e la CPU dorme per sempre col lavoro in
     * coda. Riabilitare la TSC-deadline solo dietro una verifica
     * esplicita di TSC invariant + sincronizzato fra le CPU. */
    s_use_tscd = false;
    (void)cpu_has(CPUF_TSC_DEADLINE);
    s_arm_fn   = s_use_tscd ? lapic_arm_tscd : lapic_arm_oneshot;

    if (s_use_tscd)
    {
        s_max_arm_delta_ns = LAPIC_TSCD_MAX_ARM_DELTA_NS;
    }
    else
    {
        uint32_t flags = irq_save();
        s_bus_hz = calibrate_bus_hz();
        irq_restore(flags);

        s_bus_cycles_per_ns_q32 = (s_bus_hz << 32) / 1000000000u;

        /* Limita delta_ns cosi' che delta_ns * q32 resti dentro 2^63.
         * Nessuna divisione 64/64 disponibile: bit-scan del MSB di q32,
         * 2^(62-msb) e' sempre un limite sicuro (entro 2x dal massimo
         * vero). Pavimento 1 ns, soffitto 1 s (re-arm prevedibile). */
        if (s_bus_cycles_per_ns_q32 > 0)
        {
            int msb = 63;
            while (msb >= 0 && !(s_bus_cycles_per_ns_q32 & ((uint64_t)1 << msb)))
            {
                msb--;
            }

            uint64_t safe;
            if (msb < 0)      safe = 1000000000ull;
            else if (msb >= 62) safe = 1;
            else               safe = (uint64_t)1 << (62 - msb);

            s_max_arm_delta_ns = (safe < 1000000000ull) ? safe : 1000000000ull;
        }
    }

    uint32_t lvt_mode = s_use_tscd ? LAPIC_LVT_TIMER_TSC_DEADLN
                                   : LAPIC_LVT_TIMER_ONE_SHOT;
    lapic_write(LAPIC_REG_LVT_TIMER, lvt_mode | LAPIC_VECTOR_TIMER);

    const char *mode = s_x2apic ? "x2APIC" : "xAPIC";
    if (s_use_tscd)
    {
        kprintf("[APIC] %s pronto (TSC-deadline, vettore 0x%x)\n",
                mode, LAPIC_VECTOR_TIMER);
    }
    else
    {
        uint32_t mhz = (uint32_t)(s_bus_hz / 1000000u);
        uint32_t khz = (uint32_t)((s_bus_hz / 1000u) % 1000u);
        kprintf("[APIC] %s pronto (one-shot bus %u.%03u MHz, vettore 0x%x)\n",
                mode, mhz, khz, LAPIC_VECTOR_TIMER);
    }
}

/* TSC-deadline: scrive il target TSC assoluto sull'MSR deadline. La CPU
 * confronta col proprio TSC e spara il vettore 0xF0. La LVT timer e'
 * configurata una volta in lapic_init e non cambia piu' — qui non la
 * si tocca (percorso caldo). */
static void lapic_arm_tscd(uint64_t deadline_ns, uint64_t now_cycles, uint64_t now_ns)
{
    uint64_t delta_ns = (deadline_ns > now_ns) ? (deadline_ns - now_ns) : 0;
    if (delta_ns > s_max_arm_delta_ns) delta_ns = s_max_arm_delta_ns;

    uint64_t target_tsc = now_cycles + (delta_ns ? tsc_ns_to_cycles(delta_ns) : 1);

    /* now_cycles viaggia CORRETTO (offset di sync per-CPU applicato,
     * contratto del layer TSC); l'MSR confronta col TSC GREZZO di
     * questa CPU: riconversione qui, al filo dell'hardware — l'unico
     * punto dove i due mondi si toccano. */
    target_tsc = tsc_cycles_to_raw(target_tsc);

    /* Fence richiesta dal SDM prima di scrivere IA32_TSC_DEADLINE. */
    __asm__ volatile ("lfence" ::: "memory");
    wrmsr(MSR_IA32_TSC_DEADLINE, target_tsc);
}

/* One-shot: converte il delta in cicli di bus LAPIC, scrive il
 * conteggio iniziale (parte a decrementare appena scritto). Divisore 1
 * -> un tick LAPIC == un ciclo di bus. */
static void lapic_arm_oneshot(uint64_t deadline_ns, uint64_t now_cycles, uint64_t now_ns)
{
    (void)now_cycles;
    uint64_t delta_ns = (deadline_ns > now_ns) ? (deadline_ns - now_ns) : 0;
    if (delta_ns > s_max_arm_delta_ns) delta_ns = s_max_arm_delta_ns;

    uint64_t delta_cycles_q32 = delta_ns * s_bus_cycles_per_ns_q32;
    uint64_t delta_cycles     = delta_cycles_q32 >> 32;

    uint32_t init_count;
    if (delta_cycles == 0)              init_count = 1;
    else if (delta_cycles > 0xFFFFFFFEu) init_count = 0xFFFFFFFEu;
    else                                 init_count = (uint32_t)delta_cycles;

    lapic_write(LAPIC_REG_TIMER_INIT, init_count);
}

void lapic_arm_at_ns_with_now(uint64_t deadline_ns, uint64_t now_cycles, uint64_t now_ns)
{
    s_arm_fn(deadline_ns, now_cycles, now_ns);
}

void lapic_arm_at_ns(uint64_t deadline_ns)
{
    tsc_snapshot_t snap;
    tsc_snapshot(&snap);
    s_arm_fn(deadline_ns, snap.cycles, snap.ns);
}

void lapic_disarm(void)
{
    if (s_use_tscd)
    {
        wrmsr(MSR_IA32_TSC_DEADLINE, 0);
        return;
    }
    lapic_write(LAPIC_REG_LVT_TIMER,
               LAPIC_LVT_MASKED | LAPIC_LVT_TIMER_ONE_SHOT | LAPIC_VECTOR_TIMER);
    lapic_write(LAPIC_REG_TIMER_INIT, 0);
}

/* === Superficie tick_source ============================================
 *
 * Il LAPIC e' gia' inizializzato da stage_smp_online quando il motore
 * eventi seleziona il backend: install e' un no-op dichiarato. Gli
 * altri campi puntano dritti alle funzioni esistenti — la vtable e'
 * solo la presa standard, il ferro resta questo file. */

#include "time/tick_source.h"

static void ts_lapic_install(void)
{
    /* lapic_init (MSR revival compreso) e' gia' corso: nulla da fare. */
}

static void ts_lapic_register(tick_source_cb_t cb)
{
    lapic_register_timer_callback(cb);
}

const struct tick_source tick_source_lapic =
{
    .install           = ts_lapic_install,
    .arm_deadline_ns   = lapic_timer_arm_ns,
    .disarm            = lapic_timer_disarm,
    .register_callback = ts_lapic_register,
    .name              = "LAPIC",
    .max_arm_delta_ns  = UINT64_MAX,    /* clamp interno al backend */
};
