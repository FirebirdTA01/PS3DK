# sdk/Makefile
#
# Top-level SDK build/install.  From here you can build every piece
# of our SDK with a single `make install`:
#
#   - header-only portions (sys/*, sysutil/*, cell/pad.h, etc.)
#     lay down into $(PS3DEV)/psl1ght/ppu/include/ directly.
#   - libgcm_cmd/ ships its own Makefile and is invoked recursively.
#
# This is the user-facing entry point - no separate bridging scripts.
# When we cut PSL1GHT loose, the install prefix migrates to our own
# tree but the contract here stays the same.

PS3DEV      ?= $(error PS3DEV is not set - run: source scripts/env.sh)
PS3DK       ?= $(PS3DEV)/ps3dk
INSTALL_INC     := $(PS3DK)/ppu/include
INSTALL_INC_SPU := $(PS3DK)/spu/include
INSTALL_LIB      := $(PS3DK)/ppu/lib
INSTALL_LIB_LP64 := $(INSTALL_LIB)/lp64
INSTALL_MANIFEST := $(PS3DK)/.ps3dk-install-manifest

# Path to the version generator (relative to this Makefile).
VERSION_SH  := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../scripts/version.sh)
TOOLCHAIN_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)

HEADERS := \
    include/ppu-types.h \
    include/ppu-asm.h \
    include/stdarg.h \
    include/stdio.h \
    include/stdlib.h \
    include/string.h \
    include/types.h \
    include/cellstatus.h \
    include/gcm_tool.h \
    include/mt19937.h \
    include/mspace.h \
    include/np.h \
    include/sysutil_oskdialog.h \
    include/sys/lv2_types.h \
    include/sys/lv2_syscall.h \
    include/sys/ppu-asm.h \
    include/sys/heap.h \
    include/sys/spu_image.h \
    include/sys/raw_spu.h \
    include/sys/timer.h \
    include/sys/sys_time.h \
    include/sys/sys_types.h \
    include/sys/system_types.h \
    include/sys/types.h \
    include/sys/return_code.h \
    include/sys/thread.h \
    include/sys/ppu_thread.h \
    include/sys/fs.h \
    include/sys/fs_external.h \
    include/sys/fsw.h \
    include/sys/process.h \
    include/sys/memory.h \
    include/sys/dbg.h \
    include/sys/socket.h \
    include/sys/stat.h \
    include/sys/synchronization.h \
    include/sys/time_util.h \
    include/sys/paths.h \
    include/sysutil/sysutil_sysparam.h \
    include/sysutil/sysutil_savedata.h \
    include/sysutil/sysutil_msgdialog.h \
    include/sysutil/sysutil_userinfo.h \
    include/sysutil/sysutil_bgdl.h \
    include/sysutil/sysutil_subdisplay.h \
    include/sysutil/sysutil_screenshot.h \
    include/sysutil/sysutil_imejp.h \
    include/sysutil/sysutil_gamecontent.h \
    include/sysutil/sysutil_syscache.h \
    include/sysutil/sysutil_music_decode2.h \
    include/sysutil/sysutil_music_playback_common.h \
    include/sysutil/video.h \
    include/arpa/inet.h \
    include/netdb.h \
    include/netex/net.h \
    include/netex/sockinfo.h \
    include/netex/errno.h \
    include/netex/ifctl.h \
    include/netex/libnetctl.h \
    include/netinet/in.h \
    include/cell/cell.h \
    include/cell/atomic.h \
    include/cell/pad.h \
    include/cell/error.h \
    include/cell/gcm.h \
    include/cell/gcm_pm.h \
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
    include/cell/libnetctl.h \
    include/cell/libnet.h \
    include/cell/ssl.h \
    include/cell/ssl/ssl.h \
    include/cell/ssl/cert.h \
    include/cell/http.h \
    include/cell/http/error.h \
    include/cell/http/util.h \
    include/cell/http/init.h \
    include/cell/http/proxy.h \
    include/cell/http/cookie.h \
    include/cell/http/client.h \
    include/cell/http/transaction.h \
    include/cell/http/request.h \
    include/cell/http/response.h \
    include/cell/np.h \
    include/cell/np/common.h \
    include/cell/np/init.h \
    include/cell/np/basic.h \
    include/cell/np/friendlist.h \
    include/cell/np/manager.h \
    include/cell/np/signaling.h \
    include/cell/np/lookup.h \
    include/cell/np/profile.h \
    include/cell/np/drm.h \
    include/cell/np/custom_menu.h \
    include/cell/np/score.h \
    include/cell/np/commerce.h \
    include/cell/np/matching.h \
    include/cell/np/clans.h \
    include/cell/np/commerce2.h \
    include/cell/np/tus.h \
    include/cell/np/np2.h \
    include/np/matching2.h \
    include/np/commerce2.h \
    include/np/drm.h \
    include/np/trophy.h \
    include/np/clans.h \
    include/cell/np2/init.h \
    include/cell/np2/matching2.h \
    include/cell/np/trophy.h \
    include/cell/np/sns.h \
    include/cell/np/util.h \
    include/cell/msg_dialog.h \
    include/cell/sysutil_sysparam.h \
    include/cell/sysutil_savedata.h \
    include/cell/sysutil_gameupdate.h \
    include/cell/sysutil_userinfo.h \
    include/cell/libkey2char.h \
    include/cell/font.h \
    include/cell/fontFT.h \
    include/cell/font/error.h \
    include/cell/font/ps3fontset.h \
    include/cell/font/libfont.h \
    include/cell/font/libfontFT.h \
    include/cell/sysutil_bgdl.h \
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
    include/cell/face.h \
    include/cell/face/error.h \
    include/cell/face/face.h \
    include/cell/face/util.h \
    include/cell/rudp.h \
    include/cell/sync.h \
    include/cell/sync/barrier.h \
    include/cell/sync2.h \
    include/cell/sheap.h \
    include/cell/sheap/error.h \
    include/cell/sheap/sheap_base.h \
    include/cell/sheap/key_sheap.h \
    include/cell/sheap/key_sheap_barrier.h \
    include/cell/sheap/key_sheap_buffer.h \
    include/cell/sheap/key_sheap_mutex.h \
    include/cell/sheap/key_sheap_queue.h \
    include/cell/sheap/key_sheap_rwm.h \
    include/cell/sheap/key_sheap_semaphore.h \
    include/cell/sysutil_ap.h \
    include/cell/audio.h \
    include/cell/voice.h \
    include/cell/voiceTypes.h \
    include/cell/mic.h \
    include/cell/mic/define.h \
    include/cell/mic/error.h \
    include/cell/mic/interface.h \
    include/sysutil_avc2.h \
    include/sysutil_avc2_video.h \
    include/cell/sysutil_avc2.h \
    include/cell/sysutil_avc2_video.h \
    include/cell/camera.h \
    include/cell/resc.h \
    include/cell/ovis.h \
    include/cell/pngdec.h \
    include/cell/sysmodule.h \
    include/cell/l10n.h \
    include/cell/sysutil_subdisplay.h \
    include/cell/sysutil_music.h \
    include/cell/sysutil_music_decode.h \
    include/cell/sysutil_music_decode2.h \
    include/cell/sysutil_music_export.h \
    include/cell/sysutil_imejp.h \
    include/cell/codec.h \
    include/cell/codec/jpgdec.h \
    include/cell/cgb.h \
    include/cell/spurs.h \
    include/cell/spurs/types.h \
    include/cell/spurs/common.h \
    include/cell/spurs/control.h \
    include/cell/spurs/system_workload.h \
    include/cell/spurs/system_workload_types.h \
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
    include/hl/spurs.h \
    include/sce/hl/spurs/define.h \
    include/sce/hl/spurs/spurs.h \
    include/sce/hl/spurs/taskset.h \
    include/sce/hl/spurs/task_elf.h \
    include/sce/hl/spurs/task.h \
    include/sce/hl/spurs/event_flag.h \
    include/sce/hl/spurs/job.h \
    include/sce/hl/spurs/job_chain.h \
    include/sce/hl/spurs/job_guard.h \
    include/sce/hl/spurs/command_list_dispatcher.h \
    include/sce/hl/spurs/HLSpursDefine.h \
    include/sce/hl/spurs/HLSpurs.h \
    include/sce/hl/spurs/HLSpursTaskset.h \
    include/sce/hl/spurs/HLSpursTaskElf.h \
    include/sce/hl/spurs/HLSpursTask.h \
    include/sce/hl/spurs/HLSpursEventFlag.h \
    include/sce/hl/spurs/HLSpursJobDescriptor.h \
    include/sce/hl/spurs/HLSpursJobCommand.h \
    include/sce/hl/spurs/HLSpursJobNotification.h \
    include/sce/hl/spurs/HLSpursJobChain.h \
    include/sce/hl/spurs/HLSpursJobGuard.h \
    include/sce/hl/spurs/HLSpursCommandListDispatcher.h \
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
    include/cell/spurs/task_exit_code.h \
    include/cell/spurs/policy_module.h \
    include/cell/spurs/lv2_event_queue.h \
    include/cell/spurs/lfqueue.h \
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
    include/cell/codec/libatrac3plus.h \
    include/cell/codec/libatrac3multi.h \
    include/cell/codec/celpenc.h \
    include/cell/codec/celp8enc.h \
    include/cell/codec/pngcom.h \
    include/cell/codec/pngenc.h \
    include/cell/codec/jpgenc.h \
    include/cell/codec/gifdec.h \
    include/cell/sail.h \
    include/cell/sail/common.h \
    include/cell/sail/player.h \
    include/cell/sail/source.h \
    include/cell/sail/source_check.h \
    include/cell/sail/descriptor.h \
    include/cell/sail/mp4.h \
    include/cell/sail/avi.h \
    include/cell/sail/converter.h \
    include/cell/sail/profile.h \
    include/cell/sail/au_receiver.h \
    include/cell/sail/sound_adapter.h \
    include/cell/sail/graphics_adapter.h \
    include/cell/sail/renderer_audio.h \
    include/cell/sail/renderer_video.h \
    include/cell/sail/feeder_audio.h \
    include/cell/sail/feeder_video.h \
    include/cell/sail/common_rec.h \
    include/cell/sail/muxer.h \
    include/cell/sail/recorder.h \
    include/cell/sail_rec.h \
    include/cell/sysutil_oskdialog_ext.h \
    include/cell/sysutil_search.h \
    include/sysutil/sysutil_oskdialog.h \
    include/sysutil/sysutil_search.h \
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
    include/cell/padfilter.h \
    include/rsx/commands.h \
    include/rsx/commands_inc.h \
    include/rsx/gcm_sys.h \
    include/rsx/mm.h \
    include/rsx/nv40.h \
    include/rsx/resc.h \
    include/rsx/rsx.h \
    include/rsx/rsx_function_macros.h \
    include/rsx/rsx_program.h

