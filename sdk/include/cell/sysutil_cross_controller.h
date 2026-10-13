/*! \file cell/sysutil_cross_controller.h
 \brief Cross-Controller: hand a PS Vita package to a connected system
        (libsysutil_cross_controller_stub).

  cellCrossControllerInitialize starts the transfer of a package (with its
  signature file and icon) described by CellCrossControllerParam and
  CellCrossControllerPackageInfo; the callback reports
  CELL_CROSS_CONTROLLER_STATUS_INITIALIZED / _FINALIZED with an error code.
  The system module reads the parameter structures as 32-bit pointers, so
  the pointer members keep that width in LP64 programs (ATTRIBUTE_PRXPTR).
*/

#ifndef PS3TC_CELL_SYSUTIL_CROSS_CONTROLLER_H
#define PS3TC_CELL_SYSUTIL_CROSS_CONTROLLER_H

#include <stdint.h>
#include <ppu-types.h>   /* ATTRIBUTE_PRXPTR */

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_CROSS_CONTROLLER_ERROR_CANCEL                ((int)0x8002cd80)
#define CELL_CROSS_CONTROLLER_ERROR_NETWORK               ((int)0x8002cd81)
#define CELL_CROSS_CONTROLLER_ERROR_OUT_OF_MEMORY         ((int)0x8002cd90)
#define CELL_CROSS_CONTROLLER_ERROR_FATAL                 ((int)0x8002cd91)
#define CELL_CROSS_CONTROLLER_ERROR_INVALID_PKG_FILENAME  ((int)0x8002cd92)
#define CELL_CROSS_CONTROLLER_ERROR_INVALID_SIG_FILENAME  ((int)0x8002cd93)
#define CELL_CROSS_CONTROLLER_ERROR_INVALID_ICON_FILENAME ((int)0x8002cd94)
#define CELL_CROSS_CONTROLLER_ERROR_INVALID_VALUE         ((int)0x8002cd95)
#define CELL_CROSS_CONTROLLER_ERROR_PKG_FILE_OPEN         ((int)0x8002cd96)
#define CELL_CROSS_CONTROLLER_ERROR_SIG_FILE_OPEN         ((int)0x8002cd97)
#define CELL_CROSS_CONTROLLER_ERROR_ICON_FILE_OPEN        ((int)0x8002cd98)
#define CELL_CROSS_CONTROLLER_ERROR_INVALID_STATE         ((int)0x8002cd99)
#define CELL_CROSS_CONTROLLER_ERROR_INVALID_PKG_FILE      ((int)0x8002cd9a)
#define CELL_CROSS_CONTROLLER_ERROR_INTERNAL              ((int)0x8002cda0)

#define CELL_CROSS_CONTROLLER_MAX_FILE_NAME_LEN   255
#define CELL_CROSS_CONTROLLER_PKG_TITLE_LEN       52
#define CELL_CROSS_CONTROLLER_PKG_TITLE_ID_LEN    10
#define CELL_CROSS_CONTROLLER_PKG_APP_VER_LEN     6

#define CELL_CROSS_CONTROLLER_STATUS_INITIALIZED  1
#define CELL_CROSS_CONTROLLER_STATUS_FINALIZED    2

typedef struct {
	const char *pPackageFileName ATTRIBUTE_PRXPTR;
	const char *pSignatureFileName ATTRIBUTE_PRXPTR;
	const char *pIconFileName ATTRIBUTE_PRXPTR;
	void *option ATTRIBUTE_PRXPTR;
} CellCrossControllerParam;

typedef struct {
	const char *pTitle ATTRIBUTE_PRXPTR;
	const char *pTitleId ATTRIBUTE_PRXPTR;
	const char *pAppVer ATTRIBUTE_PRXPTR;
} CellCrossControllerPackageInfo;

typedef void (*CellCrossControllerCallback)(int32_t status, int32_t errorCode,
                                            void *option, void *userdata);

int32_t cellCrossControllerInitialize(const CellCrossControllerParam *pParam,
                                      const CellCrossControllerPackageInfo *pPkgInfo,
                                      CellCrossControllerCallback cb, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_SYSUTIL_CROSS_CONTROLLER_H */
