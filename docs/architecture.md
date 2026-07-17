# MainDOB — Architecture & Development Plan

## Project Identity

MainDOB is a microkernel operating system for x86 (32-bit), written in C and assembly.
Target platform: QEMU. Bootloader: GRUB (Multiboot 1).

### Design Principles

- **Microkernel**: drivers and services run in isolated userspace processes
- **Entry Point System**: programs expose callable functions via auto-generated stubs and transparent IPC
- **Centralized config**: single protected DB, only the config server can access it
- **Native sandboxing**: each program confined to its own folder; DATA/ access requires capabilities
- **GUI with contextual panel**: square windows, right-side panel as universal menu, blue theme

## Project Structure

```
maindob/
├── Makefile, toolchain.mk, config.mk, setup.sh, Dockerfile
├── iso/boot/grub/grub.cfg
├── config/             Initial config files copied to disk: Drivers, DAS/*.das
├── kernel/
│   ├── boot/             entry.asm, multiboot.h, boot_info, bootfs (PIO ATA), startup, disk
│   ├── arch/x86/         gdt, idt, isr (asm + C), irq, paging
│   ├── mm/               pmm (bitmap), vmm, kheap, vregion, entropy
│   ├── proc/             process, thread, context (asm), scheduler, wait, elf, workqueue
│   ├── ipc/              port, channel, message
│   ├── irq/              PCI IRQ discovery + PIRQ rewire
│   ├── sync/             spinlock, mutex, semaphore, rwlock, event_group, barrier, atomic
│   ├── time/             clock (PIT + timer_mgr), rtc
│   ├── syscall/          syscall dispatcher + ~60 syscall handlers
│   ├── lib/              types, string, printf, bitmap, list
│   ├── registry.c/.h     in-kernel service registry
│   ├── kernel.h/.c       kmain(), init sequence, module loader
│   └── linker.ld
├── libc/                 crt0.asm, string, stdio, stdlib (malloc/free), unistd, syscall
│   └── user.ld           userspace linker script (loads at 0x00400000)
├── libdob/               Entry Point runtime: ipc.c, registry.c, server.c, font.c,
│                         window_servo.c, thread.c (+ inline RT helpers in DobRT.h)
├── libdobui/             DobUITools — userspace UI controls (button, textbox, slider, ...)
├── boot/                 Userspace boot services (loaded from Startup_modules)
│   ├── console/            VGA text output
│   ├── config/             Centralized settings database
│   ├── DobFileSystem/      FAT32 filesystem with sandbox enforcement
│   ├── dobinterface/       Window manager + GUI server
│   ├── popups/             System popup dialogs
│   ├── inputd/             Keyboard/mouse input router
│   ├── hotplug/            PCI enumeration, driver matching, device bubbles, DAS
│   ├── floppyprobe/        Floppy media insertion poller
│   └── audioplayer/        Audio decode + playback (WAV + MP2; MP3/OGG decoders are stubs)
├── drivers/              ata, ahci, rtc, usb_uhci/ehci/xhci, ac97, floppy
├── programs/             DobFiles, DobPicture, editor, modules, calculator, snake, tetris,
│                         minesweeper, solitaire, arrowsweeper, dobplayer, tonegen,
│                         widget_test, uidemo, benchmark
└── tools/                mkbootdisk.sh, grub_disk.cfg
```

## Hotplug System (Device Bubbles)

Hardware detection and driver management uses a "bubble" model. Each physical
device paired with its driver lives in an isolated bubble that can be created,
monitored, and torn down independently.

### Algorithm (high level)

```
main:
    scan PCI bus → build hardware table
    for each device: match driver from manifest → spawn bubble
    register as "hotplug"

    loop forever:
        handle IPC (READY, RELEASED, FAILED, queries)
        if rescan timer expired:
            diff old vs new scan
            new devices → bubble_create
            gone devices → bubble_teardown
        reap dead/timed-out bubbles
```

### Bubble lifecycle

```
[hardware detected]
    ↓
SPAWNING  →  hotplug spawns driver .mdl process
    ↓
ATTACHING →  hotplug sends ATTACH with device info (BARs, IRQ, bus:slot:func)
    ↓
LIVE      →  driver sends READY with its service port
    ↓            ↓ (driver crash detected)
    ↓       bubble reaped, re-matched on next scan
    ↓
[hardware removed or RESCAN detects absence]
    ↓
DETACHING →  hotplug sends DETACH, starts 2-second deadline
    ↓            ↓ (driver unresponsive)
    ↓       force-kill after deadline
    ↓
DEAD      →  bubble freed, service unregistered
```

### Boot Dependency System (needs:)

Init launches modules in **dependency-aware waves** rather than blind sequential order.
Each module in Startup_modules can declare dependencies via the `needs:` flag:

