# sdk/libdbgfont/Makefile
#
# Builds libdbgfont.a — tier-2 on-screen renderer for the cell-SDK
# cellDbgFont* API (see sdk/include/cell/dbgfont.h).  Renders text via
# NV40 using a compiled vertex + fragment shader pair over a 128x128
# 8-bit font atlas.
#
# Pipeline:
#   shaders/*.vcg  ---cgcomp -v---> build/*.vpo ---bin2s---> build/*.vpo.o
#   shaders/*.fcg  ---cgcomp -f---> build/*.fpo --.    ^
#                                                 '---> (via $(AS))
#   src/*.c        ---gcc--------> build/*.o
#   ar rcs libdbgfont.a build/*.o build/*.vpo.o build/*.fpo.o
#
# Requires scripts/env.sh sourced: $PS3DEV, powerpc64-ps3-elf toolchain,
# cgcomp, and bin2s on PATH.  Install drops libdbgfont.a into
# $PS3DEV/psl1ght/ppu/lib/.

CROSS    ?= powerpc64-ps3-elf-
CC       := $(CROSS)gcc
AS       := $(CROSS)as
AR       := $(CROSS)ar
RANLIB   := $(CROSS)ranlib

PS3DEV   ?= $(error PS3DEV is not set — run: source scripts/env.sh)

CGCOMP   := $(PS3DEV)/bin/cgcomp
BIN2S    := $(PS3DEV)/bin/bin2s

CFLAGS   := -O2 -Wall -Wextra -Wno-unused-parameter \
            -mcpu=cell -std=c11 \
            -I$(PS3DK)/ppu/include \
            -I$(PS3DEV)/psl1ght/ppu/include \
            -Iinclude
# -MMD/-MP emit per-object .d files so editing a shared header correctly
# invalidates the .o.
DEPFLAGS := -MMD -MP
ARFLAGS  := rcs

SRC_DIR     := src
SHADER_DIR  := shaders
OBJ_DIR     := build

C_SRCS      := $(wildcard $(SRC_DIR)/*.c)
C_OBJS      := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(C_SRCS))

VPO_SRCS    := $(wildcard $(SHADER_DIR)/*.vcg)
FPO_SRCS    := $(wildcard $(SHADER_DIR)/*.fcg)
VPO_OBJS    := $(patsubst $(SHADER_DIR)/%.vcg,$(OBJ_DIR)/%.vpo.o,$(VPO_SRCS))
FPO_OBJS    := $(patsubst $(SHADER_DIR)/%.fcg,$(OBJ_DIR)/%.fpo.o,$(FPO_SRCS))

ARCHIVE     := $(OBJ_DIR)/libdbgfont.a
PS3DK       ?= $(PS3DEV)/ps3dk
INSTALL_LIB := $(PS3DK)/ppu/lib

.PHONY: all install clean

all: $(ARCHIVE)

$(ARCHIVE): $(C_OBJS) $(VPO_OBJS) $(FPO_OBJS)
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(OBJ_DIR)/%.vpo: $(SHADER_DIR)/%.vcg | $(OBJ_DIR)
	$(CGCOMP) -v $< $@

$(OBJ_DIR)/%.fpo: $(SHADER_DIR)/%.fcg | $(OBJ_DIR)
	$(CGCOMP) -f $< $@

$(OBJ_DIR)/%.vpo.o: $(OBJ_DIR)/%.vpo
	$(BIN2S) -a 64 $< | $(AS) -o $@

$(OBJ_DIR)/%.fpo.o: $(OBJ_DIR)/%.fpo
	$(BIN2S) -a 64 $< | $(AS) -o $@

$(OBJ_DIR):
	@mkdir -p $@

install: $(ARCHIVE)
	@install -d $(INSTALL_LIB)
	install -m 0644 $(ARCHIVE) $(INSTALL_LIB)/libdbgfont.a
	@echo "installed libdbgfont.a into $(INSTALL_LIB)/"

clean:
	rm -rf $(OBJ_DIR)

-include $(wildcard $(OBJ_DIR)/*.d)
