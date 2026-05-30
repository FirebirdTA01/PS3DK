# sdk/libmt19937/Makefile

CROSS      ?= powerpc64-ps3-elf-
PPU_CC     := $(CROSS)gcc
PPU_AR     := $(CROSS)ar
PPU_RANLIB := $(CROSS)ranlib

SPU_CC     := spu-elf-gcc
SPU_AR     := spu-elf-ar
SPU_RANLIB := spu-elf-ranlib

PS3DEV     ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
SPU_PREFIX ?= $(PS3DEV)/spu

SRC        := libmt19937.c

PPU_CFLAGS := -O2 -Wall -Wextra -mcpu=cell -std=c11 -I$(PS3DK)/ppu/include
SPU_CFLAGS := -O2 -Wall -Wextra -std=c11 -I$(PS3DK)/spu/include -I../include
DEPFLAGS   := -MMD -MP
ARFLAGS    := rcs

PPU_BUILD      := build
PPU_LP64_BUILD := build-lp64
SPU_BUILD      := build-spu

PPU_ARCHIVE      := $(PPU_BUILD)/libmt19937.a
PPU_LP64_ARCHIVE := $(PPU_LP64_BUILD)/libmt19937.a
SPU_ARCHIVE      := $(SPU_BUILD)/libmt19937.a

PPU_INSTALL_LIB      := $(PS3DK)/ppu/lib
PPU_INSTALL_LIB_LP64 := $(PPU_INSTALL_LIB)/lp64
SPU_INSTALL_LIB      := $(SPU_PREFIX)/spu-elf/lib
SPU_INSTALL_MIRROR   := $(PS3DK)/spu/lib

.PHONY: all install clean

all: $(PPU_ARCHIVE) $(PPU_LP64_ARCHIVE) $(SPU_ARCHIVE)

$(PPU_ARCHIVE): $(PPU_BUILD)/libmt19937.o
	$(PPU_AR) $(ARFLAGS) $@ $^
	$(PPU_RANLIB) $@

$(PPU_LP64_ARCHIVE): $(PPU_LP64_BUILD)/libmt19937.o
	$(PPU_AR) $(ARFLAGS) $@ $^
	$(PPU_RANLIB) $@

$(SPU_ARCHIVE): $(SPU_BUILD)/libmt19937.o
	$(SPU_AR) $(ARFLAGS) $@ $^
	$(SPU_RANLIB) $@

$(PPU_BUILD)/%.o: %.c | $(PPU_BUILD)
	$(PPU_CC) $(PPU_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(PPU_LP64_BUILD)/%.o: %.c | $(PPU_LP64_BUILD)
	$(PPU_CC) $(PPU_CFLAGS) -mlp64 $(DEPFLAGS) -c -o $@ $<

$(SPU_BUILD)/%.o: %.c | $(SPU_BUILD)
	$(SPU_CC) $(SPU_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(PPU_BUILD) $(PPU_LP64_BUILD) $(SPU_BUILD):
	@mkdir -p $@

install: all
	@install -d $(PPU_INSTALL_LIB) $(PPU_INSTALL_LIB_LP64)
	install -m 0644 $(PPU_ARCHIVE) $(PPU_INSTALL_LIB)/libmt19937.a
	install -m 0644 $(PPU_LP64_ARCHIVE) $(PPU_INSTALL_LIB_LP64)/libmt19937.a
	@install -d $(SPU_INSTALL_LIB) $(SPU_INSTALL_MIRROR)
	install -m 0644 $(SPU_ARCHIVE) $(SPU_INSTALL_LIB)/libmt19937.a
	install -m 0644 $(SPU_ARCHIVE) $(SPU_INSTALL_MIRROR)/libmt19937.a
	@echo "installed libmt19937.a (PPU ILP32 + LP64, SPU)"

clean:
	rm -rf $(PPU_BUILD) $(PPU_LP64_BUILD) $(SPU_BUILD)

-include $(wildcard $(PPU_BUILD)/*.d)
-include $(wildcard $(PPU_LP64_BUILD)/*.d)
-include $(wildcard $(SPU_BUILD)/*.d)
