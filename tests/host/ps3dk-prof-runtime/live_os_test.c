/* The live link OS seam's host shim (sdk/libps3dkprof/src/live/live_os_host.c):
 * every errno a socket or file call can report is normalized to AGAIN,
 * CLOSED or ERROR; EINTR is retried at most LIVE_OS_INTR_RETRIES times in a
 * row; a short send is passed through; and a send to a peer that has gone
 * reports CLOSED instead of raising SIGPIPE. Scripted system-call results
 * come through the shim's test hooks; the real-socket and real-file cases
 * use no hooks. */
#define _GNU_SOURCE
#include "live_os.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(c)                                                                   \
    do {                                                                           \
        if (!(c)) {                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
            exit(1);                                                               \
        }                                                                          \
    } while (0)

/* ---- scripted system calls --------------------------------------------------------------------------------------- */

#define REAL (-100) /* a script step that runs the real call */

struct step {
    long ret; /* >= 0: the result; -1: fail with err; REAL: the real call */
    int err;
};

static struct step script[64];
static int nsteps, calls;

static void set_script(const struct step *s, int n)
{
    CHECK(n <= (int)(sizeof script / sizeof script[0]));
    if (n)
        memcpy(script, s, sizeof *s * (size_t)n);
    nsteps = n;
    calls = 0;
}

/* n steps of EINTR, then `last`. */
static void eintr_then(int n, struct step last)
{
    struct step s[64];
    CHECK(n + 1 <= 64);
    for (int i = 0; i < n; ++i)
        s[i] = (struct step){-1, EINTR};
    s[n] = last;
    set_script(s, n + 1);
}

static long next(long real_ret)
{
    CHECK(calls < nsteps); /* the shim made more calls than the script allows */
    struct step s = script[calls++];
    if (s.ret == REAL)
        return real_ret;
    if (s.ret < 0)
        errno = s.err;
    return s.ret;
}

static long fake_send(int fd, const void *buf, unsigned long n)
{
    (void)fd, (void)buf, (void)n;
    return next(0);
}

static long fake_recv(int fd, void *buf, unsigned long n)
{
    (void)fd, (void)buf, (void)n;
    return next(0);
}

static long fake_read(int fd, void *buf, unsigned long n)
{
    struct step s = script[calls < nsteps ? calls : 0];
    long r = s.ret == REAL ? read(fd, buf, n) : 0;
    return next(r);
}

static int fake_accept(int listen_fd)
{
    (void)listen_fd;
    return (int)next(0);
}

static void hooks_on(void)
{
    ps3dk_prof_test_live_os_hooks = (live_os_test_hooks){fake_send, fake_recv, fake_read, fake_accept};
}

static void hooks_off(void)
{
    memset(&ps3dk_prof_test_live_os_hooks, 0, sizeof ps3dk_prof_test_live_os_hooks);
}

static char buf[64];

/* ---- send -------------------------------------------------------------------------------------------------------- */

static void send_normalized(void)
{
    hooks_on();
    set_script((struct step[]){{-1, EAGAIN}}, 1);
    CHECK(live_os_send(3, buf, 10) == LIVE_OS_AGAIN && calls == 1);
    set_script((struct step[]){{-1, EPIPE}}, 1);
    CHECK(live_os_send(3, buf, 10) == LIVE_OS_CLOSED && calls == 1);
    set_script((struct step[]){{-1, ECONNRESET}}, 1);
    CHECK(live_os_send(3, buf, 10) == LIVE_OS_CLOSED && calls == 1);
    set_script((struct step[]){{-1, ENOTCONN}}, 1);
    CHECK(live_os_send(3, buf, 10) == LIVE_OS_CLOSED && calls == 1);
    set_script((struct step[]){{-1, EBADF}}, 1);
    CHECK(live_os_send(3, buf, 10) == LIVE_OS_ERROR && calls == 1);
    set_script((struct step[]){{-1, ENOBUFS}}, 1);
    CHECK(live_os_send(3, buf, 10) == LIVE_OS_ERROR && calls == 1);
    /* A short send is passed through; the caller sends the rest later. */
    set_script((struct step[]){{3, 0}}, 1);
    CHECK(live_os_send(3, buf, 10) == 3 && calls == 1);
    set_script((struct step[]){{0, 0}}, 1);
    CHECK(live_os_send(3, buf, 10) == 0 && calls == 1);
    /* Oversized requests never reach the system. */
    set_script(NULL, 0);
    CHECK(live_os_send(3, buf, LIVE_OS_MAX_IO + 1) == LIVE_OS_ERROR && calls == 0);
    hooks_off();
}

