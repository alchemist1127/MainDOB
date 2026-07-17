# MainDOB - Root Makefile
# Just type 'make' — it figures out the rest.
# No cross-compiler? It uses Docker automatically.

-include toolchain.mk
include config.mk

# Fallback toolchain values — used when toolchain.mk is absent (Docker, CI).
# If toolchain.mk exists and defines these, the ?= assignments are no-ops.
CC             ?= i686-elf-gcc
AS             ?= nasm
LD             ?= i686-elf-ld
QEMU           ?= qemu-system-i386
GRUB_MKRESCUE  ?= grub-mkrescue
KERNEL_CFLAGS  ?= -m32 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
                   -ffunction-sections -fdata-sections \
                   -Wall -Wextra -std=gnu11
USER_CFLAGS    ?= -m32 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
                   -Wall -Wextra -std=gnu11
ASFLAGS        ?= -f elf32
LDFLAGS_KERNEL ?= -m elf_i386 -nostdlib --gc-sections
LDFLAGS_USER   ?= -m elf_i386 -nostdlib


export ROOT_DIR BUILD_DIR BUILD_MODE

# === QEMU ===
QEMU_MEMORY := 256M

# Logical CPUs handed to QEMU. Default 2 — the 1.0 test profile is back
# for the multicore performance work (mirrors the CQ62 dual-core target).
# Override per-run, e.g. `make run QEMU_SMP=1` (uniprocessor regression)
# or `make run QEMU_SMP=4`. The guest boots fine with more cores than it
# starts: unstarted APs stay halted (no SIPI until bring-up).
QEMU_SMP ?= 2

# Machine model. The porting work toward PCIe / modern hardware targets the
# Q35 chipset, which is what exercises the new code paths:
#   - q35 exposes an ECAM region (MCFG), so the kernel's memory-mapped PCI
#     config path lights up instead of the legacy 0xCF8/0xCFC ports.
#   - q35 provides an IOAPIC (MADT), so the IOAPIC backend + migration run
#     instead of staying on the 8259.
# Switch back to the legacy machine by selecting QEMU_MACHINE_PC below; that
# reproduces the old behaviour (PIIX3, no ECAM, PIC) for regression testing.
QEMU_MACHINE_PC  := pc
QEMU_MACHINE_Q35 := q35
QEMU_MACHINE     := $(QEMU_MACHINE_Q35)

# CPU model + APIC control. NOAPIC turns off the CPU's local APIC feature
# (-apic): with no LAPIC present the guest cannot use the IOAPIC either, so
# interrupt delivery falls back to the legacy 8259 PIC. Pair it with
# QEMU_MACHINE_PC for a fully legacy, PIC-only target. Switch back to
# QEMU_CPU_APIC to restore the LAPIC/IOAPIC paths (needed with q35/SMP).
#   make run QEMU_CPU=$(QEMU_CPU_NOAPIC) QEMU_MACHINE=$(QEMU_MACHINE_PC) QEMU_SMP=1
# reproduces the fully legacy PIC-only profile for one run. Default is the
# 1.0 profile (q35 + APIC + SMP=2): the LAPIC/IOAPIC/SMP paths under test.
QEMU_CPU_APIC   := qemu32
QEMU_CPU_NOAPIC := qemu32,-apic
QEMU_CPU        := $(QEMU_CPU_APIC)

# Audio driver auto-detect per OS (override con: make run-disk QEMU_AUDIO_DRV=alsa)
ifeq ($(shell uname),Darwin)
  QEMU_AUDIO_DRV ?= coreaudio
else
  # Linux: prova PulseAudio/PipeWire, poi ALSA, infine nessun audio
  QEMU_AUDIO_DRV ?= $(shell \
    if pactl info >/dev/null 2>&1; then echo pa; \
    elif [ -e /dev/snd/controlC0 ]; then echo alsa; \
    else echo none; fi)
endif

# Audio device only (the -machine flag now lives in QEMU_MACHINE / QEMU_BASE,
# so it is not repeated here — a second -machine would conflict). The pc-spk
# audiodev binding was tied to type=pc; the AC97 codec is what the ac97
# driver actually targets, and it attaches on q35 just as on pc.
QEMU_AUDIO := -audiodev $(QEMU_AUDIO_DRV),id=snd0 -device AC97,audiodev=snd0
# Previously this had `-no-reboot -no-shutdown` to make QEMU freeze on
# reset/shutdown for crash inspection. With SYS_REBOOT live now, those
# flags also block legitimate reboots from MainDOB's "Riavvia ora" or
# any future shutdown UI. Removed — crashes still leave their panic
# traces on the serial stdio, which is enough.
QEMU_BASE  := -machine $(QEMU_MACHINE) -cpu $(QEMU_CPU) -m $(QEMU_MEMORY) -smp $(QEMU_SMP) -serial stdio $(QEMU_AUDIO)

