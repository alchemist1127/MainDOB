#ifndef MAINDOB_LIBC_UNISTD_H
#define MAINDOB_LIBC_UNISTD_H

#include <sys/types.h>
#include <sys/syscall.h>

/* === Process === */

static inline void
_exit(int code)
{
    syscall1(SYS_EXIT, code);
    for (;;) {}
}

static inline void     yield(void)           { syscall0(SYS_YIELD); }
static inline pid_t    getpid(void)          { return (pid_t)syscall0(SYS_GETPID); }
static inline void     sleep_ms(uint32_t ms) { syscall1(SYS_NANOSLEEP, (int)ms); }
static inline uint32_t clock_ms(void)        { return (uint32_t)syscall0(SYS_CLOCK_GETTIME); }

/* sleep_us: kernel busy-wait (does NOT yield the CPU / de-schedule).  The
 * kernel hard-caps a single call at 100 µs, so this wrapper is only valid for
 * us <= 100.  Use busy_wait_us() for longer non-yielding waits. */
static inline void     sleep_us(uint32_t us) { syscall1(SYS_SLEEP_US, (int)us); }

/* busy_wait_us: non-yielding delay of arbitrary length, built from ≤100 µs
 * busy-wait chunks.  Unlike sleep_ms (SYS_NANOSLEEP, which de-schedules the
 * caller and lets other tasks run), this keeps the calling thread on the CPU
 * for the whole duration.  Pair it with enter_critical/exit_critical around
 * hardware sequences that must be atomic against other threads. */
static inline void
busy_wait_us(uint32_t us)
{
    while (us > 100u) { sleep_us(100u); us -= 100u; }
    if (us) sleep_us(us);
}

/* enter_critical / exit_critical: driver-only non-preemption guard.  While
 * inside, the scheduler will not switch to another thread that wakes up
 * (it's deferred until exit), so a timing-sensitive hardware sequence runs
 * atomically against other threads.  Counted/nestable.  Does NOT disable
 * interrupts — IRQ handlers still run.  Return 0 on success, -1 if the
 * caller isn't a driver.  Keep the guarded region SHORT: it delays
 * higher-priority threads until you exit. */
static inline int enter_critical(void) { return syscall0(SYS_ENTER_CRITICAL); }
static inline int exit_critical(void)  { return syscall0(SYS_EXIT_CRITICAL); }

/* Get real time from RTC-calibrated clock.
 * buf must point to 7 × uint32_t: year, month, day, hour, minute, second, ms */
static inline int     gettime(uint32_t *buf) { return syscall1(SYS_GETTIME, (int)buf); }

static inline int
waitpid(pid_t pid)
{
    return syscall1(SYS_WAIT, (int)pid);
}

static inline int
kill(pid_t pid)
{
    return syscall1(SYS_KILL, (int)pid);
}

static inline int
proc_status(pid_t pid)
{
    return syscall1(SYS_PROC_STATUS, (int)pid);
}

/* Is this module a driver? Returns 1 (driver) or 0 (client). */
static inline int
is_driver(pid_t pid)
{
    return syscall1(SYS_GET_PRIVILEGES, (int)pid);
}

/* Promote a child module to driver. Only drivers can call this.
 * Used by init and hotplug after spawning a driver module. */
static inline int
make_driver(pid_t pid)
{
    return syscall1(SYS_SET_PRIVILEGES, (int)pid);
}

/* Power off the system.  Kernel-mediated ACPI shutdown — no driver
 * privilege required.  Never returns on success; returns -1 only if the
 * syscall itself is unreachable (which shouldn't happen).  Any process
 * may invoke this in the current build; intended caller is dobinterface
 * in response to the shutdown UI command. */
static inline int
power_off(void)
{
    return syscall0(SYS_SHUTDOWN);
}

/* Reboot the system.  Kernel-mediated 8042 reset.  Restricted to driver
 * or primary processes (the trusted core, e.g. dobinterface); returns -1
 * if the caller lacks the privilege or the syscall is unreachable.  Never
 * returns on success.  Intended caller is dobinterface's reboot UI. */