static void send_eintr_bounded(void)
{
    hooks_on();
    eintr_then(3, (struct step){5, 0});
    CHECK(live_os_send(3, buf, 10) == 5 && calls == 4);
    /* The last retry allowed still succeeds... */
    eintr_then(LIVE_OS_INTR_RETRIES, (struct step){7, 0});
    CHECK(live_os_send(3, buf, 10) == 7 && calls == LIVE_OS_INTR_RETRIES + 1);
    /* ...one more interruption and the call reports AGAIN, never looping on. */
    eintr_then(LIVE_OS_INTR_RETRIES + 1, (struct step){7, 0});
    CHECK(live_os_send(3, buf, 10) == LIVE_OS_AGAIN && calls == LIVE_OS_INTR_RETRIES + 1);
    eintr_then(2, (struct step){-1, EPIPE});
    CHECK(live_os_send(3, buf, 10) == LIVE_OS_CLOSED && calls == 3);
    hooks_off();
}

/* ---- recv -------------------------------------------------------------------------------------------------------- */

static void recv_normalized(void)
{
    hooks_on();
    set_script((struct step[]){{0, 0}}, 1);
    CHECK(live_os_recv(3, buf, 8) == LIVE_OS_CLOSED && calls == 1); /* orderly close */
    set_script((struct step[]){{-1, EAGAIN}}, 1);
    CHECK(live_os_recv(3, buf, 8) == LIVE_OS_AGAIN && calls == 1);
    set_script((struct step[]){{-1, ECONNRESET}}, 1);
    CHECK(live_os_recv(3, buf, 8) == LIVE_OS_CLOSED && calls == 1);
    set_script((struct step[]){{-1, EIO}}, 1);
    CHECK(live_os_recv(3, buf, 8) == LIVE_OS_ERROR && calls == 1);
    set_script((struct step[]){{2, 0}}, 1);
    CHECK(live_os_recv(3, buf, 8) == 2 && calls == 1);
    eintr_then(4, (struct step){8, 0});
    CHECK(live_os_recv(3, buf, 8) == 8 && calls == 5);
    eintr_then(LIVE_OS_INTR_RETRIES + 1, (struct step){8, 0});
    CHECK(live_os_recv(3, buf, 8) == LIVE_OS_AGAIN && calls == LIVE_OS_INTR_RETRIES + 1);
    set_script(NULL, 0);
    CHECK(live_os_recv(3, buf, LIVE_OS_MAX_IO + 1) == LIVE_OS_ERROR && calls == 0);
    hooks_off();
}

/* ---- accept ------------------------------------------------------------------------------------------------------ */

static void accept_normalized(void)
{
    hooks_on();
    set_script((struct step[]){{-1, EAGAIN}}, 1);
    CHECK(live_os_accept(3) == LIVE_OS_AGAIN && calls == 1);
    set_script((struct step[]){{-1, ECONNABORTED}}, 1); /* a connection reset while queued: nothing to take */
    CHECK(live_os_accept(3) == LIVE_OS_AGAIN && calls == 1);
    set_script((struct step[]){{-1, EMFILE}}, 1);
    CHECK(live_os_accept(3) == LIVE_OS_ERROR && calls == 1);
    eintr_then(2, (struct step){9, 0});
    CHECK(live_os_accept(3) == 9 && calls == 3);
    eintr_then(LIVE_OS_INTR_RETRIES + 1, (struct step){9, 0});
    CHECK(live_os_accept(3) == LIVE_OS_AGAIN && calls == LIVE_OS_INTR_RETRIES + 1);
    hooks_off();
}

