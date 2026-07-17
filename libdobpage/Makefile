# MainDOB libdobpage Makefile -- the page-object engine.
# Depends on libdoblayout (the content column) and libdobfont (rasterization).

-include $(ROOT_DIR)/toolchain.mk
include $(ROOT_DIR)/config.mk

INCLUDES := -I$(ROOT_DIR)/libdobpage/include \
            -I$(ROOT_DIR)/libdoblayout/include \
            -I$(ROOT_DIR)/libdobdoc/include \
            -I$(ROOT_DIR)/libdobfont/include \
            -I$(ROOT_DIR)/libc/include \
            -I$(ROOT_DIR)/libdob/include
CFLAGS   := $(USER_CFLAGS) $(OPT_FLAGS) $(INCLUDES) -MMD -MP
SOURCES := page.c
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/libdobpage/%.o,$(SOURCES))

.PHONY: all clean
all: $(OBJECTS)

$(BUILD_DIR)/libdobpage/%.o: $(ROOT_DIR)/libdobpage/src/%.c
	@mkdir -p $(BUILD_DIR)/libdobpage
	@echo "[CC]  libdobpage/$*.c"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD_DIR)/libdobpage

# Header-dependency tracking: rebuild any .o whose included headers changed
-include $(OBJECTS:.o=.d)
