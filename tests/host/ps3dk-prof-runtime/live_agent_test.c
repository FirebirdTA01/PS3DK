/* Live link agent over loopback: one named case per run.
 * usage: live-agent-test <case> <empty scratch dir>
 * The test is both the capture writer (a file it grows) and a strict
 * viewer (frame order, contiguous offsets, END fields). The finalization
 * record is injected, as the runtime will publish it after close/join. */
#define _GNU_SOURCE
#include "live_agent.h"
#include "live_os.h"
#include "prof_test_hooks.h"
#include <ps3dk/prof.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c)                                                                   \
    do {                                                                           \
        if (!(c)) {                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
            exit(1);                                                               \
        }                                                                          \
    } while (0)

ps3dk_prof_test_hooks_t ps3dk_prof_test_hooks; /* the host PAL's faults: clock, join */

static char g_dir[900], g_path[1000];
static _Atomic(const struct ps3dk_prof_finalization *) g_fin;
static struct ps3dk_prof_finalization g_fin_rec;

static const struct ps3dk_prof_finalization *fin_get(void *ctx)
{
    (void)ctx;
    return atomic_load(&g_fin);
}

static void publish(uint64_t size, int io_ok, int result)
{
    g_fin_rec = (struct ps3dk_prof_finalization){.accepted_bytes = size, .io_finalized = (uint8_t)io_ok,
                                                 .trailer_written = (uint8_t)io_ok, .shutdown_result = result};
    atomic_store(&g_fin, &g_fin_rec);
}

static uint64_t now_us(void)
{
    uint64_t t = 0;
    (void)pal_mono_now_us(&t);
    return t;
}

/* ---- capture writer ----------------------------------------------------- */

static int g_wfd = -1;
static uint64_t g_written;

static uint8_t pattern(uint64_t i) { return (uint8_t)(i * 131u + (i >> 9) * 7u + 1u); }

static void w_open(void)
{
    g_wfd = open(g_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    CHECK(g_wfd >= 0);
    g_written = 0;
}

/* Appends n pattern bytes; returns the time the write call took. */
static uint64_t w_append(uint64_t n)
{
    static uint8_t buf[65536];
    uint64_t t0 = now_us();
    while (n) {
        uint32_t k = n < sizeof buf ? (uint32_t)n : (uint32_t)sizeof buf;
        for (uint32_t i = 0; i < k; ++i)
            buf[i] = pattern(g_written + i);
        CHECK(write(g_wfd, buf, k) == (ssize_t)k);
        g_written += k;
        n -= k;
    }
    return now_us() - t0;
}

static void w_close(void)
{
    CHECK(close(g_wfd) == 0);
    g_wfd = -1;
}

/* ---- strict viewer -------------------------------------------------------- */

typedef struct client {
    int fd;
    uint8_t *data;
    uint64_t len, cap;
    int hello, end, busy, closed, pongs, close_error;
    uint32_t busy_reason;
    uint8_t endp[LIVE_END_BYTES];
    uint64_t session;
    char name[LIVE_MAX_NAME + 1];
    uint8_t rx[LIVE_HDR_BYTES + LIVE_MAX_PAYLOAD];
    uint32_t rx_len;
    uint32_t asked;  /* the version CLIENT_HELLO asked for (0: LIVE_VERSION) */
    uint32_t caps;   /* HELLO's capability field */
    int acks;        /* CAPTURE_ACKs received; the last one in ack */
    uint8_t ack[LIVE_ACK_BYTES];
    int rsxf_acks, rsxf_states; /* v3 RSX_FRAME_ACK / RSX_FRAME_STATE frames; the last of each */
    uint8_t rsxf_ack[LIVE_RSXF_ACK_BYTES], rsxf_state[LIVE_RSXF_STATE_BYTES];
} client;

/* A small receive buffer makes "the viewer stopped reading" bite quickly. */
static int g_small_rcvbuf;

static int cl_connect(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(fd >= 0);
    if (g_small_rcvbuf) {
        int sz = 4096;
        CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz) == 0);
    }
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0);
    return fd;
}

static void cl_send(int fd, uint32_t type, uint32_t flags, const void *p, uint32_t len)
{
    uint8_t h[LIVE_HDR_BYTES];
    live_put16(h, type);
    live_put16(h + 2, flags);
    live_put32(h + 4, len);
    CHECK(send(fd, h, sizeof h, MSG_NOSIGNAL) == (ssize_t)sizeof h);
    if (len)
        CHECK(send(fd, p, len, MSG_NOSIGNAL) == (ssize_t)len);
}

static void cl_hello_v(int fd, uint32_t magic, uint32_t version)
{
    uint8_t p[8];
    live_put32(p, magic);
    live_put16(p + 4, version);
    live_put16(p + 6, 0);
    cl_send(fd, LIVE_T_CLIENT_HELLO, 0, p, 8);
}

static void cl_hello(int fd, uint32_t magic) { cl_hello_v(fd, magic, LIVE_VERSION); }

static client *cl_open_v(uint16_t port, uint32_t version)
{
    client *c = calloc(1, sizeof *c);
    CHECK(c);
    c->fd = cl_connect(port);
    c->asked = version;
    cl_hello_v(c->fd, LIVE_MAGIC, version);
    return c;
}

static client *cl_open(uint16_t port) { return cl_open_v(port, LIVE_VERSION); }

static void cl_frame(client *c, uint32_t type, const uint8_t *p, uint32_t len)
{
    CHECK(!c->end); /* nothing after END */
    if (type == LIVE_T_BUSY) {
        CHECK(len == 4 && !c->hello);
        c->busy = 1;
        c->busy_reason = live_get32(p);
        return;
    }
    if (!c->hello) {
        CHECK(type == LIVE_T_HELLO); /* HELLO is first */
        CHECK(len >= 16 + 6 && live_get16(p) == (c->asked ? c->asked : LIVE_VERSION)); /* the version asked for */
        c->caps = live_get32(p + 4);
        CHECK(live_get16(p) >= 2 || c->caps == 0); /* v1: the reserved 0 */
        c->session = live_get64(p + 8);
        uint32_t o = 16, al = live_get16(p + o);
        CHECK(al >= 1 && al <= LIVE_MAX_APP);
        o += 2 + al;
        uint32_t bl = live_get16(p + o);
        CHECK(bl <= LIVE_MAX_BUILD_ID);
        o += 2 + bl;
        uint32_t nl = live_get16(p + o);
        CHECK(nl >= 1 && nl <= LIVE_MAX_NAME && o + 2 + nl == len);
        memcpy(c->name, p + o + 2, nl);
        c->hello = 1;
        return;
    }
    if (type == LIVE_T_DATA) {
        CHECK(len > 8 && len <= LIVE_MAX_PAYLOAD);
        CHECK(live_get64(p) == c->len); /* contiguous from 0 */
        uint32_t n = len - 8;
        if (c->len + n > c->cap) {
            c->cap = (c->len + n) * 2;
            c->data = realloc(c->data, c->cap);
            CHECK(c->data);
        }
        memcpy(c->data + c->len, p + 8, n);
        c->len += n;
    } else if (type == LIVE_T_END) {
        CHECK(len == LIVE_END_BYTES);
        memcpy(c->endp, p, len);
        CHECK(live_get64(p + 16) == c->len); /* sent_bytes == received */
        c->end = 1;
    } else if (type == LIVE_T_PONG) {
        CHECK(len == 0);
        c->pongs++;
    } else if (type == LIVE_T_CAPTURE_ACK) {
        CHECK(c->asked != 1); /* never on a v1 session */
        CHECK(len == LIVE_ACK_BYTES && live_get16(p + 14) == 0);
        memcpy(c->ack, p, LIVE_ACK_BYTES);
        c->acks++;
    } else if (type == LIVE_T_RSX_FRAME_ACK) {
        CHECK(c->asked == 0 || c->asked >= 3); /* v3 only */
        CHECK(len == LIVE_RSXF_ACK_BYTES && live_get32(p + 12) == 0);
        memcpy(c->rsxf_ack, p, len);
        c->rsxf_acks++;
    } else if (type == LIVE_T_RSX_FRAME_STATE) {
        CHECK(c->asked == 0 || c->asked >= 3);
        CHECK(len == LIVE_RSXF_STATE_BYTES && live_get16(p + 6) == 0);
        CHECK(c->rsxf_acks > 0); /* the ACK first */
        memcpy(c->rsxf_state, p, len);
        c->rsxf_states++;
    } else {
        CHECK(!"unexpected frame type");
    }
}

