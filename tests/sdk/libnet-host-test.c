/* Actual production wrappers; host double intercepts the PPC syscall only. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include "../../runtime/lv2/librt/socket.c"

/* Missing production implementations must fail explicitly, never bind libc. */
extern int getsockopt(int, int, int, void *, socklen_t *) __attribute__((weak));
extern int setsockopt(int, int, int, const void *, socklen_t) __attribute__((weak));
extern int poll(struct pollfd *, nfds_t, int) __attribute__((weak));
extern ssize_t sendmsg(int, const struct msghdr *, int) __attribute__((weak));
extern ssize_t recvmsg(int, struct msghdr *, int) __attribute__((weak));

extern ssize_t __librt_recv_r(struct _reent *, int, void *, size_t) __attribute__((weak));
extern ssize_t __librt_send_r(struct _reent *, int, const void *, size_t) __attribute__((weak));
static int failures, result, call_number, seen_nfds;
static uint32_t seen_bits;
static int64_t seen_usec;
static int seen_fd, option_kind, message_mode;
static uint32_t seen_length;
static char payload[8];
static int fd_argument_calls;
static int tagged_three(void) { ++fd_argument_calls; return SOCKET_FD_MASK | 3; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s (errno=%d)\n", __LINE__, #x, errno); ++failures; } } while (0)

int test_syscall(int nr, const uint64_t *a, unsigned count)
{
    (void)count;
    call_number = nr;
    seen_fd = a[0];
    if (nr == 700 || nr == 707) {
        unsigned addr = nr == 700 ? 1 : 4;
        CHECK(a[addr] == 0 && a[addr + 1] == 0);
    }
    if (nr == 705 || nr == 711) {
        seen_length = nr == 705 ? *(uint32_t *)(uintptr_t)a[4] : a[4];
        if (option_kind == 1) {
            int64_t *tv = (int64_t *)(uintptr_t)a[3];
            if (nr == 711) CHECK(tv[0] == 2 && tv[1] == 500 && seen_length == 16);
            else { tv[0] = 3; tv[1] = 123; }
        } else if (option_kind == 2) {
            CHECK(nr == 705 && a[1] == SOL_SOCKET && a[2] == SO_TYPE && seen_length == 4);
            *(int *)(uintptr_t)a[3] = SOCK_DGRAM;
        } else if (nr == 705) *(int *)(uintptr_t)a[3] = 61;
    }
    if (nr == 715) {
        struct pollfd *fds = (struct pollfd *)(uintptr_t)a[0];
        CHECK(a[1] == 2 && (int)a[2] == 17);
        CHECK(fds[0].fd == 3 && fds[1].fd == -1);
        fds[0].revents = POLLIN; fds[1].revents = POLLNVAL;
    }
    if (nr == 708 || nr == 709) {
        uint32_t *m = (uint32_t *)(uintptr_t)a[1];
        CHECK(m[0] == 0 && m[3] == 0 && m[4] == 0 && m[7] == 0 && m[8] == 0);
        CHECK(m[1] == 0 && m[2] == 0 && m[6] == 1 && m[9] == 0 && m[10] == 0);
        uint32_t *v = (uint32_t *)(uintptr_t)m[5];
        CHECK(v[0] == 0 && v[1] == (uintptr_t)payload && v[2] == 0 && v[3] == 8);
        if (message_mode) { m[2] = 16; m[10] = 0; m[11] = MSG_TRUNC; }
    }
    if (nr == 716) {
        seen_nfds = a[0];
        seen_bits = a[1] ? *(uint32_t *)(uintptr_t)a[1] : 0;
        if (a[4]) seen_usec = ((int64_t *)(uintptr_t)a[4])[1];
        if (a[1]) *(uint32_t *)(uintptr_t)a[1] = 8;
    }
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "errno")) {
        const int wire[] = {9, 35, 36, 45, 48, 54, 60, 61, 68};
        const int native[] = {EBADF, EAGAIN, EINPROGRESS, EOPNOTSUPP, EADDRINUSE, ECONNRESET, ETIMEDOUT, ECONNREFUSED, ENOSPC};
        for (unsigned i = 0; i < sizeof(wire)/sizeof(*wire); ++i) {
            result = -wire[i]; errno = 0;
            CHECK(connect(SOCKET_FD_MASK | 3, NULL, 0) == -1);
            CHECK(errno == native[i]);
        }
        result = (int)0x8001000d;
        CHECK(connect(SOCKET_FD_MASK | 3, NULL, 0) == -1 && errno == EFAULT);
        result = -61;
        struct _reent r = {0};
        errno = 73;
        CHECK(__librt_socketclose_r(&r, SOCKET_FD_MASK | 3) == -1);
        CHECK(r._errno == ECONNREFUSED && errno == 73);
        result = 5;
        CHECK(socket(AF_INET, SOCK_STREAM, 0) == (SOCKET_FD_MASK | 5));
        call_number = 0;
        CHECK(connect(3, NULL, 0) == -1 && errno == EBADF && call_number == 0);
        CHECK(__librt_recv_r && __librt_send_r);
        if (!__librt_recv_r || !__librt_send_r) return 1;
        result = -35; errno = 73; r._errno = 0;
        CHECK(__librt_recv_r(&r, SOCKET_FD_MASK | 3, payload, 8) == -1 && r._errno == EAGAIN && errno == 73);
        result = -54;
        CHECK(__librt_send_r(&r, SOCKET_FD_MASK | 3, payload, 8) == -1 && r._errno == ECONNRESET && errno == 73);
    } else if (!strcmp(argv[1], "select")) {
        fd_set fds;
        struct timeval tv = {0, 500};
        FD_ZERO(&fds); FD_SET(3, &fds); FD_SET(7, &fds);
        result = 1;
        CHECK(select(SOCKET_FD_MASK | 4, &fds, NULL, NULL, &tv) == 1);
        CHECK(call_number == 716 && seen_nfds == 4 && seen_bits == 8 && seen_usec == 500);
        CHECK(FD_ISSET(3, &fds));
        call_number = 0;
        CHECK(select(1025, NULL, NULL, NULL, NULL) == -1 && errno == EINVAL);
        CHECK(call_number == 0);
        tv.tv_sec = -1;
        CHECK(select(0, NULL, NULL, NULL, &tv) == -1 && errno == EINVAL && call_number == 0);
    } else if (!strcmp(argv[1], "fdset")) {
        fd_set fds;
        FD_ZERO(&fds); FD_SET(tagged_three(), &fds);
        CHECK(fd_argument_calls == 1);
        CHECK(FD_ISSET(SOCKET_FD_MASK | 3, &fds));
        CHECK(FD_ISSET(3, &fds));
        FD_CLR(SOCKET_FD_MASK | 3, &fds);
        CHECK(!FD_ISSET(3, &fds));
        CHECK(sizeof(fd_set) == 128);
    } else if (!strcmp(argv[1], "options")) {
        CHECK(getsockopt && setsockopt);
        if (!getsockopt || !setsockopt) return 1;
        struct timeval tv = {2, 500};
        socklen_t len = sizeof(tv);
        result = 0; option_kind = 1;
        CHECK(setsockopt(SOCKET_FD_MASK | 3, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
        CHECK(seen_fd == 3 && call_number == 711);
        CHECK(getsockopt(SOCKET_FD_MASK | 3, SOL_SOCKET, SO_SNDTIMEO, &tv, &len) == 0);
        CHECK(tv.tv_sec == 3 && tv.tv_usec == 123 && len == sizeof(tv));
        int type = -1; len = sizeof(type); option_kind = 2;
        CHECK(getsockopt(SOCKET_FD_MASK | 3, SOL_SOCKET, SO_TYPE, &type, &len) == 0);
        CHECK(type == SOCK_DGRAM && len == sizeof(type));
        int e = 0; len = sizeof(e); option_kind = 0;
        CHECK(getsockopt(SOCKET_FD_MASK | 3, SOL_SOCKET, SO_ERROR, &e, &len) == 0 && e == ECONNREFUSED);
        result = -9; e = 99;
        CHECK(getsockopt(SOCKET_FD_MASK | 3, SOL_SOCKET, SO_ERROR, &e, &len) == -1 && e == 99 && errno == EBADF);
    } else if (!strcmp(argv[1], "poll")) {
        CHECK(poll); if (!poll) return 1;
        struct pollfd fds[2] = {{SOCKET_FD_MASK | 3, POLLIN, 99}, {-1, POLLIN, 99}};
        result = 1;
        CHECK(poll(fds, 2, 17) == 1);
        CHECK(fds[0].fd == (SOCKET_FD_MASK | 3) && fds[0].revents == POLLIN && fds[1].revents == 0);
    } else if (!strcmp(argv[1], "message")) {
        CHECK(sendmsg && recvmsg); if (!sendmsg || !recvmsg) return 1;
        struct iovec v = {payload, sizeof(payload)};
        struct msghdr m = {0}; m.msg_iov = &v; m.msg_iovlen = 1;
        result = 8;
        CHECK(sendmsg(SOCKET_FD_MASK | 3, &m, 0) == 8 && call_number == 709 && seen_fd == 3);
        message_mode = 1;
        CHECK(recvmsg(SOCKET_FD_MASK | 3, &m, 0) == 8 && call_number == 708);
        CHECK(m.msg_namelen == 16 && m.msg_flags == MSG_TRUNC);
        m.msg_flags = 0; m.msg_namelen = 0; result = -35;
        CHECK(recvmsg(SOCKET_FD_MASK | 3, &m, 0) == -1 && errno == EAGAIN && m.msg_flags == 0);
    } else if (!strcmp(argv[1], "nullable")) {
        result = 3; socklen_t length = 123;
        CHECK(accept(SOCKET_FD_MASK | 4, NULL, &length) == (SOCKET_FD_MASK | 3) && length == 123);
        CHECK(recvfrom(SOCKET_FD_MASK | 4, payload, sizeof(payload), 0, NULL, &length) == 3 && length == 123);
        struct sockaddr addr = {0};
        call_number = 0;
        CHECK(accept(SOCKET_FD_MASK | 4, &addr, NULL) == -1 && errno == EFAULT && call_number == 0);
        CHECK(recvfrom(SOCKET_FD_MASK | 4, payload, 8, 0, &addr, NULL) == -1 && errno == EFAULT && call_number == 0);
        CHECK(send(SOCKET_FD_MASK | 4, payload, (size_t)INT_MAX + 1, 0) == -1 && errno == EMSGSIZE && call_number == 0);
        CHECK(recv(SOCKET_FD_MASK | 4, payload, (size_t)INT_MAX + 1, 0) == -1 && errno == EMSGSIZE && call_number == 0);
        CHECK(sendto(SOCKET_FD_MASK | 4, payload, (size_t)INT_MAX + 1, 0, NULL, 0) == -1 && errno == EMSGSIZE && call_number == 0);
        CHECK(recvfrom(SOCKET_FD_MASK | 4, payload, (size_t)INT_MAX + 1, 0, NULL, NULL) == -1 && errno == EMSGSIZE && call_number == 0);
    } else return 2;
    return failures ? 1 : 0;
}
