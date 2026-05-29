#ifndef __PS3DK_CELL_NP_UTIL_H__
#define __PS3DK_CELL_NP_UTIL_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SceNpUtilBandwidthTestResult {
    double upload_bps;
    double download_bps;
    int32_t result;
    char padding[4];
} SceNpUtilBandwidthTestResult;

#define SCE_NP_UTIL_BANDWIDTH_TEST_STATUS_NONE     0
#define SCE_NP_UTIL_BANDWIDTH_TEST_STATUS_RUNNING  1
#define SCE_NP_UTIL_BANDWIDTH_TEST_STATUS_FINISHED 2

int sceNpUtilBandwidthTestInitStart(int prio, size_t stack);
int sceNpUtilBandwidthTestGetStatus(void);
int sceNpUtilBandwidthTestShutdown(SceNpUtilBandwidthTestResult *result);
int sceNpUtilBandwidthTestAbort(void);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_UTIL_H__ */
