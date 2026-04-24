#---------------------------------------------------------------------------------
# hello-ppu-spurs-event-flag - SPURS event-flag bring-up smoke test.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(PSL1GHT)),)
$(error "PSL1GHT is not set. Run: source ../../../scripts/env.sh")
endif

include $(PSL1GHT)/ppu_rules

TARGET      := $(notdir $(CURDIR))
BUILD       := build
SOURCES     := source
INCLUDES    :=

TITLE       := Hello PPU Spurs EventFlag
APPID       := HELSPEVF01
CONTENTID   := UP0001-$(APPID)_00-0000000000000000

CFLAGS   = -O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
LDFLAGS  = $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS    := -lspurs_stub -lspu_printf_stub -lsysmodule_stub -lsysutil -lrt -llv2 -lm

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
export BUILDDIR := $(CURDIR)/$(BUILD)

CPPFILES  := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))

export LD := $(CXX)
export OFILES := $(CPPFILES:.cpp=.o)

export INCLUDE := -I$(PS3DK)/ppu/include $(LIBPSL1GHT_INC) -I$(CURDIR)/$(BUILD)
export LIBPATHS := -L$(PS3DK)/ppu/lib $(LIBPSL1GHT_LIB)

.PHONY: $(BUILD) clean run

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self

run:
	ps3load $(OUTPUT).self

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:  $(OFILES)

-include $(DEPENDS)

endif
