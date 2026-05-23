# sdk/librsx/Makefile
#
# Builds librsx.a from the SDK-owned source-level snapshot of
# PSL1GHT's legacy rsx* command/allocator surface.  This archive exists
# for compatibility with samples and homebrew that still link -lrsx;
# libgcm_cmd keeps absorbing the same source while the Cell-style
# command surface is still being filled in.
#
# Multilib structure mirrors sdk/liblv2/Makefile: an ILP32 default
# archive plus an LP64 (-mlp64) archive installed to $(PS3DK)/ppu/lib
# and $(PS3DK)/ppu/lib/lp64.

CROSS    ?= powerpc64-ps3-elf-
CC       := $(CROSS)gcc
AR       := $(CROSS)ar
RANLIB   := $(CROSS)ranlib

PS3DEV   ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK    ?= $(PS3DEV)/ps3dk

CFLAGS   := -O2 -Wall -mcpu=cell -mregnames -Wa,-mcell -std=gnu11 \
            -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections \
            -Wno-unused-parameter -Wno-implicit-fallthrough \
            -Wno-implicit-function-declaration \
            -I$(PS3DK)/ppu/include \
            -I$(PS3DK)/ppu/include/rsx \
            -DLIBRSX_INTERNAL
DEPFLAGS := -MMD -MP
ARFLAGS  := rcs

SRC_DIR  := src
OBJ_DIR  := build
SRCS     := $(filter-out $(SRC_DIR)/LICENSE $(SRC_DIR)/README.md,$(wildcard $(SRC_DIR)/*.c))
OBJS     := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

ARCHIVE        := $(OBJ_DIR)/librsx.a
INSTALL_LIB    := $(PS3DK)/ppu/lib

LP64_OBJ_DIR   := $(OBJ_DIR)-lp64
LP64_ARCHIVE   := $(LP64_OBJ_DIR)/librsx.a
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

$(LP64_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(LP64_OBJ_DIR)
	$(CC) $(LP64_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(OBJ_DIR) $(LP64_OBJ_DIR):
	@mkdir -p $@

install: $(ARCHIVE) $(LP64_ARCHIVE)
	@install -d $(INSTALL_LIB)
	install -m 0644 $(ARCHIVE) $(INSTALL_LIB)/librsx.a
	@install -d $(INSTALL_LIB)/lp64
	install -m 0644 $(LP64_ARCHIVE) $(INSTALL_LIB)/lp64/librsx.a
	@echo "installed librsx.a (ILP32 + LP64) under $(PS3DK)/ppu/lib"

clean:
	rm -rf $(OBJ_DIR) $(LP64_OBJ_DIR)

-include $(wildcard $(OBJ_DIR)/*.d)
-include $(wildcard $(LP64_OBJ_DIR)/*.d)
