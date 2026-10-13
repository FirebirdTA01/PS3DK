/*! \file cell/sysutil_photo_export.h
 \brief Export a photo file to the system's Photo column
        (libsysutil_photo_export_stub).

  Initialise, export a file from a game directory (FromFile moves it,
  FromFileWithCopy leaves the original) with an optional photo title, game
  title and comment, then finalise.  Every step finishes asynchronously
  through the callback with CELL_PHOTO_EXPORT_UTIL_RET_* or an error;
  cellPhotoExportProgress reports progress the same way.  Pointer members
  stay 32 bits wide in LP64 programs (ATTRIBUTE_PRXPTR).
*/

#ifndef PS3TC_CELL_SYSUTIL_PHOTO_EXPORT_H
#define PS3TC_CELL_SYSUTIL_PHOTO_EXPORT_H

#include <ppu-types.h>       /* ATTRIBUTE_PRXPTR */
#include <sys/memory.h>      /* sys_memory_container_t */
#include <sysutil/sysutil_common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_PHOTO_EXPORT_UTIL_RET_OK              0
#define CELL_PHOTO_EXPORT_UTIL_RET_CANCEL          1
#define CELL_PHOTO_EXPORT_UTIL_ERROR_BUSY          0x8002c201
#define CELL_PHOTO_EXPORT_UTIL_ERROR_INTERNAL      0x8002c202
#define CELL_PHOTO_EXPORT_UTIL_ERROR_PARAM         0x8002c203
#define CELL_PHOTO_EXPORT_UTIL_ERROR_ACCESS_ERROR  0x8002c204
#define CELL_PHOTO_EXPORT_UTIL_ERROR_DB_INTERNAL   0x8002c205
#define CELL_PHOTO_EXPORT_UTIL_ERROR_DB_REGIST     0x8002c206
#define CELL_PHOTO_EXPORT_UTIL_ERROR_SET_META      0x8002c207
#define CELL_PHOTO_EXPORT_UTIL_ERROR_FLUSH_META    0x8002c208
#define CELL_PHOTO_EXPORT_UTIL_ERROR_MOVE          0x8002c209
#define CELL_PHOTO_EXPORT_UTIL_ERROR_INITIALIZE    0x8002c20a

typedef enum {
	CELL_PHOTO_EXPORT_UTIL_VERSION_CURRENT = 0
} CellPhotoExportUtilVersion;

typedef enum {
	CELL_PHOTO_EXPORT_UTIL_HDD_PATH_MAX           = 1055,
	CELL_PHOTO_EXPORT_UTIL_PHOTO_TITLE_MAX_LENGTH = 64,
	CELL_PHOTO_EXPORT_UTIL_GAME_TITLE_MAX_LENGTH  = 64,
	CELL_PHOTO_EXPORT_UTIL_GAME_COMMENT_MAX_SIZE  = 1024
} CellPhotoExportUtilParamSize;

typedef struct {
	char *photo_title ATTRIBUTE_PRXPTR;
	char *game_title ATTRIBUTE_PRXPTR;
	char *game_comment ATTRIBUTE_PRXPTR;
	void *reserved ATTRIBUTE_PRXPTR;
} CellPhotoExportSetParam;

typedef void (*CellPhotoExportUtilFinishCallback)(int result, void *userdata);

int cellPhotoExportInitialize(unsigned int version, sys_memory_container_t container,
                              CellPhotoExportUtilFinishCallback funcFinish, void *userdata);
int cellPhotoExportInitialize2(unsigned int version,
                               CellPhotoExportUtilFinishCallback funcFinish, void *userdata);
int cellPhotoExportFromFile(const char *srcHddDir, const char *srcHddFile,
                            CellPhotoExportSetParam *param,
                            CellPhotoExportUtilFinishCallback funcFinish, void *userdata);
int cellPhotoExportFromFileWithCopy(const char *srcHddDir, const char *srcHddFile,
                                    CellPhotoExportSetParam *param,
                                    CellPhotoExportUtilFinishCallback funcFinish, void *userdata);
int cellPhotoExportFinalize(CellPhotoExportUtilFinishCallback funcFinish, void *userdata);
int cellPhotoExportProgress(CellPhotoExportUtilFinishCallback funcFinish, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_SYSUTIL_PHOTO_EXPORT_H */
