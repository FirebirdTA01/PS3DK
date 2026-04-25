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

$(LIB): $(OBJS) | $(BUILD)
	@echo "  AR    $(notdir $@)"
	@$(AR) rcs $@ $(OBJS)
	@# Bundle in PSL1GHT's _spu_call_event_va_arg.o (used by spu_printf
	@# macro expansion).  Lets samples that call spu_printf() link
	@# against -lsputhread alone without needing PSL1GHT's full
	@# libsputhread.a on the link line - our archive becomes the
	@# canonical SPU lv2-syscall lib in the install tree.
	@if [ -f "$(PSL1GHT_LIBSPUTHREAD)" ]; then \
	    tmp_dir="$(BUILD)/_psl1ght_extract"; \
	    mkdir -p "$$tmp_dir"; \
	    (cd "$$tmp_dir" && $(AR) x "$(PSL1GHT_LIBSPUTHREAD)" spu_call_event_va_arg.o 2>/dev/null) || true; \
	    if [ -f "$$tmp_dir/spu_call_event_va_arg.o" ]; then \
	        $(AR) r $@ "$$tmp_dir/spu_call_event_va_arg.o" 2>/dev/null; \
	        echo "  AR    + spu_call_event_va_arg.o (from PSL1GHT)"; \
	    fi; \
	fi

PSL1GHT_LIBSPUTHREAD := $(PS3DEV)/psl1ght/spu/lib/libsputhread.a

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
