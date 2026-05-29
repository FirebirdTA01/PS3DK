#ifndef __PS3DK_ARPA_INET_H__
#define __PS3DK_ARPA_INET_H__

#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

in_addr_t inet_addr(const char *cp);
int inet_aton(const char *cp, struct in_addr *addr);
in_addr_t inet_lnaof(struct in_addr in);
struct in_addr inet_makeaddr(in_addr_t net, in_addr_t lna);
in_addr_t inet_netof(struct in_addr in);
in_addr_t inet_network(const char *cp);
char *inet_ntoa(struct in_addr in);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int inet_pton(int af, const char *src, void *dst);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_ARPA_INET_H__ */
