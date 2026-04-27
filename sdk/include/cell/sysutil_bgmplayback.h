/*! \file cell/sysutil_bgmplayback.h
 \brief Background music playback control (cellSysutilBgmPlayback*).

  Lets a title suppress / restore the system BGM that the user has
  selected from the XMB while game audio is playing.  Required for
  TRC compliance on titles that produce in-game music.
*/

#ifndef PS3TC_CELL_SYSUTIL_BGMPLAYBACK_H
#define PS3TC_CELL_SYSUTIL_BGMPLAYBACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- error codes -------------------------------------------------- */
#define CELL_SYSUTIL_BGMPLAYBACK_OK                          0
#define CELL_SYSUTIL_BGMPLAYBACK_ERROR_PARAM                 0x8002b101
#define CELL_SYSUTIL_BGMPLAYBACK_ERROR_BUSY                  0x8002b102
#define CELL_SYSUTIL_BGMPLAYBACK_ERROR_GENERIC               0x8002b1ff

#define CELL_SYSUTIL_BGMPLAYBACK_EX_OK                       0
#define CELL_SYSUTIL_BGMPLAYBACK_EX_ERROR_PARAM              0x8002d301
#define CELL_SYSUTIL_BGMPLAYBACK_EX_ERROR_ALREADY_SETPARAM   0x8002d302
#define CELL_SYSUTIL_BGMPLAYBACK_EX_ERROR_DISABLE_SETPARAM   0x8002d303
#define CELL_SYSUTIL_BGMPLAYBACK_EX_ERROR_GENERIC            0x8002d3ff

/* ---- player / enable state --------------------------------------- */
#define CELL_SYSUTIL_BGMPLAYBACK_STATUS_PLAY      0
#define CELL_SYSUTIL_BGMPLAYBACK_STATUS_STOP      1
#define CELL_SYSUTIL_BGMPLAYBACK_STATUS_ENABLE    0
#define CELL_SYSUTIL_BGMPLAYBACK_STATUS_DISABLE   1

#define CELL_SYSUTIL_BGMPLAYBACK_FADE_INVALID     (-1)
#define CELL_SYSUTIL_BGMPLAYBACK_STATUS           32
#define CELL_SYSUTIL_BGMPLAYBACK_STATUS2          8

/* CellSearchContentId stand-in (avoids dragging in the full search
 * surface for a single 16-byte buffer). Layout matches the search
 * subsystem's content-id type byte-for-byte. */
typedef struct {
    char data[16];
} CellBgmPlaybackSearchContentId;

typedef struct {
    uint8_t                         playerState;
    uint8_t                         enableState;
    CellBgmPlaybackSearchContentId  contentId;
    uint8_t                         currentFadeRatio;
    char                            reserved[CELL_SYSUTIL_BGMPLAYBACK_STATUS - 3 - sizeof(CellBgmPlaybackSearchContentId)];
} CellSysutilBgmPlaybackStatus;

typedef struct {
    int32_t  systemBgmFadeInTime;
    int32_t  systemBgmFadeOutTime;
    int32_t  gameBgmFadeInTime;
    int32_t  gameBgmFadeOutTime;
    char     reserved[8];
} CellSysutilBgmPlaybackExtraParam;

typedef struct {
    uint8_t  playerState;
    char     reserved[CELL_SYSUTIL_BGMPLAYBACK_STATUS2 - 1];
} CellSysutilBgmPlaybackStatus2;

/* ---- entry points ------------------------------------------------- */
extern int cellSysutilDisableBgmPlayback(void);
extern int cellSysutilEnableBgmPlayback(void);
extern int cellSysutilGetBgmPlaybackStatus(CellSysutilBgmPlaybackStatus *status);

extern int cellSysutilSetBgmPlaybackExtraParam(CellSysutilBgmPlaybackExtraParam *param);
extern int cellSysutilDisableBgmPlaybackEx(CellSysutilBgmPlaybackExtraParam *param);
extern int cellSysutilEnableBgmPlaybackEx(CellSysutilBgmPlaybackExtraParam *param);

extern int cellSysutilGetBgmPlaybackStatus2(CellSysutilBgmPlaybackStatus2 *status);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_SYSUTIL_BGMPLAYBACK_H */
