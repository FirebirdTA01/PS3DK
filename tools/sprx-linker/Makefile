# sprx-linker build/install
#
# Installs to $(PS3DEV)/bin/sprxlinker, overwriting the PSL1GHT-stock copy
# that %.self rules (via $(SPRX) in ppu_rules) actually invoke. A mirror
# also goes to $(PS3DK)/bin/ for explicit references.

CFLAGS  ?= -O2 -Wall -std=gnu99
LDFLAGS ?=
LDLIBS  ?= -lelf

PS3DK   ?= $(PS3DEV)/ps3dk
PATHBIN := $(PS3DEV)/bin
MIRROR  := $(PS3DK)/bin

TARGET  := sprxlinker

.PHONY: all install clean

all: $(TARGET)

$(TARGET): sprx-linker.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: $(TARGET)
	@mkdir -p $(PATHBIN) $(MIRROR)
	install -m 755 $(TARGET) $(PATHBIN)/$(TARGET)
	install -m 755 $(TARGET) $(MIRROR)/$(TARGET)

clean:
	rm -f $(TARGET)
