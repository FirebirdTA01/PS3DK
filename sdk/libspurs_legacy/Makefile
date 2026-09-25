# sdk/libspurs_legacy/Makefile
#
# Builds spurs_legacy.o -- PSL1GHT's spursAttributeInitialize, forwarding to
# the SDK's cellSpursAttributeInitialize. Appended via ar-r by
# scripts/build-cell-stub-archives.sh (libspurs_stub branch), mirroring
# the libgcm_sys_legacy pattern.
#
# PSL1GHT homebrew links -lspurs; reference code links -lspurs_stub.
# The combined archive (nidgen stub + this object) goes under the
# canonical libspurs_stub.a name; a libspurs.a symlink aliases back.
#
# Headers come from the installed tree only: adding sdk/include as well puts
# two copies of the PS3DK wrapper headers on the path, and the sys/types.h
# guard (reached through cell/spurs/types.h) rejects that.

CROSS    ?= powerpc64-ps3-elf-
CC       := $(CROSS)gcc

PS3DEV   ?= $(error PS3DEV is not set — run: source scripts/env.sh)
PS3DK    ?= $(PS3DEV)/ps3dk

CFLAGS   := -O2 -Wall -Wextra -Wno-unused-parameter \
            -mcpu=cell -std=c11 \
            -I$(PS3DK)/ppu/include \
            -I$(PS3DEV)/psl1ght/ppu/include

# ABI flags (-mlp64 for the LP64 multilib) arrive from
# scripts/build-cell-stub-archives.sh, which builds this object once per ABI.
# They must NOT be passed as CFLAGS: a CFLAGS := assignment in a makefile
# overrides the environment value, so an incoming -mlp64 was silently dropped
# and the lp64 archive shipped an ILP32-compiled object.
CFLAGS += $(ABI_CFLAGS)

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
