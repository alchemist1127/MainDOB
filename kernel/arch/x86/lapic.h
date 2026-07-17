#ifndef MAINDOB_ARCH_LAPIC_H
#define MAINDOB_ARCH_LAPIC_H

#include "lib/types.h"

/* Local APIC — controller di interrupt e sorgente di eventi per-CPU.
 *
 * In questa consegna il LAPIC serve a due cose: (a) inviare le IPI
 * INIT-SIPI-SIPI che accendono le AP (arch/x86/smp.c) e le IPI di
 * reschedule/TLB-shootdown gia' predisposte per quando serviranno
 * davvero; (b) essere pronto ad armare un wake-up one-shot a precisione
 * di nanosecondo — capacita' completa e testabile qui, ma NON ancora
 * agganciata al posto del tick PIT: quel cablaggio (tick per-core)
 * arriva con la 1.1.0.0.7, insieme alla pinnatura dello scheduler (D2).
 *
 * Due modalita' di arming, scelte una volta sola in lapic_init:
 *   1. ONE_SHOT (fallback universale): si programma il registro Initial
 *      Count coi cicli di bus fino alla scadenza. Serve la calibrazione
 *      di lapic_bus_hz contro il TSC.
 *   2. TSC_DEADLINE (Sandy Bridge 2011+, CPUF_TSC_DEADLINE presente): si
 *      scrive il valore assoluto target su IA32_TSC_DEADLINE; la CPU
 *      confronta col proprio TSC e spara.
 *
 * Layout dei vettori:
 *   0xF0  timer LAPIC     (lapic_timer_irq)
 *   0xF1  reschedule IPI  (lapic_resched_irq)
 *   0xF2  TLB shootdown   (lapic_tlb_irq, solo MAINDOB_SMP)
 *   0xFF  spurious        (lapic_spurious_irq — no-op)
 * Ognuno passa dal dispatcher generico isr_dispatch (isr_stubs.asm ha
 * gia' gli stub per 240/241/242/255): niente asm dedicato, si riusa la
 * stessa macro di stub di ogni altro vettore (D9).
 *
 * EOI: sempre al registro LAPIC_EOI, MAI al PIC 8259 — che resta vivo
 * solo per tastiera/ATA/floppy in modalita' virtual-wire (LINT0=ExtINT
 * sulla BSP). Spurious e' l'unico vettore esente da EOI (per spec). */

#define LAPIC_VECTOR_TIMER       0xF0
#define LAPIC_VECTOR_RESCHED     0xF1
#define LAPIC_VECTOR_TLB         0xF2
#define LAPIC_VECTOR_SPURIOUS    0xFF

/* Inizializza il LAPIC della BSP: individua base+modalita' (xAPIC MMIO o
 * x2APIC via MSR), abilita via SVR, programma le LVT (LINT0=ExtINT,
 * LINT1=NMI, timer mascherato one-shot, resto mascherato), calibra il
 * bus (se non TSC-deadline), installa i gate IDT dei propri vettori.
 * Precondizioni: cpu_features_init, tsc_init, idt_init gia' girate. */
void lapic_init(void);

/* True se il Local APIC e' utilizzabile (xAPIC mappato o x2APIC attivo).
 * False su hardware senza LAPIC: ogni chiamata a lapic_send_ipi/
 * lapic_get_id va sempre condizionata da questo, altrimenti si
 * dereferenzia una base MMIO NULL e si va in panic. */
bool lapic_available(void);

/* Id del Local APIC della CPU chiamante. */
uint32_t lapic_get_id(void);

/* Invia una IPI a destinazione fisica singola. `icr_lo` porta gia'
 * vettore + modo di consegna + bit level/trigger. Spinna finche' il
 * LAPIC non conferma la consegna della precedente — necessario tra
 * l'INIT e le due SIPI del bring-up. */
void lapic_send_ipi(uint8_t apic_id, uint32_t icr_lo);

/* EOI al LAPIC (mai al PIC). */
void lapic_eoi(void);

#ifdef MAINDOB_SMP
/* Bring-up del LAPIC di un'AP. Riusa la mappa MMIO, i gate IDT e la
 * calibrazione della BSP; abilita e programma solo il LAPIC di QUESTA
 * CPU. Deve girare dopo che l'AP ha il proprio GDT/IDT caricati. */
void lapic_init_ap(void);

void lapic_send_resched_ipi(uint8_t apic_id);
void lapic_register_resched_callback(void (*cb)(void));
uint32_t lapic_resched_count(void);

void lapic_send_tlb_ipi(uint8_t apic_id);
#endif

/* === Arming one-shot/TSC-deadline (predisposto, non ancora agganciato
 * al tick — vedi nota d'intestazione) === */

void lapic_arm_at_ns(uint64_t deadline_ns);
void lapic_arm_at_ns_with_now(uint64_t deadline_ns,
                              uint64_t now_cycles, uint64_t now_ns);
void lapic_disarm(void);

typedef void (*lapic_timer_cb_t)(void);
void lapic_register_timer_callback(lapic_timer_cb_t cb);

/* true se il timer locale e' armabile a scadenza (calibrato o
 * TSC-deadline). */
bool lapic_timer_usable(void);
bool lapic_timer_is_tsc_deadline(void);

/* Arma il timer di QUESTA CPU perche' scatti alla scadenza assoluta
 * (ns monotoni). Un armo successivo sostituisce il precedente. */
void lapic_timer_arm_ns(uint64_t deadline_ns);

/* Spegne il timer locale (nessuna scadenza in agenda). */
void lapic_timer_disarm(void);

#endif
