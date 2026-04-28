# sdk/libgcm_cmd/Makefile
#
# Builds libgcm_cmd.a — our clean-room implementation of the
# cellGcmCg* struct-walker API over the CgBinaryProgram blob format.
# See include/Cg/cgBinary.h for the on-disk layout we target.
#
# Requires scripts/env.sh to be sourced so $PS3DEV / PPU toolchain
# binaries are on PATH.  Install target drops headers into
# $PS3DEV/psl1ght/ppu/include/ and the archive alongside our other
# PSL1GHT-resident libs.

CROSS    ?= powerpc64-ps3-elf-
CC       := $(CROSS)gcc
AR       := $(CROSS)ar
RANLIB   := $(CROSS)ranlib

PS3DEV   ?= $(error PS3DEV is not set — run: source scripts/env.sh)

CFLAGS   := -O2 -Wall -Wextra -Wno-unused-parameter \
            -mcpu=cell -std=c11 \
            -I$(PS3DK)/ppu/include \
            -Iinclude
# -MMD/-MP emit per-object .d files.  Without these, editing a header
# like sdk/include/sys/lv2_types.h leaves the dependent .o stale; make
# sees matching timestamps and the rebuild silently skips the affected
# translation units.  Bit us once on lv2_fn_to_callback_ea.
DEPFLAGS := -MMD -MP
ARFLAGS  := rcs

SRC_DIR  := src
OBJ_DIR  := build
SRCS     := $(wildcard $(SRC_DIR)/*.c)
OBJS     := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

# Absorbed rsx emitters (source-level snapshot of PSL1GHT's librsx,
# MIT-licensed — see src/rsx_legacy/LICENSE).  These land in the same
# libgcm_cmd.a so samples drop -lrsx and match the Sony SDK's layout
# (one libgcm_cmd.a, no separate rsx library).
LEGACY_DIR  := $(SRC_DIR)/rsx_legacy
LEGACY_SRCS := $(wildcard $(LEGACY_DIR)/*.c)
LEGACY_OBJS := $(patsubst $(LEGACY_DIR)/%.c,$(OBJ_DIR)/rsx_legacy_%.o,$(LEGACY_SRCS))

ARCHIVE        := $(OBJ_DIR)/libgcm_cmd.a
PS3DK          ?= $(PS3DEV)/ps3dk
INSTALL_LIB    := $(PS3DK)/ppu/lib
INSTALL_INC    := $(PS3DK)/ppu/include

.PHONY: all install clean

all: $(ARCHIVE)

$(ARCHIVE): $(OBJS) $(LEGACY_OBJS)
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

# rsx_legacy needs PSL1GHT's internal header layout: its commands.c
# includes <nv40.h> bare (not <rsx/nv40.h>) and expects LIBRSX_INTERNAL
# defined to unlock the function-pointer side of rsxSetUserCallback.
# Match PSL1GHT's librsx build flags exactly so the absorbed objects are
# ABI-identical to what librsx.a shipped — same float ABI (-mhard-float),
# same codegen (-fmodulo-sched, -ffunction-sections, -fdata-sections),
# same dialect.  A mismatch on -mhard-float against the rest of the
# runtime caused flip-sync samples to stutter badly after absorption
# until this was aligned.  -std=gnu11 unlocks the `register ... asm("r3")`
# extension in rsx_function_macros.h; -Wno-implicit-{fallthrough,
# function-declaration} silence known-intentional warnings in
# commands_impl.h.
LEGACY_CFLAGS := -O2 -Wall -mcpu=cell -mregnames -Wa,-mcell -std=gnu11 \
                 -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections \
                 -Wno-unused-parameter -Wno-implicit-fallthrough \
                 -Wno-implicit-function-declaration \
                 -I$(PS3DK)/ppu/include \
                 -I$(PS3DK)/ppu/include/rsx \
                 -Iinclude \
                 -DLIBRSX_INTERNAL

$(OBJ_DIR)/rsx_legacy_%.o: $(LEGACY_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(LEGACY_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(OBJ_DIR):
	@mkdir -p $@

install: $(ARCHIVE)
	@install -d $(INSTALL_LIB)
	install -m 0644 $(ARCHIVE) $(INSTALL_LIB)/libgcm_cmd.a
	@install -d $(INSTALL_INC)/Cg
	install -m 0644 include/Cg/cg.h       $(INSTALL_INC)/Cg/cg.h
	install -m 0644 include/Cg/cgBinary.h $(INSTALL_INC)/Cg/cgBinary.h
	@install -d $(INSTALL_INC)/cell/gcm
	install -m 0644 include/cell/gcm/gcm_cg_func.h    $(INSTALL_INC)/cell/gcm/gcm_cg_func.h
	install -m 0644 include/cell/gcm/ps3tc_fifo_wrap.h $(INSTALL_INC)/cell/gcm/ps3tc_fifo_wrap.h
	@echo "installed libgcm_cmd.a + headers under $(PS3DK)/ppu/"

clean:
	rm -rf $(OBJ_DIR)

-include $(wildcard $(OBJ_DIR)/*.d)
