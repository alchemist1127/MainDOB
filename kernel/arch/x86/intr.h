#ifndef MAINDOB_ARCH_INTR_H
#define MAINDOB_ARCH_INTR_H

#include "lib/types.h"
#include "arch/x86/isr.h"

/* Controller di interrupt unificato (arch/x86/intr.c).
 * Astrae la consegna IRQ dispositivo (PIC finche' non si migra
 * all'IOAPIC) e possiede i vettori allocabili IOAPIC/MSI 0x50..0xDF. */

/* true dopo intr_switch_to_ioapic(): consegna via IOAPIC (EOI al LAPIC). */
bool  intr_ioapic_delivery_active(void);

/* Migra la consegna dal PIC all'IOAPIC. Ritorna false (no-op) se nessun
 * IOAPIC e' disponibile: il PIC resta il controller. */
bool  intr_switch_to_ioapic(void);

/* Handler per vettore (dispatcher unificato). Installa anche il gate IDT
 * per i vettori allocabili. */
void  intr_set_vector_handler(uint8_t vector, isr_handler_t handler);
void  intr_clear_vector(uint8_t vector);
void  intr_dispatch(isr_regs_t *regs);

/* Allocazione event-driven dei vettori 0x50..0xDF (IOAPIC/MSI). */
int32_t intr_alloc_vector(pid_t owner);
void    intr_free_vector(uint8_t vector);
pid_t   intr_vector_owner(uint8_t vector);
void    intr_release_for_pid(pid_t pid);

/* Mask/unmask di una linea legacy (0..15) attraverso il controller
 * attivo: 8259 finche' non si migra, poi voce di redirezione IOAPIC. */
void  intr_line_mask(uint8_t line);
void  intr_line_unmask(uint8_t line);

/* Steering: ripunta la voce IOAPIC di una linea sul core `cpu_index`. */
void  intr_route_line_to_cpu(uint8_t line, uint32_t cpu_index);

/* Registra l'handler di una linea legacy anche nella tabella per-vettore,
 * cosi' la migrazione all'IOAPIC lo ritrova (ponte da driver.c). */
void  intr_bridge_legacy_handler(uint8_t line, isr_handler_t handler);

#endif
