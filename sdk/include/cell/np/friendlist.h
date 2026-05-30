#ifndef __PS3DK_CELL_NP_FRIENDLIST_H__
#define __PS3DK_CELL_NP_FRIENDLIST_H__

#include <cell/np/common.h>
#include <stdint.h>
#include <sys/memory.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t SceNpFriendlistCustomOptions;
typedef int (*SceNpFriendlistResultHandler)(int retCode, void *arg);

int sceNpFriendlist(SceNpFriendlistResultHandler resultHandler, void *userArg, sys_memory_container_t containerId);
int sceNpFriendlistCustom(SceNpFriendlistCustomOptions options, SceNpFriendlistResultHandler resultHandler, void *userArg, sys_memory_container_t containerId);
int sceNpFriendlistAbortGui(void);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_FRIENDLIST_H__ */
