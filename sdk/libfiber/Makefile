# sdk/libfiber/Makefile -- SPU-side fiber runtime.
#
# Outputs:
#   libfiber.a   -> $(SPU_PREFIX)/spu-elf/lib/ (canonical)
#                -> $(PS3DK)/spu/lib/           (compatibility mirror)
#
# Implements SPU fiber contexts (cell/fiber/spu_context.h): cooperative
# context switching inside one SPU program.  SPU-to-PPU fiber signalling
# (cell/fiber/ppu_fiber.h) is not implemented; its symbols stay undefined
# at link time rather than being stubbed.

PS3DEV     ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
SPU_PREFIX ?= $(PS3DEV)/spu

INSTALL_LIB := $(SPU_PREFIX)/spu-elf/lib
INSTALL_LIB_MIRROR := $(PS3DK)/spu/lib

override CC := spu-elf-gcc
AR := spu-elf-ar

CFLAGS := -O2 -Wall -Wextra -fno-exceptions -ffreestanding \
          -I../include-spu \
          -I$(PS3DK)/spu/include

BUILD := build
OBJS  := $(BUILD)/fiber_spu_context.o $(BUILD)/context_switch.o
LIB   := $(BUILD)/libfiber.a

.PHONY: all install clean

all: $(LIB)

$(LIB): $(OBJS)
	@echo "  AR    $(notdir $@)"
	@rm -f $@
	@$(AR) rcs $@ $(OBJS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	@echo "  CC    $(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.S | $(BUILD)
	@echo "  AS    $(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	@mkdir -p $@

install: $(LIB)
	@install -d $(INSTALL_LIB) $(INSTALL_LIB_MIRROR)
	@install -m 0644 $(LIB) $(INSTALL_LIB)/libfiber.a
	@install -m 0644 $(LIB) $(INSTALL_LIB_MIRROR)/libfiber.a
	@echo "  lib    libfiber.a"

clean:
	rm -rf $(BUILD)
