/*! \file cell/sysutil_music_decode.h
 \brief Sony-SDK-source-compat libsysutil_music_decode (XMB BGM decode-to-memory) API.

  Backed by libsysutil_music_decode_stub.a — the loader resolves the 20
  exports (v1 + v2 variants) against the cellMusicDecodeUtility SPRX
  module at runtime.  No PSL1GHT runtime wrapper.

  Like sysutil_music.h, opaque forward decls cover types that need
  SPURS / search / playback-common headers we don't yet ship.
*/

#ifndef __PSL1GHT_CELL_SYSUTIL_MUSIC_DECODE_H__
#define __PSL1GHT_CELL_SYSUTIL_MUSIC_DECODE_H__

#include <stdint.h>
#include <stddef.h>
#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sys_memory_container_t bridge. */
#ifndef _SYS_MEMORY_CONTAINER_T_DEFINED
#define _SYS_MEMORY_CONTAINER_T_DEFINED
typedef sys_mem_container_t sys_memory_container_t;
#endif

/* Forward decls — full layouts live in unshipped Sony headers. */
typedef struct CellMusicDecodeSelectionContext  CellMusicDecodeSelectionContext;
typedef struct CellMusicDecodeSelectionContext2 CellMusicDecodeSelectionContext2;
typedef struct CellSearchContentId              CellSearchContentId;
typedef struct CellSpurs                        CellSpurs;
typedef struct CellSpursSystemWorkloadAttribute CellSpursSystemWorkloadAttribute;

typedef void (*CellMusicDecodeCallback)(uint32_t event, void *param, void *userData);

/* v1 surface. */
int cellMusicDecodeInitialize(int mode, sys_memory_container_t container,
                              int spuPriority, CellMusicDecodeCallback func,
                              void *userData);
int cellMusicDecodeInitializeSystemWorkload(int mode, sys_memory_container_t container,
                                            CellMusicDecodeCallback func, void *userData,
                                            CellSpurs *spurs, const uint8_t priority[8],
                                            const CellSpursSystemWorkloadAttribute *attr);
int cellMusicDecodeFinalize(void);
int cellMusicDecodeSelectContents(void);
int cellMusicDecodeSetDecodeCommand(int command, void *param);
int cellMusicDecodeSetSelectionContext(CellMusicDecodeSelectionContext *context);
int cellMusicDecodeGetDecodeStatus(int *status);
int cellMusicDecodeGetSelectionContext(CellMusicDecodeSelectionContext *context);
int cellMusicDecodeGetContentsId(CellSearchContentId *contents_id);
int cellMusicDecodeRead(void *buf, uint32_t *startTime, uint32_t reqSize,
                        uint32_t *readSize, uint32_t *position);

/* v2 surface — same shape, distinct FNIDs. */
int cellMusicDecodeInitialize2(int mode, sys_memory_container_t container,
                               int spuPriority, CellMusicDecodeCallback func,
                               void *userData);
int cellMusicDecodeInitialize2SystemWorkload(int mode, sys_memory_container_t container,
                                             CellMusicDecodeCallback func, void *userData,
                                             CellSpurs *spurs, const uint8_t priority[8],
                                             const CellSpursSystemWorkloadAttribute *attr);
int cellMusicDecodeFinalize2(void);
int cellMusicDecodeSelectContents2(void);
int cellMusicDecodeSetDecodeCommand2(int command, void *param);
int cellMusicDecodeSetSelectionContext2(CellMusicDecodeSelectionContext2 *context);
int cellMusicDecodeGetDecodeStatus2(int *status);
int cellMusicDecodeGetSelectionContext2(CellMusicDecodeSelectionContext2 *context);
int cellMusicDecodeGetContentsId2(CellSearchContentId *contents_id);
int cellMusicDecodeRead2(void *buf, uint32_t *startTime, uint32_t reqSize,
                         uint32_t *readSize, uint32_t *position);

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYSUTIL_MUSIC_DECODE_H__ */
