# MainDOB Drivers Makefile

-include $(ROOT_DIR)/toolchain.mk
include $(ROOT_DIR)/config.mk

LIBC_INC   := -I$(ROOT_DIR)/libc/include
LIBDOB_INC := -I$(ROOT_DIR)/libdob/include
INCLUDES   := $(LIBC_INC) $(LIBDOB_INC)

CFLAGS := $(USER_CFLAGS) $(OPT_FLAGS) $(INCLUDES)

DRIVERS := ata ahci rtc usb_uhci usb_ehci usb_xhci usb_mass_storage ac97 maestro2e floppy cdrom bga mach64 x3100

DRIVER_BINS := $(patsubst %,$(BUILD_DIR)/drivers/%.mdl,$(DRIVERS))

LIBC_OBJS   := $(BUILD_DIR)/libc/crt0.o $(BUILD_DIR)/libc/string.o \
               $(BUILD_DIR)/libc/stdio.o $(BUILD_DIR)/libc/stdlib.o
LIBDOB_OBJS := $(BUILD_DIR)/libdob/ipc.o $(BUILD_DIR)/libdob/registry.o $(BUILD_DIR)/libdob/server.o \
               $(BUILD_DIR)/libdob/window_servo.o $(BUILD_DIR)/libdob/font.o $(BUILD_DIR)/libdob/thread.o

# The cdrom driver additionally needs libdob/mem.o to call dob_mem_load.
LIBDOB_OBJS_WITH_MEM := $(LIBDOB_OBJS) $(BUILD_DIR)/libdob/mem.o

# The ata and ahci drivers additionally need libdob/partition.o to call
# partition_scan_announce — boot-time first scan and on-demand RESCAN.
LIBDOB_OBJS_WITH_PARTITION := $(LIBDOB_OBJS) $(BUILD_DIR)/libdob/partition.o

.PHONY: all clean

all: $(DRIVER_BINS) $(BUILD_DIR)/drivers/cdrom/iso9660.mem

# ata.mdl — explicit rule because we need partition.o on the link line.
$(BUILD_DIR)/drivers/ata.mdl: $(ROOT_DIR)/drivers/ata/main.c $(LIBC_OBJS) $(LIBDOB_OBJS_WITH_PARTITION)
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/ata/main.c"
	@$(CC) $(CFLAGS) -c $< -o $(BUILD_DIR)/drivers/ata_main.o
	@echo "[LD]  ata.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(BUILD_DIR)/drivers/ata_main.o \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOB_OBJS_WITH_PARTITION) \
		-o $@

# ahci.mdl — explicit rule, same reason as ata.mdl.
$(BUILD_DIR)/drivers/ahci.mdl: $(ROOT_DIR)/drivers/ahci/main.c $(LIBC_OBJS) $(LIBDOB_OBJS_WITH_PARTITION)
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/ahci/main.c"
	@$(CC) $(CFLAGS) -c $< -o $(BUILD_DIR)/drivers/ahci_main.o
	@echo "[LD]  ahci.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(BUILD_DIR)/drivers/ahci_main.o \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOB_OBJS_WITH_PARTITION) \
		-o $@

# AC97 audio driver: needs DobInterface (widget) + slider + label + separator (UI)
AC97_CFLAGS := $(CFLAGS) -I$(ROOT_DIR)/drivers/ac97 -I$(BUILD_DIR)/eps -I$(ROOT_DIR)/libdobui

$(BUILD_DIR)/drivers/ac97.mdl: $(ROOT_DIR)/drivers/ac97/main.c $(LIBC_OBJS) $(LIBDOB_OBJS) \
		$(BUILD_DIR)/eps/DobInterface_stub.o $(BUILD_DIR)/libdobui/slider.o \
		$(BUILD_DIR)/libdobui/label.o $(BUILD_DIR)/libdobui/separator.o
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/ac97/main.c"
	@$(CC) $(AC97_CFLAGS) -c $< -o $(BUILD_DIR)/drivers/ac97_main.o
	@echo "[LD]  ac97.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(BUILD_DIR)/drivers/ac97_main.o \
		$(BUILD_DIR)/eps/DobInterface_stub.o \
		$(BUILD_DIR)/libdobui/slider.o \
		$(BUILD_DIR)/libdobui/label.o \
		$(BUILD_DIR)/libdobui/separator.o \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOB_OBJS) \
		-o $@

