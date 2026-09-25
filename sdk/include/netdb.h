#ifndef __PS3DK_NETDB_H__
#define __PS3DK_NETDB_H__

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

int *_sys_net_h_errno_loc(void);
extern int h_errno;

#define sys_net_h_errno (*_sys_net_h_errno_loc())

/* Legacy address-info declarations; no getaddrinfo implementation implied. */
#define NI_MAXHOST 1025
#define NI_MAXSERV 32
#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
#define NI_NOFQDN 4
#define NI_NAMEREQD 8
#define NI_DGRAM 16
#define NI_IDN 32
#define NI_IDN_ALLOW_UNASSIGNED 64
#define NI_IDN_USE_STD3_ASCII_RULES 128
#define AI_PASSIVE 0x0001
#define AI_CANONNAME 0x0002
#define AI_NUMERICHOST 0x0004
#define AI_V4MAPPED 0x0008
#define AI_ALL 0x0010
#define AI_ADDRCONFIG 0x0020
#define AI_IDN 0x0040
#define AI_CANONIDN 0x0080
#define AI_IDN_ALLOW_UNASSIGNED 0x0100
#define AI_IDN_USE_STD3_ASCII_RULES 0x0200
struct addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

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

/* Results are native pointers, owned by the calling thread, and remain valid
 * until its next host lookup (or thread exit). No raw firmware struct cast.
 * Resolver lists are bounded to 1024 entries and strings to 1024 bytes;
 * malformed/oversized firmware results fail with NO_RECOVERY/EOVERFLOW.
 */
struct servent {
    char *s_name;
    char **s_aliases;
    int s_port; /* network byte order, as required by BSD */
    char *s_proto;
};
/* Firmware has no service database. SDK supplies the documented common-service
 * table; unknown service/protocol pairs return NULL. Result is thread-local. */
struct servent *getservbyport(int port, const char *protocol);
struct servent *getservbyname(const char *name, const char *protocol);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_NETDB_H__ */
