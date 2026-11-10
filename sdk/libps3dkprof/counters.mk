# Named per-frame counters variant (docs/design/ps3dk-prof-format.md section 11): file flag bit 8, chunk 39.
# Explicit goals only: no prerequisite of all/shipped/install, and their own object directories, so the shipped
# archives are unchanged.  Per data model:
#   libps3dkprof_counters.a      the manual-lifecycle members plus counter_runtime, in place of libps3dkprof.a
#   libps3dkprof_ext_counters.a  the SPU/RSX extension members plus counter_runtime (plain extension form, without
#                                the PROFILE identity), in place of libps3dkprof_ext.a
COUNTERS_DIR := $(BUILD)/counters
COUNTERS_ABI_ilp32 :=
COUNTERS_ABI_lp64 := -mlp64
COUNTERS_OBJ_manual := $(MANUAL) counter_runtime
COUNTERS_OBJ_ext := $(EXT_MEMBERS) counter_runtime
COUNTERS_FLAGS_manual = $(PPU_FLAGS) -Isrc/core -Isrc/pal -Isrc/live
COUNTERS_FLAGS_ext = $(EXT_FLAGS)
COUNTERS_NAME_manual := libps3dkprof_counters.a
COUNTERS_NAME_ext := libps3dkprof_ext_counters.a
vpath %.c src/counters src/spu src/rsx

.PHONY: counters install-counters
define COUNTERS_VARIANT
COUNTERS_OBJECTS_$(1)_$(2) := $$(addprefix $$(COUNTERS_DIR)/$(1)-$(2)/,$$(addsuffix .o,$$(COUNTERS_OBJ_$(2))))
$$(COUNTERS_DIR)/$(1)-$(2)/%.o: %.c $$(CONFIG) Makefile counters.mk ext-members.mk
	@mkdir -p "$$(@D)"
	$$(PPU_CC) $$(COUNTERS_FLAGS_$(2)) $$(COUNTERS_ABI_$(1)) -DPS3DK_PROF_WITH_COUNTERS=1 -Isrc/counters \
	    -include "$$(CONFIG)" -MMD -MP -c "$$<" -o "$$@"
$$(COUNTERS_DIR)/$(1)-$(2)/$$(COUNTERS_NAME_$(2)): $$(COUNTERS_OBJECTS_$(1)_$(2))
	@rm -f "$$@"
	$$(PPU_AR) rcs "$$@" $$^
COUNTERS_LIBS += $$(COUNTERS_DIR)/$(1)-$(2)/$$(COUNTERS_NAME_$(2))
-include $$(COUNTERS_OBJECTS_$(1)_$(2):.o=.d)
endef
$(foreach abi,ilp32 lp64,$(foreach mode,manual ext,$(eval $(call COUNTERS_VARIANT,$(abi),$(mode)))))
counters: $(COUNTERS_LIBS)
install-counters: counters
	install -d "$(PS3DK)/ppu/lib" "$(PS3DK)/ppu/lib/lp64" "$(PS3DK)/ppu/include/ps3dk"
	for abi in ilp32 lp64; do \
	    sub=; test "$$abi" = ilp32 || sub=/lp64; \
	    for lib in "$(COUNTERS_DIR)/$$abi"-*/lib*.a; do \
	        install -m 0644 "$$lib" "$(PS3DK)/ppu/lib$$sub/" || exit 1; \
	        cmp "$$lib" "$(PS3DK)/ppu/lib$$sub/$${lib##*/}" || exit 1; \
	    done; \
	done
	install -m 0644 ../include/ps3dk/prof_counter.h "$(PS3DK)/ppu/include/ps3dk/"
