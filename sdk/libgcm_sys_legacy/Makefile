# sdk/libgcm_sys_legacy/Makefile
#
# Builds a single translation unit — gcm_legacy_wrappers.o — that ships
# inside $PS3DK/ppu/lib/libgcm_sys.a alongside the nidgen-generated
# cellGcm* stubs.  Provides the PSL1GHT-flavoured gcm* names that
# <cell/gcm.h>'s static-inline forwarders still emit.
#
# Driven by scripts/build-cell-stub-archives.sh — it invokes `make -C
# sdk/libgcm_sys_legacy` to produce build/gcm_legacy_wrappers.o, then
# `ar r` the object into the nidgen stub archive before installing it
# as libgcm_sys.a under ps3dk/ppu/lib/.

CROSS    ?= powerpc64-ps3-elf-
CC       := $(CROSS)gcc

PS3DEV   ?= $(error PS3DEV is not set — run: source scripts/env.sh)
PS3DK    ?= $(PS3DEV)/ps3dk

CFLAGS   := -O2 -Wall -Wextra -Wno-unused-parameter \
            -mcpu=cell -std=c11 \
            -I$(PS3DK)/ppu/include \
            -I$(PS3DEV)/psl1ght/ppu/include
# -MMD/-MP emit per-object .d files so editing a shared header (e.g.
# sdk/include/sys/lv2_types.h) correctly invalidates the .o.
DEPFLAGS := -MMD -MP

SRC_DIR  := src
OBJ_DIR  := build
SRCS     := $(wildcard $(SRC_DIR)/*.c)
OBJS     := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

.PHONY: all clean

all: $(OBJS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(OBJ_DIR):
	@mkdir -p $@

clean:
	rm -rf $(OBJ_DIR)

-include $(wildcard $(OBJ_DIR)/*.d)
