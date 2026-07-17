# MainDOB libdobdoc Makefile
#
# Builds the document-model objects. Mirrors libdobfont/Makefile:
# objects land in $(BUILD_DIR)/libdobdoc/ and the top-level build links
# them into programs that use documents (DobWrite, doctest).

-include $(ROOT_DIR)/toolchain.mk
include $(ROOT_DIR)/config.mk

INCLUDES := -I$(ROOT_DIR)/libdobdoc/include \
            -I$(ROOT_DIR)/libc/include \
            -I$(ROOT_DIR)/libdob/include
CFLAGS   := $(USER_CFLAGS) $(OPT_FLAGS) $(INCLUDES) -MMD -MP
SOURCES := piecetable.c runs.c document.c docfile.c
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/libdobdoc/%.o,$(SOURCES))

.PHONY: all clean

all: $(OBJECTS)

$(BUILD_DIR)/libdobdoc/%.o: $(ROOT_DIR)/libdobdoc/src/%.c
	@mkdir -p $(BUILD_DIR)/libdobdoc
	@echo "[CC]  libdobdoc/$*.c"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD_DIR)/libdobdoc

# Header-dependency tracking: rebuild any .o whose included headers changed
-include $(OBJECTS:.o=.d)
