/*
 * PS3 Custom Toolchain - cell/sysutil_music_decode.h
 *
 * cellMusicDecodeUtility v1 + v2 surface: BGM decode-to-memory API
 * for XMB music playback with SPURS-based decode offload.
 *
 * Twenty exported entry points (10 v1 + 10 v2) backed by
 * libsysutil_music_decode_stub.a.  The loader resolves calls
 * against the cellMusicDecodeUtility module at runtime.
 *
 * ABI notes:
 *  - Opaque forward-decls for CellSpurs, CellSpursSystemWorkloadAttribute,
 *    CellMusicSelectionContext, and CellSearchContentId - full layouts
 *    live in external headers not yet shipped.
 *  - Read / Read2 use uint64_t for reqSize / readSize to match the
 *    module wire contract (ILP32-safe; LP64 audit deferred).
 */

#ifndef __PSL1GHT_CELL_SYSUTIL_MUSIC_DECODE_H__
#define __PSL1GHT_CELL_SYSUTIL_MUSIC_DECODE_H__

#include <stdint.h>
#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sys_memory_container_t bridge. */
#ifndef _SYS_MEMORY_CONTAINER_T_DEFINED
#define _SYS_MEMORY_CONTAINER_T_DEFINED
typedef sys_mem_container_t sys_memory_container_t;
#endif

/* Forward declarations - concrete layouts live in external headers we
 * do not yet ship (cell/spurs.h, sysutil/sysutil_music_playback_common.h,
 * sysutil/sysutil_search_types.h).  Opaque pointers cross the SPRX
 * boundary; PRXPTR audit deferred until structs are materialized. */
typedef struct CellMusicSelectionContext       CellMusicSelectionContext;
typedef struct CellSearchContentId             CellSearchContentId;
typedef struct CellSpurs                       CellSpurs;
typedef struct CellSpursSystemWorkloadAttribute CellSpursSystemWorkloadAttribute;

/* v1 callback. */
typedef void (*CellMusicDecodeCallback)(uint32_t event, void *param,
                                        void *userData);

/* v2 callback - same calling convention, type-namespaced for source
 * compatibility with the reference-SDK music_decode2.h API path. */
typedef void (*CellMusicDecode2Callback)(uint32_t event, void *param,
                                         void *userData);

/* ---------------------------------------------------------------------
 * v1 constants
 * --------------------------------------------------------------------- */

#define CELL_MUSIC_DECODE_MEMORY_CONTAINER_SIZE    (12 * 1024 * 1024)

#define CELL_SYSUTIL_MUSIC_DECODE_INITIALIZING_FINISHED  1
#define CELL_SYSUTIL_MUSIC_DECODE_SHUTDOWN_FINISHED      4
#define CELL_SYSUTIL_MUSIC_DECODE_LOADING_FINISHED       5
#define CELL_SYSUTIL_MUSIC_DECODE_UNLOADING_FINISHED     7
#define CELL_SYSUTIL_MUSIC_DECODE_RELEASED               9
#define CELL_SYSUTIL_MUSIC_DECODE_GRABBED               11

#define CELL_MUSIC_DECODE_EVENT_STATUS_NOTIFICATION           0
#define CELL_MUSIC_DECODE_EVENT_INITIALIZE_RESULT             1
#define CELL_MUSIC_DECODE_EVENT_FINALIZE_RESULT               2
#define CELL_MUSIC_DECODE_EVENT_SELECT_CONTENTS_RESULT        3
#define CELL_MUSIC_DECODE_EVENT_SET_DECODE_COMMAND_RESULT     4
#define CELL_MUSIC_DECODE_EVENT_SET_SELECTION_CONTEXT_RESULT  5
#define CELL_MUSIC_DECODE_EVENT_UI_NOTIFICATION               6
#define CELL_MUSIC_DECODE_EVENT_NEXT_CONTENTS_READY_RESULT    7

