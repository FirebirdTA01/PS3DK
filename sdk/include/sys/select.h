/* SPDX-License-Identifier: BSD-2-Clause
 * SDK sockets carry SOCKET_FD_MASK. The kernel bitset carries raw indices.
 * This fixed 1024-bit layout is shared by both PPU ABIs. Only sockets are
 * selectable; regular filesystem descriptors are not supported by Lv-2.
 */
#ifndef __PS3DK_SYS_SELECT_H__
#define __PS3DK_SYS_SELECT_H__
#define _SYS_SELECT_H
#include <stdint.h>
#include <sys/_timeval.h>
#include <sys/timespec.h>

#define FD_SETSIZE 1024
typedef uint32_t fd_mask;
#define NFDBITS 32
#define NFDBITS_SHIFT 5
#define NFDBITS_MASK 31
typedef struct fd_set { fd_mask fds_bits[32]; } fd_set;

static inline unsigned __ps3dk_fd_index(int fd)
{ return (unsigned)fd & ~0x40000000u; }
static inline void __ps3dk_fd_set(int fd, fd_set *set)
{
    unsigned n = __ps3dk_fd_index(fd);
    if (n < FD_SETSIZE) set->fds_bits[n >> 5] |= UINT32_C(1) << (n & 31);
}
static inline void __ps3dk_fd_clr(int fd, fd_set *set)
{
    unsigned n = __ps3dk_fd_index(fd);
    if (n < FD_SETSIZE) set->fds_bits[n >> 5] &= ~(UINT32_C(1) << (n & 31));
}
static inline int __ps3dk_fd_isset(int fd, const fd_set *set)
{
    unsigned n = __ps3dk_fd_index(fd);
    return n < FD_SETSIZE && (set->fds_bits[n >> 5] & (UINT32_C(1) << (n & 31))) != 0;
}
static inline void __ps3dk_fd_zero(fd_set *set)
{ unsigned i; for (i = 0; i < 32; ++i) set->fds_bits[i] = 0; }
#define FD_SET(fd, set) __ps3dk_fd_set((fd), (set))
#define FD_CLR(fd, set) __ps3dk_fd_clr((fd), (set))
#define FD_ISSET(fd, set) __ps3dk_fd_isset((fd), (set))
#define FD_ZERO(set) __ps3dk_fd_zero((set))

#ifdef __cplusplus
extern "C" {
#endif
/* nfds accepts either raw maximum+1 or tagged maximum+1. */
int socketselect(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
#ifdef __cplusplus
}
#endif
#endif
