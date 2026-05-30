# sdk/libperf/Makefile
#
# Builds libperf.a, a PPU-side placeholder archive for the public
# cell/perf/performance.h surface.

CROSS    ?= powerpc64-ps3-elf-
CC       := $(CROSS)gcc
AR       := $(CROSS)ar
RANLIB   := $(CROSS)ranlib

PS3DEV   ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK    ?= $(PS3DEV)/ps3dk

CFLAGS   := -O2 -Wall -Wextra -Wno-unused-parameter \
            -mcpu=cell -std=c11 \
            -I$(PS3DK)/ppu/include
DEPFLAGS := -MMD -MP
ARFLAGS  := rcs

SRC_DIR  := .
OBJ_DIR  := build
SRCS     := libperf.c
OBJS     := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SRCS))

ARCHIVE        := $(OBJ_DIR)/libperf.a
INSTALL_LIB    := $(PS3DK)/ppu/lib

LP64_OBJ_DIR   := $(OBJ_DIR)-lp64
LP64_ARCHIVE   := $(LP64_OBJ_DIR)/libperf.a
LP64_OBJS      := $(patsubst %.c,$(LP64_OBJ_DIR)/%.o,$(SRCS))
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
	install -m 0644 $(ARCHIVE) $(INSTALL_LIB)/libperf.a
	@install -d $(INSTALL_LIB)/lp64
	install -m 0644 $(LP64_ARCHIVE) $(INSTALL_LIB)/lp64/libperf.a
	@echo "installed libperf.a (ILP32 + LP64) under $(PS3DK)/ppu/lib"

clean:
	rm -rf $(OBJ_DIR) $(LP64_OBJ_DIR)

-include $(wildcard $(OBJ_DIR)/*.d)
-include $(wildcard $(LP64_OBJ_DIR)/*.d)