SUBLIBS := libgcm_cmd librsx libdbgfont libspurs_task libsputhread libspurs_job libspurs_jq libdma libfiber libsync2 libsync liblv2 libface libmt19937

# Headers shared across PPU + SPU sides - the SPU job runtime needs to
# see the same CellSpursJobHeader / CellSpursJobContext2 / error-code
# definitions the PPU control surface uses.  These get installed into
# both $(PS3DK)/ppu/include/ (via HEADERS) and $(PS3DK)/spu/include/
# (via this list).
SHARED_SPURS_SPU_HEADERS := \
    include/cellstatus.h \
    include/mt19937.h \
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
    include/cell/spurs/task_types.h \
    include/cell/spurs/barrier.h \
    include/cell/spurs/event_flag.h \
    include/cell/spurs/queue.h \
    include/cell/spurs/job_guard.h \
    include/cell/spurs/semaphore.h \
    include/cell/spurs/task_exit_code.h \
    include/cell/spurs/policy_module.h \
    include/cell/spurs/lfqueue.h \
    include/cell/trace/trace_types.h \
    include/vectormath/cpp/vectormath_soa.h

# SPU-only headers - installed to $(INSTALL_INC_SPU) but NOT to the
# PPU side.  These are SPU-flavored variants (e.g. the slim SPU
# spurs.h umbrella that omits PPU syscall surfaces).
SPU_ONLY_HEADERS := \
    include-spu/cell/spurs.h

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
    include-spu/stdlib.h \
    include-spu/cell/atomic.h \
    include-spu/cell/dma.h \
    include-spu/cell/sync.h \
    include-spu/cell/sync/error.h \
    include-spu/cell/sync/barrier.h \
    include-spu/cell/sync/barrier_types.h \
    include-spu/cell/sync/queue.h \
    include-spu/cell/sync/mutex.h \
    include-spu/cell/sync/rwm.h \
    include-spu/cell/sync/lfqueue.h \
    include-spu/cell/sync2.h \
    include-spu/cell/fiber.h