# ESS Maestro-2E audio driver: same UI dependency set as ac97 (the
# Volume Mixer widget reuses DobInterface + slider + label + separator).
# It also pulls DobAudio.h from drivers/ac97 — the "audio" service
# protocol header is shared, the two drivers are interchangeable
# providers of the same service, picked per-machine by the DAS.
MAESTRO2E_CFLAGS := $(CFLAGS) -I$(ROOT_DIR)/drivers/ac97 -I$(BUILD_DIR)/eps \
                    -I$(ROOT_DIR)/libdobui

$(BUILD_DIR)/drivers/maestro2e.mdl: $(ROOT_DIR)/drivers/maestro2e/main.c \
		$(LIBC_OBJS) $(LIBDOB_OBJS) \
		$(BUILD_DIR)/eps/DobInterface_stub.o $(BUILD_DIR)/libdobui/slider.o \
		$(BUILD_DIR)/libdobui/label.o $(BUILD_DIR)/libdobui/separator.o
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/maestro2e/main.c"
	@$(CC) $(MAESTRO2E_CFLAGS) -c $< -o $(BUILD_DIR)/drivers/maestro2e_main.o
	@echo "[LD]  maestro2e.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(BUILD_DIR)/drivers/maestro2e_main.o \
		$(BUILD_DIR)/eps/DobInterface_stub.o \
		$(BUILD_DIR)/libdobui/slider.o \
		$(BUILD_DIR)/libdobui/label.o \
		$(BUILD_DIR)/libdobui/separator.o \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOB_OBJS) \
		-o $@

# Floppy driver: spawned on-demand by the DAS, calls dobfiles_OpenMount
# via the DobFiles EPS stub after a successful media probe. NOT in any
# startup list — the FDC stays cold until the user double-clicks the
# desktop icon.
#
# v1.0.0.408: DobFiles_stub.o now pulls in DobConfig_stub (mount handoff
# via config keys) and dob/spawn.h (spawning a fresh DobFiles instance
# instead of hijacking an existing one). spawn_file() is an inline that
# uses dobfs_Stat/Open/Read/Close, so DobFileSystem_stub must also be
# on the link line even though the floppy driver itself never calls
# the plain dobfs_* API directly.
FLOPPY_CFLAGS := $(CFLAGS) -I$(BUILD_DIR)/eps

$(BUILD_DIR)/drivers/floppy.mdl: $(ROOT_DIR)/drivers/floppy/main.c $(LIBC_OBJS) $(LIBDOB_OBJS) \
		$(BUILD_DIR)/eps/DobFiles_stub.o $(BUILD_DIR)/eps/DobConfig_stub.o \
		$(BUILD_DIR)/eps/DobFileSystem_stub.o
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/floppy/main.c"
	@$(CC) $(FLOPPY_CFLAGS) -c $< -o $(BUILD_DIR)/drivers/floppy_main.o
	@echo "[LD]  floppy.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(BUILD_DIR)/drivers/floppy_main.o \
		$(BUILD_DIR)/eps/DobFiles_stub.o \
		$(BUILD_DIR)/eps/DobConfig_stub.o \
		$(BUILD_DIR)/eps/DobFileSystem_stub.o \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOB_OBJS) \
		-o $@

# CDROM driver: like floppy, calls dobfiles_OpenMount after a good
# media probe. Also loads iso9660.mem at startup via dob_mem_load,
# hence libdob/mem.o on the link line and iso9660.mem as an order-only
# prerequisite so both land on the disk image.
CDROM_CFLAGS := $(CFLAGS) -I$(BUILD_DIR)/eps

$(BUILD_DIR)/drivers/cdrom.mdl: $(ROOT_DIR)/drivers/cdrom/main.c $(LIBC_OBJS) $(LIBDOB_OBJS_WITH_MEM) \
		$(BUILD_DIR)/eps/DobFiles_stub.o $(BUILD_DIR)/eps/DobConfig_stub.o \
		$(BUILD_DIR)/eps/DobFileSystem_stub.o \
		| $(BUILD_DIR)/drivers/cdrom/iso9660.mem
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/cdrom/main.c"
	@$(CC) $(CDROM_CFLAGS) -c $< -o $(BUILD_DIR)/drivers/cdrom_main.o
	@echo "[LD]  cdrom.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(BUILD_DIR)/drivers/cdrom_main.o \
		$(BUILD_DIR)/eps/DobFiles_stub.o \
		$(BUILD_DIR)/eps/DobConfig_stub.o \
		$(BUILD_DIR)/eps/DobFileSystem_stub.o \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOB_OBJS_WITH_MEM) \
		-o $@

