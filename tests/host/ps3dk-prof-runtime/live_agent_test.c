/* Live link agent over loopback: one named case per run.
 * usage: live-agent-test <case> <empty scratch dir>
 * The test is both the capture writer (a file it grows) and a strict
 * viewer (frame order, contiguous offsets, END fields). The finalization
 * record is injected, as the runtime will publish it after close/join. */
#define _GNU_SOURCE
#include "live_agent.h"
#include "prof_test_hooks.h"
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
    int hello, end, busy, closed, pongs;
    uint32_t busy_reason;
    uint8_t endp[LIVE_END_BYTES];
    uint64_t session;
    char name[LIVE_MAX_NAME + 1];
    uint8_t rx[LIVE_HDR_BYTES + LIVE_MAX_PAYLOAD];
    uint32_t rx_len;
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

static void cl_hello(int fd, uint32_t magic)
{
    uint8_t p[8];
    live_put32(p, magic);
    live_put16(p + 4, LIVE_VERSION);
    live_put16(p + 6, 0);
    cl_send(fd, LIVE_T_CLIENT_HELLO, 0, p, 8);
}

static client *cl_open(uint16_t port)
{
    client *c = calloc(1, sizeof *c);
    CHECK(c);
    c->fd = cl_connect(port);
    cl_hello(c->fd, LIVE_MAGIC);
    return c;
}

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
        CHECK(len >= 16 + 6 && live_get16(p) == LIVE_VERSION);
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
    w_close();
    publish(50000, 1, 0);
    uint16_t port = start_agent(test_cfg());
    client *a = cl_open(port);
    cl_pump(a, 2000, has_hello);
    cl_send(a->fd, 99, 0, NULL, 0);
    CHECK(cl_wait_closed(a, 2000));
    client *b = cl_open(port); /* the agent is back in LISTEN */
    cl_pump(b, 3000, NULL);
    CHECK(b->end);
    check_bytes(b, 50000); /* a new candidate from offset 0 */
    client *late = cl_open(port); /* after a completed stream, still served */
    cl_pump(late, 3000, NULL);
    CHECK(late->end);
    check_bytes(late, 50000);
    cl_free(a);
    cl_free(b);
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

struct named { const char *name; void (*fn)(void); };
static const struct named cases[] = {
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
