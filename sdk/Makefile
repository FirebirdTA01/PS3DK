# sdk/Makefile
#
# Top-level SDK build/install.  From here you can build every piece
# of our SDK with a single `make install`:
#
#   - header-only portions (sys/*, sysutil/*, cell/pad.h, etc.)
#     lay down into $(PS3DEV)/psl1ght/ppu/include/ directly.
#   - libgcm_cmd/ ships its own Makefile and is invoked recursively.
#
# This is the user-facing entry point — no separate bridging scripts.
# When we cut PSL1GHT loose, the install prefix migrates to our own
# tree but the contract here stays the same.

PS3DEV      ?= $(error PS3DEV is not set — run: source scripts/env.sh)
PS3DK       ?= $(PS3DEV)/ps3dk
INSTALL_INC     := $(PS3DK)/ppu/include
INSTALL_INC_SPU := $(PS3DK)/spu/include

# Path to the version generator (relative to this Makefile).
VERSION_SH  := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../scripts/version.sh)

HEADERS := \
    include/types.h \
    include/sys/lv2_types.h \
    include/sys/lv2_syscall.h \
    include/sys/spu_image.h \
    include/sys/timer.h \
    include/sys/sys_time.h \
    include/sys/return_code.h \
    include/sys/thread.h \
    include/sys/ppu_thread.h \
    include/sys/process.h \
    include/sys/stat.h \
    include/sys/synchronization.h \
    include/sys/time_util.h \
    include/sys/paths.h \
    include/sysutil/sysutil_sysparam.h \
    include/cell/cell.h \
    include/cell/pad.h \
    include/cell/error.h \
    include/cell/gcm.h \
    include/cell/gcm/gcm_enum.h \
    include/cell/gcm/gcm_cmd_overloads.h \
    include/cell/gcm/gcm_cg_bridge.h \
    include/cell/gcm/gcm_tex_bridge.h \
    include/cell/gcm/gcm_command_c.h \
    include/cell/cell_video_out.h \
    include/cell/cell_audio_out.h \
    include/cell/cell_fs.h \
    include/cell/fs/cell_fs_file_api.h \
    include/cell/fs/cell_fs_errno.h \
    include/cell/dbgfont.h \
    include/cell/sysutil.h \
    include/cell/msg_dialog.h \
    include/cell/sysutil_sysparam.h \
    include/cell/sysutil_savedata.h \
    include/cell/sysutil_gamecontent.h \
    include/cell/sysutil_screenshot.h \
    include/cell/sysutil_oskdialog.h \
    include/cell/sysutil_bgmplayback.h \
    include/cell/sysutil_syscache.h \
    include/cell/sysutil_sysconf.h \
    include/cell/sysutil_storagedata.h \
    include/cell/sysutil_avc.h \
    include/cell/sysutil_game_internal.h \
    include/cell/rtc.h \
    include/cell/sync.h \
    include/cell/sync/barrier.h \
    include/cell/sync2.h \
    include/cell/sysutil_ap.h \
    include/cell/audio.h \
    include/cell/pngdec.h \
    include/cell/sysmodule.h \
    include/cell/l10n.h \
    include/cell/sysutil_subdisplay.h \
    include/cell/sysutil_music.h \
    include/cell/sysutil_music_decode.h \
    include/cell/sysutil_music_export.h \
    include/cell/sysutil_imejp.h \
    include/cell/codec/jpgdec.h \
    include/cell/cgb.h \
    include/cell/spurs.h \
    include/cell/spurs/types.h \
    include/cell/spurs/common.h \
    include/cell/spurs/task_types.h \
    include/cell/spurs/task.h \
    include/cell/spurs/workload.h \
    include/cell/spurs/barrier.h \
    include/cell/spurs/event_flag.h \
    include/cell/spurs/queue.h \
    include/cell/spurs/semaphore.h \
    include/cell/spurs/exception_types.h \
    include/cell/spurs/version.h \
    include/cell/spurs/error.h \
    include/cell/spurs/job_commands.h \
    include/cell/spurs/job_descriptor.h \
    include/cell/spurs/job_guard.h \
    include/cell/spurs/job_chain_types.h \
    include/cell/spurs/job_chain.h \
    include/cell/spurs/job_queue_define.h \
    include/cell/spurs/job_queue_types.h \
    include/cell/spurs/job_queue_cpp_types.h \
    include/cell/spurs/job_queue_port_types.h \
    include/cell/spurs/job_queue_port_cpp_types.h \
    include/cell/spurs/job_queue_port2_types.h \
    include/cell/spurs/job_queue_port2_cpp_types.h \
    include/cell/spurs/job_queue.h \
    include/cell/spurs/job_queue_port.h \
    include/cell/spurs/job_queue_port2.h \
    include/cell/spurs/job_queue_semaphore.h \
    include/cell/fiber.h \
    include/cell/fiber/error.h \
    include/cell/fiber/version.h \
    include/cell/fiber/ppu_context_types.h \
    include/cell/fiber/ppu_fiber_types.h \
    include/cell/fiber/ppu_fiber_trace_types.h \
    include/cell/fiber/user_scheduler_types.h \
    include/cell/fiber/ppu_initialize.h \
    include/cell/fiber/ppu_context.h \
    include/cell/fiber/ppu_fiber.h \
    include/cell/fiber/user_scheduler.h \
    include/cell/fiber/ppu_fiber_worker_control.h \
    include/cell/usbd.h \
    include/cell/usbd/usb.h \
    include/cell/usbd/libusbd.h \
    include/cell/usbd/error.h \
    include/cell/codec/dmux.h \
    include/cell/codec/dmux_pamf.h \
    include/cell/codec/types.h \
    include/cell/codec/pamf.h \
    include/cell/codec/adec.h \
    include/cell/codec/adec_base.h \
    include/cell/vpost.h \
    include/cell/sysutil_oskdialog_ext.h \
    include/cell/sysutil_search.h \
    include/cell/sysutil_search_types.h \
    include/sys/crashdump.h \
    include/cell/trace/trace_types.h \
    include/sys/spu_initialize.h \
    include/sys/spu_thread.h \
    include/sys/spu_thread_group.h \
    include/sys/spu_utility.h \
    include/spu_printf.h \
    include/simdmath.h \
    include/sdk_version.h \
    include/vectormath/cpp/vectormath_soa.h \
    include/sys/cdefs.h \
    include/sys/event.h \
    include/sys/sce_cdefs.h \
    include/sysutil/sysutil_common.h \
    include/cell/keyboard.h \
    include/cell/mouse.h \
    include/cell/padfilter.h

