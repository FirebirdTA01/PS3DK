#ifndef __PS3DK_CELL_HTTP_PROXY_H__
#define __PS3DK_CELL_HTTP_PROXY_H__

#include <stddef.h>

#include <cell/http/init.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellHttpSetProxy(const CellHttpUri *proxy);
int cellHttpGetProxy(CellHttpUri *proxy, void *pool, size_t poolSize,
                     size_t *required);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_HTTP_PROXY_H__ */
