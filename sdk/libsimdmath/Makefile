# sdk/libsimdmath/Makefile
#
# Builds + installs the BSD libsimdmath (from PSL1GHT common/) for both PPU
# (ILP32 + LP64) and SPU.  Also installs the public declarations header
# (simdmath/simdmath.h) and the per-arch static-inline headers
# (simdmath/_name.h) into $(PS3DK)/ppu/include/simdmath/ and
# $(PS3DK)/spu/include/simdmath/ respectively.
#
# This file replaces the vendor's ppu/Makefile + spu/Makefile pair, which
# depended on $(PSL1GHT)/{ppu,spu}_rules that are not part of our tree.
# The cross compilers, install prefix conventions, and per-arch split are
# exactly the same as sdk/liblv2 (PPU ILP32+LP64) and sdk/libspuPSGL
# (SPU + mirror).
#
# The source lists are derived from common/*.c with a PPU-excludes list, so
# adding a function to either arch only requires adding it to common/ -
# no Makefile edit needed.  The split matches the vendor's ppu/Makefile
# (41 PPU functions) and spu/Makefile (119 SPU functions) byte-for-byte.

PS3DEV     ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
CROSS      ?= powerpc64-ps3-elf-
PPU_CC     := $(CROSS)gcc
PPU_AR     := $(CROSS)ar
PPU_RANLIB := $(CROSS)ranlib
SPU_CC     := spu-elf-gcc
SPU_AR     := spu-elf-ar
SPU_RANLIB := spu-elf-ranlib

PPU_INC      := $(PS3DK)/ppu/include
SPU_INC      := $(PS3DK)/spu/include
PPU_LIB      := $(PS3DK)/ppu/lib
PPU_LIB_LP64 := $(PPU_LIB)/lp64
SPU_LIB      := $(PS3DEV)/spu/spu-elf/lib
SPU_LIB_MIR  := $(PS3DK)/spu/lib

COMMON_FLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 -D__PS3DK_SIMDMATH_BUILD
PPU_INC_FLAGS := -Ippu -I. -Icommon -Icommon/simdmath
SPU_INC_FLAGS := -Ispu -I. -Icommon -Icommon/simdmath
PPU_CFLAGS      := $(COMMON_FLAGS) -mcpu=cell $(PPU_INC_FLAGS)
PPU_CFLAGS_LP64 := $(PPU_CFLAGS) -mlp64
SPU_CFLAGS      := $(COMMON_FLAGS) $(SPU_INC_FLAGS)

