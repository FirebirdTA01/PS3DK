TARGET := powerpc64-ps3-kernel-elf

PS3DEV ?= $(abspath ../../../stage/ps3dev)
TOOL_PREFIX ?= $(PS3DEV)/ppu-kernel/bin/$(TARGET)-

CC      := $(TOOL_PREFIX)gcc
OBJCOPY := $(TOOL_PREFIX)objcopy
READELF := $(TOOL_PREFIX)readelf

BUILD_DIR := build
ELF       := $(BUILD_DIR)/payload.elf
BIN       := $(BUILD_DIR)/payload.bin
MAP       := $(BUILD_DIR)/payload.map

SOURCES_C := source/main.c
SOURCES_S := source/start.S
OBJECTS   := $(BUILD_DIR)/main.o $(BUILD_DIR)/start.o

CFLAGS := -O2 -Wall -Wextra -ffreestanding -fno-builtin \
          -ffunction-sections -fdata-sections -fno-stack-protector
ASFLAGS := -ffreestanding
LDFLAGS := -nostartfiles -nostdlib -nodefaultlibs -T payload.ld \
           -Wl,-q -Wl,--gc-sections -Wl,-Map=$(MAP)

.PHONY: all verify clean

all: $(ELF) $(BIN)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/main.o: source/main.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/start.o: source/start.S | $(BUILD_DIR)
	$(CC) $(ASFLAGS) -c -o $@ $<

$(ELF): $(OBJECTS) payload.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

verify: $(ELF) $(BIN)
	@test -s $(BIN)
	@$(READELF) -SW $(ELF) | awk '$$3 == ".opd" { found = 1; if ($$6 != "000048" || $$12 != "8") exit 1 } END { exit found ? 0 : 1 }'
	@$(READELF) -r $(ELF) | awk '/\.rela\.opd/ { in_opd = 1; next } /^Relocation section/ { in_opd = 0 } in_opd && /R_PPC64_ADDR64/ { count++ } END { exit count >= 3 ? 0 : 1 }'
	@$(READELF) -r $(ELF) | awk '/\.rela\.data/ { in_data = 1; next } /^Relocation section/ { in_data = 0 } in_data && /R_PPC64_ADDR64/ { count++ } END { exit count >= 2 ? 0 : 1 }'
	@$(READELF) -SW $(ELF) | grep -E ' \.text| \.data| \.opd| \.toc'
	@$(READELF) -r $(ELF) | grep -E 'R_PPC64_ADDR64|\.rela\.opd|\.rela\.data'
	@ls -l $(ELF) $(BIN)

clean:
	rm -rf $(BUILD_DIR)
