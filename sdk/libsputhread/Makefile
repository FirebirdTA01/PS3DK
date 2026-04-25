# sdk/libsputhread/Makefile — SPU-side lv2 syscall wrappers.
#
#   libsputhread.a            -> $(PS3DK)/spu/lib/
#   sys/event_flag.h          -> $(PS3DK)/spu/include/
#   sys/spu_thread.h          -> $(PS3DK)/spu/include/
#
# Each function is a hand-written assembly stub matching the lv2
# kernel's SPU-side calling convention (mailbox channels ch22/23/24
# for sync, ch28/29/30 for arg passing, `stop` traps for exit).
# Byte-for-byte equivalent to the reference libsputhread.a.

PS3DEV     ?= $(error PS3DEV is not set — run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
INSTALL_LIB := $(PS3DK)/spu/lib
INSTALL_INC := $(PS3DK)/spu/include

CC      := spu-elf-gcc
AR      := spu-elf-ar

SFLAGS  := -Wall

BUILD   := build
OBJS    := $(BUILD)/event_flag.o \
           $(BUILD)/spu_thread_exit.o

LIB     := $(BUILD)/libsputhread.a

.PHONY: all install clean

all: $(LIB)

$(LIB): $(OBJS)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(OBJS)

$(BUILD)/%.o: src/%.S | $(BUILD)
	@echo "  AS    $(notdir $<)"
	@$(CC) $(SFLAGS) -c $< -o $@

$(BUILD):
	@mkdir -p $@

install: $(LIB)
	@install -d $(INSTALL_LIB) $(INSTALL_INC)/sys
	@install -m 0644 $(LIB) $(INSTALL_LIB)/libsputhread.a
	@echo "  lib   libsputhread.a"
	@install -m 0644 include/sys/event_flag.h $(INSTALL_INC)/sys/event_flag.h
	@echo "  header sys/event_flag.h"
	@install -m 0644 include/sys/spu_thread.h $(INSTALL_INC)/sys/spu_thread.h
	@echo "  header sys/spu_thread.h"
	@install -m 0644 include/spu_printf.h $(INSTALL_INC)/spu_printf.h
	@echo "  header spu_printf.h"
	@install -m 0644 ../include/sys/return_code.h $(INSTALL_INC)/sys/return_code.h
	@echo "  header sys/return_code.h (SPU)"

clean:
	@rm -rf $(BUILD)
