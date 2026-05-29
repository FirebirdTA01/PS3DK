#ifndef __PS3DK_SYS_SOCKET_H__
#define __PS3DK_SYS_SOCKET_H__

#include <stddef.h>
#include <stdint.h>
#include <sys/_types.h>
#include <ppu-types.h>

#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif

#ifndef _SSIZE_T_DECLARED
typedef _ssize_t ssize_t;
#define _SSIZE_T_DECLARED
#endif

#ifndef __PS3DK_SOCKLEN_T_DECLARED
typedef uint32_t socklen_t;
#define __PS3DK_SOCKLEN_T_DECLARED
#endif

#ifndef __PS3DK_SA_FAMILY_T_DECLARED
typedef uint8_t sa_family_t;
#define __PS3DK_SA_FAMILY_T_DECLARED
#endif

#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3
#define SOCK_DGRAM_P2P  6
#define SOCK_STREAM_P2P 10

#define SOL_SOCKET      0xffff

#define SO_REUSEADDR    0x0004
#define SO_KEEPALIVE    0x0008
#define SO_BROADCAST    0x0020
#define SO_LINGER       0x0080
#define SO_OOBINLINE    0x0100
#define SO_REUSEPORT    0x0200
#define SO_ONESBCAST    0x0800
#define SO_USECRYPTO    0x1000
#define SO_USESIGNATURE 0x2000
#define SO_SNDBUF       0x1001
#define SO_RCVBUF       0x1002
#define SO_SNDLOWAT     0x1003
#define SO_RCVLOWAT     0x1004
#define SO_SNDTIMEO     0x1005
#define SO_RCVTIMEO     0x1006
#define SO_ERROR        0x1007
#define SO_TYPE         0x1008
#define SO_NBIO         0x1100
#define SO_TPPOLICY     0x1101

#define AF_UNSPEC       0
#define AF_LOCAL        1
#define AF_UNIX         AF_LOCAL
#define AF_INET         2
#define AF_INET6        24
#define AF_MAX          31

#define PF_UNSPEC       AF_UNSPEC
#define PF_LOCAL        AF_LOCAL
#define PF_UNIX         AF_UNIX
#define PF_INET         AF_INET
#define PF_INET6        AF_INET6

#define MSG_OOB          0x0001
#define MSG_PEEK         0x0002
#define MSG_DONTROUTE    0x0004
#define MSG_EOR          0x0008
#define MSG_TRUNC        0x0010
#define MSG_CTRUNC       0x0020
#define MSG_WAITALL      0x0040
#define MSG_DONTWAIT     0x0080
#define MSG_BCAST        0x0100
#define MSG_MCAST        0x0200
#define MSG_USECRYPTO    0x0400
#define MSG_USESIGNATURE 0x0800

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define SOCKET_FD_MASK 0x40000000

struct sockaddr {
    uint8_t sa_len;
    sa_family_t sa_family;
    char sa_data[14];
};

struct iovec {
    void *iov_base ATTRIBUTE_PRXPTR;
    uint32_t iov_len;
};

struct msghdr {
    void *msg_name ATTRIBUTE_PRXPTR;
    socklen_t msg_namelen;
    struct iovec *msg_iov ATTRIBUTE_PRXPTR;
    int32_t msg_iovlen;
    void *msg_control ATTRIBUTE_PRXPTR;
    socklen_t msg_controllen;
    int32_t msg_flags;
};

struct cmsghdr {
    socklen_t cmsg_len;
    int32_t cmsg_level;
    int32_t cmsg_type;
};

struct linger {
    int32_t l_onoff;
    int32_t l_linger;
};

#ifdef __cplusplus
extern "C" {
#endif

int accept(int s, struct sockaddr *addr, socklen_t *addrlen);
int bind(int s, const struct sockaddr *addr, socklen_t addrlen);
int connect(int s, const struct sockaddr *name, socklen_t namelen);
int getpeername(int s, struct sockaddr *name, socklen_t *namelen);
int getsockname(int s, struct sockaddr *name, socklen_t *namelen);
int getsockopt(int s, int level, int optname, void *optval,
               socklen_t *optlen);
int listen(int s, int backlog);
ssize_t recv(int s, void *buf, size_t len, int flags);
ssize_t recvfrom(int s, void *buf, size_t len, int flags,
                 struct sockaddr *from, socklen_t *fromlen);
ssize_t recvmsg(int s, struct msghdr *msg, int flags);
ssize_t send(int s, const void *msg, size_t len, int flags);
ssize_t sendto(int s, const void *msg, size_t len, int flags,
               const struct sockaddr *to, socklen_t tolen);
ssize_t sendmsg(int s, const struct msghdr *msg, int flags);
int setsockopt(int s, int level, int optname, const void *optval,
               socklen_t optlen);
int shutdown(int s, int how);
int socket(int domain, int type, int protocol);
int socketclose(int s);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SYS_SOCKET_H__ */
