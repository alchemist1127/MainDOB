# MainDOB Programs Makefile
#
# Each program lives in programs/<n>/main.c.  The build is driven by
# a single pattern rule plus a few per-program variables:
#
#   EPS_<n>    := list of EPS stub names this program needs
#                    (each entry expands to $(BUILD_DIR)/eps/<n>_stub.o)
#   NO_DOBUI      := programs that don't link against $(DOBUI_OBJS)
#
# Anything truly special (extra source files, extra library object
# sets) is handled with explicit rules at the bottom of the file.

-include $(ROOT_DIR)/toolchain.mk
include $(ROOT_DIR)/config.mk

LIBC_INC   := -I$(ROOT_DIR)/libc/include
LIBDOB_INC := -I$(ROOT_DIR)/libdob/include
EPS_INC    := -I$(BUILD_DIR)/eps
DOBUI_INC  := -I$(ROOT_DIR)/libdobui
# MACH64_INC: for enginediag, which includes the shared mach64_diag.h
MACH64_INC := -I$(ROOT_DIR)/drivers/mach64
# DobWrite stack headers (font engine, document model, layout engine)
DOBWRITE_INC := -I$(ROOT_DIR)/libdobfont/include -I$(ROOT_DIR)/libdobdoc/include \
                -I$(ROOT_DIR)/libdoblayout/include -I$(ROOT_DIR)/libdobpage/include
INCLUDES   := $(LIBC_INC) $(LIBDOB_INC) $(EPS_INC) $(DOBUI_INC) $(MACH64_INC) $(DOBWRITE_INC)

CFLAGS := $(USER_CFLAGS) $(OPT_FLAGS) $(INCLUDES)

PROGRAMS := DobFiles editor snake tetris minesweeper uidemo graphdemo griddemo modules \
            benchmark calculator solitaire spider arrowsweeper DobPicture widget_test \
            tonegen dobplayer DobArchive DobInstaller DobTable valueParser \
            videotest boomtest keymap DobSettings demosettings DobDisk \
            MainDOB_Setup enginediag clock atadiag usbdiag exfat_attach phase2_init \
            fonttest fontd doctest laytest pagetest DobWrite panictest taskmgr

PROGRAM_BINS := $(patsubst %,$(BUILD_DIR)/programs/%.mdl,$(PROGRAMS))

LIBC_OBJS   := $(BUILD_DIR)/libc/crt0.o $(BUILD_DIR)/libc/string.o \
               $(BUILD_DIR)/libc/stdio.o $(BUILD_DIR)/libc/stdlib.o
LIBDOB_OBJS := $(BUILD_DIR)/libdob/ipc.o $(BUILD_DIR)/libdob/registry.o \
               $(BUILD_DIR)/libdob/server.o $(BUILD_DIR)/libdob/font.o \
               $(BUILD_DIR)/libdob/window_servo.o $(BUILD_DIR)/libdob/thread.o \
               $(BUILD_DIR)/libdob/mem.o
DOBUI_OBJS  := $(BUILD_DIR)/libdobui/textbox.o $(BUILD_DIR)/libdobui/button.o \
               $(BUILD_DIR)/libdobui/label.o $(BUILD_DIR)/libdobui/dropdown.o \
               $(BUILD_DIR)/libdobui/checkbox.o $(BUILD_DIR)/libdobui/progressbar.o \
               $(BUILD_DIR)/libdobui/graph.o \
               $(BUILD_DIR)/libdobui/radiogroup.o $(BUILD_DIR)/libdobui/slider.o \
               $(BUILD_DIR)/libdobui/listview.o $(BUILD_DIR)/libdobui/table.o \
               $(BUILD_DIR)/libdobui/grid.o \
               $(BUILD_DIR)/libdobui/checked_listview.o \
               $(BUILD_DIR)/libdobui/separator.o \
               $(BUILD_DIR)/libdobui/app.o \
               $(BUILD_DIR)/libdobui/picturebutton.o $(BUILD_DIR)/libdobui/switch.o \
               $(BUILD_DIR)/libdobui/focus.o \
               $(BUILD_DIR)/libdobui/textbox_clip.o $(BUILD_DIR)/libdobui/cliptext.o

