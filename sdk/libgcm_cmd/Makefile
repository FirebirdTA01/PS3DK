# sdk/libgcm_cmd/Makefile
#
# Builds libgcm_cmd.a — our independent implementation of the
# cellGcmCg* struct-walker API over the CgBinaryProgram blob format.
# See include/Cg/cgBinary.h for the on-disk layout we target.
#
# Requires scripts/env.sh to be sourced so $PS3DEV / PPU toolchain
# binaries are on PATH.  Install target drops headers into
# $PS3DEV/psl1ght/ppu/include/ and the archive alongside our other
# PSL1GHT-resident libs.

CROSS    ?= powerpc64-ps3-elf-
CC       := $(CROSS)gcc
AR       := $(CROSS)ar
RANLIB   := $(CROSS)ranlib

PS3DEV   ?= $(error PS3DEV is not set — run: source scripts/env.sh)

CFLAGS   := -O2 -Wall -Wextra -Wno-unused-parameter \
            -mcpu=cell -std=c11 \
            -I$(PS3DK)/ppu/include \
            -Iinclude
# -MMD/-MP emit per-object .d files.  Without these, editing a header
# like sdk/include/sys/lv2_types.h leaves the dependent .o stale; make
# sees matching timestamps and the rebuild silently skips the affected
# translation units.  Bit us once on lv2_fn_to_callback_ea.
DEPFLAGS := -MMD -MP
ARFLAGS  := rcs