#define CELL_MUSIC_DECODE_OK                       0
#define CELL_MUSIC_DECODE_CANCELED                 1
#define CELL_MUSIC_DECODE_DECODE_FINISHED         0x8002c101
#define CELL_MUSIC_DECODE_ERROR_PARAM             0x8002c102
#define CELL_MUSIC_DECODE_ERROR_BUSY              0x8002c103
#define CELL_MUSIC_DECODE_ERROR_NO_ACTIVE_CONTENT 0x8002c104
#define CELL_MUSIC_DECODE_ERROR_NO_MATCH_FOUND    0x8002c105
#define CELL_MUSIC_DECODE_ERROR_INVALID_CONTEXT   0x8002c106
#define CELL_MUSIC_DECODE_ERROR_DECODE_FAILURE    0x8002c107
#define CELL_MUSIC_DECODE_ERROR_NO_MORE_CONTENT   0x8002c108
#define CELL_MUSIC_DECODE_DIALOG_OPEN             0x8002c109
#define CELL_MUSIC_DECODE_DIALOG_CLOSE            0x8002c10A
#define CELL_MUSIC_DECODE_ERROR_NO_LPCM_DATA      0x8002c10B
#define CELL_MUSIC_DECODE_NEXT_CONTENTS_READY     0x8002c10C
#define CELL_MUSIC_DECODE_ERROR_GENERIC           0x8002c1FF

#define CELL_MUSIC_DECODE_MODE_NORMAL    0

#define CELL_MUSIC_DECODE_CMD_STOP       0
#define CELL_MUSIC_DECODE_CMD_START      1
#define CELL_MUSIC_DECODE_CMD_NEXT       2
#define CELL_MUSIC_DECODE_CMD_PREV       3

#define CELL_MUSIC_DECODE_STATUS_DORMANT  0
#define CELL_MUSIC_DECODE_STATUS_DECODING 1

#define CELL_MUSIC_DECODE_POSITION_NONE          0
#define CELL_MUSIC_DECODE_POSITION_START         1
#define CELL_MUSIC_DECODE_POSITION_MID           2
#define CELL_MUSIC_DECODE_POSITION_END           3
#define CELL_MUSIC_DECODE_POSITION_END_LIST_END  4

/* ---------------------------------------------------------------------
 * v2 constants
 * --------------------------------------------------------------------- */

#define CELL_MUSIC_DECODE2_MIN_BUFFER_SIZE   (448 * 1024)
#define CELL_MUSIC_DECODE2_MANAGEMENT_SIZE    (64 * 1024)
#define CELL_MUSIC_DECODE2_PAGESIZE_64K       (64 * 1024)
#define CELL_MUSIC_DECODE2_PAGESIZE_1M       (1 * 1024 * 1024)

#define CELL_SYSUTIL_MUSIC_DECODE2_INITIALIZING_FINISHED  1
#define CELL_SYSUTIL_MUSIC_DECODE2_SHUTDOWN_FINISHED      4
#define CELL_SYSUTIL_MUSIC_DECODE2_LOADING_FINISHED       5
#define CELL_SYSUTIL_MUSIC_DECODE2_UNLOADING_FINISHED     7
#define CELL_SYSUTIL_MUSIC_DECODE2_RELEASED               9
#define CELL_SYSUTIL_MUSIC_DECODE2_GRABBED               11

#define CELL_MUSIC_DECODE2_EVENT_STATUS_NOTIFICATION           0
#define CELL_MUSIC_DECODE2_EVENT_INITIALIZE_RESULT             1
#define CELL_MUSIC_DECODE2_EVENT_FINALIZE_RESULT               2
#define CELL_MUSIC_DECODE2_EVENT_SELECT_CONTENTS_RESULT        3
#define CELL_MUSIC_DECODE2_EVENT_SET_DECODE_COMMAND_RESULT     4
#define CELL_MUSIC_DECODE2_EVENT_SET_SELECTION_CONTEXT_RESULT  5
#define CELL_MUSIC_DECODE2_EVENT_UI_NOTIFICATION               6
#define CELL_MUSIC_DECODE2_EVENT_NEXT_CONTENTS_READY_RESULT    7

