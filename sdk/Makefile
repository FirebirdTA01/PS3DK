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
INSTALL_INC := $(PS3DK)/ppu/include

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
    include/cell/spurs/task_types.h \
    include/cell/spurs/task.h \
    include/cell/spurs/workload.h \
    include/cell/spurs/barrier.h \
    include/cell/spurs/event_flag.h \
    include/cell/spurs/queue.h \
    include/cell/spurs/semaphore.h \
    include/sys/spu_thread.h \
    include/sys/spu_thread_group.h \
    include/spu_printf.h \
    include/simdmath.h \
    include/sys/cdefs.h \
    include/sys/event.h \
    include/sys/sce_cdefs.h \
    include/sysutil/sysutil_common.h \
    include/cell/keyboard.h \
    include/cell/mouse.h

SUBLIBS := libgcm_cmd libdbgfont libspurs_task libsputhread

.PHONY: all install clean version-header $(SUBLIBS)

all: $(SUBLIBS)

install: install-headers install-version install-sublibs

install-headers:
	@for h in $(HEADERS); do \
	    rel="$${h#include/}"; \
	    dst="$(INSTALL_INC)/$$rel"; \
	    install -d "$$(dirname $$dst)"; \
	    install -m 0644 "$$h" "$$dst"; \
	    echo "  header  $$rel"; \
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

install-sublibs:
	@for lib in $(SUBLIBS); do \
	    echo "  sublib  $$lib"; \
	    $(MAKE) -C $$lib install; \
	done

$(SUBLIBS):
	$(MAKE) -C $@

clean:
	@for lib in $(SUBLIBS); do $(MAKE) -C $$lib clean; done
