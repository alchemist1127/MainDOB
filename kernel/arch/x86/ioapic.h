#ifndef MAINDOB_ARCH_IOAPIC_H
#define MAINDOB_ARCH_IOAPIC_H

#include "lib/types.h"

/* L'I/O APIC instrada un Global System Interrupt (GSI, un piedino di
 * ingresso) su un vettore IDT a scelta, su un Local APIC a scelta, con
 * polarita' e trigger programmabili — a differenza dell'8259, la cui
 * relazione linea->vettore(32+linea) e' fissa.
 *
 * Scope di questa consegna: scoperta dalla MADT, mapping MMIO, e la
 * capacita' meccanica di instradare/mascherare un GSI. La MIGRAZIONE
 * della consegna IRQ dei dispositivi (dal PIC a questo backend) resta
 * fuori: arch/x86/irq.c oggi non ha un'astrazione di controller
 * intercambiabile (serve al blocco PCI/PIRQ della 1.1.3, non all'SMP) —
 * il PIC resta l'unico che consegna IRQ dispositivo, invariato, finche'
 * quel lavoro non arriva. Qui l'IOAPIC e' pronto ma inerte.
 *
 * Compatibilita' duale: si inizializza solo se la MADT ha riportato
 * almeno un IOAPIC E la CPU ha un Local APIC. Su macchine senza (il
 * Compaq Armada E500 quando l'APIC resta assente) ioapic_init() ritorna
 * false — non deve mai capitare, dato che oggi nessun chiamante lo usa,
 * ma l'invariante resta documentata per quando servira'. */

/* Scopre e mappa ogni IOAPIC dalla MADT, maschera tutti i loro ingressi.
 * Ritorna true se almeno un IOAPIC e' stato inizializzato. Chiamare dopo
 * acpi_init() e lapic_init(). */
bool ioapic_init(void);

bool ioapic_available(void);

/* Instrada un GSI su (vettore) sul Local APIC dato, con polarita'/trigger
 * presi dai flag MPS INTI (0 = default ISA: edge, active-high). La voce
 * e' programmata ma lasciata mascherata: chiamare ioapic_unmask_gsi per
 * armarla. Ritorna 0 su successo, -1 se nessun IOAPIC copre il GSI. */
int ioapic_route_gsi(uint32_t gsi, uint8_t vector, uint8_t lapic_id,
                     uint16_t mps_flags);

void ioapic_mask_gsi(uint32_t gsi);
void ioapic_unmask_gsi(uint32_t gsi);

/* True se un qualche IOAPIC scoperto ha davvero un piedino per `gsi`. */
bool ioapic_covers_gsi(uint32_t gsi);

#endif
