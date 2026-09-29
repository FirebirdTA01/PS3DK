# sdk/libsync2/Makefile -- SPU-side cellSync2 runtime.
#
# Outputs:
#   libsync2.a   -> $(SPU_PREFIX)/spu-elf/lib/ (canonical)
#                -> $(PS3DK)/spu/lib/           (compatibility mirror)
#
# The SPU half of cellSync2: mutex, semaphore, condition variable and queue
# over the shared object lines and waiting queues the PPU system module also
# operates on, plus the SPURS task thread type and the notifiers that wake
# PPU threads, PPU fibers, SPURS tasks and job queue jobs.  One object per public symbol so a link pulls only what
# it names.
#
# Built with spu-elf-gcc.

PS3DEV     ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK      ?= $(PS3DEV)/ps3dk
SPU_PREFIX ?= $(PS3DEV)/spu

INSTALL_LIB := $(SPU_PREFIX)/spu-elf/lib
INSTALL_LIB_MIRROR := $(PS3DK)/spu/lib

override CC := spu-elf-gcc
AR := spu-elf-ar

CFLAGS := -O2 -Wall -Wextra -fno-exceptions -ffreestanding -std=gnu99           -I../libspurs_task/include -I../include-spu -I../include -I../libsputhread/include

BUILD := build
OBJS  := $(addprefix $(BUILD)/,lock_line.o waiting_queue.o mutex.o            mutex_lock.o mutex_trylock.o mutex_unlock.o            semaphore.o semaphore_acquire.o semaphore_try_acquire.o semaphore_release.o semaphore_get_count.o            cond.o cond_wait.o cond_signal.o cond_signal_all.o            queue.o queue_push.o queue_try_push.o queue_pop.o queue_try_pop.o queue_get_size.o queue_get_depth.o            spurs_task_thread.o spurs_task_notifier.o thread_config_spurs_task.o            ppu_thread_notifier.o ppu_fiber_notifier.o spurs_jqjob_notifier.o)
LIB   := $(BUILD)/libsync2.a

.PHONY: all install clean

all: $(LIB)

$(LIB): $(OBJS)
	@echo "  AR    $(notdir $@)"
	@rm -f $@
	@$(AR) rcs $@ $(OBJS)

$(BUILD)/%.o: src/%.c src/sync2_internal.h | $(BUILD)
	@echo "  CC    $(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	@mkdir -p $@

install: $(LIB)
	@install -d $(INSTALL_LIB) $(INSTALL_LIB_MIRROR)
	@install -m 0644 $(LIB) $(INSTALL_LIB)/libsync2.a
	@install -m 0644 $(LIB) $(INSTALL_LIB_MIRROR)/libsync2.a
	@echo "  lib    libsync2.a"

clean:
	@rm -rf $(BUILD)
