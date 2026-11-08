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
CORE := prof_core prof_drain prof_version pal_lv2
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

# all and install cover BOTH the shipped archives (the shipped / install-shipped goals, whose plan is unchanged) and the
# release form of the SPU/RSX extension for both data models (ext-release / install-ext, defined below).
.PHONY: all install shipped install-shipped clean FORCE
all: shipped ext-release
shipped: $(LIBS)

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

install: install-shipped install-ext
install-shipped: shipped
	install -d "$(PS3DK)/ppu/lib" "$(PS3DK)/ppu/lib/lp64"
	install -m 0644 "$(BUILD)/ilp32/libps3dkprof.a" "$(BUILD)/ilp32/libps3prof.a" "$(PS3DK)/ppu/lib/"
	install -m 0644 "$(BUILD)/lp64/libps3dkprof.a" "$(BUILD)/lp64/libps3prof.a" "$(PS3DK)/ppu/lib/lp64/"

clean:
	rm -f $(ILP32_AUTO) $(LP64_AUTO) $(ILP32_LIVE) $(LP64_LIVE) $(ILP32_AUTO:.o=.d) $(LP64_AUTO:.o=.d) 	      $(ILP32_LIVE:.o=.d) $(LP64_LIVE:.o=.d) $(LIBS) "$(CONFIG)" "$(CONFIG).tmp"

-include $(ILP32_AUTO:.o=.d) $(LP64_AUTO:.o=.d) $(ILP32_LIVE:.o=.d) $(LP64_LIVE:.o=.d)

# The SPU-lane + GPU-domain extension (P3.3 R4b): one archive per PPU data model.  Its own build directories and
# rules; the shipped archives above are untouched.  all and install take its PROFILE form (ext-release, below).  The data-model flag lives in one per-ABI
# variable (EXT_FLAGS_<abi>) that every extension recipe derives from; the ILP32 rules expand exactly as before the
# LP64 twin existed (EXT_FLAGS_ilp32 is EXT_FLAGS), and the LP64 ones add -mlp64.  Compile/link gating only: no target
# behaviour follows from it.
include ext-members.mk
EXT_DIR := $(BUILD)/ilp32-ext
EXT_OBJS := $(addprefix $(EXT_DIR)/,$(addsuffix .o,$(EXT_MEMBERS)))
EXT_FLAGS := $(PPU_FLAGS) -DPS3DK_PROF_WITH_SPU -Isrc/core -Isrc/pal -Isrc/spu -Isrc/rsx
EXT_FLAGS_ilp32 := $(EXT_FLAGS)
EXT_FLAGS_lp64 := $(EXT_FLAGS) -mlp64
EXT64_DIR := $(BUILD)/lp64-ext
EXT64_OBJS := $(addprefix $(EXT64_DIR)/,$(addsuffix .o,$(EXT_MEMBERS)))

.PHONY: ext-ilp32 ext-lp64 ext-clean
ext-ilp32: $(EXT_DIR)/libps3dkprof_ext.a
$(EXT_DIR)/%.o: src/core/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXT_FLAGS_ilp32) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXT_DIR)/%.o: src/pal/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXT_FLAGS_ilp32) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXT_DIR)/%.o: src/spu/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXT_FLAGS_ilp32) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXT_DIR)/%.o: src/rsx/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXT_FLAGS_ilp32) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXT_DIR)/libps3dkprof_ext.a: $(EXT_OBJS)
	@rm -f "$@"
	$(PPU_AR) rcs "$@" $^
ext-lp64: $(EXT64_DIR)/libps3dkprof_ext.a
$(EXT64_DIR)/%.o: src/core/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXT_FLAGS_lp64) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXT64_DIR)/%.o: src/pal/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXT_FLAGS_lp64) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXT64_DIR)/%.o: src/spu/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXT_FLAGS_lp64) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXT64_DIR)/%.o: src/rsx/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXT_FLAGS_lp64) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXT64_DIR)/libps3dkprof_ext.a: $(EXT64_OBJS)
	@rm -f "$@"
	$(PPU_AR) rcs "$@" $^

# The live link members (live_agent live_os_lv2 live_api) for an extension app: compiled with the extension recipe
# of the same data model plus -Isrc/live (a recipe-consistency contract: the live members do not include
# prof_internal.h), archived separately as libps3dkprof_ext_live.a beside that ABI's ext archive.  An app links both
# (the live archive first); no global symbol may be defined in both (tests/host/ps3dk-prof-ext-target/r4b_live.py).
EXTL_DIR := $(BUILD)/ilp32-ext-live
EXTL_OBJS := $(addprefix $(EXTL_DIR)/,$(addsuffix .o,$(LIVE)))
EXTL64_DIR := $(BUILD)/lp64-ext-live
EXTL64_OBJS := $(addprefix $(EXTL64_DIR)/,$(addsuffix .o,$(LIVE)))
.PHONY: ext-live-ilp32 ext-live-lp64
ext-live-ilp32: $(EXTL_DIR)/libps3dkprof_ext_live.a
ext-live-lp64: $(EXTL64_DIR)/libps3dkprof_ext_live.a
$(EXTL_DIR)/%.o: src/live/%.c $(CONFIG) Makefile
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXT_FLAGS_ilp32) -Isrc/live -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXTL_DIR)/libps3dkprof_ext_live.a: $(EXTL_OBJS)
	@rm -f "$@"
	$(PPU_AR) rcs "$@" $^