SUBLIBS := libgcm_cmd libdbgfont libspurs_task libsputhread libspurs_job libspurs_jq

# Headers shared across PPU + SPU sides - the SPU job runtime needs to
# see the same CellSpursJobHeader / CellSpursJobContext2 / error-code
# definitions the PPU control surface uses.  These get installed into
# both $(PS3DK)/ppu/include/ (via HEADERS) and $(PS3DK)/spu/include/
# (via this list).
SHARED_SPURS_SPU_HEADERS := \
    include/cell/error.h \
    include/cell/spurs/types.h \
    include/cell/spurs/common.h \
    include/cell/spurs/exception_types.h \
    include/cell/spurs/job_descriptor.h \
    include/cell/spurs/job_commands.h \
    include/cell/spurs/job_chain_types.h \
    include/cell/spurs/error.h \
    include/cell/spurs/version.h \
    include/cell/spurs/job_queue_define.h \
    include/cell/spurs/job_queue_types.h \
    include/cell/spurs/job_queue_cpp_types.h \
    include/cell/spurs/job_queue_port_types.h \
    include/cell/spurs/job_queue_port_cpp_types.h \
    include/cell/spurs/job_queue_port2_types.h \
    include/cell/spurs/job_queue_port2_cpp_types.h \
    include/cell/spurs/job_queue.h \
    include/cell/spurs/job_queue_port.h \
    include/cell/spurs/job_queue_port2.h \
    include/cell/spurs/job_queue_semaphore.h \
    include/cell/trace/trace_types.h \
    include/vectormath/cpp/vectormath_soa.h

