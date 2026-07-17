#ifndef MAINDOB_ARCH_SMP_H
#define MAINDOB_ARCH_SMP_H

#include "lib/types.h"

/* Pagina fisica bassa fissa dove viene collocato il trampolino AP. Il
 * vettore SIPI e' il numero di questa pagina (0x8000 >> 12 = 0x08). Sta
 * nel primo MB, che pmm_init() riserva PERMANENTEMENTE (reserve_critical_
 * regions marca l'intero primo MB), quindi nessun altro la alloca mai. */
#define AP_TRAMPOLINE_PHYS   0x8000u

/* Punto di ingresso su cui salta il trampolino su ogni AP (meta' alta,
 * C). In questa consegna: carica il GDT/IDT/LAPIC/FPU per-CPU dell'AP,
 * si unisce all'insieme partecipante del TLB shootdown, e resta in un
 * idle loop indipendente — NON adotta ancora un thread ne' entra nello
 * scheduler (quello e' 1.1.0.0.7, insieme alla pinnatura D2). */
void ap_main(void);

/* Porta online ogni CPU non-BSP enabled riportata da ACPI: copia il
 * trampolino, INIT-SIPI-SIPI ogni AP, attende il check-in. No-op su
 * uniprocessore o senza Local APIC. */
void smp_boot_aps(void);

#endif
