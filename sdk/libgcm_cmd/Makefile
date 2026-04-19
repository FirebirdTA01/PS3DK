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
            -I$(PS3DEV)/psl1ght/ppu/include \
            -Iinclude
ARFLAGS  := rcs

SRC_DIR  := src
OBJ_DIR  := build
SRCS     := $(wildcard $(SRC_DIR)/*.c)
OBJS     := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

ARCHIVE        := $(OBJ_DIR)/libgcm_cmd.a
INSTALL_LIB    := $(PS3DEV)/psl1ght/ppu/lib
INSTALL_INC    := $(PS3DEV)/psl1ght/ppu/include

.PHONY: all install clean

all: $(ARCHIVE)

$(ARCHIVE): $(OBJS)
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

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
	@echo "installed libgcm_cmd.a + headers under $(PS3DEV)/psl1ght/ppu/"

clean:
	rm -rf $(OBJ_DIR)
