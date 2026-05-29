#ifndef __PS3DK_CELL_HTTP_INIT_H__
#define __PS3DK_CELL_HTTP_INIT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cell/http/util.h>
#include <cell/ssl.h>
#include <ppu-types.h>

#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct CellHttpClient;
struct CellHttpTransaction;
typedef struct CellHttpClient *CellHttpClientId ATTRIBUTE_PRXPTR;
typedef struct CellHttpTransaction *CellHttpTransId ATTRIBUTE_PRXPTR;
typedef const void *CellHttpSslId ATTRIBUTE_PRXPTR;

typedef struct CellHttpsData {
    char *ptr ATTRIBUTE_PRXPTR;
    uint32_t size;
} CellHttpsData;

#define CELL_HTTP_MAX_USERNAME 256
#define CELL_HTTP_MAX_PASSWORD 256

enum {
    CELL_HTTP_DEFAULT_POOL_SIZE = 128 * 1024
};

#define cell_http_initialize_default() ({                       \
    static uint8_t __cell_http_pool[CELL_HTTP_DEFAULT_POOL_SIZE]; \
    cellHttpInit(__cell_http_pool, sizeof(__cell_http_pool));   \
})

typedef int (*CellHttpAuthenticationCallback)(
    CellHttpTransId transId, const char *realm, const CellHttpUri *uri,
    char *username, char *password, bool *save, void *userArg);
typedef int (*CellHttpTransactionStateCallback)(CellHttpTransId transId,
                                                int32_t state,
                                                void *userArg);
typedef int (*CellHttpRedirectCallback)(CellHttpTransId transId,
                                        const CellHttpStatusLine *response,
                                        const CellHttpUri *from,
                                        const CellHttpUri *to,
                                        void *userArg);
typedef int (*CellHttpsSslCallback)(uint32_t verifyErr,
                                    CellSslCert const sslCerts[],
                                    int certNum, const char *hostname,
                                    CellHttpSslId id, void *userArg);
typedef int (*CellHttpCookieSendCallback)(CellHttpTransId transId,
                                          const CellHttpUri *uri,
                                          const char *cookieValue,
                                          void *userArg);
typedef int (*CellHttpCookieRecvCallback)(CellHttpTransId transId,
                                          const CellHttpUri *uri,
                                          const char *cookieValue,
                                          void *userArg);
typedef int (*CellHttpSslIdDestroyCallback)(CellHttpSslId id, void *userArg);

#define CELL_HTTPS_VERIFY_ERROR_NONE           0x00000000U
#define CELL_HTTPS_VERIFY_ERROR_NO_CERT        0x00000001U
#define CELL_HTTPS_VERIFY_ERROR_BAD_SSL        0x00000002U
#define CELL_HTTPS_VERIFY_ERROR_BAD_CLIENT     0x00000004U
#define CELL_HTTPS_VERIFY_ERROR_UNKNOWN_CA     0x00000008U
#define CELL_HTTPS_VERIFY_ERROR_BAD_CHAIN      0x00000010U
#define CELL_HTTPS_VERIFY_ERROR_NO_MEMORY      0x00000020U
#define CELL_HTTPS_VERIFY_ERROR_NOT_VERIFIABLE 0x00000040U
#define CELL_HTTPS_VERIFY_ERROR_INVALID_CERT   0x00000080U
#define CELL_HTTPS_VERIFY_ERROR_BAD_CONSTRAINT 0x00000100U
#define CELL_HTTPS_VERIFY_ERROR_VERIFY_FAILED  0x00000200U
#define CELL_HTTPS_VERIFY_ERROR_COMMON_NAME    0x00000400U
#define CELL_HTTPS_VERIFY_ERROR_EXPIRED        0x00000800U
#define CELL_HTTPS_VERIFY_ERROR_NOT_YET_VALID  0x00001000U

int cellHttpInit(void *pool, size_t poolSize);
int cellHttpEnd(void);
int cellHttpsInit(size_t caCertNum, const CellHttpsData *caList);
int cellHttpsEnd(void);
int cellHttpInitCookie(void *pool, size_t poolSize);
int cellHttpEndCookie(void);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_HTTP_INIT_H__ */
