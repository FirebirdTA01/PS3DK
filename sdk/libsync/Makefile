# sdk/libsync/Makefile -- SPU-side cell/sync runtime.
#
# Outputs:
#   libsync.a    -> $(SPU_PREFIX)/spu-elf/lib/ (canonical)
#                -> $(PS3DK)/spu/lib/           (compatibility mirror)
#
# Implements the cellSyncBarrier/Queue/Mutex/Rwm/Lfqueue entry points
# over MFC atomic primitives from cell/atomic.h.
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
          -I../include-spu \
          -I$(PS3DK)/spu/include

BUILD := build
OBJS  := $(BUILD)/barrier.o $(BUILD)/queue.o $(BUILD)/mutex.o $(BUILD)/rwm.o $(BUILD)/lfqueue.o
LIB   := $(BUILD)/libsync.a

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
	@install -m 0644 $(LIB) $(INSTALL_LIB)/libsync.a
	@install -m 0644 $(LIB) $(INSTALL_LIB_MIRROR)/libsync.a
	@echo "  lib    libsync.a"

clean:
	@rm -rf $(BUILD)
