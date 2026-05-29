#ifndef __PS3DK_CELL_HTTP_CLIENT_H__
#define __PS3DK_CELL_HTTP_CLIENT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cell/http/init.h>

#ifdef __cplusplus
extern "C" {
#endif

static const char CELL_HTTP_METHOD_OPTIONS[] = "OPTIONS";
static const char CELL_HTTP_METHOD_GET[] = "GET";
static const char CELL_HTTP_METHOD_HEAD[] = "HEAD";
static const char CELL_HTTP_METHOD_POST[] = "POST";
static const char CELL_HTTP_METHOD_PUT[] = "PUT";
static const char CELL_HTTP_METHOD_DELETE[] = "DELETE";
static const char CELL_HTTP_METHOD_TRACE[] = "TRACE";

#define CELL_HTTP_TRANSACTION_STATE_GETTING_CONNECTION     1
#define CELL_HTTP_TRANSACTION_STATE_PREPARING_REQUEST      2
#define CELL_HTTP_TRANSACTION_STATE_SENDING_REQUEST        3
#define CELL_HTTP_TRANSACTION_STATE_SENDING_BODY           4
#define CELL_HTTP_TRANSACTION_STATE_WAITING_FOR_REPLY      5
#define CELL_HTTP_TRANSACTION_STATE_READING_REPLY          6
#define CELL_HTTP_TRANSACTION_STATE_SETTING_REDIRECTION    7
#define CELL_HTTP_TRANSACTION_STATE_SETTING_AUTHENTICATION 8

int cellHttpCreateClient(CellHttpClientId *clientId);
int cellHttpDestroyClient(CellHttpClientId clientId);
int cellHttpClientSetProxy(CellHttpClientId clientId,
                           const CellHttpUri *proxy);
int cellHttpClientGetProxy(CellHttpClientId clientId, CellHttpUri *proxy,
                           void *pool, size_t poolSize, size_t *required);
int cellHttpClientSetVersion(CellHttpClientId clientId, uint32_t major,
                             uint32_t minor);
int cellHttpClientGetVersion(CellHttpClientId clientId, uint32_t *major,
                             uint32_t *minor);
int cellHttpClientSetPipeline(CellHttpClientId clientId, bool enable);
int cellHttpClientGetPipeline(CellHttpClientId clientId, bool *enable);
int cellHttpClientSetKeepAlive(CellHttpClientId clientId, bool enable);
int cellHttpClientGetKeepAlive(CellHttpClientId clientId, bool *enable);
int cellHttpClientSetAutoRedirect(CellHttpClientId clientId, bool enable);
int cellHttpClientGetAutoRedirect(CellHttpClientId clientId, bool *enable);
int cellHttpClientSetAutoAuthentication(CellHttpClientId clientId,
                                        bool enable);
int cellHttpClientGetAutoAuthentication(CellHttpClientId clientId,
                                        bool *enable);
int cellHttpClientSetAuthenticationCacheStatus(CellHttpClientId clientId,
                                               bool enable);
int cellHttpClientGetAuthenticationCacheStatus(CellHttpClientId clientId,
                                               bool *enable);
int cellHttpClientSetCookieStatus(CellHttpClientId clientId, bool enable);
int cellHttpClientGetCookieStatus(CellHttpClientId clientId, bool *enable);
int cellHttpClientSetAuthenticationCallback(
    CellHttpClientId clientId, CellHttpAuthenticationCallback cbfunc,
    void *userArg);
int cellHttpClientSetTransactionStateCallback(
    CellHttpClientId clientId, CellHttpTransactionStateCallback cbfunc,
    void *userArg);
int cellHttpClientSetRedirectCallback(CellHttpClientId clientId,
                                      CellHttpRedirectCallback cbfunc,
                                      void *userArg);
int cellHttpClientSetUserAgent(CellHttpClientId clientId,
                               const char *userAgent);
int cellHttpClientGetUserAgent(CellHttpClientId clientId, char *userAgent,
                               size_t size, size_t *required);
int cellHttpClientCloseAllConnections(CellHttpClientId clientId);
int cellHttpClientCloseConnections(CellHttpClientId clientId,
                                   const CellHttpUri *uri);
int cellHttpClientPollConnections(CellHttpClientId clientId,
                                  CellHttpTransId *transId, int64_t usec);
int cellHttpClientSetRecvTimeout(CellHttpClientId clientId, int64_t usec);
int cellHttpClientGetRecvTimeout(CellHttpClientId clientId, int64_t *usec);
int cellHttpClientSetSendTimeout(CellHttpClientId clientId, int64_t usec);
int cellHttpClientGetSendTimeout(CellHttpClientId clientId, int64_t *usec);
int cellHttpClientSetConnTimeout(CellHttpClientId clientId, int64_t usec);
int cellHttpClientGetConnTimeout(CellHttpClientId clientId, int64_t *usec);
int cellHttpClientSetTotalPoolSize(CellHttpClientId clientId,
                                   size_t poolSize);
int cellHttpClientGetTotalPoolSize(CellHttpClientId clientId,
                                   size_t *poolSize);
int cellHttpClientSetPerHostPoolSize(CellHttpClientId clientId,
                                     size_t poolSize);
int cellHttpClientGetPerHostPoolSize(CellHttpClientId clientId,
                                     size_t *poolSize);
int cellHttpClientSetPerHostKeepAliveMax(CellHttpClientId clientId,
                                         size_t maxSize);
int cellHttpClientGetPerHostKeepAliveMax(CellHttpClientId clientId,
                                         size_t *maxSize);
int cellHttpClientSetPerPipelineMax(CellHttpClientId clientId,
                                    size_t pipeMax);
int cellHttpClientGetPerPipelineMax(CellHttpClientId clientId,
                                    size_t *pipeMax);
int cellHttpClientSetResponseBufferMax(CellHttpClientId clientId, size_t max);
int cellHttpClientGetResponseBufferMax(CellHttpClientId clientId,
                                       size_t *max);
int cellHttpClientSetRecvBufferSize(CellHttpClientId clientId, int size);
int cellHttpClientGetRecvBufferSize(CellHttpClientId clientId, int *size);
int cellHttpClientSetSendBufferSize(CellHttpClientId clientId, int size);
int cellHttpClientGetSendBufferSize(CellHttpClientId clientId, int *size);
int cellHttpClientGetAllHeaders(CellHttpClientId clientId,
                                CellHttpHeaderPtr *headers, size_t *items,
                                void *pool, size_t poolSize,
                                size_t *required);
int cellHttpClientGetHeader(CellHttpClientId clientId, CellHttpHeader *header,
                            const char *name, void *pool, size_t poolSize,
                            size_t *required);
int cellHttpClientSetHeader(CellHttpClientId clientId,
                            const CellHttpHeader *header);
int cellHttpClientAddHeader(CellHttpClientId clientId,
                            const CellHttpHeader *header);
int cellHttpClientDeleteHeader(CellHttpClientId clientId, const char *name);
int cellHttpClientSetMinSslVersion(CellHttpClientId clientId,
                                   int32_t version);
int cellHttpClientGetMinSslVersion(CellHttpClientId clientId,
                                   int32_t *version);
int cellHttpClientSetSslVersion(CellHttpClientId clientId, int32_t version);
int cellHttpClientGetSslVersion(CellHttpClientId clientId, int32_t *version);
int cellHttpClientSetSslCallback(CellHttpClientId clientId,
                                 CellHttpsSslCallback cbfunc,
                                 void *userArg);
int cellHttpClientSetSslClientCertificate(CellHttpClientId clientId,
                                          const CellHttpsData *cert,
                                          const CellHttpsData *privKey);
int cellHttpClientSetSslIdDestroyCallback(
    CellHttpClientId clientId, CellHttpSslIdDestroyCallback cbfunc,
    void *userArg);
int cellHttpClientSetConnectionWaitStatus(CellHttpClientId clientId,
                                          bool enable);
int cellHttpClientGetConnectionWaitStatus(CellHttpClientId clientId,
                                          bool *enable);
int cellHttpClientSetConnectionWaitTimeout(CellHttpClientId clientId,
                                           int64_t usec);
int cellHttpClientGetConnectionWaitTimeout(CellHttpClientId clientId,
                                           int64_t *usec);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_HTTP_CLIENT_H__ */
