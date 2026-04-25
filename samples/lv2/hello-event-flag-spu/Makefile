#---------------------------------------------------------------------------------
# hello-event-flag-spu - reference SDK lv2/event_flag sample, adapted for our SDK.
#
# Six-thread synchronization round-trip: a PPU master + 5 PPU workers
# coordinate via sys_event_flag_*; one SPU thread (running our embedded
# spu_ef.elf) takes commands via SNR1/SNR2 and either sets or
# imatiently-sets a bit on a PPU-owned event flag.  Tests our new
# SPU-side libsputhread (sys_event_flag_set_bit / set_bit_impatient /
# spu_thread_exit / spu_thread_group_exit), our PPU-side
# sys/synchronization.h sys_event_flag_* surface, and the
# sys_spu_image_import + sys_spu_thread_initialize boundary.
#
# Diverges from Sony's source on one line: the reference uses
# sys_spu_image_open against an /app_home/ path; we use
# sys_spu_image_import with an embedded SPU ELF blob (bin2o macro)
# so the sample is self-contained.
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
INCLUDES    := source

TITLE       := Hello Event Flag SPU
APPID       := HELEFSPU01
CONTENTID   := UP0001-$(APPID)_00-0000000000000000

CFLAGS   = -O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
LDFLAGS  = $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS    := -lspu_printf_stub -lrt -llv2 -lm
LIBDIRS :=

# SPU side: standard spu-elf-gcc build using PSL1GHT's spu_rules,
# linked against our libsputhread for sys_event_flag_set_bit etc.
SPU_ELF       := $(CURDIR)/spu/spu_ef.elf
SPU_BIN       := $(CURDIR)/$(DATA)/spu_ef_bin

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
BINFILES  := spu_ef_bin

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

$(BUILD): $(SPU_BIN)
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(SPU_BIN): spu
	@mkdir -p $(dir $@)
	@cp -f $(SPU_ELF) $@

spu:
	@$(MAKE) --no-print-directory -C spu

clean:
	@echo clean ...
	@$(MAKE) --no-print-directory -C spu clean
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(SPU_BIN) $(CURDIR)/$(DATA)

run:
	ps3load $(OUTPUT).self

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:  $(OFILES)

%.bin.o : %.bin
	@echo $(notdir $<)
	@$(bin2o)

# bin2o is keyed on the input file extension being .bin; our SPU blob
# is named spu_ef_bin (no extension) so VPATH picks it up via BINFILES
# directly and the bin2s/$(AS) pipeline sees it as a generic file.
spu_ef_bin.o : spu_ef_bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
