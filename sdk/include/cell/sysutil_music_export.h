/*! \file cell/sysutil_music_export.h
 \brief Sony-SDK-source-compat libsysutil_music_export (export track to XMB) API.

  Backed by libsysutil_music_export_stub.a — five exports against the
  cellMusicExportUtility SPRX module.  No PSL1GHT runtime wrapper.
*/

#ifndef __PSL1GHT_CELL_SYSUTIL_MUSIC_EXPORT_H__
#define __PSL1GHT_CELL_SYSUTIL_MUSIC_EXPORT_H__

#include <stdint.h>
#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes. */
#define CELL_MUSIC_EXPORT_UTIL_RET_OK              0
#define CELL_MUSIC_EXPORT_UTIL_RET_CANCEL          1
#define CELL_MUSIC_EXPORT_UTIL_ERROR_BUSY          0x8002c601
#define CELL_MUSIC_EXPORT_UTIL_ERROR_INTERNAL      0x8002c602
#define CELL_MUSIC_EXPORT_UTIL_ERROR_PARAM         0x8002c603
#define CELL_MUSIC_EXPORT_UTIL_ERROR_ACCESS_ERROR  0x8002c604
#define CELL_MUSIC_EXPORT_UTIL_ERROR_DB_INTERNAL   0x8002c605
#define CELL_MUSIC_EXPORT_UTIL_ERROR_DB_REGIST     0x8002c606
#define CELL_MUSIC_EXPORT_UTIL_ERROR_SET_META      0x8002c607
#define CELL_MUSIC_EXPORT_UTIL_ERROR_FLUSH_META    0x8002c608
#define CELL_MUSIC_EXPORT_UTIL_ERROR_MOVE          0x8002c609
#define CELL_MUSIC_EXPORT_UTIL_ERROR_INITIALIZE    0x8002c60a

typedef enum {
	CELL_MUSIC_EXPORT_UTIL_VERSION_CURRENT = 0,
} CellMusicUtilVersion;

enum {
	CELL_MUSIC_EXPORT_UTIL_HDD_PATH_MAX           = 1055,
	CELL_MUSIC_EXPORT_UTIL_MUSIC_TITLE_MAX_LENGTH = 64,
	CELL_MUSIC_EXPORT_UTIL_GAME_TITLE_MAX_LENGTH  = 64,
	CELL_MUSIC_EXPORT_UTIL_GAME_COMMENT_MAX_SIZE  = 1024,
};

/* sys_memory_container_t bridge. */
#ifndef _SYS_MEMORY_CONTAINER_T_DEFINED
#define _SYS_MEMORY_CONTAINER_T_DEFINED
typedef sys_mem_container_t sys_memory_container_t;
#endif

typedef struct CellMusicExportSetParam {
	char *title;
	char *game_title;
	char *artist;
	char *genre;
	char *game_comment;
	char *reserved1;
	void *reserved2;
} CellMusicExportSetParam;

typedef void (*CellMusicExportUtilFinishCallback)(int result, void *userdata);

/* Resolved by libsysutil_music_export_stub.a. */
int cellMusicExportInitialize(unsigned int version, sys_memory_container_t container,
                              CellMusicExportUtilFinishCallback funcFinish, void *userdata);
int cellMusicExportInitialize2(unsigned int version,
                               CellMusicExportUtilFinishCallback funcFinish, void *userdata);
int cellMusicExportFromFile(const char *srcHddDir, const char *srcHddFile,
                            CellMusicExportSetParam *param,
                            CellMusicExportUtilFinishCallback funcFinish, void *userdata);
int cellMusicExportFinalize(CellMusicExportUtilFinishCallback funcFinish, void *userdata);
int cellMusicExportProgress(CellMusicExportUtilFinishCallback funcFinish, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* __PSL1GHT_CELL_SYSUTIL_MUSIC_EXPORT_H__ */