# iso9660.mem — PIC shared object loaded at runtime by cdrom.mdl.
# Self-contained: no crt0, no libc, no libdob. The parser has its own
# string helpers inline; only R_386_RELATIVE relocations are produced.
#
# We compile + link in two steps using the underlying binutils ld
# directly rather than the gcc driver. The gcc driver on some i686-elf
# toolchains decides "-shared" means "static exec with shared-ish
# semantics" because there is no libdl / ld.so in the sysroot; the
# resulting ELF comes out as ET_EXEC (type=2) and the kernel rightly
# rejects it. Calling ld with -shared ourselves produces ET_DYN=3
# as we require.
MEM_CFLAGS := $(filter-out -fno-pic -fno-pie, $(USER_CFLAGS)) \
              $(OPT_FLAGS) -fPIC \
              -mno-sse -mno-mmx -mno-80387 -fno-stack-protector \
              -fno-builtin -fvisibility=default \
              -I$(ROOT_DIR)/libc/include -I$(ROOT_DIR)/libdob/include

$(BUILD_DIR)/drivers/cdrom/iso9660.mem: $(ROOT_DIR)/drivers/cdrom/iso9660.c \
                                        $(ROOT_DIR)/drivers/cdrom/iso9660_api.h
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/cdrom/iso9660.c (.mem)"
	@$(CC) $(MEM_CFLAGS) -c $< -o $(BUILD_DIR)/drivers/cdrom/iso9660.o
	@echo "[LD]  iso9660.mem"
	@$(LD) -shared -nostdlib --no-undefined -z noexecstack \
		-Bsymbolic \
		$(BUILD_DIR)/drivers/cdrom/iso9660.o -o $@
	@# Verify the kernel will accept this .mem: ET_DYN only.
	@etype=$$(od -An -tx1 -j16 -N2 $@ | tr -d ' '); \
	 if [ "$$etype" != "0300" ]; then \
	    echo "[LD]  ERROR: iso9660.mem has ELF type $$etype, expected 0300 (ET_DYN)"; \
	    rm -f $@; \
	    exit 1; \
	 fi
	@echo "[LD]  iso9660.mem OK (ET_DYN)"

# BGA video driver — multi-source.  Driver implementation in main.c,
# transport split in two:
#   bga_transport_fast.c + bga_fast_entry.asm: int 0x85 boomerang for
#     the data plane (DV_* opcodes — every hot client call).
#   bga_transport_ipc.c: plain IPC for the control plane (DOBVC_*
#     opcodes — mode change, gamma, arrangement, gpu reset).
BGA_CFLAGS := $(CFLAGS) -I$(ROOT_DIR)/drivers/bga

$(BUILD_DIR)/drivers/bga.mdl: $(ROOT_DIR)/drivers/bga/main.c \
                              $(ROOT_DIR)/drivers/bga/bga_transport_ipc.c \
                              $(ROOT_DIR)/drivers/bga/bga_transport_fast.c \
                              $(ROOT_DIR)/drivers/bga/bga_fast_entry.asm \
                              $(ROOT_DIR)/drivers/bga/bga_state.h \
                              $(ROOT_DIR)/drivers/bga/dobvc_protocol.h \
                              $(LIBC_OBJS) $(LIBDOB_OBJS)
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/bga/main.c"
	@$(CC) $(BGA_CFLAGS) -c $(ROOT_DIR)/drivers/bga/main.c \
		-o $(BUILD_DIR)/drivers/bga_main.o
	@echo "[CC]  drivers/bga/bga_transport_ipc.c"
	@$(CC) $(BGA_CFLAGS) -c $(ROOT_DIR)/drivers/bga/bga_transport_ipc.c \
		-o $(BUILD_DIR)/drivers/bga_transport_ipc.o
	@echo "[CC]  drivers/bga/bga_transport_fast.c"
	@$(CC) $(BGA_CFLAGS) -c $(ROOT_DIR)/drivers/bga/bga_transport_fast.c \
		-o $(BUILD_DIR)/drivers/bga_transport_fast.o
	@echo "[ASM] drivers/bga/bga_fast_entry.asm"
	@$(AS) $(ASFLAGS) $(ROOT_DIR)/drivers/bga/bga_fast_entry.asm \
		-o $(BUILD_DIR)/drivers/bga_fast_entry.o
	@echo "[LD]  bga.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(BUILD_DIR)/drivers/bga_main.o \
		$(BUILD_DIR)/drivers/bga_transport_ipc.o \
		$(BUILD_DIR)/drivers/bga_transport_fast.o \
		$(BUILD_DIR)/drivers/bga_fast_entry.o \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOB_OBJS) \
		-o $@

