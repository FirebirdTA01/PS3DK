# sdk/libfiber_stub_extras/Makefile — extra .o objects appended to
# libfiber_stub.a after the FNID stub generation step.
#
# cellFiberPpuInitialize: public wrapper that loads the libfiber SPRX
# via cellSysmoduleLoadModule first, then forwards to the SPRX's
# _cellFiberPpuInitialize.  The nidgen-generated stub only covers the
# underscored SPRX export, so the public name needs a real object.
#
# _gCellFiberPpuThreadLocalStorage: 64-byte BSS area that the SPRX
# references.  The reference archive ships this in
# fiber_ppu_tls_area_for_prx.o; we replicate the same symbol here.
#
# Build invocation: scripts/build-cell-stub-archives.sh calls
# `make -C sdk/libfiber_stub_extras all` and ar-appends the produced
# .o objects into libfiber_stub.a after the nidgen step.

PS3DEV     ?= $(error PS3DEV is not set — run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
PSL1GHT    ?= $(PS3DEV)/psl1ght

CC      := powerpc64-ps3-elf-gcc
AR      := powerpc64-ps3-elf-ar

CFLAGS  := -O2 -Wall -Wextra -mcpu=cell -fno-strict-aliasing \
           -I$(PS3_TOOLCHAIN_ROOT)/sdk/include \
           -I$(PS3DK)/ppu/include -I$(PSL1GHT)/ppu/include

BUILD   := build
OBJS    := $(BUILD)/fiber_init_wrapper.o $(BUILD)/fiber_init_wrapper_asm.o

.PHONY: all clean

all: $(OBJS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	@echo "  CC    $(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%_asm.o: src/%.S | $(BUILD)
	@echo "  AS    $(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	@mkdir -p $@

clean:
	@rm -rf $(BUILD)
