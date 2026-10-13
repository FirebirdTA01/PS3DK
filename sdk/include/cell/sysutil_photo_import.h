/*! \file cell/sysutil_photo_import.h
 \brief Let the user pick a photo and copy it into a game directory
        (libsysutil_photo_import_stub).

  cellPhotoImport / cellPhotoImport2 (the latter without a memory
  container) show the system's photo picker, copy the chosen file into
  dstHddPath and call back with CELL_PHOTO_IMPORT_RET_* and a
  CellPhotoImportFileData describing it (name, titles, comment, and its
  size, format and rotation in data_sub).  The system writes the file data,
  so its layout is fixed (1608 bytes); pointer members stay 32 bits wide in
  LP64 programs (ATTRIBUTE_PRXPTR).
*/

#ifndef PS3TC_CELL_SYSUTIL_PHOTO_IMPORT_H
#define PS3TC_CELL_SYSUTIL_PHOTO_IMPORT_H

#include <ppu-types.h>                 /* ATTRIBUTE_PRXPTR */
#include <sys/memory.h>                /* sys_memory_container_t */
#include <cell/fs/cell_fs_file_api.h>  /* CELL_FS_MAX_FS_FILE_NAME_LENGTH */
#include <sysutil/sysutil_common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_PHOTO_IMPORT_RET_OK              0
#define CELL_PHOTO_IMPORT_RET_CANCEL          1
#define CELL_PHOTO_IMPORT_ERROR_BUSY          0x8002c701
#define CELL_PHOTO_IMPORT_ERROR_INTERNAL      0x8002c702
#define CELL_PHOTO_IMPORT_ERROR_PARAM         0x8002c703
#define CELL_PHOTO_IMPORT_ERROR_ACCESS_ERROR  0x8002c704
#define CELL_PHOTO_IMPORT_ERROR_COPY          0x8002c705
#define CELL_PHOTO_IMPORT_ERROR_INITIALIZE    0x8002c706

typedef enum {
	CELL_PHOTO_IMPORT_VERSION_CURRENT = 0
} CellPhotoImportVersion;

typedef enum {
	CELL_PHOTO_IMPORT_HDD_PATH_MAX           = 1055,
	CELL_PHOTO_IMPORT_PHOTO_TITLE_MAX_LENGTH = 64,
	CELL_PHOTO_IMPORT_GAME_TITLE_MAX_SIZE    = 128,
	CELL_PHOTO_IMPORT_GAME_COMMENT_MAX_SIZE  = 1024
} CellPhotoImportParamSize;

typedef enum {
	CELL_PHOTO_IMPORT_FT_UNKNOWN = 0,
	CELL_PHOTO_IMPORT_FT_JPEG,
	CELL_PHOTO_IMPORT_FT_PNG,
	CELL_PHOTO_IMPORT_FT_GIF,
	CELL_PHOTO_IMPORT_FT_BMP,
	CELL_PHOTO_IMPORT_FT_TIFF,
	CELL_PHOTO_IMPORT_FT_MPO
} CellPhotoImportFormatType;

typedef enum {
	CELL_PHOTO_IMPORT_TEX_ROT_0 = 0,
	CELL_PHOTO_IMPORT_TEX_ROT_90,
	CELL_PHOTO_IMPORT_TEX_ROT_180,
	CELL_PHOTO_IMPORT_TEX_ROT_270
} CellPhotoImportTexRot;

typedef struct {
	unsigned int fileSizeMax;
	void *reserved1 ATTRIBUTE_PRXPTR;
	void *reserved2 ATTRIBUTE_PRXPTR;
} CellPhotoImportSetParam;

typedef struct {
	int width;
	int height;
	CellPhotoImportFormatType format;
	CellPhotoImportTexRot rotate;
} CellPhotoImportFileDataSub;

typedef struct {
	char dstFileName[CELL_FS_MAX_FS_FILE_NAME_LENGTH];
	char photo_title[CELL_PHOTO_IMPORT_PHOTO_TITLE_MAX_LENGTH * 3];   /* UTF-8, up to 3 bytes a character */
	char game_title[CELL_PHOTO_IMPORT_GAME_TITLE_MAX_SIZE];
	char game_comment[CELL_PHOTO_IMPORT_GAME_COMMENT_MAX_SIZE];
	char padding;                                                      /* aligns data_sub */
	CellPhotoImportFileDataSub *data_sub ATTRIBUTE_PRXPTR;
	void *reserved ATTRIBUTE_PRXPTR;
} CellPhotoImportFileData;

typedef void (*CellPhotoImportFinishCallback)(int result, CellPhotoImportFileData *filedata,
                                              void *userdata);

int cellPhotoImport(unsigned int version, char *dstHddPath, CellPhotoImportSetParam *param,
                    sys_memory_container_t container, CellPhotoImportFinishCallback funcFinish,
                    void *userdata);
int cellPhotoImport2(unsigned int version, char *dstHddPath, CellPhotoImportSetParam *param,
                     CellPhotoImportFinishCallback funcFinish, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_SYSUTIL_PHOTO_IMPORT_H */