static inline int
reboot(void)
{
    return syscall0(SYS_REBOOT);
}

/* Get the home directory (sandbox root) of a process.
 * Only the process itself or a driver can query another process's home_dir.
 * Returns 0 on success, -1 on error. */
static inline int
get_home_dir(pid_t pid, char *buf, uint32_t size)
{
    return syscall3(SYS_GET_HOME_DIR, (int)pid, (int)buf, (int)size);
}

/* === IPC === */

static inline int port_create(void)           { return syscall0(SYS_PORT_CREATE); }
static inline int port_destroy(int port_id)   { return syscall1(SYS_PORT_DESTROY, port_id); }

/* === I/O Ports (requires PRIV_DRIVER) === */

static inline uint8_t io_inb(uint16_t port)
{
    return (uint8_t)syscall2(SYS_IO_PORT_IN, port, 1);
}

static inline uint16_t io_inw(uint16_t port)
{
    return (uint16_t)syscall2(SYS_IO_PORT_IN, port, 2);
}

static inline uint32_t io_inl(uint16_t port)
{
    return (uint32_t)syscall2(SYS_IO_PORT_IN, port, 4);
}

static inline void io_outb(uint16_t port, uint8_t val)
{
    syscall3(SYS_IO_PORT_OUT, port, 1, val);
}

static inline void io_outw(uint16_t port, uint16_t val)
{
    syscall3(SYS_IO_PORT_OUT, port, 2, val);
}

static inline void io_outl(uint16_t port, uint32_t val)
{
    syscall3(SYS_IO_PORT_OUT, port, 4, (int)val);
}

/* === IRQ === */

/* Register an IRQ: notifications will arrive on given IPC port */
static inline int irq_register(uint32_t irq_num, uint32_t notify_port)
{
    return syscall2(SYS_IRQ_REGISTER, (int)irq_num, (int)notify_port);
}

static inline void irq_done(uint32_t irq_num)
{
    syscall1(SYS_IRQ_DONE, (int)irq_num);
}

/* Ask the kernel for a free IRQ line (reserved for caller with a 2s lease).
 * Returns the line number on success, -1 if none available.
 * Call this only after irq_register() on your default line has failed. */
static inline int irq_find_free(void)
{
    return syscall0(SYS_IRQ_FIND_FREE);
}

/* Reprogram the chipset so the PCI device (bus, slot, func) delivers its
 * interrupt on 'new_line'. Use after irq_find_free to make the hardware
 * actually fire on the granted line. Returns 0 on success, -1 on failure. */
static inline int irq_wire_device(uint8_t bus, uint8_t slot, uint8_t func, uint8_t new_line)
{
    uint32_t bsf = ((uint32_t)bus << 16) | ((uint32_t)slot << 8) | (uint32_t)func;
    return syscall2(SYS_IRQ_WIRE_DEVICE, (int)bsf, (int)new_line);
}

/* Register for a PCI device's INTx interrupt BY DEVICE IDENTITY, letting
 * the kernel work out where the interrupt actually lands. This is the
 * routing-agnostic replacement for irq_register() on PCI devices: a driver
 * cannot know its IRQ line in IOAPIC mode (the PCI Interrupt Line byte is
 * an 8259-only value there), so it hands the kernel its bus:slot:func and
 * the kernel resolves delivery.
 *
 * Return value:
 *   > 0  : resolved immediately to this line/GSI number — use it for
 *          irq_done(). Always the case under PIC delivery (the kernel read
 *          the device's Interrupt Line), and under IOAPIC delivery when the
 *          device's GSI was already learned.
 *   == 0 : IOAPIC delivery, GSI not yet known — resolution is PENDING. The
 *          GSI will arrive in the bitmask (arg0) of the first IRQ
 *          notification; on that notification, if your device's status
 *          shows it was the source, call irq_pci_claim(gsi) to bind, then
 *          use that gsi for irq_done() from then on.
 *   < 0  : error (not a driver, no Interrupt Line in PIC mode, tables full).
 *
 * Drivers for non-PCI fixed-line devices (floppy, keyboard) keep using
 * irq_register() with their known line. */
