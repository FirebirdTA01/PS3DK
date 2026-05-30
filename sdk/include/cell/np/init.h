#ifndef __PS3DK_CELL_NP_INIT_H__
#define __PS3DK_CELL_NP_INIT_H__

#include <cell/np/common.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_NP_POOL_SIZE_MIN (128U * 1024U)
#define SCE_NP_MIN_POOL_SIZE CELL_NP_POOL_SIZE_MIN

#define cell_np_initialize_default() \
    ({ static uint8_t cell_np_default_pool[CELL_NP_POOL_SIZE_MIN]; \
       sceNpInit(sizeof(cell_np_default_pool), cell_np_default_pool); })

int sceNpInit(size_t poolsize, void *poolptr);
int sceNpTerm(void);

int sceNpUtilCmpNpId(const SceNpId *npid1, const SceNpId *npid2);
int sceNpUtilCmpNpIdInOrder(const SceNpId *npid1, const SceNpId *npid2, int *order);
int sceNpUtilCmpOnlineId(const SceNpId *npid1, const SceNpId *npid2);
int sceNpUtilGetPlatformType(SceNpPlatformType *platformType);
int sceNpUtilSetPlatformType(SceNpPlatformType platformType);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_INIT_H__ */
