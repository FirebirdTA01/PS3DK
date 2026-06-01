# libcgb.a -- Cg binary reader archive

CROSS    ?= powerpc64-ps3-elf-
CC       := $(CROSS)gcc
AR       := $(CROSS)ar
RANLIB   := $(CROSS)ranlib
PS3DEV   ?= $(error PS3DEV is not set. Source scripts/env.sh first)
PS3DK    ?= $(PS3DEV)/ps3dk

CFLAGS   := -O2 -Wall -Wextra -Wno-unused-parameter \
            -mcpu=cell -std=c11 \
            -I../include \
            -I$(PS3DK)/ppu/include
DEPFLAGS := -MMD -MP
ARFLAGS  := rcs

SRC_DIR  := src
OBJ_DIR  := build

SRCS     := $(wildcard $(SRC_DIR)/*.c)
OBJS     := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

ARCHIVE  := $(OBJ_DIR)/libcgb.a

LP64_OBJ_DIR  := $(OBJ_DIR)-lp64
LP64_ARCHIVE  := $(LP64_OBJ_DIR)/libcgb.a
LP64_OBJS     := $(patsubst $(SRC_DIR)/%.c,$(LP64_OBJ_DIR)/%.o,$(SRCS))
LP64_CFLAGS   := $(CFLAGS) -mlp64

INSTALL_LIB      := $(PS3DK)/ppu/lib
INSTALL_LP64_LIB := $(INSTALL_LIB)/lp64

.PHONY: all clean install

all: $(ARCHIVE) $(LP64_ARCHIVE)

$(ARCHIVE): $(OBJS)
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

$(LP64_ARCHIVE): $(LP64_OBJS)
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(LP64_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(LP64_OBJ_DIR)
	$(CC) $(LP64_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(OBJ_DIR) $(LP64_OBJ_DIR):
	mkdir -p $@

install: $(ARCHIVE) $(LP64_ARCHIVE)
	install -d $(INSTALL_LIB) $(INSTALL_LP64_LIB)
	install -m 0644 $(ARCHIVE) $(INSTALL_LIB)/libcgb.a
	install -m 0644 $(ARCHIVE) $(INSTALL_LIB)/libcgbLevelC.a
	install -m 0644 $(LP64_ARCHIVE) $(INSTALL_LP64_LIB)/libcgb.a
	install -m 0644 $(LP64_ARCHIVE) $(INSTALL_LP64_LIB)/libcgbLevelC.a

clean:
	rm -rf $(OBJ_DIR) $(LP64_OBJ_DIR)

-include $(wildcard $(OBJ_DIR)/*.d)
-include $(wildcard $(LP64_OBJ_DIR)/*.d)