# format=raw is required: without it QEMU's auto-detect silently
# restricts block-0 writes, breaking installer MBR writes.
# cache=writethrough: every guest write is fsync'd to host before the
# command returns, so the installed disk is durable across QEMU
# sessions even with abrupt shutdown.
#
# Three presentations of the same disk image:
#   QEMU_HDA_IDE   — legacy PATA/IDE on the PIIX3 (driver: ata, shows "IDE").
#                    Only valid on the pc machine.
#   QEMU_HDA_SATA  — behind an explicit ich9-ahci controller (driver: ahci,
#                    shows "SATA"). Works on pc; also fine on q35.
#   QEMU_HDA_Q35   — on the Q35 chipset's built-in AHCI (no extra -device).
#                    The most faithful "modern SATA" presentation for q35.
# QEMU_HDA selects which one the run targets use. Default pairs q35 with its
# native AHCI; switch the alias line below to change disk presentation.
QEMU_HDA_IDE  = -drive file="$(TEST_DISK_FILE)",format=raw,cache=writethrough,if=ide,index=0,media=disk
QEMU_HDA_SATA = -drive file="$(TEST_DISK_FILE)",format=raw,cache=writethrough,if=none,id=hd0,media=disk -device ich9-ahci,id=ahci0 -device ide-hd,drive=hd0,bus=ahci0.0
QEMU_HDA_Q35  = -drive file="$(TEST_DISK_FILE)",format=raw,cache=writethrough,if=none,id=hd0,media=disk -device ide-hd,drive=hd0,bus=ide.0
QEMU_HDA    = $(QEMU_HDA_IDE)

# Optical drive attachment.
#
# Hard-won lesson: attaching an optical device to an explicit port INDEX on
# an ich9-ahci controller (bus=ahciN.M for M>0) is NOT portable — QEMU's
# command-line bus naming for the extra ports varies by version, and on the
# target QEMU "Bus 'ahci0.1' not found" even though the controller's own
# port scan shows ports 0..5 exist (the AHCI register-level port count and
# the QEMU -device bus topology are different things). The disk on ahci0.0
# works; higher indices do not.
#
# So on q35 the optical drives use QEMU's `-cdrom`/auto-attach, which QEMU
# places on the machine's built-in AHCI itself — the form that booted in
# the earliest runs. This may sit on a different AHCI controller than the
# explicit ahci0 holding the disk; that is acceptable now that the AHCI
# driver stays resident and supports runtime hotplug (it no longer needs to
# see disk and optical on one controller in a single instance).
#   QEMU_CDROM_EMPTY  — an empty optical drive (insert media via the menu)
#   QEMU_CDROM_ISO    — a secondary optical drive holding $(ISO)
ifeq ($(QEMU_MACHINE),pc)
  QEMU_CDROM_EMPTY = -drive if=ide,index=2,media=cdrom
  QEMU_CDROM_ISO   = -drive file="$(ISO)",if=ide,index=2,media=cdrom
else
  QEMU_CDROM_EMPTY = -drive if=none,id=optempty,media=cdrom -device ide-cd,drive=optempty
  QEMU_CDROM_ISO   = -drive if=none,id=optical1,media=cdrom -device ide-cd,drive=optical1
endif

# Boot ISO (the live CD that runs the installer). The plain `-cdrom` form
# is what reliably boots on both machines: on pc it is the legacy IDE CD, on
# q35 QEMU attaches it to the built-in AHCI and `-boot d` boots it. This is
# the form that worked in the earliest q35 runs; the explicit-port
# experiment above broke it, so we are back to the dependable shorthand.
QEMU_BOOT_ISO = -cdrom "$(ISO_FILE)"
DISK_FILE  := disk.img
DISK_SIZE  := 32  # MB

