#ifndef MAINDOB_SYSCALL_VIDEO_BOOMERANG_H
#define MAINDOB_SYSCALL_VIDEO_BOOMERANG_H

#include "lib/types.h"
#include "arch/x86/isr.h"

/* Boomerang video — deflettore int 0x85 per le chiamate dv_* (dobVideo).
 *
 * Percorso caldo (ingresso 0x85, ritorno 0x86, copia payload, switch
 * CR3) in video_boomerang.asm; registrazione, init e teardown in C
 * (syscall/driver.c). Slot globale unico; ripulito alla morte del
 * driver. Gli offset di struct che l'asm codifica sono blindati da
 * _Static_assert in driver.c. */

#define VIDEO_BOOMERANG_INT  0x85
#define VIDEO_BOOMERANG_RET  0x86

/* Stato condiviso con l'asm (definito la'). */
extern uint32_t g_video_driver_cr3;
extern uint32_t g_video_driver_entry;
extern uint32_t g_video_driver_pid;
extern uint32_t g_dispatch_stack_top;
extern uint32_t g_driver_payload_buf;

/* Punti d'ingresso asm (per idt_set_gate). */
void int_85_entry(void);
void int_86_return(void);

/* Recupero da fault col trasporto in volo (implementato in driver.c):
 * se lo slot per-CPU e' ACTIVE, il fault appartiene alla fase
 * in-driver — non al chiamante parcheggiato. Rilascia il lock,
 * ripristina CR3 e frame del chiamante in *regs (rc = DV_ERR_RESET) e
 * ritorna true: il fault handler non deve fare altro. false = fault
 * ordinario, si procede normalmente. */
bool video_boomerang_fault_recover(isr_regs_t *regs);

/* Fase in-driver del trasporto su QUESTA CPU (slot ACTIVE). */
bool video_boomerang_in_driver_phase(void);

/* Processo il cui AS e' installato adesso: il driver video durante la
 * fase in-driver, process_current() altrimenti. Da usare nelle
 * syscall che modificano lo spazio di indirizzamento (sbrk). */
struct process *video_boomerang_as_process(void);

#endif
