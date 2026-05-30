#ifndef __PS3DK_CELL_NP_SNS_H__
#define __PS3DK_CELL_NP_SNS_H__

#include <stdint.h>

#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_SNS_ERROR_UNKNOWN                         0x80024501
#define SCE_NP_SNS_ERROR_NOT_SIGN_IN                     0x80024502
#define SCE_NP_SNS_ERROR_INVALID_ARGUMENT                0x80024503
#define SCE_NP_SNS_ERROR_OUT_OF_MEMORY                   0x80024504
#define SCE_NP_SNS_ERROR_SHUTDOWN                        0x80024505
#define SCE_NP_SNS_ERROR_BUSY                            0x80024506
#define SCE_NP_SNS_FB_ERROR_ALREADY_INITIALIZED          0x80024511
#define SCE_NP_SNS_FB_ERROR_NOT_INITIALIZED              0x80024512
#define SCE_NP_SNS_FB_ERROR_EXCEEDS_MAX                  0x80024513
#define SCE_NP_SNS_FB_ERROR_UNKNOWN_HANDLE               0x80024514
#define SCE_NP_SNS_FB_ERROR_ABORTED                      0x80024515
#define SCE_NP_SNS_FB_ERROR_ALREADY_ABORTED              0x80024516
#define SCE_NP_SNS_FB_ERROR_CONFIG_DISABLED              0x80024517
#define SCE_NP_SNS_FB_ERROR_FBSERVER_ERROR_RESPONSE      0x80024518
#define SCE_NP_SNS_FB_ERROR_THROTTLE_CLOSED              0x80024519
#define SCE_NP_SNS_FB_ERROR_OPERATION_INTERVAL_VIOLATION 0x8002451a
#define SCE_NP_SNS_FB_ERROR_UNLOADED_THROTTLE            0x8002451b
#define SCE_NP_SNS_FB_ERROR_ACCESS_NOT_ALLOWED           0x8002451c

#define SCE_NP_SNS_FB_ACCESS_TOKEN_PARAM_OPTIONS_SILENT 0x00000001

#define SCE_NP_SNS_FB_INVALID_HANDLE               0
#define SCE_NP_SNS_FB_HANDLE_SLOT_MAX              4
#define SCE_NP_SNS_FB_PERMISSIONS_LENGTH_MAX       255
#define SCE_NP_SNS_FB_ACCESS_TOKEN_LENGTH_MAX      255
#define SCE_NP_SNS_FB_LONG_ACCESS_TOKEN_LENGTH_MAX 4096

#define SCE_NP_SNS_FB_THROTTLE_STATE_OPEN   0
#define SCE_NP_SNS_FB_THROTTLE_STATE_CLOSED 1

typedef uint32_t SceNpSnsFbHandle;

typedef struct SceNpSnsFbInitParams {
    void *pool ATTRIBUTE_PRXPTR;
    uint32_t poolSize;
} SceNpSnsFbInitParams;

typedef struct SceNpSnsFbAccessTokenParam {
    uint64_t fb_app_id;
    char permissions[SCE_NP_SNS_FB_PERMISSIONS_LENGTH_MAX + 1];
    uint32_t options;
} SceNpSnsFbAccessTokenParam;

typedef struct SceNpSnsFbAccessTokenResult {
    uint64_t expiration;
    char access_token[SCE_NP_SNS_FB_ACCESS_TOKEN_LENGTH_MAX + 1];
} SceNpSnsFbAccessTokenResult;

typedef struct SceNpSnsFbLongAccessTokenResult {
    uint64_t expiration;
    char access_token[SCE_NP_SNS_FB_LONG_ACCESS_TOKEN_LENGTH_MAX + 1];
} SceNpSnsFbLongAccessTokenResult;

typedef struct SceNpSnsFbCheckConfigParam {
    uint32_t size;
    uint32_t flags;
} SceNpSnsFbCheckConfigParam;

typedef struct SceNpSnsFbCheckConfigResult {
    uint32_t size;
    uint32_t enabled;
} SceNpSnsFbCheckConfigResult;

typedef struct SceNpSnsFbCheckThrottleParam {
    uint32_t size;
    SceNpSnsFbHandle handle;
} SceNpSnsFbCheckThrottleParam;

typedef struct SceNpSnsFbCheckThrottleResult {
    uint32_t size;
    uint32_t state;
    uint32_t remainingTime;
} SceNpSnsFbCheckThrottleResult;

typedef struct SceNpSnsFbLoadThrottleParam {
    uint32_t size;
    SceNpSnsFbHandle handle;
} SceNpSnsFbLoadThrottleParam;

typedef struct SceNpSnsFbLoadThrottleResult {
    uint32_t size;
    uint32_t state;
} SceNpSnsFbLoadThrottleResult;

typedef struct SceNpSnsFbStreamPublishParam {
    uint32_t size;
    const char *message ATTRIBUTE_PRXPTR;
    const char *name ATTRIBUTE_PRXPTR;
    const char *caption ATTRIBUTE_PRXPTR;
    const char *description ATTRIBUTE_PRXPTR;
    const char *link ATTRIBUTE_PRXPTR;
    const char *picture ATTRIBUTE_PRXPTR;
    const void *data ATTRIBUTE_PRXPTR;
    uint32_t dataSize;
} SceNpSnsFbStreamPublishParam;

int sceNpSnsFbInit(const SceNpSnsFbInitParams *params);
int sceNpSnsFbTerm(void);
int sceNpSnsFbCreateHandle(SceNpSnsFbHandle *handle);
int sceNpSnsFbDestroyHandle(SceNpSnsFbHandle handle);
int sceNpSnsFbAbortHandle(SceNpSnsFbHandle handle);
int sceNpSnsFbGetAccessToken(SceNpSnsFbHandle handle, const SceNpSnsFbAccessTokenParam *param, SceNpSnsFbAccessTokenResult *result);
int sceNpSnsFbGetLongAccessToken(SceNpSnsFbHandle handle, const SceNpSnsFbAccessTokenParam *param, SceNpSnsFbLongAccessTokenResult *result);
int sceNpSnsFbStreamPublish(SceNpSnsFbHandle handle, const SceNpSnsFbStreamPublishParam *param);
int sceNpSnsFbCheckThrottle(SceNpSnsFbCheckThrottleParam *param);
int sceNpSnsFbCheckConfig(SceNpSnsFbCheckConfigParam *param);
int sceNpSnsFbLoadThrottle(SceNpSnsFbHandle handle);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_SNS_H__ */
