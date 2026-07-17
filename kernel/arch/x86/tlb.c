/* TLB shootdown cross-core: broadcast con handshake generazione/ack.
 *
 * Invarianti di correttezza (non romperle):
 *   - s_tlb_lock serializza gli iniziatori e si acquisisce con uno spin
 *     SERVENTE (applica gli shootdown in arrivo mentre gira) — e' questo
 *     che impedisce a due iniziatori simultanei di incastrarsi sull'ack
 *     l'uno dell'altro.
 *   - Solo s_tlb_lock resta tenuto durante l'attesa degli ack (nessun
 *     altro lock kernel), cosi' un bersaglio in una sezione critica non
 *     correlata riabilita sempre IF e conferma.
 *   - Il payload + il reset degli ack sono pubblicati PRIMA di
 *     incrementare s_gen; il TSO di x86 piu' una barriera garantiscono
 *     che una CPU che vede la nuova generazione veda anche il payload
 *     giusto.
 * Contratto del chiamante in tlb.h. */

#ifdef MAINDOB_SMP

#include "arch/x86/tlb.h"
#include "arch/x86/lapic.h"
#include "arch/x86/cpu.h"
#include "mm/pmm.h"                /* PAGE_SIZE                          */
#include "proc/percpu.h"           /* this_cpu(), g_cpus[], MAX_CPUS     */
#include "sync/atomic.h"
#include "sync/spinlock.h"
#include "time/clock.h"            /* clock_now_ns (watchdog dell'attesa)*/
#include "kernel.h"                /* kpanic                             */

#define TLB_FLUSH_ALL   0xFFFFFFFFu   /* sentinella npages: flush intero */
#define TLB_INVLPG_MAX  64u           /* oltre, un reload CR3 costa meno */

/* Serializzazione degli iniziatori. Tenuto solo dentro il protocollo di
 * questo file; nessun altro lock viene preso mentre e' tenuto. */
static spinlock_t s_tlb_lock = SPINLOCK_INIT;

static volatile uint32_t s_flush_va;
static volatile uint32_t s_flush_pages;

/* Generazione: incrementata DOPO che il payload e' pubblicato, per
 * segnalare un nuovo shootdown. Ogni CPU serve una data generazione
 * esattamente una volta. */
static volatile uint32_t s_gen;

/* Ack per la generazione viva; azzerati prima di ogni incremento di
 * generazione. */
static volatile uint32_t s_ack;

/* Ultima generazione gia' servita, per CPU. Una CPU invalida+conferma
 * solo quando la generazione viva differisce da questa. */
static volatile uint32_t s_serviced_gen[MAX_CPUS];

/* Bitmask delle CPU partecipanti (online, IDT+LAPIC vivi, serviced-gen
 * primata). Mutata solo sotto s_tlb_lock, quindi non puo' cambiare sotto
 * uno shootdown in-flight che ne ha gia' fatto uno snapshot. */
static volatile uint32_t s_online_mask;

/* Popcount senza tirare dentro __popcountsi2 di libgcc (il target non
 * garantisce POPCNT). SWAR branchless. */
static inline uint32_t tlb_popcount(uint32_t x)
{
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    return (x * 0x01010101u) >> 24;
}

/* Applica un'invalidazione locale: pagina per pagina per range piccoli,
 * flush completo (reload CR3) altrimenti. Le pagine di heap kernel non
 * sono globali, quindi un reload CR3 le evince tutte. */
static inline void tlb_local_apply(uint32_t va, uint32_t pages)
{
    if (pages == TLB_FLUSH_ALL || pages > TLB_INVLPG_MAX)
    {
        cpu_write_cr3(cpu_read_cr3());   /* butta ogni TLB non-globale   */
        return;
    }
    for (uint32_t i = 0; i < pages; i++)
    {
        cpu_invlpg(va + i * PAGE_SIZE);
    }
}

