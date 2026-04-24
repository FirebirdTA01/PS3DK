# sdk/libspurs_task/Makefile — SPU-side Spurs task runtime.
#
#   libspurs_task.a    → $(PS3DK)/spu/lib/
#   spurs_task.ld      → $(PS3DK)/spu/ldscripts/
#   cell/spurs/spu_task.h → $(PS3DK)/spu/include/cell/spurs/
#
# Built with spu-elf-gcc.  Minimal deps - only depends on stdint.h
# and spu_intrinsics.h, both ships with the upstream SPU toolchain.

PS3DEV     ?= $(error PS3DEV is not set — run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
SPU_PREFIX ?= $(PS3DEV)/spu

INSTALL_LIB := $(PS3DK)/spu/lib
INSTALL_LDS := $(PS3DK)/spu/ldscripts
INSTALL_INC := $(PS3DK)/spu/include

CC      := spu-elf-gcc
AR      := spu-elf-ar

CFLAGS  := -O2 -Wall -Wextra -fno-exceptions -ffreestanding \
           -falign-functions=8 -mno-branch-hints
SFLAGS  := -Wall

BUILD   := build
OBJS    := $(BUILD)/crt_spurs_task.o \
           $(BUILD)/spurs_task_exit.o \
           $(BUILD)/spurs_task_runtime.o

LIB     := $(BUILD)/libspurs_task.a

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
	@install -d $(INSTALL_LIB) $(INSTALL_LDS) $(INSTALL_INC)/cell/spurs
	@install -m 0644 $(LIB) $(INSTALL_LIB)/libspurs_task.a
	@echo "  lib   libspurs_task.a"
	@install -m 0644 scripts/spurs_task.ld $(INSTALL_LDS)/spurs_task.ld
	@echo "  ld    spurs_task.ld"
	@install -m 0644 include/cell/spurs/spu_task.h $(INSTALL_INC)/cell/spurs/spu_task.h
	@echo "  header cell/spurs/spu_task.h"
	@install -m 0644 ../include/cell/spurs/task_types.h $(INSTALL_INC)/cell/spurs/task_types.h
	@echo "  header cell/spurs/task_types.h"

clean:
	@rm -rf $(BUILD)
