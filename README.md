# MainDOB

Microkernel operating system with Entry Point System for seamless inter-application communication.

## Key Features

- **Microkernel architecture**: drivers and services run in isolated userspace processes, detachable on fault
- **Entry Point System**: programs expose callable functions to each other through stub libraries (`#include` + call, IPC is transparent)
- **Centralized config**: all settings in a single protected database, accessed only through the config server
- **Native sandboxing**: each program confined to its own folder; cross-folder access enforced by DobFileSystem
- **SYSTEM/DATA layout**: clean disk structure separating system files from user data
- **Executables**: `.mdl` format (ELF internally)

## Build Requirements

### Cross-compiler (i686-elf-gcc)

```bash
# Arch Linux
pacman -S i686-elf-gcc i686-elf-binutils

# Others: build from source
# See https://wiki.osdev.org/GCC_Cross-Compiler
```

### Other tools

```bash
# Ubuntu/Debian
sudo apt install nasm grub-pc-bin grub-common xorriso qemu-system-x86 gdb

# Arch Linux
sudo pacman -S nasm grub xorriso qemu-system-x86 gdb
```

## Build & Run

```bash
make            # Build kernel + ISO
make run        # Launch in QEMU
make debug      # Launch in QEMU with GDB (port 1234)
make clean      # Clean build artifacts
```

## Project Structure

```
maindob/
├── kernel/          Microkernel (ring 0)
│   ├── boot/          Multiboot entry, higher-half setup, bootfs, startup
│   ├── arch/x86/      GDT, IDT, ISR, IRQ, paging, TSS
│   ├── mm/            PMM (bitmap), VMM, kheap, vregions, entropy pool
│   ├── proc/          Process, thread, scheduler, context switch, ELF, workqueue
│   ├── ipc/           Ports, channels, synchronous send/receive/reply, async notify
│   ├── irq/           PCI/PIRQ discovery, IRQ line allocation
│   ├── syscall/       System call dispatcher (int 0x80) + ~60 handlers
│   ├── sync/          Spinlock, mutex, semaphore, rwlock, event groups, atomics
│   ├── time/          PIT clock, RTC, software timer manager
│   ├── lib/           types, string, printf, bitmap, intrusive list
│   ├── registry.c     Service registry (name → IPC port)
│   └── kernel.c       kmain(), startup module loader
├── boot/            Userspace boot services (loaded from Startup_modules)
│   ├── console/       VGA text output service
│   ├── config/        Centralized settings database
│   ├── DobFileSystem/ FAT32 filesystem with sandbox enforcement
│   ├── dobinterface/  Window manager + GUI server
│   ├── popups/        System popup dialogs
│   ├── inputd/        Keyboard/mouse input router
│   ├── hotplug/       PCI enumeration, driver matching, device bubbles, DAS
│   ├── floppyprobe/   Floppy disk insertion poller
│   └── audioplayer/   Audio decode + playback service (WAV + MP2)
├── drivers/         Userspace drivers (isolated, detachable)
│   ├── ata/           ATA PIO disk
│   ├── ahci/          SATA AHCI
│   ├── floppy/        FDC i8272A floppy
│   ├── rtc/           CMOS Real Time Clock (opt-in, not in default startup)
│   ├── ac97/          AC'97 audio codec
│   └── usb_uhci, usb_ehci, usb_xhci/  USB controllers
├── libdob/          Runtime library for Entry Point System
│   ├── include/dob/   types, ipc wrapper, registry client, server framework
│   ├── include/       DobRT.h, rt_helpers.h (real-time control API)
│   └── src/           Implementation
├── libdobui/        DobUITools — userspace UI control library (button, textbox, ...)
├── libc/            Minimal C library for userspace
│   ├── include/       stdio, stdlib, string, unistd, sys/types, sys/syscall
│   └── src/           crt0.asm, string.c, stdio.c, stdlib.c
└── tools/           mkbootdisk.sh (bootable FAT32 disk)
```

## Kernel Subsystem Status