# DobWrite stack object sets (built by the libdobfont/libdobdoc/libdoblayout
# directories; mirrored here the way DobDisk mirrors its extra libdob objects).
LIBDOBFONT_OBJS := $(BUILD_DIR)/libdobfont/sfnt.o $(BUILD_DIR)/libdobfont/outline.o \
                   $(BUILD_DIR)/libdobfont/cff.o $(BUILD_DIR)/libdobfont/raster.o \
                   $(BUILD_DIR)/libdobfont/backend_sfnt.o $(BUILD_DIR)/libdobfont/backend_mono.o \
                   $(BUILD_DIR)/libdobfont/dobfont.o \
                   $(BUILD_DIR)/libdobfont/resolver.o
LIBDOBDOC_OBJS  := $(BUILD_DIR)/libdobdoc/piecetable.o $(BUILD_DIR)/libdobdoc/runs.o \
                   $(BUILD_DIR)/libdobdoc/document.o $(BUILD_DIR)/libdobdoc/docfile.o
LIBDOBLAYOUT_OBJS := $(BUILD_DIR)/libdoblayout/layout.o
LIBDOBPAGE_OBJS  := $(BUILD_DIR)/libdobpage/page.o
# Any program that links DOBUI_OBJS transitively pulls cliptext.o,
# which in turn depends on the DobConfig EPS stub. Every program in
# PROGRAMS (outside NO_DOBUI) therefore lists "DobConfig" in its
# EPS_<name> below. The clipboard SHM is created lazily on first
# use, so programs that never touch Copy/Paste pay only the ~9 KB
# of add-on code.

# === Per-program EPS stub dependencies ===
EPS_DobFiles     := DobInterface DobFileSystem DobPopup DobConfig DobTable DobSettings
EPS_editor       := DobInterface DobFileSystem DobPopup DobFiles DobConfig
EPS_snake        := DobInterface DobConfig
EPS_tetris       := DobInterface DobConfig
EPS_minesweeper  := DobInterface DobConfig
EPS_uidemo       := DobInterface DobConfig
EPS_graphdemo    := DobInterface DobConfig
EPS_griddemo     := DobInterface DobConfig
EPS_modules      := DobInterface DobFileSystem DobPopup DobConfig
EPS_benchmark    := DobInterface DobConfig
# taskmgr: finestra+widget (DobInterface), conferma kill (DobPopup),
# DobConfig per cliptext (dipendenza transitiva di DOBUI_OBJS).
EPS_taskmgr      := DobInterface DobPopup DobConfig
EPS_calculator   := DobInterface DobFileSystem DobConfig
EPS_solitaire    := DobInterface DobConfig
EPS_spider       := DobInterface DobConfig
EPS_arrowsweeper := DobInterface DobConfig
EPS_DobPicture   := DobInterface DobFileSystem DobPopup DobFiles DobConfig
# DobPicture carica imgcodec.mem a runtime (decoder+encoder PNG/JPEG).
EPS_widget_test  := DobInterface DobPopupToast
EPS_tonegen      := DobInterface DobConfig
EPS_dobplayer    := DobAudioPlayer DobConfig DobInterface
EPS_DobArchive   := DobFiles DobFileSystem DobPopupToast DobConfig
EPS_DobInstaller := DobInterface DobFileSystem DobPopup DobConfig DobArchive
EPS_DobTable     := DobInterface DobConfig
EPS_valueParser  := DobTable DobFileSystem DobPopupToast
EPS_videotest    := DobTable DobFileSystem DobPopupToast DobVideoControl
EPS_enginediag   := DobTable DobFileSystem DobPopupToast DobVideoControl
EPS_atadiag      := DobTable DobFileSystem DobPopupToast
EPS_usbdiag      := DobTable DobFileSystem DobPopupToast
EPS_boomtest     := DobTable DobFileSystem DobPopupToast
EPS_keymap       := DobInterface DobSettings DobFileSystem
EPS_DobSettings  := DobInterface DobSettings DobConfig
EPS_demosettings := DobInterface DobSettings DobConfig
EPS_DobDisk      := DobInterface DobFileSystem DobPopup DobConfig DobTable
EPS_MainDOB_Setup := DobInterface DobFileSystem DobPopup DobConfig
# clock: tray clock + calendar + agenda. DobInterface (widget/window),
# DobPopup (add/edit event dialogs + alarm popup), DobFileSystem
# (persist events under the program's own sandboxed folder), DobSettings
# (timezone offset + DST, declared to settingsd and read back). DobConfig
# is pulled in transitively by libdobui (cliptext).
EPS_clock        := DobInterface DobPopup DobFileSystem DobSettings DobConfig
EPS_exfat_attach := DobFileSystem
EPS_phase2_init  := DobFileSystem
EPS_fonttest     := DobFileSystem
EPS_fontd        := DobFileSystem
EPS_doctest      := DobFileSystem
EPS_laytest      := DobFileSystem
EPS_pagetest     := DobFileSystem
EPS_DobWrite     := DobInterface DobFileSystem DobPopup DobFiles DobConfig

