# DobUITools — Client-side UI control library
# Builds textbox.o (and future controls) for linking into programs.

-include $(ROOT_DIR)/toolchain.mk
include $(ROOT_DIR)/config.mk

LIBC_INC   := -I$(ROOT_DIR)/libc/include
LIBDOB_INC := -I$(ROOT_DIR)/libdob/include
EPS_INC    := -I$(BUILD_DIR)/eps
LOCAL_INC  := -I$(ROOT_DIR)/libdobui
INCLUDES   := $(LIBC_INC) $(LIBDOB_INC) $(EPS_INC) $(LOCAL_INC)

CFLAGS := $(USER_CFLAGS) $(OPT_FLAGS) $(INCLUDES) -MMD -MP

SOURCES := textbox.c textbox_clip.c cliptext.c \
           button.c label.c dropdown.c checkbox.c progressbar.c graph.c \
           radiogroup.c slider.c listview.c checked_listview.c table.c grid.c separator.c app.c \
           picturebutton.c switch.c focus.c file_lander.c file_giver.c
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/libdobui/%.o,$(SOURCES))

.PHONY: all clean

all: $(OBJECTS)

$(BUILD_DIR)/libdobui/%.o: $(ROOT_DIR)/libdobui/%.c
	@mkdir -p $(BUILD_DIR)/libdobui
	@echo "[CC]  libdobui/$<"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD_DIR)/libdobui

# Header-dependency tracking: rebuild any .o whose included headers changed

# Force-rebuild objects when ANY libdobui header changes (covers the first
# build after a struct change, before .d files exist).
$(OBJECTS): $(wildcard $(ROOT_DIR)/libdobui/*.h)

-include $(OBJECTS:.o=.d)