# SPU-side headers: SIMD class wrappers (simd + bits/simd_*.h).
# Reference SDK convention is `target/spu/include/{simd,bits/simd_*.h}` and
# we mirror that under $(PS3DK)/spu/include/.
SPU_HEADERS := \
    include-spu/simd \
    include-spu/bits/simd_scalars.h \
    include-spu/bits/simd_bool2.h \
    include-spu/bits/simd_bool4.h \
    include-spu/bits/simd_bool8.h \
    include-spu/bits/simd_bool16.h \
    include-spu/bits/simd_char16.h \
    include-spu/bits/simd_uchar16.h \
    include-spu/bits/simd_short8.h \
    include-spu/bits/simd_ushort8.h \
    include-spu/bits/simd_int4.h \
    include-spu/bits/simd_uint4.h \
    include-spu/bits/simd_llong2.h \
    include-spu/bits/simd_ullong2.h \
    include-spu/bits/simd_float4.h \
    include-spu/bits/simd_double2.h \
    include-spu/cell/dma.h \
    include-spu/cell/sync.h \
    include-spu/cell/sync/barrier.h

.PHONY: all install clean version-header $(SUBLIBS)

all: $(SUBLIBS)

install: install-headers install-spu-headers install-shared-spurs-spu-headers install-version install-sublibs

install-headers:
	@for h in $(HEADERS); do \
	    rel="$${h#include/}"; \
	    dst="$(INSTALL_INC)/$$rel"; \
	    install -d "$$(dirname $$dst)"; \
	    install -m 0644 "$$h" "$$dst"; \
	    echo "  header  $$rel"; \
	done

install-spu-headers:
	@for h in $(SPU_HEADERS); do \
	    rel="$${h#include-spu/}"; \
	    dst="$(INSTALL_INC_SPU)/$$rel"; \
	    install -d "$$(dirname $$dst)"; \
	    install -m 0644 "$$h" "$$dst"; \
	    echo "  spu hdr $$rel"; \
	done

install-shared-spurs-spu-headers:
	@for h in $(SHARED_SPURS_SPU_HEADERS); do \
	    rel="$${h#include/}"; \
	    dst="$(INSTALL_INC_SPU)/$$rel"; \
	    install -d "$$(dirname $$dst)"; \
	    install -m 0644 "$$h" "$$dst"; \
	    echo "  shared  $$rel (spu)"; \
	done

# cell/sdk_version.h is generated from the current git state at install
# time so every consumer sees the same version macros that the rest of
# the SDK was tagged at.  Also write a plain-text VERSION file at the
# install root for human/script consumers.
install-version:
	@install -d "$(INSTALL_INC)/cell"
	@"$(VERSION_SH)" --format=header > "$(INSTALL_INC)/cell/sdk_version.h"
	@echo "  header  cell/sdk_version.h (generated)"
	@install -d "$(PS3DK)"
	@"$(VERSION_SH)" --format=plain > "$(PS3DK)/VERSION"
	@echo "  marker  $(PS3DK)/VERSION ($$(cat $(PS3DK)/VERSION))"

# Bail on first sublib failure.  A bare `for ... done` loop's exit
# status is the last iteration's, which silently masks earlier
# failures: a libgcm_cmd build break with libspurs_job succeeding
# afterwards looks like "make install" returning 0.  Explicit
# `|| exit 1` makes the loop fail-fast.
install-sublibs:
	@for lib in $(SUBLIBS); do \
	    echo "  sublib  $$lib"; \
	    $(MAKE) -C $$lib install || exit 1; \
	done

$(SUBLIBS):
	$(MAKE) -C $@

# libdbgfont's sources transitively include <Cg/cg.h>, which is
# installed by libgcm_cmd.  Force the order so a parallel `make all`
# can't race.
libdbgfont: libgcm_cmd

clean:
	@for lib in $(SUBLIBS); do $(MAKE) -C $$lib clean || exit 1; done
