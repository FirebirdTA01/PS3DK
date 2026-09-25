/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef __PS3DK_POLL_H__
#define __PS3DK_POLL_H__
typedef unsigned int nfds_t;
struct pollfd { int fd; short events; short revents; };
#define POLLIN 0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020
#define POLLRDNORM 0x0040
#define POLLWRNORM POLLOUT
#define POLLRDBAND 0x0080
#define POLLWRBAND 0x0100
#ifdef __cplusplus
extern "C" {
#endif
int poll(struct pollfd *, nfds_t, int);
int socketpoll(struct pollfd *, nfds_t, int);
#ifdef __cplusplus
}
#endif
#endif
