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

.PHONY: func install-func
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
