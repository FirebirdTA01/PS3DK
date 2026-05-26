/*
 * PS3 Custom Toolchain - cell/sysutil_userinfo.h
 *
 * cellUserInfo* surface: enumerate registered console users, get
 * per-user metadata, and present the system user-select dialog
 * (synchronous accessors + async dialog with finish callback).
 *
 * Five exported entry points backed by libsysutil_userinfo_stub.a:
 *   cellUserInfoGetStat               (synchronous)
 *   cellUserInfoGetList               (synchronous)
 *   cellUserInfoSelectUser_SetList    (asynchronous, caller-fixed list)
 *   cellUserInfoSelectUser_ListType   (asynchronous, list-type select)
 *   cellUserInfoEnableOverlay         (overlay visibility toggle)
 */

#ifndef PS3TC_CELL_SYSUTIL_USERINFO_H
#define PS3TC_CELL_SYSUTIL_USERINFO_H

#include <sysutil/sysutil_common.h>
#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Synchronous return codes. */
#define CELL_USERINFO_RET_OK            0
#define CELL_USERINFO_RET_CANCEL        1

/* Error codes (BASE_USERINFO = 0x8002c300). */
#define CELL_USERINFO_ERROR_BUSY        0x8002c301
#define CELL_USERINFO_ERROR_INTERNAL    0x8002c302
#define CELL_USERINFO_ERROR_PARAM       0x8002c303
#define CELL_USERINFO_ERROR_NOUSER      0x8002c304

/* Surface sizes. */
typedef enum {
	CELL_USERINFO_USER_MAX       = 16,
	CELL_USERINFO_TITLE_SIZE     = 256,
	CELL_USERINFO_USERNAME_SIZE  = 64,
} CellUserInfoParamSize;

/* Dialog list-population mode. */
typedef enum {
	CELL_USERINFO_LISTTYPE_ALL       = 0,
	CELL_USERINFO_LISTTYPE_NOCURRENT = 1,
} CellUserInfoListType;

/* Sentinel: focus the head of the rendered user list. */
typedef enum {
	CELL_USERINFO_FOCUS_LISTHEAD = 0xffffffff,
} CellUserInfoFocus;

typedef struct {
	CellSysutilUserId id;
	char name[CELL_USERINFO_USERNAME_SIZE];
} CellUserInfoUserStat;

typedef struct {
	CellSysutilUserId userId[CELL_USERINFO_USER_MAX];
} CellUserInfoUserList;

typedef struct {
	char *title;
	CellSysutilUserId focus;
	unsigned int fixedListNum;
	CellUserInfoUserList *fixedList;
	void *reserved;
} CellUserInfoListSet;

typedef struct {
	char *title;
	CellSysutilUserId focus;
	CellUserInfoListType type;
	void *reserved;
} CellUserInfoTypeSet;

typedef void (*CellUserInfoFinishCallback)(int result,
                                           CellUserInfoUserStat *selectedUser,
                                           void *userdata);

int cellUserInfoGetStat(CellSysutilUserId id, CellUserInfoUserStat *stat);

int cellUserInfoGetList(unsigned int *listNum,
                        CellUserInfoUserList *listBuf,
                        CellSysutilUserId *currentUserId);

int cellUserInfoSelectUser_SetList(CellUserInfoListSet *setList,
                                   CellUserInfoFinishCallback funcSelect,
                                   sys_memory_container_t container,
                                   void *userdata);

int cellUserInfoSelectUser_ListType(CellUserInfoTypeSet *listType,
                                    CellUserInfoFinishCallback funcSelect,
                                    sys_memory_container_t container,
                                    void *userdata);

void cellUserInfoEnableOverlay(int enable);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_SYSUTIL_USERINFO_H */