# === USB test attachment ===
# q35 has no PIIX UHCI on its own (it is a modern chipset; its USB is
# xHCI/EHCI). The usb_uhci driver targets USB 1.1 UHCI, so to exercise it we
# attach a standalone piix3-usb-uhci controller plus a mass-storage device
# (a "data pendrive"). The backing image only needs to exist — enumeration
# and the USB-DAS match happen before any sector is read, so an unformatted
# 16 MB image is enough to make QEMU present a mass-storage device whose
# interface advertises class 0x08 / subclass 0x06 / protocol 0x50, which
# matches config/DAS/USB/mass_storage.das.
#
# Build the image once on the host:  qemu-img create -f raw usbstick.img 16M
# Then boot with the `run-usb` target below (device present at startup — the
# simplest case, isolating enumeration + DAS match from runtime hotplug).
USBSTICK_FILE := usbstick.img
QEMU_USB = -device piix3-usb-uhci,id=uhci \
           -drive if=none,id=usbstick,format=raw,file="$(USBSTICK_FILE)" \
           -device usb-storage,bus=uhci.0,drive=usbstick

# === LIVE FILESYSTEM BLOB ===
# Flat FAT32 superfloppy carrying /SYSTEM and /DATA. Embedded in the
# ISO as a GRUB module under the cmdline tag "live_fs"; the kernel
# bootfs reads it straight from RAM, so the CD never gets touched
# again after boot. Sized to comfortably fit the userspace tree —
# every byte here is mirrored into RAM at boot.
LIVE_IMG  := $(BUILD_DIR)/live.img
LIVE_SIZE := 32  # MB — must be >= 32 to give mkfs.fat enough clusters for a
                 # well-formed FAT32 with default cluster size. The kernel
                 # bootfs parser doesn't check cluster count and would happily
                 # read a "small FAT32" with too few clusters, but smaller
                 # volumes risk mkfs.fat refusing or producing FAT16. The
                 # blob mirrors into RAM at boot; size it for content fit
                 # plus a little slack, not for "maximum we could afford".

# Secondary disk: a small, blank image attached to QEMU as -hdb. Lets
# the user exercise DobDisk's destructive operations (format, partition,
# rename, delete) without touching the boot disk. Created blank — no
# MBR, no partitions — so MainDOB itself can build the layout via
# DobDisk's "Crea partizione" / "Formatta" actions. Kept across builds
# (clean does not wipe it), so partitions you create on it persist;
# distclean removes it together with disk.img.
TEST_DISK_FILE := test_disk.img
TEST_DISK_SIZE := 48  # MB

# === Floppy test attachment ===
# The pc (i440FX/PIIX3) machine carries an onboard floppy controller
# (isa-fdc). Modern QEMU only presents a drive when a backing image is
# supplied, so we hand it a blank 1.44 MB image. This exercises
# drivers/floppy + boot/floppyprobe. Attached via if=floppy so QEMU wires
# it to the onboard FDC (fd0). Kept across builds; distclean removes it.
#   Skip the floppy for one run:  make run QEMU_FLOPPY=
FLOPPY_FILE := floppy.img
FLOPPY_SIZE := 1440  # KiB — standard 3.5" 1.44 MB
QEMU_FLOPPY  = -drive file="$(FLOPPY_FILE)",if=floppy,format=raw,index=0

# === Auto-detect build method ===
# macOS non può creare dischi bootable nativamente (no losetup/grub-install),
# quindi forziamo la via Docker anche se il cross-compiler è installato.
HAS_CROSS := $(shell command -v i686-elf-gcc >/dev/null 2>&1 && echo yes || echo no)
ifeq ($(shell uname),Darwin)
HAS_CROSS := no
endif
HAS_DOCKER := $(shell command -v docker >/dev/null 2>&1 && echo yes || echo no)
DOCKER_IMAGE := maindob-builder

.PHONY: all run run-disk run-cd clean distclean info debug \
        tools kernel libc libdob libdobui eps boot drivers programs userspace iso live disk test-disk floppy grub-blobs \
        libdobfont libdobdoc libdoblayout libdobpage \
        docker-image docker-build

# =====================================================================
# DEFAULT TARGETS — just 'make' and 'make run'
# =====================================================================
ifeq ($(HAS_CROSS),yes)
# --- Native build (Linux with cross-compiler) ---
all: iso
run: iso test-disk floppy
	@echo "[QEMU] Booting MainDOB live CD (install target on -hda)..."
	@$(QEMU) $(QEMU_BASE) $(QEMU_BOOT_ISO) $(QEMU_HDA) $(QEMU_FLOPPY) -boot d
else ifeq ($(HAS_DOCKER),yes)
# --- Docker build (macOS or Linux without cross-compiler) ---
all: docker-build
run: docker-build test-disk floppy
	@echo "[QEMU] Booting MainDOB live CD (install target on -hda)..."
	@$(QEMU) $(QEMU_BASE) $(QEMU_BOOT_ISO) $(QEMU_HDA) $(QEMU_FLOPPY) -boot d
