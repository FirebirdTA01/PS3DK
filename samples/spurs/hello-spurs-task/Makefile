#---------------------------------------------------------------------------------
# hello-ppu-spurs-task - Spurs Task end-to-end (PPU + embedded SPU task).
#
# Builds the SPU task ELF first with spu-elf-gcc against libspurs_task.a
# using our spurs_task.ld linker script.  Copies the ELF to data/ so
# PSL1GHT's bin2o embeds it into the PPU ELF.  The resulting symbols
# `spu_task_bin[]` + `spu_task_bin_end[]` + `spu_task_bin_size` give
# the PPU code a pointer at which the SPRX can parse the task ELF.
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

TITLE       := Hello PPU Spurs Task
APPID       := HELSPTSK01
CONTENTID   := UP0001-$(APPID)_00-0000000000000000

CFLAGS   = -O2 -Wall -mcpu=cell $(MACHDEP) $(INCLUDE)
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
LDFLAGS  = $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS    := -lspurs_stub -lc_stub -lsysmodule_stub -lsysutil -lrt -llv2 -lm
LIBDIRS :=

# SPU-side build: produce spu_task.elf then stage to data/spu_task.bin
SPU_CC        := spu-elf-gcc
SPU_LD_SCRIPT := $(PS3DK)/spu/ldscripts/spurs_task.ld
SPU_SRC       := spu/source/spu_task_main.c
SPU_ELF       := $(CURDIR)/$(BUILD)/spu_task.elf
SPU_BIN       := $(CURDIR)/$(DATA)/spu_task.bin

SPU_CFLAGS    := -O2 -Wall -Wextra -fno-exceptions -ffreestanding \
                 -I$(PS3DK)/spu/include

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
BINFILES  := spu_task.bin

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

$(BUILD): $(SPU_BIN)
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(SPU_BIN): $(SPU_ELF)
	@mkdir -p $(dir $@)
	@cp -f $< $@

$(SPU_ELF): $(SPU_SRC) $(SPU_LD_SCRIPT)
	@mkdir -p $(dir $@)
	@echo "  SPU CC+LD $(notdir $@)"
	@$(SPU_CC) $(SPU_CFLAGS) -nostartfiles -T $(SPU_LD_SCRIPT) \
	    $(SPU_SRC) -L$(PS3DK)/spu/lib -lspurs_task -o $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(CURDIR)/data

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
