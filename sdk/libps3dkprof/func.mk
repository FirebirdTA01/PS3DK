# Function-path variant (docs/design/ps3dk-prof-func-paths.md): the manual-lifecycle archive members plus the
# -finstrument-functions hooks, compiled with PS3DK_PROF_WITH_FUNC, one archive per data model.  Explicit goals only:
# no prerequisite of all/shipped/install, and its own object directories, so the shipped archives are unchanged.
# The library itself is never instrumented (PPU_FLAGS carries -fno-instrument-functions).
FUNC_MEMBERS := func_runtime func_hooks
FUNC_OBJ_NAMES := $(MANUAL) $(FUNC_MEMBERS)
FUNC_DIR := $(BUILD)/func
FUNC_ABI_ilp32 :=
FUNC_ABI_lp64 := -mlp64
FUNC_SUB_ilp32 :=
FUNC_SUB_lp64 := /lp64
vpath %.c src/func

.PHONY: func install-func func-ext install-func-ext
define FUNC_VARIANT
FUNC_OBJECTS_$(1) := $$(addprefix $$(FUNC_DIR)/$(1)/,$$(addsuffix .o,$$(FUNC_OBJ_NAMES)))
$$(FUNC_DIR)/$(1)/%.o: %.c $$(CONFIG) Makefile func.mk
	@mkdir -p "$$(@D)"
	$$(PPU_CC) $$(PPU_FLAGS) $$(FUNC_ABI_$(1)) -DPS3DK_PROF_WITH_FUNC=1 -Isrc/core -Isrc/pal -Isrc/live -Isrc/func \
	    -include "$$(CONFIG)" -MMD -MP -c "$$<" -o "$$@"
$$(FUNC_DIR)/$(1)/libps3dkprof_func.a: $$(FUNC_OBJECTS_$(1))
	@rm -f "$$@"
	$$(PPU_AR) rcs "$$@" $$^
FUNC_LIBS += $$(FUNC_DIR)/$(1)/libps3dkprof_func.a
-include $$(FUNC_OBJECTS_$(1):.o=.d)
endef
$(foreach abi,ilp32 lp64,$(eval $(call FUNC_VARIANT,$(abi))))
func: $(FUNC_LIBS)
install-func: func
	install -d "$(PS3DK)/ppu/lib" "$(PS3DK)/ppu/lib/lp64" "$(PS3DK)/ppu/include/ps3dk"
	for abi in ilp32 lp64; do \
	    sub=; test "$$abi" = ilp32 || sub=/lp64; \
	    install -m 0644 "$(FUNC_DIR)/$$abi/libps3dkprof_func.a" "$(PS3DK)/ppu/lib$$sub/" || exit 1; \
	    cmp "$(FUNC_DIR)/$$abi/libps3dkprof_func.a" "$(PS3DK)/ppu/lib$$sub/libps3dkprof_func.a" || exit 1; \
	done
	install -m 0644 ../include/ps3dk/prof_func.h "$(PS3DK)/ppu/include/ps3dk/"

# The function-path form of the RELEASE extension archive (SPU lanes + GPU domains, PROFILE identity), per data model:
# the ext members plus the hooks, compiled with that ABI's PROFILE extension flags and PS3DK_PROF_WITH_FUNC, then the
# same identity pipeline as ext-<abi>-profile (a generated prof_identity.o holding the ARCHIVE-DIGEST-V1 of the other
# members, re-verified before the archive is replaced).  An app links libps3dkprof_ext_live.a (unchanged: the live
# members do not include prof_internal.h) and libps3dkprof_ext_func.a INSTEAD of libps3dkprof_ext.a.  The existing
# ext archives, their directories and their rules are untouched.
FUNCX_OBJ_NAMES := $(EXT_MEMBERS) $(FUNC_MEMBERS)
vpath %.c src/spu src/rsx
define FUNCX_VARIANT
FUNCX_DIR_$(1) := $$(FUNC_DIR)/$(1)-ext
FUNCX_OBJECTS_$(1) := $$(addprefix $$(FUNCX_DIR_$(1))/,$$(addsuffix .o,$$(FUNCX_OBJ_NAMES)))
FUNCX_FLAGS_$(1) := $$(EXT_FLAGS_$(1)) -DPS3DK_PROF_WITH_PROFILE -DPS3DK_PROF_WITH_FUNC=1 -Isrc/func
FUNCX_ID_CONFIG_$(1) := $$(FUNCX_DIR_$(1))/profile-identity.cfg
$$(FUNCX_ID_CONFIG_$(1)): FORCE
	@mkdir -p "$$(@D)"
	@printf 'id=%s\npremises=%s\nkinds=%s\n' '$$(PROF_PROFILE_ID)' '$$(PROF_PREMISES)' '$$(PROF_KINDS)' > "$$@.tmp"
	@cmp -s "$$@.tmp" "$$@" || cp "$$@.tmp" "$$@"
	@rm -f "$$@.tmp"
