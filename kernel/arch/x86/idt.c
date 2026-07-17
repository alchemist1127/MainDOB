#include "arch/x86/idt.h"
#include "arch/x86/gdt.h"
#include "lib/string.h"
#include "console/console.h"

typedef struct __attribute__((packed))
{
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t offset_high;
} idt_entry_t;

typedef struct __attribute__((packed))
{
    uint16_t limit;
    uint32_t base;
} idt_ptr_t;

static idt_entry_t s_idt[256];
static idt_ptr_t   s_idt_ptr;

void idt_set_gate(uint8_t vector, uint32_t handler,
                  uint16_t selector, uint8_t flags)
{
    s_idt[vector].offset_low  = (uint16_t)(handler & 0xFFFF);
    s_idt[vector].offset_high = (uint16_t)(handler >> 16);
    s_idt[vector].selector    = selector;
    s_idt[vector].zero        = 0;
    s_idt[vector].flags       = flags;
}

void idt_init(void)
{
    memset(s_idt, 0, sizeof(s_idt));

    s_idt_ptr.limit = sizeof(s_idt) - 1;
    s_idt_ptr.base  = (uint32_t)&s_idt;
    __asm__ volatile ("lidt %0" : : "m"(s_idt_ptr));

    kprintf("[IDT ] 256 gate pronti.\n");
}

void idt_load_this_cpu(void)
{
    /* Stessa tabella (s_idt e' condivisa), IDTR e' pero' per-CPU: ogni
     * AP deve caricarlo da se'. Nessuna voce cambia — idt_set_gate
     * resta l'unico modo di scrivere un gate, chiamato solo dalla BSP
     * durante l'init (D9: la IDT non si duplica per-core). */
    __asm__ volatile ("lidt %0" : : "m"(s_idt_ptr));
}
