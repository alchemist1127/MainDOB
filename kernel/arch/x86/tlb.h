#ifndef MAINDOB_ARCH_TLB_H
#define MAINDOB_ARCH_TLB_H

/* TLB shootdown cross-core.
 *
 * La meta' superiore del kernel e' condivisa da ogni address space (le
 * PDE kernel sono copiate in ogni PD di processo). Quando una mappatura
 * kernel CONDIVISA viene rimossa o ristretta a runtime, un semplice
 * invlpg locale sistema solo il TLB della CPU chiamante — ogni altra
 * CPU online puo' ancora tenere una traduzione stantia, ora permissiva.
 * Su x86 e' un buco di correttezza silenzioso: il caso peggiore e'
 * l'allocatore kernel che smonta una pagina di heap e ne restituisce il
 * frame fisico al pool, dopo di che il TLB stantio di un'altra CPU
 * mappa ancora quel frame (riciclato, riusato).
 *
 * Questo sottosistema fa si' che la CPU iniziatrice invalidi il range
 * su SE STESSA e poi porti ogni altra CPU online a fare lo stesso,
 * ATTENDENDO l'acknowledge di tutte prima che il chiamante possa
 * riusare il frame/l'indirizzo virtuale.
 *
 * Scope: gestisce mappature NON-GLOBALI (l'heap kernel, oggi l'unico
 * smontatore a runtime di pagine condivise). Una pagina davvero
 * *globale* (PTE_GLOBAL) non verrebbe evitta dal fallback a reload di
 * CR3 usato per i range grandi; nessun chiamante attuale smonta una
 * pagina globale a runtime.
 *
 * Assenza di deadlock: un iniziatore tiene SOLO il lock di shootdown
 * mentre attende gli ack (nessun altro lock kernel), e lo spin di
 * acquisizione del lock stesso serve ogni shootdown in-flight diretto a
 * questa CPU — cosi' due iniziatori simultanei non possono incastrarsi
 * a vicenda. I chiamanti NON DEVONO invocare uno shootdown tenendo uno
 * spinlock su cui una CPU remota potrebbe girare con interrupt
 * disattivati.
 * Cablato nei percorsi che riciclano frame/VA condivisi: kheap
 * (strip_range_frames, quindi anche kpages_free e mmio_unmap) e la
 * creazione della pagina guard degli stack kernel. */

#include "lib/types.h"

#ifdef MAINDOB_SMP

/* Invalida [va, va + npages*PAGE_SIZE) su questa CPU e su ogni altra CPU
 * online, ritornando solo dopo che tutte hanno confermato. `npages` puo'
 * essere 1. Un conteggio grande ricade su un flush completo per CPU
 * (piu' economico di molte invalidazioni singole). Percorso rapido
 * no-op quando questa e' l'unica CPU online. Puo' essere chiamata con
 * interrupt gia' disattivati. */
void tlb_shootdown_range(uint32_t va, uint32_t npages);

static inline void tlb_shootdown_page(uint32_t va)
{
    tlb_shootdown_range(va, 1u);
}

/* Shootdown per pagine UTENTE di un address space specifico. Su questo
 * hardware (niente PCID) una traduzione utente non sopravvive a un
 * cambio di CR3: se nessun'altra CPU ha `pd_phys` caricato in questo
 * istante, nessuna puo' avere entry stantie e non si spedisce nulla.
 * Il caso comune (thread pinnati: l'AS vive su una sola CPU) e' quindi
 * un confronto per CPU e zero IPI; il broadcast parte solo quando un
 * altro core sta davvero eseguendo quell'address space. */
void tlb_shootdown_aspace(uint32_t pd_phys, uint32_t va, uint32_t npages);

/* Segna la boot CPU come partecipante allo shootdown. Chiamata una
 * volta, dopo che la mappa apic-id esiste e prima che qualunque AP sia
 * rilasciata. */
void tlb_bsp_online(void);

/* Segna l'AP chiamante come partecipante. Chiamata da ap_main DOPO che
 * IDT/LAPIC dell'AP sono vivi e gli interrupt attivi, cosi' puo' sia
 * ricevere la IPI di shootdown sia non dover mai un ack per una
 * generazione completata prima che si unisse. */
void tlb_ap_online(void);

/* Applica uno shootdown pendente per QUESTA CPU e lo conferma. Invocata
 * dalla ISR della IPI di shootdown (lapic_tlb_irq) e, difensivamente,
 * dallo spin di acquisizione del lock cosi' un iniziatore in attesa
 * serve gli shootdown in arrivo. Idempotente. */
void tlb_on_ipi(void);

#else /* !MAINDOB_SMP — uniprocessore: l'invlpg locale di paging_unmap_page basta */

static inline void tlb_shootdown_range(uint32_t va, uint32_t npages)
{
    (void)va; (void)npages;
}
static inline void tlb_shootdown_page(uint32_t va) { (void)va; }
static inline void tlb_shootdown_aspace(uint32_t pd_phys, uint32_t va,
                                        uint32_t npages)
{
    (void)pd_phys; (void)va; (void)npages;
}

#endif /* MAINDOB_SMP */

#endif