# Programs that don't need the DobUITools control library
NO_DOBUI := widget_test DobArchive valueParser videotest boomtest enginediag atadiag usbdiag exfat_attach phase2_init fonttest fontd doctest laytest pagetest panictest

# === Helpers ===
eps_objs   = $(foreach e,$(EPS_$(1)),$(BUILD_DIR)/eps/$(e)_stub.o)
dobui_objs = $(if $(filter $(1),$(NO_DOBUI)),,$(DOBUI_OBJS))

.PHONY: all clean

all: $(PROGRAM_BINS)

# === Generic build rule ===
# main.c -> main.o -> link with crt0 + libc + libdob + (dobui) + EPS stubs.
# Per-program prerequisites for the EPS stubs are attached below via
# $(eval) so the linker actually waits for them to be built.
$(BUILD_DIR)/programs/%.mdl: $(ROOT_DIR)/programs/%/main.c $(LIBC_OBJS) $(LIBDOB_OBJS)
	@mkdir -p $(BUILD_DIR)/programs
	@echo "[CC]  programs/$*/main.c"
	@$(CC) $(CFLAGS) -I$(ROOT_DIR)/programs/$* -c $< -o $(BUILD_DIR)/programs/$*_main.o
	@echo "[LD]  $*.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(BUILD_DIR)/programs/$*_main.o \
		$(call eps_objs,$*) \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(call dobui_objs,$*) \
		$(LIBDOB_OBJS) \
		-o $@

# Wire per-program EPS stub and DOBUI prerequisites
$(foreach p,$(PROGRAMS),$(eval \
    $(BUILD_DIR)/programs/$(p).mdl: $(call eps_objs,$(p)) \
                                    $(call dobui_objs,$(p))))

# === DobDisk: links block/partition/fs_ops/fat32_ops from libdob ===
# These are heavy disk-utility objects that the regular LIBDOB_OBJS
# set leaves out — DobDisk is one of the few clients that needs them
# (DobLiveSetup and the eventual format-from-installer will share the
# same set, mirrored explicitly per-program). Mirrored pattern of
# boot/Makefile:DobFileSystem.mdl.
DOBDISK_EXTRA := $(BUILD_DIR)/libdob/block.o \
                 $(BUILD_DIR)/libdob/partition.o \
                 $(BUILD_DIR)/libdob/fs_ops.o \
                 $(BUILD_DIR)/libdob/fat32_ops.o \
                 $(BUILD_DIR)/libdob/exfat_ops.o

