# Explicit observed variants. No prerequisites on shipped/install-shipped, and
# no writes to their object directories or archive names.
WAIT_MEMBERS := wait_config wait_runtime wait_writer wait_startup
WAIT_manual := $(MANUAL) $(WAIT_MEMBERS)
WAIT_auto := $(AUTO) $(WAIT_MEMBERS)
WAIT_ext := $(EXT_MEMBERS) $(LIVE) $(WAIT_MEMBERS)
WAIT_NAME_manual := libps3dkprof_wait.a
WAIT_NAME_auto := libps3prof_wait.a
WAIT_NAME_ext := libps3dkprof_ext_wait.a
WAIT_ABI_ilp32 :=
WAIT_ABI_lp64 := -mlp64
WAIT_MODE_ext := -DPS3DK_PROF_WITH_SPU -DPS3DK_PROF_WITH_PROFILE
WAIT_DIR := $(BUILD)/waits
# Required measured build inputs. There is deliberately no shipping default.
WAIT_STACK_ILP32 ?=
WAIT_STACK_LP64 ?=
WAIT_TLS_ILP32 ?=
WAIT_TLS_LP64 ?=
WAIT_STATIC_ILP32 ?=
WAIT_STATIC_LP64 ?=
WAIT_STATIC_EXT_ILP32 ?=
WAIT_STATIC_EXT_LP64 ?=
WAIT_STACK_ilp32 = $(WAIT_STACK_ILP32)
WAIT_STACK_lp64 = $(WAIT_STACK_LP64)
WAIT_TLS_ilp32 = $(WAIT_TLS_ILP32)
WAIT_TLS_lp64 = $(WAIT_TLS_LP64)
WAIT_STATIC_manual_ilp32 = $(WAIT_STATIC_ILP32)
WAIT_STATIC_auto_ilp32 = $(WAIT_STATIC_ILP32)
WAIT_STATIC_manual_lp64 = $(WAIT_STATIC_LP64)
WAIT_STATIC_auto_lp64 = $(WAIT_STATIC_LP64)
WAIT_STATIC_ext_ilp32 = $(WAIT_STATIC_EXT_ILP32)
WAIT_STATIC_ext_lp64 = $(WAIT_STATIC_EXT_LP64)
vpath %.c src/waits src/spu src/rsx

.PHONY: waits waits-check install-waits
waits-check:
	@python3 tools/wait_manifest.py check-inputs --stack $(WAIT_STACK_ILP32) $(WAIT_STACK_LP64) \
	    --tls $(WAIT_TLS_ILP32) $(WAIT_TLS_LP64) --static $(WAIT_STATIC_ILP32) $(WAIT_STATIC_LP64) \
	    --static-ext $(WAIT_STATIC_EXT_ILP32) $(WAIT_STATIC_EXT_LP64)

define WAIT_VARIANT
WAIT_OBJECTS_$(1)_$(2) := $$(addprefix $$(WAIT_DIR)/$(1)/$(2)/,$$(addsuffix .o,$$(WAIT_$(2))))
$$(WAIT_DIR)/$(1)/$(2)/config.h: FORCE | waits-check
	@mkdir -p "$$(@D)"
	@printf '#define PS3DK_PROF_WAIT_STACK_BYTES %s\n#define PS3DK_PROF_WAIT_TLS_BYTES %s\n#define PS3DK_PROF_WAIT_STATIC_BYTES %s\n' '$$(WAIT_STACK_$(1))' '$$(WAIT_TLS_$(1))' '$$(WAIT_STATIC_$(2)_$(1))' > "$$@.tmp"
	@printf '/* profile %s %s %s */\n' '$$(PROF_PROFILE_ID)' '$$(PROF_PREMISES)' '$$(PROF_KINDS)' >> "$$@.tmp"
	@cmp -s "$$@.tmp" "$$@" || cp "$$@.tmp" "$$@"
	@rm -f "$$@.tmp"
$$(WAIT_DIR)/$(1)/$(2)/%.o: %.c $$(CONFIG) $$(WAIT_DIR)/$(1)/$(2)/config.h Makefile waits.mk ext-members.mk | waits-check
	@mkdir -p "$$(@D)"
	$$(PPU_CC) $$(PPU_FLAGS) $$(WAIT_ABI_$(1)) $$(WAIT_MODE_$(2)) -DPS3DK_PROF_WITH_WAITS=1 \
	    -Isrc/core -Isrc/pal -Isrc/spu -Isrc/rsx -Isrc/live -Isrc/waits -fstack-usage \
	    -include "$$(CONFIG)" -include "$$(WAIT_DIR)/$(1)/$(2)/config.h" -MMD -MP -c "$$<" -o "$$@"
$$(WAIT_DIR)/$(1)/$(2)/$$(WAIT_NAME_$(2)): $$(WAIT_OBJECTS_$(1)_$(2)) waits.mk tools/wait_manifest.py tools/archive_digest.py | waits-check
	@rm -f "$$@.tmp"
	$$(PPU_AR) rcsD "$$@.tmp" $$(WAIT_OBJECTS_$(1)_$(2))
	python3 tools/wait_manifest.py finish --archive "$$@.tmp" --compiler "$$(PPU_CC)" --ar "$$(PPU_AR)" \
	    --abi $(1) --mode $(2) --stack $$(WAIT_STACK_$(1)) --tls $$(WAIT_TLS_$(1)) --static $$(WAIT_STATIC_$(2)_$(1)) \
	    --profile-id "$$(PROF_PROFILE_ID)" --premises "$$(PROF_PREMISES)" --kinds "$$(PROF_KINDS)"
	mv -f "$$@.tmp" "$$@"
	mv -f "$$@.tmp.json" "$$@.json"
WAIT_LIBS += $$(WAIT_DIR)/$(1)/$(2)/$$(WAIT_NAME_$(2))
-include $$(WAIT_OBJECTS_$(1)_$(2):.o=.d)
endef
$(foreach abi,ilp32 lp64,$(foreach mode,manual auto ext,$(eval $(call WAIT_VARIANT,$(abi),$(mode)))))
waits: $(WAIT_LIBS)
install-waits: waits
	install -d "$(PS3DK)/ppu/lib" "$(PS3DK)/ppu/lib/lp64" "$(PS3DK)/ppu/include/ps3dk"
	for abi in ilp32 lp64; do \
	    sub=; test "$$abi" = ilp32 || sub=/lp64; \
	    for lib in "$(WAIT_DIR)/$$abi"/*/lib*.a; do \
	        install -m 0644 "$$lib" "$$lib.json" "$(PS3DK)/ppu/lib$$sub/" || exit 1; \
	        cmp "$$lib" "$(PS3DK)/ppu/lib$$sub/$${lib##*/}" || exit 1; \
	    done; \
	done
	install -m 0644 ../include/ps3dk/prof_wait.h ../include/ps3dk/prof_wait_internal.h "$(PS3DK)/ppu/include/ps3dk/"
