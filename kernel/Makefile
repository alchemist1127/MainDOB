# MainDOB Kernel Makefile (1.1)
#
# Auto-discovery dei sorgenti: ogni .c e .asm sotto kernel/ entra nella
# build senza doverlo elencare (l'albero 1.1 e' cresciuto per moduli, e
# un elenco manuale e' l'unico modo di dimenticarne uno). Flag, layout
# degli oggetti e linker restano quelli del sistema (toolchain.mk /
# config.mk), cosi' l'innesto e' trasparente al resto dell'archivio.

-include $(ROOT_DIR)/toolchain.mk
include $(ROOT_DIR)/config.mk

KERNEL_SRC := $(ROOT_DIR)/kernel
INCLUDES   := -I$(KERNEL_SRC)

CFLAGS := $(KERNEL_CFLAGS) $(OPT_FLAGS) $(KERNEL_SMP_FLAGS) $(KERNEL_IOAPIC_FLAGS) $(KERNEL_SMOKE_FLAGS) $(KERNEL_RAM_FLAGS) $(INCLUDES)

# NASM riceve gli stessi -D del compilatore per le sezioni condizionali
# (es. -DMAINDOB_SMP che seleziona GS = GDT_SEL_PERCPU negli stub ISR).
ASFLAGS_KERNEL := $(ASFLAGS) $(KERNEL_SMP_FLAGS)

# Sorgenti: relativi a kernel/, scoperti a ricorsione.
ASM_SOURCES := $(patsubst $(KERNEL_SRC)/%,%,$(shell find $(KERNEL_SRC) -name '*.asm'))
C_SOURCES   := $(patsubst $(KERNEL_SRC)/%,%,$(shell find $(KERNEL_SRC) -name '*.c'))

ASM_OBJECTS := $(patsubst %.asm,$(BUILD_DIR)/kernel/%.o,$(ASM_SOURCES))
C_OBJECTS   := $(patsubst %.c,$(BUILD_DIR)/kernel/%.o,$(C_SOURCES))
OBJECTS     := $(ASM_OBJECTS) $(C_OBJECTS)

LINKER_SCRIPT := $(KERNEL_SRC)/linker.ld

.PHONY: all clean

all: $(KERNEL_BIN)

$(KERNEL_BIN): $(OBJECTS) $(LINKER_SCRIPT)
	@echo "[LD]  kernel.bin"
	@$(LD) $(LDFLAGS_KERNEL) -Map=$(KERNEL_BIN).map -T $(LINKER_SCRIPT) -o $@ $(OBJECTS)

$(BUILD_DIR)/kernel/%.o: $(KERNEL_SRC)/%.c
	@mkdir -p $(dir $@)
	@echo "[CC]  kernel/$*.c"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/kernel/%.o: $(KERNEL_SRC)/%.asm
	@mkdir -p $(dir $@)
	@echo "[AS]  kernel/$*.asm"
	@$(AS) $(ASFLAGS_KERNEL) $< -o $@

clean:
	@rm -rf $(BUILD_DIR)/kernel

# Dipendenze dagli header: senza i
# file .d un .c non toccato NON si ricompila quando cambia un header
# che inline-a --- due TU dello stesso kernel possono cosi' disaccordare
# su una static inline (e' successo con clock.h). -MMD -MP genera le
# dipendenze come effetto collaterale della compilazione; -include le
# consuma se esistono, in silenzio alla prima build.
-include $(shell find $(BUILD_DIR)/kernel -name '*.d' 2>/dev/null)