else
all:
	@echo ""
	@echo "  ERROR: No cross-compiler (i686-elf-gcc) and no Docker found."
	@echo ""
	@echo "  Option A: Install Docker Desktop → https://docker.com"
	@echo "  Option B (Linux): bash setup.sh"
	@echo ""
	@exit 1
run: all
endif

# =====================================================================
# NATIVE BUILD PHASES (used when cross-compiler is available)
# =====================================================================

tools:
	@mkdir -p $(BUILD_DIR)
	@$(MAKE) -C tools BUILD_DIR=$(BUILD_DIR) ROOT_DIR=$(ROOT_DIR)

kernel:
	@mkdir -p $(BUILD_DIR)
	@$(MAKE) -C kernel BUILD_DIR=$(BUILD_DIR) ROOT_DIR=$(ROOT_DIR)

libc:
	@$(MAKE) -C libc BUILD_DIR=$(BUILD_DIR) ROOT_DIR=$(ROOT_DIR)

libdob: libc
	@$(MAKE) -C libdob BUILD_DIR=$(BUILD_DIR) ROOT_DIR=$(ROOT_DIR)

# DobWrite stack: scalable font engine, document model, layout engine.
libdobfont: libc libdob
	@$(MAKE) -C libdobfont BUILD_DIR=$(BUILD_DIR) ROOT_DIR=$(ROOT_DIR)

libdobdoc: libc libdob
	@$(MAKE) -C libdobdoc BUILD_DIR=$(BUILD_DIR) ROOT_DIR=$(ROOT_DIR)

libdoblayout: libc libdob libdobfont libdobdoc
	@$(MAKE) -C libdoblayout BUILD_DIR=$(BUILD_DIR) ROOT_DIR=$(ROOT_DIR)

libdobpage: libc libdobfont libdoblayout
	@$(MAKE) -C libdobpage BUILD_DIR=$(BUILD_DIR) ROOT_DIR=$(ROOT_DIR)

# Entry Point System — copy interface headers and compile stubs once.
# All components can find them with #include <DobInterface.h> etc.
EPS_CFLAGS := $(USER_CFLAGS) $(OPT_FLAGS) \
              -I$(ROOT_DIR)/libc/include -I$(ROOT_DIR)/libdob/include \
              -I$(BUILD_DIR)/eps -I$(ROOT_DIR)/libdobui

