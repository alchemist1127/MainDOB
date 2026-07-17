#ifndef MAINDOB_ARCH_ISR_H
#define MAINDOB_ARCH_ISR_H

#include "lib/types.h"

/* Frame salvato dagli stub in isr_stubs.asm (ordine di push). */
typedef struct __attribute__((packed))
{
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;   /* pusha    */
    uint32_t vector;
    uint32_t error_code;
    uint32_t eip, cs, eflags;
    uint32_t useresp, ss;               /* solo su transizione da ring 3 */
} isr_regs_t;

typedef void (*isr_handler_t)(isr_regs_t *regs);

void isr_init(void);
void isr_register_handler(uint8_t vector, isr_handler_t handler);
void isr_register_syscall(void (*dispatch)(isr_regs_t *regs));

#endif
