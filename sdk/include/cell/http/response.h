#ifndef __PS3DK_CELL_HTTP_RESPONSE_H__
#define __PS3DK_CELL_HTTP_RESPONSE_H__

#include <stddef.h>
#include <stdint.h>

#include <cell/http/init.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellHttpResponseGetContentLength(CellHttpTransId transId,
                                     uint64_t *length);
int cellHttpResponseGetStatusCode(CellHttpTransId transId, int32_t *code);
int cellHttpResponseGetAllHeaders(CellHttpTransId transId,
                                  CellHttpHeaderPtr *headers, size_t *items,
                                  void *pool, size_t poolSize,
                                  size_t *required);
int cellHttpResponseGetHeader(CellHttpTransId transId, CellHttpHeader *header,
                              const char *name, void *pool, size_t poolSize,
                              size_t *required);
int cellHttpResponseGetStatusLine(CellHttpTransId transId,
                                  CellHttpStatusLine *status, void *pool,
                                  size_t poolSize, size_t *required);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_HTTP_RESPONSE_H__ */
