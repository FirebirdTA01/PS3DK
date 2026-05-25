# sdk/libdma/Makefile -- SPU-side MFC DMA helper library.
#
# Outputs:
#   libdma.a     -> $(SPU_PREFIX)/spu-elf/lib/ (canonical)
#                -> $(PS3DK)/spu/lib/           (compatibility mirror)
#
# Three extern entry points not covered by the inline macros in
# <cell/dma.h>: cellDmaAndWait, cellDmaLargeCmd, cellDmaUnalignedCmd.
#
# Built with spu-elf-gcc.

PS3DEV     ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
SPU_PREFIX ?= $(PS3DEV)/spu

INSTALL_LIB := $(SPU_PREFIX)/spu-elf/lib
INSTALL_LIB_MIRROR := $(PS3DK)/spu/lib

CC := spu-elf-gcc
AR := spu-elf-ar

CFLAGS := -O2 -Wall -Wextra -fno-exceptions -ffreestanding \
          -I$(PS3DK)/spu/include

BUILD := build
OBJS  := $(BUILD)/dma_and_wait.o $(BUILD)/dma_large_cmd.o $(BUILD)/dma_unaligned_cmd.o
LIB   := $(BUILD)/libdma.a

.PHONY: all install clean

all: $(LIB)

$(LIB): $(OBJS)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(OBJS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	@echo "  CC    $(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	@mkdir -p $@

install: $(LIB)
	@install -d $(INSTALL_LIB) $(INSTALL_LIB_MIRROR)
	@install -m 0644 $(LIB) $(INSTALL_LIB)/libdma.a
	@install -m 0644 $(LIB) $(INSTALL_LIB_MIRROR)/libdma.a
	@echo "  lib    libdma.a"

clean:
	@rm -rf $(BUILD)
