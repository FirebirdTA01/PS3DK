# sdk/libio_legacy/Makefile
#
# Builds a single translation unit — io_legacy_wrappers.o — that ships
# inside $PS3DK/ppu/lib/libio.a alongside the nidgen-generated cellPad*/
# cellKb*/cellMouse* stubs.  Provides PSL1GHT's ioPad*/ioKb*/ioMouse*
# spellings as one-line pass-throughs to the Sony-named entry points.
#
# Driven by scripts/build-cell-stub-archives.sh — it invokes
# `make -C sdk/libio_legacy` to produce build/io_legacy_wrappers.o, then
# `ar r` the object into the nidgen stub archive before installing it as
# libio.a under ps3dk/ppu/lib/ (shadows PSL1GHT's libio.a).

CROSS    ?= powerpc64-ps3-elf-
CC       := $(CROSS)gcc

PS3DEV   ?= $(error PS3DEV is not set — run: source scripts/env.sh)
PS3DK    ?= $(PS3DEV)/ps3dk

CFLAGS   := -O2 -Wall -Wextra -Wno-unused-parameter \
            -mcpu=cell -std=c11 \
            -I$(PS3DK)/ppu/include \
            -I$(PS3DEV)/psl1ght/ppu/include
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
