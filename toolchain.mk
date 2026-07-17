# MainDOB - Toolchain Configuration
# Cross-compiler for i686 freestanding target

CC      := i686-elf-gcc
AS      := nasm
LD      := i686-elf-ld
AR      := i686-elf-ar
OBJCOPY := i686-elf-objcopy

# Common flags. -fno-omit-frame-pointer is left ON regardless of
# build mode because the kernel panic handler walks frames to print
# a stack trace; without %ebp set up that becomes guesswork. The
# cost is one push/pop per call (~2 cycles) — measurable but worth
# it for diagnosibility.
CFLAGS_COMMON := -std=gnu99 -ffreestanding -fno-builtin -nostdlib \
                 -Wall -Wextra -Werror=implicit-function-declaration \
                 -fno-omit-frame-pointer -fno-exceptions

# Kernel flags (ring 0, no SSE/MMX to avoid issues with context switch).
#
# -ffunction-sections + -fdata-sections: place every function and
# global in its own ELF section. Combined with --gc-sections below,
# the linker drops sections that nothing references — kernel objects
# from a previous build that lost their last caller stop polluting
# the I-cache and bloating the binary. Userspace already had this;
# kernel was missing it. Saves a few KB and tightens hot-path
# locality.
KERNEL_CFLAGS := $(CFLAGS_COMMON) -mno-sse -mno-mmx -mno-80387 \
                 -fno-stack-protector -fno-pie -fno-pic \
                 -ffunction-sections -fdata-sections

# Userspace flags (ring 3). Same -ffunction/data-sections as kernel
# now (it used to be a userspace-only optimization).
USER_CFLAGS := $(CFLAGS_COMMON) -fno-stack-protector -fno-pic \
               -ffunction-sections -fdata-sections

# Assembler
ASFLAGS := -f elf32

# Linker. --gc-sections is the companion to -ffunction-sections above:
# the linker traces from the entry point (crt0/_start) and discards
# any .text.<fn> / .data.<sym> section that isn't reachable. Now
# applied to both kernel and userspace.
LDFLAGS_KERNEL := -nostdlib -z noexecstack --gc-sections
LDFLAGS_USER   := -nostdlib --gc-sections

# Tools — auto-detect grub-mkrescue vs grub2-mkrescue (Fedora/RHEL)
GRUB_MKRESCUE := $(shell command -v grub-mkrescue 2>/dev/null || command -v grub2-mkrescue 2>/dev/null || echo grub-mkrescue)
QEMU          := $(shell command -v qemu-system-i386 2>/dev/null || command -v qemu-system-x86_64 2>/dev/null || echo qemu-system-i386)
GDB           := gdb