static inline int irq_register_pci(uint8_t bus, uint8_t slot, uint8_t func,
                                   uint32_t notify_port)
{
    uint32_t bsf = ((uint32_t)bus << 16) | ((uint32_t)slot << 8) | (uint32_t)func;
    return syscall2(SYS_IRQ_REGISTER_PCI, (int)bsf, (int)notify_port);
}

/* Confirm, during empirical GSI resolution (when irq_register_pci returned
 * 0), that the GSI just delivered in a notification belongs to THIS
 * device. Extract the gsi from the notify bitmask (the set bit index) and
 * pass it here only after verifying your device's status register shows it
 * as the interrupt source. The kernel then binds your device to that GSI
 * permanently. Returns 0 on success. */
static inline int irq_pci_claim(uint32_t gsi)
{
    return syscall1(SYS_IRQ_PCI_CLAIM, (int)gsi);
}

/* Report which controller delivers device interrupts on this machine:
 * returns 1 = IOAPIC (native APIC delivery active), 0 = 8259 PIC. The
 * system-wide truth straight from the kernel, independent of whether any
 * device has resolved its GSI. Meant for diagnostics that must show the
 * delivery mode on hardware where boot logs aren't reachable. */
static inline int intr_delivery_mode(void)
{
    return syscall0(SYS_INTR_DELIVERY_MODE);
}

/* === MMIO mapping === */

/* Map physical memory into process (for MMIO access). Returns virtual address. */
static inline void *mmap_phys(uint32_t phys_addr, uint32_t size)
{
    return (void *)syscall2(SYS_MMAP_PHYS, (int)phys_addr, (int)size);
}

/* Allocate physically contiguous DMA buffer.
 * Returns virtual address; *phys_out receives the physical address.
 * Driver modules only. */
static inline void *dma_alloc(uint32_t size, uint32_t *phys_out)
{
    int virt, phys;
    __asm__ volatile (
        "int $0x80"
        : "=a"(virt), "=b"(phys)
        : "a"(SYS_DMA_ALLOC), "b"(size)
        : "memory"
    );
    if (phys_out) *phys_out = (uint32_t)phys;
    return (void *)virt;
}

/* Free a DMA buffer previously returned by dma_alloc. Driver modules only.
 * Returns 0 on success, -1 on a bad/unowned handle. */
static inline int dma_free(void *virt)
{
    int rc;
    __asm__ volatile (
        "int $0x80"
        : "=a"(rc)
        : "a"(SYS_DMA_FREE), "b"((uint32_t)virt)
        : "memory"
    );
    return rc;
}

/* Check that [phys, phys+len) lies entirely within a DMA buffer owned by this
 * driver. Returns 1 if so, 0 otherwise. Gate every DMA descriptor target (a
 * zero-copy data pointer, a PRDT dba, ...) through this before submitting, so
 * an out-of-bounds address is refused instead of silently DMA'd over other
 * memory. Driver modules only. */
static inline int dma_verify(uint32_t phys, uint32_t len)
{
    int rc;
    __asm__ volatile (
        "int $0x80"
        : "=a"(rc)
        : "a"(SYS_DMA_VERIFY), "b"(phys), "c"(len)
        : "memory"
    );
    return rc;
}

/* Backward compat alias for hotplug/init code that uses set_privileges */
static inline int
set_privileges(pid_t pid, uint32_t unused)
{
    (void)unused;
    return make_driver(pid);
}

/* === Debug ===
 * Emette `str` sul log del kernel: lato kernel sys_debug_print lo passa a
 * kprintf, che scrive sia sul VGA testo sia sul seriale COM1 — cioe' la
 * console di chiamata (lo stdio collegato a -serial). L'emissione e'
 * INCONDIZIONATA anche in release: le diagnostiche dei driver ([ata], [uhci],
 * ...) servono proprio quando si legge il boot dal terminale, non solo nei
 * build debug. Il costo e' un syscall per chiamata; per silenziarlo in un
 * run di misura si gate lato kernel, non qui. NB: sys_debug_print accetta
 * solo chiamanti con privilegio driver (caller_is_driver): da un processo
 * non-driver il syscall ritorna -1 e non stampa nulla. */