/* Reads frames for up to ms, or until END/close (or stop_when becomes true).
 * With past_end, keeps reading after END to observe the close (any frame
 * after END fails the test). */
static void cl_read(client *c, int ms, int (*stop_when)(client *), int past_end)
{
    uint64_t until = now_us() + (uint64_t)ms * 1000;
    while (!c->closed && (past_end || !c->end) && now_us() < until && !(stop_when && stop_when(c))) {
        struct pollfd pf = {.fd = c->fd, .events = POLLIN};
        if (poll(&pf, 1, 10) <= 0)
            continue;
        ssize_t n = recv(c->fd, c->rx + c->rx_len, sizeof c->rx - c->rx_len, 0);
        if (n <= 0) {
            c->close_error = n < 0 ? errno : 0;
            c->closed = 1;
            break;
        }
        c->rx_len += (uint32_t)n;
        for (;;) {
            if (c->rx_len < LIVE_HDR_BYTES)
                break;
            uint32_t type = live_get16(c->rx), flags = live_get16(c->rx + 2), len = live_get32(c->rx + 4);
            CHECK(flags == 0 && len <= LIVE_MAX_PAYLOAD);
            if (c->rx_len < LIVE_HDR_BYTES + len)
                break;
            cl_frame(c, type, c->rx + LIVE_HDR_BYTES, len);
            memmove(c->rx, c->rx + LIVE_HDR_BYTES + len, c->rx_len - LIVE_HDR_BYTES - len);
            c->rx_len -= LIVE_HDR_BYTES + len;
        }
    }
}

static void cl_pump(client *c, int ms, int (*stop_when)(client *)) { cl_read(c, ms, stop_when, 0); }

/* Waits for the agent to close the connection (EOF), up to ms. */
static int cl_wait_closed(client *c, int ms)
{
    cl_read(c, ms, NULL, 1);
    return c->closed;
}

static void cl_free(client *c)
{
    close(c->fd);
    free(c->data);
    free(c);
}

static void check_bytes(const client *c, uint64_t n)
{
    CHECK(c->len == n);
    for (uint64_t i = 0; i < n; ++i)
        if (c->data[i] != pattern(i)) {
            fprintf(stderr, "FAIL byte %llu differs\n", (unsigned long long)i);
            exit(1);
        }
}

/* ---- agent ---------------------------------------------------------------- */

static live_agent g_agent;

static live_config test_cfg(void)
{
    live_config cfg;
    live_config_defaults(&cfg);
    cfg.port = 0;
    cfg.loopback_only = 1;
    cfg.poll_us = 2000;
    cfg.send_timeout_us = 300000;
    cfg.handshake_timeout_us = 200000;
    return cfg;
}

static uint16_t start_agent(live_config cfg)
{
    live_source src = {.path = g_path, .session_id = 0x1122334455667788ull, .app = "liveapp",
                       .app_len = 7, .build_id = "bid", .build_id_len = 3, .finalization = fin_get};
    CHECK(live_agent_start(&g_agent, &cfg, &src) == LIVE_OK);
    return live_agent_port(&g_agent);
}

static unsigned long long drops(enum live_drop d) { return atomic_load(&g_agent.drops[d]); }

static int has_hello(client *c) { return c->hello; }
static int has_bytes_1m(client *c) { return c->len >= (1u << 20); }

/* ---- cases ---------------------------------------------------------------- */

/* Hold the agent after the kernel accepts END, before its close path. A PING
 * is then confirmed unread in that socket, so an immediate close sends RST. */
static atomic_int end_held, end_release, end_fd;
static long hold_end_send(int fd, const void *buf, unsigned long n)
{
    long sent = send(fd, buf, n, MSG_NOSIGNAL);
    if (n == LIVE_HDR_BYTES + LIVE_END_BYTES && live_get16(buf) == LIVE_T_END) {
        CHECK(sent == (long)n);
        atomic_store(&end_fd, fd);
        atomic_store(&end_held, 1);
        while (!atomic_load(&end_release))
            pal_sleep_us(1000);
    }
    return sent;
}

static void case_end_with_unread_ping(void)
{
    w_open();
    w_append(1000);
    w_close();
    publish(1000, 1, 0);
    ps3dk_prof_test_live_os_hooks.send = hold_end_send;
    client *c = cl_open(start_agent(test_cfg()));
    cl_pump(c, 2000, NULL);
    CHECK(c->end);
    uint64_t until = now_us() + 2000000;
    while (!atomic_load(&end_held) && now_us() < until)
        pal_sleep_us(1000);
    CHECK(atomic_load(&end_held));
    cl_send(c->fd, LIVE_T_PING, 0, NULL, 0);
    int queued = 0;
    until = now_us() + 2000000;
    while (queued < (int)LIVE_HDR_BYTES && now_us() < until) {
        CHECK(ioctl(atomic_load(&end_fd), FIONREAD, &queued) == 0);
        pal_sleep_us(1000);
    }
    CHECK(queued >= (int)LIVE_HDR_BYTES);
    atomic_store(&end_release, 1);
    CHECK(cl_wait_closed(c, 2000));
    if (c->close_error)
        fprintf(stderr, "close after END: %s\n", strerror(c->close_error));
    CHECK(c->close_error == 0); /* FIN, never ECONNRESET after a valid END */
    check_bytes(c, 1000);
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 1000000, 1000000) == LIVE_OK);
}

/* A peer that keeps its send side open cannot hold the agent after END.
 * Input during the drain must not move its fixed deadline. */
static void case_end_drain_deadline(void)
{
    w_open();
    w_append(1000);
    w_close();
    publish(1000, 1, 0);
    live_config cfg = test_cfg();
    cfg.send_timeout_us = 100000;
    uint16_t port = start_agent(cfg);
    client *c = cl_open(port);
    cl_pump(c, 2000, NULL);
    CHECK(c->end && cl_wait_closed(c, 2000) && c->close_error == 0);
    /* Do not close c's send side. The connection remains owned while the
     * peer can still send, rather than disappearing as soon as FIN is sent. */
    client *busy = cl_open(port);
    CHECK(cl_wait_closed(busy, 2000) && busy->busy);
    cl_free(busy);
    uint8_t ping[LIVE_HDR_BYTES] = {0};
    live_put16(ping, LIVE_T_PING);
    uint64_t until = now_us() + 200000;
    while (now_us() < until) {
        (void)send(c->fd, ping, sizeof ping, MSG_NOSIGNAL);
        pal_sleep_us(5000);
    }
    client *next = cl_open(port);
    cl_pump(next, 2000, NULL);
    CHECK(next->end && !next->busy); /* input did not extend the deadline */
    CHECK(cl_wait_closed(next, 2000) && next->close_error == 0);
    check_bytes(next, 1000);
    CHECK(drops(LIVE_DROP_END) == 2); /* one completion per stream */
    cl_free(next);
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 1000000, 1000000) == LIVE_OK);
}

static void case_end_drain_cancel(void)
{
    w_open();
    w_append(1000);
    w_close();
    publish(1000, 1, 0);
    live_config cfg = test_cfg();
    cfg.send_timeout_us = 10000000;
    client *c = cl_open(start_agent(cfg));
    cl_pump(c, 2000, NULL);
    CHECK(c->end && cl_wait_closed(c, 2000) && c->close_error == 0);
    uint64_t t0 = now_us();
    CHECK(live_agent_stop(&g_agent, 50000, 1000000) == LIVE_OK);
    CHECK(now_us() - t0 < 500000); /* graceful stop wins over the drain limit */
    CHECK(drops(LIVE_DROP_END) == 1 && drops(LIVE_DROP_CANCELLED) == 0);
    cl_free(c);
}

static void case_end_drain_peer_fin(void)
{
    w_open();
    w_append(1000);
    w_close();
    publish(1000, 1, 0);
    live_config cfg = test_cfg();
    cfg.send_timeout_us = 10000000;
    uint16_t port = start_agent(cfg);
    client *c = cl_open(port);
    cl_pump(c, 2000, NULL);
    CHECK(c->end && cl_wait_closed(c, 2000) && c->close_error == 0);
    uint64_t t0 = now_us();
    CHECK(shutdown(c->fd, SHUT_WR) == 0); /* peer FIN, not the drain timeout */
    client *next = cl_open(port);
    cl_pump(next, 1000, NULL);
    CHECK(next->end && !next->busy);
    CHECK(now_us() - t0 < 1000000);
    CHECK(cl_wait_closed(next, 1000) && next->close_error == 0);
    CHECK(drops(LIVE_DROP_END) == 2 && drops(LIVE_DROP_STALLED) == 0);
    cl_free(c);
    cl_free(next);
    CHECK(live_agent_stop(&g_agent, 1000000, 1000000) == LIVE_OK);
}