.PHONY: all install clean version-header $(SUBLIBS) \
	install-stub-archives install-native-aliases install-manifest verify-install \
	install-spu-only-headers clean-stale-spu-spurs

all: $(SUBLIBS)

install: install-headers install-spu-headers install-shared-spurs-spu-headers install-spu-only-headers clean-stale-spu-spurs install-version install-sublibs install-stub-archives install-native-aliases install-manifest

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

# SPU-only headers - installed from include-spu/ to the SPU sysroot.
# These are SPU-flavored variants (e.g. the slim spurs.h umbrella)
# that must not leak into the PPU include tree.
install-spu-only-headers:
	@for h in $(SPU_ONLY_HEADERS); do \
	    rel="$${h#include-spu/}"; \
	    dst="$(INSTALL_INC_SPU)/$$rel"; \
	    install -d "$$(dirname $$dst)"; \
	    install -m 0644 "$$h" "$$dst"; \
	    echo "  spu-only $$rel"; \
	done

# Remove stale PPU-only spurs sub-headers that may linger in the SPU
# sysroot from a prior install that installed a broader set.  The SPU
# umbrella now deliberately excludes workload.h; if it exists from an
# old install, purge it so no stale declarations leak into SPU compiles.
# NOTE: job_chain.h is intentionally kept - installed by libspurs_job.
clean-stale-spu-spurs:
	@stale="workload.h"; \
	for f in $$stale; do \
	    path="$(INSTALL_INC_SPU)/cell/spurs/$$f"; \
	    if test -f "$$path"; then \
	        rm -f "$$path"; \
	        echo "  cleaned stale spu $$f"; \
	    fi; \
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

