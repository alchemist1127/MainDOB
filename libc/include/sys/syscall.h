#ifndef MAINDOB_LIBC_SYS_SYSCALL_H
#define MAINDOB_LIBC_SYS_SYSCALL_H

#include <sys/types.h>

/* Must match kernel/syscall/syscall.h */
#define SYS_EXIT        0
#define SYS_YIELD       1
#define SYS_GETPID      2
/* slot 3 reserved (formerly SYS_SPAWN — superseded by SYS_SPAWN_DATA) */
#define SYS_WAIT        4
#define SYS_KILL        5
#define SYS_PROC_STATUS 6
#define SYS_GET_PRIVILEGES 7
#define SYS_SHUTDOWN    8   /* power off the system; never returns */
#define SYS_PORT_CREATE 10
#define SYS_PORT_DESTROY 11
#define SYS_SEND        12
#define SYS_RECEIVE     13
#define SYS_REPLY       14
#define SYS_NOTIFY      15
#define SYS_WAIT_NOTIFY 16
#define SYS_RECEIVE_NOWAIT 17
#define SYS_POST           18
#define SYS_MMAP        20
#define SYS_MUNMAP      21
#define SYS_BRK         22
#define SYS_SHM_CREATE  23
#define SYS_SHM_MAP     24
#define SYS_SHM_UNMAP   25
#define SYS_HANDLE_CLOSE 30
#define SYS_CLOCK_GETTIME 40
#define SYS_NANOSLEEP   41
#define SYS_IO_PORT_IN  50  /* driver only */
#define SYS_IO_PORT_OUT 51  /* driver only */
#define SYS_IRQ_REGISTER 52 /* driver only */
#define SYS_IRQ_DONE    53  /* driver only */
#define SYS_IRQ_FIND_FREE   100 /* driver only: ask for a free IRQ line */
#define SYS_IRQ_WIRE_DEVICE 101 /* driver only: chipset PIRQ rewire */
#define SYS_IRQ_REGISTER_PCI 110 /* driver: register PCI INTx by bus:slot:func; kernel resolves line/GSI */
#define SYS_IRQ_PCI_CLAIM    111 /* driver: confirm the just-notified GSI is my device's (empirical resolve) */
#define SYS_INTR_DELIVERY_MODE 112 /* any process: 1=IOAPIC (native), 0=PIC (8259) */
#define SYS_MMAP_PHYS   54  /* driver only */
#define SYS_DMA_ALLOC   55  /* driver only */
#define SYS_SET_PRIVILEGES 56  /* driver only: make child a driver */
#define SYS_GET_HOME_DIR 57   /* get process home directory */

/* Device management (driver only) */
#define SYS_DEVICE_CLAIM    60
#define SYS_DEVICE_RELEASE  61
#define SYS_PCI_READ        64
#define SYS_PCI_WRITE       65

/* Async timers */
#define SYS_TIMER_SET       62
#define SYS_TIMER_CANCEL    63

/* Realtime */
#define SYS_SET_PRIORITY    66
#define SYS_WATCHDOG_SET    67

/* Entropy */
#define SYS_RANDOM          68
#define SYS_DEBUG_PRINT 99  /* driver only */

/* Spawn from userspace data */
#define SYS_SPAWN_DATA      74  /* spawn from memory (arg0=data, arg1=size) */

/* Boot */
#define SYS_GET_STARTUP     75  /* get Startup_modules text (arg0=buf, arg1=size) */

#define SYS_MEMINFO         76  /* query memory stats */
#define SYS_GETVERSION      77  /* get version array (5 x uint32_t: M.m.p.r.b) */
#define SYS_CLOCK_US        79  /* read TSC microsecond timestamp */
#define SYS_SLEEP_US        80  /* busy-wait N microseconds */
#define SYS_THREAD_EXIT     81  /* terminate calling thread only */
#define SYS_THREAD_CREATE   82  /* spawn thread in current process */
#define SYS_EVENT_CREATE    83  /* create event group → gid */
#define SYS_EVENT_SETCLEAR  84  /* set/clear/pulse/poison/reset */
#define SYS_EVENT_WAIT      85  /* wait on pattern with timeout */
#define SYS_EVENT_GETFLAGS  86  /* non-blocking read flags */
#define SYS_GETTIME         87  /* get real time (7 x uint32_t) */
#define SYS_FUTEX           88  /* op=ebx (0 wait/1 wake), uaddr=ecx, val=edx, timeout_ms=esi */