$(BUILD_DIR)/programs/DobDisk.mdl: $(ROOT_DIR)/programs/DobDisk/main.c \
                                   $(LIBC_OBJS) $(LIBDOB_OBJS) $(DOBDISK_EXTRA) \
                                   $(call eps_objs,DobDisk) \
                                   $(call dobui_objs,DobDisk)
	@mkdir -p $(dir $@)
	@echo "[CC]  programs/DobDisk/main.c"
	@$(CC) $(CFLAGS) -c $< -o $(BUILD_DIR)/programs/DobDisk_main.o
	@echo "[LD]  DobDisk.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(BUILD_DIR)/programs/DobDisk_main.o \
		$(call eps_objs,DobDisk) \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(call dobui_objs,DobDisk) \
		$(LIBDOB_OBJS) \
		$(DOBDISK_EXTRA) \
		-o $@

# === MainDOB_Setup: same disk-utility object set as DobDisk ===
# The installer enumerates disks, reads partition tables, and writes
# the target FAT32 volume + GRUB blobs. It pulls in block/partition/
# fs_ops/fat32_ops by exactly the same mechanism DobDisk uses, which
# is why the foreseeing comment on DOBDISK_EXTRA mentions it.
$(BUILD_DIR)/programs/MainDOB_Setup.mdl: $(ROOT_DIR)/programs/MainDOB_Setup/main.c \
                                          $(LIBC_OBJS) $(LIBDOB_OBJS) $(DOBDISK_EXTRA) \
                                          $(call eps_objs,MainDOB_Setup) \
                                          $(call dobui_objs,MainDOB_Setup)
	@mkdir -p $(dir $@)
	@echo "[CC]  programs/MainDOB_Setup/main.c"
	@$(CC) $(CFLAGS) -c $< -o $(BUILD_DIR)/programs/MainDOB_Setup_main.o
	@echo "[LD]  MainDOB_Setup.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(BUILD_DIR)/programs/MainDOB_Setup_main.o \
		$(call eps_objs,MainDOB_Setup) \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(call dobui_objs,MainDOB_Setup) \
		$(LIBDOB_OBJS) \
		$(DOBDISK_EXTRA) \
		-o $@

# === calculator: math_operations linked directly into the binary ===
# (Formerly a companion ModuleLink; the integer math routines are now
# compiled straight into calculator.mdl.)
$(BUILD_DIR)/programs/calculator.mdl: $(ROOT_DIR)/programs/calculator/main.c \
                                      $(ROOT_DIR)/programs/calculator/math_operations.c \
                                      $(LIBC_OBJS) $(LIBDOB_OBJS) \
                                      $(call eps_objs,calculator) \
                                      $(call dobui_objs,calculator)
	@mkdir -p $(dir $@)
	@echo "[CC]  programs/calculator/main.c"
	@$(CC) $(CFLAGS) -I$(ROOT_DIR)/programs/calculator -c $< -o $(BUILD_DIR)/programs/calculator_main.o
	@echo "[CC]  programs/calculator/math_operations.c"
	@$(CC) $(CFLAGS) -I$(ROOT_DIR)/programs/calculator -c $(ROOT_DIR)/programs/calculator/math_operations.c -o $(BUILD_DIR)/programs/calculator_math_operations.o
	@echo "[LD]  calculator.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(BUILD_DIR)/programs/calculator_main.o \
		$(BUILD_DIR)/programs/calculator_math_operations.o \
		$(call eps_objs,calculator) \
		$(BUILD_DIR)/libc/string.o \
		$(BUILD_DIR)/libc/stdio.o \
		$(BUILD_DIR)/libc/stdlib.o \
		$(call dobui_objs,calculator) \
		$(LIBDOB_OBJS) \
		-o $@

# === .mem shared objects ===
# A .mem is a PIC ELF shared object (see libdob/include/dob/mem.h).
# Each program that wants a companion .mem declares an explicit source
# dependency like:
#
#   $(BUILD_DIR)/programs/compression.mem: $(ROOT_DIR)/programs/foo/compression.c
#
# and then lists the .mem as a prerequisite of its .mdl target so the
# build orders them correctly.  A .mem is self-contained: no crt0, no
# libc link-in.  If you need helpers, define them inside the .mem or
# use inline static functions from headers.

