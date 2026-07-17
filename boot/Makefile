# MainDOB Boot Services Makefile (OS services in /SYSTEM/OperatingSystem/)
#
# Each service is a single .mdl built from boot/<n>/main.c (plus extra
# .c files when listed).  The build is driven by per-service variables:
#
#   EPS_<n>     := list of EPS stub names this service needs
#   EXTRA_SRC_<n>  := extra .c files compiled alongside main.c
#   EXTRA_INC_<n>  := extra -I directories
#
# Truly special bits (multi-source services, audio decoders) drop to
# their own rule blocks at the bottom.

-include $(ROOT_DIR)/toolchain.mk
include $(ROOT_DIR)/config.mk

LIBC_INC   := -I$(ROOT_DIR)/libc/include
LIBDOB_INC := -I$(ROOT_DIR)/libdob/include
EPS_INC    := -I$(BUILD_DIR)/eps
DOBUI_INC  := -I$(ROOT_DIR)/libdobui
INCLUDES   := $(LIBC_INC) $(LIBDOB_INC) $(EPS_INC) $(DOBUI_INC)

CFLAGS := $(USER_CFLAGS) $(OPT_FLAGS) $(INCLUDES)

SERVERS := console config hotplug DobFileSystem inputd \
           dobinterface audioplayer floppyprobe settingsd

SERVER_BINS := $(patsubst %,$(BUILD_DIR)/boot/%.mdl,$(SERVERS))

# exfat.mem — read-only exFAT parser as a PIC shared object (Phase 1),
# built exactly like drivers/cdrom/iso9660.mem: self-contained, no crt0/
# libc/libdob, only R_386_RELATIVE relocations. DobFileSystem loads it at
# runtime in Phase 3 (libdob/mem.o is already on its link line). The rule
# itself lives at the bottom of this file.
MEM_CFLAGS := $(filter-out -fno-pic -fno-pie, $(USER_CFLAGS)) \
              $(OPT_FLAGS) -fPIC \
              -mno-sse -mno-mmx -mno-80387 -fno-stack-protector \
              -fno-builtin -fvisibility=default \
              $(LIBC_INC) $(LIBDOB_INC)
EXFAT_MEM := $(BUILD_DIR)/boot/DobFileSystem/exfat.mem

LIBC_OBJS   := $(BUILD_DIR)/libc/crt0.o $(BUILD_DIR)/libc/string.o \
               $(BUILD_DIR)/libc/stdio.o $(BUILD_DIR)/libc/stdlib.o
LIBDOB_OBJS := $(BUILD_DIR)/libdob/ipc.o $(BUILD_DIR)/libdob/registry.o \
               $(BUILD_DIR)/libdob/server.o $(BUILD_DIR)/libdob/font.o \
               $(BUILD_DIR)/libdob/window_servo.o $(BUILD_DIR)/libdob/thread.o \
               $(BUILD_DIR)/libdob/mem.o $(BUILD_DIR)/libdob/video.o

# === Per-service EPS stub dependencies ===
EPS_config         := DobFileSystem
EPS_dobinterface   := DobFileSystem DobVideoControl DobSettings
EPS_hotplug        := DobFileSystem DobPopupToast DobSettings
EPS_audioplayer    := DobFileSystem DobAudio
EPS_settingsd      := DobFileSystem
EPS_DobFileSystem  := DobFiles DobFileSystem
# DobFiles → because handle_open_view calls dobfiles_OpenMount.
# DobFileSystem (self) → DobFiles_stub.o pulls in <dob/spawn.h> whose
# inline _spawn_worker uses dobfs_Stat/Open/Read/Close to load the .mdl
# from disk; those land in DobFileSystem_stub.o. The stub IPC routes
# back to ourselves (self-call), which the kernel handles fine — same
# pattern hotplug already uses (it lists DobFileSystem in its own EPS
# even though hotplug isn't the FS service, and DobFiles_stub there
# pulls the same chain).

# Helper: produce the list of EPS stub object files for a given service
eps_objs = $(foreach e,$(EPS_$(1)),$(BUILD_DIR)/eps/$(e)_stub.o)

.PHONY: all clean libc_objs libdob_objs

all: libc_objs libdob_objs $(SERVER_BINS) $(EXFAT_MEM)