$$(FUNCX_DIR_$(1))/%.o: %.c $$(CONFIG) Makefile func.mk ext-members.mk
	@mkdir -p "$$(@D)"
	$$(PPU_CC) $$(FUNCX_FLAGS_$(1)) -include "$$(CONFIG)" -MMD -MP -c "$$<" -o "$$@"
$$(FUNCX_DIR_$(1))/libps3dkprof_ext_func.a: $$(FUNCX_OBJECTS_$(1)) $$(FUNCX_ID_CONFIG_$(1)) tools/archive_digest.py src/core/prof_identity.h
	@rm -f "$$@" "$$@.tmp" "$$(FUNCX_DIR_$(1))/members.a" "$$(FUNCX_DIR_$(1))/prof_identity.c" "$$(FUNCX_DIR_$(1))/prof_identity.o"
	$$(PPU_AR) rcsD "$$(FUNCX_DIR_$(1))/members.a" $$(FUNCX_OBJECTS_$(1))
	python3 tools/archive_digest.py emit "$$(FUNCX_DIR_$(1))/members.a" --id "$$(PROF_PROFILE_ID)" \
	    --premises "$$(PROF_PREMISES)" --kinds "$$(PROF_KINDS)" > "$$(FUNCX_DIR_$(1))/prof_identity.c"
	$$(PPU_CC) $$(FUNCX_FLAGS_$(1)) -c "$$(FUNCX_DIR_$(1))/prof_identity.c" -o "$$(FUNCX_DIR_$(1))/prof_identity.o"
	$$(PPU_AR) rcsD "$$@.tmp" $$(FUNCX_OBJECTS_$(1)) "$$(FUNCX_DIR_$(1))/prof_identity.o"
	python3 tools/archive_digest.py verify "$$@.tmp" --id "$$(PROF_PROFILE_ID)" --premises "$$(PROF_PREMISES)" \
	    --kinds "$$(PROF_KINDS)"
	mv -f "$$@.tmp" "$$@"
FUNCX_LIBS += $$(FUNCX_DIR_$(1))/libps3dkprof_ext_func.a
-include $$(FUNCX_OBJECTS_$(1):.o=.d)
endef
$(foreach abi,ilp32 lp64,$(eval $(call FUNCX_VARIANT,$(abi))))
func-ext: $(FUNCX_LIBS)
install-func-ext: func-ext
	install -d "$(PS3DK)/ppu/lib" "$(PS3DK)/ppu/lib/lp64" "$(PS3DK)/ppu/include/ps3dk"
	for abi in ilp32 lp64; do \
	    sub=; test "$$abi" = ilp32 || sub=/lp64; \
	    install -m 0644 "$(FUNC_DIR)/$$abi-ext/libps3dkprof_ext_func.a" "$(PS3DK)/ppu/lib$$sub/" || exit 1; \
	    cmp "$(FUNC_DIR)/$$abi-ext/libps3dkprof_ext_func.a" "$(PS3DK)/ppu/lib$$sub/libps3dkprof_ext_func.a" || exit 1; \
	    python3 tools/archive_digest.py verify "$(PS3DK)/ppu/lib$$sub/libps3dkprof_ext_func.a" \
	        --id "$(PROF_PROFILE_ID)" --premises "$(PROF_PREMISES)" --kinds "$(PROF_KINDS)" || exit 1; \
	done
	install -m 0644 ../include/ps3dk/prof_func.h "$(PS3DK)/ppu/include/ps3dk/"