MEM_CFLAGS := $(USER_CFLAGS) $(OPT_FLAGS) -fPIC \
              -mno-sse -mno-mmx -mno-80387 -fno-stack-protector \
              -I$(ROOT_DIR)/libc/include -I$(ROOT_DIR)/libdob/include

$(BUILD_DIR)/programs/%.mem:
	@mkdir -p $(BUILD_DIR)/programs
	@echo "[CC]  $< (.mem)"
	@$(CC) $(MEM_CFLAGS) -shared -nostdlib \
		-Wl,--no-undefined \
		$< -o $@

# === DobWrite tools: link the extra library object sets ===
# fonttest/fontd -> libdobfont; doctest -> libdobdoc; laytest -> all three.
# (.o files link unconditionally, so object order doesn't affect resolution.)
$(BUILD_DIR)/programs/fonttest.mdl: $(ROOT_DIR)/programs/fonttest/main.c \
                                    $(LIBC_OBJS) $(LIBDOB_OBJS) $(LIBDOBFONT_OBJS) \
                                    $(call eps_objs,fonttest)
	@mkdir -p $(dir $@)
	@echo "[CC]  programs/fonttest/main.c"
	@$(CC) $(CFLAGS) -I$(ROOT_DIR)/programs/fonttest -c $< -o $(BUILD_DIR)/programs/fonttest_main.o
	@echo "[LD]  fonttest.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o $(BUILD_DIR)/programs/fonttest_main.o \
		$(call eps_objs,fonttest) \
		$(BUILD_DIR)/libc/string.o $(BUILD_DIR)/libc/stdio.o $(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOBFONT_OBJS) $(LIBDOB_OBJS) -o $@

$(BUILD_DIR)/programs/fontd.mdl: $(ROOT_DIR)/programs/fontd/main.c \
                                 $(LIBC_OBJS) $(LIBDOB_OBJS) $(LIBDOBFONT_OBJS) \
                                 $(call eps_objs,fontd)
	@mkdir -p $(dir $@)
	@echo "[CC]  programs/fontd/main.c"
	@$(CC) $(CFLAGS) -I$(ROOT_DIR)/programs/fontd -c $< -o $(BUILD_DIR)/programs/fontd_main.o
	@echo "[LD]  fontd.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o $(BUILD_DIR)/programs/fontd_main.o \
		$(call eps_objs,fontd) \
		$(BUILD_DIR)/libc/string.o $(BUILD_DIR)/libc/stdio.o $(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOBFONT_OBJS) $(LIBDOB_OBJS) -o $@

$(BUILD_DIR)/programs/doctest.mdl: $(ROOT_DIR)/programs/doctest/main.c \
                                   $(LIBC_OBJS) $(LIBDOB_OBJS) $(LIBDOBDOC_OBJS) \
                                   $(call eps_objs,doctest)
	@mkdir -p $(dir $@)
	@echo "[CC]  programs/doctest/main.c"
	@$(CC) $(CFLAGS) -I$(ROOT_DIR)/programs/doctest -c $< -o $(BUILD_DIR)/programs/doctest_main.o
	@echo "[LD]  doctest.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o $(BUILD_DIR)/programs/doctest_main.o \
		$(call eps_objs,doctest) \
		$(BUILD_DIR)/libc/string.o $(BUILD_DIR)/libc/stdio.o $(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOBDOC_OBJS) $(LIBDOB_OBJS) -o $@

$(BUILD_DIR)/programs/laytest.mdl: $(ROOT_DIR)/programs/laytest/main.c \
                                   $(LIBC_OBJS) $(LIBDOB_OBJS) \
                                   $(LIBDOBLAYOUT_OBJS) $(LIBDOBDOC_OBJS) $(LIBDOBFONT_OBJS) \
                                   $(call eps_objs,laytest)
	@mkdir -p $(dir $@)
	@echo "[CC]  programs/laytest/main.c"
	@$(CC) $(CFLAGS) -I$(ROOT_DIR)/programs/laytest -c $< -o $(BUILD_DIR)/programs/laytest_main.o
	@echo "[LD]  laytest.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o $(BUILD_DIR)/programs/laytest_main.o \
		$(call eps_objs,laytest) \
		$(BUILD_DIR)/libc/string.o $(BUILD_DIR)/libc/stdio.o $(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOBLAYOUT_OBJS) $(LIBDOBDOC_OBJS) $(LIBDOBFONT_OBJS) $(LIBDOB_OBJS) -o $@

