# sdk/libdbgfont/Makefile
#
# Builds libdbgfont.a — tier-2 on-screen renderer for the cell-SDK
# cellDbgFont* API (see sdk/include/cell/dbgfont.h).  Renders text via
# NV40 using a compiled vertex + fragment shader pair over a 128x128
# 8-bit font atlas.
#
# Pipeline:
#   src/*.c                   ---gcc--------> build/*.o
#   shaders/*.vcg/.fcg        ---rsx-cg-compiler-> build/*.{vpo,fpo}
#   build/*.{vpo,fpo}         ---bin2s------> build/*.s
#   build/*.s                 ---as---------> build/*.o
#   ar rcs libdbgfont.a build/*.o
#
# Requires scripts/env.sh sourced: $PS3DEV, powerpc64-ps3-elf toolchain,
# the PPU SDK headers installed, and rsx-cg-compiler available on $PATH
# (or in $PS3DEV/bin / $PS3DK/bin / tools/rsx-cg-compiler/build).
# Install drops libdbgfont.a into $PS3DK/ppu/lib/.

CROSS    ?= powerpc64-ps3-elf-
CC       := $(CROSS)gcc
AS       := $(CROSS)as
AR       := $(CROSS)ar
RANLIB   := $(CROSS)ranlib

PS3DEV   ?= $(error PS3DEV is not set — run: source scripts/env.sh)

# Locate rsx-cg-compiler.  Prefer a build-tree binary (so a fresh local
# build is picked up without a reinstall), fall back to PS3DEV/PS3DK.
REPO_ROOT  := $(abspath $(CURDIR)/../..)
RSX_CG_BUILD := $(REPO_ROOT)/tools/rsx-cg-compiler/build/rsx-cg-compiler
RSX_CG  := $(firstword \
    $(wildcard $(RSX_CG_BUILD)) \
    $(wildcard $(PS3DEV)/bin/rsx-cg-compiler) \
    $(wildcard $(PS3DK)/bin/rsx-cg-compiler))
ifeq ($(RSX_CG),)
    $(error rsx-cg-compiler not found — build tools/rsx-cg-compiler first)
endif

BIN2S    := $(PS3DEV)/bin/bin2s

CFLAGS   := -O2 -Wall -Wextra -Wno-unused-parameter \
            -mcpu=cell -std=c11 \
            -I$(PS3DK)/ppu/include \
            -Iinclude
# -MMD/-MP emit per-object .d files so editing a shared header correctly
# invalidates the .o.
DEPFLAGS := -MMD -MP
ARFLAGS  := rcs

SRC_DIR     := src
SHADER_DIR  := shaders
OBJ_DIR     := build

C_SRCS      := $(filter-out $(SRC_DIR)/dbgfont_shaders.c,$(wildcard $(SRC_DIR)/*.c))
C_OBJS      := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(C_SRCS))

VP_SRCS     := $(wildcard $(SHADER_DIR)/*.vcg)
FP_SRCS     := $(wildcard $(SHADER_DIR)/*.fcg)
SHADER_OBJS := $(patsubst $(SHADER_DIR)/%.vcg,$(OBJ_DIR)/%.vpo.o,$(VP_SRCS)) \
               $(patsubst $(SHADER_DIR)/%.fcg,$(OBJ_DIR)/%.fpo.o,$(FP_SRCS))

ARCHIVE     := $(OBJ_DIR)/libdbgfont.a
PS3DK       ?= $(PS3DEV)/ps3dk
INSTALL_LIB := $(PS3DK)/ppu/lib

.PHONY: all install clean

all: $(ARCHIVE)

$(ARCHIVE): $(C_OBJS) $(SHADER_OBJS)
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

# Vertex/fragment shader pipeline: .vcg/.fcg → rsx-cg-compiler →
# .vpo/.fpo → bin2s → .s → as → .o.  bin2s names the symbols after the
# input file's basename (`vpshader_dbgfont.vpo` → `vpshader_dbgfont_vpo`,
# `_end`, `_size`), so the rsx-cg-compiler output goes into a file with
# the exact name we need to embed.
$(OBJ_DIR)/%.vpo: $(SHADER_DIR)/%.vcg | $(OBJ_DIR)
	$(RSX_CG) -p sce_vp_rsx --O2 --fastmath --emit-container $@ $<

$(OBJ_DIR)/%.fpo: $(SHADER_DIR)/%.fcg | $(OBJ_DIR)
	$(RSX_CG) -p sce_fp_rsx --O2 --fastmath --emit-container $@ $<

$(OBJ_DIR)/%.vpo.s: $(OBJ_DIR)/%.vpo
	$(BIN2S) $< > $@

$(OBJ_DIR)/%.fpo.s: $(OBJ_DIR)/%.fpo
	$(BIN2S) $< > $@

$(OBJ_DIR)/%.vpo.o: $(OBJ_DIR)/%.vpo.s
	$(AS) -o $@ $<

$(OBJ_DIR)/%.fpo.o: $(OBJ_DIR)/%.fpo.s
	$(AS) -o $@ $<

$(OBJ_DIR):
	@mkdir -p $@

install: $(ARCHIVE)
	@install -d $(INSTALL_LIB)
	install -m 0644 $(ARCHIVE) $(INSTALL_LIB)/libdbgfont.a
	@echo "installed libdbgfont.a into $(INSTALL_LIB)/"

clean:
	rm -rf $(OBJ_DIR)

-include $(wildcard $(OBJ_DIR)/*.d)
