/* Live link OS seam: every socket, poll and file call the agent makes goes
 * through these functions, so the protocol and state machine in
 * live_agent.c stay platform-free. live_os_host.c implements them over
 * POSIX; the LV2 shim is part of the target checkpoint.
 *
 * Results are normalized. A call that succeeds returns >= 0; a failure is
 * one of the codes below, never an errno.
 *
 * Interrupted calls (EINTR):
 * - accept, send, recv, file_open and the read in file_read_at are retried
 *   inside the shim, at most LIVE_OS_INTR_RETRIES times in a row (one call
 *   plus 16 retries). After that a socket call reports LIVE_OS_AGAIN (try at
 *   the next poll) and a file call LIVE_OS_ERROR.
 * - poll reports an interruption as 0, like a timeout; the caller polls
 *   again.
 * - listen (socket, bind, listen, the non-blocking switch, the port query),
 *   set_nonblock and the seek in file_read_at are not retried: they fail at
 *   once with LIVE_OS_ERROR.
 * - close is never retried: the descriptor is released either way.
 * None of this bounds the time a file call takes.
 *
 * Descriptors: whoever receives one from listen, accept or file_open owns
 * it and closes it exactly once with live_os_close. The agent thread is the
 * only user of a file descriptor (read_at seeks it, so nothing else may
 * share its offset). */
#ifndef PS3DK_PROF_LIVE_OS_H
#define PS3DK_PROF_LIVE_OS_H
#include <stdint.h>

#define LIVE_OS_AGAIN (-1)  /* nothing can be done now: wait for the next poll */
#define LIVE_OS_CLOSED (-2) /* the peer is gone: orderly close, reset or broken pipe */
#define LIVE_OS_ERROR (-3)  /* any other failure */

#define LIVE_OS_INTR_RETRIES 16
#define LIVE_OS_MAX_IO (1u << 20) /* the largest n a send, recv or read_at accepts */

#define LIVE_OS_IN 1u   /* readable, or a pending connection on a listener */
#define LIVE_OS_OUT 2u  /* writable */
#define LIVE_OS_HUP 4u  /* reported, never requested: the peer hung up */
#define LIVE_OS_ERR 8u  /* reported, never requested: an error on the descriptor, or not an open one */

typedef struct live_os_pollfd {
    int fd;           /* a negative fd is skipped: its revents stays 0 */
    uint32_t events;  /* LIVE_OS_IN | LIVE_OS_OUT */
    uint32_t revents; /* set by live_os_poll: the requested events that are ready, plus HUP and ERR */
} live_os_pollfd;

/* A non-blocking TCP listener on port (0: an ephemeral one), on 127.0.0.1
 * when loopback_only, else on every interface. 0 with *fd and *bound_port
 * set, or LIVE_OS_ERROR with nothing left open. */
int live_os_listen(uint16_t port, int loopback_only, int *fd, uint16_t *bound_port);

/* One pending connection, never waiting: its descriptor, LIVE_OS_AGAIN when
 * none is pending, or LIVE_OS_ERROR. The new descriptor is NOT yet
 * non-blocking; the caller decides (live_os_set_nonblock). */
int live_os_accept(int listen_fd);

/* 0, or LIVE_OS_ERROR. */
int live_os_set_nonblock(int fd);

/* On a non-blocking socket. Bytes accepted (possibly fewer than n),
 * LIVE_OS_AGAIN, LIVE_OS_CLOSED or LIVE_OS_ERROR. Never raises a signal. */
int32_t live_os_send(int fd, const void *buf, uint32_t n);

/* On a non-blocking socket. Bytes received (> 0), LIVE_OS_CLOSED (the peer
 * closed or reset the connection), LIVE_OS_AGAIN or LIVE_OS_ERROR. */
int32_t live_os_recv(int fd, void *buf, uint32_t n);

/* Waits up to timeout_ms for the requested events. Returns the number of
 * entries whose revents is nonzero (HUP and ERR count, as they wake the
 * wait), 0 on timeout or interruption (every revents 0), or LIVE_OS_ERROR.
 * The agent reads only IN on its listener; for the viewer socket it ignores
 * revents and always tries recv and send, so a hung-up or failed descriptor
 * surfaces there (CLOSED or ERROR) and is dropped as before. */
int live_os_poll(live_os_pollfd *fds, uint32_t n, uint32_t timeout_ms);

/* Closes a descriptor; a negative fd is ignored. */
void live_os_close(int fd);

/* Opens a file read-only: its descriptor, or LIVE_OS_ERROR. */
int live_os_file_open(const char *path);

/* Reads up to n bytes at offset off (a seek, then a read): bytes read (0 at
 * end of file) or LIVE_OS_ERROR. */
int32_t live_os_file_read_at(int fd, void *buf, uint32_t n, uint64_t off);

#ifdef PS3DK_PROF_TEST_HOOKS
/* Seam-test fault injection: a non-NULL entry replaces that system call in
 * the host shim and returns what the real call would (a result, or -1 with
 * errno set). Set only while no agent thread runs. */
typedef struct live_os_test_hooks {
    long (*send)(int fd, const void *buf, unsigned long n);
    long (*recv)(int fd, void *buf, unsigned long n);
    long (*read)(int fd, void *buf, unsigned long n);
    int (*accept)(int listen_fd);
} live_os_test_hooks;
extern live_os_test_hooks ps3dk_prof_test_live_os_hooks;
#endif

#endif
