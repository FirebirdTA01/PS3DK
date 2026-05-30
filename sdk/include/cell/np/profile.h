#ifndef __PS3DK_CELL_NP_PROFILE_H__
#define __PS3DK_CELL_NP_PROFILE_H__

#include <cell/np/common.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*SceNpProfileResultHandler)(int result, void *arg);

int sceNpProfileCallGui(const SceNpId *npid, SceNpProfileResultHandler handler, void *userArg, uint64_t options);
int sceNpProfileAbortGui(void);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_PROFILE_H__ */