libc_objs:
	@$(MAKE) -C $(ROOT_DIR)/libc BUILD_DIR=$(BUILD_DIR) ROOT_DIR=$(ROOT_DIR)

libdob_objs:
	@$(MAKE) -C $(ROOT_DIR)/libdob BUILD_DIR=$(BUILD_DIR) ROOT_DIR=$(ROOT_DIR)

# === Generic single-source service rule ===
# Used by every server whose source tree is just main.c and that doesn't
# need any extra link-line objects beyond libc + libdob + EPS stubs.
# Multi-source services (hotplug, audioplayer) override this with
# explicit rules below.
$(BUILD_DIR)/boot/%.mdl: $(ROOT_DIR)/boot/%/main.c $(LIBC_OBJS) $(LIBDOB_OBJS)
	@mkdir -p $(dir $@)
	@echo "[CC]  boot/$*/main.c"
	@$(CC) $(CFLAGS) -c $< -o $(BUILD_DIR)/boot/$*_main.o
	@echo "[LD]  $*.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(BUILD_DIR)/boot/$*_main.o \
		$(call eps_objs,$*) \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOB_OBJS) \
		-o $@

# Wire per-service EPS stub prerequisites
$(foreach s,$(SERVERS),$(eval \
    $(BUILD_DIR)/boot/$(s).mdl: $(call eps_objs,$(s))))

# === DobFileSystem: links libdob/partition.o for secondary-mount MBR ===
# The --mount mode takes a packed token (partition_index << 24 | selector)
# and reads sector 0 of the disk to find the partition's starting LBA.
# That MBR parse uses partition_mbr_parse from libdob/dob/partition.h —
# hence partition.o on the link line.
$(BUILD_DIR)/boot/DobFileSystem.mdl: $(ROOT_DIR)/boot/DobFileSystem/main.c \
                                     $(LIBC_OBJS) $(LIBDOB_OBJS) \
                                     $(BUILD_DIR)/libdob/partition.o \
                                     $(call eps_objs,DobFileSystem) \
                                     | $(EXFAT_MEM)
	@mkdir -p $(dir $@)
	@echo "[CC]  boot/DobFileSystem/main.c"
	@$(CC) $(CFLAGS) -c $< -o $(BUILD_DIR)/boot/DobFileSystem_main.o
	@echo "[LD]  DobFileSystem.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(BUILD_DIR)/boot/DobFileSystem_main.o \
		$(call eps_objs,DobFileSystem) \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOB_OBJS) \
		$(BUILD_DIR)/libdob/partition.o \
		-o $@

# === hotplug: multi-source (main.c + das.c) ===
# Owns the DAS database, so it needs DobFileSystem_stub (to read
# /SYSTEM/CONFIG/DAS/*.das files and to spawn driver .mdl files via
# spawn_file()).  It also shows messages (the DAS action interpreter
# calls dobpopup_info / dobpopup_error), but being windowless it links
# the toast backend (DobPopupToast) which routes them to a WM banner
# rather than a popup window.
HOTPLUG_OBJS := $(BUILD_DIR)/boot/hotplug_main.o $(BUILD_DIR)/boot/hotplug_das.o

$(BUILD_DIR)/boot/hotplug_%.o: $(ROOT_DIR)/boot/hotplug/%.c
	@mkdir -p $(dir $@)
	@echo "[CC]  boot/hotplug/$*.c"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/boot/hotplug.mdl: $(HOTPLUG_OBJS) $(LIBC_OBJS) $(LIBDOB_OBJS) \
                               $(call eps_objs,hotplug)
	@mkdir -p $(dir $@)
	@echo "[LD]  hotplug.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(HOTPLUG_OBJS) \
		$(call eps_objs,hotplug) \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOB_OBJS) \
		-o $@

# === audioplayer: main.c + decoders + DSP helpers ===
AP_CFLAGS := $(CFLAGS) -I$(ROOT_DIR)/boot/audioplayer
AP_SRCS   := main.c math_helpers.c synthesis_filter.c \
             decode_wav.c decode_mp2.c decode_mp3.c decode_ogg.c
AP_OBJS   := $(patsubst %.c,$(BUILD_DIR)/boot/audioplayer_%.o,$(AP_SRCS))

