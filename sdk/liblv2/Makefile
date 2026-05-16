# sdk/liblv2/Makefile
#
# Builds liblv2.a — our clean-room native re-implementation of the
# five "liblv2" PPU wrappers (sysThreadCreate, sysProcessExitSpawn2,
# sysSpuImageOpenELF, the SPU-printf server, spu_thread_printf /
# spu_thread_sprintf).  These are thin glue over the SDK's own
# sys/* primitives + the nidgen-emitted SPRX imports in liblv2_stub.a;
# PSL1GHT-style public names are kept as compatibility entry points.
#
# Multilib structure mirrors sdk/libgcm_cmd/Makefile exactly: an
# ILP32 default archive plus an LP64 (-mlp64) archive built into a
# parallel OBJ_DIR-lp64, installed to $(INSTALL_LIB) and
# $(INSTALL_LIB)/lp64 respectively.
#
# Requires scripts/env.sh to be sourced so $PS3DEV / PPU toolchain
# binaries are on PATH.

CROSS    ?= powerpc64-ps3-elf-
CC       := $(CROSS)gcc
AR       := $(CROSS)ar
RANLIB   := $(CROSS)ranlib

PS3DEV   ?= $(error PS3DEV is not set — run: source scripts/env.sh)
PS3DK    ?= $(PS3DEV)/ps3dk

CFLAGS   := -O2 -Wall -Wextra -Wno-unused-parameter \
            -mcpu=cell -std=c11 \
            -I$(PS3DK)/ppu/include
DEPFLAGS := -MMD -MP
ARFLAGS  := rcs

SRC_DIR  := src
OBJ_DIR  := build
SRCS     := $(wildcard $(SRC_DIR)/*.c)
OBJS     := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

ARCHIVE        := $(OBJ_DIR)/liblv2.a
INSTALL_LIB    := $(PS3DK)/ppu/lib

# --- LP64 (-mlp64) multilib (after all ILP32 := expansions) ---
LP64_OBJ_DIR   := $(OBJ_DIR)-lp64
LP64_ARCHIVE   := $(LP64_OBJ_DIR)/liblv2.a
LP64_OBJS      := $(patsubst $(SRC_DIR)/%.c,$(LP64_OBJ_DIR)/%.o,$(SRCS))
LP64_CFLAGS    := $(CFLAGS) -mlp64

.PHONY: all install clean

all: $(ARCHIVE) $(LP64_ARCHIVE)

$(ARCHIVE): $(OBJS)
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

$(LP64_ARCHIVE): $(LP64_OBJS)
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(OBJ_DIR):
	@mkdir -p $@

$(LP64_OBJ_DIR):
	@mkdir -p $@

$(LP64_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(LP64_OBJ_DIR)
	$(CC) $(LP64_CFLAGS) $(DEPFLAGS) -c -o $@ $<

install: $(ARCHIVE) $(LP64_ARCHIVE)
	@install -d $(INSTALL_LIB)
	install -m 0644 $(ARCHIVE) $(INSTALL_LIB)/liblv2.a
	@install -d $(INSTALL_LIB)/lp64
	install -m 0644 $(LP64_ARCHIVE) $(INSTALL_LIB)/lp64/liblv2.a
	@echo "installed liblv2.a (ILP32 + LP64) under $(PS3DK)/ppu/lib"

clean:
	rm -rf $(OBJ_DIR) $(LP64_OBJ_DIR)

-include $(wildcard $(OBJ_DIR)/*.d)
-include $(wildcard $(LP64_OBJ_DIR)/*.d)
