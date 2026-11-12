# sdk/libgcm_spu_cmd/Makefile -- SPU GCM command writer.
#
# Outputs:
#   libgcm_spu_cmd.a  -> $(SPU_PREFIX)/spu-elf/lib/ (canonical)
#                     -> $(PS3DK)/spu/lib/          (compatibility mirror)
#
# <cell/gcm/gcm_spu_cmd.h>: method words built in local store with the
# PPU cellGcm* names, plus cellGcmSpuPutSegment to DMA the finished
# buffer out.  gcm_spu_cmd.c is portable C (the host encoding test builds
# it too); gcm_spu_put.c is SPU-only.  Built with spu-elf-gcc.

PS3DEV     ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
SPU_PREFIX ?= $(PS3DEV)/spu

INSTALL_LIB := $(SPU_PREFIX)/spu-elf/lib
INSTALL_LIB_MIRROR := $(PS3DK)/spu/lib

override CC := spu-elf-gcc
AR := spu-elf-ar

CFLAGS := -O2 -Wall -Wextra -Werror -std=gnu11 -fno-exceptions -ffreestanding \
          -ffunction-sections -fdata-sections -I../include-spu -I$(PS3DK)/spu/include

BUILD := build
OBJS  := $(BUILD)/gcm_spu_cmd.o $(BUILD)/gcm_spu_put.o
LIB   := $(BUILD)/libgcm_spu_cmd.a

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
	@install -m 0644 $(LIB) $(INSTALL_LIB)/libgcm_spu_cmd.a
	@install -m 0644 $(LIB) $(INSTALL_LIB_MIRROR)/libgcm_spu_cmd.a
	@echo "  lib    libgcm_spu_cmd.a"

clean:
	@rm -rf $(BUILD)
