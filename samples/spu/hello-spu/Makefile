#---------------------------------------------------------------------------------
# hello-spu — minimal PPU+SPU smoke test.
#
# The SPU binary is built in spu/ via $(PSL1GHT)/spu_rules, converted to a
# header embedding, and linked into the PPU ELF.
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
INCLUDES    := include

TITLE       := Hello SPU
APPID       := HELSPU001
CONTENTID   := UP0001-$(APPID)_00-0000000000000000

CFLAGS   = -O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
CXXFLAGS = $(CFLAGS) -std=c++17
LDFLAGS  = $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS    := -lrt -llv2 -lm
LIBDIRS :=

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   := $(CURDIR)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)
export BUILDDIR := $(CURDIR)/$(BUILD)

# SPU binary artifact: spu/hello_spu.elf -> hello_spu.bin -> hello_spu_bin.{h,o}
SPU_ELF := $(CURDIR)/spu/hello_spu.elf

CFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES  := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES  := hello_spu.bin

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

.PHONY: $(BUILD) clean run spu

$(BUILD): spu
	@[ -d $@ ] || mkdir -p $@
	@cp -f $(SPU_ELF) hello_spu.elf
	@sprxlinker hello_spu.elf || true
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

spu:
	@$(MAKE) --no-print-directory -C spu

clean:
	@echo clean ...
	@$(MAKE) --no-print-directory -C spu clean
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self hello_spu.elf hello_spu.bin

run:
	ps3load $(OUTPUT).self

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:  $(OFILES)

%.bin.o : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
