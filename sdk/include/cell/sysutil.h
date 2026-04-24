/*! \file cell/sysutil.h
 \brief Cell-SDK-named forwarder over PSL1GHT's <sysutil/sysutil.h>.

  Re-exports the PSL1GHT system-utility callback API under the
  reference-SDK naming (`cellSysutilRegisterCallback`, etc.) so
  source written against the reference SDK compiles against PSL1GHT's
  runtime with no other changes.  The underlying SPRX linkage is
  identical — PSL1GHT already uses the reference FNIDs; only the
  C-level identifiers differ.

  Scope:
    - Callback registry: register / check / unregister.
    - The common event codes (CELL_SYSUTIL_REQUEST_EXITGAME etc.).
    - The CellSysutilCallback typedef.

  Out of scope here (queued as libsysutil backfill follow-ups):
    - cellMsgDialog*, cellSaveData*, cellGameData*, etc. — those live
      in their own cell-SDK sub-headers and each wants a matching
      forwarder (cell/msg_dialog.h, cell/save_data.h, ...).  Pattern
      is the same as here.
    - cellSysutilGetSystemParam{Int,String} — wait on a clean-room
      match of the reference param-ID enum before forwarding.
*/

#ifndef __PSL1GHT_CELL_SYSUTIL_H__
#define __PSL1GHT_CELL_SYSUTIL_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cell SDK callback type.  Declared directly (not forwarded off
 * PSL1GHT's sysutilCallback typedef) so this header doesn't drag in
 * <sysutil/sysutil.h>'s global-namespace `sysutilCallback` symbol,
 * which clashes with reference-SDK sample code that declares its own
 * function of that name. */
typedef void (*CellSysutilCallback)(uint64_t status, uint64_t param, void *userdata);

/* Cell SDK userid alias — most APIs that take a CellSysutilUserId
 * just pass it through to the underlying SPRX as an opaque uint32. */
typedef unsigned int CellSysutilUserId;

/* Common event codes — cell-SDK names over PSL1GHT's symbols.
 * Values are identical across the rename (same SPRX constants).
 * RPCS3 fires (at minimum) DRAWING_BEGIN / SYSTEM_MENU_OPEN /
 * BGMPLAYBACK_PLAY when the XMB opens and the matching _END /
 * _CLOSE / _STOP on close. */
#define CELL_SYSUTIL_REQUEST_EXITGAME         0x0101
#define CELL_SYSUTIL_DRAWING_BEGIN            0x0121
#define CELL_SYSUTIL_DRAWING_END              0x0122
#define CELL_SYSUTIL_SYSTEM_MENU_OPEN         0x0131
#define CELL_SYSUTIL_SYSTEM_MENU_CLOSE        0x0132
#define CELL_SYSUTIL_BGMPLAYBACK_PLAY         0x0141
#define CELL_SYSUTIL_BGMPLAYBACK_STOP         0x0142
#define CELL_SYSUTIL_NP_INVITATION_SELECTED   0x0151
#define CELL_SYSUTIL_NP_DATA_MESSAGE_SELECTED 0x0152
#define CELL_SYSUTIL_SYSCHAT_START                       0x0161
#define CELL_SYSUTIL_SYSCHAT_STOP                        0x0162
#define CELL_SYSUTIL_SYSCHAT_VOICE_STREAMING_RESUMED     0x0163
#define CELL_SYSUTIL_SYSCHAT_VOICE_STREAMING_PAUSED      0x0164

/* PSL1GHT-side entry points - declared locally with our types so we
 * don't pull <sysutil/sysutil.h> (and its sysutilCallback typedef)
 * into every TU that just wants the Cell-SDK callback surface.
 * Linker resolves by name; the function-pointer arg mangles the
 * same under C linkage regardless of which typedef the prototype
 * names. */
extern int sysUtilCheckCallback(void);
extern int sysUtilRegisterCallback(int slot, CellSysutilCallback func, void *userdata);
extern int sysUtilUnregisterCallback(int slot);

/* Callback registry — thin static inlines so Cell-SDK-named call sites
 * compile down to direct branches against PSL1GHT's existing runtime. */
static inline int cellSysutilCheckCallback(void) {
	return sysUtilCheckCallback();
}

static inline int cellSysutilRegisterCallback(int slot, CellSysutilCallback func, void *userdata) {
	return sysUtilRegisterCallback(slot, func, userdata);
}

static inline int cellSysutilUnregisterCallback(int slot) {
	return sysUtilUnregisterCallback(slot);
}

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYSUTIL_H__ */
