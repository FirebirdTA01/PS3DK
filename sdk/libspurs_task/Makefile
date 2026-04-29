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
           -falign-functions=8 -mno-branch-hints \
           -fno-schedule-insns2 \
           -I../include
SFLAGS  := -Wall

# -fno-schedule-insns2 disables GCC's post-RA instruction scheduler.
# SPU GCC's scheduler over-pads around indirect calls (`bisl`) — for a
# 6-instruction body containing a single indirect call it inserts ~32
# NOPs around the call site, bloating bodies 4-5x.  The reference SDK
# was built with the same flag (compare emitted sizes vs the
# disassembled libspurs.a object files) — keeping it ensures we match
# their function sizes byte-for-byte where the rest of the code lines
# up.

BUILD   := build
OBJS    := $(BUILD)/crt_spurs_task.o \
           $(BUILD)/spurs_task_exit.o \
           $(BUILD)/spurs_task_runtime.o \
           $(BUILD)/spurs_module_runtime.o \
           $(BUILD)/spurs_can_call_block_wait.o \
           $(BUILD)/spurs_get_workload_flag.o \
           $(BUILD)/spurs_send_workload_signal.o \
           $(BUILD)/spurs_send_signal.o \
           $(BUILD)/spurs_semaphore_p.o \
           $(BUILD)/spurs_semaphore_v.o

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
	@install -m 0644 ../include/cell/spurs/common.h $(INSTALL_INC)/cell/spurs/common.h
	@echo "  header cell/spurs/common.h"
	@install -m 0644 ../include/cell/spurs/semaphore.h $(INSTALL_INC)/cell/spurs/semaphore.h
	@echo "  header cell/spurs/semaphore.h"

clean:
	@rm -rf $(BUILD)
