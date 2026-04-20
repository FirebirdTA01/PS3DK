/*! \file cell/sysutil_music.h
 \brief Sony-SDK-source-compat libsysutil_music (XMB BGM playback) API.

  Backed by libsysutil_music_stub.a — the loader resolves the 22 exports
  (v1 + v2 variants) against the cellMusicUtility SPRX module at runtime.
  No PSL1GHT runtime wrapper.

  Some functions reference SPURS (cell/spurs/types.h, cell/spurs/system_workload.h),
  search (cell/search/...), and music-playback context types we don't yet ship.
  Those types are forward-declared here as opaque structs so that user code
  which doesn't touch them still compiles; user code that does call e.g.
  `cellMusicInitializeSystemWorkload` will need to include the proper SPURS
  headers separately.
*/

#ifndef __PSL1GHT_CELL_SYSUTIL_MUSIC_H__
#define __PSL1GHT_CELL_SYSUTIL_MUSIC_H__

#include <stdint.h>
#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memory container size required to bring the cellMusicUtility SPRX up. */
#define CELL_MUSIC_PLAYBACK_MEMORY_CONTAINER_SIZE  (11 * 1024 * 1024)

/* sysutil_common event codes (delivered via cellSysutilRegisterCallback). */
#define CELL_SYSUTIL_MUSIC_INITIALIZING_FINISHED  1
#define CELL_SYSUTIL_MUSIC_SHUTDOWN_FINISHED      4
#define CELL_SYSUTIL_MUSIC_LOADING_FINISHED       5
#define CELL_SYSUTIL_MUSIC_UNLOADING_FINISHED     7
#define CELL_SYSUTIL_MUSIC_RELEASED               9
#define CELL_SYSUTIL_MUSIC_GRABBED               11

/* Music-callback event codes. */
#define CELL_MUSIC_EVENT_STATUS_NOTIFICATION             0
#define CELL_MUSIC_EVENT_INITIALIZE_RESULT               1
#define CELL_MUSIC_EVENT_FINALIZE_RESULT                 2
#define CELL_MUSIC_EVENT_SELECT_CONTENTS_RESULT          3
#define CELL_MUSIC_EVENT_SET_PLAYBACK_COMMAND_RESULT     4
#define CELL_MUSIC_EVENT_SET_VOLUME_RESULT               5
#define CELL_MUSIC_EVENT_SET_SELECTION_CONTEXT_RESULT    6
#define CELL_MUSIC_EVENT_UI_NOTIFICATION                 7

/* Return codes. */
#define CELL_MUSIC_OK                          0
#define CELL_MUSIC_CANCELED                    1
#define CELL_MUSIC_PLAYBACK_FINISHED           0x8002c101
#define CELL_MUSIC_ERROR_PARAM                 0x8002c102
#define CELL_MUSIC_ERROR_BUSY                  0x8002c103
#define CELL_MUSIC_ERROR_NO_ACTIVE_CONTENT     0x8002c104
#define CELL_MUSIC_ERROR_NO_MATCH_FOUND        0x8002c105
#define CELL_MUSIC_ERROR_INVALID_CONTEXT       0x8002c106
#define CELL_MUSIC_ERROR_PLAYBACK_FAILURE      0x8002c107
#define CELL_MUSIC_ERROR_NO_MORE_CONTENT       0x8002c108
#define CELL_MUSIC_DIALOG_OPEN                 0x8002c109
#define CELL_MUSIC_DIALOG_CLOSE                0x8002c10A
#define CELL_MUSIC_ERROR_GENERIC               0x8002c1FF

/* Operating mode. */
#define CELL_MUSIC_PLAYER_MODE_NORMAL          0

/* Playback commands. */
#define CELL_MUSIC_PB_CMD_STOP                 0
#define CELL_MUSIC_PB_CMD_PLAY                 1
#define CELL_MUSIC_PB_CMD_PAUSE                2
#define CELL_MUSIC_PB_CMD_NEXT                 3
#define CELL_MUSIC_PB_CMD_PREV                 4
#define CELL_MUSIC_PB_CMD_FASTFORWARD          5
#define CELL_MUSIC_PB_CMD_FASTREVERSE          6

/* Playback status. */
#define CELL_MUSIC_PB_STATUS_STOP              0
#define CELL_MUSIC_PB_STATUS_PLAY              1
#define CELL_MUSIC_PB_STATUS_PAUSE             2
#define CELL_MUSIC_PB_STATUS_FASTFORWARD       3
#define CELL_MUSIC_PB_STATUS_FASTREVERSE       4

/* sys_memory_container_t bridge. */
#ifndef _SYS_MEMORY_CONTAINER_T_DEFINED
#define _SYS_MEMORY_CONTAINER_T_DEFINED
typedef sys_mem_container_t sys_memory_container_t;
#endif

/* Forward decls for types defined by other headers (search, SPURS,
 * music_playback_common).  Users that need their full definitions must
 * include the appropriate Sony header. */
typedef struct CellMusicSelectionContext  CellMusicSelectionContext;
typedef struct CellMusicSelectionContext2 CellMusicSelectionContext2;
typedef struct CellSearchContentId        CellSearchContentId;
typedef struct CellSpurs                  CellSpurs;
typedef struct CellSpursSystemWorkloadAttribute CellSpursSystemWorkloadAttribute;

typedef void (*CellMusicCallback)(uint32_t event, void *param, void *userData);

/* Resolved by libsysutil_music_stub.a — v1 surface. */
int cellMusicInitialize(int mode, sys_memory_container_t container, int spuPriority,
                        CellMusicCallback func, void *userData);
int cellMusicInitializeSystemWorkload(int mode, sys_memory_container_t container,
                                      CellMusicCallback func, void *userData,
                                      CellSpurs *spurs, const uint8_t priority[8],
                                      const CellSpursSystemWorkloadAttribute *attr);
int cellMusicFinalize(void);
int cellMusicSelectContents(sys_memory_container_t container);
int cellMusicSetVolume(float level);
int cellMusicSetPlaybackCommand(int command, void *param);
int cellMusicSetSelectionContext(CellMusicSelectionContext *context);
int cellMusicGetVolume(float *level);
int cellMusicGetPlaybackStatus(int *status);
int cellMusicGetSelectionContext(CellMusicSelectionContext *context);
int cellMusicGetContentsId(CellSearchContentId *contents_id);

/* v2 surface — same shape, distinct FNIDs.  See sysutil_music2.h in
 * Sony's reference SDK for the full v2 docs. */
int cellMusicInitialize2(int mode, sys_memory_container_t container, int spuPriority,
                         CellMusicCallback func, void *userData);
int cellMusicInitialize2SystemWorkload(int mode, sys_memory_container_t container,
                                       CellMusicCallback func, void *userData,
                                       CellSpurs *spurs, const uint8_t priority[8],
                                       const CellSpursSystemWorkloadAttribute *attr);
int cellMusicFinalize2(void);
int cellMusicSelectContents2(void);
int cellMusicSetVolume2(float level);
int cellMusicSetPlaybackCommand2(int command, void *param);
int cellMusicSetSelectionContext2(CellMusicSelectionContext2 *context);
int cellMusicGetVolume2(float *level);
int cellMusicGetPlaybackStatus2(int *status);
int cellMusicGetSelectionContext2(CellMusicSelectionContext2 *context);
int cellMusicGetContentsId2(CellSearchContentId *contents_id);

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYSUTIL_MUSIC_H__ */
