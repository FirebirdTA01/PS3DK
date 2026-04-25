#---------------------------------------------------------------------------------
# hello-ppu-cellgcm-chain — rsx-cg-compiler FP arithmetic-chain validation.
#
# Built explicitly with our own rsx-cg-compiler (NOT PSL1GHT cgcomp) so
# the sample exercises the FP arithmetic-chain ADDR shape (varying +
# uniform + uniform + uniform — chained adds where the running
# accumulator lives in a temp register and SRC0=TEMP / SRC1=CONST flips
# from the canonical lone-add SRC0=CONST / SRC1=INPUT order).
#
# Override with `USE_RSX_CG_COMPILER=0 make` to fall back to PSL1GHT's
# cgcomp (will likely diverge — cgcomp's chain shape is not byte-exact
# vs sce-cgc).
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(PSL1GHT)),)
$(error "PSL1GHT is not set. Run: source ../../../scripts/env.sh")
endif

include $(PSL1GHT)/ppu_rules

# Default to our compiler — that's the whole point of this sample.
USE_RSX_CG_COMPILER ?= 1

ifneq ($(strip $(USE_RSX_CG_COMPILER)),0)
RSX_CG_COMPILER ?= $(abspath $(CURDIR)/../../../tools/rsx-cg-compiler/build/rsx-cg-compiler)
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

TITLE       := Hello PPU cellGcm Chain
APPID       := HELLOCH01
CONTENTID   := UP0001-$(APPID)_00-0000000000000000

CFLAGS   = -O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
CXXFLAGS = $(CFLAGS)

LDFLAGS  = $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS    := -lgcm_cmd -lgcm_sys -lio -lsysutil -lsysmodule -lrt -llv2 -lm
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
                  -I$(PS3DK)/ppu/include \
                  $(LIBPSL1GHT_INC) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
                   -L$(PS3DK)/ppu/lib \
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
