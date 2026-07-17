# MainDOB Tools Makefile — these run on the HOST, compiled with host gcc

HOST_CC := gcc

.PHONY: all clean

all: $(BUILD_DIR)/tools/mkicon

$(BUILD_DIR)/tools/mkicon: $(ROOT_DIR)/tools/mkicon.c
	@mkdir -p $(BUILD_DIR)/tools
	@echo "[HOST] mkicon"
	@$(HOST_CC) -Wall -O2 -o $@ $<

clean:
	@rm -rf $(BUILD_DIR)/tools