eps: libdob
	@mkdir -p $(BUILD_DIR)/eps
	@cp $(ROOT_DIR)/boot/dobinterface/DobInterface.h $(BUILD_DIR)/eps/
	@cp $(ROOT_DIR)/boot/DobFileSystem/DobFileSystem.h $(BUILD_DIR)/eps/
	@cp $(ROOT_DIR)/boot/DobFileSystem/dobfs_protocol.h $(BUILD_DIR)/eps/
	@cp $(ROOT_DIR)/boot/popups/DobPopup.h $(BUILD_DIR)/eps/
	@cp $(ROOT_DIR)/boot/config/DobConfig.h $(BUILD_DIR)/eps/
	@cp $(ROOT_DIR)/programs/DobFiles/DobFiles.h $(BUILD_DIR)/eps/
	@cp $(ROOT_DIR)/drivers/ac97/DobAudio.h $(BUILD_DIR)/eps/
	@cp $(ROOT_DIR)/boot/audioplayer/DobAudioPlayer.h $(BUILD_DIR)/eps/
	@cp $(ROOT_DIR)/programs/DobArchive/DobArchive.h $(BUILD_DIR)/eps/
	@cp $(ROOT_DIR)/programs/DobArchive/archive_protocol.h $(BUILD_DIR)/eps/
	@cp $(ROOT_DIR)/programs/DobTable/DobTable.h $(BUILD_DIR)/eps/
	@cp $(ROOT_DIR)/programs/DobTable/dobtable_protocol.h $(BUILD_DIR)/eps/
	@cp $(ROOT_DIR)/drivers/bga/dobvc_protocol.h $(BUILD_DIR)/eps/
	@cp $(ROOT_DIR)/boot/settingsd/DobSettings.h $(BUILD_DIR)/eps/
	@cp $(ROOT_DIR)/libdob/include/dob/dobsettings_protocol.h $(BUILD_DIR)/eps/
	@echo "[EPS] Compiling stubs..."
	@$(CC) $(EPS_CFLAGS) -c $(ROOT_DIR)/boot/dobinterface/DobInterface_stub.c -o $(BUILD_DIR)/eps/DobInterface_stub.o
	@$(CC) $(EPS_CFLAGS) -c $(ROOT_DIR)/boot/DobFileSystem/DobFileSystem_stub.c -o $(BUILD_DIR)/eps/DobFileSystem_stub.o
	@$(CC) $(EPS_CFLAGS) -c $(ROOT_DIR)/boot/popups/DobPopup_stub.c -o $(BUILD_DIR)/eps/DobPopup_stub.o
	@$(CC) $(EPS_CFLAGS) -c $(ROOT_DIR)/boot/popups/DobPopup_toast_stub.c -o $(BUILD_DIR)/eps/DobPopupToast_stub.o
	@$(CC) $(EPS_CFLAGS) -c $(ROOT_DIR)/boot/config/DobConfig_stub.c -o $(BUILD_DIR)/eps/DobConfig_stub.o
	@$(CC) $(EPS_CFLAGS) -c $(ROOT_DIR)/programs/DobFiles/DobFiles_stub.c -o $(BUILD_DIR)/eps/DobFiles_stub.o
	@$(CC) $(EPS_CFLAGS) -c $(ROOT_DIR)/drivers/ac97/DobAudio_stub.c -o $(BUILD_DIR)/eps/DobAudio_stub.o
	@$(CC) $(EPS_CFLAGS) -c $(ROOT_DIR)/boot/audioplayer/DobAudioPlayer_stub.c -o $(BUILD_DIR)/eps/DobAudioPlayer_stub.o
	@$(CC) $(EPS_CFLAGS) -c $(ROOT_DIR)/programs/DobArchive/DobArchive_stub.c -o $(BUILD_DIR)/eps/DobArchive_stub.o
	@$(CC) $(EPS_CFLAGS) -c $(ROOT_DIR)/programs/DobTable/DobTable_stub.c -o $(BUILD_DIR)/eps/DobTable_stub.o
	@$(CC) $(EPS_CFLAGS) -c $(ROOT_DIR)/libdob/src/DobVideoControl_stub.c -o $(BUILD_DIR)/eps/DobVideoControl_stub.o
	@$(CC) $(EPS_CFLAGS) -c $(ROOT_DIR)/boot/settingsd/DobSettings_stub.c -o $(BUILD_DIR)/eps/DobSettings_stub.o

libdobui: libc libdob eps
	@$(MAKE) -C libdobui BUILD_DIR=$(BUILD_DIR) ROOT_DIR=$(ROOT_DIR)

boot: libc libdob libdobui eps
	@$(MAKE) -C boot BUILD_DIR=$(BUILD_DIR) ROOT_DIR=$(ROOT_DIR)

drivers: libc libdob
	@$(MAKE) -C drivers BUILD_DIR=$(BUILD_DIR) ROOT_DIR=$(ROOT_DIR)

programs: tools libc libdob libdobui eps libdobfont libdobdoc libdoblayout libdobpage
	@$(MAKE) -C programs BUILD_DIR=$(BUILD_DIR) ROOT_DIR=$(ROOT_DIR)

userspace: boot drivers programs

# === GRUB BLOBS FOR INSTALLER ===
# Pre-built MBR boot.img + post-MBR core.img shipped inside the live
# blob. INST_PHASE_GRUB in MainDOB_Setup reads them from the live FS
# at install time and writes them to the target disk's sector 0 (MBR)
# and sectors 1..N (post-MBR gap) — no grub-install on the running
# system, the bytes come from the build environment.
#
# boot.img is shipped by grub-pc-bin verbatim; core.img is generated
# with the minimal module set required to read FAT32 (biosdisk
# part_msdos fat), parse a config file (configfile normal), and load
# a multiboot kernel (multiboot). prefix=/boot/grub points GRUB at
# the grub.cfg the installer drops on the target partition.
GRUB_MKIMAGE   ?= grub-mkimage
GRUB_HOST_DIR  ?= /usr/lib/grub/i386-pc
GRUB_BLOBS_DIR := $(BUILD_DIR)/grub-blobs
GRUB_BOOT_IMG  := $(GRUB_BLOBS_DIR)/boot.img
GRUB_CORE_IMG  := $(GRUB_BLOBS_DIR)/core.img

$(GRUB_BLOBS_DIR):
	@mkdir -p $@

$(GRUB_BOOT_IMG): | $(GRUB_BLOBS_DIR)
	@echo "[GRUB] Copying boot.img from $(GRUB_HOST_DIR)"
	@cp $(GRUB_HOST_DIR)/boot.img $@