$(BUILD_DIR)/programs/pagetest.mdl: $(ROOT_DIR)/programs/pagetest/main.c \
                                    $(LIBC_OBJS) $(LIBDOB_OBJS) \
                                    $(LIBDOBPAGE_OBJS) $(LIBDOBLAYOUT_OBJS) \
                                    $(LIBDOBDOC_OBJS) $(LIBDOBFONT_OBJS) \
                                    $(call eps_objs,pagetest)
	@mkdir -p $(dir $@)
	@echo "[CC]  programs/pagetest/main.c"
	@$(CC) $(CFLAGS) -I$(ROOT_DIR)/programs/pagetest -c $< -o $(BUILD_DIR)/programs/pagetest_main.o
	@echo "[LD]  pagetest.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o $(BUILD_DIR)/programs/pagetest_main.o \
		$(call eps_objs,pagetest) \
		$(BUILD_DIR)/libc/string.o $(BUILD_DIR)/libc/stdio.o $(BUILD_DIR)/libc/stdlib.o \
		$(LIBDOBPAGE_OBJS) $(LIBDOBLAYOUT_OBJS) $(LIBDOBDOC_OBJS) $(LIBDOBFONT_OBJS) $(LIBDOB_OBJS) -o $@

$(BUILD_DIR)/programs/DobWrite.mdl: $(ROOT_DIR)/programs/DobWrite/main.c \
                                    $(ROOT_DIR)/programs/DobWrite/colorpick.c \
                                    $(ROOT_DIR)/programs/DobWrite/autocorr.c \
                                    $(ROOT_DIR)/programs/DobWrite/subsedit.c \
                                    $(ROOT_DIR)/programs/DobWrite/findrepl.c \
                                    $(ROOT_DIR)/programs/DobWrite/pagesetup.c \
                                    $(ROOT_DIR)/programs/DobWrite/fontreg.c \
                                    $(LIBC_OBJS) $(LIBDOB_OBJS) \
                                    $(LIBDOBPAGE_OBJS) $(LIBDOBLAYOUT_OBJS) \
                                    $(LIBDOBDOC_OBJS) $(LIBDOBFONT_OBJS) \
                                    $(call eps_objs,DobWrite) \
                                    $(call dobui_objs,DobWrite)
	@mkdir -p $(dir $@)
	@echo "[CC]  programs/DobWrite/main.c"
	@$(CC) $(CFLAGS) -I$(ROOT_DIR)/programs/DobWrite -c $< -o $(BUILD_DIR)/programs/DobWrite_main.o
	@echo "[CC]  programs/DobWrite/colorpick.c"
	@$(CC) $(CFLAGS) -I$(ROOT_DIR)/programs/DobWrite -c $(ROOT_DIR)/programs/DobWrite/colorpick.c -o $(BUILD_DIR)/programs/DobWrite_colorpick.o
	@echo "[CC]  programs/DobWrite/autocorr.c"
	@$(CC) $(CFLAGS) -I$(ROOT_DIR)/programs/DobWrite -c $(ROOT_DIR)/programs/DobWrite/autocorr.c -o $(BUILD_DIR)/programs/DobWrite_autocorr.o
	@echo "[CC]  programs/DobWrite/subsedit.c"
	@$(CC) $(CFLAGS) -I$(ROOT_DIR)/programs/DobWrite -c $(ROOT_DIR)/programs/DobWrite/subsedit.c -o $(BUILD_DIR)/programs/DobWrite_subsedit.o
	@echo "[CC]  programs/DobWrite/findrepl.c"
	@$(CC) $(CFLAGS) -I$(ROOT_DIR)/programs/DobWrite -c $(ROOT_DIR)/programs/DobWrite/findrepl.c -o $(BUILD_DIR)/programs/DobWrite_findrepl.o
	@echo "[CC]  programs/DobWrite/pagesetup.c"
	@$(CC) $(CFLAGS) -I$(ROOT_DIR)/programs/DobWrite -c $(ROOT_DIR)/programs/DobWrite/pagesetup.c -o $(BUILD_DIR)/programs/DobWrite_pagesetup.o
	@echo "[CC]  programs/DobWrite/fontreg.c"
	@$(CC) $(CFLAGS) -I$(ROOT_DIR)/programs/DobWrite -c $(ROOT_DIR)/programs/DobWrite/fontreg.c -o $(BUILD_DIR)/programs/DobWrite_fontreg.o
	@echo "[LD]  DobWrite.mdl"
	@$(LD) $(LDFLAGS_USER) -T $(ROOT_DIR)/libc/user.ld \
		$(BUILD_DIR)/libc/crt0.o \
		$(BUILD_DIR)/programs/DobWrite_main.o $(BUILD_DIR)/programs/DobWrite_colorpick.o $(BUILD_DIR)/programs/DobWrite_autocorr.o $(BUILD_DIR)/programs/DobWrite_subsedit.o $(BUILD_DIR)/programs/DobWrite_findrepl.o $(BUILD_DIR)/programs/DobWrite_pagesetup.o $(BUILD_DIR)/programs/DobWrite_fontreg.o \
		$(call eps_objs,DobWrite) \
		$(BUILD_DIR)/libc/string.o $(BUILD_DIR)/libc/stdio.o $(BUILD_DIR)/libc/stdlib.o \
		$(call dobui_objs,DobWrite) \
		$(LIBDOBPAGE_OBJS) $(LIBDOBLAYOUT_OBJS) $(LIBDOBDOC_OBJS) $(LIBDOBFONT_OBJS) $(LIBDOB_OBJS) -o $@