# SPU: the full common/*.c set (verified to equal the vendor spu/Makefile).
SPU_SRCS := $(wildcard common/*.c)

# PPU: all of common/ except the d2 lane functions and the handful of SPU
# extras the vendor's ppu/Makefile does not include.  (This exclusion list
# is verified byte-identical to `common/*.c minus vendor-ppu-OBJS`.)
PPU_EXCLUDES := \
    ceild2.c copysignd2.c cosd2.c divd2.c divu4.c fabsd2.c fdimd2.c \
    floord2.c fmad2.c fmaxd2.c fmind2.c fmodd2.c fpclassifyd2.c \
    fpclassifyf4.c frexpd2.c hypotd2.c ilogbd2.c irintf4.c iroundf4.c \
    is0denormd2.c is0denormf4.c isequald2.c isequalf4.c isfinited2.c \
    isfinitef4.c isgreaterd2.c isgreaterequald2.c isgreaterequalf4.c \
    isgreaterf4.c isinfd2.c isinff4.c islessd2.c islessequald2.c \
    islessequalf4.c islessf4.c islessgreaterd2.c islessgreaterf4.c \
    isnand2.c isnanf4.c isnormald2.c isnormalf4.c isunorderedd2.c \
    isunorderedf4.c ldexpd2.c llabsi2.c lldivi2.c lldivu2.c llrintd2.c \
    llrintf4.c llroundd2.c llroundf4.c logbd2.c modfd2.c nearbyintd2.c \
    nearbyintf4.c negated2.c negatell2.c nextafterd2.c nextafterf4.c \
    recipd2.c remainderd2.c remquod2.c remquof4.c rintd2.c \
    rintf4.c roundd2.c roundf4.c rsqrtd2.c scalbllnd2.c scalbnf4.c \
    signbitd2.c signbitf4.c sincosd2.c sind2.c sqrtd2.c tand2.c truncd2.c

# Drop the exclusion files from the wildcarded list.
PPU_SRCS := $(filter-out $(addprefix common/,$(PPU_EXCLUDES)),$(SPU_SRCS))

PPU_BUILD      := build-ppu
PPU_BUILD_LP64 := build-ppu-lp64
SPU_BUILD      := build-spu

PPU_OBJS      := $(addprefix $(PPU_BUILD)/,$(notdir $(PPU_SRCS:.c=.o)))
PPU_OBJS_LP64 := $(addprefix $(PPU_BUILD_LP64)/,$(notdir $(PPU_SRCS:.c=.o)))
SPU_OBJS      := $(addprefix $(SPU_BUILD)/,$(notdir $(SPU_SRCS:.c=.o)))
PPU_LIB_OUT      := $(PPU_BUILD)/libsimdmath.a
PPU_LIB_OUT_LP64 := $(PPU_BUILD_LP64)/libsimdmath.a
SPU_LIB_OUT      := $(SPU_BUILD)/libsimdmath.a

.PHONY: all install clean ppu spu

all: ppu spu

ppu: $(PPU_LIB_OUT) $(PPU_LIB_OUT_LP64)
spu: $(SPU_LIB_OUT)

$(PPU_BUILD) $(PPU_BUILD_LP64) $(SPU_BUILD):
	@mkdir -p $@

$(PPU_BUILD)/%.o: common/%.c | $(PPU_BUILD)
	@echo "  CC    ppu      $*"
	@$(PPU_CC) $(PPU_CFLAGS) -c -o $@ $<

$(PPU_BUILD_LP64)/%.o: common/%.c | $(PPU_BUILD_LP64)
	@echo "  CC    ppu-lp64 $*"
	@$(PPU_CC) $(PPU_CFLAGS_LP64) -c -o $@ $<

$(SPU_BUILD)/%.o: common/%.c | $(SPU_BUILD)
	@echo "  CC    spu      $*"
	@$(SPU_CC) $(SPU_CFLAGS) -c -o $@ $<

$(PPU_LIB_OUT): $(PPU_OBJS)
	@echo "  AR    $(PPU_LIB_OUT)"
	@$(PPU_AR) rcs $@ $^
	@$(PPU_RANLIB) $@

$(PPU_LIB_OUT_LP64): $(PPU_OBJS_LP64)
	@echo "  AR    $(PPU_LIB_OUT_LP64)"
	@$(PPU_AR) rcs $@ $^
	@$(PPU_RANLIB) $@

$(SPU_LIB_OUT): $(SPU_OBJS)
	@echo "  AR    $(SPU_LIB_OUT)"
	@$(SPU_AR) rcs $@ $^
	@$(SPU_RANLIB) $@

# ------------------------------------------------------------------
# Install
# ------------------------------------------------------------------
install: all
	@# --- declarations + SPU shared constants header (identical across arches)
	@install -d $(PPU_INC)/simdmath $(SPU_INC)/simdmath
	@install -m 0644 common/simdmath.h     $(PPU_INC)/simdmath/simdmath.h
	@install -m 0644 common/simdmath.h     $(SPU_INC)/simdmath/simdmath.h
	@install -m 0644 common/simdmath/_sincos.h $(PPU_INC)/simdmath/_sincos.h
	@install -m 0644 common/simdmath/_sincos.h $(SPU_INC)/simdmath/_sincos.h
	@# --- PPU per-arch inlines
	@for h in ppu/simdmath/*.h; do install -m 0644 $$h $(PPU_INC)/simdmath/; done
	@# --- SPU per-arch inlines
	@for h in spu/simdmath/*.h; do install -m 0644 $$h $(SPU_INC)/simdmath/; done
	@# --- PPU archives (ILP32 + LP64)
	@install -d $(PPU_LIB) $(PPU_LIB_LP64)
	@install -m 0644 $(PPU_LIB_OUT)      $(PPU_LIB)/libsimdmath.a
	@install -m 0644 $(PPU_LIB_OUT_LP64) $(PPU_LIB_LP64)/libsimdmath.a
	@# --- SPU archive (canonical + mirror, like sdk/libspuPSGL)
	@install -d $(SPU_LIB) $(SPU_LIB_MIR)
	@install -m 0644 $(SPU_LIB_OUT) $(SPU_LIB)/libsimdmath.a
	@install -m 0644 $(SPU_LIB_OUT) $(SPU_LIB_MIR)/libsimdmath.a
	@echo "  simdmath  PPU (ILP32+LP64) + SPU archives + per-arch inline headers"

clean:
	@rm -rf $(PPU_BUILD) $(PPU_BUILD_LP64) $(SPU_BUILD)