/* ---- file -------------------------------------------------------------------------------------------------------- */

static void file_read_at(const char *dir)
{
    char path[512];
    unsigned char data[100], got[100];
    snprintf(path, sizeof path, "%s/file.bin", dir);
    for (int i = 0; i < 100; ++i)
        data[i] = (unsigned char)(i * 7 + 1);
    FILE *f = fopen(path, "wb");
    CHECK(f && fwrite(data, 1, sizeof data, f) == sizeof data && fclose(f) == 0);

    int fd = live_os_file_open(path);
    CHECK(fd >= 0);
    /* Each read seeks first, in any order. */
    CHECK(live_os_file_read_at(fd, got, 10, 40) == 10 && memcmp(got, data + 40, 10) == 0);
    CHECK(live_os_file_read_at(fd, got, 10, 0) == 10 && memcmp(got, data, 10) == 0);
    CHECK(live_os_file_read_at(fd, got, 50, 90) == 10 && memcmp(got, data + 90, 10) == 0);
    CHECK(live_os_file_read_at(fd, got, 10, 100) == 0);  /* end of file */
    CHECK(live_os_file_read_at(fd, got, 10, 5000) == 0); /* past it */
    CHECK(live_os_file_read_at(fd, got, 10, (uint64_t)INT64_MAX + 1) == LIVE_OS_ERROR);
    CHECK(live_os_file_read_at(fd, got, LIVE_OS_MAX_IO + 1, 0) == LIVE_OS_ERROR);

    hooks_on();
    eintr_then(2, (struct step){REAL, 0});
    memset(got, 0, sizeof got);
    CHECK(live_os_file_read_at(fd, got, 10, 20) == 10 && calls == 3 && memcmp(got, data + 20, 10) == 0);
    eintr_then(LIVE_OS_INTR_RETRIES + 1, (struct step){REAL, 0});
    CHECK(live_os_file_read_at(fd, got, 10, 20) == LIVE_OS_ERROR && calls == LIVE_OS_INTR_RETRIES + 1);
    set_script((struct step[]){{-1, EIO}}, 1);
    CHECK(live_os_file_read_at(fd, got, 10, 20) == LIVE_OS_ERROR && calls == 1);
    hooks_off();
    live_os_close(fd);

    CHECK(live_os_file_read_at(fd, got, 10, 0) == LIVE_OS_ERROR); /* closed: the seek fails */
    snprintf(path, sizeof path, "%s/missing.bin", dir);
    CHECK(live_os_file_open(path) == LIVE_OS_ERROR);
    live_os_close(-1); /* ignored */
}

/* ---- real sockets ------------------------------------------------------------------------------------------------ */