#define CELL_MUSIC_DECODE2_OK                       0
#define CELL_MUSIC_DECODE2_CANCELED                 1
#define CELL_MUSIC_DECODE2_DECODE_FINISHED         0x8002c101
#define CELL_MUSIC_DECODE2_ERROR_PARAM             0x8002c102
#define CELL_MUSIC_DECODE2_ERROR_BUSY              0x8002c103
#define CELL_MUSIC_DECODE2_ERROR_NO_ACTIVE_CONTENT 0x8002c104
#define CELL_MUSIC_DECODE2_ERROR_NO_MATCH_FOUND    0x8002c105
#define CELL_MUSIC_DECODE2_ERROR_INVALID_CONTEXT   0x8002c106
#define CELL_MUSIC_DECODE2_ERROR_DECODE_FAILURE    0x8002c107
#define CELL_MUSIC_DECODE2_ERROR_NO_MORE_CONTENT   0x8002c108
#define CELL_MUSIC_DECODE2_DIALOG_OPEN             0x8002c109
#define CELL_MUSIC_DECODE2_DIALOG_CLOSE            0x8002c10A
#define CELL_MUSIC_DECODE2_ERROR_NO_LPCM_DATA      0x8002c10B
#define CELL_MUSIC_DECODE2_NEXT_CONTENTS_READY     0x8002c10C
#define CELL_MUSIC_DECODE2_ERROR_GENERIC           0x8002c1FF

#define CELL_MUSIC_DECODE2_MODE_NORMAL    0

#define CELL_MUSIC_DECODE2_SPEED_MAX      0
#define CELL_MUSIC_DECODE2_SPEED_2        2

#define CELL_MUSIC_DECODE2_CMD_STOP       0
#define CELL_MUSIC_DECODE2_CMD_START      1
#define CELL_MUSIC_DECODE2_CMD_NEXT       2
#define CELL_MUSIC_DECODE2_CMD_PREV       3

#define CELL_MUSIC_DECODE2_STATUS_DORMANT  0
#define CELL_MUSIC_DECODE2_STATUS_DECODING 1

#define CELL_MUSIC_DECODE2_POSITION_NONE          0
#define CELL_MUSIC_DECODE2_POSITION_START         1
#define CELL_MUSIC_DECODE2_POSITION_MID           2
#define CELL_MUSIC_DECODE2_POSITION_END           3
#define CELL_MUSIC_DECODE2_POSITION_END_LIST_END  4

/* ---------------------------------------------------------------------
 * v1 surface (10 entry points)
 * --------------------------------------------------------------------- */

int cellMusicDecodeInitialize(int mode, sys_memory_container_t container,
                              int spuPriority, CellMusicDecodeCallback func,
                              void *userData);

int cellMusicDecodeInitializeSystemWorkload(int mode,
    sys_memory_container_t container,
    CellMusicDecodeCallback func, void *userData,
    int spuUsageRate,
    CellSpurs *spurs, const uint8_t priority[8],
    const CellSpursSystemWorkloadAttribute *attr);

int cellMusicDecodeFinalize(void);
int cellMusicDecodeSelectContents(void);

int cellMusicDecodeSetDecodeCommand(int command);

int cellMusicDecodeSetSelectionContext(CellMusicSelectionContext *context);
int cellMusicDecodeGetDecodeStatus(int *status);
int cellMusicDecodeGetSelectionContext(CellMusicSelectionContext *context);
int cellMusicDecodeGetContentsId(CellSearchContentId *contents_id);

int cellMusicDecodeRead(void *buf, uint32_t *startTime,
                        uint64_t reqSize, uint64_t *readSize,
                        int *position);

/* ---------------------------------------------------------------------
 * v2 surface (10 entry points)
 * --------------------------------------------------------------------- */

int cellMusicDecodeInitialize2(int mode, sys_memory_container_t container,
                               int spuPriority, CellMusicDecode2Callback func,
                               void *userData, int speed, int bufSize);

int cellMusicDecodeInitialize2SystemWorkload(int mode,
    sys_memory_container_t container,
    CellMusicDecode2Callback func, void *userData,
    int spuUsageRate, int bufSize,
    CellSpurs *spurs, const uint8_t priority[8],
    const CellSpursSystemWorkloadAttribute *attr);

int cellMusicDecodeFinalize2(void);
int cellMusicDecodeSelectContents2(void);

int cellMusicDecodeSetDecodeCommand2(int command);

int cellMusicDecodeSetSelectionContext2(CellMusicSelectionContext *context);
int cellMusicDecodeGetDecodeStatus2(int *status);
int cellMusicDecodeGetSelectionContext2(CellMusicSelectionContext *context);
int cellMusicDecodeGetContentsId2(CellSearchContentId *contents_id);

int cellMusicDecodeRead2(void *buf, uint32_t *startTime,
                         uint64_t reqSize, uint64_t *readSize,
                         int *position);

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYSUTIL_MUSIC_DECODE_H__ */