$(EXTL64_DIR)/%.o: src/live/%.c $(CONFIG) Makefile
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXT_FLAGS_lp64) -Isrc/live -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXTL64_DIR)/libps3dkprof_ext_live.a: $(EXTL64_OBJS)
	@rm -f "$@"
	$(PPU_AR) rcs "$@" $^

ext-clean:
	rm -f $(EXT_OBJS) $(EXT_OBJS:.o=.d) "$(EXT_DIR)/libps3dkprof_ext.a" $(EXT64_OBJS) $(EXT64_OBJS:.o=.d) \
	      "$(EXT64_DIR)/libps3dkprof_ext.a" $(EXTL_OBJS) $(EXTL_OBJS:.o=.d) "$(EXTL_DIR)/libps3dkprof_ext_live.a" \
	      $(EXTL64_OBJS) $(EXTL64_OBJS:.o=.d) "$(EXTL64_DIR)/libps3dkprof_ext_live.a"

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
EXTP_FLAGS := $(EXT_FLAGS_ilp32) -DPS3DK_PROF_WITH_PROFILE
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

# The LP64 twin of ext-ilp32-profile: the same members and identity, compiled with EXT_FLAGS_lp64, in its own build
# directory with its own identity config.  The ILP32 rules above are unchanged.
EXTP64_DIR := $(BUILD)/lp64-ext-profile
EXTP64_OBJS := $(addprefix $(EXTP64_DIR)/,$(addsuffix .o,$(EXT_MEMBERS)))
EXTP64_FLAGS := $(EXT_FLAGS_lp64) -DPS3DK_PROF_WITH_PROFILE
EXTP64_ID_CONFIG := $(EXTP64_DIR)/profile-identity.cfg
.PHONY: ext-lp64-profile
ext-lp64-profile: $(EXTP64_DIR)/libps3dkprof_ext_profile.a
$(EXTP64_ID_CONFIG): FORCE
	@mkdir -p "$(@D)"
	@printf 'id=%s\npremises=%s\nkinds=%s\n' '$(PROF_PROFILE_ID)' '$(PROF_PREMISES)' '$(PROF_KINDS)' > "$@.tmp"
	@cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"
	@rm -f "$@.tmp"
$(EXTP64_DIR)/%.o: src/core/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXTP64_FLAGS) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXTP64_DIR)/%.o: src/pal/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXTP64_FLAGS) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXTP64_DIR)/%.o: src/spu/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXTP64_FLAGS) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXTP64_DIR)/%.o: src/rsx/%.c $(CONFIG) Makefile ext-members.mk
	@mkdir -p "$(@D)"
	$(PPU_CC) $(EXTP64_FLAGS) -include "$(CONFIG)" -MMD -MP -c "$<" -o "$@"
$(EXTP64_DIR)/libps3dkprof_ext_profile.a: $(EXTP64_OBJS) $(EXTP64_ID_CONFIG) tools/archive_digest.py src/core/prof_identity.h
	@rm -f "$@" "$@.tmp" "$(EXTP64_DIR)/members.a" "$(EXTP64_DIR)/prof_identity.c" "$(EXTP64_DIR)/prof_identity.o"
	$(PPU_AR) rcsD "$(EXTP64_DIR)/members.a" $(EXTP64_OBJS)
	python3 tools/archive_digest.py emit "$(EXTP64_DIR)/members.a" --id "$(PROF_PROFILE_ID)" \
	    --premises "$(PROF_PREMISES)" --kinds "$(PROF_KINDS)" > "$(EXTP64_DIR)/prof_identity.c"
	$(PPU_CC) $(EXTP64_FLAGS) -c "$(EXTP64_DIR)/prof_identity.c" -o "$(EXTP64_DIR)/prof_identity.o"
	$(PPU_AR) rcsD "$@.tmp" $(EXTP64_OBJS) "$(EXTP64_DIR)/prof_identity.o"
	python3 tools/archive_digest.py verify "$@.tmp" --id "$(PROF_PROFILE_ID)" --premises "$(PROF_PREMISES)" \
	    --kinds "$(PROF_KINDS)"
	mv -f "$@.tmp" "$@"
-include $(EXTP64_OBJS:.o=.d)

