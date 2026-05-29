#ifndef __PS3DK_CELL_HTTP_TRANSACTION_H__
#define __PS3DK_CELL_HTTP_TRANSACTION_H__

#include <stddef.h>
#include <stdint.h>

#include <cell/http/init.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellHttpCreateTransaction(CellHttpTransId *transId,
                              CellHttpClientId clientId, const char *method,
                              const CellHttpUri *uri);
int cellHttpDestroyTransaction(CellHttpTransId transId);
int cellHttpSendRequest(CellHttpTransId transId, const void *buf, size_t size,
                        size_t *sent);
int cellHttpRecvResponse(CellHttpTransId transId, void *buf, size_t size,
                         size_t *recvd);
int cellHttpTransactionGetUri(CellHttpTransId transId, CellHttpUri *uri,
                              void *pool, size_t poolSize, size_t *required);
int cellHttpTransactionCloseConnection(CellHttpTransId transId);
int cellHttpTransactionReleaseConnection(CellHttpTransId transId, int *sid);
int cellHttpTransactionAbortConnection(CellHttpTransId transId);
int cellHttpTransactionGetSslCipherName(CellHttpTransId transId, char *name,
                                        size_t size, size_t *required);
int cellHttpTransactionGetSslCipherId(CellHttpTransId transId, int32_t *id);
int cellHttpTransactionGetSslCipherVersion(CellHttpTransId transId,
                                           char *version, size_t size,
                                           size_t *required);
int cellHttpTransactionGetSslCipherBits(CellHttpTransId transId,
                                        int32_t *effectiveBits,
                                        int32_t *algorithmBits);
int cellHttpTransactionGetSslCipherString(CellHttpTransId transId,
                                          char *buffer, size_t size);
int cellHttpTransactionGetSslVersion(CellHttpTransId transId,
                                     int32_t *version);
int cellHttpTransactionGetSslId(CellHttpTransId transId, CellHttpSslId *id);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_HTTP_TRANSACTION_H__ */