$(GRUB_CORE_IMG): | $(GRUB_BLOBS_DIR)
	@echo "[GRUB] Building core.img (prefix=(hd0,msdos1)/boot/grub, embed cfg)"
	@printf '%s\n' \
	    'set timeout=3' \
	    'set default=0' \
	    'set root=(hd0,msdos1)' \
	    '' \
	    'menuentry "MainDOB" {' \
	    '    multiboot /boot/kernel.bin' \
	    '    boot' \
	    '}' \
	    > $(GRUB_BLOBS_DIR)/embed.cfg
	@$(GRUB_MKIMAGE) -O i386-pc -c $(GRUB_BLOBS_DIR)/embed.cfg \
	    -p "(hd0,msdos1)/boot/grub" -o $@ \
	    biosdisk part_msdos fat multiboot configfile normal ls

grub-blobs: $(GRUB_BOOT_IMG) $(GRUB_CORE_IMG)

live: kernel userspace grub-blobs
	@bash $(ROOT_DIR)/tools/mklive.sh $(LIVE_IMG) $(LIVE_SIZE) $(BUILD_DIR) $(ROOT_DIR)

iso: kernel live
	@mkdir -p $(ISO_DIR)/boot/grub
	@cp $(KERNEL_BIN) $(ISO_DIR)/boot/kernel.bin
	@cp $(LIVE_IMG) $(ISO_DIR)/boot/live.img
	@$(GRUB_MKRESCUE) -o $(ISO_FILE) $(ISO_DIR) 2>/dev/null
	@echo "[ISO] $(ISO_FILE) created."

# === BOOTABLE DISK IMAGE ===
# Creates a fully bootable FAT32 disk with GRUB and all programs installed.
# mkbootdisk.sh uses mtools + grub-mkimage — no root, no losetup, no mount.
# Requires: mtools, grub-pc-bin (Debian/Ubuntu) or grub2-tools (Fedora/Arch).
disk: kernel userspace
	@if [ ! -f $(DISK_FILE) ]; then \
		bash $(ROOT_DIR)/tools/mkbootdisk.sh $(DISK_FILE) $(DISK_SIZE) $(BUILD_DIR) $(ROOT_DIR); \
	else \
		echo "[DISK] $(DISK_FILE) already exists (keeping data)."; \
	fi

# === TEST DISK ===
# A blank secondary disk image attached to QEMU as -hdb. No MBR, no
# partitions, no filesystem — just zeros — so that DobDisk's
# "Crea partizione" / "Formatta" actions have something real to
# operate on. Preserved across builds (only distclean removes it);
# delete it manually to start fresh.
test-disk:
	@if [ ! -f $(TEST_DISK_FILE) ]; then \
		echo "[TEST-DISK] Creating blank $(TEST_DISK_SIZE) MB image at $(TEST_DISK_FILE)..."; \
		dd if=/dev/zero of=$(TEST_DISK_FILE) bs=1M count=$(TEST_DISK_SIZE) 2>/dev/null; \
		echo "[TEST-DISK] Done."; \
	else \
		echo "[TEST-DISK] $(TEST_DISK_FILE) already exists (keeping data)."; \
	fi

# Blank 1.44 MB floppy image for the onboard FDC. Created once, then kept
# (so anything written to it survives across runs); distclean removes it.
floppy:
	@if [ ! -f $(FLOPPY_FILE) ]; then \
		echo "[FLOPPY] Creating blank $(FLOPPY_SIZE) KiB image at $(FLOPPY_FILE)..."; \
		dd if=/dev/zero of=$(FLOPPY_FILE) bs=1024 count=$(FLOPPY_SIZE) 2>/dev/null; \
		echo "[FLOPPY] Done."; \
	else \
		echo "[FLOPPY] $(FLOPPY_FILE) already exists (keeping data)."; \
	fi

# =====================================================================
# DOCKER BUILD (used automatically on macOS)
# =====================================================================

docker-image:
	@echo "[DOCKER] Building build environment (first time ~15 min, then cached)..."
	@docker build --platform linux/amd64 -t $(DOCKER_IMAGE) .
	@echo "[DOCKER] Image ready."

docker-build: docker-image
	@echo "[DOCKER] Compiling MainDOB..."
	@docker run --platform linux/amd64 --rm -v "$(shell pwd):/src" -w /src $(DOCKER_IMAGE) bash -c "make clean && make iso"
	@echo "[DOCKER] Done → $(ISO_FILE)"

# =====================================================================
# UTILITIES
# =====================================================================

