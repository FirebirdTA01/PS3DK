# sdk/libovis/Makefile -- SPU code overlays (cell/ovis.h).
#
# Outputs:
#   build-spu/libovis.a  -> $(SPU_PREFIX)/spu-elf/lib/ and $(PS3DK)/spu/lib/
#                           the SPU mapper: _cellOvisStartMapping,
#                           _cellOvisWaitMapping, _cellOvisUpdateAndMapSection,
#                           _ovly_debug_event, the auto-mapping state
#   build/libovis.a      -> $(PS3DK)/ppu/lib/        (and lp64/)
#                           the PPU side: overlay table from an SPU ELF, and
#                           dropping overlay segments from an SPU image.
#                           Plain code over the ELF and the image, so it
#                           needs no system module; libovis_stub.a (the
#                           module's imports) stays available beside it.

CROSS      ?= powerpc64-ps3-elf-
PPU_CC     := $(CROSS)gcc
PPU_AR     := $(CROSS)ar
SPU_CC     := spu-elf-gcc
SPU_AR     := spu-elf-ar

PS3DEV     ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
SPU_PREFIX ?= $(PS3DEV)/spu

PPU_CFLAGS := -O2 -Wall -Wextra -mcpu=cell -std=gnu99 -I$(PS3DK)/ppu/include
SPU_CFLAGS := -O2 -Wall -Wextra -std=gnu99 -ffreestanding \
              -I../include-spu -I$(PS3DK)/spu/include -idirafter ../include

PPU_SRCS := ovis_table ovis_segments
SPU_C    := mapper auto
SPU_S    := debug_event update_and_map

PPU_OBJS      := $(addprefix build/,$(addsuffix .o,$(PPU_SRCS)))
PPU_LP64_OBJS := $(addprefix build-lp64/,$(addsuffix .o,$(PPU_SRCS)))
SPU_OBJS      := $(addprefix build-spu/,$(addsuffix .o,$(SPU_C) $(SPU_S)))

.PHONY: all install clean

all: build/libovis.a build-lp64/libovis.a build-spu/libovis.a

build/libovis.a: $(PPU_OBJS)
	@rm -f $@; $(PPU_AR) rcs $@ $^; echo "  AR    ppu $(notdir $@)"
build-lp64/libovis.a: $(PPU_LP64_OBJS)
	@rm -f $@; $(PPU_AR) rcs $@ $^; echo "  AR    ppu lp64 $(notdir $@)"
build-spu/libovis.a: $(SPU_OBJS)
	@rm -f $@; $(SPU_AR) rcs $@ $^; echo "  AR    spu $(notdir $@)"

build/%.o: src/ppu/%.c | build
	@echo "  CC    ppu $(notdir $<)"; $(PPU_CC) $(PPU_CFLAGS) -c $< -o $@
build-lp64/%.o: src/ppu/%.c | build-lp64
	@echo "  CC    ppu lp64 $(notdir $<)"; $(PPU_CC) $(PPU_CFLAGS) -mlp64 -c $< -o $@
build-spu/%.o: src/spu/%.c | build-spu
	@echo "  CC    spu $(notdir $<)"; $(SPU_CC) $(SPU_CFLAGS) -c $< -o $@
build-spu/%.o: src/spu/%.S | build-spu
	@echo "  AS    spu $(notdir $<)"; $(SPU_CC) -c $< -o $@

build build-lp64 build-spu:
	@mkdir -p $@

install: all
	@install -d $(PS3DK)/ppu/lib/lp64 $(SPU_PREFIX)/spu-elf/lib $(PS3DK)/spu/lib
	@install -m 0644 build/libovis.a $(PS3DK)/ppu/lib/libovis.a
	@install -m 0644 build-lp64/libovis.a $(PS3DK)/ppu/lib/lp64/libovis.a
	@install -m 0644 build-spu/libovis.a $(SPU_PREFIX)/spu-elf/lib/libovis.a
	@install -m 0644 build-spu/libovis.a $(PS3DK)/spu/lib/libovis.a
	@echo "  lib    libovis.a (ppu, ppu lp64, spu)"

clean:
	@rm -rf build build-lp64 build-spu
