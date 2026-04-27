/*! \file cell/sysutil_storagedata.h
 \brief XMB-mediated file copy between /dev_hdd0 and removable
        storage (cellStorageData*).  Used by photo / music / video
        import & export utilities.
*/

#ifndef PS3TC_CELL_SYSUTIL_STORAGEDATA_H
#define PS3TC_CELL_SYSUTIL_STORAGEDATA_H

#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PS3TC_SYS_MEMORY_CONTAINER_T_DEFINED
#define PS3TC_SYS_MEMORY_CONTAINER_T_DEFINED
typedef sys_mem_container_t sys_memory_container_t;
#endif

#define CELL_STORAGEDATA_RET_OK              0
#define CELL_STORAGEDATA_RET_CANCEL          1

#define CELL_STORAGEDATA_ERROR_BUSY          0x8002be01
#define CELL_STORAGEDATA_ERROR_INTERNAL      0x8002be02
#define CELL_STORAGEDATA_ERROR_PARAM         0x8002be03
#define CELL_STORAGEDATA_ERROR_ACCESS_ERROR  0x8002be04
#define CELL_STORAGEDATA_ERROR_FAILURE       0x8002be05

typedef enum {
    CELL_STORAGEDATA_VERSION_CURRENT       = 0,
    CELL_STORAGEDATA_VERSION_DST_FILENAME  = 1
} CellStorageDataVersion;

enum {
    CELL_STORAGEDATA_HDD_PATH_MAX    = 1055,
    CELL_STORAGEDATA_MEDIA_PATH_MAX  = 1024,
    CELL_STORAGEDATA_FILENAME_MAX    = 64,
    CELL_STORAGEDATA_FILESIZE_MAX    = 1024 * 1024 * 1024,
    CELL_STORAGEDATA_TITLE_MAX       = 256
};

#define CELL_STORAGEDATA_IMPORT_FILENAME  "IMPORT.BIN"
#define CELL_STORAGEDATA_EXPORT_FILENAME  "EXPORT.BIN"

typedef struct {
    unsigned int  fileSizeMax;
    char         *title;
    void         *reserved;
} CellStorageDataSetParam;

typedef void (*CellStorageDataFinishCallback)(int result, void *userdata);

extern int cellStorageDataImport(unsigned int version,
                                 char *srcMediaFile,
                                 char *dstHddDir,
                                 CellStorageDataSetParam *param,
                                 CellStorageDataFinishCallback funcFinish,
                                 sys_memory_container_t container,
                                 void *userdata);

extern int cellStorageDataExport(unsigned int version,
                                 char *srcHddFile,
                                 char *dstMediaDir,
                                 CellStorageDataSetParam *param,
                                 CellStorageDataFinishCallback funcFinish,
                                 sys_memory_container_t container,
                                 void *userdata);

extern int cellStorageDataImportMove(unsigned int version,
                                     char *srcMediaFile,
                                     char *dstHddDir,
                                     CellStorageDataSetParam *param,
                                     CellStorageDataFinishCallback funcFinish,
                                     sys_memory_container_t container,
                                     void *userdata);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_SYSUTIL_STORAGEDATA_H */
