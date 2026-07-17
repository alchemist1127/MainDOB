# MainDOB libdobfont Makefile
#
# Builds the scalable font engine objects. Mirrors libdob/Makefile:
# objects land in $(BUILD_DIR)/libdobfont/ and the top-level build
# links them into programs that use fonts (DobWrite, fonttest).

-include $(ROOT_DIR)/toolchain.mk
include $(ROOT_DIR)/config.mk

INCLUDES := -I$(ROOT_DIR)/libdobfont/include \
            -I$(ROOT_DIR)/libc/include \
            -I$(ROOT_DIR)/libdob/include
CFLAGS   := $(USER_CFLAGS) $(OPT_FLAGS) $(INCLUDES) -MMD -MP
SOURCES := sfnt.c outline.c raster.c backend_sfnt.c backend_mono.c cff.c dobfont.c resolver.c
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/libdobfont/%.o,$(SOURCES))

.PHONY: all clean

all: $(OBJECTS)

$(BUILD_DIR)/libdobfont/%.o: $(ROOT_DIR)/libdobfont/src/%.c
	@mkdir -p $(BUILD_DIR)/libdobfont
	@echo "[CC]  libdobfont/$*.c"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD_DIR)/libdobfont

# Header-dependency tracking: rebuild any .o whose included headers changed
-include $(OBJECTS:.o=.d)
