#ifndef __PS3DK_CELL_HTTP_REQUEST_H__
#define __PS3DK_CELL_HTTP_REQUEST_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cell/http/init.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellHttpRequestSetContentLength(CellHttpTransId transId,
                                    uint64_t totalSize);
int cellHttpRequestGetContentLength(CellHttpTransId transId,
                                    uint64_t *totalSize);
int cellHttpRequestGetChunkedTransferStatus(CellHttpTransId transId,
                                            bool *enable);
int cellHttpRequestSetChunkedTransferStatus(CellHttpTransId transId,
                                            bool enable);
int cellHttpRequestGetAllHeaders(CellHttpTransId transId,
                                 CellHttpHeaderPtr *headers, size_t *items,
                                 void *pool, size_t poolSize,
                                 size_t *required);
int cellHttpRequestGetHeader(CellHttpTransId transId, CellHttpHeader *header,
                             const char *name, void *pool, size_t poolSize,
                             size_t *required);
int cellHttpRequestSetHeader(CellHttpTransId transId,
                             const CellHttpHeader *header);
int cellHttpRequestAddHeader(CellHttpTransId transId,
                             const CellHttpHeader *header);
int cellHttpRequestDeleteHeader(CellHttpTransId transId, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_HTTP_REQUEST_H__ */
