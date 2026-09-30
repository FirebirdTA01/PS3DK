# sdk/libspurs_job/Makefile - SPU side of the cellSpursJobMain2 entry-point convention.
#
# Outputs:
#   libspurs_job.a            -> $(SPU_PREFIX)/spu-elf/lib/ (canonical)
#                            -> $(PS3DK)/spu/lib/           (compatibility mirror)
#   spurs_job.ld              -> $(PS3DK)/spu/ldscripts/
#   cell/spurs/job_context.h  -> $(PS3DK)/spu/include/cell/spurs/
#   cell/spurs/job_chain.h    -> $(PS3DK)/spu/include/cell/spurs/
#
# Built with spu-elf-gcc.  Job binaries link against this archive +
# spurs_job.ld with -nostartfiles to get the right layout.

PS3DEV     ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
SPU_PREFIX ?= $(PS3DEV)/spu

INSTALL_LIB := $(SPU_PREFIX)/spu-elf/lib
INSTALL_LIB_MIRROR := $(PS3DK)/spu/lib
INSTALL_LDS := $(PS3DK)/spu/ldscripts
INSTALL_INC := $(PS3DK)/spu/include

override CC := spu-elf-gcc
AR := spu-elf-ar

# -fpic: the archive's objects are position-independent, so they work in
# an image linked at any base.  A -mspurs-job image itself is linked
# where the job manager loads it (spurs_job.ld, LS 0x4c00, from
# __spurs_job_base in job_start.S) and needs no run-time relocation;
# only a -mspurs-job-initialize image, linked at 0, is relocated (by its
# startup, libspurs_jq).
CFLAGS := -O2 -Wall -Wextra -fno-exceptions -ffreestanding -fpic \
          -falign-functions=8 -mno-branch-hints
SFLAGS := -Wall -fpic

BUILD := build
OBJS  := $(BUILD)/job_start.o $(BUILD)/job_initialize.o
LIB   := $(BUILD)/libspurs_job.a
RUNTIME_LIB := $(BUILD)/libspurs_job_runtime.a
RUNTIME_OBJS := $(BUILD)/job_initialize.o

.PHONY: all install clean

all: $(LIB) $(RUNTIME_LIB)

$(LIB): $(OBJS)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(OBJS)

$(RUNTIME_LIB): $(RUNTIME_OBJS)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(RUNTIME_OBJS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	@echo "  CC    $(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.S | $(BUILD)
	@echo "  AS    $(notdir $<)"
	@$(CC) $(SFLAGS) -c $< -o $@

$(BUILD):
	@mkdir -p $@

install: $(LIB) $(RUNTIME_LIB)
	@install -d $(INSTALL_LIB) $(INSTALL_LIB_MIRROR) $(INSTALL_LDS) $(INSTALL_INC)/cell/spurs
	@install -m 0644 $(LIB) $(INSTALL_LIB)/libspurs_job.a
	@install -m 0644 $(LIB) $(INSTALL_LIB_MIRROR)/libspurs_job.a
	@install -m 0644 $(RUNTIME_LIB) $(INSTALL_LIB)/libspurs_job_runtime.a
	@install -m 0644 $(RUNTIME_LIB) $(INSTALL_LIB_MIRROR)/libspurs_job_runtime.a
	@install -m 0644 $(BUILD)/job_start.o $(INSTALL_LIB)/job_start.o
	@install -m 0644 $(BUILD)/job_start.o $(INSTALL_LIB_MIRROR)/job_start.o
	@echo "  lib    libspurs_job.a"
	@install -m 0644 scripts/spurs_job.ld $(INSTALL_LDS)/spurs_job.ld
	@install -m 0644 scripts/spurs_job.ld $(INSTALL_LIB)/spurs_job.ld
	@echo "  ld     spurs_job.ld"
	@install -m 0644 include/cell/spurs/job_context.h $(INSTALL_INC)/cell/spurs/job_context.h
	@echo "  header cell/spurs/job_context.h"
	@install -m 0644 include/cell/spurs/job_chain.h $(INSTALL_INC)/cell/spurs/job_chain.h
	@echo "  header cell/spurs/job_chain.h"

clean:
	@rm -rf $(BUILD)