/* Kernel Registry */
#define SYS_REG_REGISTER    90  /* register service name → port */
#define SYS_REG_UNREGISTER  91  /* unregister service */
#define SYS_REG_FIND        92  /* find service port by name */
/* Slot 93: retired (was SYS_GET_DRIVERS). */
#define SYS_REG_WAIT        94  /* block until service registered (name, timeout_ms) */
#define SYS_GET_MODULE_FLAGS 95  /* get caller's raw Startup_modules flags string */

/* .mem shared objects */
#define SYS_MEM_LOAD        96

/* Live-CD ramdisk access. Mirrors kernel/syscall/syscall.h. Used
 * by DobFileSystem to serve the RAM-resident root volume when the
 * system was booted from CD. SYS_LIVE_QUERY takes no arguments and
 * returns the blob size in bytes (0 = not live). SYS_LIVE_READ
 * takes (lba, count, user_buf) and returns 0 on success, -1 on
 * any failure. */
#define SYS_LIVE_QUERY      102
#define SYS_LIVE_READ       103

/* SYS_REBOOT: hard-reset via the 8042 keyboard controller. Restricted
 * to driver-privileged callers. Never returns under normal conditions. */
#define SYS_REBOOT          104

/* Driver-only non-preemption guard (defer wake-time preemption of caller).
 * Must match kernel/syscall/syscall.h. */
#define SYS_ENTER_CRITICAL  105
#define SYS_EXIT_CRITICAL   106

/* Driver-only: enable Message Signaled Interrupts on a PCI device.
 * Must match kernel/syscall/syscall.h. */
#define SYS_MSI_ENABLE      107
#define SYS_DMA_FREE        108 /* free a DMA buffer from dma_alloc (arg0=vaddr) */
#define SYS_DMA_VERIFY      109 /* [phys,len] within a caller-owned DMA buffer? (arg0=phys,arg1=len)->1/0 */

/* Port generation / ABA-safe delivery. Mirrors kernel/syscall/syscall.h.
 * SYS_PORT_GEN(port_id) -> current generation of the live port at that
 * id, or 0 if none is live. SYS_POST_CK(port_id, gen, msg) posts only
 * if the port still carries `gen`; a recycled id returns IPC_ERR_DEAD
 * instead of delivering to the stranger who now owns the id. */
#define SYS_PORT_GEN        113
#define SYS_POST_CK         114

/* SYS_WATCH_PORT(target_port, target_gen, notify_port, code): when the
 * port (target_port, target_gen) dies, the kernel posts `code` on the
 * caller's `notify_port` with arg0=target_port, arg1=target_gen. Keyed
 * by (id, gen) so it is immune to id recycling. Returns 0 (watching),
 * 1 (target already dead — reap now), -1 (table full / bad notify_port).
 * Replaces liveness polling with an event. */
#define SYS_WATCH_PORT      115
/* TEST: panic deliberato (mirror di kernel/syscall/syscall.h). */
#define SYS_PANIC_TEST      116
#define SYS_SET_PANIC_FB      117  /* driver video: registra il framebuffer per il panic */
#define SYS_TASK_SNAPSHOT     118  /* fotografia processi+CPU per il task manager */
#define SYS_PROC_SET_PRIORITY 119  /* renice di un processo per PID */

static inline int syscall0(int num)
{
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static inline int syscall1(int num, int a1)
{
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num), "b"(a1) : "memory");
    return ret;
}

static inline int syscall2(int num, int a1, int a2)
{
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2) : "memory");
    return ret;
}

static inline int syscall3(int num, int a1, int a2, int a3)
{
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3) : "memory");
    return ret;
}

static inline int syscall4(int num, int a1, int a2, int a3, int a4)
{
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4) : "memory");
    return ret;
}

static inline int syscall5(int num, int a1, int a2, int a3, int a4, int a5)
{
    int ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5) : "memory");
    return ret;
}

/* Video boomerang */
#define SYS_REGISTER_VIDEO_DRIVER 97  /* driver only: arg0=entry vaddr */
#define VIDEO_BOOMERANG_INT       0x85

