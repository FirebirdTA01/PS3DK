#---------------------------------------------------------------------------------
# hello-ppu-sony-gcm-basic — Cg→rsx bridge runtime verification.
#
# Compiles clean-room Cg sources via sce-cgc (Sony's official compiler,
# run through wine), producing Sony-format CgBinaryProgram blobs that
# flow through our cellGcmSetVertexProgram / cellGcmSetFragmentProgram
# bridge at runtime.  This exercises the full toolchain path that a
# ported PS3 title would take; contrast with hello-ppu-cellgcm-triangle,
# which uses the PSL1GHT-native cgcomp + rsxLoadVertexProgram path.
#
# Build requirements:
#   - PSL1GHT (source ../../scripts/env.sh)
#   - wine on $PATH
#   - sce-cgc.exe at $(SCE_CGC), default:
#       $(HOME)/cell-sdk/475.001/cell/host-win32/Cg/bin/sce-cgc.exe
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(PSL1GHT)),)
$(error "PSL1GHT is not set. Run: source ../../scripts/env.sh")
endif

include $(PSL1GHT)/ppu_rules

TARGET      := $(notdir $(CURDIR))
BUILD       := build
SOURCES     := source
DATA        := data
SHADERS     := shaders
INCLUDES    :=

TITLE       := Hello PPU Sony GCM Basic
APPID       := HELLOSGB01
CONTENTID   := UP0001-$(APPID)_00-0000000000000000

CFLAGS   = -O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
CXXFLAGS = $(CFLAGS) -fpermissive

LDFLAGS  = $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS    := -lgcm_cmd -lrsx -lgcm_sys -lio -lsysutil -lsysmodule -lrt -llv2 -lm
LIBDIRS :=

# sce-cgc (Sony's Cg compiler) — run via wine.
SCE_CGC ?= $(HOME)/cell-sdk/475.001/cell/host-win32/Cg/bin/sce-cgc.exe
WINE    ?= wine

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

# We embed two shader blobs — vpshader.vpo (vertex) and fpshader.fpo
# (fragment) — produced from .cg by sce-cgc + wine.  List them
# explicitly rather than globbing, since both share the same .cg
# extension but use different sce-cgc profiles.
VPOFILES  := vpshader.vpo
FPOFILES  := fpshader.fpo

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

# Export so the sub-make (inside $(BUILD)) can find sce-cgc + wine.
export SCE_CGC
export WINE

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

# Override PSL1GHT's implicit %.vpo:%.vcg / %.fpo:%.fcg rules with
# sce-cgc invocations.  We build in the $(BUILD) working dir, so
# emit the .vpo / .fpo there; bin2o consumes them from the same dir.
vpshader.vpo: vpshader.cg
	@echo sce-cgc vp: $(notdir $<)
	@$(WINE) $(SCE_CGC) -profile sce_vp_rsx -o $@ $< 2>&1 | \
	  grep -Ev 'libEGL|pci id|^[[:space:]]*$$' || true

fpshader.fpo: fpshader.cg
	@echo sce-cgc fp: $(notdir $<)
	@$(WINE) $(SCE_CGC) -profile sce_fp_rsx -o $@ $< 2>&1 | \
	  grep -Ev 'libEGL|pci id|^[[:space:]]*$$' || true

%.vpo.o: %.vpo
	@echo $(notdir $<)
	@$(bin2o)

%.fpo.o: %.fpo
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
