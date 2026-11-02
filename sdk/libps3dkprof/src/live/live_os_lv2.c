/* Live link OS seam over the SDK's librt (LV2 builds). See live_os.h.
 *
 * Differences from the host shim, all from librt itself:
 * - there is no fcntl: a socket is made non-blocking with SO_NBIO;
 * - there is no MSG_NOSIGNAL: sends pass flags 0. Whether a send to a peer
 *   that has gone can raise a signal on LV2 is UNVERIFIED until the target
 *   smoke test; EPIPE is CLOSED either way;
 * - net errors arrive as POSIX errno values (librt's net_errno.h);
 * - poll handles socket descriptors only and allocates a wire array per
 *   call (measured in the overhead gate, not worked around here);
 * - close routes socket descriptors (SOCKET_FD_MASK) to socketclose. */
#if !defined(PS3DK_PROF_PAL_LV2)
#error "live_os_lv2.c: the LV2 shim; host builds use live_os_host.c"
#endif
#include "live_os.h"
#include "live_path.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int again(int e)
{
    return e == EAGAIN || e == EWOULDBLOCK;
}

/* A peer that is gone, as a socket call reports it. */
static int gone(int e)
{
    return e == EPIPE || e == ECONNRESET || e == ENOTCONN || e == ECONNABORTED;
}

int live_os_set_nonblock(int fd)
{
    int one = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NBIO, &one, sizeof one) < 0 ? LIVE_OS_ERROR : 0;
}

int live_os_listen(uint16_t port, int loopback_only, int *out_fd, uint16_t *bound_port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return LIVE_OS_ERROR;
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
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
        int fd = accept(listen_fd, NULL, NULL);
        if (fd >= 0)
            return fd;
        if (errno != EINTR)
            return again(errno) || errno == ECONNABORTED ? LIVE_OS_AGAIN : LIVE_OS_ERROR;
    }
    return LIVE_OS_AGAIN;
}

int32_t live_os_send(int fd, const void *buf, uint32_t n)
{
    if (n > LIVE_OS_MAX_IO)
        return LIVE_OS_ERROR;
    for (int i = 0; i <= LIVE_OS_INTR_RETRIES; ++i) {
        ssize_t r = send(fd, buf, n, 0);
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
        ssize_t r = recv(fd, buf, n, 0);
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
        int fd = open(path, O_RDONLY);
        if (fd >= 0)
            return fd;
        if (errno != EINTR)
            return LIVE_OS_ERROR;
    }
    return LIVE_OS_ERROR;
}

#ifdef PS3DK_PROF_LIVE_STACK_PAINT
#include <sys/ppu_thread.h>

#define STACK_PAINT 0x5a5aa5a5c3c33c3cull
#define STACK_GAP 1024u

static struct {
    int32_t status;
    uintptr_t base, top, painted_top;
} g_stack = {.status = LIVE_OS_STACK_OFF};
static live_os_stack_report g_report = {.status = LIVE_OS_STACK_OFF};

/* A leaf: it calls nothing after reading the bounds, so its own frame is all the stack it uses below SP. */
__attribute__((noinline)) void live_os_thread_enter(void)
{
    sys_ppu_thread_stack_t info;
    if (sys_ppu_thread_get_stack_information(&info) != 0) {
        g_report.status = g_stack.status = LIVE_OS_STACK_NO_INFO;
        return;
    }
    uintptr_t base = (uintptr_t)info.pst_addr, top = base + (uintptr_t)info.pst_size;
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    if (sp <= base || sp > top || sp - base <= STACK_GAP + 64u) {
        g_report.status = g_stack.status = LIVE_OS_STACK_SP_OUTSIDE;
        return;
    }
    uintptr_t lo = (base + 7u) & ~(uintptr_t)7u, hi = (sp - STACK_GAP) & ~(uintptr_t)7u;
    for (volatile uint64_t *w = (volatile uint64_t *)lo; (uintptr_t)w < hi; ++w)
        *w = STACK_PAINT;
    g_stack.base = lo;
    g_stack.top = top;
    g_stack.painted_top = hi;
    g_report = (live_os_stack_report){.status = LIVE_OS_STACK_RUNNING, .size = (uint32_t)info.pst_size,
                                      .painted = (uint32_t)(hi - lo)};
    g_stack.status = LIVE_OS_STACK_RUNNING;
}

void live_os_thread_exit(void)
{
    if (g_stack.status != LIVE_OS_STACK_RUNNING)
        return;
    const volatile uint64_t *w = (const volatile uint64_t *)g_stack.base;
    while ((uintptr_t)w < g_stack.painted_top && *w == STACK_PAINT)
        ++w;
    g_report.used = (uint32_t)(g_stack.top - (uintptr_t)w);
    g_report.status = g_stack.status = LIVE_OS_STACK_DONE;
}

void live_os_stack_read(live_os_stack_report *out)
{
    *out = g_report;
}
#else
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
#endif

/* The internal HDD only, with no "." or ".." component (live_path.h): an unmeasured device is never admitted as
 * a measured one, whatever directory string the sink was given. */
int live_os_capture_supported(const char *path)
{
    return live_path_under(path, "/dev_hdd0/");
}

int32_t live_os_file_read_at(int fd, void *buf, uint32_t n, uint64_t off)
{
    /* off_t may be 32 bits here (newlib, ILP32): an offset it cannot hold is refused, never truncated. */
    const uint64_t off_max = sizeof(off_t) >= 8 ? (uint64_t)INT64_MAX : (uint64_t)INT32_MAX;
    if (n > LIVE_OS_MAX_IO || off > off_max)
        return LIVE_OS_ERROR;
    if (lseek(fd, (off_t)off, SEEK_SET) == (off_t)-1)
        return LIVE_OS_ERROR;
    for (int i = 0; i <= LIVE_OS_INTR_RETRIES; ++i) {
        ssize_t r = read(fd, buf, n);
        if (r >= 0)
            return (int32_t)r;
        if (errno != EINTR)
            return LIVE_OS_ERROR;
    }
    return LIVE_OS_ERROR;
}