install-stub-archives:
	@echo "  stubs   build-cell-stub-archives"
	@PS3DEV="$(PS3DEV)" PS3DK="$(PS3DK)" "$(TOOLCHAIN_ROOT)/scripts/build-cell-stub-archives.sh"

install-native-aliases:
	@install -d "$(INSTALL_LIB)" "$(INSTALL_LIB_LP64)"
	@test -f "$(INSTALL_LIB)/libsysutil_stub.a" || { echo "missing $(INSTALL_LIB)/libsysutil_stub.a"; exit 1; }
	@test -f "$(INSTALL_LIB_LP64)/libsysutil_stub.a" || { echo "missing $(INSTALL_LIB_LP64)/libsysutil_stub.a"; exit 1; }
	@ln -sfn libsysutil_stub.a "$(INSTALL_LIB)/libsysutil.a"
	@ln -sfn libsysutil_stub.a "$(INSTALL_LIB_LP64)/libsysutil.a"
	@test -f "$(INSTALL_LIB)/liblv2.a" || { echo "missing $(INSTALL_LIB)/liblv2.a"; exit 1; }
	@test -f "$(INSTALL_LIB_LP64)/liblv2.a" || { echo "missing $(INSTALL_LIB_LP64)/liblv2.a"; exit 1; }
	@echo "  alias   libsysutil.a -> libsysutil_stub.a (ILP32 + LP64)"
	@test -f "$(INSTALL_LIB)/libdbgfont.a" || { echo "missing $(INSTALL_LIB)/libdbgfont.a"; exit 1; }
	@test -f "$(INSTALL_LIB_LP64)/libdbgfont.a" || { echo "missing $(INSTALL_LIB_LP64)/libdbgfont.a"; exit 1; }
	@ln -sfn libdbgfont.a "$(INSTALL_LIB)/libdbgfont_gcm.a"
	@ln -sfn libdbgfont.a "$(INSTALL_LIB_LP64)/libdbgfont_gcm.a"
	@echo "  alias   libdbgfont_gcm.a -> libdbgfont.a (ILP32 + LP64)"