/* dv_call0..5 — fire `int 0x85` with the given dobVideo opcode and
 * up to 5 scalar args.  Returns the driver's int32_t (or
 * DV_ERR_NOTREADY when the kernel boomerang slot is empty / no driver
 * registered).  Argument-passing convention mirrors syscallN.
 *
 * BUG-FIX NOTE 1 (call-in): the kernel boomerang interprets EDI as
 * the payload size and ESI as the payload pointer.  If we left
 * ESI/EDI unconstrained, the compiler could give us whatever residual
 * value happened to be in those registers — and if EDI ended up
 * non-zero, the kernel would `rep movsb` from ESI as if there were a
 * payload, faulting at whatever address ESI held.  Every dv_callN
 * that carries no payload therefore feeds explicit zeros to ESI/EDI.
 *
 * BUG-FIX NOTE 2 (call-out): the int 0x85 boomerang ROUND TRIP
 * clobbers EBX, ECX, EDX, ESI and EDI — EBX is the CR3 scratch, the
 * driver's C dispatcher trashes ECX/EDX per the cdecl scratch rule,
 * and ESI/EDI are left pointing into the kernel payload buffer by the
 * copy-out `rep movsb`.  Only EAX (return code), EBP and ESP survive.
 * These registers MUST therefore be declared modified: the ones we
 * feed inputs to are read-write ("+") operands, the rest sit in the
 * clobber list.  Declaring them as plain inputs is a latent
 * miscompilation — the compiler may keep a live value (e.g. a stack
 * pointer it placed in ESI for the "S" input) and reuse the stale,
 * now-kernel-VA register after the call, faulting on the next access. */

static inline int dv_call0_raw(int op)
{
    int ret;
    int s = 0, d = 0;                  /* ESI/EDI: zeroed in, clobbered out */
    __asm__ volatile ("int $0x85"
        : "=a"(ret), "+S"(s), "+D"(d)
        : "a"(op)
        : "memory", "ebx", "ecx", "edx");
    return ret;
}

static inline int dv_call1_raw(int op, int a1)
{
    int ret;
    int s = 0, d = 0;
    __asm__ volatile ("int $0x85"
        : "=a"(ret), "+b"(a1), "+S"(s), "+D"(d)
        : "a"(op)
        : "memory", "ecx", "edx");
    return ret;
}

static inline int dv_call2_raw(int op, int a1, int a2)
{
    int ret;
    int s = 0, d = 0;
    __asm__ volatile ("int $0x85"
        : "=a"(ret), "+b"(a1), "+c"(a2), "+S"(s), "+D"(d)
        : "a"(op)
        : "memory", "edx");
    return ret;
}

static inline int dv_call3_raw(int op, int a1, int a2, int a3)
{
    int ret;
    int s = 0, d = 0;
    __asm__ volatile ("int $0x85"
        : "=a"(ret), "+b"(a1), "+c"(a2), "+d"(a3), "+S"(s), "+D"(d)
        : "a"(op)
        : "memory");
    return ret;
}

/* dv_call4 / dv_call5 — pass scalar a4 in ESI and a5 in EDI.  These
 * overload the registers the boomerang would otherwise use for payload
 * pointer/size, so the kernel will misinterpret non-zero a4/a5 as a
 * payload reference.  Not used in libdob today; keep them for
 * legacy/diagnostic purposes only.  If a future caller needs 4+ args
 * AND wants the kernel to not parse them as payload, the right answer
 * is a dv_call_pl with a small struct, not these.  Marked deprecated
 * in spirit, no enforcement yet. */
static inline int dv_call4_raw(int op, int a1, int a2, int a3, int a4)
{
    int ret;
    int d = 0;                         /* EDI: zeroed in (no payload) */
    __asm__ volatile ("int $0x85"
        : "=a"(ret), "+b"(a1), "+c"(a2), "+d"(a3), "+S"(a4), "+D"(d)
        : "a"(op)
        : "memory");
    return ret;
}

static inline int dv_call5_raw(int op, int a1, int a2, int a3, int a4, int a5)
{
    int ret;
    __asm__ volatile ("int $0x85"
        : "=a"(ret), "+b"(a1), "+c"(a2), "+d"(a3), "+S"(a4), "+D"(a5)
        : "a"(op)
        : "memory");
    return ret;
}

