#ifndef MAINDOB_ARCH_IRQ_H
#define MAINDOB_ARCH_IRQ_H

#include "lib/types.h"
#include "arch/x86/isr.h"

#define IRQ_BASE_VECTOR 32u

typedef void (*irq_handler_t)(isr_regs_t *regs);

void irq_init(void);
void irq_register_handler(uint8_t irq, irq_handler_t handler);
void irq_unmask(uint8_t irq);
void irq_mask(uint8_t irq);

/* Contatore IRQ spuri (linea 7/15 fantasma del PIC): diagnostica ferro. */
uint32_t irq_spurious_count(void);

/* Vero quando gli IRQ dei device sono consegnati via IOAPIC anziche'
 * dal PIC 8259 (ABI: SYS_INTR_DELIVERY_MODE). Il flag viene alzato
 * dal codice che instrada le linee legacy sull'IOAPIC. */
bool irq_delivery_via_ioapic(void);
void irq_set_delivery_via_ioapic(bool active);

/* Hook eseguito dopo ogni EOI: unico punto di preemption da IRQ. */
void irq_set_post_eoi_hook(void (*hook)(void));
/* Invoca il hook di preemption post-EOI: chiamato da OGNI dispatcher di
 * interrupt hardware (irq_dispatch e intr_dispatch) dopo EOI + handler. */
void irq_run_post_eoi_hook(void);

#endif
