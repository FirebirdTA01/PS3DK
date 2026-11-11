# FIFO wrap phase events (docs/design/ps3dk-prof-fifo.md): the extension archive (SPU lanes + GPU domains, PROFILE
# identity) plus the FIFO observer, compiled with PS3DK_PROF_WITH_FIFO, one archive per data model.  Explicit goals only
# (fifo-ext / install-fifo-ext): no prerequisite of all, shipped or install, and its own object directories, so the
# default archives and their build plans are unchanged.  An app links libps3dkprof_ext_live.a (unchanged) and
# libps3dkprof_ext_fifo.a INSTEAD of libps3dkprof_ext.a, and libgcm_cmd_prof_fifo.a (sdk/libgcm_cmd `fifo`) INSTEAD of
# libgcm_cmd.a; with the default libgcm_cmd.a the observer bridge symbols stay undefined and the link fails.  There is
# no function-path composition of this archive yet.  The same identity pipeline as ext-<abi>-profile: the generated
# prof_identity.o holds the ARCHIVE-DIGEST-V1 of the other members, re-verified before the archive is replaced.
FIFO_MEMBERS := fifo_runtime fifo_writer
FIFOX_OBJ_NAMES := $(EXT_MEMBERS) $(FIFO_MEMBERS)
FIFOX_DIR := $(BUILD)/fifo
vpath %.c src/fifo

.PHONY: fifo-ext install-fifo-ext
define FIFOX_VARIANT
FIFOX_DIR_$(1) := $$(FIFOX_DIR)/$(1)-ext
FIFOX_OBJECTS_$(1) := $$(addprefix $$(FIFOX_DIR_$(1))/,$$(addsuffix .o,$$(FIFOX_OBJ_NAMES)))
FIFOX_FLAGS_$(1) := $$(EXT_FLAGS_$(1)) -DPS3DK_PROF_WITH_PROFILE -DPS3DK_PROF_WITH_FIFO=1 -Isrc/fifo -I../libgcm_cmd/src
FIFOX_ID_CONFIG_$(1) := $$(FIFOX_DIR_$(1))/profile-identity.cfg
$$(FIFOX_ID_CONFIG_$(1)): FORCE
	@mkdir -p "$$(@D)"
	@printf 'id=%s\npremises=%s\nkinds=%s\n' '$$(PROF_PROFILE_ID)' '$$(PROF_PREMISES)' '$$(PROF_KINDS)' > "$$@.tmp"
	@cmp -s "$$@.tmp" "$$@" || cp "$$@.tmp" "$$@"
	@rm -f "$$@.tmp"
$$(FIFOX_DIR_$(1))/%.o: %.c $$(CONFIG) Makefile fifo.mk ext-members.mk
	@mkdir -p "$$(@D)"
	$$(PPU_CC) $$(FIFOX_FLAGS_$(1)) -include "$$(CONFIG)" -MMD -MP -c "$$<" -o "$$@"
$$(FIFOX_DIR_$(1))/libps3dkprof_ext_fifo.a: $$(FIFOX_OBJECTS_$(1)) $$(FIFOX_ID_CONFIG_$(1)) tools/archive_digest.py src/core/prof_identity.h
	@rm -f "$$@" "$$@.tmp" "$$(FIFOX_DIR_$(1))/members.a" "$$(FIFOX_DIR_$(1))/prof_identity.c" "$$(FIFOX_DIR_$(1))/prof_identity.o"
	$$(PPU_AR) rcsD "$$(FIFOX_DIR_$(1))/members.a" $$(FIFOX_OBJECTS_$(1))
	python3 tools/archive_digest.py emit "$$(FIFOX_DIR_$(1))/members.a" --id "$$(PROF_PROFILE_ID)" \
	    --premises "$$(PROF_PREMISES)" --kinds "$$(PROF_KINDS)" > "$$(FIFOX_DIR_$(1))/prof_identity.c"
	$$(PPU_CC) $$(FIFOX_FLAGS_$(1)) -c "$$(FIFOX_DIR_$(1))/prof_identity.c" -o "$$(FIFOX_DIR_$(1))/prof_identity.o"
	$$(PPU_AR) rcsD "$$@.tmp" $$(FIFOX_OBJECTS_$(1)) "$$(FIFOX_DIR_$(1))/prof_identity.o"
	python3 tools/archive_digest.py verify "$$@.tmp" --id "$$(PROF_PROFILE_ID)" --premises "$$(PROF_PREMISES)" \
	    --kinds "$$(PROF_KINDS)"
	mv -f "$$@.tmp" "$$@"
FIFOX_LIBS += $$(FIFOX_DIR_$(1))/libps3dkprof_ext_fifo.a
-include $$(FIFOX_OBJECTS_$(1):.o=.d)
endef
$(foreach abi,ilp32 lp64,$(eval $(call FIFOX_VARIANT,$(abi))))
fifo-ext: $(FIFOX_LIBS)
install-fifo-ext: fifo-ext
	install -d "$(PS3DK)/ppu/lib" "$(PS3DK)/ppu/lib/lp64" "$(PS3DK)/ppu/include/ps3dk"
	for abi in ilp32 lp64; do \
	    sub=; test "$$abi" = ilp32 || sub=/lp64; \
	    install -m 0644 "$(FIFOX_DIR)/$$abi-ext/libps3dkprof_ext_fifo.a" "$(PS3DK)/ppu/lib$$sub/" || exit 1; \
	    cmp "$(FIFOX_DIR)/$$abi-ext/libps3dkprof_ext_fifo.a" "$(PS3DK)/ppu/lib$$sub/libps3dkprof_ext_fifo.a" || exit 1; \
	    python3 tools/archive_digest.py verify "$(PS3DK)/ppu/lib$$sub/libps3dkprof_ext_fifo.a" \
	        --id "$(PROF_PROFILE_ID)" --premises "$(PROF_PREMISES)" --kinds "$(PROF_KINDS)" || exit 1; \
	done
	install -m 0644 ../include/ps3dk/prof_fifo.h "$(PS3DK)/ppu/include/ps3dk/"

# Run after sdk/libgcm_cmd install-fifo. This explicit goal adds only the
# observed package and its unchanged live companion; no default archive write.
FIFO_SOURCE_COMMIT ?= $(shell git rev-parse HEAD)
PPU_READELF ?= $(CROSS)readelf
.PHONY: install-fifo-package
install-fifo-package: install-fifo-ext ext-live-ilp32 ext-live-lp64
	install -m 0644 "$(EXTL_DIR)/libps3dkprof_ext_live.a" "$(PS3DK)/ppu/lib/"
	install -m 0644 "$(EXTL64_DIR)/libps3dkprof_ext_live.a" "$(PS3DK)/ppu/lib/lp64/"
	install -m 0644 ../include/ps3dk/prof.h $(EXT_PUB_HEADERS) "$(PS3DK)/ppu/include/ps3dk/"
	install -d "$(PS3DK)/cmake"
	install -m 0644 ../../cmake/ps3-prof-fifo.cmake ../../cmake/ps3-prof-fifo-check.py "$(PS3DK)/cmake/"
	for abi in ilp32 lp64; do \
	    python3 "$(PS3DK)/cmake/ps3-prof-fifo-check.py" --root "$(PS3DK)" --abi "$$abi" \
	        --seal --readelf "$(PPU_READELF)" --source-commit "$(FIFO_SOURCE_COMMIT)" || exit 1; \
	done