/* dv_call_pl — boomerang call with a payload buffer.  EBX/ECX/EDX
 * carry up to 3 scalar args; ESI = pointer to caller-AS buffer; EDI
 * = buffer size in bytes.  Kernel copies the buffer into a global
 * kernel scratch (capped at 16 KiB) before CR3 switch, sets ESI to
 * the kernel-VA copy for the driver, and copies the buffer back to
 * the caller after the driver returns — so the driver can use the
 * payload as a bidirectional struct (e.g. dv_surface_create_io with
 * IN desc + OUT handle).
 *
 * payload_size of 0 with a NULL pointer is allowed and equivalent to
 * dv_call3.  Size > 16384 returns DV_ERR_INVAL without invoking the
 * driver. */
static inline int dv_call_pl_raw(int op, int a1, int a2, int a3,
                             void *payload, unsigned payload_size)
{
    int ret;
    __asm__ volatile ("int $0x85"
        : "=a"(ret), "+b"(a1), "+c"(a2), "+d"(a3),
          "+S"(payload), "+D"(payload_size)
        : "a"(op)
        : "memory");
    return ret;
}

/* ---- retry su DV_ERR_BUSY -------------------------------------------
 * Il boomerang kernel, a lock conteso oltre il budget di spin (ring 0,
 * IF=0: non puo' attendere), MOLLA con DV_ERR_BUSY. E' una condizione
 * TRANSITORIA per design — il bail esiste per non murare la CPU, non
 * come risposta al chiamante. Senza retry, ogni contesa tra due
 * dv_call su CPU diverse (o una dv_call durante una fase in-driver
 * lunga) diventava un errore duro e SILENZIOSO sulle chiamate
 * fondative della GUI: boot bimodale senza alcuna firma nel log.
 * Il retry vive QUI, nella primitiva: ogni chiamante lo eredita.
 * yield() tra i tentativi: chi tiene il lock gira altrove e non
 * attende mai noi — progresso garantito. NOTREADY non si ritenta
 * (driver assente = stato reale). Nessun driver usa DV_ERR_BUSY come
 * risposta applicativa (verificato repo-wide); se un giorno servisse,
 * dovra' usare un codice diverso. */

#define DV_CALL_ERR_BUSY_       (-10)   /* = DV_ERR_BUSY (dob/video.h) */
#define DV_CALL_BUSY_RETRY_MAX  4096u

#define DV_CALL_RETRY_(expr)                                   \
    do {                                                       \
        unsigned dv_try_;                                      \
        int dv_rc_;                                            \
        for (dv_try_ = 0; ; dv_try_++) {                       \
            dv_rc_ = (expr);                                   \
            if (dv_rc_ != DV_CALL_ERR_BUSY_ ||                 \
                dv_try_ >= DV_CALL_BUSY_RETRY_MAX)             \
                return dv_rc_;                                 \
            syscall0(SYS_YIELD);                               \
        }                                                      \
    } while (0)

static inline int dv_call0(int op)
{ DV_CALL_RETRY_(dv_call0_raw(op)); }

static inline int dv_call1(int op, int a1)
{ DV_CALL_RETRY_(dv_call1_raw(op, a1)); }

static inline int dv_call2(int op, int a1, int a2)
{ DV_CALL_RETRY_(dv_call2_raw(op, a1, a2)); }

static inline int dv_call3(int op, int a1, int a2, int a3)
{ DV_CALL_RETRY_(dv_call3_raw(op, a1, a2, a3)); }

static inline int dv_call4(int op, int a1, int a2, int a3, int a4)
{ DV_CALL_RETRY_(dv_call4_raw(op, a1, a2, a3, a4)); }

static inline int dv_call5(int op, int a1, int a2, int a3, int a4, int a5)
{ DV_CALL_RETRY_(dv_call5_raw(op, a1, a2, a3, a4, a5)); }

static inline int dv_call_pl(int op, int a1, int a2, int a3,
                             void *payload, unsigned int size)
{ DV_CALL_RETRY_(dv_call_pl_raw(op, a1, a2, a3, payload, size)); }

#endif
