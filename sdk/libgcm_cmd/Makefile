# sdk/libgcm_cmd/Makefile
#
# Builds libgcm_cmd.a — our independent implementation of the
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
# libgcm_cmd.a so samples drop -lrsx and match the the reference SDK's layout
# (one libgcm_cmd.a, no separate rsx library).
LEGACY_DIR  := $(SRC_DIR)/rsx_legacy
LEGACY_SRCS := $(wildcard $(LEGACY_DIR)/*.c)
LEGACY_OBJS := $(patsubst $(LEGACY_DIR)/%.c,$(OBJ_DIR)/rsx_legacy_%.o,$(LEGACY_SRCS))

# rsx_legacy needs PSL1GHT's internal header layout: its commands.c
# includes <nv40.h> bare (not <rsx/nv40.h>) and expects LIBRSX_INTERNAL
# defined to unlock the function-pointer side of rsxSetUserCallback.
LEGACY_CFLAGS := -O2 -Wall -mcpu=cell -mregnames -Wa,-mcell -std=gnu11 \
                 -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections \
                 -Wno-unused-parameter -Wno-implicit-fallthrough \
                 -Wno-implicit-function-declaration \
                 -I$(PS3DK)/ppu/include \
                 -I$(PS3DK)/ppu/include/rsx \
                 -Iinclude \
                 -DLIBRSX_INTERNAL

ARCHIVE        := $(OBJ_DIR)/libgcm_cmd.a
PS3DK          ?= $(PS3DEV)/ps3dk
INSTALL_LIB    := $(PS3DK)/ppu/lib
INSTALL_INC    := $(PS3DK)/ppu/include

# --- LP64 (-mlp64) multilib (after all ILP32 := expansions; before targets) ---
LP64_OBJ_DIR       := $(OBJ_DIR)-lp64
LP64_ARCHIVE       := $(LP64_OBJ_DIR)/libgcm_cmd.a
LP64_OBJS          := $(patsubst $(SRC_DIR)/%.c,$(LP64_OBJ_DIR)/%.o,$(SRCS))
LP64_LEGACY_OBJS   := $(patsubst $(LEGACY_DIR)/%.c,$(LP64_OBJ_DIR)/rsx_legacy_%.o,$(LEGACY_SRCS))
LP64_CFLAGS        := $(CFLAGS) -mlp64
LP64_LEGACY_CFLAGS := $(LEGACY_CFLAGS) -mlp64

.PHONY: all install clean

all: $(ARCHIVE) $(LP64_ARCHIVE)

$(ARCHIVE): $(OBJS) $(LEGACY_OBJS)
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

$(LP64_ARCHIVE): $(LP64_OBJS) $(LP64_LEGACY_OBJS)
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(OBJ_DIR)/rsx_legacy_%.o: $(LEGACY_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(LEGACY_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(OBJ_DIR):
	@mkdir -p $@

$(LP64_OBJ_DIR):
	@mkdir -p $@

$(LP64_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(LP64_OBJ_DIR)
	$(CC) $(LP64_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(LP64_OBJ_DIR)/rsx_legacy_%.o: $(LEGACY_DIR)/%.c | $(LP64_OBJ_DIR)
	$(CC) $(LP64_LEGACY_CFLAGS) $(DEPFLAGS) -c -o $@ $<

install: $(ARCHIVE) $(LP64_ARCHIVE)
	@install -d $(INSTALL_LIB)
	install -m 0644 $(ARCHIVE) $(INSTALL_LIB)/libgcm_cmd.a
	@install -d $(INSTALL_LIB)/lp64
	install -m 0644 $(LP64_ARCHIVE) $(INSTALL_LIB)/lp64/libgcm_cmd.a
	@install -d $(INSTALL_INC)/Cg
	install -m 0644 include/Cg/cg.h       $(INSTALL_INC)/Cg/cg.h
	install -m 0644 include/Cg/cgBinary.h $(INSTALL_INC)/Cg/cgBinary.h
	@install -d $(INSTALL_INC)/cell/gcm
	install -m 0644 include/cell/gcm/gcm_cg_func.h    $(INSTALL_INC)/cell/gcm/gcm_cg_func.h
	install -m 0644 include/cell/gcm/ps3tc_fifo_wrap.h $(INSTALL_INC)/cell/gcm/ps3tc_fifo_wrap.h
	@echo "installed libgcm_cmd.a + headers under $(PS3DK)/ppu/"

clean:
	rm -rf $(OBJ_DIR) $(LP64_OBJ_DIR)

-include $(wildcard $(OBJ_DIR)/*.d)