# Mach64 video driver (ATI Rage Mobility-P 4C4D / Rage LT Pro 4C42) —
# the real-hardware counterpart to BGA.  Same multi-source layout: one
# main.c with the dv_* dispatcher + driver state, two transport files
# (boomerang fast-path for the data plane, plain IPC for the control
# plane), one asm shim, hardware-specific modules for modeset / 2D /
# IRQ / cursor.  See drivers/mach64/README.md for design rationale.
MACH64_CFLAGS := $(CFLAGS) -I$(ROOT_DIR)/drivers/mach64 -I$(BUILD_DIR)/eps

MACH64_OBJS := \
	$(BUILD_DIR)/drivers/mach64_main.o \
	$(BUILD_DIR)/drivers/mach64_modeset.o \
	$(BUILD_DIR)/drivers/mach64_2d.o \
	$(BUILD_DIR)/drivers/mach64_3d.o \
	$(BUILD_DIR)/drivers/mach64_overlay.o \
	$(BUILD_DIR)/drivers/mach64_irq.o \
	$(BUILD_DIR)/drivers/mach64_cursor.o \
	$(BUILD_DIR)/drivers/mach64_transport_ipc.o \
	$(BUILD_DIR)/drivers/mach64_transport_fast.o \
	$(BUILD_DIR)/drivers/mach64_fast_entry.o

MACH64_SRCS := \
	$(ROOT_DIR)/drivers/mach64/main.c \
	$(ROOT_DIR)/drivers/mach64/mach64_modeset.c \
	$(ROOT_DIR)/drivers/mach64/mach64_2d.c \
	$(ROOT_DIR)/drivers/mach64/mach64_3d.c \
	$(ROOT_DIR)/drivers/mach64/mach64_overlay.c \
	$(ROOT_DIR)/drivers/mach64/mach64_irq.c \
	$(ROOT_DIR)/drivers/mach64/mach64_cursor.c \
	$(ROOT_DIR)/drivers/mach64/mach64_transport_ipc.c \
	$(ROOT_DIR)/drivers/mach64/mach64_transport_fast.c \
	$(ROOT_DIR)/drivers/mach64/mach64_fast_entry.asm \
	$(ROOT_DIR)/drivers/mach64/mach64_state.h \
	$(ROOT_DIR)/drivers/mach64/mach64_hw.h \
	$(ROOT_DIR)/drivers/mach64/dobvc_protocol.h