static void real_sockets(void)
{
    int lfd;
    uint16_t port;
    CHECK(signal(SIGPIPE, SIG_DFL) != SIG_ERR); /* a stray SIGPIPE would kill this process */
    CHECK(live_os_listen(0, 1, &lfd, &port) == 0 && port != 0);
    CHECK(live_os_accept(lfd) == LIVE_OS_AGAIN); /* nothing pending, and the listener never blocks */

    live_os_pollfd pf[1] = {{.fd = lfd, .events = LIVE_OS_IN}};
    CHECK(live_os_poll(pf, 1, 0) == 0 && pf[0].revents == 0);

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(cfd >= 0 && connect(cfd, (struct sockaddr *)&sa, sizeof sa) == 0);
    CHECK(live_os_poll(pf, 1, 2000) == 1 && pf[0].revents == LIVE_OS_IN);
    int sfd = live_os_accept(lfd);
    CHECK(sfd >= 0 && live_os_set_nonblock(sfd) == 0);

    CHECK(live_os_recv(sfd, buf, 8) == LIVE_OS_AGAIN); /* nothing sent yet */
    CHECK(write(cfd, "abcd", 4) == 4);
    live_os_pollfd sp[1] = {{.fd = sfd, .events = LIVE_OS_IN | LIVE_OS_OUT}};
    CHECK(live_os_poll(sp, 1, 2000) >= 1 && (sp[0].revents & LIVE_OS_IN));
    CHECK(live_os_recv(sfd, buf, 8) == 4 && memcmp(buf, "abcd", 4) == 0);
    CHECK(live_os_send(sfd, "xy", 2) == 2);
    CHECK(read(cfd, buf, 2) == 2 && memcmp(buf, "xy", 2) == 0);

    /* The viewer goes away: an orderly close reads as CLOSED, and sends to it
     * end in CLOSED (EPIPE or ECONNRESET) with no signal raised. */
    CHECK(close(cfd) == 0);
    CHECK(live_os_poll(sp, 1, 2000) >= 1);
    CHECK(live_os_recv(sfd, buf, 8) == LIVE_OS_CLOSED);
    int32_t r = 0;
    for (int i = 0; i < 100 && r >= 0; ++i)
        r = live_os_send(sfd, buf, sizeof buf);
    CHECK(r == LIVE_OS_CLOSED);

    live_os_close(sfd);
    live_os_close(lfd);
    CHECK(live_os_listen(port, 1, &lfd, &port) == 0); /* SO_REUSEADDR: the port is free again at once */
    live_os_close(lfd);

    live_os_pollfd many[5] = {{0}};
    CHECK(live_os_poll(many, 5, 0) == LIVE_OS_ERROR); /* more entries than the shim takes */
    CHECK(live_os_poll(pf, 1, 60001) == LIVE_OS_ERROR);
}

/* revents carries HUP and ERR without being asked, and the count is exactly the entries with revents set. */
static void poll_status(void)
{
    int p[2];
    CHECK(pipe(p) == 0);
    live_os_pollfd pf[3] = {{.fd = p[0], .events = LIVE_OS_IN}, {.fd = -1, .events = LIVE_OS_IN},
                            {.fd = p[1], .events = LIVE_OS_OUT}};
    CHECK(live_os_poll(pf, 3, 0) == 1);                         /* only the write end is ready */
    CHECK(pf[0].revents == 0 && pf[1].revents == 0 && pf[2].revents == LIVE_OS_OUT);
    CHECK(close(p[1]) == 0);                                     /* the writer hangs up */
    CHECK(live_os_poll(pf, 2, 2000) == 1);                      /* a negative fd is skipped */
    CHECK((pf[0].revents & LIVE_OS_HUP) && pf[1].revents == 0);
    CHECK(read(p[0], buf, 1) == 0);                              /* what the next recv would see: the end */

    /* A descriptor that is not open: ERR, counted, with no IN or OUT. */
    int dead = p[0];
    CHECK(close(dead) == 0);
    live_os_pollfd bad[2] = {{.fd = dead, .events = LIVE_OS_IN | LIVE_OS_OUT}, {.fd = -1, .events = LIVE_OS_IN}};
    CHECK(live_os_poll(bad, 2, 0) == 1);
    CHECK(bad[0].revents == LIVE_OS_ERR && bad[1].revents == 0);

    /* A timeout clears every revents, including stale ones. */
    int q[2];
    CHECK(pipe(q) == 0);
    live_os_pollfd idle[1] = {{.fd = q[0], .events = LIVE_OS_IN, .revents = 0xffu}};
    CHECK(live_os_poll(idle, 1, 0) == 0 && idle[0].revents == 0);
    CHECK(close(q[0]) == 0 && close(q[1]) == 0);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 2;
    }
    send_normalized();
    puts("PASS send_normalized");
    send_eintr_bounded();
    puts("PASS send_eintr_bounded");
    recv_normalized();
    puts("PASS recv_normalized");
    accept_normalized();
    puts("PASS accept_normalized");
    file_read_at(argv[1]);
    puts("PASS file_read_at");
    real_sockets();
    puts("PASS real_sockets");
    poll_status();
    puts("PASS poll_status");
    puts("live-os: PASS");
    return 0;
}
