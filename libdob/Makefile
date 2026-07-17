# MainDOB Libdob Makefile

-include $(ROOT_DIR)/toolchain.mk
include $(ROOT_DIR)/config.mk

INCLUDES := -I$(ROOT_DIR)/libdob/include -I$(ROOT_DIR)/libc/include
CFLAGS   := $(USER_CFLAGS) $(OPT_FLAGS) $(INCLUDES) -MMD -MP
SOURCES := ipc.c registry.c server.c window_servo.c font.c thread.c mem.c video.c block.c partition.c fs_ops.c fat32_ops.c exfat_ops.c
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/libdob/%.o,$(SOURCES))

.PHONY: all clean

all: $(OBJECTS)

$(BUILD_DIR)/libdob/%.o: $(ROOT_DIR)/libdob/src/%.c
	@mkdir -p $(BUILD_DIR)/libdob
	@echo "[CC]  libdob/$*.c"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD_DIR)/libdob

# Header-dependency tracking: rebuild any .o whose included headers changed
-include $(OBJECTS:.o=.d)
