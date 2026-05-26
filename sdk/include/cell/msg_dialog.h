/*! \file cell/msg_dialog.h
 \brief cellMsgDialog API — direct reference-NID imports.

  These bind to the native nidgen-emitted multilib stub archive
  (libsysutil_stub.a, ILP32 + LP64) which provides the canonical
  cellMsgDialog* SPRX trampolines via our compact-OPD frame-less
  stub shape.  No PSL1GHT forwarder (<sysutil/msg.h>, msgDialogOpen2,
  msgDialogOpenEx, PSL1GHT libsysutil.a) is involved.

  Signatures and constant values are the reference-SDK contract
  (sysutil_msgdialog.h): the type bitfield is a set of documented
  bit positions; the button-return / progressbar-index values are
  the documented enumerators expressed as macros so existing call
  sites (which use the CELL_MSGDIALOG_* names interchangeably) keep
  compiling.  Link with -lsysutil_stub (not the PSL1GHT -lsysutil).
*/

#ifndef __PSL1GHT_CELL_MSG_DIALOG_H__
#define __PSL1GHT_CELL_MSG_DIALOG_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Reference callback type: button code is delivered as int. */
typedef void (*CellMsgDialogCallback)(int buttonType, void *userData);

/* Progressbar index. */
#define CELL_MSGDIALOG_PROGRESSBAR_INDEX_SINGLE        0
#define CELL_MSGDIALOG_PROGRESSBAR_INDEX_DOUBLE_UPPER  0
#define CELL_MSGDIALOG_PROGRESSBAR_INDEX_DOUBLE_LOWER  1
#define CELL_MSGDIALOG_PROGRESSBAR_STRING_SIZE         (64)

/* Button-return codes. */
#define CELL_MSGDIALOG_BUTTON_NONE      (-1)
#define CELL_MSGDIALOG_BUTTON_INVALID   0
#define CELL_MSGDIALOG_BUTTON_OK        1
#define CELL_MSGDIALOG_BUTTON_YES       1
#define CELL_MSGDIALOG_BUTTON_NO        2
#define CELL_MSGDIALOG_BUTTON_ESCAPE    3

/* Dialog-type bitfield — documented reference bit positions. */
#define CELL_MSGDIALOG_TYPE_SE_TYPE_ERROR           (0 << 0)
#define CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL          (1 << 0)
#define CELL_MSGDIALOG_TYPE_SE_MUTE_OFF             (0 << 1)
#define CELL_MSGDIALOG_TYPE_SE_MUTE_ON              (1 << 1)
#define CELL_MSGDIALOG_TYPE_BG_VISIBLE              (0 << 2)
#define CELL_MSGDIALOG_TYPE_BG_INVISIBLE            (1 << 2)
#define CELL_MSGDIALOG_TYPE_BUTTON_TYPE_NONE        (0 << 4)
#define CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO       (1 << 4)
#define CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK          (2 << 4)
#define CELL_MSGDIALOG_TYPE_DISABLE_CANCEL_OFF      (0 << 7)
#define CELL_MSGDIALOG_TYPE_DISABLE_CANCEL_ON       (1 << 7)
#define CELL_MSGDIALOG_TYPE_DEFAULT_CURSOR_NONE     (0 << 8)
#define CELL_MSGDIALOG_TYPE_DEFAULT_CURSOR_YES      (0 << 8)
#define CELL_MSGDIALOG_TYPE_DEFAULT_CURSOR_NO       (1 << 8)
#define CELL_MSGDIALOG_TYPE_DEFAULT_CURSOR_OK       (0 << 8)
#define CELL_MSGDIALOG_TYPE_PROGRESSBAR_NONE        (0 << 12)
#define CELL_MSGDIALOG_TYPE_PROGRESSBAR_SINGLE      (1 << 12)
#define CELL_MSGDIALOG_TYPE_PROGRESSBAR_DOUBLE      (2 << 12)

/* Reference-NID function imports (resolved from libsysutil_stub.a). */
extern int cellMsgDialogOpen(unsigned int type, const char *msgString,
                             CellMsgDialogCallback func,
                             void *userData, void *extParam);
extern int cellMsgDialogOpen2(unsigned int type, const char *msgString,
                              CellMsgDialogCallback func,
                              void *userData, void *extParam);
extern int cellMsgDialogOpenErrorCode(unsigned int errorNum,
                                      CellMsgDialogCallback func,
                                      void *userData, void *extParam);
extern int cellMsgDialogOpenSimulViewWarning(CellMsgDialogCallback func,
                                             void *userData, void *extParam);
extern int cellMsgDialogAbort(void);
extern int cellMsgDialogClose(float delayTime);
extern int cellMsgDialogProgressBarSetMsg(unsigned int progressbarIndex,
                                          const char *msgString);
extern int cellMsgDialogProgressBarReset(unsigned int progressbarIndex);
extern int cellMsgDialogProgressBarInc(unsigned int progressbarIndex,
                                       unsigned int delta);

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_MSG_DIALOG_H__ */
