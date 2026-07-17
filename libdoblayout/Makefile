# MainDOB libdoblayout Makefile
#
# Builds the layout engine. Depends on libdobdoc (model) and libdobfont
# (faces/metrics). Objects land in $(BUILD_DIR)/libdoblayout/.

-include $(ROOT_DIR)/toolchain.mk
include $(ROOT_DIR)/config.mk

INCLUDES := -I$(ROOT_DIR)/libdoblayout/include \
            -I$(ROOT_DIR)/libdobdoc/include \
            -I$(ROOT_DIR)/libdobfont/include \
            -I$(ROOT_DIR)/libc/include \
            -I$(ROOT_DIR)/libdob/include
CFLAGS   := $(USER_CFLAGS) $(OPT_FLAGS) $(INCLUDES) -MMD -MP
SOURCES := layout.c
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/libdoblayout/%.o,$(SOURCES))

.PHONY: all clean

all: $(OBJECTS)

$(BUILD_DIR)/libdoblayout/%.o: $(ROOT_DIR)/libdoblayout/src/%.c
	@mkdir -p $(BUILD_DIR)/libdoblayout
	@echo "[CC]  libdoblayout/$*.c"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD_DIR)/libdoblayout

# Header-dependency tracking: rebuild any .o whose included headers changed
-include $(OBJECTS:.o=.d)
