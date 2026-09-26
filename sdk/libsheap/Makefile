# sdk/libsheap/Makefile -- SPU shared heap and keyed shared objects.
#
# Outputs:
#   libsheap.a   -> $(SPU_PREFIX)/spu-elf/lib/ (canonical)
#                -> $(PS3DK)/spu/lib/           (compatibility mirror)
#
# Implements cell/sheap.h for the SPU: the heap functions, the keyed heap
# and the keyed buffer/mutex/barrier/queue/rwm/semaphore New and Delete.
# The heap layout is the one the PPU firmware (cellSheap) uses, so a heap
# initialised on either side can be used from both.  src/core/ is plain
# portable C (also built on the host by tests/sdk/sheap-core-host-test.sh);
# src/spu/ adds the DMA, lock-line and cache layer.
#
# Programs link -lsheap -lsync -ldma; the keyed objects call libsync.
#
# The shared headers (cell/sheap/error.h, cell/sheap/sheap_types.h) live in
# ../include and are searched last, after the SPU and installed headers.

PS3DEV     ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
SPU_PREFIX ?= $(PS3DEV)/spu

INSTALL_LIB := $(SPU_PREFIX)/spu-elf/lib
INSTALL_LIB_MIRROR := $(PS3DK)/spu/lib

override CC := spu-elf-gcc
AR := spu-elf-ar

CFLAGS := -O2 -Wall -Wextra -fno-exceptions -ffreestanding \
          -I../include-spu \
          -I$(PS3DK)/spu/include \
          -idirafter ../include

BUILD := build
CORE  := tree heap
SPU   := lockline linecache sheap keytable key_objects
OBJS  := $(addprefix $(BUILD)/,$(addsuffix .o,$(CORE) $(SPU)))
LIB   := $(BUILD)/libsheap.a

.PHONY: all install clean

all: $(LIB)

$(LIB): $(OBJS)
	@echo "  AR    $(notdir $@)"
	@rm -f $@
	@$(AR) rcs $@ $(OBJS)

$(BUILD)/%.o: src/core/%.c src/core/sheap_core.h | $(BUILD)
	@echo "  CC    core/$(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/spu/%.c src/spu/sheap_spu.h src/sheap_layout.h src/core/sheap_core.h | $(BUILD)
	@echo "  CC    spu/$(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	@mkdir -p $@

install: $(LIB)
	@install -d $(INSTALL_LIB) $(INSTALL_LIB_MIRROR)
	@install -m 0644 $(LIB) $(INSTALL_LIB)/libsheap.a
	@install -m 0644 $(LIB) $(INSTALL_LIB_MIRROR)/libsheap.a
	@echo "  lib    libsheap.a"

clean:
	rm -rf $(BUILD)
