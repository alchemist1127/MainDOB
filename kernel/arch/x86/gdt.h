#ifndef MAINDOB_ARCH_GDT_H
#define MAINDOB_ARCH_GDT_H

#include "lib/types.h"

/* Selettori (indice * 8, RPL nei 2 bit bassi). */
#define GDT_SEL_KCODE   0x08
#define GDT_SEL_KDATA   0x10
#define GDT_SEL_UCODE   (0x18 | 3)
#define GDT_SEL_UDATA   (0x20 | 3)
#define GDT_SEL_TSS     0x28
/* Base %gs per-CPU (milestone SMP): con GS = GDT_SEL_PERCPU caricato,
 * [gs:0] legge percpu_t.self della CPU corrente — il fast path di
 * this_cpu() in proc/percpu.h. Indice 6 (0x30), dopo il TSS. */
#define GDT_SEL_PERCPU  0x30

/* TSS 32 bit. Pubblico (non piu' statico in gdt.c) perche' e' EMBEDDED
 * in percpu_t: ogni CPU ha il proprio, cosi' caricare TR non lotta con
 * un'altra CPU sul bit "busy" del descrittore. */
typedef struct __attribute__((packed))
{
    uint32_t prev_task;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t unused[23];
} tss_entry_t;

void gdt_init(void);

/* Stack kernel usato dalla CPU su interrupt da ring 3 (TSS.esp0) — DELLA
 * CPU CORRENTE (this_cpu()->tss.esp0.) Va richiamata ad ogni dispatch
 * dello scheduler, come gia' faceva prima dell'SMP. */
void gdt_set_kernel_stack(uint32_t esp0);

#ifdef MAINDOB_SMP
/* Costruisce e carica il GDT/TSS/base-%gs di QUESTA AP (slot assegnato
 * da percpu_smp_init). Chiamata una volta da ap_main, dopo che l'AP e'
 * gia' sulla propria pila. La BSP resta sempre slot 0 (gdt_init). */
void gdt_ap_init(uint32_t slot);
#endif

#endif
