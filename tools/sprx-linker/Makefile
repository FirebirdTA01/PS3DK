# sprx-linker build/install
#
# Installs to $(PS3DK)/bin/sprxlinker so PSL1GHT's ppu_rules can find
# us on PATH ahead of the stock copy at $(PS3DEV)/bin/.

CFLAGS  ?= -O2 -Wall -std=gnu99
LDFLAGS ?=
LDLIBS  ?= -lelf

PS3DK   ?= $(PS3DEV)/ps3dk
BINDIR  := $(PS3DK)/bin

TARGET  := sprxlinker

.PHONY: all install clean

all: $(TARGET)

$(TARGET): sprx-linker.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: $(TARGET)
	@mkdir -p $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)
