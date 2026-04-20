/*! \file cell/sysutil.h
 \brief Sony-named forwarder over PSL1GHT's <sysutil/sysutil.h>.

  Re-exports the PSL1GHT system-utility callback API under the Sony
  reference SDK naming (`cellSysutilRegisterCallback`, etc.) so source
  written against the Sony SDK compiles against PSL1GHT's runtime with
  no other changes.  The underlying SPRX linkage is identical — PSL1GHT
  already uses Sony's FNIDs; only the C-level identifiers differ.

  Scope:
    - Callback registry: register / check / unregister.
    - The common event codes (CELL_SYSUTIL_REQUEST_EXITGAME etc.).
    - The CellSysutilCallback typedef.

  Out of scope here (queued as libsysutil backfill follow-ups):
    - cellMsgDialog*, cellSaveData*, cellGameData*, etc. — those live
      in their own Sony sub-headers and each wants a matching forwarder
      (cell/msg_dialog.h, cell/save_data.h, ...).  Pattern is the same
      as here.
    - cellSysutilGetSystemParam{Int,String} — wait on a clean-room
      match of Sony's param ID enum before forwarding.
*/

#ifndef __PSL1GHT_CELL_SYSUTIL_H__
#define __PSL1GHT_CELL_SYSUTIL_H__

#include <sysutil/sysutil.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sony's callback type; layout-identical to PSL1GHT's sysutilCallback. */
typedef sysutilCallback CellSysutilCallback;

/* Sony userid alias — most APIs that take a CellSysutilUserId
 * just pass it through to the underlying SPRX as an opaque uint32. */
typedef unsigned int CellSysutilUserId;

/* Common event codes — Sony's names over PSL1GHT's symbols.  Values
 * are identical across the rename; code paraphrased from Sony SDK
 * 475.001 sysutil_common.h.  RPCS3 fires (at minimum)
 * DRAWING_BEGIN / SYSTEM_MENU_OPEN / BGMPLAYBACK_PLAY when the XMB
 * opens and the matching _END / _CLOSE / _STOP on close. */
#define CELL_SYSUTIL_REQUEST_EXITGAME         SYSUTIL_EXIT_GAME
#define CELL_SYSUTIL_DRAWING_BEGIN            SYSUTIL_DRAW_BEGIN
#define CELL_SYSUTIL_DRAWING_END              SYSUTIL_DRAW_END
#define CELL_SYSUTIL_SYSTEM_MENU_OPEN         SYSUTIL_MENU_OPEN
#define CELL_SYSUTIL_SYSTEM_MENU_CLOSE        SYSUTIL_MENU_CLOSE
#define CELL_SYSUTIL_BGMPLAYBACK_PLAY         0x0141
#define CELL_SYSUTIL_BGMPLAYBACK_STOP         0x0142
#define CELL_SYSUTIL_NP_INVITATION_SELECTED   0x0151
#define CELL_SYSUTIL_NP_DATA_MESSAGE_SELECTED 0x0152
#define CELL_SYSUTIL_SYSCHAT_START                       0x0161
#define CELL_SYSUTIL_SYSCHAT_STOP                        0x0162
#define CELL_SYSUTIL_SYSCHAT_VOICE_STREAMING_RESUMED     0x0163
#define CELL_SYSUTIL_SYSCHAT_VOICE_STREAMING_PAUSED      0x0164

/* Callback registry — thin static inlines so Sony-named call sites
 * compile down to direct branches against PSL1GHT's existing runtime. */
static inline int cellSysutilCheckCallback(void) {
	return (int)sysUtilCheckCallback();
}

static inline int cellSysutilRegisterCallback(int slot, CellSysutilCallback func, void *userdata) {
	return (int)sysUtilRegisterCallback((s32)slot, func, userdata);
}

static inline int cellSysutilUnregisterCallback(int slot) {
	return (int)sysUtilUnregisterCallback((s32)slot);
}

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYSUTIL_H__ */
