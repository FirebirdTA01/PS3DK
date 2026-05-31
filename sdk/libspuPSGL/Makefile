# sdk/libspuPSGL/Makefile -- SPU-side PSGL library.
# Builds libspuPSGL.a with SPU draw-hole filler and IOIF helpers.

PS3DEV     ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
SPU_PREFIX ?= $(PS3DEV)/spu

INSTALL_LIB := $(SPU_PREFIX)/spu-elf/lib
INSTALL_LIB_MIRROR := $(PS3DK)/spu/lib

CC      := spu-elf-gcc
AR      := spu-elf-ar

CFLAGS  := -O2 -Wall -Wextra -fno-exceptions -ffreestanding \
           -I../include -I../include-spu

SRC     := src/api.c
OBJ     := build/api.o
ARCHIVE := build/libspuPSGL.a

.PHONY: all clean install

all: $(ARCHIVE)

$(ARCHIVE): $(OBJ)
	$(AR) rcs $@ $^

$(OBJ): $(SRC) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

install: $(ARCHIVE)
	install -d $(INSTALL_LIB) $(INSTALL_LIB_MIRROR)
	install -m 0644 $(ARCHIVE) $(INSTALL_LIB)/libspuPSGL.a
	install -m 0644 $(ARCHIVE) $(INSTALL_LIB_MIRROR)/libspuPSGL.a
	install -d $(PS3DK)/spu/include/PSGL
	install -m 0644 ../include-spu/PSGL/spu_psgl.h $(PS3DK)/spu/include/PSGL/spu_psgl.h

clean:
	rm -rf build
