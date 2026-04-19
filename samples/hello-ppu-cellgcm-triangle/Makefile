#---------------------------------------------------------------------------------
# hello-ppu-cellgcm-triangle — Phase 7 step 1.5 / parallel-track demo.
#
# Compiles vpshader.vcg / fpshader.fcg into Sony-format .vpo / .fpo
# binaries, embeds them into the ELF as binary blobs (bin2s), and
# links against PSL1GHT's RSX runtime + our cellGcm forwarders.
#
# Shader compiler: defaults to PSL1GHT's cgcomp (the in-tree pinned
# baseline).  Set USE_RSX_CG_COMPILER=1 to swap in our own
# rsx-cg-compiler instead — its output is byte-identical to sce-cgc
# for both shaders in this sample (verified end-to-end via
# tools/rsx-cg-compiler/tests/run_diff.sh).
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(PSL1GHT)),)
$(error "PSL1GHT is not set. Run: source ../../scripts/env.sh")
endif

include $(PSL1GHT)/ppu_rules

# Optional: build shaders with our compiler instead of PSL1GHT cgcomp.
# Output is byte-identical to sce-cgc (and a small superset of cgcomp's
# rules — e.g. emits DPH for `mul(matrix, float4(vec3, 1.0))`).
#
# Compute the absolute path here in the outer scope; CURDIR shifts to
# the BUILD subdir during the recursive make, so we resolve + export.
ifneq ($(strip $(USE_RSX_CG_COMPILER)),)
# Recursive make re-evaluates this Makefile with $(CURDIR) shifted to
# the BUILD subdir.  Using `?=` means the inner pass inherits the
# value from the outer scope's env (via export) and skips the wrong
# CURDIR-relative recompute.
RSX_CG_COMPILER ?= $(abspath $(CURDIR)/../../tools/rsx-cg-compiler/build/rsx-cg-compiler)
export RSX_CG_COMPILER
%.vpo: %.vcg
	@echo "rsx-cg-compiler: $(notdir $<)"
	$(VERB) $(RSX_CG_COMPILER) --profile sce_vp_rsx --emit-container $@ $<
%.fpo: %.fcg
	@echo "rsx-cg-compiler: $(notdir $<)"
	$(VERB) $(RSX_CG_COMPILER) --profile sce_fp_rsx --emit-container $@ $<
endif

TARGET      := $(notdir $(CURDIR))
BUILD       := build
SOURCES     := source
DATA        := data
SHADERS     := shaders
INCLUDES    :=

TITLE       := Hello PPU cellGcm Triangle
APPID       := HELLOTRI01
CONTENTID   := UP0001-$(APPID)_00-0000000000000000

CFLAGS   = -O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
CXXFLAGS = $(CFLAGS)

LDFLAGS  = $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS    := -lgcm_cmd -lrsx -lgcm_sys -lio -lsysutil -lsysmodule -lrt -llv2 -lm
LIBDIRS :=

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(SHADERS),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
export BUILDDIR := $(CURDIR)/$(BUILD)

CFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES  := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES  := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))
VCGFILES  := $(foreach dir,$(SHADERS),$(notdir $(wildcard $(dir)/*.vcg)))
FCGFILES  := $(foreach dir,$(SHADERS),$(notdir $(wildcard $(dir)/*.fcg)))

VPOFILES  := $(VCGFILES:.vcg=.vpo)
FPOFILES  := $(FCGFILES:.fcg=.fpo)

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export OFILES := $(addsuffix .o,$(BINFILES)) \
                 $(addsuffix .o,$(VPOFILES)) \
                 $(addsuffix .o,$(FPOFILES)) \
                 $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
                 $(sFILES:.s=.o) $(SFILES:.S=.o)

export INCLUDE := $(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  $(LIBPSL1GHT_INC) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
                   $(LIBPSL1GHT_LIB)

.PHONY: $(BUILD) clean run

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self

run:
	ps3load $(OUTPUT).self

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:  $(OFILES)

%.vpo.o: %.vpo
	@echo $(notdir $<)
	@$(bin2o)

%.fpo.o: %.fpo
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
