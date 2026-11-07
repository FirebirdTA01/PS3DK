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

# The SPU-lane + GPU-domain extension (P3.3 R4b): ILP32 only, NOT part of all or install.  Its own build directory and
# rules; the shipped archives above are untouched.  Compile/link gating only: no target behaviour follows from it.
include ext-members.mk
EXT_DIR := $(BUILD)/ilp32-ext
EXT_OBJS := $(addprefix $(EXT_DIR)/,$(addsuffix .o,$(EXT_MEMBERS)))
EXT_FLAGS := $(PPU_FLAGS) -DPS3DK_PROF_WITH_SPU -Isrc/core -Isrc/pal -Isrc/spu -Isrc/rsx

.PHONY: ext-ilp32 ext-clean
ext-ilp32: $(EXT_DIR)/libps3dkprof_ext.a
$(EXT_DIR)/%.o: src/core/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXT_FLAGS) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXT_DIR)/%.o: src/pal/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXT_FLAGS) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXT_DIR)/%.o: src/spu/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXT_FLAGS) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXT_DIR)/%.o: src/rsx/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXT_FLAGS) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXT_DIR)/libps3dkprof_ext.a: $(EXT_OBJS)
	@rm -f "$@"
	$(PPU_AR) rcs "$@" $^
ext-clean:
	rm -f $(EXT_OBJS) $(EXT_OBJS:.o=.d) "$(EXT_DIR)/libps3dkprof_ext.a"

# The PROFILE variant of the extension archive (profile-format-spec.md, gate B2p draft): OPT-IN and separate, so the
# reviewed ext archive above and its member contract (tests/host/ps3dk-prof-ext-target) are unchanged.  The same
# members compiled with PS3DK_PROF_WITH_PROFILE, plus the generated identity member prof_identity.o, which holds the
# ARCHIVE-DIGEST-V1 of every other member (tools/archive_digest.py) and is excluded from its own digest; the build
# re-verifies the finished archive, digest AND declared identity, before it replaces the archive.  The declared
# identity (PROF_PROFILE_ID, PROF_PREMISES, PROF_KINDS) is a build input through a config file rewritten only when a
# value changes, as $(CONFIG) is for the version: a changed value rebuilds the identity member and the archive (the
# other members stay), an unchanged one leaves the archive up to date.
PROF_PROFILE_ID ?= RSX-R1
PROF_PREMISES ?= 463
PROF_KINDS ?= 1
EXTP_DIR := $(BUILD)/ilp32-ext-profile
EXTP_OBJS := $(addprefix $(EXTP_DIR)/,$(addsuffix .o,$(EXT_MEMBERS)))
EXTP_FLAGS := $(EXT_FLAGS) -DPS3DK_PROF_WITH_PROFILE
EXTP_ID_CONFIG := $(EXTP_DIR)/profile-identity.cfg
.PHONY: ext-ilp32-profile
ext-ilp32-profile: $(EXTP_DIR)/libps3dkprof_ext_profile.a
$(EXTP_ID_CONFIG): FORCE
	@mkdir -p "$(@D)"
	@printf 'id=%s\npremises=%s\nkinds=%s\n' '$(PROF_PROFILE_ID)' '$(PROF_PREMISES)' '$(PROF_KINDS)' > "$@.tmp"
	@cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"
	@rm -f "$@.tmp"
$(EXTP_DIR)/%.o: src/core/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXTP_FLAGS) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXTP_DIR)/%.o: src/pal/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXTP_FLAGS) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXTP_DIR)/%.o: src/spu/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXTP_FLAGS) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXTP_DIR)/%.o: src/rsx/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXTP_FLAGS) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXTP_DIR)/libps3dkprof_ext_profile.a: $(EXTP_OBJS) $(EXTP_ID_CONFIG) tools/archive_digest.py src/core/prof_identity.h
	@rm -f "$@" "$@.tmp" "$(EXTP_DIR)/members.a" "$(EXTP_DIR)/prof_identity.c" "$(EXTP_DIR)/prof_identity.o"
	$(PPU_AR) rcsD "$(EXTP_DIR)/members.a" $(EXTP_OBJS)
	python3 tools/archive_digest.py emit "$(EXTP_DIR)/members.a" --id "$(PROF_PROFILE_ID)" \
	    --premises "$(PROF_PREMISES)" --kinds "$(PROF_KINDS)" > "$(EXTP_DIR)/prof_identity.c"
	$(PPU_CC) $(EXTP_FLAGS) -c "$(EXTP_DIR)/prof_identity.c" -o "$(EXTP_DIR)/prof_identity.o"
	$(PPU_AR) rcsD "$@.tmp" $(EXTP_OBJS) "$(EXTP_DIR)/prof_identity.o"
	python3 tools/archive_digest.py verify "$@.tmp" --id "$(PROF_PROFILE_ID)" --premises "$(PROF_PREMISES)" \
	    --kinds "$(PROF_KINDS)"
	mv -f "$@.tmp" "$@"
-include $(EXTP_OBJS:.o=.d)

-include $(EXT_OBJS:.o=.d)
