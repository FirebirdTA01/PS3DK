#ifndef __PS3DK_CELL_SSL_SSL_H__
#define __PS3DK_CELL_SSL_SSL_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_SSL_ERROR_NOT_INITIALIZED       0x80740001
#define CELL_SSL_ERROR_ALREADY_INITIALIZED   0x80740002
#define CELL_SSL_ERROR_INITIALIZATION_FAILED 0x80740003
#define CELL_SSL_ERROR_NO_BUFFER             0x80740004
#define CELL_SSL_ERROR_INVALID_CERTIFICATE   0x80740005
#define CELL_SSL_ERROR_UNRETRIEVABLE         0x80740006
#define CELL_SSL_ERROR_INVALID_FORMAT        0x80740007
#define CELL_SSL_ERROR_NOT_FOUND             0x80740008
#define CELL_SSL_ERROR_INVALID_TIME          0x80740031
#define CELL_SSL_ERROR_INVALID_NEGATIVE_TIME 0x80740032
#define CELL_SSL_ERROR_INCORRECT_TIME        0x80740033
#define CELL_SSL_ERROR_UNDEFINED_TIME_TYPE   0x80740034
#define CELL_SSL_ERROR_NO_MEMORY             0x80740035
#define CELL_SSL_ERROR_NO_STRING             0x80740036
#define CELL_SSL_ERROR_UNKNOWN_LOAD_CERT     0x80740037

enum {
    CELL_SSL_DEFAULT_POOL_SIZE = 128 * 1024
};

#define cell_ssl_initialize_default() ({                      \
    static uint8_t __cell_ssl_pool[CELL_SSL_DEFAULT_POOL_SIZE]; \
    cellSslInit(__cell_ssl_pool, sizeof(__cell_ssl_pool));     \
})

int cellSslInit(void *pool, size_t poolSize);
int cellSslEnd(void);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SSL_SSL_H__ */
