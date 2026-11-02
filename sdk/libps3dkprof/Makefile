# PPU profiler archives, both ABIs. Manual users link ps3dkprof; automatic
# lifecycle users link ps3prof and force __ps3dk_prof_startup_anchor. Compiler
# adapters are a separate integration step; neither archive contains dummy hooks.
# Manual named zones use only the timing format, so their archive omits the
# automatic mode's separate arc-count companion serializer.
CROSS ?= powerpc64-ps3-elf-
PPU_CC := $(CROSS)gcc
PPU_AR := $(CROSS)ar
PS3DEV ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK ?= $(PS3DEV)/ps3dk
PS3_BUILD_ROOT ?= $(HOME)/ps3tc/build
BUILD := $(PS3_BUILD_ROOT)/sdk/libps3dkprof
VERSION_SH := $(abspath ../../scripts/version.sh)
SDK_VERSION := $(shell "$(VERSION_SH)" --format=plain)
ifeq ($(strip $(SDK_VERSION)),)
$(error cannot determine the SDK version)
endif

# Deliberately do not inherit application CFLAGS/CPPFLAGS: the runtime, PAL,
# serializer and startup must never acquire -pg or -finstrument-functions.
PPU_FLAGS := -O2 -Wall -Wextra -Werror -Wno-prio-ctor-dtor -std=c11 -mcpu=cell \
             -ffunction-sections -fdata-sections -fno-instrument-functions \
             -DPS3DK_PROF=1 -DPS3DK_PROF_PAL_LV2 -I../include -I"$(PS3DK)/ppu/include"
CONFIG := $(BUILD)/config.h
CORE := prof_core prof_drain pal_lv2
# The live link (ps3dk_prof_live_start/stop) is manual-lifecycle only: separate members, pulled in by a reference.
LIVE := live_agent live_os_lv2 live_api
MANUAL := $(CORE) $(LIVE)
AUTO := $(CORE) prof_arcs_writer startup
ILP32_CORE := $(addprefix $(BUILD)/ilp32/,$(addsuffix .o,$(MANUAL)))
LP64_CORE := $(addprefix $(BUILD)/lp64/,$(addsuffix .o,$(MANUAL)))
ILP32_LIVE := $(addprefix $(BUILD)/ilp32/,$(addsuffix .o,$(LIVE)))
LP64_LIVE := $(addprefix $(BUILD)/lp64/,$(addsuffix .o,$(LIVE)))
ILP32_AUTO := $(addprefix $(BUILD)/ilp32/,$(addsuffix .o,$(AUTO)))
LP64_AUTO := $(addprefix $(BUILD)/lp64/,$(addsuffix .o,$(AUTO)))
LIBS := $(foreach abi,ilp32 lp64,$(BUILD)/$(abi)/libps3dkprof.a $(BUILD)/$(abi)/libps3prof.a)

.PHONY: all install clean FORCE
all: $(LIBS)

# A version change rebuilds the objects, but an unchanged version leaves them
# alone. The config file also pins the SDK identity embedded in each capture.
$(CONFIG): FORCE
	@mkdir -p "$(BUILD)"
	@printf '#define PS3DK_PROF_SDK_VERSION "%s"\n' '$(SDK_VERSION)' > "$@.tmp"
	@cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"
	@rm -f "$@.tmp"

vpath %.c src/core src/pal src/auto src/live
$(ILP32_LIVE) $(LP64_LIVE): PPU_FLAGS += -Isrc/core -Isrc/pal -Isrc/live
$(BUILD)/ilp32/%.o: %.c $(CONFIG) Makefile
	@mkdir -p "$(@D)"
	$(PPU_CC) $(PPU_FLAGS) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(BUILD)/lp64/%.o: %.c $(CONFIG) Makefile
	@mkdir -p "$(@D)"
	$(PPU_CC) $(PPU_FLAGS) -mlp64 -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"

$(BUILD)/ilp32/libps3dkprof.a: $(ILP32_CORE)
	@rm -f "$@"
	$(PPU_AR) rcs "$@" $^
$(BUILD)/lp64/libps3dkprof.a: $(LP64_CORE)
	@rm -f "$@"
	$(PPU_AR) rcs "$@" $^
$(BUILD)/ilp32/libps3prof.a: $(ILP32_AUTO)
	@rm -f "$@"
	$(PPU_AR) rcs "$@" $^
$(BUILD)/lp64/libps3prof.a: $(LP64_AUTO)
	@rm -f "$@"
	$(PPU_AR) rcs "$@" $^

install: all
	install -d "$(PS3DK)/ppu/lib" "$(PS3DK)/ppu/lib/lp64"
	install -m 0644 "$(BUILD)/ilp32/libps3dkprof.a" "$(BUILD)/ilp32/libps3prof.a" "$(PS3DK)/ppu/lib/"
	install -m 0644 "$(BUILD)/lp64/libps3dkprof.a" "$(BUILD)/lp64/libps3prof.a" "$(PS3DK)/ppu/lib/lp64/"

clean:
	rm -f $(ILP32_AUTO) $(LP64_AUTO) $(ILP32_LIVE) $(LP64_LIVE) $(ILP32_AUTO:.o=.d) $(LP64_AUTO:.o=.d) 	      $(ILP32_LIVE:.o=.d) $(LP64_LIVE:.o=.d) $(LIBS) "$(CONFIG)" "$(CONFIG).tmp"

-include $(ILP32_AUTO:.o=.d) $(LP64_AUTO:.o=.d) $(ILP32_LIVE:.o=.d) $(LP64_LIVE:.o=.d)
