# MainDOB - Build Configuration

# Build mode: debug or release
#
# release (default): -O2, NDEBUG, no debug info. This is the
#   performance-priority default — every micro-optimization in the
#   kernel and userspace assumes the compiler will inline static
#   functions, fold constants, and eliminate dead branches. With
#   -O0 the seqlock readers, hot-path inlines, fast-path branches,
#   and per-thread sleep_timer accesses all materialize as full
#   function calls and stack spills, making the kernel measurably
#   slower than a tick-based design.
#
# debug: -Og keeps optimizations that don't hurt debuggability
#   (inlining, dead-code elimination), preserves frame pointers and
#   line tables. Pure -O0 is no longer offered as a build target —
#   the kernel will boot but bench results are misleading.
BUILD_MODE ?= release

ifeq ($(BUILD_MODE),debug)
    OPT_FLAGS  := -Og -g -DMAINDOB_DEBUG
else
    OPT_FLAGS  := -O2 -DNDEBUG
endif

# SMP: 1 = multi-CPU kernel (per-CPU state via g_cpus[], AP scheduling);
#   0 = uniprocessor build (e.g. the single-core PIII), zero per-CPU
#   indirection — this_cpu() is a constant address. Default on; build the
#   uniprocessor kernel with `make SMP=0`.
SMP ?= 1
ifeq ($(SMP),1)
    KERNEL_SMP_FLAGS := -DMAINDOB_SMP
else
    KERNEL_SMP_FLAGS :=
endif

# PCI device-IRQ delivery controller:
#   1 (default): migrate device-IRQ delivery to the IOAPIC at boot
#     (intr_switch_to_ioapic). The kernel resolves PCI INTx -> GSI
#     empirically — each driver registers its device by bus:slot:func and
#     claims its GSI on the device's first interrupt (SYS_IRQ_REGISTER_PCI
#     plus the discovery machinery in syscall.c), so PCI devices landing on
#     GSI>=16 are routed and serviced without an ACPI _PRT / AML interpreter.
#     SAFE EVERYWHERE: a machine with no IOAPIC stays on the PIC
#     automatically, because intr_switch_to_ioapic finds no IOAPIC and does
#     not migrate (e.g. Compaq Armada E500, APIC hardware-disabled). The
#     resolver's logic is gated on the IOAPIC actually being active, so a
#     PIC fallback is byte-for-byte the legacy behaviour.
#   0: legacy 8259 PIC. The PCI "Interrupt Line" config byte (offset 0x3C)
#     is valid in PIC mode, so device interrupts are delivered on the line
#     the driver reads. A conservative fallback that forces the working PIC
#     path explicitly (e.g. Acer Extensa 5220 / ICH8M), useful for A/B
#     testing or if a new chipset misbehaves under IOAPIC delivery.
#   The LAPIC timer is unaffected either way (delivered via its own LAPIC
#   vector, never through PIC/IOAPIC).
# Default on (native delivery); fall back with `make IOAPIC_DELIVERY=0`.
IOAPIC_DELIVERY ?= 1
ifeq ($(IOAPIC_DELIVERY),1)
    KERNEL_IOAPIC_FLAGS := -DMAINDOB_IOAPIC_DELIVERY
else
    KERNEL_IOAPIC_FLAGS :=
endif

# SMP cross-core smoke test: diagnostic only. When 1, kmain spawns a few
# kernel workers across the APs after scheduler_start to prove cross-core
# dispatch + teardown. OFF by default: on a live boot it runs concurrently
# with userspace module spawn, so its short-lived worker TIDs (and their
# deferred teardown) race the drivers being spawned — producing boot-to-boot
# variance in spawn order, duplicate driver instances, and the "destroy
# TID 0 denied" warning. Enable with `make SMP_SMOKE=1` for a controlled
# bring-up check, never in production.
SMP_SMOKE ?= 0
ifeq ($(SMP_SMOKE),1)
    KERNEL_SMOKE_FLAGS := -DMAINDOB_SMP_SMOKE
else
    KERNEL_SMOKE_FLAGS :=
endif

# RAM ceiling policy: what to do when usable RAM exceeds the kernel
# direct-map (256 MB). The PMM never hands out a frame above the
# direct-map either way (the invariant holds regardless); this only
# chooses the boot-time reaction to the excess.
#   0 (default): cap-and-warn — boot on the addressable RAM, log the
#     ignored amount loudly, keep running. Right for appliances whose
#     BOM may vary but must stay up.
#   1: strict — refuse to boot (kpanic) outside the validated memory
#     envelope. Right for a certified fixed-BOM build that must not run
#     a configuration it wasn't qualified for. Enable with
#     `make RAM_CEILING_STRICT=1`.
RAM_CEILING_STRICT ?= 0
ifeq ($(RAM_CEILING_STRICT),1)
    KERNEL_RAM_FLAGS := -DMAINDOB_STRICT_RAM_CEILING
else
    KERNEL_RAM_FLAGS :=
endif

# Paths
ROOT_DIR    := $(shell pwd)
KERNEL_DIR  := $(ROOT_DIR)/kernel
OS_DIR := $(ROOT_DIR)/boot
DRIVERS_DIR := $(ROOT_DIR)/drivers
LIBDOB_DIR  := $(ROOT_DIR)/libdob
LIBC_DIR    := $(ROOT_DIR)/libc
TOOLS_DIR   := $(ROOT_DIR)/tools
ISO_DIR     := $(ROOT_DIR)/iso
BUILD_DIR   := $(ROOT_DIR)/build

# Output
KERNEL_BIN  := $(BUILD_DIR)/kernel.bin
ISO_FILE    := $(BUILD_DIR)/maindob.iso