static inline void debug_print(const char *str)
{
    syscall1(SYS_DEBUG_PRINT, (int)str);
}

/* === Heap management === */

/* Adjust program break. Returns new break, or (void*)-1 on error.
 * Pass NULL to query current break. */
static inline void *sbrk(int increment)
{
    uint32_t cur = (uint32_t)syscall1(SYS_BRK, 0);
    if (increment == 0)
        return (void *)cur;
    int32_t result = syscall1(SYS_BRK, (int)(cur + increment));
    if (result == -1)
        return (void *)-1;
    return (void *)cur;  /* Return old break (start of new region) */
}

static inline int brk(void *addr)
{
    int32_t result = syscall1(SYS_BRK, (int)addr);
    return (result == -1) ? -1 : 0;
}

/* Map anonymous pages at virt_addr (page-aligned). Returns 0 on success. */
static inline int mmap_anon(void *virt_addr, uint32_t num_pages)
{
    return syscall2(SYS_MMAP, (int)virt_addr, (int)num_pages);
}

/* Unmap pages at virt_addr. Returns 0 on success. */
static inline int munmap(void *virt_addr, uint32_t num_pages)
{
    return syscall2(SYS_MUNMAP, (int)virt_addr, (int)num_pages);
}

/* Shared memory: create a region of `size` bytes.
 * Returns shm_id (>= 0) on success, -1 on error.
 * *out_vaddr receives the mapped virtual address. */
static inline int shm_create(uint32_t size, uint32_t *out_vaddr)
{
    return syscall2(SYS_SHM_CREATE, (int)size, (int)out_vaddr);
}

/* Shared memory: map an existing region by shm_id.
 * Returns 0 on success, -1 on error.
 * *out_vaddr receives the mapped virtual address. */
static inline int shm_map(int shm_id, uint32_t *out_vaddr)
{
    return syscall2(SYS_SHM_MAP, (int)shm_id, (int)out_vaddr);
}

/* Shared memory: unmap a region. Frees vaddr (recyclable), decrements
 * refcount. When refcount reaches 0, physical frames are freed. */
static inline int shm_unmap(int shm_id)
{
    return syscall1(SYS_SHM_UNMAP, shm_id);
}

/* Device management (driver only) */

/* Claim a device: IRQ + MMIO range + IPC notification port.
 * Returns claim_id (>= 0) or -1. */
static inline int device_claim(uint32_t irq, uint32_t phys_base,
                                uint32_t phys_size, uint32_t port)
{
    return syscall4(SYS_DEVICE_CLAIM, (int)irq, (int)phys_base,
                    (int)phys_size, (int)port);
}

/* Release a previously claimed device. */
static inline int device_release(int claim_id)
{
    return syscall1(SYS_DEVICE_RELEASE, claim_id);
}

/* Read PCI config space. bus/slot/func/offset. Returns 32-bit value. */
static inline uint32_t pci_config_read(uint32_t bus, uint32_t slot,
                                        uint32_t func, uint32_t offset)
{
    return (uint32_t)syscall4(SYS_PCI_READ, (int)bus, (int)slot,
                              (int)func, (int)offset);
}

/* Write PCI config space. bsf = (bus<<16 | slot<<8 | func). */
static inline int pci_config_write(uint32_t bus, uint32_t slot,
                                    uint32_t func, uint32_t offset,
                                    uint32_t value)
{
    uint32_t bsf = (bus << 16) | (slot << 8) | func;
    return syscall3(SYS_PCI_WRITE, (int)bsf, (int)offset, (int)value);
}

/* Enable MSI on a PCI device. `cap_offset` is the MSI capability offset
 * the caller found by walking the device's capability list (start at the
 * Capabilities Pointer, config offset 0x34; each capability's low byte is
 * its id — 0x05 is MSI — and the next byte points to the following one).
 * On success the kernel allocates a vector, programs the device to fire
 * it, and routes interrupts to `notify_port` as notifications. Returns the
 * vector (>= 0x50) or -1. MSI is edge-triggered: no irq_done() is needed,
 * unlike the legacy line path. */
