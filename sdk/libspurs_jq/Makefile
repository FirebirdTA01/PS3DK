# sdk/libspurs_jq/Makefile - SPU side of the cell/spurs/job_queue family.
#
# Outputs:
#   libspurs_jq.a   -> $(PS3DK)/spu/lib/
#
# Built with spu-elf-gcc.  SPU job binaries that participate in a JQ
# workload link against this archive (via the JOBBIN flag's LIBS hook
# in ps3_add_spu_image).
#
# Current state: skeleton with stubs returning CELL_OK / INVAL for
# every entry point declared in cell/spurs/job_queue*.h.  Real bodies
# are filled in incrementally - see docs/abi/spurs-job-entry-point.md
# (job-queue runtime layout to be folded into a successor doc).

PS3DEV     ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
SPU_PREFIX ?= $(PS3DEV)/spu

INSTALL_LIB := $(PS3DK)/spu/lib

CC := spu-elf-gcc
AR := spu-elf-ar

CFLAGS := -O2 -Wall -Wextra -fno-exceptions -ffreestanding -fpic \
          -falign-functions=8 -mno-branch-hints \
          -ffunction-sections -fdata-sections \
          -I$(PS3DK)/spu/include
SFLAGS := -fpic

BUILD := build
OBJS  := $(BUILD)/stubs.o $(BUILD)/job_start_w_crt.o $(BUILD)/job_crt.o
LIB   := $(BUILD)/libspurs_jq.a

.PHONY: all install clean

all: $(LIB)

$(LIB): $(OBJS)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(OBJS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	@echo "  CC    $(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.S | $(BUILD)
	@echo "  AS    $(notdir $<)"
	@$(CC) $(SFLAGS) -c $< -o $@

$(BUILD):
	@mkdir -p $@

install: $(LIB)
	@install -d $(INSTALL_LIB)
	@install -m 0644 $(LIB) $(INSTALL_LIB)/libspurs_jq.a
	@echo "  lib    libspurs_jq.a"

clean:
	@rm -rf $(BUILD)