```
# Syntax
module.mdl    flags    needs:dep1,dep2     # AND — all must be registered
module.mdl    flags    needs:dep1|dep2     # OR  — any one is enough
module.mdl    flags                        # No deps — launches immediately
```

**Algorithm:**
The kernel reads Startup_modules and spawns every module in order.
There is no init process — the registry is built into the kernel
(accessible via SYS_REG_REGISTER / SYS_REG_FIND / SYS_REG_UNREGISTER
syscalls) and critical process monitoring is handled by the kernel
directly in the process teardown path.

Modules marked `critical` are automatically respawned by the kernel
if they die. The respawn is event-driven (triggered by process_destroy,
deferred via workqueue) — zero polling overhead.

**Example Startup_modules:**
```
# Registry is built into the kernel — services register via syscall
# console, rtc, serial are intentionally not started by default —
# see Startup_modules for rationale.
hotplug.mdl                                 driver
inputd.mdl                                  driver
config.mdl
DobFileSystem.mdl
dobinterface.mdl                            driver critical
popups.mdl
/SYSTEM/PROGRAMS/DobFiles/DobFiles.mdl
```

Boot ordering is determined by the file order. Services keep their
internal reconnect logic (`_dob_call_reconnect`) for crash recovery.

### What the kernel launches vs what hotplug spawns

The kernel launches everything in Startup_modules — **non-PCI legacy
devices** (fixed I/O ports) and OS services:
config, hotplug, inputd, DobFileSystem

Hotplug handles **PCI-discoverable devices** (matched by vendor:device or class:subclass):
ata, ahci, usb_uhci, usb_ehci, usb_xhci

### Sudden removal

When hardware disappears between rescans:
1. Hotplug detects the absence in pci_rescan_and_diff()
2. bubble_teardown() sends DETACH to the driver
3. If the driver responds with RELEASED within 2 seconds: clean exit
4. If the driver is unresponsive: force-killed, bubble reaped
5. Service name unregistered from registry so no new clients connect
6. Existing clients get IPC_ERR_DEAD on their next call

### Driver database — DAS

Hardware-to-driver routing lives in `/SYSTEM/CONFIG/DAS/`. Each `.das`
file describes one supported device class: bus type, signature
(vendor:device, class:subclass, prog_if, or legacy I/O base), the
driver `.mdl` to spawn, the service name to register, and — for GUI
devices — the desktop icon and the action / menu sequences executed
on user interaction. `kind = system` devices are spawned at match
time with no UI; `kind = GUI` defers spawning until the user
activates the icon.

When multiple `.das` files match the same device (e.g. a chip-specific
driver and a generic class fallback), the matcher picks the most
specific one via a score: `vendor:device` (1000) > `class+subclass+
prog_if` (70) > `class+subclass` (20). This makes a USB-xHCI rule
(`prog_if = 0x30`) win over a hypothetical generic USB rule, and a
quirk-specific S3 driver win over a VBE catch-all.

Adding a new driver: drop `<name>.mdl` in `/SYSTEM/DRIVERS/<name>/`
and `<name>.das` in `/SYSTEM/CONFIG/DAS/`. No edits to hotplug, no
recompilation.

## Architecture Decisions

| Decision | Choice | Rationale |
|---|---|---|
| CPU arch | x86 32-bit | Well-documented, QEMU support, 2-level paging |
| Kernel model | Microkernel | Driver isolation, hot-pluggable services |
| Higher half | Kernel at 0xC0000000 | 3GB user / 1GB kernel, standard practice |
| Bootloader | GRUB Multiboot 1 | Mature, handles A20/protected mode/mmap |
| Executable format | ELF | Standard, integrated loader |
| Filesystem | FAT32 (DobFS planned) | Simple, compatible, real R/W implementation |
| IPC | Synchronous send/receive/reply | QNX/L4 inspired, simple, efficient |

## Disk Layout at Runtime

```
/SYSTEM/
├── PROGRAMS/          Installed applications (one folder each)
├── DRIVERS/           Driver packages
├── OperatingSystem/  Kernel, core servers, system files
├── PROGRAM FILES/     Shared libraries, stubs
└── GAMES/

/DATA/
├── Desktop/     Documents/    Downloads/    Music/
├── Video/       Pictures/     Screenshots/  Module files/
└── Other files/
```

## Entry Point System

### Exposing functions (server side):
1. Write the interface header (`<n>.h`) declaring the struct of function pointers
2. Write the stub (`<n>_stub.c`) implementing each pointer as an IPC call
3. Implement the handler function in the server

### Calling functions (client side):
1. `#include <stubs/DobFileSystem.h>`
2. Call `dobfs_Open(path, FS_READ);`
3. Done. The stub handles IPC transparently.