| Subsystem | Status | Description |
|-----------|--------|-------------|
| Boot | Complete | Multiboot, higher-half at 0xC0000000, PSE→4KB paging transition |
| GDT/IDT/ISR/IRQ | Complete | Flat segments, kernel+user, TSS, PIC remap, spurious IRQ handling |
| PMM | Complete | Bitmap allocator, protects kernel/BIOS/modules |
| VMM/Paging | Complete | Recursive mapping, kernel page alloc, process address spaces |
| Heap | Complete | First-fit free list, splitting, bidirectional coalescing, auto-expand |
| Regions | Complete | Per-process VM region tracking (code/data/stack/heap/mmap) via vregions |
| Clock/Timer | Complete | PIT at 1000Hz, software timer manager, sleep |
| Process | Complete | PCB, sandbox profile, parent/child hierarchy |
| Thread | Complete | Kernel/user threads, per-thread stacks, context save/restore |
| Scheduler | Complete | Priority round-robin, preemption, sleep queue |
| Context Switch | Complete | Assembly save/restore, CR3 switch for address spaces |
| Sync | Complete | Spinlock (irqsave), mutex, semaphore, rwlock, event groups, atomics |
| IPC | Complete | Sync send/receive/reply, async notify bitmask, port access control |
| Syscall | Complete | int 0x80 dispatcher, ~60 syscalls (proc, IPC, mem, time, IRQ, etc.) |
| ELF Loader | Complete | Loads PT_LOAD segments, validates 32-bit i386 ELF |
| Registry | Complete | In-kernel name → port table for service discovery |
| Sandbox | Userspace | Path-based access control enforced by DobFileSystem (kernel relays IPC only) |

## Self-Tests

The kernel runs self-tests at boot:
- **Scheduler test**: two threads interleaving with different sleep intervals
- **IPC test**: server thread receives 3 messages, replies with transformed code
- **Heap test**: alloc/free/reuse cycle with coalescing verification
- **Module loading**: bootfs reads `/SYSTEM/CONFIG/Startup_modules` and spawns each entry as its own process

## Disk Layout

```
/
├── SYSTEM/
│   ├── PROGRAMS/          Installed applications (sandboxed per-folder)
│   ├── DRIVERS/           Loadable driver .mdl files
│   ├── OperatingSystem/   Kernel, boot services, config DB
│   ├── PROGRAM FILES/     Shared resources (fonts, icons)
│   └── GAMES/             Games (same sandbox rules as PROGRAMS)
└── DATA/
    ├── Desktop/    Documents/    Downloads/    Music/
    ├── Video/      Pictures/     Screenshots/
    └── Module files/             Other files/
```

## Entry Point System

Programs expose functions through hand-written stub libraries (`<Name>.h` + `<Name>_stub.c`)
that wrap IPC calls into a flat C API. The header exposes a struct of function pointers,
the stub provides the implementation that marshals arguments and routes them to the
target server's port.

**Calling another program's function:**
```c
#include <stubs/emailclient.h>
emailclient.messages.SendMessage(text, recipient);
// That's it. IPC is handled transparently by the stub.
```

**Exposing functions:**
```c
#include <dob/server.h>
dob_status_t handle_SendMessage(dob_msg_t *msg, dob_msg_t *reply) {
    // Do the work here
    return DOB_OK;
}
int main() {
    dob_server_init("emailclient");
    dob_server_register("messages.SendMessage", handle_SendMessage);
    dob_server_loop();
}
```

## Architecture Decisions

| Choice | Decision | Why |
|--------|----------|-----|
| Arch | x86 32-bit | Well documented, QEMU support, 2-level paging |
| Bootloader | GRUB + Multiboot 1 | Protected mode, mmap, initrd, framebuffer |
| Kernel model | Microkernel | Driver isolation, fault recovery, matches MainDOB design |
| IPC | Sync + async notify | QNX/L4 inspired: send/receive/reply + notify bitmask |
| Memory | Bitmap allocator | Fast, simple, as per original spec |
| Executable | ELF (.mdl extension) | Standard format, loader in kernel for bootstrap |
| Filesystem | FAT32 initial | Simple, compatible; native DobFS planned |
| Test target | QEMU | GDB debug, serial logging, known devices |

## Documentation

See `docs/architecture.md` for the complete design document.