/* Serve lo shootdown vivo per QUESTA cpu, una volta per generazione,
 * poi conferma. Sicura da chiamare sia dalla ISR della IPI sia dallo
 * spin di acquisizione del lock: gated su s_serviced_gen, cosi' una
 * seconda chiamata nella stessa generazione e' un no-op (e non conferma
 * mai due volte). */
void tlb_on_ipi(void)
{
    uint32_t idx = this_cpu()->cpu_index;
    uint32_t gen = atomic_read(&s_gen);
    if (gen == s_serviced_gen[idx])
    {
        return;                                   /* niente di nuovo per noi */
    }

    /* s_gen resta stabile per tutta la finestra (un solo iniziatore alla
     * volta, che tiene s_tlb_lock), quindi il payload letto qui
     * corrisponde a questa generazione. */
    tlb_local_apply(s_flush_va, s_flush_pages);
    s_serviced_gen[idx] = gen;
    atomic_inc(&s_ack);
}

/* Acquisisce s_tlb_lock servendo ogni shootdown diretto a noi nel
 * frattempo. Ritorna con interrupt disattivati (stato precedente in
 * *flags) e il lock tenuto. A differenza di una acquisizione irqsave
 * normale, lo spin SERVE gli shootdown in-flight cosi' un iniziatore
 * remoto che ci punta puo' sempre raccogliere il nostro ack — spezzando
 * il ciclo a due iniziatori anche se il nostro IF e' 0 qui. */
static uint32_t tlb_lock_acquire_servicing(void)
{
    uint32_t flags;
    uint32_t spins = 0;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags));
    while (atomic_xchg(&s_tlb_lock.lock, 1u) != 0u)
    {
        tlb_on_ipi();                 /* non blocca uno shootdown remoto */
        __asm__ volatile ("pause");
        if (unlikely(++spins > SPINLOCK_LOCKUP_SPINS))
        {
            spinlock_lockup_report(&s_tlb_lock.lock, __builtin_return_address(0));
        }
    }
    return flags;
}

void tlb_shootdown_range(uint32_t va, uint32_t npages)
{
    if (npages == 0) return;

    /* Snapshot dell'insieme partecipante SOTTO s_tlb_lock. L'insieme
     * muta solo sotto questo lock (tlb_bsp_online/tlb_ap_online), quindi
     * prenderlo qui rende la decisione "sono l'unico partecipante?"
     * senza corse contro un'AP che si sta unendo in quel momento: o la
     * osserviamo (e la includiamo nello shootdown) o si unisce
     * strettamente dopo la barriera di rilascio del lock (quando la
     * nostra PTE azzerata e' gia' visibile, quindi non puo' mettere in
     * cache la mappatura stantia). */
    uint32_t flags = tlb_lock_acquire_servicing();   /* IF=0, lock tenuto */

    uint32_t self     = this_cpu()->cpu_index;
    uint32_t targets  = s_online_mask & ~(1u << self);
    uint32_t ntargets = tlb_popcount(targets);

    if (ntargets == 0)
    {
        /* Siamo l'unico partecipante: basta un'invalidazione locale,
         * nessun altro puo' tenere una voce stantia. */
        tlb_local_apply(va, npages);
        spinlock_release(&s_tlb_lock);
        __asm__ volatile ("push %0; popf" : : "r"(flags) : "memory", "cc");
        return;
    }

    /* Pubblica la richiesta, azzera gli ack, POI incrementa la
     * generazione: una CPU che vede la nuova gen e' garantita vedere
     * anche questo payload e un contatore azzerato. */
    s_flush_va    = va;
    s_flush_pages = npages;
    atomic_set(&s_ack, 0);
    memory_barrier();
    uint32_t gen = atomic_add_return(&s_gen, 1);

    /* Reclama la generazione come servita per noi stessi, cosi' una
     * chiamata di servizio vagante su questa cpu (non puo' succedere con
     * IF=0, ma per difesa) non si auto-conferma — non siamo in
     * `targets` e invalidiamo localmente sotto. */
    s_serviced_gen[self] = gen;

    for (uint32_t c = 0; c < (uint32_t)MAX_CPUS; c++)
    {
        if (targets & (1u << c))
        {
            lapic_send_tlb_ipi(g_cpus[c].apic_id);
        }
    }

    tlb_local_apply(va, npages);

    /* Nessun altro shootdown puo' essere in volo (teniamo il lock),
     * quindi non serve servire nulla mentre attendiamo: i bersagli
     * momentaneamente a IF=0 prenderanno la IPI appena la riabilitano e
     * confermeranno. Un ack che non arriva mai e' un bersaglio incastrato
     * (o un chiamante che ha violato il contratto "nessun altro lock
     * durante uno shootdown"): dopo ~2s si nominano le CPU delinquenti e
     * si fa panic — un freeze silenzioso qui sarebbe indebuggabile, un
     * panic mirato no. */
    {
        uint64_t ack_deadline = clock_now_ns() + 2000000000ull;
        while (atomic_read(&s_ack) < ntargets)
        {
            __asm__ volatile ("pause");
            if (unlikely(clock_now_ns() > ack_deadline))
            {
                uint32_t missing = 0;
                for (uint32_t c = 0; c < (uint32_t)MAX_CPUS; c++)
                {
                    if ((targets & (1u << c)) && s_serviced_gen[c] != gen)
                    {
                        missing |= (1u << c);
                    }
                }
                kpanic("TLB shootdown gen %u: %u/%u ack dopo 2s "
                       "(iniziatore CPU %u, maschera CPU mancanti %08x). "
                       "Sospetto: bersaglio incastrato con IF=0, o "
                       "shootdown chiamato tenendo un lock su cui un "
                       "bersaglio sta girando.",
                       gen, atomic_read(&s_ack), ntargets, self, missing);
            }
        }
    }

    spinlock_release(&s_tlb_lock);
    __asm__ volatile ("push %0; popf" : : "r"(flags) : "memory", "cc");
}