static void case_late_join(void)
{
    w_open();
    w_append(300000);
    w_close();
    publish(300000, 1, 0);
    client *c = cl_open(start_agent(test_cfg()));
    cl_pump(c, 5000, NULL);
    CHECK(c->hello && c->end);
    CHECK(c->session == 0x1122334455667788ull && strcmp(c->name, "cap.prof") == 0);
    check_bytes(c, 300000);
    CHECK(c->endp[0] == 1 && c->endp[1] == 1 && c->endp[2] == 0);
    CHECK((int32_t)live_get32(c->endp + 4) == 0 && live_get64(c->endp + 8) == 300000);
    CHECK(cl_wait_closed(c, 2000));
    CHECK(drops(LIVE_DROP_END) == 1);
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 1000000, 1000000) == LIVE_OK);
}

static void case_grow_wait(void)
{
    w_open();
    client *c = cl_open(start_agent(test_cfg()));
    for (int i = 0; i < 100; ++i) {
        w_append(10000 + (uint64_t)i);
        cl_pump(c, 5, NULL);
        CHECK(!c->end); /* temporary EOF is a wait, never END */
    }
    cl_pump(c, 300, NULL);
    CHECK(!c->end && c->len == g_written);
    w_close();
    cl_pump(c, 200, NULL);
    CHECK(!c->end); /* closed file but no finalization record: still no END */
    publish(g_written, 1, 0);
    cl_pump(c, 3000, NULL);
    CHECK(c->end);
    check_bytes(c, g_written);
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 1000000, 1000000) == LIVE_OK);
}

static void case_failed_final(void)
{
    w_open();
    w_append(5000);
    w_close();
    publish(4000, 0, -10); /* failed sink: io_finalized 0, a size that is not the file */
    client *c = cl_open(start_agent(test_cfg()));
    cl_pump(c, 3000, NULL);
    CHECK(c->end);
    check_bytes(c, 5000);
    CHECK(c->endp[0] == 0 && (int32_t)live_get32(c->endp + 4) == -10);
    CHECK(live_get64(c->endp + 8) == 4000 && live_get64(c->endp + 16) == 5000);
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 1000000, 1000000) == LIVE_OK);
}

static void case_cap(void)
{
    w_open();
    w_append(5000);
    w_close();
    publish(5000, 1, 0);
    live_config cfg = test_cfg();
    cfg.cap_bytes = 1000;
    client *c = cl_open(start_agent(cfg));
    cl_pump(c, 3000, NULL);
    CHECK(c->end && c->len == 1000);
    CHECK(live_get64(c->endp + 8) == 5000 && live_get64(c->endp + 16) == 1000);
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 1000000, 1000000) == LIVE_OK);
}

static void case_start_errors(void)
{
    w_open();
    w_close();
    live_config bad = test_cfg();
    bad.poll_us = 0;
    live_source src = {.path = g_path, .app = "a", .app_len = 1, .finalization = fin_get};
    CHECK(live_agent_stop(&g_agent, 0, 0) == LIVE_ESTATE); /* never started */
    CHECK(live_agent_start(&g_agent, &bad, &src) == LIVE_EINVAL);
    live_config cfg = test_cfg();
    live_source nopath = src;
    nopath.path = "";
    CHECK(live_agent_start(&g_agent, &cfg, &nopath) == LIVE_EINVAL);
    live_source nofin = src;
    nofin.finalization = NULL;
    CHECK(live_agent_start(&g_agent, &cfg, &nofin) == LIVE_EINVAL);
    live_source longapp = src;
    longapp.app_len = LIVE_MAX_APP + 1;
    CHECK(live_agent_start(&g_agent, &cfg, &longapp) == LIVE_EINVAL);
    CHECK(live_agent_start(&g_agent, &cfg, &src) == LIVE_OK);
    CHECK(live_agent_start(&g_agent, &cfg, &src) == LIVE_EALREADY);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
    CHECK(live_agent_stop(&g_agent, 0, 0) == LIVE_ESTATE); /* already stopped */
    CHECK(live_agent_start(&g_agent, &cfg, &src) == LIVE_EALREADY); /* one start per agent */
}

