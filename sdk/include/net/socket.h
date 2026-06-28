/*
 * PS3 Custom Toolchain — <net/socket.h> compat override.
 *
 * PSL1GHT's <net/net.h> includes <net/socket.h> for the BSD socket types,
 * while our <netinet/in.h> pulls <sys/socket.h>.  Both header families
 * define sockaddr / iovec / msghdr / cmsghdr / linger, so a sample that
 * includes the PSL1GHT net stack *and* the POSIX headers (e.g. networktest,
 * debugtest, echoserv) hits "redefinition of struct".
 *
 * This override (installed over PSL1GHT's net/socket.h) routes the socket
 * structs/constants to the single POSIX source <sys/socket.h>, so the two
 * families agree.  The PSL1GHT net API (netInitialize/netSocket/netSend/...)
 * still comes from <net/net.h> itself.
 */
#ifndef __PS3DK_COMPAT_NET_SOCKET_H__
#define __PS3DK_COMPAT_NET_SOCKET_H__

#include <sys/socket.h>

#endif /* __PS3DK_COMPAT_NET_SOCKET_H__ */
