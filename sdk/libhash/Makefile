# sdk/libhash/Makefile -- PPU message-digest libraries (<cell/hash/*.h>).
#
# Outputs, ILP32 and LP64:
#   libmd5.a libsha1.a libsha224.a libsha256.a libsha384.a libsha512.a
#     -> $(PS3DK)/ppu/lib/ and $(PS3DK)/ppu/lib/lp64/
#
# One archive per algorithm, as programs link them.  SHA-224 and SHA-384
# compile their own copy of the SHA-256 / SHA-512 compression function
# (sha256_core.h, sha512_core.h), so no archive needs another.

CROSS      ?= powerpc64-ps3-elf-
PPU_CC     := $(CROSS)gcc
PPU_AR     := $(CROSS)ar
PPU_RANLIB := $(CROSS)ranlib

PS3DEV     ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk

ALGOS      := md5 sha1 sha224 sha256 sha384 sha512

PPU_CFLAGS := -O2 -Wall -Wextra -mcpu=cell -std=gnu99 -ffunction-sections -fdata-sections \
              -I../include -Isrc
DEPFLAGS   := -MMD -MP
ARFLAGS    := rcs

PPU_BUILD      := build
PPU_LP64_BUILD := build-lp64

PPU_ARCHIVES      := $(foreach a,$(ALGOS),$(PPU_BUILD)/lib$(a).a)
PPU_LP64_ARCHIVES := $(foreach a,$(ALGOS),$(PPU_LP64_BUILD)/lib$(a).a)

PPU_INSTALL_LIB      := $(PS3DK)/ppu/lib
PPU_INSTALL_LIB_LP64 := $(PPU_INSTALL_LIB)/lp64

.PHONY: all install clean

all: $(PPU_ARCHIVES) $(PPU_LP64_ARCHIVES)

$(PPU_BUILD)/lib%.a: $(PPU_BUILD)/%.o
	$(PPU_AR) $(ARFLAGS) $@ $^
	$(PPU_RANLIB) $@

$(PPU_LP64_BUILD)/lib%.a: $(PPU_LP64_BUILD)/%.o
	$(PPU_AR) $(ARFLAGS) $@ $^
	$(PPU_RANLIB) $@

$(PPU_BUILD)/%.o: src/%.c | $(PPU_BUILD)
	$(PPU_CC) $(PPU_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(PPU_LP64_BUILD)/%.o: src/%.c | $(PPU_LP64_BUILD)
	$(PPU_CC) $(PPU_CFLAGS) -mlp64 $(DEPFLAGS) -c -o $@ $<

$(PPU_BUILD) $(PPU_LP64_BUILD):
	@mkdir -p $@

install: all
	@install -d $(PPU_INSTALL_LIB) $(PPU_INSTALL_LIB_LP64)
	@for a in $(ALGOS); do \
		install -m 0644 $(PPU_BUILD)/lib$$a.a $(PPU_INSTALL_LIB)/lib$$a.a || exit 1; \
		install -m 0644 $(PPU_LP64_BUILD)/lib$$a.a $(PPU_INSTALL_LIB_LP64)/lib$$a.a || exit 1; \
	done
	@echo "installed libmd5/libsha1/libsha224/libsha256/libsha384/libsha512 (PPU ILP32 + LP64)"

clean:
	rm -rf $(PPU_BUILD) $(PPU_LP64_BUILD)

.SECONDARY:

-include $(wildcard $(PPU_BUILD)/*.d)
-include $(wildcard $(PPU_LP64_BUILD)/*.d)
