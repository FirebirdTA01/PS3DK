# Explicit EXT + FIFO + AUTO provider. Default/archive lifecycle recipes stay
# independent; application instrumentation flags never reach these objects.
AUTORSX_MEMBERS := auto_runtime auto_startup auto_config auto_wire auto_writer auto_queue \
                   auto_commands auto_completion auto_frames auto_clock
AUTORSX_NAMES := $(EXT_MEMBERS) $(FIFO_MEMBERS) $(AUTORSX_MEMBERS)
AUTORSX_DIR := $(BUILD)/auto-rsx
AUTO_PROFILE_ID ?= AUTO-RSX
AUTO_SOURCE_COMMIT ?= $(shell git rev-parse HEAD)
vpath %.c src/auto_rsx

.PHONY: auto-rsx install-auto-rsx install-auto-package
define AUTORSX_VARIANT
AUTORSX_DIR_$(1) := $$(AUTORSX_DIR)/$(1)
AUTORSX_OBJECTS_$(1) := $$(addprefix $$(AUTORSX_DIR_$(1))/,$$(addsuffix .o,$$(AUTORSX_NAMES)))
AUTORSX_FLAGS_$(1) := $$(EXT_FLAGS_$(1)) -DPS3DK_PROF_WITH_PROFILE -DPS3DK_PROF_WITH_FIFO=1 \
                      -DPS3DK_PROF_WITH_AUTO=1 -Isrc/fifo -Isrc/auto_rsx -I../libgcm_cmd/src
AUTORSX_CONFIG_$(1) := $$(AUTORSX_DIR_$(1))/profile-identity.cfg
$$(AUTORSX_CONFIG_$(1)): FORCE
	@mkdir -p "$$(@D)"
	@printf 'id=%s\npremises=%s\nkinds=%s\n' '$$(AUTO_PROFILE_ID)' '$$(PROF_PREMISES)' '$$(PROF_KINDS)' > "$$@.tmp"
	@cmp -s "$$@.tmp" "$$@" || cp "$$@.tmp" "$$@"
	@rm -f "$$@.tmp"
$$(AUTORSX_DIR_$(1))/%.o: %.c $$(CONFIG) Makefile auto-rsx.mk ext-members.mk
	@mkdir -p "$$(@D)"
	$$(PPU_CC) $$(AUTORSX_FLAGS_$(1)) -include "$$(CONFIG)" -MMD -MP -c "$$<" -o "$$@"
$$(AUTORSX_DIR_$(1))/libps3dkprof_auto.a: $$(AUTORSX_OBJECTS_$(1)) $$(AUTORSX_CONFIG_$(1)) tools/archive_digest.py src/core/prof_identity.h
	@rm -f "$$@.tmp" "$$(AUTORSX_DIR_$(1))/members.a"
	$$(PPU_AR) rcsD "$$(AUTORSX_DIR_$(1))/members.a" $$(AUTORSX_OBJECTS_$(1))
	python3 tools/archive_digest.py emit "$$(AUTORSX_DIR_$(1))/members.a" --id "$$(AUTO_PROFILE_ID)" \
	    --premises "$$(PROF_PREMISES)" --kinds "$$(PROF_KINDS)" > "$$(AUTORSX_DIR_$(1))/prof_identity.c"
	$$(PPU_CC) $$(AUTORSX_FLAGS_$(1)) -c "$$(AUTORSX_DIR_$(1))/prof_identity.c" -o "$$(AUTORSX_DIR_$(1))/prof_identity.o"
	$$(PPU_AR) rcsD "$$@.tmp" $$(AUTORSX_OBJECTS_$(1)) "$$(AUTORSX_DIR_$(1))/prof_identity.o"
	python3 tools/archive_digest.py verify "$$@.tmp" --id "$$(AUTO_PROFILE_ID)" \
	    --premises "$$(PROF_PREMISES)" --kinds "$$(PROF_KINDS)"
	mv -f "$$@.tmp" "$$@"
AUTORSX_LIBS += $$(AUTORSX_DIR_$(1))/libps3dkprof_auto.a
-include $$(AUTORSX_OBJECTS_$(1):.o=.d)
endef
$(foreach abi,ilp32 lp64,$(eval $(call AUTORSX_VARIANT,$(abi))))
auto-rsx: $(AUTORSX_LIBS)

install-auto-rsx: auto-rsx
	install -d "$(PS3DK)/ppu/lib" "$(PS3DK)/ppu/lib/lp64" "$(PS3DK)/ppu/include/ps3dk"
	for abi in ilp32 lp64; do \
	    sub=; test "$$abi" = ilp32 || sub=/lp64; \
	    install -m 0644 "$(AUTORSX_DIR)/$$abi/libps3dkprof_auto.a" "$(PS3DK)/ppu/lib$$sub/" || exit 1; \
	    cmp "$(AUTORSX_DIR)/$$abi/libps3dkprof_auto.a" "$(PS3DK)/ppu/lib$$sub/libps3dkprof_auto.a" || exit 1; \
	    python3 tools/archive_digest.py verify "$(PS3DK)/ppu/lib$$sub/libps3dkprof_auto.a" \
	        --id "$(AUTO_PROFILE_ID)" --premises "$(PROF_PREMISES)" --kinds "$(PROF_KINDS)" || exit 1; \
	done
	install -m 0644 ../include/ps3dk/prof.h ../include/ps3dk/prof_auto.h ../include/ps3dk/prof_fifo.h $(EXT_PUB_HEADERS) "$(PS3DK)/ppu/include/ps3dk/"

# Run after libgcm_cmd install-auto. Seal only a complete installed pair of
# providers; default archives are never destinations of this explicit target.
install-auto-package: install-auto-rsx ext-live-ilp32 ext-live-lp64
	install -m 0644 "$(EXTL_DIR)/libps3dkprof_ext_live.a" "$(PS3DK)/ppu/lib/"
	install -m 0644 "$(EXTL64_DIR)/libps3dkprof_ext_live.a" "$(PS3DK)/ppu/lib/lp64/"
	install -d "$(PS3DK)/cmake"
	install -m 0644 ../../cmake/ps3-prof-auto.cmake ../../cmake/ps3-prof-auto-check.py ../../cmake/ps3-prof-auto-identity.py "$(PS3DK)/cmake/"
	for abi in ilp32 lp64; do \
	    python3 "$(PS3DK)/cmake/ps3-prof-auto-check.py" --root "$(PS3DK)" --abi "$$abi" \
	        --seal --readelf "$(PPU_READELF)" --source-commit "$(AUTO_SOURCE_COMMIT)" || exit 1; \
	done
