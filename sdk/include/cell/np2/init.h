#ifndef __PS3DK_CELL_NP2_INIT_H__
#define __PS3DK_CELL_NP2_INIT_H__

#include <stdint.h>
#include <stddef.h>

#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_OAUTH_ERROR_UNKNOWN                                         0x80025f01
#define SCE_NP_OAUTH_ERROR_ALREADY_INITIALIZED                             0x80025f02
#define SCE_NP_OAUTH_ERROR_NOT_INITIALIZED                                 0x80025f03
#define SCE_NP_OAUTH_ERROR_INVALID_ARGUMENT                                0x80025f04
#define SCE_NP_OAUTH_ERROR_OUT_OF_MEMORY                                   0x80025f05
#define SCE_NP_OAUTH_ERROR_OUT_OF_BUFFER                                   0x80025f06
#define SCE_NP_OAUTH_ERROR_BAD_RESPONSE                                    0x80025f07
#define SCE_NP_OAUTH_ERROR_ABORTED                                         0x80025f08
#define SCE_NP_OAUTH_ERROR_SIGNED_OUT                                      0x80025f09
#define SCE_NP_OAUTH_ERROR_REQUEST_NOT_FOUND                               0x80025f0a
#define SCE_NP_OAUTH_ERROR_SSL_ERR_CN_CHECK                                0x80025f0b
#define SCE_NP_OAUTH_ERROR_SSL_ERR_UNKNOWN_CA                              0x80025f0c
#define SCE_NP_OAUTH_ERROR_SSL_ERR_NOT_AFTER_CHECK                         0x80025f0d
#define SCE_NP_OAUTH_ERROR_SSL_ERR_NOT_BEFORE_CHECK                        0x80025f0e
#define SCE_NP_OAUTH_ERROR_SSL_ERR_INVALID_CERT                            0x80025f0f
#define SCE_NP_OAUTH_ERROR_SSL_ERR_INTERNAL                                0x80025f10
#define SCE_NP_OAUTH_ERROR_REQUEST_MAX                                     0x80025f11
#define SCE_NP_OAUTH_SERVER_ERROR_BANNED_CONSOLE                           0x80025d14
#define SCE_NP_OAUTH_SERVER_ERROR_INVALID_LOGIN                            0x82e00014
#define SCE_NP_OAUTH_SERVER_ERROR_INACTIVE_ACCOUNT                         0x82e0001b
#define SCE_NP_OAUTH_SERVER_ERROR_SUSPENDED_ACCOUNT                        0x82e0001c
#define SCE_NP_OAUTH_SERVER_ERROR_SUSPENDED_DEVICE                         0x82e0001d
#define SCE_NP_OAUTH_SERVER_ERROR_PASSWORD_EXPIRED                         0x82e00064
#define SCE_NP_OAUTH_SERVER_ERROR_TOSUA_MUST_BE_RE_ACCEPTED                0x82e00067
#define SCE_NP_OAUTH_SERVER_ERROR_TOSUA_MUST_BE_RE_ACCEPTED_FOR_SUBACCOUNT 0x82e01042
#define SCE_NP_OAUTH_SERVER_ERROR_BANNED_ACCOUNT                           0x82e01050
#define SCE_NP_OAUTH_SERVER_ERROR_SERVICE_END                              0x82e1019a
#define SCE_NP_OAUTH_SERVER_ERROR_SERVICE_UNAVAILABLE                      0x82e101f7

#define SCE_NP_AUTHORIZATION_CODE_MAX_LEN 128
#define SCE_NP_CLIENT_ID_MAX_LEN          128

#define SCE_NP2_DEFAULT_POOL_SIZE (128U * 1024U)

typedef int32_t SceNpAuthOAuthRequestId;

typedef struct SceNpClientId {
    char id[SCE_NP_CLIENT_ID_MAX_LEN + 1];
    uint8_t padding[7];
} SceNpClientId;

typedef struct SceNpAuthorizationCode {
    char code[SCE_NP_AUTHORIZATION_CODE_MAX_LEN + 1];
    uint8_t padding[7];
} SceNpAuthorizationCode;

typedef struct SceNpAuthGetAuthorizationCodeParameter {
    uint32_t size;
    const SceNpClientId *pClientId ATTRIBUTE_PRXPTR;
    const char *pScope ATTRIBUTE_PRXPTR;
} SceNpAuthGetAuthorizationCodeParameter;

#define sce_np2_initialize_default()                                      \
    __extension__ ({                                                      \
        static uint8_t __sce_np2_pool[SCE_NP2_DEFAULT_POOL_SIZE];         \
        sceNp2Init(sizeof(__sce_np2_pool), __sce_np2_pool);               \
    })

int sceNp2Init(uint32_t poolsize, void *poolptr);
int sceNp2Term(void);
int sceNpAuthOAuthInit(void);
int sceNpAuthOAuthTerm(void);
int sceNpAuthCreateOAuthRequest(void);
int sceNpAuthDeleteOAuthRequest(SceNpAuthOAuthRequestId reqId);
int sceNpAuthAbortOAuthRequest(SceNpAuthOAuthRequestId reqId);
int sceNpAuthGetAuthorizationCode(SceNpAuthOAuthRequestId reqId, const SceNpAuthGetAuthorizationCodeParameter *param, SceNpAuthorizationCode *authCode, int32_t *issuerId);
int sceNpUtilBuildCdnUrl(const char *url, char *buf, uint32_t bufSize, uint32_t *required, void *option);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP2_INIT_H__ */