debug: iso test-disk floppy
	@echo "[QEMU] Debug mode — GDB on :1234"
	@$(QEMU) $(QEMU_BASE) $(QEMU_BOOT_ISO) $(QEMU_HDA) $(QEMU_FLOPPY) -boot d -s -S

# Re-run the live CD with current build, no rebuild. Fails if the
# ISO doesn't exist (run `make run` once to build it).
rerun: test-disk floppy
	@if [ ! -f "$(ISO_FILE)" ]; then \
	    echo "[RERUN] ISO not found at $(ISO_FILE) — run 'make run' once first."; \
	    exit 1; \
	fi
	@# GUARDIA ANTI-STANTIO: rerun salta il rebuild PER VELOCITA', non per
	@# testare binari vecchi. Se un sorgente qualunque (kernel, libc,
	@# libdob, driver, boot, programs) e' piu' recente della ISO, il test
	@# NON sta eseguendo il codice che credi — e' gia' successo: tre giri
	@# di caccia bruciati su fix di libc mai entrati nell'immagine.
	@STALE=$$(find kernel libc libdob boot drivers programs common_files \
	          \( -name '*.c' -o -name '*.h' -o -name '*.asm' \) \
	          -newer "$(ISO_FILE)" 2>/dev/null | head -1); \
	if [ -n "$$STALE" ]; then \
	    echo "[RERUN] RIFIUTATO: '$$STALE' e' piu' recente della ISO."; \
	    echo "[RERUN] I binari nell'immagine NON contengono le ultime"; \
	    echo "[RERUN] modifiche. Usa 'make run' (rebuild) — o, se vuoi"; \
	    echo "[RERUN] DAVVERO testare la ISO vecchia: 'make rerun-stale'."; \
	    exit 1; \
	fi
	@echo "[QEMU] Re-running live CD (no rebuild)..."
	@$(QEMU) $(QEMU_BASE) $(QEMU_BOOT_ISO) $(QEMU_HDA) $(QEMU_FLOPPY) -boot d

# Come rerun, ma senza la guardia: per confronti deliberati con una
# immagine vecchia. Il nome dice cosa stai facendo.
rerun-stale: test-disk floppy
	@echo "[QEMU] Re-running STALE live CD (deliberato)..."
	@$(QEMU) $(QEMU_BASE) $(QEMU_BOOT_ISO) $(QEMU_HDA) $(QEMU_FLOPPY) -boot d

# Boot from disk only — no live CD.
# After `make run` and a successful wizard install onto -hda (the
# TEST_DISK_FILE), this target boots that very installed disk so you can
# verify the system comes up via GRUB from the target's MBR.
#
# One empty IDE CD-ROM is attached (driver: ata). It starts with no disc,
# so there is NO iso dependency and nothing is rebuilt: this target boots
# the hard disk, period. Insert an ISO at runtime from the QEMU menu
# (Machine -> Removable media) to exercise CD-ROM detection.
run-disk: test-disk floppy
	@echo "[QEMU] Booting installed disk (empty CD drive; insert media via QEMU menu)..."
	@$(QEMU) $(QEMU_BASE) $(QEMU_HDA) $(QEMU_FLOPPY) -boot c \
	    $(QEMU_CDROM_EMPTY)

# Boot the installed disk with a USB pendrive attached at startup, on a
# standalone UHCI controller, to exercise the usb_uhci driver's enumeration
# + USB-DAS match. Watch the serial log for:
#     [uhci] ENUM DONE port N: VID=.... ifClass=08 sub=06 proto=50
#     [uhci] DAS match: USB Mass Storage -> subdev class=01:06
# and a USB-storage icon appearing on the desktop.
#
# Create the backing image once before first use:
#     qemu-img create -f raw usbstick.img 16M
run-usb: test-disk
	@if [ ! -f "$(USBSTICK_FILE)" ]; then \
		echo "[QEMU] Creating $(USBSTICK_FILE) (16 MB) ..."; \
		qemu-img create -f raw "$(USBSTICK_FILE)" 16M >/dev/null; \
	fi
	@echo "[QEMU] Booting installed disk with a USB pendrive on UHCI..."
	@$(QEMU) $(QEMU_BASE) $(QEMU_HDA) -boot c \
	    $(QEMU_CDROM_EMPTY) \
	    $(QEMU_USB)

