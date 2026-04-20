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
INSTALL_INC := $(PS3DEV)/psl1ght/ppu/include

HEADERS := \
    include/sys/timer.h \
    include/sys/sys_time.h \
    include/sys/return_code.h \
    include/sys/thread.h \
    include/sys/ppu_thread.h \
    include/sys/process.h \
    include/sys/stat.h \
    include/sys/synchronization.h \
    include/sys/time_util.h \
    include/sysutil/sysutil_sysparam.h \
    include/cell/pad.h \
    include/cell/error.h \
    include/cell/gcm.h \
    include/cell/gcm/gcm_enum.h \
    include/cell/gcm/gcm_cmd_overloads.h \
    include/cell/gcm/gcm_cg_bridge.h \
    include/cell/gcm/gcm_tex_bridge.h \
    include/cell/gcm/gcm_command_c.h \
    include/cell/cell_video_out.h \
    include/cell/dbgfont.h \
    include/cell/sysutil.h \
    include/cell/msg_dialog.h \
    include/cell/sysutil_sysparam.h \
    include/cell/sysutil_savedata.h \
    include/cell/sysutil_gamecontent.h \
    include/cell/sysutil_screenshot.h \
    include/cell/sysutil_ap.h \
    include/cell/sysmodule.h \
    include/cell/l10n.h \
    include/cell/sysutil_subdisplay.h \
    include/cell/sysutil_music.h \
    include/cell/sysutil_music_decode.h \
    include/cell/sysutil_music_export.h \
    include/cell/sysutil_imejp.h

SUBLIBS := libgcm_cmd libdbgfont

.PHONY: all install clean $(SUBLIBS)

all: $(SUBLIBS)

install: install-headers install-sublibs

install-headers:
	@for h in $(HEADERS); do \
	    rel="$${h#include/}"; \
	    dst="$(INSTALL_INC)/$$rel"; \
	    install -d "$$(dirname $$dst)"; \
	    install -m 0644 "$$h" "$$dst"; \
	    echo "  header  $$rel"; \
	done

install-sublibs:
	@for lib in $(SUBLIBS); do \
	    echo "  sublib  $$lib"; \
	    $(MAKE) -C $$lib install; \
	done

$(SUBLIBS):
	$(MAKE) -C $@

clean:
	@for lib in $(SUBLIBS); do $(MAKE) -C $$lib clean; done