clean:
	@rm -rf $(BUILD_DIR)/programs $(BUILD_DIR)/eps

# === imgcodec.mem: decoder+encoder PNG/JPEG come .mem (vedi dob/mem.h) ===
# Stesso schema di exfat.mem: PIC, self-contained, ld -shared -> ET_DYN.
# NOTA: $(INCLUDES) e $(OPT_FLAGS) vanno aggiunti esplicitamente —
# imgcodec_api.h include <dob/types.h> (libdob), e USER_CFLAGS da solo
# non porta alcun -I (quelli vivono in INCLUDES, come per CFLAGS sopra).
PROG_MEM_CFLAGS := $(filter-out -fno-pic -fno-pie, $(USER_CFLAGS)) \
                   $(OPT_FLAGS) $(INCLUDES) \
                   -fPIC -fvisibility=hidden

IMGCODEC_MEM := $(BUILD_DIR)/programs/imgcodec.mem

$(IMGCODEC_MEM): $(ROOT_DIR)/programs/DobPicture/imgcodec.c \
                 $(ROOT_DIR)/programs/DobPicture/imgcodec_api.h
	@mkdir -p $(dir $@)
	@echo "[CC]  programs/DobPicture/imgcodec.c (.mem)"
	@$(CC) $(PROG_MEM_CFLAGS) -I$(ROOT_DIR)/programs/DobPicture -c $< -o $(BUILD_DIR)/programs/imgcodec_mem.o
	@echo "[LD]  imgcodec.mem"
	@$(LD) -shared -nostdlib --no-undefined -z noexecstack -Bsymbolic \
		$(BUILD_DIR)/programs/imgcodec_mem.o -o $@
	@etype=$$(od -An -tx1 -j16 -N2 $@ | tr -d ' '); \
	 if [ "$$etype" != "0300" ]; then \
	    echo "[LD]  ERROR: imgcodec.mem ELF type $$etype != ET_DYN"; \
	    rm -f $@; exit 1; \
	 fi

all: $(IMGCODEC_MEM)
