/*! \file cell/sysutil_video_export.h
 \brief Export a video file to the system's Video column
        (libsysutil_video_export_stub).

  The same flow as photo export: initialise, export a file from a game
  directory (FromFile moves it, FromFileWithCopy copies it) with a title,
  game title, comment and whether the user may edit it, then finalise; each
  step finishes through the callback with CELL_VIDEO_EXPORT_UTIL_RET_* or an
  error.  Pointer members stay 32 bits wide in LP64 programs
  (ATTRIBUTE_PRXPTR).
*/

#ifndef PS3TC_CELL_SYSUTIL_VIDEO_EXPORT_H
#define PS3TC_CELL_SYSUTIL_VIDEO_EXPORT_H

#include <ppu-types.h>       /* ATTRIBUTE_PRXPTR */
#include <sys/memory.h>      /* sys_memory_container_t */
#include <sysutil/sysutil_common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_VIDEO_EXPORT_UTIL_RET_OK              0
#define CELL_VIDEO_EXPORT_UTIL_RET_CANCEL          1
#define CELL_VIDEO_EXPORT_UTIL_ERROR_BUSY          0x8002ca01
#define CELL_VIDEO_EXPORT_UTIL_ERROR_INTERNAL      0x8002ca02
#define CELL_VIDEO_EXPORT_UTIL_ERROR_PARAM         0x8002ca03
#define CELL_VIDEO_EXPORT_UTIL_ERROR_ACCESS_ERROR  0x8002ca04
#define CELL_VIDEO_EXPORT_UTIL_ERROR_DB_INTERNAL   0x8002ca05
#define CELL_VIDEO_EXPORT_UTIL_ERROR_DB_REGIST     0x8002ca06
#define CELL_VIDEO_EXPORT_UTIL_ERROR_SET_META      0x8002ca07
#define CELL_VIDEO_EXPORT_UTIL_ERROR_FLUSH_META    0x8002ca08
#define CELL_VIDEO_EXPORT_UTIL_ERROR_MOVE          0x8002ca09
#define CELL_VIDEO_EXPORT_UTIL_ERROR_INITIALIZE    0x8002ca0a

typedef enum {
	CELL_VIDEO_EXPORT_UTIL_VERSION_CURRENT = 0
} CellVideoUtilVersion;

typedef enum {
	CELL_VIDEO_EXPORT_UTIL_HDD_PATH_MAX           = 1055,
	CELL_VIDEO_EXPORT_UTIL_VIDEO_TITLE_MAX_LENGTH = 64,
	CELL_VIDEO_EXPORT_UTIL_GAME_TITLE_MAX_LENGTH  = 64,
	CELL_VIDEO_EXPORT_UTIL_GAME_COMMENT_MAX_SIZE  = 1024
} CellVideoUtilParamSize;

typedef struct {
	char *title ATTRIBUTE_PRXPTR;
	char *game_title ATTRIBUTE_PRXPTR;
	char *game_comment ATTRIBUTE_PRXPTR;
	int editable;
	void *reserved2 ATTRIBUTE_PRXPTR;
} CellVideoExportSetParam;

typedef void (*CellVideoExportUtilFinishCallback)(int result, void *userdata);

int cellVideoExportInitialize(unsigned int version, sys_memory_container_t container,
                              CellVideoExportUtilFinishCallback funcFinish, void *userdata);
int cellVideoExportInitialize2(unsigned int version,
                               CellVideoExportUtilFinishCallback funcFinish, void *userdata);
int cellVideoExportFromFile(const char *srcHddDir, const char *srcHddFile,
                            CellVideoExportSetParam *param,
                            CellVideoExportUtilFinishCallback funcFinish, void *userdata);
int cellVideoExportFromFileWithCopy(const char *srcHddDir, const char *srcHddFile,
                                    CellVideoExportSetParam *param,
                                    CellVideoExportUtilFinishCallback funcFinish, void *userdata);
int cellVideoExportFinalize(CellVideoExportUtilFinishCallback funcFinish, void *userdata);
int cellVideoExportProgress(CellVideoExportUtilFinishCallback funcFinish, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_SYSUTIL_VIDEO_EXPORT_H */
