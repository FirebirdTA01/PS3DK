# sdk/libatomic/Makefile -- SPU out-of-line cellAtomic* functions.
#
# Outputs:
#   libatomic.a  -> $(SPU_PREFIX)/spu-elf/lib/ (canonical)
#                -> $(PS3DK)/spu/lib/           (compatibility mirror)
#
# <cell/atomic.h> keeps every operation inline; this archive adds one
# external definition per operation, one per member, for code that takes
# an operation's address or links -latomic.  src/atomic.c is compiled once
# per operation (see its header comment for the two definitions passed).

PS3DEV     ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
SPU_PREFIX ?= $(PS3DEV)/spu

INSTALL_LIB := $(SPU_PREFIX)/spu-elf/lib
INSTALL_LIB_MIRROR := $(PS3DK)/spu/lib

override CC := spu-elf-gcc
AR := spu-elf-ar

CFLAGS := -O2 -Wall -Wextra -Werror -fno-exceptions -ffreestanding \
          -I../include-spu \
          -I$(PS3DK)/spu/include

# Operation names in the order of their CELL_ATOMIC_OP numbers (1..20).
OPS := Add32 Sub32 And32 Or32 Store32 Incr32 Decr32 Nop32 TestAndDecr32 \
       CompareAndSwap32 \
       Add64 Sub64 And64 Or64 Store64 Incr64 Decr64 Nop64 TestAndDecr64 \
       CompareAndSwap64
op_index = $(strip $(foreach i,$(shell seq 1 $(words $(OPS))),$(if $(filter $(1),$(word $(i),$(OPS))),$(i))))

BUILD := build
OBJS  := $(foreach op,$(OPS),$(BUILD)/atomic_$(op).o)
LIB   := $(BUILD)/libatomic.a

.PHONY: all install clean

all: $(LIB)

$(LIB): $(OBJS)
	@echo "  AR    $(notdir $@)"
	@rm -f $@
	@$(AR) rcs $@ $(OBJS)

$(BUILD)/atomic_%.o: src/atomic.c | $(BUILD)
	@echo "  CC    atomic ($*)"
	@$(CC) $(CFLAGS) -DcellAtomic$*=__ps3dk_inline_cellAtomic$* \
	    -DCELL_ATOMIC_OP=$(call op_index,$*) -c $< -o $@

$(BUILD):
	@mkdir -p $@

install: $(LIB)
	@install -d $(INSTALL_LIB) $(INSTALL_LIB_MIRROR)
	@install -m 0644 $(LIB) $(INSTALL_LIB)/libatomic.a
	@install -m 0644 $(LIB) $(INSTALL_LIB_MIRROR)/libatomic.a
	@echo "  lib    libatomic.a"

clean:
	rm -rf $(BUILD)
