#ifndef MAINDOB_ARCH_IDT_H
#define MAINDOB_ARCH_IDT_H

#include "lib/types.h"

void idt_init(void);
void idt_set_gate(uint8_t vector, uint32_t handler,
                  uint16_t selector, uint8_t flags);

/* La IDT e' UNA sola tabella condivisa da tutte le CPU (i gate non
 * dipendono dal core che li serve). Ogni AP deve pero' ripetere il
 * proprio lidt — il registro IDTR e' per-CPU anche se punta alla stessa
 * tabella. Chiamata da ap_main dopo che l'AP e' sulla propria pila. */
void idt_load_this_cpu(void);

#define IDT_FLAG_INT_KERNEL 0x8E    /* present, ring0, interrupt gate 32 */
#define IDT_FLAG_INT_USER   0xEE    /* present, ring3 (int 0x80 futuro)  */

#endif