$(BUILD_DIR)/drivers/mach64_main.o: $(ROOT_DIR)/drivers/mach64/main.c
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/mach64/main.c"
	@$(CC) $(MACH64_CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/mach64_modeset.o: $(ROOT_DIR)/drivers/mach64/mach64_modeset.c
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/mach64/mach64_modeset.c"
	@$(CC) $(MACH64_CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/mach64_2d.o: $(ROOT_DIR)/drivers/mach64/mach64_2d.c
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/mach64/mach64_2d.c"
	@$(CC) $(MACH64_CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/mach64_3d.o: $(ROOT_DIR)/drivers/mach64/mach64_3d.c
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/mach64/mach64_3d.c"
	@$(CC) $(MACH64_CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/mach64_overlay.o: $(ROOT_DIR)/drivers/mach64/mach64_overlay.c
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/mach64/mach64_overlay.c"
	@$(CC) $(MACH64_CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/mach64_irq.o: $(ROOT_DIR)/drivers/mach64/mach64_irq.c
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/mach64/mach64_irq.c"
	@$(CC) $(MACH64_CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/mach64_cursor.o: $(ROOT_DIR)/drivers/mach64/mach64_cursor.c
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/mach64/mach64_cursor.c"
	@$(CC) $(MACH64_CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/mach64_transport_ipc.o: $(ROOT_DIR)/drivers/mach64/mach64_transport_ipc.c
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/mach64/mach64_transport_ipc.c"
	@$(CC) $(MACH64_CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/mach64_transport_fast.o: $(ROOT_DIR)/drivers/mach64/mach64_transport_fast.c
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/mach64/mach64_transport_fast.c"
	@$(CC) $(MACH64_CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/mach64_fast_entry.o: $(ROOT_DIR)/drivers/mach64/mach64_fast_entry.asm
	@mkdir -p $(dir $@)
	@echo "[ASM] drivers/mach64/mach64_fast_entry.asm"
	@$(AS) $(ASFLAGS) $< -o $@

# Il doppio frame e' una SCELTA in DobSettings (video.double_buffer):
# il driver la legge all'avvio via DobSettings_stub, come hotplug.
$(BUILD_DIR)/drivers/mach64.mdl: $(MACH64_OBJS) $(LIBC_OBJS) $(LIBDOB_OBJS) $(MACH64_SRCS) \
		$(BUILD_DIR)/eps/DobSettings_stub.o
	@mkdir -p $(dir $@)
	@echo "[LD]  mach64.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(MACH64_OBJS) \
		$(BUILD_DIR)/eps/DobSettings_stub.o \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOB_OBJS) \
		-o $@

# Generic driver rule (for drivers that only need libc + libdob)
# USB UHCI host-controller driver (USB 1.1). Beyond the base libdob set,
# it enumerates attached devices and matches them against the device-level
# DAS files in /SYSTEM/CONFIG/DAS/USB/ via the shared usb_common/usb_das
# matcher, which reads those files through the DobFileSystem EPS stub. Hence
# usb_das.o on the link line and DobFileSystem_stub.o for the dobfs_* API.
# (usb_ehci / usb_xhci will reuse usb_das.o the same way once their
# enumeration reaches the same point.)
USB_UHCI_CFLAGS := $(CFLAGS) -I$(ROOT_DIR)/drivers/usb_common -I$(BUILD_DIR)/eps

$(BUILD_DIR)/drivers/usb_uhci.mdl: $(ROOT_DIR)/drivers/usb_uhci/main.c \
		$(ROOT_DIR)/drivers/usb_common/usb_das.c \
		$(ROOT_DIR)/drivers/usb_common/usb_das.h \
		$(ROOT_DIR)/libdob/include/dob/usb_uhci_protocol.h \
		$(LIBC_OBJS) $(LIBDOB_OBJS) $(BUILD_DIR)/eps/DobFileSystem_stub.o
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/usb_uhci/main.c"
	@$(CC) $(USB_UHCI_CFLAGS) -c $< -o $(BUILD_DIR)/drivers/usb_uhci_main.o
	@echo "[CC]  drivers/usb_common/usb_das.c"
	@$(CC) $(USB_UHCI_CFLAGS) -c $(ROOT_DIR)/drivers/usb_common/usb_das.c \
		-o $(BUILD_DIR)/drivers/usb_das.o
	@echo "[LD]  usb_uhci.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(BUILD_DIR)/drivers/usb_uhci_main.o \
		$(BUILD_DIR)/drivers/usb_das.o \
		$(BUILD_DIR)/eps/DobFileSystem_stub.o \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOB_OBJS) \
		-o $@

# x3100 (Intel GMA X3100, GM965/gen4) — full multi-file driver: the ported DV
# data plane (main.c) + the i965 2D engine (x3100_2d.c) + the X3100 hardware
# module (x3100_modeset.c) + the IPC/fast transports + the int 0x85 fast entry.
X3100_CFLAGS := $(CFLAGS) -I$(ROOT_DIR)/drivers/x3100 -I$(BUILD_DIR)/eps

X3100_OBJS := \
    $(BUILD_DIR)/drivers/x3100_main.o \
    $(BUILD_DIR)/drivers/x3100_modeset.o \
    $(BUILD_DIR)/drivers/x3100_2d.o \
    $(BUILD_DIR)/drivers/x3100_transport_ipc.o \
    $(BUILD_DIR)/drivers/x3100_transport_fast.o \
    $(BUILD_DIR)/drivers/x3100_fast_entry.o

X3100_SRCS := \
    $(ROOT_DIR)/drivers/x3100/main.c \
    $(ROOT_DIR)/drivers/x3100/x3100_modeset.c \
    $(ROOT_DIR)/drivers/x3100/x3100_2d.c \
    $(ROOT_DIR)/drivers/x3100/x3100_transport_ipc.c \
    $(ROOT_DIR)/drivers/x3100/x3100_transport_fast.c \
    $(ROOT_DIR)/drivers/x3100/x3100_fast_entry.asm \
    $(ROOT_DIR)/drivers/x3100/x3100_2d.h \
    $(ROOT_DIR)/drivers/x3100/x3100_state.h \
    $(ROOT_DIR)/drivers/x3100/x3100_hw.h \
    $(ROOT_DIR)/drivers/x3100/dobvc_protocol.h

$(BUILD_DIR)/drivers/x3100_main.o: $(ROOT_DIR)/drivers/x3100/main.c $(X3100_SRCS)
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/x3100/main.c"
	@$(CC) $(X3100_CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/x3100_modeset.o: $(ROOT_DIR)/drivers/x3100/x3100_modeset.c
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/x3100/x3100_modeset.c"
	@$(CC) $(X3100_CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/x3100_2d.o: $(ROOT_DIR)/drivers/x3100/x3100_2d.c
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/x3100/x3100_2d.c"
	@$(CC) $(X3100_CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/x3100_transport_ipc.o: $(ROOT_DIR)/drivers/x3100/x3100_transport_ipc.c
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/x3100/x3100_transport_ipc.c"
	@$(CC) $(X3100_CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/x3100_transport_fast.o: $(ROOT_DIR)/drivers/x3100/x3100_transport_fast.c
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/x3100/x3100_transport_fast.c"
	@$(CC) $(X3100_CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/x3100_fast_entry.o: $(ROOT_DIR)/drivers/x3100/x3100_fast_entry.asm
	@mkdir -p $(dir $@)
	@echo "[ASM] drivers/x3100/x3100_fast_entry.asm"
	@$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/drivers/x3100.mdl: $(X3100_OBJS) $(LIBC_OBJS) $(LIBDOB_OBJS) $(X3100_SRCS) \
		$(BUILD_DIR)/eps/DobSettings_stub.o
	@mkdir -p $(dir $@)
	@echo "[LD]  x3100.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
	$(BUILD_DIR)/libc/crt0.o \
	$(X3100_OBJS) \
	$(BUILD_DIR)/eps/DobSettings_stub.o \
	$(BUILD_DIR)/libc/string.o \
	$(BUILD_DIR)/libc/stdio.o \
	$(BUILD_DIR)/libc/stdlib.o \
	$(LIBDOB_OBJS) \
	-o $@

$(BUILD_DIR)/drivers/%.mdl: $(ROOT_DIR)/drivers/%/main.c $(LIBC_OBJS) $(LIBDOB_OBJS)
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/$*/main.c"
	@$(CC) $(CFLAGS) -c $< -o $(BUILD_DIR)/drivers/$*_main.o
	@echo "[LD]  $*.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(BUILD_DIR)/drivers/$*_main.o \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOB_OBJS) \
		-o $@


# usb_mass_storage.mdl — USB BOT/SCSI sub-driver. Explicit rule: needs
# partition.o (volume scan/announce, same as ata/ahci).
$(BUILD_DIR)/drivers/usb_mass_storage.mdl: $(ROOT_DIR)/drivers/usb_mass_storage/main.c \
		$(ROOT_DIR)/libdob/include/dob/usb_uhci_protocol.h \
		$(LIBC_OBJS) $(LIBDOB_OBJS) $(BUILD_DIR)/libdob/partition.o \
		$(BUILD_DIR)/eps/DobFileSystem_stub.o
	@mkdir -p $(dir $@)
	@echo "[CC]  drivers/usb_mass_storage/main.c"
	@$(CC) $(CFLAGS) -I$(BUILD_DIR)/eps -c $< -o $(BUILD_DIR)/drivers/usb_mass_storage_main.o
	@echo "[LD]  usb_mass_storage.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(BUILD_DIR)/drivers/usb_mass_storage_main.o \
		$(BUILD_DIR)/eps/DobFileSystem_stub.o \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOB_OBJS_WITH_PARTITION) \
		-o $@

clean:
	@rm -rf $(BUILD_DIR)/drivers