SRC_DIR  := src
OBJ_DIR  := build
SRCS     := $(wildcard $(SRC_DIR)/*.c)
OBJS     := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

# Absorbed rsx emitters (source-level snapshot of PSL1GHT's librsx,
# MIT-licensed — see src/rsx_legacy/LICENSE).  These land in the same
# libgcm_cmd.a so samples drop -lrsx and match the platform ABI's layout
# (one libgcm_cmd.a, no separate rsx library).
LEGACY_DIR  := $(SRC_DIR)/rsx_legacy
LEGACY_SRCS := $(wildcard $(LEGACY_DIR)/*.c)
LEGACY_OBJS := $(patsubst $(LEGACY_DIR)/%.c,$(OBJ_DIR)/rsx_legacy_%.o,$(LEGACY_SRCS))

# rsx_legacy needs PSL1GHT's internal header layout: its commands.c
# includes <nv40.h> bare (not <rsx/nv40.h>) and expects LIBRSX_INTERNAL
# defined to unlock the function-pointer side of rsxSetUserCallback.
LEGACY_CFLAGS := -O2 -Wall -mcpu=cell -mregnames -Wa,-mcell -std=gnu11 \
                 -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections \
                 -Wno-unused-parameter -Wno-implicit-fallthrough \
                 -Wno-implicit-function-declaration \
                 -I$(PS3DK)/ppu/include \
                 -I$(PS3DK)/ppu/include/rsx \
                 -Iinclude \
                 -DLIBRSX_INTERNAL

ARCHIVE        := $(OBJ_DIR)/libgcm_cmd.a
PS3DK          ?= $(PS3DEV)/ps3dk
INSTALL_LIB    := $(PS3DK)/ppu/lib
INSTALL_INC    := $(PS3DK)/ppu/include

# --- LP64 (-mlp64) multilib (after all ILP32 := expansions; before targets) ---
LP64_OBJ_DIR       := $(OBJ_DIR)-lp64
LP64_ARCHIVE       := $(LP64_OBJ_DIR)/libgcm_cmd.a
LP64_OBJS          := $(patsubst $(SRC_DIR)/%.c,$(LP64_OBJ_DIR)/%.o,$(SRCS))
LP64_LEGACY_OBJS   := $(patsubst $(LEGACY_DIR)/%.c,$(LP64_OBJ_DIR)/rsx_legacy_%.o,$(LEGACY_SRCS))
LP64_CFLAGS        := $(CFLAGS) -mlp64
LP64_LEGACY_CFLAGS := $(LEGACY_CFLAGS) -mlp64

.PHONY: all install clean

all: $(ARCHIVE) $(LP64_ARCHIVE)

$(ARCHIVE): $(OBJS) $(LEGACY_OBJS)
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

$(LP64_ARCHIVE): $(LP64_OBJS) $(LP64_LEGACY_OBJS)
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(OBJ_DIR)/rsx_legacy_%.o: $(LEGACY_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(LEGACY_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(OBJ_DIR):
	@mkdir -p $@

$(LP64_OBJ_DIR):
	@mkdir -p $@

$(LP64_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(LP64_OBJ_DIR)
	$(CC) $(LP64_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(LP64_OBJ_DIR)/rsx_legacy_%.o: $(LEGACY_DIR)/%.c | $(LP64_OBJ_DIR)
	$(CC) $(LP64_LEGACY_CFLAGS) $(DEPFLAGS) -c -o $@ $<

install: $(ARCHIVE) $(LP64_ARCHIVE)
	@install -d $(INSTALL_LIB)
	install -m 0644 $(ARCHIVE) $(INSTALL_LIB)/libgcm_cmd.a
	@install -d $(INSTALL_LIB)/lp64
	install -m 0644 $(LP64_ARCHIVE) $(INSTALL_LIB)/lp64/libgcm_cmd.a
	@install -d $(INSTALL_INC)/cell/gcm
	install -m 0644 include/cell/gcm/gcm_cg_func.h    $(INSTALL_INC)/cell/gcm/gcm_cg_func.h
	install -m 0644 include/cell/gcm/ps3tc_fifo_wrap.h $(INSTALL_INC)/cell/gcm/ps3tc_fifo_wrap.h
	@echo "installed libgcm_cmd.a + headers under $(PS3DK)/ppu/"
	@echo "REPLACING ppu/lib/libgcm_cmd.a with sdk/libgcm_cmd (vendored copy at ppu/lib/libgcm_cmd.a is NOT installed)"
	@echo "REPLACING ppu/lib/lp64/libgcm_cmd.a with sdk/libgcm_cmd (vendored copy at ppu/lib/lp64/libgcm_cmd.a is NOT installed)"

clean:
	rm -rf $(OBJ_DIR) $(LP64_OBJ_DIR)

# --- Observed FIFO wrap variant (docs/design/ps3dk-prof-fifo.md) ---
# libgcm_cmd_prof_fifo.a, per data model: the default members with
# ps3tc_fifo_wrap.o replaced by the same source compiled with
# PS3TC_FIFO_OBSERVE (the six phase-boundary observation points and the
# private observer bridge).  Explicit goals only: no prerequisite of all,
# install or clean, and its own object directories, so the default archives
# and their build plan are unchanged.  The shared default objects are
# prerequisites, never rewritten here.
FIFO_OBJ_DIR      := $(OBJ_DIR)-fifo
FIFO_LP64_OBJ_DIR := $(OBJ_DIR)-fifo-lp64
FIFO_ARCHIVE      := $(FIFO_OBJ_DIR)/libgcm_cmd_prof_fifo.a
FIFO_LP64_ARCHIVE := $(FIFO_LP64_OBJ_DIR)/libgcm_cmd_prof_fifo.a
FIFO_OBJS         := $(filter-out $(OBJ_DIR)/ps3tc_fifo_wrap.o,$(OBJS)) $(FIFO_OBJ_DIR)/ps3tc_fifo_wrap.o
FIFO_LP64_OBJS    := $(filter-out $(LP64_OBJ_DIR)/ps3tc_fifo_wrap.o,$(LP64_OBJS)) $(FIFO_LP64_OBJ_DIR)/ps3tc_fifo_wrap.o

.PHONY: fifo install-fifo fifo-clean
fifo: $(FIFO_ARCHIVE) $(FIFO_LP64_ARCHIVE)

$(FIFO_OBJ_DIR)/ps3tc_fifo_wrap.o: $(SRC_DIR)/ps3tc_fifo_wrap.c Makefile
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DPS3TC_FIFO_OBSERVE=1 $(DEPFLAGS) -c -o $@ $<

$(FIFO_LP64_OBJ_DIR)/ps3tc_fifo_wrap.o: $(SRC_DIR)/ps3tc_fifo_wrap.c Makefile
	@mkdir -p $(@D)
	$(CC) $(LP64_CFLAGS) -DPS3TC_FIFO_OBSERVE=1 $(DEPFLAGS) -c -o $@ $<

$(FIFO_ARCHIVE): $(FIFO_OBJS) $(LEGACY_OBJS)
	@rm -f $@
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

$(FIFO_LP64_ARCHIVE): $(FIFO_LP64_OBJS) $(LP64_LEGACY_OBJS)
	@rm -f $@
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

install-fifo: fifo
	@install -d $(INSTALL_LIB) $(INSTALL_LIB)/lp64
	install -m 0644 $(FIFO_ARCHIVE) $(INSTALL_LIB)/libgcm_cmd_prof_fifo.a
	install -m 0644 $(FIFO_LP64_ARCHIVE) $(INSTALL_LIB)/lp64/libgcm_cmd_prof_fifo.a
	cmp $(FIFO_ARCHIVE) $(INSTALL_LIB)/libgcm_cmd_prof_fifo.a
	cmp $(FIFO_LP64_ARCHIVE) $(INSTALL_LIB)/lp64/libgcm_cmd_prof_fifo.a

fifo-clean:
	rm -f $(FIFO_OBJ_DIR)/ps3tc_fifo_wrap.o $(FIFO_OBJ_DIR)/ps3tc_fifo_wrap.d $(FIFO_ARCHIVE)
	rm -f $(FIFO_LP64_OBJ_DIR)/ps3tc_fifo_wrap.o $(FIFO_LP64_OBJ_DIR)/ps3tc_fifo_wrap.d $(FIFO_LP64_ARCHIVE)

-include $(wildcard $(OBJ_DIR)/*.d)
-include $(wildcard $(FIFO_OBJ_DIR)/*.d $(FIFO_LP64_OBJ_DIR)/*.d)

# AUTO uses the original default members except the canonical command,
# publication and wrap objects. Compile those three together with both seams;
# never compose two independently observed GCM archives.
AUTO_OBJ_DIR := $(OBJ_DIR)-auto
AUTO_LP64_OBJ_DIR := $(OBJ_DIR)-auto-lp64
AUTO_REPLACED := ps3tc_fifo_wrap gcm_segment rsx_legacy_commands rsx_legacy_buffer
AUTO_OBJS := $(filter-out $(addprefix $(OBJ_DIR)/,$(addsuffix .o,$(AUTO_REPLACED))),$(OBJS) $(LEGACY_OBJS)) \
             $(addprefix $(AUTO_OBJ_DIR)/,$(addsuffix .o,$(AUTO_REPLACED)))
AUTO_LP64_OBJS := $(filter-out $(addprefix $(LP64_OBJ_DIR)/,$(addsuffix .o,$(AUTO_REPLACED))),$(LP64_OBJS) $(LP64_LEGACY_OBJS)) \
                  $(addprefix $(AUTO_LP64_OBJ_DIR)/,$(addsuffix .o,$(AUTO_REPLACED)))
AUTO_FLAGS := -DPS3TC_FIFO_OBSERVE=1 -DPS3TC_PROF_AUTO=1
.PHONY: auto install-auto
auto: $(AUTO_OBJ_DIR)/libgcm_cmd_prof_auto.a $(AUTO_LP64_OBJ_DIR)/libgcm_cmd_prof_auto.a

$(AUTO_OBJ_DIR)/ps3tc_fifo_wrap.o: $(SRC_DIR)/ps3tc_fifo_wrap.c Makefile
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(AUTO_FLAGS) $(DEPFLAGS) -c -o $@ $<
$(AUTO_LP64_OBJ_DIR)/ps3tc_fifo_wrap.o: $(SRC_DIR)/ps3tc_fifo_wrap.c Makefile
	@mkdir -p $(@D)
	$(CC) $(LP64_CFLAGS) $(AUTO_FLAGS) $(DEPFLAGS) -c -o $@ $<
$(AUTO_OBJ_DIR)/gcm_segment.o: $(SRC_DIR)/gcm_segment.c Makefile
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(AUTO_FLAGS) $(DEPFLAGS) -c -o $@ $<
$(AUTO_LP64_OBJ_DIR)/gcm_segment.o: $(SRC_DIR)/gcm_segment.c Makefile
	@mkdir -p $(@D)
	$(CC) $(LP64_CFLAGS) $(AUTO_FLAGS) $(DEPFLAGS) -c -o $@ $<
$(AUTO_OBJ_DIR)/rsx_legacy_%.o: $(LEGACY_DIR)/%.c Makefile
	@mkdir -p $(@D)
	$(CC) $(LEGACY_CFLAGS) $(AUTO_FLAGS) $(DEPFLAGS) -c -o $@ $<
$(AUTO_LP64_OBJ_DIR)/rsx_legacy_%.o: $(LEGACY_DIR)/%.c Makefile
	@mkdir -p $(@D)
	$(CC) $(LP64_LEGACY_CFLAGS) $(AUTO_FLAGS) $(DEPFLAGS) -c -o $@ $<
$(AUTO_OBJ_DIR)/libgcm_cmd_prof_auto.a: $(AUTO_OBJS)
	@rm -f $@
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@
$(AUTO_LP64_OBJ_DIR)/libgcm_cmd_prof_auto.a: $(AUTO_LP64_OBJS)
	@rm -f $@
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@
install-auto: auto
	install -d $(INSTALL_LIB) $(INSTALL_LIB)/lp64 $(INSTALL_INC)/cell/gcm
	install -m 0644 $(AUTO_OBJ_DIR)/libgcm_cmd_prof_auto.a $(INSTALL_LIB)/
	install -m 0644 $(AUTO_LP64_OBJ_DIR)/libgcm_cmd_prof_auto.a $(INSTALL_LIB)/lp64/
	cmp $(AUTO_OBJ_DIR)/libgcm_cmd_prof_auto.a $(INSTALL_LIB)/libgcm_cmd_prof_auto.a
	cmp $(AUTO_LP64_OBJ_DIR)/libgcm_cmd_prof_auto.a $(INSTALL_LIB)/lp64/libgcm_cmd_prof_auto.a
	install -m 0644 ../include/cell/gcm.h $(INSTALL_INC)/cell/
	install -m 0644 include/cell/gcm/ps3tc_fifo_wrap.h $(INSTALL_INC)/cell/gcm/
-include $(wildcard $(AUTO_OBJ_DIR)/*.d $(AUTO_LP64_OBJ_DIR)/*.d)
