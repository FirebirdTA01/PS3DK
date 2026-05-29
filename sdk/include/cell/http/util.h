#ifndef __PS3DK_CELL_HTTP_UTIL_H__
#define __PS3DK_CELL_HTTP_UTIL_H__

#include <stddef.h>
#include <stdint.h>
#include <ppu-types.h>

#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CellHttpUri {
    const char *scheme ATTRIBUTE_PRXPTR;
    const char *hostname ATTRIBUTE_PRXPTR;
    const char *username ATTRIBUTE_PRXPTR;
    const char *password ATTRIBUTE_PRXPTR;
    const char *path ATTRIBUTE_PRXPTR;
    uint32_t port;
    uint8_t reserved[4];
} CellHttpUri;

typedef struct CellHttpUriPath {
    const char *path ATTRIBUTE_PRXPTR;
    const char *query ATTRIBUTE_PRXPTR;
    const char *fragment ATTRIBUTE_PRXPTR;
} CellHttpUriPath;

typedef struct CellHttpRequestLine {
    const char *method ATTRIBUTE_PRXPTR;
    const char *path ATTRIBUTE_PRXPTR;
    const char *protocol ATTRIBUTE_PRXPTR;
    uint32_t majorVersion;
    uint32_t minorVersion;
} CellHttpRequestLine;

typedef struct CellHttpStatusLine {
    const char *protocol ATTRIBUTE_PRXPTR;
    uint32_t majorVersion;
    uint32_t minorVersion;
    const char *reasonPhrase ATTRIBUTE_PRXPTR;
    int32_t statusCode;
    uint8_t reserved[4];
} CellHttpStatusLine;

typedef struct CellHttpHeader {
    const char *name ATTRIBUTE_PRXPTR;
    const char *value ATTRIBUTE_PRXPTR;
} CellHttpHeader;
typedef CellHttpHeader *CellHttpHeaderPtr ATTRIBUTE_PRXPTR;

#define CELL_HTTP_UTIL_BASE64_ENC_BUF_SIZE(_size) (((_size) + 2) / 3 * 4)
#define CELL_HTTP_UTIL_BASE64_DEC_BUF_SIZE(_size) ((_size) / 4 * 3)

#define CELL_HTTP_UTIL_URI_FLAG_FULL_URI       0x00000000
#define CELL_HTTP_UTIL_URI_FLAG_NO_SCHEME      0x00000001
#define CELL_HTTP_UTIL_URI_FLAG_NO_CREDENTIALS 0x00000002
#define CELL_HTTP_UTIL_URI_FLAG_NO_PASSWORD    0x00000004
#define CELL_HTTP_UTIL_URI_FLAG_NO_PATH        0x00000008

int cellHttpUtilEscapeUri(char *out, size_t outSize, const unsigned char *in,
                          size_t inSize, size_t *required);
int cellHttpUtilUnescapeUri(unsigned char *out, size_t size, const char *in,
                            size_t *required);
int cellHttpUtilFormUrlEncode(char *out, size_t outSize,
                              const unsigned char *in, size_t inSize,
                              size_t *required);
int cellHttpUtilFormUrlDecode(unsigned char *out, size_t size, const char *in,
                              size_t *required);
int cellHttpUtilBase64Encoder(char *out, const void *input, size_t len);
int cellHttpUtilBase64Decoder(void *output, const char *in, size_t len);
int cellHttpUtilCopyUri(CellHttpUri *dest, const CellHttpUri *src, void *pool,
                        size_t poolSize, size_t *required);
int cellHttpUtilCopyHeader(CellHttpHeader *dest, const CellHttpHeader *src,
                           void *pool, size_t poolSize, size_t *required);
int cellHttpUtilCopyStatusLine(CellHttpStatusLine *dest,
                               const CellHttpStatusLine *src, void *pool,
                               size_t poolSize, size_t *required);
int cellHttpUtilMergeUriPath(CellHttpUri *uri, const CellHttpUri *src,
                             const char *path, void *pool, size_t poolSize,
                             size_t *required);
int cellHttpUtilAppendHeaderValue(CellHttpHeader *dest,
                                  const CellHttpHeader *src,
                                  const char *value, void *pool,
                                  size_t poolSize, size_t *required);
int cellHttpUtilParseUri(CellHttpUri *uri, const char *str, void *pool,
                         size_t size, size_t *required);
int cellHttpUtilParseUriPath(CellHttpUriPath *path, const char *str,
                             void *pool, size_t size, size_t *required);
int cellHttpUtilParseProxy(CellHttpUri *proxy, const char *str, void *pool,
                           size_t size, size_t *required);
int cellHttpUtilParseStatusLine(CellHttpStatusLine *resp, const char *str,
                                size_t len, void *pool, size_t size,
                                size_t *required, size_t *parsedLength);
int cellHttpUtilParseHeader(CellHttpHeader *header, const char *str,
                            size_t len, void *pool, size_t size,
                            size_t *required, size_t *parsedLength);
int cellHttpUtilBuildRequestLine(const CellHttpRequestLine *req, char *buf,
                                 size_t len, size_t *required);
int cellHttpUtilBuildHeader(const CellHttpHeader *header, char *buf,
                            size_t len, size_t *required);
int cellHttpUtilBuildUri(const CellHttpUri *uri, char *buf, size_t len,
                         size_t *required, int32_t flags);
int cellHttpUtilSweepPath(char *dst, const char *src, size_t srcSize);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_HTTP_UTIL_H__ */
