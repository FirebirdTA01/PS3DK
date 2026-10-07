# libPSGLFX_stub -- build-unblock placeholder
#
# The reference SDK's unconditional -lPSGLFX flag resolves against the
# closed-source CgFX Effects Framework runtime (not distributed).
# cgb_basic_psgl (and other CGB samples) do not call *any* PSGLFX
# symbols; they carry the flag by template convention only.
#
# This stub is an empty archive that satisfies the linker.  Real PSGLFX
# (cgCreateEffect, cgCreateTechnique, state assignment dispatch, etc.)
# stays a future workstream for samples that actually reference .cgfx
# files.
#
# When a real libPSGLFX implementation ships, drop this stub directory
# and replace with the full sdk/libPSGLFX tree.
#
# No `u` modifier: the SDK's ar defaults to deterministic (D) mode, which
# ignores `u` with a warning, and an empty archive has nothing to update.

CROSS    ?= powerpc64-ps3-elf-
AR       := $(CROSS)ar
PS3DEV   ?= $(error PS3DEV is not set. Source scripts/env.sh first)
PS3DK    ?= $(PS3DEV)/ps3dk

PS3DK_LIB   = $(PS3DK)/ppu/lib
INSTALL_DIR = $(PS3DK_LIB)
TARGET      = libPSGLFX.a

all: $(INSTALL_DIR)/$(TARGET)

$(INSTALL_DIR)/$(TARGET):
	@mkdir -p $(INSTALL_DIR)
	$(AR) crs $@

.PHONY: install
install: $(INSTALL_DIR)/$(TARGET)

clean:
	rm -f $(INSTALL_DIR)/$(TARGET)
