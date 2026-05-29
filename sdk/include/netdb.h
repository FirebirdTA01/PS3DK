#ifndef __PS3DK_NETDB_H__
#define __PS3DK_NETDB_H__

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

int *_sys_net_h_errno_loc(void);

#define sys_net_h_errno (*_sys_net_h_errno_loc())

struct hostent {
    char *h_name;
    char **h_aliases;
    int h_addrtype;
    int h_length;
    char **h_addr_list;
};

#define h_addr h_addr_list[0]

#define NETDB_INTERNAL -1
#define NETDB_SUCCESS  0
#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3
#define NO_DATA        4
#define NO_ADDRESS     NO_DATA

struct hostent *gethostbyaddr(const char *addr, socklen_t len, int type);
struct hostent *gethostbyname(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_NETDB_H__ */