# Boot the installed disk with the UHCI controller and the stick's
# DRIVE BACKEND defined but NO usb-storage device attached: the runtime
# hot-plug test. Switch to the QEMU monitor (Ctrl+Alt+2, on macOS
# Ctrl+Option+2) and drive the test by hand:
#
#     device_add usb-storage,id=pen,bus=uhci.0,drive=usbstick   <- inserimento
#     device_del pen                                             <- rimozione
#
# Expected: icon appears on insert (RD wake -> enumeration -> announce),
# icon disappears on removal (GONE), usbms exits (DETACH), the mount
# flushes and dies (DOBFS_SHUTDOWN), any DobFiles window on the stick
# closes (UNMOUNT_NOTIFY). Re-adding re-runs the pipeline from scratch.
# Ctrl+Alt+1 / Ctrl+Option+1 returns to the guest display.
run-hotplug: test-disk
	@if [ ! -f "$(USBSTICK_FILE)" ]; then \
		echo "[QEMU] Creating $(USBSTICK_FILE) (16 MB) ..."; \
		qemu-img create -f raw "$(USBSTICK_FILE)" 16M >/dev/null; \
	fi
	@echo "[QEMU] Hot-plug test: monitor = Ctrl+Alt+2 (mac: Ctrl+Option+2)"
	@echo "[QEMU]   attach: device_add usb-storage,id=pen,bus=uhci.0,drive=usbstick"
	@echo "[QEMU]   detach: device_del pen        (ripetibili quante volte si vuole)"
	@$(QEMU) $(QEMU_BASE) $(QEMU_HDA) -boot c \
	    $(QEMU_CDROM_EMPTY) \
	    -device piix3-usb-uhci,id=uhci \
	    -blockdev driver=file,node-name=usbstickf,filename="$(USBSTICK_FILE)" \
	    -blockdev driver=raw,node-name=usbstick,file=usbstickf
# Il backend e' -blockdev, NON -drive if=none: con -drive, al device_del
# QEMU distrugge anche il backend (semantica legacy: il blocco "appartiene"
# al device) e ogni re-add fallisce con "can't find value 'usbstick'" —
# il ciclo insert/remove era testabile UNA volta sola per sessione. I nodi
# -blockdev sopravvivono all'unplug per contratto: add/del a volonta'.

# Boot the live CD with an additional ISO attached as a secondary
# optical device. Useful when you want the cdrom driver inside the
# running system to see another disc (e.g. for ISO9660 testing) while
# still booting from the regular MainDOB install CD.
#
# Usage: make run-cd ISO=/path/to/something.iso
run-cd: iso test-disk floppy
	@if [ -z "$(ISO)" ]; then \
		echo "Usage: make run-cd ISO=/path/to/image.iso"; exit 1; \
	fi
	@if [ ! -f "$(ISO)" ]; then \
		echo "[QEMU] ERROR: ISO file $(ISO) not found"; exit 1; \
	fi
	@echo "[QEMU] Live CD + secondary ISO $(ISO)..."
	@$(QEMU) $(QEMU_BASE) $(QEMU_BOOT_ISO) $(QEMU_HDA) $(QEMU_FLOPPY) -boot d $(QEMU_CDROM_ISO)

clean:
	@rm -rf $(BUILD_DIR)
	@echo "[CLEAN] Done. (disk.img preserved — use 'make distclean' to remove)"

distclean: clean
	@rm -f $(DISK_FILE) $(TEST_DISK_FILE) $(FLOPPY_FILE)
	@echo "[DISTCLEAN] Disk image, test disk and floppy image removed."

info:
	@echo "MainDOB Build System"
	@echo ""
	@echo "  Cross-compiler: $(HAS_CROSS)"
	@echo "  Docker:         $(HAS_DOCKER)"
	@echo "  QEMU:           $(QEMU)"
	@echo ""
	@echo "  make            Build the live CD ISO (auto-detects Docker vs native)"
	@echo "  make run        Build + boot live CD ($(TEST_DISK_SIZE) MB blank target on -hda)"
	@echo "  make iso        Build the live CD ISO only"
	@echo "  make live       Build the live FAT32 blob only"
	@echo "  make disk       Build a pre-installed reference HDD (legacy path)"
	@echo "  make run-disk   Boot the installed disk + IDE & SATA CD-ROMs"
	@echo "  make run-cd ISO=file.iso  Live CD + an extra ISO on the secondary optical drive"
	@echo "  make test-disk  Create the blank $(TEST_DISK_SIZE) MB install-target disk"
	@echo "  make debug      Build + QEMU with GDB on :1234"
	@echo "  make clean      Remove build artifacts"
	@echo "  make distclean  Remove everything including disk images"
	@echo "  make info       This help"
