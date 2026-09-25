/* SPDX-License-Identifier: BSD-2-Clause
 * PSL1GHT compatibility: netSocket/netAccept return RAW PRX descriptors.
 * Never pass these to BSD close/read/write/send/recv. The BSD family uses
 * tagged descriptors and native errno; the raw family uses net_errno.
 * New applications should use the BSD family after netInitialize().
 */
#ifndef __PS3DK_NET_NET_H__
#define __PS3DK_NET_NET_H__
#include <cell/libnet.h>
#include <netdb.h>
#include <net/errno.h>
#define net_errno (*netErrnoLoc())
#define net_h_errno (*netHErrnoLoc())
typedef struct { uint32_t memory, memory_size; int32_t flags; } netInitParam;
typedef struct {
    int32_t s, proto, recv_queue_len, send_queue_len;
    struct in_addr local_adr;
    int32_t local_port;
    struct in_addr remote_adr;
    int32_t remote_port, state;
} netSocketInfo;
typedef sys_net_sockinfo_ex_t netSocketInfoEx;
struct net_hostent { uint32_t h_name, h_aliases; int32_t h_addrtype, h_length; uint32_t h_addr_list; };
struct net_msghdr {
    uint32_t _pad0, msg_name, msg_namlen, _pad1, _pad2, msg_iov;
    int32_t msg_iovlen;
    uint32_t _pad3, _pad4, msg_control, msg_controllen;
    int32_t msg_flags;
};
#ifdef __cplusplus
extern "C" {
#endif
int netAbortResolver(sys_net_thread_id_t t, int f);
int netAbortSocket(int s, int f);
int netCloseDump(int id, int *f);
int netFreethreadContext(sys_net_thread_id_t t, int f);
int netGetLibNameServer(struct in_addr *a, struct in_addr *b);
int netGetNetEmuTestParam(sys_net_test_param_t *p);
int netGetTestParam(sys_net_test_param_t *p);
int netGetUdpp2pTestparam(sys_net_test_param_t *p);
int netIfCtl(int i, int c, void *p, int n);
int netOpenDump(int n, int f);
int netReadDump(int i, void *b, int n, int *f);
int netSetResolverConfigurations(int a, int b, int c);
int netSetTestParam(sys_net_test_param_t *p);
int netSetUdpp2pTestParam(sys_net_test_param_t *p);
int netSetlibNameServer(struct in_addr *a, struct in_addr *b);
int netSetnetemutestparam(sys_net_test_param_t *p);
int netShowNameserver(void);
int netInitialize(void);
int netDeinitialize(void);
int *netErrnoLoc(void);
int *netHErrnoLoc(void);
int netInitializeNetworkEx(netInitParam *);
int netFinalizeNetwork(void);
int netGetSockInfo(int, netSocketInfo *, int);
int netGetSockInfoEx(int, netSocketInfoEx *, int, int);
int netShowIfConfig(void);
int netShowNameServer(void);
int netShowRoute(void);
int netSocket(int, int, int);
int netAccept(int, const struct sockaddr *, socklen_t *);
int netBind(int, const struct sockaddr *, socklen_t);
int netConnect(int, const struct sockaddr *, socklen_t);
int netListen(int, int);
int netClose(int);
int netShutdown(int, int);
ssize_t netRecv(int, void *, size_t, int);
ssize_t netRecvFrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
ssize_t netRecvMsg(int, struct net_msghdr *, int);
ssize_t netSend(int, const void *, size_t, int);
ssize_t netSendTo(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
ssize_t netSendMsg(int, const struct net_msghdr *, int);
int netPoll(struct pollfd *, nfds_t, int);
int netSelect(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int netGetSockName(int, struct sockaddr *, socklen_t *);
int netGetPeerName(int, struct sockaddr *, socklen_t *);
int netGetSockOpt(int, int, int, void *, socklen_t *);
int netSetSockOpt(int, int, int, const void *, socklen_t);
struct net_hostent *netGetHostByAddr(const char *, socklen_t, int);
struct net_hostent *netGetHostByName(const char *);
#ifdef __cplusplus
}
#endif
#endif
