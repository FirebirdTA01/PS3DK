#---------------------------------------------------------------------------------
# hello-ppu-png - cellPngDec decode smoke test via nidgen stub archive.
#
# Bakes an 8x8 RGBA PNG into the ELF via bin2s and drives the full
# libpngdec lifecycle against RPCS3 / real hardware.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(PSL1GHT)),)
$(error "PSL1GHT is not set. Run: source ../../../scripts/env.sh")
endif

include $(PSL1GHT)/ppu_rules

TARGET      := $(notdir $(CURDIR))
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    :=

TITLE       := Hello PPU PNG
APPID       := HELPPUPNG
CONTENTID   := UP0001-$(APPID)_00-0000000000000000

CFLAGS   = -O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
CXXFLAGS = $(CFLAGS)

LDFLAGS  = $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS    := -lpngdec_stub -lsysmodule_stub -lgcm_sys -lrsx -lio -lsysutil -lrt -llv2 -lm
LIBDIRS :=

# .pkg target: pkg_files/USRDIR/* gets copied to pkg/USRDIR/* so
# /dev_hdd0/game/HELPPUPNG/USRDIR/ps3dk.png exists once the pkg is
# installed on a console / RPCS3.  Build with `make pkg`.
PKGFILES := $(CURDIR)/pkg_files

# Sample asset: pkg_files/USRDIR/ps3dk.png is derived from the SDK's
# shared logo at sdk/assets/ICON0.PNG.  Keeping a single source of
# truth means editing the SDK image re-flows into this sample's pkg
# automatically - no manual copy needed.
PS3DK_LOGO_SRC := $(PS3_TOOLCHAIN_ROOT)/sdk/assets/ICON0.PNG
PS3DK_LOGO_DST := $(CURDIR)/pkg_files/USRDIR/ps3dk.png

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
export BUILDDIR := $(CURDIR)/$(BUILD)

CFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES  := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES  := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
    export LD := $(CC)
else
    export LD := $(CXX)
endif

export OFILES := $(addsuffix .o,$(BINFILES)) \
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

# Copy sdk/assets/ICON0.PNG into the pkg's USRDIR before the PSL1GHT
# %.pkg rule's `cp -rf $(PKGFILES)/*` runs, so the sample's displayed
# image always reflects the current SDK logo.
$(OUTPUT).pkg: $(PS3DK_LOGO_DST)

$(PS3DK_LOGO_DST): $(PS3DK_LOGO_SRC)
	@mkdir -p $(dir $@)
	@echo "  copy    $< -> pkg_files/USRDIR/ps3dk.png"
	@cp $< $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(PS3DK_LOGO_DST)

run:
	ps3load $(OUTPUT).self

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:  $(OFILES)

-include $(DEPENDS)

endif
