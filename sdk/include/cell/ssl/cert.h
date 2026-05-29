#ifndef __PS3DK_CELL_SSL_CERT_H__
#define __PS3DK_CELL_SSL_CERT_H__

#include <stddef.h>
#include <stdint.h>

#include <cell/rtc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef const void *CellSslCert;
typedef const void *CellSslCertName;

#define CELL_SSL_MD5_FINGERPRINT_MAX_SIZE 20

int cellSslCertificateLoader(uint64_t flag, char *buffer, size_t size,
                             size_t *required);
int cellSslCertGetSerialNumber(CellSslCert sslCert, const uint8_t **sboData,
                               size_t *sboLength);
int cellSslCertGetPublicKey(CellSslCert sslCert, const uint8_t **sboData,
                            size_t *sboLength);
int cellSslCertGetRsaPublicKeyModulus(CellSslCert sslCert,
                                      const uint8_t **sboData,
                                      size_t *sboLength);
int cellSslCertGetRsaPublicKeyExponent(CellSslCert sslCert,
                                       const uint8_t **sboData,
                                       size_t *sboLength);
int cellSslCertGetNotBefore(CellSslCert sslCert, CellRtcTick *begin);
int cellSslCertGetNotAfter(CellSslCert sslCert, CellRtcTick *limit);
int cellSslCertGetSubjectName(CellSslCert sslCert, CellSslCertName *certName);
int cellSslCertGetIssuerName(CellSslCert sslCert, CellSslCertName *certName);
int cellSslCertGetNameEntryCount(CellSslCertName certName, uint32_t *entryCount);
int cellSslCertGetNameEntryInfo(CellSslCertName certName, uint32_t entryNum,
                                const char **oidName, const uint8_t **value,
                                size_t *valueLength, int32_t flag);
int cellSslCertGetMd5Fingerprint(CellSslCert sslCert, uint8_t *buf,
                                 uint32_t *plen);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_SSL_CERT_H__ */
