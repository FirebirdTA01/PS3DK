#ifndef __PS3DK_CELL_HTTP_COOKIE_H__
#define __PS3DK_CELL_HTTP_COOKIE_H__

#include <stddef.h>

#include <cell/http/init.h>

#ifdef __cplusplus
extern "C" {
#endif

int cellHttpAddCookieWithClientId(const CellHttpUri *uri, const char *cookie,
                                  CellHttpClientId clientId);
int cellHttpSessionCookieFlush(CellHttpClientId clientId);
int cellHttpCookieExportWithClientId(void *buffer, size_t size,
                                     size_t *exportSize,
                                     CellHttpClientId clientId);
int cellHttpCookieImportWithClientId(const void *buffer, size_t size,
                                     CellHttpClientId clientId);
int cellHttpClientSetCookieSendCallback(CellHttpClientId clientId,
                                        CellHttpCookieSendCallback cbfunc,
                                        void *userArg);
int cellHttpClientSetCookieRecvCallback(CellHttpClientId clientId,
                                        CellHttpCookieRecvCallback cbfunc,
                                        void *userArg);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_HTTP_COOKIE_H__ */
