#ifndef MAINDOB_SYSCALL_SYSCALL_H
#define MAINDOB_SYSCALL_SYSCALL_H

#include "lib/types.h"
#include "arch/x86/isr.h"

/* ABI userspace 1:1 col kernel 1.0 (DESIGN D10). Numeri IMMUTABILI:
 * convenzione registri eax=num, ebx/ecx/edx/esi=arg0..3, ritorno in eax. */

#define SYSCALL_INT     0x80
#define SYSCALL_MAX     128

/* Process */
#define SYS_EXIT            0
#define SYS_YIELD           1
#define SYS_GETPID          2
#define SYS_WAIT            4
#define SYS_KILL            5
#define SYS_PROC_STATUS     6
#define SYS_GET_PRIVILEGES  7
#define SYS_SHUTDOWN        8

/* IPC */
#define SYS_PORT_CREATE     10
#define SYS_PORT_DESTROY    11
#define SYS_SEND            12
#define SYS_RECEIVE         13
#define SYS_REPLY           14
#define SYS_NOTIFY          15
#define SYS_WAIT_NOTIFY     16
#define SYS_RECEIVE_NOWAIT  17
#define SYS_POST            18

/* Memory */
#define SYS_MMAP            20
#define SYS_MUNMAP          21
#define SYS_BRK             22
#define SYS_SHM_CREATE      23
#define SYS_SHM_MAP         24
#define SYS_SHM_UNMAP       25

#define SYS_HANDLE_CLOSE    30

/* Time */
#define SYS_CLOCK_GETTIME   40
#define SYS_NANOSLEEP       41

/* Hardware I/O (driver only) */
#define SYS_IO_PORT_IN      50
#define SYS_IO_PORT_OUT     51
#define SYS_IRQ_REGISTER    52
#define SYS_IRQ_DONE        53
#define SYS_MMAP_PHYS       54
#define SYS_DMA_ALLOC       55
#define SYS_SET_PRIVILEGES  56
#define SYS_GET_HOME_DIR    57
#define SYS_DEVICE_CLAIM    60
#define SYS_DEVICE_RELEASE  61
#define SYS_TIMER_SET       62
#define SYS_TIMER_CANCEL    63
#define SYS_PCI_READ        64
#define SYS_PCI_WRITE       65
#define SYS_SET_PRIORITY    66
#define SYS_WATCHDOG_SET    67
#define SYS_RANDOM          68
#define SYS_SPAWN_DATA      74
#define SYS_GET_STARTUP     75
#define SYS_MEMINFO         76
#define SYS_GETVERSION      77
#define SYS_CLOCK_US        79
#define SYS_SLEEP_US        80
#define SYS_THREAD_EXIT     81
#define SYS_THREAD_CREATE   82
#define SYS_EVENT_CREATE    83
#define SYS_EVENT_SETCLEAR  84
#define SYS_EVENT_WAIT      85
#define SYS_EVENT_GETFLAGS  86
#define SYS_GETTIME         87
#define SYS_FUTEX           88
#define SYS_REG_REGISTER    90
#define SYS_REG_UNREGISTER  91
#define SYS_REG_FIND        92
#define SYS_REG_WAIT        94
#define SYS_GET_MODULE_FLAGS 95
#define SYS_MEM_LOAD        96
#define SYS_REGISTER_VIDEO_DRIVER 97
#define SYS_DEBUG_PRINT     99
#define SYS_IRQ_FIND_FREE   100
#define SYS_IRQ_WIRE_DEVICE 101
#define SYS_LIVE_QUERY      102
#define SYS_LIVE_READ       103
#define SYS_REBOOT          104
#define SYS_ENTER_CRITICAL  105
#define SYS_EXIT_CRITICAL   106
#define SYS_MSI_ENABLE      107
#define SYS_DMA_FREE        108
#define SYS_DMA_VERIFY      109
#define SYS_IRQ_REGISTER_PCI 110
#define SYS_IRQ_PCI_CLAIM    111
#define SYS_INTR_DELIVERY_MODE 112  /* ogni processo: 1=IOAPIC, 0=PIC (8259) */
#define SYS_PORT_GEN         113  /* generazione corrente di una porta (0=morta) */
#define SYS_POST_CK          114  /* post verificato per generazione (anti-ABA)  */
#define SYS_WATCH_PORT       115  /* notifica event-driven di morte di una porta */
#define SYS_PANIC_TEST       116  /* TEST: panic deliberato per verificare la schermata di panic */
#define SYS_SET_PANIC_FB       117  /* driver video: registra il framebuffer per il panic */
#define SYS_TASK_SNAPSHOT      118  /* fotografia processi+CPU per il task manager */
#define SYS_PROC_SET_PRIORITY  119  /* renice di un processo per PID (task manager) */

typedef int32_t (*syscall_handler_t)(isr_regs_t *regs);

void syscall_init(void);

/* Vero se il processo corrente ha PRIV_DRIVER (condiviso col layer
 * driver in syscall/driver.c). */
bool caller_is_driver(void);

/* Layer driver-hardware (syscall/driver.c). */
void driver_syscalls_init(void);
void driver_cleanup_process(pid_t pid);
void syscall_register(uint32_t num, syscall_handler_t handler);

#endif
