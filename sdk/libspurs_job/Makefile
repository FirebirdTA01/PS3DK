# sdk/libspurs_job/Makefile - SPU side of the cellSpursJobMain2 entry-point convention.
#
# Outputs:
#   libspurs_job.a            -> $(PS3DK)/spu/lib/
#   spurs_job.ld              -> $(PS3DK)/spu/ldscripts/
#   cell/spurs/job_context.h  -> $(PS3DK)/spu/include/cell/spurs/
#   cell/spurs/job_chain.h    -> $(PS3DK)/spu/include/cell/spurs/
#
# Built with spu-elf-gcc.  Job binaries link against this archive +
# spurs_job.ld with -nostartfiles to get the right layout.

PS3DEV     ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
SPU_PREFIX ?= $(PS3DEV)/spu

INSTALL_LIB := $(PS3DK)/spu/lib
INSTALL_LDS := $(PS3DK)/spu/ldscripts
INSTALL_INC := $(PS3DK)/spu/include

CC := spu-elf-gcc
AR := spu-elf-ar

# -fpic: required for spurs-job binaries.  The SPRX dispatcher loads
# the binary at a runtime-chosen LS offset, so all absolute references
# (ila, ilhu/iohl) need PC-relative resolution.  The reference SDK's
# patched GCC adds -mspurs-job which enables this plus a couple of
# spurs-specific tweaks; stock GCC 9.5 doesn't have that flag, so we
# stick with -fpic alone.  -Wl,-q (--emit-relocs) is applied at link
# time by the consuming sample, not here (this archive is just objects).
CFLAGS := -O2 -Wall -Wextra -fno-exceptions -ffreestanding -fpic \
          -falign-functions=8 -mno-branch-hints
SFLAGS := -Wall -fpic

BUILD := build
OBJS  := $(BUILD)/job_start.o $(BUILD)/job_initialize.o
LIB   := $(BUILD)/libspurs_job.a

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
	@install -m 0644 $(LIB) $(INSTALL_LIB)/libspurs_job.a
	@echo "  lib    libspurs_job.a"
	@install -m 0644 scripts/spurs_job.ld $(INSTALL_LDS)/spurs_job.ld
	@echo "  ld     spurs_job.ld"
	@install -m 0644 include/cell/spurs/job_context.h $(INSTALL_INC)/cell/spurs/job_context.h
	@echo "  header cell/spurs/job_context.h"
	@install -m 0644 include/cell/spurs/job_chain.h $(INSTALL_INC)/cell/spurs/job_chain.h
	@echo "  header cell/spurs/job_chain.h"

clean:
	@rm -rf $(BUILD)
