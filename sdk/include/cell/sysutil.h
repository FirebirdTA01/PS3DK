/*! \file cell/sysutil.h
 \brief reference-SDK-named forwarder over PSL1GHT's <sysutil/sysutil.h>.

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
      in their own the reference SDK sub-headers and each wants a matching
      forwarder (cell/msg_dialog.h, cell/save_data.h, ...).  Pattern
      is the same as here.
    - cellSysutilGetSystemParam{Int,String} — wait on an independent
      match of the reference param-ID enum before forwarding.
*/

#ifndef __PSL1GHT_CELL_SYSUTIL_H__
#define __PSL1GHT_CELL_SYSUTIL_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* reference SDK callback type.  Declared directly (not forwarded off
 * PSL1GHT's sysutilCallback typedef) so this header doesn't drag in
 * <sysutil/sysutil.h>'s global-namespace `sysutilCallback` symbol,
 * which clashes with reference-SDK sample code that declares its own
 * function of that name. */
typedef void (*CellSysutilCallback)(uint64_t status, uint64_t param, void *userdata);

/* reference SDK userid alias — most APIs that take a CellSysutilUserId
 * just pass it through to the underlying SPRX as an opaque uint32. */
typedef unsigned int CellSysutilUserId;

/* Common event codes — the reference SDK names over PSL1GHT's symbols.
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

/* Callback registry — direct reference-NID imports.
 *
 * These bind to the native nidgen-emitted multilib stub archive
 * (libsysutil_stub.a, ILP32 + LP64) which provides the canonical
 * cellSysutil* SPRX trampolines via our compact-OPD frame-less stub
 * shape.  No PSL1GHT forwarder (sysUtilRegisterCallback /
 * __get_opd32 / PSL1GHT libsysutil.a) is involved.  Signatures are
 * the reference-SDK contract (sysutil_common.h §callback registry):
 *   int cellSysutilCheckCallback(void);
 *   int cellSysutilRegisterCallback(int, CellSysutilCallback, void*);
 *   int cellSysutilUnregisterCallback(int);
 * Link with -lsysutil_stub (not the PSL1GHT -lsysutil). */
extern int cellSysutilCheckCallback(void);
extern int cellSysutilRegisterCallback(int slot, CellSysutilCallback func, void *userdata);
extern int cellSysutilUnregisterCallback(int slot);

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYSUTIL_H__ */