$(BUILD_DIR)/boot/audioplayer_%.o: $(ROOT_DIR)/boot/audioplayer/%.c
	@mkdir -p $(dir $@)
	@echo "[CC]  boot/audioplayer/$*.c"
	@$(CC) $(AP_CFLAGS) -c $< -o $@

$(BUILD_DIR)/boot/audioplayer.mdl: $(AP_OBJS) $(LIBC_OBJS) $(LIBDOB_OBJS) \
                                    $(call eps_objs,audioplayer)
	@mkdir -p $(dir $@)
	@echo "[LD]  audioplayer.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(AP_OBJS) \
		$(call eps_objs,audioplayer) \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOB_OBJS) \
		-o $@

# === dobinterface: multi-source (DobInterface 2.0, ~18 fogli) ===
# La riscrittura 2.0 ha smontato il monolite main.c in fogli per
# responsabilita' (video, draw, font, cursor, cmdbuf, window,
# winactions, style, compositor, toast, panel, tray, icons,
# missionctl, screenshot, input, ipc, main), tutti sotto il
# vocabolario comune di_internal.h.  Il wildcard tiene la lista dei
# sorgenti nel filesystem, non nel Makefile: un foglio nuovo entra
# nel link creandolo.
# La pipeline di rendering un tempo era impacchettata come blob .mem
# separato via objcopy; quell'indirezione e' stata ritirata quando la
# pipeline dv_* cmdlist/layer ha preso il posto (il .mem conteneva un
# solo renderer, mai raggiunto, con tutta l'infrastruttura attorno).
# Le primitive ora vivono nei fogli e passano dal driver video via
# int 0x85 (libdob/src/video.c).
# DobInterface_stub.c e' lo STUB CLIENT (lato programmi), compilato dal
# Makefile radice nel pool EPS: escluso dal wildcard del server.
DI_SRCS := $(filter-out DobInterface_stub.c,\
             $(notdir $(wildcard $(ROOT_DIR)/boot/dobinterface/*.c)))
DI_OBJS := $(patsubst %.c,$(BUILD_DIR)/boot/dobinterface_%.o,$(DI_SRCS))

$(BUILD_DIR)/boot/dobinterface_%.o: $(ROOT_DIR)/boot/dobinterface/%.c \
                                    $(ROOT_DIR)/boot/dobinterface/di_internal.h
	@mkdir -p $(dir $@)
	@echo "[CC]  boot/dobinterface/$*.c"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/boot/dobinterface.mdl: $(DI_OBJS) $(LIBC_OBJS) $(LIBDOB_OBJS) \
                                    $(call eps_objs,dobinterface)
	@mkdir -p $(dir $@)
	@echo "[LD]  dobinterface.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(DI_OBJS) \
		$(call eps_objs,dobinterface) \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOB_OBJS) \
		-o $@

clean:
	@rm -rf $(BUILD_DIR)/boot

# === exfat.mem: read-only exFAT parser (.mem), Phase 1 ===
# Two-step compile+link with ld directly (the gcc driver emits ET_EXEC for
# -shared on i686-elf without an ld.so; ld -shared yields the ET_DYN the
# kernel's elf_load_shared requires). Mirrors drivers/cdrom/iso9660.mem.
$(EXFAT_MEM): $(ROOT_DIR)/boot/DobFileSystem/exfat.c \
              $(ROOT_DIR)/boot/DobFileSystem/exfat_api.h
	@mkdir -p $(dir $@)
	@echo "[CC]  boot/DobFileSystem/exfat.c (.mem)"
	@$(CC) $(MEM_CFLAGS) -c $< -o $(BUILD_DIR)/boot/DobFileSystem/exfat.o
	@echo "[LD]  exfat.mem"
	@$(LD) -shared -nostdlib --no-undefined -z noexecstack \
		-Bsymbolic \
		$(BUILD_DIR)/boot/DobFileSystem/exfat.o -o $@
	@etype=$$(od -An -tx1 -j16 -N2 $@ | tr -d ' '); \
	 if [ "$$etype" != "0300" ]; then \
	    echo "[LD]  ERROR: exfat.mem has ELF type $$etype, expected 0300 (ET_DYN)"; \
	    rm -f $@; \
	    exit 1; \
	 fi
	@echo "[LD]  exfat.mem OK (ET_DYN)"