install-manifest:
	@install -d "$(PS3DK)"
	@src_sha="$$(sha256sum include/sysutil/video.h | awk '{print $$1}')"; \
	  dst_sha="$$(sha256sum "$(INSTALL_INC)/sysutil/video.h" | awk '{print $$1}')"; \
	  test "$$src_sha" = "$$dst_sha" || { echo "installed sysutil/video.h sha mismatch"; exit 1; }; \
	  test "$$(readlink "$(INSTALL_LIB)/libsysutil.a")" = "libsysutil_stub.a" || { echo "bad ILP32 libsysutil.a alias"; exit 1; }; \
	  test "$$(readlink "$(INSTALL_LIB_LP64)/libsysutil.a")" = "libsysutil_stub.a" || { echo "bad LP64 libsysutil.a alias"; exit 1; }; \
	  test -f "$(INSTALL_LIB)/liblv2.a" || { echo "missing $(INSTALL_LIB)/liblv2.a"; exit 1; }; \
	  test -f "$(INSTALL_LIB_LP64)/liblv2.a" || { echo "missing $(INSTALL_LIB_LP64)/liblv2.a"; exit 1; }; \
	  test -f "$(INSTALL_LIB)/librsx.a" || { echo "missing $(INSTALL_LIB)/librsx.a"; exit 1; }; \
	  test -f "$(INSTALL_LIB_LP64)/librsx.a" || { echo "missing $(INSTALL_LIB_LP64)/librsx.a"; exit 1; }; \
	  { \
	    echo "schema=1"; \
	    echo "source_root=$(TOOLCHAIN_ROOT)"; \
	    printf "version="; "$(VERSION_SH)" --format=plain; \
	    echo "sysutil_video_sha256=$$src_sha"; \
	    echo "installed_sysutil_video_sha256=$$dst_sha"; \
	    echo "libsysutil_ilp32=symlink:libsysutil_stub.a"; \
	    echo "libsysutil_lp64=symlink:libsysutil_stub.a"; \
	    echo "liblv2_ilp32=present"; \
	    echo "liblv2_lp64=present"; \
	    echo "librsx_ilp32=present"; \
	    echo "librsx_lp64=present"; \
	  } > "$(INSTALL_MANIFEST).tmp"; \
	  mv "$(INSTALL_MANIFEST).tmp" "$(INSTALL_MANIFEST)"
	@echo "  marker  $(INSTALL_MANIFEST)"

verify-install:
	@test -f "$(INSTALL_MANIFEST)" || { echo "missing $(INSTALL_MANIFEST)"; exit 1; }
	@test -L "$(INSTALL_LIB)/libsysutil.a" || { echo "bad ILP32 libsysutil.a"; exit 1; }
	@test -L "$(INSTALL_LIB_LP64)/libsysutil.a" || { echo "bad LP64 libsysutil.a"; exit 1; }
	@test "$$(readlink "$(INSTALL_LIB)/libsysutil.a")" = "libsysutil_stub.a" || { echo "bad ILP32 libsysutil.a target"; exit 1; }
	@test "$$(readlink "$(INSTALL_LIB_LP64)/libsysutil.a")" = "libsysutil_stub.a" || { echo "bad LP64 libsysutil.a target"; exit 1; }
	@test -f "$(INSTALL_LIB)/liblv2.a" || { echo "missing $(INSTALL_LIB)/liblv2.a"; exit 1; }
	@test -f "$(INSTALL_LIB_LP64)/liblv2.a" || { echo "missing $(INSTALL_LIB_LP64)/liblv2.a"; exit 1; }
	@test -f "$(INSTALL_LIB)/librsx.a" || { echo "missing $(INSTALL_LIB)/librsx.a"; exit 1; }
	@test -f "$(INSTALL_LIB_LP64)/librsx.a" || { echo "missing $(INSTALL_LIB_LP64)/librsx.a"; exit 1; }
	@echo "  verify  SDK install manifest + critical artifacts OK"

$(SUBLIBS):
	$(MAKE) -C $@

# libdbgfont's sources transitively include <Cg/cg.h>, which is
# installed by libgcm_cmd.  Force the order so a parallel `make all`
# can't race.
libdbgfont: libgcm_cmd

clean:
	@for lib in $(SUBLIBS); do $(MAKE) -C $$lib clean || exit 1; done
