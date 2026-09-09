# sdk/libc_stub_extras/Makefile — extra .o objects appended to libc_stub.a
# after the FNID stub generation step.
#
# Functions in this directory are user-facing names that already
# exist as exports in libc.sprx, but the SPRX-side implementation
# (or RPCS3's HLE for it) is broken in some way.  The cleanest fix is
# to ship a working real-PPU implementation under the same name and
# bake it into libc_stub.a so callers get the working version rather
# than the broken FNID-stub-to-SPRX path.  See the per-file comment
# at the top of each source for the specific reason.
#
# Build invocation: scripts/build-cell-stub-archives.sh calls
# `make -C sdk/libc_stub_extras all` and ar-appends the produced
# .o objects into libc_stub.a after the nidgen step.

PS3DEV     ?= $(error PS3DEV is not set — run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
PSL1GHT    ?= $(PS3DEV)/psl1ght

CC      := powerpc64-ps3-elf-gcc
AR      := powerpc64-ps3-elf-ar

CFLAGS  := -O2 -Wall -Wextra -mcpu=cell -fno-strict-aliasing \
           -I$(PS3DK)/ppu/include -I$(PSL1GHT)/ppu/include

# ABI flags (-mlp64 for the LP64 multilib) arrive from
# scripts/build-cell-stub-archives.sh, which builds this object once per ABI.
# They must NOT be passed as CFLAGS: a CFLAGS := assignment in a makefile
# overrides the environment value, so an incoming -mlp64 was silently dropped
# and the lp64 archive shipped an ILP32-compiled object.
CFLAGS += $(ABI_CFLAGS)

BUILD   := build
OBJS    := $(BUILD)/spu_printf.o

.PHONY: all clean

all: $(OBJS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	@echo "  CC    $(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	@mkdir -p $@

clean:
	@rm -rf $(BUILD)
