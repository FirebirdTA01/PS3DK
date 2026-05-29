/*! \file cell/sysutil_gameupdate.h
 \brief cellGameUpdate utility declarations.

  Game-update entry points are resolved by libnetctl_stub.a because the
  update utility exports live in the same runtime module as cellNetCtl.
  Link with -lnetctl_stub.
*/

#ifndef __PS3DK_CELL_SYSUTIL_GAMEUPDATE_H__
#define __PS3DK_CELL_SYSUTIL_GAMEUPDATE_H__

#include <stddef.h>
#include <cell/sysutil_gamecontent.h>
#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED                 0x8002cc01
#define CELL_GAMEUPDATE_ERROR_ALREADY_INITIALIZED             0x8002cc02
#define CELL_GAMEUPDATE_ERROR_INVALID_ADDR                    0x8002cc03
#define CELL_GAMEUPDATE_ERROR_INVALID_SIZE                    0x8002cc04
#define CELL_GAMEUPDATE_ERROR_INVALID_MEMORY_CONTAINER        0x8002cc05
#define CELL_GAMEUPDATE_ERROR_INSUFFICIENT_MEMORY_CONTAINER   0x8002cc06
#define CELL_GAMEUPDATE_ERROR_BUSY                            0x8002cc07
#define CELL_GAMEUPDATE_ERROR_NOT_START                       0x8002cc08
#define CELL_GAMEUPDATE_ERROR_LOAD_FAILED                     0x8002cc09

#define CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE               0
#define CELL_GAMEUPDATE_RESULT_STATUS_UPDATE_FOUND            1
#define CELL_GAMEUPDATE_RESULT_STATUS_MAINTENANCE             2
#define CELL_GAMEUPDATE_RESULT_STATUS_ERROR                   3
#define CELL_GAMEUPDATE_RESULT_STATUS_CANCELLED               4
#define CELL_GAMEUPDATE_RESULT_STATUS_FINISHED                5
#define CELL_GAMEUPDATE_RESULT_STATUS_ABORTED                 6
#define CELL_GAMEUPDATE_RESULT_STATUS_SYSTEM_UPDATE_NEEDED    7

typedef int CellGameUpdateResultStatus;

typedef void (*CellGameUpdateCallback)(
    CellGameUpdateResultStatus status,
    int error_code,
    void *userdata);

typedef struct CellGameUpdateResult {
    CellGameUpdateResultStatus status;
    int error_code;
    char app_ver[CELL_GAME_SYSP_APP_VER_SIZE];
    char padding[2];
} CellGameUpdateResult;

typedef void (*CellGameUpdateCallbackEx)(
    CellGameUpdateResult *result,
    void *userdata);

typedef struct CellGameUpdateParam {
    uint32_t size;
    sys_memory_container_t cid;
} CellGameUpdateParam;

int cellGameUpdateInit(void);
int cellGameUpdateCheckStartAsync(const CellGameUpdateParam *param,
                                  CellGameUpdateCallback cb_func,
                                  void *userdata);
int cellGameUpdateCheckFinishAsync(CellGameUpdateCallback cb_func,
                                   void *userdata);
int cellGameUpdateCheckStartWithoutDialogAsync(CellGameUpdateCallback cb_func,
                                               void *userdata);
int cellGameUpdateCheckAbort(void);
int cellGameUpdateTerm(void);
int cellGameUpdateCheckStartAsyncEx(const CellGameUpdateParam *param,
                                    CellGameUpdateCallbackEx cb_func,
                                    void *userdata);
int cellGameUpdateCheckFinishAsyncEx(CellGameUpdateCallbackEx cb_func,
                                     void *userdata);
int cellGameUpdateCheckStartWithoutDialogAsyncEx(CellGameUpdateCallbackEx cb_func,
                                                 void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SYSUTIL_GAMEUPDATE_H__ */