static inline int msi_enable(uint32_t bus, uint32_t slot, uint32_t func,
                             uint32_t cap_offset, uint32_t notify_port)
{
    uint32_t bsf = (bus << 16) | (slot << 8) | func;
    return syscall3(SYS_MSI_ENABLE, (int)bsf, (int)cap_offset,
                    (int)notify_port);
}

/* Async timers */

/* Create an async timer. After delay_ms, an IPC message (code=70)
 * is posted to port. If repeat != 0, timer fires periodically.
 * Returns timer_id (>= 0) or -1. */
static inline int timer_set(uint32_t port, uint32_t delay_ms, int repeat)
{
    return syscall3(SYS_TIMER_SET, (int)port, (int)delay_ms, repeat);
}

/* Cancel a pending timer. */
static inline int timer_cancel_async(int timer_id)
{
    return syscall1(SYS_TIMER_CANCEL, timer_id);
}

/* Realtime */

/* Set thread priority: 0=realtime, 1=high, 2=normal, 3=low.
 * Only drivers can set priority 0. */
static inline int set_priority(uint32_t priority)
{
    return syscall1(SYS_SET_PRIORITY, (int)priority);
}

/* Arm or refresh process watchdog. If not refreshed within
 * timeout_ms, the kernel kills the process. Pass 0 to disable. */
static inline int watchdog_set(uint32_t timeout_ms)
{
    return syscall1(SYS_WATCHDOG_SET, (int)timeout_ms);
}

/* Boot — Startup_modules */

/* Get the Startup_modules text from the kernel.
 * Returns length of text copied, or -1 on error. */
static inline int get_startup_text(char *buf, uint32_t buf_size)
{
    return syscall2(SYS_GET_STARTUP, (int)buf, (int)buf_size);
}

/* Spawn from data */

/* Spawn a process from ELF data already in memory.
 * Used by programs that read .mdl files via DobFileSystem
 * and want to launch them without the kernel touching the disk. */
static inline int spawn_from_data(const void *elf_data, uint32_t size)
{
    return syscall2(SYS_SPAWN_DATA, (int)elf_data, (int)size);
}

/* Same as spawn_from_data, but passes a "name hint" that becomes the
 * basename of the child's home_dir. Without this, every spawned child
 * lands in /SYSTEM/PROGRAMS/spawned/ — which breaks DobFileSystem's
 * config_area_allowed() whitelist (it keys off the bubble name).
 * The hint is sanitized kernel-side (path separators rejected). */
static inline int spawn_from_data_named(const void *elf_data, uint32_t size,
                                        const char *name_hint)
{
    return syscall3(SYS_SPAWN_DATA, (int)elf_data, (int)size, (int)name_hint);
}

/* Full form with argv blob.
 *
 * argv_blob layout (built by libdob/spawn.h, opaque to most callers):
 *     [uint32_t argc]
 *     [uint32_t blob_size]   — total bytes including this header
 *     [argc NUL-terminated strings, packed]
 *
 * argv_blob may be NULL (equivalent to argc=0). When non-NULL, the
 * kernel copies the blob into its own memory and populates the child's
 * user stack so crt0 can pass argc/argv to main(). Most userspace code
 * should call spawn_file() from <dob/spawn.h> rather than touching
 * this directly — the libdob wrapper handles serialisation. */
static inline int spawn_from_data_named_argv(const void *elf_data, uint32_t size,
                                             const char *name_hint,
                                             const void *argv_blob)
{
    return syscall4(SYS_SPAWN_DATA, (int)elf_data, (int)size,
                    (int)name_hint, (int)argv_blob);
}

/* Entropy */

/* Fill buffer with random bytes from kernel entropy pool.
 * Max 4096 bytes per call. Returns 0 on success, -1 on error. */
static inline int get_random(void *buf, uint32_t len)
{
    return syscall2(SYS_RANDOM, (int)buf, (int)len);
}

/* Convenience: return a single random uint32_t. */
static inline uint32_t random_u32(void)
{
    uint32_t val = 0;
    get_random(&val, sizeof(val));
    return val;
}

#endif /* MAINDOB_LIBC_UNISTD_H */
