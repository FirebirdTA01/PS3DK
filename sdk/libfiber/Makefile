# sdk/libfiber/Makefile -- SPU-side fiber support library placeholder.
#
# Outputs:
#   libfiber.a   -> $(SPU_PREFIX)/spu-elf/lib/ (canonical)
#                -> $(PS3DK)/spu/lib/           (compatibility mirror)
#
# Current state: empty archive.  The -lfiber flag resolves at link
# time but no fiber symbols are provided.  Samples that actually call
# cellFiber* functions will fail with honest undefined-reference
# errors at link time.  Fill in real implementations incrementally.

PS3DEV     ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
SPU_PREFIX ?= $(PS3DEV)/spu

INSTALL_LIB := $(SPU_PREFIX)/spu-elf/lib
INSTALL_LIB_MIRROR := $(PS3DK)/spu/lib

AR := spu-elf-ar

BUILD := build
LIB   := $(BUILD)/libfiber.a

.PHONY: all install clean

all: $(LIB)

$(LIB): | $(BUILD)
	@echo "  AR    $(notdir $@) (empty)"
	@$(AR) rcs $@

$(BUILD):
	@mkdir -p $@

install: $(LIB)
	@install -d $(INSTALL_LIB) $(INSTALL_LIB_MIRROR)
	@install -m 0644 $(LIB) $(INSTALL_LIB)/libfiber.a
	@install -m 0644 $(LIB) $(INSTALL_LIB_MIRROR)/libfiber.a
	@echo "  lib    libfiber.a (empty)"

clean:
	@rm -rf $(BUILD)