### Under the hood:
```
Client program  ->  Stub (IPC call)  ->  Server (handler)
    |                    |                    |
    |  dobfs_Open()      |  dob_ipc_call()   |  dob_server_loop()
    |                    |  port lookup       |  dispatch by code
    |                    |  serialize args    |  execute operation
    |                    |  block for reply   |  dob_ipc_reply()
    |  <-- return val    |  <-- unblock       |
```

## Memory Map (Virtual Address Space)

| Range | Usage |
|---|---|
| 0x00000000 - 0x003FFFFF | Null guard area (unmapped) |
| 0x00400000 - 0x5FFFFFFF | User program code/data/heap |
| 0x60000000 - 0x6FFFFFFF | `.mem` shared objects (256 MB) |
| 0x70000000 - 0x7FEFFFFF | DMA buffers (sys_dma_alloc) |
| 0x7FF00000 - 0x7FFFFFFF | Per-process IPC buffer (64KB) |
| 0x80000000 - 0xBFFDFFFF | MMIO mappings (sys_mmap_phys) |
| 0xBFFE0000 - 0xBFFFFFFF | User stack (64KB, grows down) |
| 0xC0000000 - 0xC0FFFFFF | Identity-mapped physical RAM (16MB+, extensible) |
| 0xC1000000+ | Kernel heap (vmm_alloc_kernel_pages) |
| 0xFF800000 - 0xFF801FFF | Paging temp mapping area |
| 0xFFC00000 - 0xFFFFFFFF | Recursive page table mapping |

`.mem` shared objects live in a dedicated high range so they cannot
fragment the heap. Before v1.0.0.420.40 they were placed at the first
free gap above the program's code, which made `sbrk()` collide with
the `.mem`'s base on any non-trivial heap growth. The per-process heap
cap (`sys_brk`) is `0x40000000`, so it cannot reach the `.mem` range.

## Synchronization

All shared kernel data structures are protected by spinlocks with IRQ save/restore:

| Lock | Protects |
|---|---|
| `pmm_lock` | Physical frame bitmap |
| `vmm_lock` | Kernel virtual address allocator |
| `heap_lock` | Kernel heap free list |
| `sched_lock` | Run queues + sleep queue |
| `process_lock` | Process table |
| `thread_lock` | Thread table |
| `port_global_lock` | IPC port table |
| `vga_lock` | VGA text buffer + cursor |
| `paging_temp_lock` | Temporary page directory mappings |
| `mmap_lock` | MMIO/DMA virtual address allocators |
| `timer_lock` | Active timer list |
| `handle_lock` | Handle transfer atomicity |

Higher-level primitives (mutex, semaphore, rwlock) use a guard spinlock pattern:
pre-allocate wait entry outside lock -> check condition under lock -> enqueue + block ->
release lock -> yield -> re-check on wakeup.

## Known Constraints (v1.0)

### Single-core only (no SMP)
The kernel assumes a single CPU core. Spinlocks work because critical sections
disable interrupts, preventing concurrent access. On a multi-core system, all
spinlocks would need to be true test-and-set locks (which they are structurally,
but the IRQ disable wouldn't protect against access from another core). Specific
areas that would break on SMP:
- `scheduler_tick` accesses run queues under `sched_lock` but could race with
  `scheduler_yield` on another core
- `context_switch` is not SMP-safe (per-CPU current_thread needed)
- VGA output would interleave (vga_lock helps but cursor management isn't per-CPU)
- TLB shootdown would be needed after page table modifications

### Virtual address space recycling
`sys_mmap_phys` and `sys_dma_alloc` use `vregion_alloc` for gap-filling virtual
address allocation. `sys_munmap` calls `vregion_free` to return the region,
enabling reuse. `kpages_alloc` uses a multi-page free list with bump fallback,
recycling kernel stack virtual addresses across thread creation/destruction.

### PMM bitmap in BSS
The physical memory bitmap (128KB for 4GB support) is statically allocated in
the kernel BSS section. This is simple but contributes to kernel image size.

## Build Commands

```bash
./setup.sh          # Install dependencies + cross-compiler (~20 min first time)
make                # Full build: kernel + userspace + bootable disk image
make run            # Build + launch in QEMU
make run-disk       # Launch from existing disk only (skip rebuild)
make debug          # Build + QEMU with GDB on :1234
make clean          # Remove build artifacts (keeps disk.img)
make distclean      # Remove everything including disk.img
```

## Toolchain Required

```
i686-elf-gcc        # Cross-compiler for x86 (built by setup.sh)
i686-elf-ld         # Cross-linker
nasm                # Assembler for .asm files
grub-mkrescue       # Creates bootable ISO
xorriso             # Required by grub-mkrescue
qemu-system-i386    # Emulator
```
