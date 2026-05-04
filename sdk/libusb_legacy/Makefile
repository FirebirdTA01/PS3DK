# sdk/libusb_legacy/Makefile
#
# Builds usb_legacy_wrappers.o — PSL1GHT usb* names forwarding into
# the cellUsbd* stubs in libusbd_stub.a.  Appended via ar-r by
# scripts/build-cell-stub-archives.sh (libusbd_stub branch), mirroring
# the libgcm_sys_legacy pattern.
#
# Reference Makefiles link -lusb; PSL1GHT homebrew links -lusb too.
# The combined archive (nidgen stub + this wrapper) goes under the
# canonical libusbd_stub.a name; a libusb.a symlink aliases back.

CROSS    ?= powerpc64-ps3-elf-
CC       := $(CROSS)gcc

PS3DEV   ?= $(error PS3DEV is not set — run: source scripts/env.sh)
PS3DK    ?= $(PS3DEV)/ps3dk

CFLAGS   := -O2 -Wall -Wextra -Wno-unused-parameter \
            -mcpu=cell -std=c11 \
            -I$(PS3_TOOLCHAIN_ROOT)/sdk/include \
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