# The release form of the extension, built by all and installed by install, for BOTH data models (director decision
# D1).  Per ABI: the PROFILE-identity archive, installed as libps3dkprof_ext.a beside that ABI's libps3dkprof.a
# (ppu/lib, ppu/lib/lp64), and the live members as libps3dkprof_ext_live.a; the plain ext-ilp32 / ext-lp64 archives
# stay build-only.  An app links the ext archive INSTEAD of ps3dkprof (the live archive first, with the live link).
# The public headers go to ppu/include/ps3dk/: prof_spu.h, the psp_desc.h it includes, and prof_gpu.h.  The install
# re-verifies each INSTALLED profile archive (digest and declared identity) and compares every installed file with
# its build output, so a partial or substituted install fails here.
EXT_PUB_HEADERS := src/spu/prof_spu.h src/spu/psp_desc.h src/rsx/prof_gpu.h
.PHONY: ext-release install-ext verify-install-ext
ext-release: ext-ilp32-profile ext-lp64-profile ext-live-ilp32 ext-live-lp64
install-ext: ext-release
	install -d "$(PS3DK)/ppu/lib" "$(PS3DK)/ppu/lib/lp64" "$(PS3DK)/ppu/include/ps3dk"
	install -m 0644 "$(EXTP_DIR)/libps3dkprof_ext_profile.a" "$(PS3DK)/ppu/lib/libps3dkprof_ext.a"
	install -m 0644 "$(EXTL_DIR)/libps3dkprof_ext_live.a" "$(PS3DK)/ppu/lib/libps3dkprof_ext_live.a"
	install -m 0644 "$(EXTP64_DIR)/libps3dkprof_ext_profile.a" "$(PS3DK)/ppu/lib/lp64/libps3dkprof_ext.a"
	install -m 0644 "$(EXTL64_DIR)/libps3dkprof_ext_live.a" "$(PS3DK)/ppu/lib/lp64/libps3dkprof_ext_live.a"
	install -m 0644 $(EXT_PUB_HEADERS) "$(PS3DK)/ppu/include/ps3dk/"
	python3 tools/archive_digest.py verify "$(PS3DK)/ppu/lib/libps3dkprof_ext.a" --id "$(PROF_PROFILE_ID)" \
	    --premises "$(PROF_PREMISES)" --kinds "$(PROF_KINDS)"
	python3 tools/archive_digest.py verify "$(PS3DK)/ppu/lib/lp64/libps3dkprof_ext.a" --id "$(PROF_PROFILE_ID)" \
	    --premises "$(PROF_PREMISES)" --kinds "$(PROF_KINDS)"
	cmp "$(EXTP_DIR)/libps3dkprof_ext_profile.a" "$(PS3DK)/ppu/lib/libps3dkprof_ext.a"
	cmp "$(EXTL_DIR)/libps3dkprof_ext_live.a" "$(PS3DK)/ppu/lib/libps3dkprof_ext_live.a"
	cmp "$(EXTP64_DIR)/libps3dkprof_ext_profile.a" "$(PS3DK)/ppu/lib/lp64/libps3dkprof_ext.a"
	cmp "$(EXTL64_DIR)/libps3dkprof_ext_live.a" "$(PS3DK)/ppu/lib/lp64/libps3dkprof_ext_live.a"
	for h in $(EXT_PUB_HEADERS); do cmp "$$h" "$(PS3DK)/ppu/include/ps3dk/$${h##*/}" || exit 1; done
# The installed tree alone, without the build directory (scripts/build-sdk.sh runs it after the SDK install): every
# installed extension file is present and each installed profile archive verifies with the declared identity.
verify-install-ext:
	for f in ppu/lib/libps3dkprof_ext.a ppu/lib/libps3dkprof_ext_live.a ppu/lib/lp64/libps3dkprof_ext.a \
	    ppu/lib/lp64/libps3dkprof_ext_live.a $(addprefix ppu/include/ps3dk/,$(notdir $(EXT_PUB_HEADERS))); do \
	    test -f "$(PS3DK)/$$f" || { echo "missing $(PS3DK)/$$f"; exit 1; }; done
	python3 tools/archive_digest.py verify "$(PS3DK)/ppu/lib/libps3dkprof_ext.a" --id "$(PROF_PROFILE_ID)" \
	    --premises "$(PROF_PREMISES)" --kinds "$(PROF_KINDS)"
	python3 tools/archive_digest.py verify "$(PS3DK)/ppu/lib/lp64/libps3dkprof_ext.a" --id "$(PROF_PROFILE_ID)" \
	    --premises "$(PROF_PREMISES)" --kinds "$(PROF_KINDS)"

-include $(EXT_OBJS:.o=.d) $(EXT64_OBJS:.o=.d) $(EXTL_OBJS:.o=.d) $(EXTL64_OBJS:.o=.d)