static void case_handshake_timeout(void)
{
    w_open();
    w_close();
    uint16_t port = start_agent(test_cfg());
    client *c = calloc(1, sizeof *c);
    c->fd = cl_connect(port);
    uint64_t t0 = now_us();
    CHECK(cl_wait_closed(c, 3000));
    CHECK(now_us() - t0 < 1000000);
    CHECK(!c->hello && drops(LIVE_DROP_HANDSHAKE_TIMEOUT) == 1);
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

static void case_bad_hello(void)
{
    w_open();
    w_close();
    uint16_t port = start_agent(test_cfg());
    client *c = calloc(1, sizeof *c);
    c->fd = cl_connect(port);
    cl_hello(c->fd, 0xdeadbeef);
    CHECK(cl_wait_closed(c, 2000) && !c->hello);
    CHECK(drops(LIVE_DROP_BAD_HELLO) == 1);
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

/* After HELLO, a frame the viewer must never send closes the link. */
static void protocol_case(uint32_t type, uint32_t flags, uint32_t len)
{
    w_open();
    w_append(100);
    uint16_t port = start_agent(test_cfg());
    client *c = cl_open(port);
    cl_pump(c, 2000, has_hello);
    CHECK(c->hello);
    uint8_t h[LIVE_HDR_BYTES];
    live_put16(h, type);
    live_put16(h + 2, flags);
    live_put32(h + 4, len);
    CHECK(send(c->fd, h, sizeof h, MSG_NOSIGNAL) == (ssize_t)sizeof h);
    CHECK(cl_wait_closed(c, 2000) && !c->end);
    CHECK(drops(LIVE_DROP_PROTOCOL) == 1);
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
    w_close();
}

static void case_unknown_type(void) { protocol_case(99, 0, 0); }
static void case_oversize(void) { protocol_case(LIVE_T_PING, 0, 70000); }
static void case_nonzero_flags(void) { protocol_case(LIVE_T_PING, 1, 0); }
static void case_reserved_type(void) { protocol_case(LIVE_T_RESERVED_TTY_FIRST, 0, 0); }

static void case_ping(void)
{
    w_open();
    w_append(100);
    client *c = cl_open(start_agent(test_cfg()));
    cl_pump(c, 2000, has_hello);
    for (int i = 0; i < 3; ++i)
        cl_send(c->fd, LIVE_T_PING, 0, NULL, 0);
    cl_pump(c, 300, NULL);
    CHECK(c->pongs == 3 && !c->closed);
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
    w_close();
}

static void case_ping_flood(void)
{
    w_open();
    w_append(100);
    client *c = cl_open(start_agent(test_cfg()));
    cl_pump(c, 2000, has_hello);
    for (int i = 0; i < 10; ++i)
        cl_send(c->fd, LIVE_T_PING, 0, NULL, 0);
    CHECK(cl_wait_closed(c, 2000));
    CHECK(drops(LIVE_DROP_PING_FLOOD) == 1 && c->pongs <= (int)LIVE_MAX_PINGS_PER_S);
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
    w_close();
}

static void case_second_client_busy(void)
{
    w_open();
    w_append(200000);
    uint16_t port = start_agent(test_cfg());
    client *a = cl_open(port);
    cl_pump(a, 2000, has_hello);
    client *b = calloc(1, sizeof *b);
    b->fd = cl_connect(port);
    uint64_t t0 = now_us();
    CHECK(cl_wait_closed(b, 2000));
    CHECK(now_us() - t0 < 100000 + 50000); /* refused within ~100 ms (+ scheduling slack) */
    CHECK(b->busy && b->busy_reason == LIVE_BUSY_STREAMING && !b->hello);
    w_close();
    publish(g_written, 1, 0);
    cl_pump(a, 3000, NULL);
    CHECK(a->end); /* the first viewer was not displaced */
    check_bytes(a, g_written);
    CHECK(atomic_load(&g_agent.busy_refusals) == 1);
    cl_free(a);
    cl_free(b);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

static void case_reconnect_after_error(void)
{
    w_open();
    w_append(50000);
    uint16_t port = start_agent(test_cfg());
    client *a = cl_open(port);
    cl_pump(a, 2000, has_hello);
    cl_send(a->fd, 99, 0, NULL, 0);
    CHECK(cl_wait_closed(a, 2000));
    CHECK(!a->end && drops(LIVE_DROP_PROTOCOL) == 1);
    cl_free(a);
    w_close();
    publish(50000, 1, 0);
    client *b = cl_open(port); /* the agent is back in LISTEN */
    cl_pump(b, 3000, NULL);
    CHECK(b->end);
    check_bytes(b, 50000); /* a new candidate from offset 0 */
    CHECK(cl_wait_closed(b, 2000) && b->close_error == 0);
    cl_free(b); /* acknowledge FIN before connecting the next viewer */
    client *late = cl_open(port); /* after a completed stream, still served */
    cl_pump(late, 3000, NULL);
    CHECK(late->end);
    check_bytes(late, 50000);
    cl_free(late);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

/* Writer timing: the same appends with no agent, then with a viewer that
 * never reads. The writer must not wait on the viewer. Prints both. */
static uint64_t timed_writes(int blocks, uint64_t *worst)
{
    uint64_t total = 0;
    *worst = 0;
    for (int i = 0; i < blocks; ++i) {
        uint64_t t = w_append(65536);
        total += t;
        if (t > *worst)
            *worst = t;
    }
    return total;
}

static void case_stalled_viewer(void)
{
    uint64_t base_worst, stall_worst;
    w_open();
    uint64_t base = timed_writes(512, &base_worst);
    w_close();
    CHECK(unlink(g_path) == 0);
    w_open();
    g_small_rcvbuf = 1;
    client *c = cl_open(start_agent(test_cfg()));
    cl_pump(c, 2000, has_hello);
    /* Stop reading: the socket buffers fill and the agent cannot send. */
    uint64_t stall = timed_writes(512, &stall_worst);
    uint64_t t0 = now_us();
    while (drops(LIVE_DROP_STALLED) == 0 && now_us() - t0 < 3000000)
        pal_sleep_us(10000);
    printf("MEASURE writer 32 MiB: no agent total %llu us worst %llu us; stalled viewer total %llu us worst %llu us\n",
           (unsigned long long)base, (unsigned long long)base_worst, (unsigned long long)stall,
           (unsigned long long)stall_worst);
    CHECK(drops(LIVE_DROP_STALLED) == 1);
    CHECK(stall_worst < 200000); /* no write waited on the viewer (send timeout is 300 ms) */
    w_close();
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

static void case_stop_graceful(void)
{
    w_open();
    w_append(400000);
    client *c = cl_open(start_agent(test_cfg()));
    cl_pump(c, 2000, has_hello);
    w_close();
    publish(400000, 1, 0);
    CHECK(live_agent_stop(&g_agent, 2000000, 1000000) == LIVE_OK); /* the stream may finish */
    cl_pump(c, 2000, NULL);
    CHECK(c->end);
    check_bytes(c, 400000);
    cl_free(c);
}

static void case_stop_cancel(void)
{
    w_open();
    w_append(32u << 20);
    w_close();
    publish(g_written, 1, 0);
    g_small_rcvbuf = 1;
    live_config cfg = test_cfg();
    cfg.send_timeout_us = 10000000; /* the stall rule must not end it first */
    client *c = cl_open(start_agent(cfg));
    cl_pump(c, 2000, has_bytes_1m);
    /* The viewer stops reading; stop gives 100 ms grace, then cancels. */
    uint64_t t0 = now_us();
    CHECK(live_agent_stop(&g_agent, 100000, 1000000) == LIVE_OK);
    uint64_t took = now_us() - t0;
    CHECK(took >= 100000 && took < 600000);
    CHECK(drops(LIVE_DROP_CANCELLED) == 1 && drops(LIVE_DROP_END) == 0);
    cl_pump(c, 2000, NULL);
    CHECK(!c->end && c->len < g_written); /* a partial download, never END */
    CHECK(atomic_load(&g_agent.bytes_delivered) <= c->len + LIVE_MAX_DATA); /* not overstated */
    cl_free(c);
}

static void case_stop_timeout(void)
{
    /* The capture path is a FIFO: the agent's open() blocks until a writer
     * appears, standing in for an LV2 file call with no deadline. */
    CHECK(mkfifo(g_path, 0644) == 0);
    client *c = cl_open(start_agent(test_cfg()));
    pal_sleep_us(100000); /* the agent is now inside open() */
    CHECK(live_agent_stop(&g_agent, 0, 50000) == LIVE_ETIMEDOUT);
    CHECK(atomic_load(&g_agent.exited) == 0); /* state kept: the thread still owns it */
    int wfd = open(g_path, O_WRONLY | O_CLOEXEC); /* releases the agent's open() */
    CHECK(wfd >= 0);
    CHECK(close(wfd) == 0);
    CHECK(live_agent_stop(&g_agent, 0, 2000000) == LIVE_OK);
    /* The cancellation fell due while open() blocked: it wins over the stream. */
    CHECK(drops(LIVE_DROP_CANCELLED) == 1 && drops(LIVE_DROP_IO) == 0);
    cl_free(c);
}

static void case_late_hello(void)
{
    /* The agent sleeps in poll longer than the handshake allows; a complete
     * hello that arrives after the deadline must still be refused. */
    w_open();
    w_append(100);
    live_config cfg = test_cfg();
    cfg.poll_us = 500000;
    cfg.handshake_timeout_us = 50000;
    uint16_t port = start_agent(cfg);
    client *c = calloc(1, sizeof *c);
    c->fd = cl_connect(port);
    pal_sleep_us(300000);
    cl_hello(c->fd, LIVE_MAGIC);
    CHECK(cl_wait_closed(c, 3000));
    CHECK(!c->hello && drops(LIVE_DROP_HANDSHAKE_TIMEOUT) == 1);
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 0, 2000000) == LIVE_OK);
    w_close();
}

static void case_stop_overflow(void)
{
    w_open();
    w_close();
    start_agent(test_cfg());
    CHECK(live_agent_stop(&g_agent, UINT64_MAX, 0) == LIVE_EINVAL);
    CHECK(live_agent_stop(&g_agent, 0, UINT64_MAX) == LIVE_EINVAL);
    CHECK(live_agent_stop(&g_agent, UINT64_MAX / 2, UINT64_MAX / 2) == LIVE_EINVAL);
    CHECK(atomic_load(&g_agent.state) == 1); /* still running: nothing changed */
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

static void case_clock_fail(void)
{
    w_open();
    w_append(100);
    client *c = cl_open(start_agent(test_cfg()));
    cl_pump(c, 2000, has_hello);
    atomic_store(&ps3dk_prof_test_live_hooks.clock_fail, 1);
    CHECK(live_agent_stop(&g_agent, 1000000, 1000000) == LIVE_ECLOCK);
    CHECK(atomic_load(&g_agent.state) == 1); /* the failed stop changed nothing */
    /* The agent cannot keep deadlines without a clock: it drops the viewer and exits. */
    CHECK(cl_wait_closed(c, 3000));
    atomic_store(&ps3dk_prof_test_live_hooks.clock_fail, 0);
    CHECK(drops(LIVE_DROP_CLOCK) == 1);
    CHECK(live_agent_stop(&g_agent, 0, 2000000) == LIVE_OK);
    cl_free(c);
    w_close();
}

static void case_join_fail(void)
{
    w_open();
    w_close();
    start_agent(test_cfg());
    ps3dk_prof_test_hooks.join_fail = 1;
    CHECK(live_agent_stop(&g_agent, 0, 2000000) == LIVE_EJOIN);
    ps3dk_prof_test_hooks.join_fail = 0;
    CHECK(atomic_load(&g_agent.state) == 4);             /* terminal */
    CHECK(live_agent_stop(&g_agent, 0, 0) == LIVE_ESTATE); /* never retried */
    live_config cfg = test_cfg();
    live_source src = {.path = g_path, .app = "a", .app_len = 1, .finalization = fin_get};
    CHECK(live_agent_start(&g_agent, &cfg, &src) == LIVE_EALREADY); /* not reusable */
}

/* A refused second viewer is closed promptly whether or not BUSY could be sent. */
static void busy_fault_case(int nonblock_fail)
{
    w_open();
    w_append(200000);
    uint16_t port = start_agent(test_cfg());
    client *a = cl_open(port);
    cl_pump(a, 2000, has_hello);
    atomic_store(nonblock_fail ? &ps3dk_prof_test_live_hooks.busy_nonblock_fail
                               : &ps3dk_prof_test_live_hooks.busy_send_eagain, 1);
    client *b = calloc(1, sizeof *b);
    b->fd = cl_connect(port);
    uint64_t t0 = now_us();
    CHECK(cl_wait_closed(b, 2000));
    CHECK(now_us() - t0 < 150000);
    CHECK(!b->busy && atomic_load(&g_agent.busy_refusals) == 1); /* BUSY is best-effort */
    atomic_store(&ps3dk_prof_test_live_hooks.busy_nonblock_fail, 0);
    atomic_store(&ps3dk_prof_test_live_hooks.busy_send_eagain, 0);
    w_close();
    publish(g_written, 1, 0);
    cl_pump(a, 3000, NULL);
    CHECK(a->end);
    check_bytes(a, g_written);
    cl_free(a);
    cl_free(b);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

static void case_busy_nonblock_fail(void) { busy_fault_case(1); }
static void case_busy_send_eagain(void) { busy_fault_case(0); }

/* A pread held across the end of stop's grace period: when it returns, with
 * data or with EOF after the finalization record, the stream is cancelled,
 * never continued and never completed with END. */
static void held_pread_case(int at_eof)
{
    live_test_hooks *h = &ps3dk_prof_test_live_hooks;
    w_open();
    w_append(100000);
    client *c = cl_open(start_agent(test_cfg()));
    /* Take everything written so far, so the next pread is at EOF. */
    uint64_t t0 = now_us();
    while (c->len < 100000 && now_us() - t0 < 3000000)
        cl_pump(c, 20, NULL);
    CHECK(c->len == 100000);
    /* Hold the agent's next pread (it reads only after release), then give
     * it something to return. */
    atomic_store(&h->pread_block, 1);
    t0 = now_us();
    while (!atomic_load(&h->pread_entered) && now_us() - t0 < 3000000)
        pal_sleep_us(1000);
    CHECK(atomic_load(&h->pread_entered));
    if (!at_eof) {
        w_append(50000); /* the held pread will return data */
    } else {
        w_close();
        publish(g_written, 1, 0); /* END would be next */
    }
    CHECK(live_agent_stop(&g_agent, 0, 50000) == LIVE_ETIMEDOUT); /* grace 0: cancellation due */
    uint64_t before = c->len;
    atomic_store(&h->pread_release, 1);
    CHECK(live_agent_stop(&g_agent, 0, 2000000) == LIVE_OK);
    CHECK(cl_wait_closed(c, 2000));
    CHECK(!c->end && c->len == before); /* nothing sent after the deadline */
    CHECK(drops(LIVE_DROP_CANCELLED) == 1 && drops(LIVE_DROP_END) == 0);
    atomic_store(&h->pread_release, 0);
    atomic_store(&h->pread_entered, 0);
    cl_free(c);
    if (!at_eof)
        w_close();
}

static void case_held_pread_data(void) { held_pread_case(0); }
static void case_held_pread_eof(void) { held_pread_case(1); }

static void case_delivered_counts_sent_frames(void)
{
    w_open();
    w_append(200000);
    w_close();
    publish(200000, 1, 0);
    client *c = cl_open(start_agent(test_cfg()));
    cl_pump(c, 3000, NULL);
    CHECK(c->end);
    CHECK(atomic_load(&g_agent.bytes_delivered) == 200000);
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

/* ---- capture commands (protocol v2) ---------------------------------------------------------------------- */

/* A scripted command source, standing in for the runtime's mailbox: the test decides each result. */
static struct {
    atomic_int submits, polls, cancels;
    atomic_int submit_rc;    /* returned by submit (0: queued) */
    atomic_int done;         /* poll reports the result once this is set */
    atomic_int cancel_rc;    /* returned by cancel */
    int32_t result;
    uint32_t window;
    uint32_t type, frames, name_len;
    char name[LIVE_MAX_WINDOW_NAME + 1];
} g_cmd;

static int fake_submit(void *ctx, uint32_t type, const char *name, uint32_t name_len, uint32_t frames)
{
    (void)ctx;
    g_cmd.type = type;
    g_cmd.frames = frames;
    g_cmd.name_len = name_len;
    CHECK(name_len <= LIVE_MAX_WINDOW_NAME && (name_len == 0) == (type == LIVE_T_CAPTURE_END));
    memcpy(g_cmd.name, name ? name : "", name_len);
    g_cmd.name[name_len] = 0;
    atomic_fetch_add(&g_cmd.submits, 1);
    return atomic_load(&g_cmd.submit_rc);
}

static int fake_poll(void *ctx, int32_t *result, uint32_t *window_id)
{
    (void)ctx;
    atomic_fetch_add(&g_cmd.polls, 1);
    if (!atomic_exchange(&g_cmd.done, 0))
        return 0;
    *result = g_cmd.result;
    *window_id = g_cmd.window;
    return 1;
}

static int fake_cancel(void *ctx)
{
    (void)ctx;
    atomic_fetch_add(&g_cmd.cancels, 1);
    return atomic_load(&g_cmd.cancel_rc);
}

static const live_cmd_ops g_fake_ops = {.submit = fake_submit, .poll = fake_poll, .cancel = fake_cancel};

static uint16_t start_cmd_agent(live_config cfg, const live_cmd_ops *ops)
{
    live_source src = {.path = g_path, .session_id = 0x1122334455667788ull, .app = "liveapp",
                       .app_len = 7, .build_id = "bid", .build_id_len = 3, .finalization = fin_get, .cmd = ops};
    CHECK(live_agent_start(&g_agent, &cfg, &src) == LIVE_OK);
    return live_agent_port(&g_agent);
}

static void send_begin(int fd, uint32_t req, const char *name)
{
    uint8_t p[8 + LIVE_MAX_WINDOW_NAME];
    uint32_t n = (uint32_t)strlen(name);
    live_put32(p, req);
    live_put16(p + 4, n);
    live_put16(p + 6, 0);
    memcpy(p + 8, name, n);
    cl_send(fd, LIVE_T_CAPTURE_BEGIN, 0, p, 8 + n);
}

static void send_frames(int fd, uint32_t req, uint32_t frames, const char *name)
{
    uint8_t p[12 + LIVE_MAX_WINDOW_NAME];
    uint32_t n = (uint32_t)strlen(name);
    live_put32(p, req);
    live_put32(p + 4, frames);
    live_put16(p + 8, n);
    live_put16(p + 10, 0);
    memcpy(p + 12, name, n);
    cl_send(fd, LIVE_T_CAPTURE_FRAMES, 0, p, 12 + n);
}

static void send_end(int fd, uint32_t req)
{
    uint8_t p[4];
    live_put32(p, req);
    cl_send(fd, LIVE_T_CAPTURE_END, 0, p, 4);
}

static int has_ack(client *c) { return c->acks > 0; }

static void check_ack(const client *c, uint32_t req, int32_t result, uint32_t window, uint32_t type)
{
    CHECK(live_get32(c->ack) == req);
    CHECK((int32_t)live_get32(c->ack + 4) == result);
    CHECK(live_get32(c->ack + 8) == window);
    CHECK(live_get16(c->ack + 12) == type);
}

/* A v2 hello is answered in v2 with the TRIGGERS capability; BEGIN, FRAMES and END each reach the source with
 * their exact fields and come back as one ACK carrying the request id, the result, the window id and the type. */
static void case_cmd_round_trip(void)
{
    w_open();
    w_append(100);
    client *c = cl_open(start_cmd_agent(test_cfg(), &g_fake_ops));
    cl_pump(c, 2000, has_hello);
    CHECK(c->hello && c->caps == LIVE_CAP_TRIGGERS);

    send_begin(c->fd, 7, "pc-window");
    uint64_t until = now_us() + 2000000;
    while (atomic_load(&g_cmd.submits) < 1 && now_us() < until)
        pal_sleep_us(1000);
    CHECK(g_cmd.type == LIVE_T_CAPTURE_BEGIN && strcmp(g_cmd.name, "pc-window") == 0 && g_cmd.frames == 0);
    cl_pump(c, 100, has_ack);
    CHECK(c->acks == 0); /* pending until the source finishes it */
    g_cmd.result = 0;
    g_cmd.window = 5;
    atomic_store(&g_cmd.done, 1);
    cl_pump(c, 2000, has_ack);
    CHECK(c->acks == 1);
    check_ack(c, 7, 0, 5, LIVE_T_CAPTURE_BEGIN);

    c->acks = 0;
    send_end(c->fd, 8);
    g_cmd.result = PS3DK_PROF_EPERM;
    g_cmd.window = 9; /* a refusal never names a window */
    atomic_store(&g_cmd.done, 1);
    cl_pump(c, 2000, has_ack);
    CHECK(c->acks == 1 && g_cmd.type == LIVE_T_CAPTURE_END && g_cmd.name_len == 0);
    check_ack(c, 8, PS3DK_PROF_EPERM, 0, LIVE_T_CAPTURE_END);

    c->acks = 0;
    char longest[LIVE_MAX_WINDOW_NAME + 1];
    memset(longest, 'n', LIVE_MAX_WINDOW_NAME);
    longest[LIVE_MAX_WINDOW_NAME] = 0;
    send_frames(c->fd, 9, LIVE_MAX_WINDOW_FRAMES, longest);
    g_cmd.result = 0;
    g_cmd.window = 6;
    atomic_store(&g_cmd.done, 1);
    cl_pump(c, 2000, has_ack);
    CHECK(c->acks == 1 && g_cmd.type == LIVE_T_CAPTURE_FRAMES && g_cmd.frames == LIVE_MAX_WINDOW_FRAMES);
    CHECK(strcmp(g_cmd.name, longest) == 0);
    check_ack(c, 9, 0, 6, LIVE_T_CAPTURE_FRAMES);

    /* the stream goes on, and ends normally */
    w_close();
    publish(100, 1, 0);
    cl_pump(c, 3000, NULL);
    CHECK(c->end && !c->closed);
    check_bytes(c, 100);
    CHECK(atomic_load(&g_agent.acks) == 3 && atomic_load(&g_agent.ack_refusals) == 1);
    CHECK(atomic_load(&g_cmd.cancels) == 0);
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

/* A refusal at submit is acked at once; nothing is queued. */
static void case_cmd_refused_at_submit(void)
{
    w_open();
    w_append(100);
    client *c = cl_open(start_cmd_agent(test_cfg(), &g_fake_ops));
    cl_pump(c, 2000, has_hello);
    atomic_store(&g_cmd.submit_rc, PS3DK_PROF_EFULL);
    send_frames(c->fd, 3, 10, "x");
    cl_pump(c, 2000, has_ack);
    check_ack(c, 3, PS3DK_PROF_EFULL, 0, LIVE_T_CAPTURE_FRAMES);
    CHECK(atomic_load(&g_cmd.cancels) == 0);
    /* the next command is accepted (one at a time, and the last was answered) */
    c->acks = 0;
    atomic_store(&g_cmd.submit_rc, PS3DK_PROF_ESTOPPED);
    send_begin(c->fd, 4, "y");
    cl_pump(c, 2000, has_ack);
    check_ack(c, 4, PS3DK_PROF_ESTOPPED, 0, LIVE_T_CAPTURE_BEGIN);
    CHECK(!c->closed);
    w_close();
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

/* No frame mark took it before the deadline: withdrawn (cancel) and refused ETIMEDOUT, never silent.  A cancel
 * that loses to the frame thread (0) waits for the source's result instead. */
static void case_cmd_timeout(void)
{
    w_open();
    w_append(100);
    live_config cfg = test_cfg();
    cfg.cmd_timeout_us = 50000;
    client *c = cl_open(start_cmd_agent(cfg, &g_fake_ops));
    cl_pump(c, 2000, has_hello);
    atomic_store(&g_cmd.cancel_rc, 1);
    uint64_t t0 = now_us();
    send_begin(c->fd, 11, "slow");
    cl_pump(c, 2000, has_ack);
    CHECK(c->acks == 1 && now_us() - t0 >= 50000);
    check_ack(c, 11, PS3DK_PROF_ETIMEDOUT, 0, LIVE_T_CAPTURE_BEGIN);
    CHECK(atomic_load(&g_cmd.cancels) == 1);

    c->acks = 0;
    atomic_store(&g_cmd.cancel_rc, 0); /* being applied: the agent keeps polling */
    send_end(c->fd, 12);
    cl_pump(c, 300, has_ack);
    CHECK(c->acks == 0 && atomic_load(&g_cmd.cancels) >= 2);
    g_cmd.result = 0;
    g_cmd.window = 2;
    atomic_store(&g_cmd.done, 1);
    cl_pump(c, 2000, has_ack);
    check_ack(c, 12, 0, 2, LIVE_T_CAPTURE_END);
    w_close();
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

/* A v2 agent without a command source: HELLO has no TRIGGERS bit; a command is refused ENOTSUP. */
static void case_cmd_no_source(void)
{
    w_open();
    w_append(100);
    client *c = cl_open(start_agent(test_cfg()));
    cl_pump(c, 2000, has_hello);
    CHECK(c->caps == 0);
    send_end(c->fd, 1);
    cl_pump(c, 2000, has_ack);
    check_ack(c, 1, PS3DK_PROF_ENOTSUP, 0, LIVE_T_CAPTURE_END);
    CHECK(!c->closed);
    w_close();
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

/* A v2 agent serves a v1 viewer exactly as v1: HELLO version 1 with the reserved 0 (even with a command source),
 * and a command frame on that session is a protocol drop. */
static void case_cmd_v1_session(void)
{
    w_open();
    w_append(5000);
    uint16_t port = start_cmd_agent(test_cfg(), &g_fake_ops);
    client *c = cl_open_v(port, 1);
    cl_pump(c, 2000, has_hello);
    CHECK(c->hello && c->caps == 0);
    for (int i = 0; i < 3; ++i)
        cl_send(c->fd, LIVE_T_PING, 0, NULL, 0);
    cl_pump(c, 300, NULL);
    CHECK(c->pongs == 3 && !c->closed);
    send_begin(c->fd, 1, "v1");
    CHECK(cl_wait_closed(c, 2000) && !c->end && c->acks == 0);
    CHECK(drops(LIVE_DROP_PROTOCOL) == 1 && atomic_load(&g_cmd.submits) == 0);
    cl_free(c);
    /* and a v1 viewer still streams to the end */
    w_close();
    publish(5000, 1, 0);
    client *d = cl_open_v(port, 1);
    cl_pump(d, 3000, NULL);
    CHECK(d->end && d->caps == 0);
    check_bytes(d, 5000);
    cl_free(d);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

/* Versions outside 1..LIVE_VERSION are bad hellos. */
static void case_cmd_bad_versions(void)
{
    w_open();
    w_close();
    uint16_t port = start_cmd_agent(test_cfg(), &g_fake_ops);
    const uint32_t bad[] = {0, LIVE_VERSION + 1, 0xffff};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        client *c = calloc(1, sizeof *c);
        c->fd = cl_connect(port);
        cl_hello_v(c->fd, LIVE_MAGIC, bad[i]);
        CHECK(cl_wait_closed(c, 2000) && !c->hello);
        cl_free(c);
    }
    CHECK(drops(LIVE_DROP_BAD_HELLO) == 3);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

/* ---- RSX frame capture (protocol v3) ------------------------------------------------------------------ */

/* A scripted capture source: the test decides availability, the request result and the status it reports. */
static struct {
    atomic_int available, requests, rc;
    uint32_t req, flags, name_len, id;
    char name[LIVE_MAX_WINDOW_NAME + 1];
    live_rsxf_status st;
    atomic_int st_seq;
} g_rf;

static int rf_request(uint32_t req, uint32_t flags, const char *name, uint32_t n, uint32_t *id)
{
    g_rf.req = req;
    g_rf.flags = flags;
    g_rf.name_len = n;
    memcpy(g_rf.name, name, n);
    g_rf.name[n] = 0;
    atomic_fetch_add(&g_rf.requests, 1);
    *id = g_rf.id;
    return atomic_load(&g_rf.rc);
}
static void rf_status(live_rsxf_status *out)
{
    (void)atomic_load(&g_rf.st_seq);
    *out = g_rf.st;
}
static int rf_available(void) { return atomic_load(&g_rf.available); }
static const live_rsxf_ops g_rf_ops = {.request = rf_request, .status = rf_status, .available = rf_available};

static uint16_t start_rsxf_agent(live_config cfg, const live_rsxf_ops *ops)
{
    live_source src = {.path = g_path, .session_id = 0x1122334455667788ull, .app = "liveapp", .app_len = 7,
                       .build_id = "bid", .build_id_len = 3, .finalization = fin_get, .cmd = &g_fake_ops,
                       .rsxf = ops};
    CHECK(live_agent_start(&g_agent, &cfg, &src) == LIVE_OK);
    return live_agent_port(&g_agent);
}

static void send_rsxf(int fd, uint32_t req, uint32_t flags, uint32_t frames, const char *name, uint32_t reserved)
{
    uint8_t p[12 + LIVE_MAX_WINDOW_NAME];
    uint32_t n = (uint32_t)strlen(name);
    live_put32(p, req);
    live_put16(p + 4, flags);
    live_put16(p + 6, frames);
    live_put16(p + 8, n);
    live_put16(p + 10, reserved);
    memcpy(p + 12, name, n);
    cl_send(fd, LIVE_T_RSX_FRAME_REQ, 0, p, 12 + n);
}
static int has_rsxf_ack(client *c) { return c->rsxf_acks > 0; }
static int rf_want_states;
static int has_rsxf_states(client *c) { return c->rsxf_states >= rf_want_states; }
static void rf_set(uint32_t state, uint32_t draws, uint64_t frame)
{
    g_rf.st.state = state;
    g_rf.st.draws = draws;
    g_rf.st.frame = frame;
    atomic_fetch_add(&g_rf.st_seq, 1);
}

/* v3 with a capture source: HELLO carries LIVE_CAP_RSX_FRAME; a request is answered at once (16-byte ACK), then
 * one STATE per change (32 bytes: id, state, reason, draws, request, frame, bytes), none once it is final. */
static void case_rsxf_round_trip(void)
{
    w_open();
    w_append(100);
    atomic_store(&g_rf.available, 1);
    g_rf.id = 4;
    client *c = cl_open(start_rsxf_agent(test_cfg(), &g_rf_ops));
    cl_pump(c, 2000, has_hello);
    CHECK(c->hello && c->caps == (LIVE_CAP_TRIGGERS | LIVE_CAP_RSX_FRAME));
    send_rsxf(c->fd, 11, 0, 1, "frame-a", 0);
    cl_pump(c, 2000, has_rsxf_ack);
    CHECK(c->rsxf_acks == 1 && atomic_load(&g_rf.requests) == 1 && g_rf.req == 11 && g_rf.flags == 0);
    CHECK(strcmp(g_rf.name, "frame-a") == 0);
    CHECK(live_get32(c->rsxf_ack) == 11 && live_get32(c->rsxf_ack + 4) == 0 && live_get32(c->rsxf_ack + 8) == 4);
    /* a second request while the link still follows capture 4 (no final STATE yet): EBUSY from the agent itself,
     * the source is not asked, so capture 4's final state cannot be lost */
    c->rsxf_acks = 0;
    send_rsxf(c->fd, 13, 0, 1, "frame-b", 0);
    cl_pump(c, 2000, has_rsxf_ack);
    CHECK(c->rsxf_acks == 1 && live_get32(c->rsxf_ack) == 13);
    CHECK((int32_t)live_get32(c->rsxf_ack + 4) == PS3DK_PROF_EBUSY && live_get32(c->rsxf_ack + 8) == 0);
    CHECK(atomic_load(&g_rf.requests) == 1);
    g_rf.st.id = 4;
    g_rf.st.request = 11;
    g_rf.st.bytes = 777;
    rf_set(1, 0, 42);
    rf_want_states = 1;
    cl_pump(c, 2000, has_rsxf_states);
    CHECK(c->rsxf_states == 1 && live_get32(c->rsxf_state) == 4 && c->rsxf_state[4] == 1);
    CHECK(live_get32(c->rsxf_state + 12) == 11 && live_get64(c->rsxf_state + 16) == 42);
    rf_set(2, 22, 42);
    rf_want_states = 2;
    cl_pump(c, 2000, has_rsxf_states);
    CHECK(c->rsxf_states == 2 && c->rsxf_state[4] == 2 && live_get32(c->rsxf_state + 8) == 22);
    CHECK(live_get64(c->rsxf_state + 24) == 777);
    rf_set(3, 22, 42);
    rf_want_states = 3;
    cl_pump(c, 2000, has_rsxf_states);
    CHECK(c->rsxf_states == 3 && c->rsxf_state[4] == 3);
    rf_set(4, 22, 42); /* after a final state nothing more is sent for this capture */
    cl_pump(c, 300, NULL);
    CHECK(c->rsxf_states == 3 && !c->closed);
    /* a refusal: one ACK with the code and capture id 0, no STATE */
    c->rsxf_acks = 0;
    atomic_store(&g_rf.rc, PS3DK_PROF_EBUSY);
    send_rsxf(c->fd, 12, 0, 1, "", 0);
    cl_pump(c, 2000, has_rsxf_ack);
    CHECK((int32_t)live_get32(c->rsxf_ack + 4) == PS3DK_PROF_EBUSY && live_get32(c->rsxf_ack + 8) == 0);
    cl_pump(c, 300, NULL);
    CHECK(c->rsxf_states == 3);
    w_close();
    cl_free(c);
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

/* Not available (not built with the option, no ring): no capability bit, a request is refused ENOTSUP without
 * reaching the source; without a source at all the same. A v2 session never sees the bit and a request on it drops. */
static void case_rsxf_unavailable_and_v2(void)
{
    w_open();
    w_append(100);
    atomic_store(&g_rf.available, 0);
    uint16_t port = start_rsxf_agent(test_cfg(), &g_rf_ops);
    client *c = cl_open(port);
    cl_pump(c, 2000, has_hello);
    CHECK(c->caps == LIVE_CAP_TRIGGERS);
    send_rsxf(c->fd, 1, 0, 1, "x", 0);
    cl_pump(c, 2000, has_rsxf_ack);
    CHECK((int32_t)live_get32(c->rsxf_ack + 4) == PS3DK_PROF_ENOTSUP && atomic_load(&g_rf.requests) == 0);
    cl_free(c);
    atomic_store(&g_rf.available, 1);
    client *d = cl_open_v(port, 2);
    cl_pump(d, 2000, has_hello);
    CHECK(d->hello && d->caps == LIVE_CAP_TRIGGERS); /* v2: no RSX frame bit even when available */
    send_rsxf(d->fd, 2, 0, 1, "x", 0);
    CHECK(cl_wait_closed(d, 2000) && d->rsxf_acks == 0 && atomic_load(&g_rf.requests) == 0);
    CHECK(drops(LIVE_DROP_PROTOCOL) == 1);
    cl_free(d);
    w_close();
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

/* Every malformed request closes the link and reaches no source: request id 0, frames != 1, a reserved field, a
 * length that is not 12 + n. */
static void case_rsxf_malformed(void)
{
    w_open();
    w_append(100);
    atomic_store(&g_rf.available, 1);
    uint16_t port = start_rsxf_agent(test_cfg(), &g_rf_ops);
    unsigned long long expected = 0;
    for (int k = 0; k < 4; ++k) {
        client *c = cl_open(port);
        cl_pump(c, 2000, has_hello);
        if (k == 0) send_rsxf(c->fd, 0, 0, 1, "a", 0);
        if (k == 1) send_rsxf(c->fd, 1, 0, 2, "a", 0);
        if (k == 2) send_rsxf(c->fd, 1, 0, 1, "a", 1);
        if (k == 3) {
            uint8_t p[16] = {0};
            live_put32(p, 1);
            live_put16(p + 6, 1);
            live_put16(p + 8, 1); /* says 1 name byte, sends 4 */
            cl_send(c->fd, LIVE_T_RSX_FRAME_REQ, 0, p, 16);
        }
        CHECK(cl_wait_closed(c, 2000) && c->rsxf_acks == 0);
        CHECK(drops(LIVE_DROP_PROTOCOL) == ++expected);
        cl_free(c);
    }
    CHECK(atomic_load(&g_rf.requests) == 0);
    w_close();
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

/* Every malformed command closes the link (a protocol drop), and nothing reaches the source. */
static void malformed_one(uint16_t port, uint32_t type, const uint8_t *p, uint32_t len, int header_only)
{
    unsigned long long before = drops(LIVE_DROP_PROTOCOL);
    client *c = cl_open(port);
    cl_pump(c, 2000, has_hello);
    CHECK(c->hello);
    if (header_only) {
        uint8_t h[LIVE_HDR_BYTES];
        live_put16(h, type);
        live_put16(h + 2, 0);
        live_put32(h + 4, len);
        CHECK(send(c->fd, h, sizeof h, MSG_NOSIGNAL) == (ssize_t)sizeof h);
    } else {
        cl_send(c->fd, type, 0, p, len);
    }
    CHECK(cl_wait_closed(c, 2000) && !c->end && c->acks == 0);
    CHECK(drops(LIVE_DROP_PROTOCOL) == before + 1);
    cl_free(c);
}

static void case_cmd_malformed(void)
{
    w_open();
    w_append(100);
    uint16_t port = start_cmd_agent(test_cfg(), &g_fake_ops);
    uint8_t p[16 + LIVE_MAX_WINDOW_NAME];
    memset(p, 'a', sizeof p);
    int n = 0;
    /* BEGIN: request 0; reserved field; a name length that is not the payload's; lengths past the bounds */
    live_put32(p, 0); live_put16(p + 4, 1); live_put16(p + 6, 0);
    malformed_one(port, LIVE_T_CAPTURE_BEGIN, p, 9, 0); n++;
    live_put32(p, 1); live_put16(p + 4, 1); live_put16(p + 6, 1);
    malformed_one(port, LIVE_T_CAPTURE_BEGIN, p, 9, 0); n++;
    live_put32(p, 1); live_put16(p + 4, 2); live_put16(p + 6, 0);
    malformed_one(port, LIVE_T_CAPTURE_BEGIN, p, 9, 0); n++;
    malformed_one(port, LIVE_T_CAPTURE_BEGIN, NULL, 8, 1); n++;                        /* empty name */
    malformed_one(port, LIVE_T_CAPTURE_BEGIN, NULL, 8 + LIVE_MAX_WINDOW_NAME + 1, 1); n++; /* name too long */
    /* END: any length but 4; request 0 */
    malformed_one(port, LIVE_T_CAPTURE_END, NULL, 5, 1); n++;
    malformed_one(port, LIVE_T_CAPTURE_END, NULL, 0, 1); n++;
    live_put32(p, 0);
    malformed_one(port, LIVE_T_CAPTURE_END, p, 4, 0); n++;
    /* FRAMES: 0 frames, too many frames, reserved, name length mismatch, bounds */
    live_put32(p, 1); live_put32(p + 4, 0); live_put16(p + 8, 1); live_put16(p + 10, 0);
    malformed_one(port, LIVE_T_CAPTURE_FRAMES, p, 13, 0); n++;
    live_put32(p + 4, LIVE_MAX_WINDOW_FRAMES + 1);
    malformed_one(port, LIVE_T_CAPTURE_FRAMES, p, 13, 0); n++;
    live_put32(p + 4, 1); live_put16(p + 10, 1);
    malformed_one(port, LIVE_T_CAPTURE_FRAMES, p, 13, 0); n++;
    live_put16(p + 10, 0); live_put16(p + 8, 3);
    malformed_one(port, LIVE_T_CAPTURE_FRAMES, p, 13, 0); n++;
    malformed_one(port, LIVE_T_CAPTURE_FRAMES, NULL, 12, 1); n++;
    malformed_one(port, LIVE_T_CAPTURE_FRAMES, NULL, 12 + LIVE_MAX_WINDOW_NAME + 1, 1); n++;
    /* the agent's own frame types, and the ACK, from a viewer */
    malformed_one(port, LIVE_T_CAPTURE_ACK, NULL, LIVE_ACK_BYTES, 1); n++;
    malformed_one(port, 12, NULL, 0, 1); n++;
    CHECK(atomic_load(&g_cmd.submits) == 0);
    CHECK(drops(LIVE_DROP_PROTOCOL) == (unsigned long long)n);
    printf("MEASURE malformed commands refused: %d\n", n);
    w_close();
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

/* A second command before the first's ACK closes the link, and the queued one is withdrawn. */
static void case_cmd_overlap(void)
{
    w_open();
    w_append(100);
    client *c = cl_open(start_cmd_agent(test_cfg(), &g_fake_ops));
    cl_pump(c, 2000, has_hello);
    atomic_store(&g_cmd.cancel_rc, 1);
    send_begin(c->fd, 1, "first");
    send_end(c->fd, 2);
    CHECK(cl_wait_closed(c, 2000) && c->acks == 0);
    CHECK(drops(LIVE_DROP_PROTOCOL) == 1 && atomic_load(&g_cmd.submits) == 1 && atomic_load(&g_cmd.cancels) == 1);
    cl_free(c);
    w_close();
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

/* The viewer leaves with a command queued: the agent withdraws it. */
static void case_cmd_peer_close_cancels(void)
{
    w_open();
    w_append(100);
    client *c = cl_open(start_cmd_agent(test_cfg(), &g_fake_ops));
    cl_pump(c, 2000, has_hello);
    atomic_store(&g_cmd.cancel_rc, 1);
    send_begin(c->fd, 1, "orphan");
    uint64_t until = now_us() + 2000000;
    while (atomic_load(&g_cmd.submits) < 1 && now_us() < until)
        pal_sleep_us(1000);
    cl_free(c);
    until = now_us() + 2000000;
    while (atomic_load(&g_cmd.cancels) < 1 && now_us() < until)
        pal_sleep_us(1000);
    CHECK(atomic_load(&g_cmd.cancels) == 1 && drops(LIVE_DROP_PEER_CLOSED) == 1);
    w_close();
    CHECK(live_agent_stop(&g_agent, 0, 1000000) == LIVE_OK);
}

/* A command source with a missing function is a bad source. */
static void case_cmd_bad_source(void)
{
    live_config cfg = test_cfg();
    live_cmd_ops ops = g_fake_ops;
    ops.cancel = NULL;
    live_source src = {.path = g_path, .app = "a", .app_len = 1, .finalization = fin_get, .cmd = &ops};
    CHECK(live_agent_start(&g_agent, &cfg, &src) == LIVE_EINVAL);
    cfg.cmd_timeout_us = 999;
    src.cmd = &g_fake_ops;
    CHECK(live_agent_start(&g_agent, &cfg, &src) == LIVE_EINVAL);
    cfg.cmd_timeout_us = 60000001;
    CHECK(live_agent_start(&g_agent, &cfg, &src) == LIVE_EINVAL);
}

struct named { const char *name; void (*fn)(void); };
static const struct named cases[] = {
    {"end-with-unread-ping", case_end_with_unread_ping},
    {"end-drain-deadline", case_end_drain_deadline},
    {"end-drain-cancel", case_end_drain_cancel},
    {"end-drain-peer-fin", case_end_drain_peer_fin},
    {"late-join", case_late_join},
    {"grow-wait", case_grow_wait},
    {"failed-final", case_failed_final},
    {"cap", case_cap},
    {"start-errors", case_start_errors},
    {"handshake-timeout", case_handshake_timeout},
    {"bad-hello", case_bad_hello},
    {"unknown-type", case_unknown_type},
    {"oversize", case_oversize},
    {"nonzero-flags", case_nonzero_flags},
    {"reserved-type", case_reserved_type},
    {"ping", case_ping},
    {"ping-flood", case_ping_flood},
    {"second-client-busy", case_second_client_busy},
    {"reconnect-after-error", case_reconnect_after_error},
    {"stalled-viewer", case_stalled_viewer},
    {"stop-graceful", case_stop_graceful},
    {"stop-cancel", case_stop_cancel},
    {"stop-timeout", case_stop_timeout},
    {"late-hello", case_late_hello},
    {"stop-overflow", case_stop_overflow},
    {"clock-fail", case_clock_fail},
    {"join-fail", case_join_fail},
    {"busy-nonblock-fail", case_busy_nonblock_fail},
    {"busy-send-eagain", case_busy_send_eagain},
    {"delivered-counts-sent-frames", case_delivered_counts_sent_frames},
    {"held-pread-data", case_held_pread_data},
    {"held-pread-eof", case_held_pread_eof},
    {"cmd-round-trip", case_cmd_round_trip},
    {"cmd-refused-at-submit", case_cmd_refused_at_submit},
    {"cmd-timeout", case_cmd_timeout},
    {"cmd-no-source", case_cmd_no_source},
    {"cmd-v1-session", case_cmd_v1_session},
    {"cmd-bad-versions", case_cmd_bad_versions},
    {"cmd-malformed", case_cmd_malformed},
    {"cmd-overlap", case_cmd_overlap},
    {"cmd-peer-close-cancels", case_cmd_peer_close_cancels},
    {"cmd-bad-source", case_cmd_bad_source},
    {"rsxf-round-trip", case_rsxf_round_trip},
    {"rsxf-unavailable-and-v2", case_rsxf_unavailable_and_v2},
    {"rsxf-malformed", case_rsxf_malformed},
};

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
            puts(cases[i].name);
        return 0;
    }
    CHECK(argc == 3 && strlen(argv[2]) < sizeof g_dir);
    snprintf(g_dir, sizeof g_dir, "%s", argv[2]);
    snprintf(g_path, sizeof g_path, "%s/cap.prof", g_dir);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
        if (strcmp(argv[1], cases[i].name) == 0) {
            cases[i].fn();
            printf("PASS %s\n", argv[1]);
            return 0;
        }
    fprintf(stderr, "unknown case %s\n", argv[1]);
    return 2;
}
