/* Live link OS seam over POSIX (host builds). See live_os.h. */
#if !defined(PS3DK_PROF_PAL_HOST)
#error "live_os_host.c: the host shim; the LV2 shim is part of the target checkpoint"
#endif
#define _GNU_SOURCE
#include "live_os.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef PS3DK_PROF_TEST_HOOKS
live_os_test_hooks ps3dk_prof_test_live_os_hooks;
#define CALL(name, real, ...) \
    (ps3dk_prof_test_live_os_hooks.name ? ps3dk_prof_test_live_os_hooks.name(__VA_ARGS__) : (real))
#else
#define CALL(name, real, ...) (real)
#endif

static int again(int e)
{
    return e == EAGAIN || e == EWOULDBLOCK;
}

/* A peer that is gone, as a socket call reports it. */
static int gone(int e)
{
    return e == EPIPE || e == ECONNRESET || e == ENOTCONN || e == ECONNABORTED;
}

int live_os_listen(uint16_t port, int loopback_only, int *out_fd, uint16_t *bound_port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return LIVE_OS_ERROR;
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY)};
    socklen_t sl = sizeof sa;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 2) < 0 || live_os_set_nonblock(fd) < 0 ||
        getsockname(fd, (struct sockaddr *)&sa, &sl) < 0) {
        close(fd);
        return LIVE_OS_ERROR;
    }
    *out_fd = fd;
    *bound_port = ntohs(sa.sin_port);
    return 0;
}

int live_os_accept(int listen_fd)
{
    for (int i = 0; i <= LIVE_OS_INTR_RETRIES; ++i) {
        int fd = CALL(accept, accept(listen_fd, NULL, NULL), listen_fd);
        if (fd >= 0)
            return fd;
        if (errno != EINTR)
            return again(errno) || errno == ECONNABORTED ? LIVE_OS_AGAIN : LIVE_OS_ERROR;
    }
    return LIVE_OS_AGAIN;
}

int live_os_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 ? LIVE_OS_ERROR : 0;
}

int32_t live_os_send(int fd, const void *buf, uint32_t n)
{
    if (n > LIVE_OS_MAX_IO)
        return LIVE_OS_ERROR;
    for (int i = 0; i <= LIVE_OS_INTR_RETRIES; ++i) {
        /* MSG_NOSIGNAL: a send to a peer that has gone is EPIPE, not SIGPIPE. */
        long r = CALL(send, send(fd, buf, n, MSG_NOSIGNAL), fd, buf, n);
        if (r >= 0)
            return (int32_t)r;
        if (errno == EINTR)
            continue;
        return again(errno) ? LIVE_OS_AGAIN : gone(errno) ? LIVE_OS_CLOSED : LIVE_OS_ERROR;
    }
    return LIVE_OS_AGAIN;
}

int32_t live_os_recv(int fd, void *buf, uint32_t n)
{
    if (n > LIVE_OS_MAX_IO)
        return LIVE_OS_ERROR;
    for (int i = 0; i <= LIVE_OS_INTR_RETRIES; ++i) {
        long r = CALL(recv, recv(fd, buf, n, 0), fd, buf, n);
        if (r > 0)
            return (int32_t)r;
        if (r == 0)
            return n ? LIVE_OS_CLOSED : 0;
        if (errno == EINTR)
            continue;
        return again(errno) ? LIVE_OS_AGAIN : gone(errno) ? LIVE_OS_CLOSED : LIVE_OS_ERROR;
    }
    return LIVE_OS_AGAIN;
}

int live_os_poll(live_os_pollfd *fds, uint32_t n, uint32_t timeout_ms)
{
    struct pollfd pf[4];
    if (n > sizeof pf / sizeof pf[0] || timeout_ms > 60000)
        return LIVE_OS_ERROR;
    for (uint32_t i = 0; i < n; ++i) {
        pf[i].fd = fds[i].fd;
        pf[i].events = (short)((fds[i].events & LIVE_OS_IN ? POLLIN : 0) | (fds[i].events & LIVE_OS_OUT ? POLLOUT : 0));
        pf[i].revents = 0;
    }
    int r = poll(pf, n, (int)timeout_ms);
    int count = 0;
    for (uint32_t i = 0; i < n; ++i) {
        short e = r > 0 ? pf[i].revents : 0;
        fds[i].revents = (e & POLLIN ? LIVE_OS_IN : 0u) | (e & POLLOUT ? LIVE_OS_OUT : 0u) |
                         (e & POLLHUP ? LIVE_OS_HUP : 0u) | (e & (POLLERR | POLLNVAL) ? LIVE_OS_ERR : 0u);
        count += fds[i].revents != 0;
    }
    if (r < 0)
        return errno == EINTR ? 0 : LIVE_OS_ERROR;
    return count;
}

void live_os_close(int fd)
{
    if (fd >= 0)
        (void)close(fd); /* never retried: the descriptor is released even when close reports EINTR */
}

int live_os_file_open(const char *path)
{
    for (int i = 0; i <= LIVE_OS_INTR_RETRIES; ++i) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0)
            return fd;
        if (errno != EINTR)
            return LIVE_OS_ERROR;
    }
    return LIVE_OS_ERROR;
}

void live_os_thread_enter(void)
{
}

void live_os_thread_exit(void)
{
}

void live_os_stack_read(live_os_stack_report *out)
{
    *out = (live_os_stack_report){.status = LIVE_OS_STACK_OFF};
}

int live_os_capture_supported(const char *path)
{
    return path && path[0] != 0;
}

int32_t live_os_file_read_at(int fd, void *buf, uint32_t n, uint64_t off)
{
    if (n > LIVE_OS_MAX_IO || off > (uint64_t)INT64_MAX)
        return LIVE_OS_ERROR;
    if (lseek(fd, (off_t)off, SEEK_SET) == (off_t)-1)
        return LIVE_OS_ERROR;
    for (int i = 0; i <= LIVE_OS_INTR_RETRIES; ++i) {
        long r = CALL(read, read(fd, buf, n), fd, buf, n);
        if (r >= 0)
            return (int32_t)r;
        if (errno != EINTR)
            return LIVE_OS_ERROR;
    }
    return LIVE_OS_ERROR;
}
