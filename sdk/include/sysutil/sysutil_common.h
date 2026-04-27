/*! \file sysutil/sysutil_common.h
 \brief Reference-SDK source-compat header — sysutil error-code base
        macros + extern "C" guard.

  Sony's cellSysutil headers all `#include "sysutil_common.h"` for the
  CELL_SYSUTIL_ERROR_BASE_* prefix constants.  Pure forwarder shape:
  defines + extern "C" only, no types.
*/

#ifndef PS3TC_SYSUTIL_SYSUTIL_COMMON_H
#define PS3TC_SYSUTIL_SYSUTIL_COMMON_H

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_SYSUTIL_ERROR_BASE_INTERNAL          0x8002b000
#define CELL_SYSUTIL_ERROR_BASE_COMMON            0x8002b100
#define CELL_SYSUTIL_ERROR_BASE_SYSTEMPARAM       0x8002b200
#define CELL_SYSUTIL_ERROR_BASE_MSGDIALOG         0x8002b300
#define CELL_SYSUTIL_ERROR_BASE_OSKDIALOG         0x8002b500
#define CELL_SYSUTIL_ERROR_BASE_VIDEO_OUT         0x8002b220
#define CELL_SYSUTIL_ERROR_BASE_AUDIO_OUT         0x8002b240
#define CELL_SYSUTIL_ERROR_BASE_BGMPLAYBACK       0x8002b100
#define CELL_SYSUTIL_ERROR_BASE_AVC               0x8002b700
#define CELL_SYSUTIL_ERROR_BASE_GAMECONTENT       0x8002cb00
#define CELL_SYSUTIL_ERROR_BASE_HDDGAME           0x8002cb80
#define CELL_SYSUTIL_ERROR_BASE_DISCGAME          0x8002cbc0
#define CELL_SYSUTIL_ERROR_BASE_GAMEDATA          0x8002cb40
#define CELL_SYSUTIL_ERROR_BASE_SAVEDATA          0x8002b400
#define CELL_SYSUTIL_ERROR_BASE_SYSCACHE          0x8002bc00
#define CELL_SYSUTIL_ERROR_BASE_SYSCONF           0x8002bb00
#define CELL_SYSUTIL_ERROR_BASE_STORAGEDATA       0x8002be00
#define CELL_SYSUTIL_ERROR_BASE_BGMPLAYBACK_EX    0x8002d300
#define CELL_SYSUTIL_ERROR_BASE_AP                0x8002cd00

/* ---- Sysutil callback type + entry points ----
 * Reference SDK declares these in sysutil_common.h; sample code
 * reaches them transitively without #include <cell/sysutil.h>.
 * Reproduce the prototypes here. */
#define CELL_SYSUTIL_USERID_MAX  99999999

typedef unsigned int CellSysutilUserId;
typedef void (*CellSysutilCallback)(uint64_t status, uint64_t param,
                                    void *userdata);

int cellSysutilCheckCallback(void);
int cellSysutilRegisterCallback(int slot, CellSysutilCallback func,
                                void *userdata);
int cellSysutilUnregisterCallback(int slot);

int cellSysutilGetSystemParamInt(int id, int *value);
int cellSysutilGetSystemParamString(int id, char *buf, unsigned int bufsize);

/* ---- Sysutil callback request / event status codes ----
 * Mirror the values in <cell/sysutil.h>; reproduced here so source
 * that only #include <sysutil/sysutil_common.h> still resolves the
 * EXITGAME / DRAWING / MENU / BGMPLAYBACK constants. */
#ifndef CELL_SYSUTIL_REQUEST_EXITGAME
# define CELL_SYSUTIL_REQUEST_EXITGAME            0x0101
# define CELL_SYSUTIL_DRAWING_BEGIN               0x0121
# define CELL_SYSUTIL_DRAWING_END                 0x0122
# define CELL_SYSUTIL_SYSTEM_MENU_OPEN            0x0131
# define CELL_SYSUTIL_SYSTEM_MENU_CLOSE           0x0132
# define CELL_SYSUTIL_BGMPLAYBACK_PLAY            0x0141
# define CELL_SYSUTIL_BGMPLAYBACK_STOP            0x0142
# define CELL_SYSUTIL_NP_INVITATION_SELECTED      0x0151
# define CELL_SYSUTIL_NP_DATA_MESSAGE_SELECTED    0x0152
# define CELL_SYSUTIL_SYSCHAT_START               0x0161
# define CELL_SYSUTIL_SYSCHAT_STOP                0x0162
# define CELL_SYSUTIL_SYSCHAT_VOICE_STREAMING_RESUMED  0x0163
# define CELL_SYSUTIL_SYSCHAT_VOICE_STREAMING_PAUSED   0x0164
#endif

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_SYSUTIL_SYSUTIL_COMMON_H */
