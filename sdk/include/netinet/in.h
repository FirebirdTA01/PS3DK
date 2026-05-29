#ifndef __PS3DK_NETINET_IN_H__
#define __PS3DK_NETINET_IN_H__

#include <stdint.h>
#include <sys/socket.h>

#ifndef _IN_ADDR_T_DECLARED
typedef uint32_t in_addr_t;
#define _IN_ADDR_T_DECLARED
#endif

#ifndef _IN_PORT_T_DECLARED
typedef uint16_t in_port_t;
#define _IN_PORT_T_DECLARED
#endif

#define IPPROTO_IP     0
#define IPPROTO_ICMP   1
#define IPPROTO_IGMP   2
#define IPPROTO_TCP    6
#define IPPROTO_UDP    17
#define IPPROTO_ICMPV6 58

struct in_addr {
    in_addr_t s_addr;
};

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in {
    uint8_t sin_len;
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    uint8_t sin_zero[8];
};

struct sockaddr_in6 {
    uint8_t sin6_len;
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

struct ip_mreq {
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
};

#define IN_CLASSA(i)        (((uint32_t)(i) & 0x80000000u) == 0)
#define IN_CLASSA_NET       0xff000000u
#define IN_CLASSA_NSHIFT    24
#define IN_CLASSA_HOST      0x00ffffffu
#define IN_CLASSA_MAX       128

#define IN_CLASSB(i)        (((uint32_t)(i) & 0xc0000000u) == 0x80000000u)
#define IN_CLASSB_NET       0xffff0000u
#define IN_CLASSB_NSHIFT    16
#define IN_CLASSB_HOST      0x0000ffffu
#define IN_CLASSB_MAX       65536

#define IN_CLASSC(i)        (((uint32_t)(i) & 0xe0000000u) == 0xc0000000u)
#define IN_CLASSC_NET       0xffffff00u
#define IN_CLASSC_NSHIFT    8
#define IN_CLASSC_HOST      0x000000ffu

#define IN_CLASSD(i)        (((uint32_t)(i) & 0xf0000000u) == 0xe0000000u)
#define IN_MULTICAST(i)     IN_CLASSD(i)

#define INADDR_ANY          0x00000000u
#define INADDR_LOOPBACK     0x7f000001u
#define INADDR_BROADCAST    0xffffffffu
#define INADDR_NONE         0xffffffffu
#define IN_LOOPBACKNET      127

#define INET_ADDRSTRLEN     16
#define INET6_ADDRSTRLEN    46

#define IP_HDRINCL          2
#define IP_TOS              3
#define IP_TTL              4
#define IP_MULTICAST_IF     9
#define IP_MULTICAST_TTL    10
#define IP_MULTICAST_LOOP   11
#define IP_ADD_MEMBERSHIP   12
#define IP_DROP_MEMBERSHIP  13
#define IP_TTLCHK           23
#define IP_MAXTTL           24
#define IP_DONTFRAG         26

#define IP_DEFAULT_MULTICAST_TTL  1
#define IP_DEFAULT_MULTICAST_LOOP 1

#define htonl(x) ((uint32_t)(x))
#define htons(x) ((uint16_t)(x))
#define ntohl(x) ((uint32_t)(x))
#define ntohs(x) ((uint16_t)(x))

#endif /* __PS3DK_NETINET_IN_H__ */
