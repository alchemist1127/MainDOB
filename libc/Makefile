# MainDOB Libc Makefile

-include $(ROOT_DIR)/toolchain.mk
include $(ROOT_DIR)/config.mk

INCLUDES := -I$(ROOT_DIR)/libc/include -I$(ROOT_DIR)/libdob/include
CFLAGS   := $(USER_CFLAGS) $(OPT_FLAGS) $(INCLUDES)

OBJECTS := $(BUILD_DIR)/libc/crt0.o $(BUILD_DIR)/libc/string.o \
           $(BUILD_DIR)/libc/stdio.o $(BUILD_DIR)/libc/stdlib.o

.PHONY: all clean

all: $(OBJECTS)

$(BUILD_DIR)/libc/crt0.o: $(ROOT_DIR)/libc/src/crt0.asm
	@mkdir -p $(BUILD_DIR)/libc
	@echo "[ASM] libc/crt0.asm"
	@$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/libc/string.o: $(ROOT_DIR)/libc/src/string.c
	@mkdir -p $(BUILD_DIR)/libc
	@echo "[CC]  libc/string.c"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/libc/stdio.o: $(ROOT_DIR)/libc/src/stdio.c
	@mkdir -p $(BUILD_DIR)/libc
	@echo "[CC]  libc/stdio.c"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/libc/stdlib.o: $(ROOT_DIR)/libc/src/stdlib.c
	@mkdir -p $(BUILD_DIR)/libc
	@echo "[CC]  libc/stdlib.c"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD_DIR)/libc
