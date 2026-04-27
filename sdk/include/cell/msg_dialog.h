/*! \file cell/msg_dialog.h
 \brief Sony-named forwarder for the cellMsgDialog API.

  Thin static-inline forwarders over PSL1GHT's <sysutil/msg.h>.  PSL1GHT
  already embeds Sony's FNIDs for these functions (the underlying SPRX
  stub uses `msgDialogOpenEx` etc. with the FNID of Sony's
  `cellMsgDialogOpen`); this header closes the source-compat gap so
  users writing against Sony's reference SDK can compile and link
  without changes.

  Covered:
    - cellMsgDialogOpen / Open2 / OpenErrorCode — forwarders.
    - cellMsgDialogAbort / Close.
    - cellMsgDialogProgressBarSetMsg / Reset / Inc.
    - CellMsgDialogCallback typedef + CELL_MSGDIALOG_BUTTON_* enum.
    - CELL_MSGDIALOG_TYPE_* bitfield constants matching PSL1GHT's
      msgType layout (verified bit-for-bit against Sony's
      sysutil_msgdialog.h).

  Late-SDK additions:
    - cellMsgDialogOpenSimulViewWarning — declared as a plain extern
      and resolved by libsysutil_stub.a (no PSL1GHT counterpart).
*/

#ifndef __PSL1GHT_CELL_MSG_DIALOG_H__
#define __PSL1GHT_CELL_MSG_DIALOG_H__

#include <sysutil/msg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Callback type — Sony passes the button as int, PSL1GHT passes an
 * msgButton enum; the enum underlies int so they're ABI-compatible. */
typedef void (*CellMsgDialogCallback)(int button_type, void *userdata);

/* Progressbar index constants.  Same values as PSL1GHT's
 * MSG_PROGRESSBAR_INDEX{0,1}; aliasing to Sony names. */
#define CELL_MSGDIALOG_PROGRESSBAR_INDEX_SINGLE        0
#define CELL_MSGDIALOG_PROGRESSBAR_INDEX_DOUBLE_UPPER  0
#define CELL_MSGDIALOG_PROGRESSBAR_INDEX_DOUBLE_LOWER  1

/* Button-return enum, Sony-named; values match PSL1GHT's msgButton. */
#define CELL_MSGDIALOG_BUTTON_NONE      MSG_DIALOG_BTN_NONE
#define CELL_MSGDIALOG_BUTTON_INVALID   MSG_DIALOG_BTN_INVALID
#define CELL_MSGDIALOG_BUTTON_OK        MSG_DIALOG_BTN_OK
#define CELL_MSGDIALOG_BUTTON_YES       MSG_DIALOG_BTN_YES
#define CELL_MSGDIALOG_BUTTON_NO        MSG_DIALOG_BTN_NO
#define CELL_MSGDIALOG_BUTTON_ESCAPE    MSG_DIALOG_BTN_ESCAPE

/* Dialog-type bitfield.  PSL1GHT's msgType values are the exact bit
 * positions Sony documents — verified bit-for-bit against
 * reference/sony-sdk/target/ppu/include/sysutil/sysutil_msgdialog.h. */
#define CELL_MSGDIALOG_TYPE_SE_TYPE_ERROR           MSG_DIALOG_ERROR
#define CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL          MSG_DIALOG_NORMAL
#define CELL_MSGDIALOG_TYPE_SE_MUTE_OFF             0
#define CELL_MSGDIALOG_TYPE_SE_MUTE_ON              MSG_DIALOG_MUTE_ON
#define CELL_MSGDIALOG_TYPE_BG_VISIBLE              0
#define CELL_MSGDIALOG_TYPE_BG_INVISIBLE            MSG_DIALOG_BKG_INVISIBLE
#define CELL_MSGDIALOG_TYPE_BUTTON_TYPE_NONE        0
#define CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO       MSG_DIALOG_BTN_TYPE_YESNO
#define CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK          MSG_DIALOG_BTN_TYPE_OK
#define CELL_MSGDIALOG_TYPE_DISABLE_CANCEL_OFF      0
#define CELL_MSGDIALOG_TYPE_DISABLE_CANCEL_ON       MSG_DIALOG_DISABLE_CANCEL_ON
#define CELL_MSGDIALOG_TYPE_DEFAULT_CURSOR_NONE     0
#define CELL_MSGDIALOG_TYPE_DEFAULT_CURSOR_YES      0
#define CELL_MSGDIALOG_TYPE_DEFAULT_CURSOR_NO       MSG_DIALOG_DEFAULT_CURSOR_NO
#define CELL_MSGDIALOG_TYPE_DEFAULT_CURSOR_OK       0
#define CELL_MSGDIALOG_TYPE_PROGRESSBAR_NONE        0
#define CELL_MSGDIALOG_TYPE_PROGRESSBAR_SINGLE      MSG_DIALOG_SINGLE_PROGRESSBAR
#define CELL_MSGDIALOG_TYPE_PROGRESSBAR_DOUBLE      MSG_DIALOG_DOUBLE_PROGRESSBAR

/* Function forwarders.  PSL1GHT's msgDialogOpen et al. are
 * thin wrappers over msgDialogOpenEx (etc.), which are the SPRX stubs
 * that hold Sony's FNIDs (0xf81eca25, 0x7603d3db, 0x3e22cb4b, ...).
 * Calling the Sony-named function here produces a final ELF whose
 * .rodata.sceFNID imports those exact FNIDs. */

static inline int cellMsgDialogOpen(unsigned int type, const char *msg,
                                    CellMsgDialogCallback func,
                                    void *userdata, void *reserved) {
	return (int)msgDialogOpen((msgType)type, msg,
	                          (msgDialogCallback)func, userdata, reserved);
}

static inline int cellMsgDialogOpen2(unsigned int type, const char *msg,
                                     CellMsgDialogCallback func,
                                     void *userdata, void *reserved) {
	return (int)msgDialogOpen2((msgType)type, msg,
	                           (msgDialogCallback)func, userdata, reserved);
}

static inline int cellMsgDialogOpenErrorCode(unsigned int error_num,
                                             CellMsgDialogCallback func,
                                             void *userdata, void *reserved) {
	return (int)msgDialogOpenErrorCode(error_num,
	                                   (msgDialogCallback)func, userdata, reserved);
}

static inline int cellMsgDialogAbort(void) {
	return (int)msgDialogAbort();
}

static inline int cellMsgDialogClose(float delay_time_ms) {
	return (int)msgDialogClose((f32)delay_time_ms);
}

static inline int cellMsgDialogProgressBarSetMsg(unsigned int progressbar_index,
                                                 const char *msg) {
	return (int)msgDialogProgressBarSetMsg(progressbar_index, msg);
}

static inline int cellMsgDialogProgressBarReset(unsigned int progressbar_index) {
	return (int)msgDialogProgressBarReset(progressbar_index);
}

static inline int cellMsgDialogProgressBarInc(unsigned int progressbar_index,
                                              unsigned int percent) {
	return (int)msgDialogProgressBarInc(progressbar_index, percent);
}

/* Stereoscopic-3D "please remove glasses" splash.  Resolves directly
 * to libsysutil_stub.a; no PSL1GHT shim. */
extern int cellMsgDialogOpenSimulViewWarning(CellMsgDialogCallback callback,
                                             void *userdata,
                                             void *extparam);

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_MSG_DIALOG_H__ */