void tlb_shootdown_aspace(uint32_t pd_phys, uint32_t va, uint32_t npages)
{
    uint32_t self = this_cpu()->cpu_index;

    for (uint32_t cpu = 0; cpu < MAX_CPUS; cpu++)
    {
        if (cpu == self || ((s_online_mask >> cpu) & 1u) == 0)
        {
            continue;
        }
        if (g_cpus[cpu].loaded_cr3 == pd_phys)
        {
            /* Almeno un altro core esegue questo AS: broadcast. Le CPU
             * con un CR3 diverso pagano un invlpg inutile — accettabile
             * per un evento raro rispetto a tracciare maschere per-AS. */
            tlb_shootdown_range(va, npages);
            return;
        }
    }
    /* Nessun altro core ha questo CR3: niente entry remote possibili. */
}

void tlb_bsp_online(void)
{
    uint32_t flags = spinlock_acquire_irqsave(&s_tlb_lock);
    uint32_t idx = this_cpu()->cpu_index;          /* 0 per la BSP       */
    s_serviced_gen[idx] = atomic_read(&s_gen);
    s_online_mask |= (1u << idx);
    spinlock_release_irqrestore(&s_tlb_lock, flags);
}

void tlb_ap_online(void)
{
    /* Acquisizione semplice va bene: questa AP non e' ancora in
     * s_online_mask, quindi nessuno shootdown in-flight la punta — non
     * puo' essere l'ack che un possessore attende. */
    uint32_t flags = spinlock_acquire_irqsave(&s_tlb_lock);
    uint32_t idx = this_cpu()->cpu_index;
    /* Prima serviced-gen alla generazione viva, cosi' non applichiamo/
     * confermiamo mai uno shootdown completato prima di unirci. */
    s_serviced_gen[idx] = atomic_read(&s_gen);
    s_online_mask |= (1u << idx);
    spinlock_release_irqrestore(&s_tlb_lock, flags);
}

#endif /* MAINDOB_SMP */
