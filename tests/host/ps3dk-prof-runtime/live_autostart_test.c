/* The opt-in live viewer auto-start (live/live_autostart.c) over the real runtime, the live agent, the host OS shim
 * and libnet's real holder layer (sdk/libnet/src/net_hold.c) on a modelled firmware network: the agent starts with
 * no live call, a raw viewer gets the whole capture and END, the network hold is released only after END and only
 * the hold the profiler took; plus every off path, the explicit-start rule and the exit path.  One case per process.
 * Lines the runtime prints go to stderr; the driver (live-autostart-test.py) checks them per case.
 * usage: live-autostart-test --list | <case> <scratch dir> */
#define _GNU_SOURCE
#include <ps3dk/prof.h>
#include "live_api.h"
#include "live_proto.h"
#include "net_hold.h"
#include "prof_test_hooks.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define CHECK(c)                                                                                                    \
    do {                                                                                                            \
        if (!(c)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                            \
            exit(1);                                                                                                \
        }                                                                                                           \
    } while (0)

/* ---- the modelled firmware network under libnet's holders --------------------------------------------------------- */

struct wire_init { uint32_t memory, size; int32_t flags; };
static atomic_int net_up, inits, finals, acquires, releases, acquire_result, block_acquire, acquire_entered,
    acquire_go, end_seen_at_release;

int __ps3dk_raw_sys_net_initialize_network_ex(const struct wire_init *w)
{
    (void)w;
    atomic_fetch_add(&inits, 1);
    return atomic_exchange(&net_up, 1) ? -0x7fffff01 : 0;
}

int __ps3dk_raw_sys_net_finalize_network(void)
{
    atomic_fetch_add(&finals, 1);
    return atomic_exchange(&net_up, 0) ? 0 : -0x7fffff02;
}

static int prof_acquire(void *pool, uint32_t bytes)
{
    atomic_fetch_add(&acquires, 1);
    if (atomic_load(&block_acquire)) {
        atomic_store(&acquire_entered, 1);
        while (!atomic_load(&acquire_go))
            usleep(1000);
    }
    if (atomic_load(&acquire_result))
        return atomic_load(&acquire_result);
    if (!pool)
        pool = __ps3dk_net_default_pool(&bytes);
    return __ps3dk_net_hold_acquire(PS3DK_NET_HOLD_PROF, pool, bytes, 0, NULL, NULL);
}

static int prof_release(void)
{
    const live_agent *a = ps3dk_prof__live_agent();
    atomic_store(&end_seen_at_release, (int)atomic_load(&a->drops[LIVE_DROP_END]));
    atomic_fetch_add(&releases, 1);
    int rc = __ps3dk_net_hold_release(PS3DK_NET_HOLD_PROF);
    char line[96];
    int n = snprintf(line, sizeof line, "NET_RELEASE rc=%d net_up=%d\n", rc, atomic_load(&net_up));
    (void)!write(1, line, (size_t)n); /* also from the exit destructor, after main returned */
    return rc;
}

/* The ps3dk-run hook's stand-in: it holds the network as netInitialize does (the legacy holder) and keeps a connected
 * socket that must still work after the profiler is gone. */
static int hook_fd[2] = {-1, -1};
static void hook_up(void)
{
    uint32_t bytes;
    void *pool = __ps3dk_net_default_pool(&bytes);
    CHECK(__ps3dk_net_hold_acquire(PS3DK_NET_HOLD_LEGACY, pool, bytes, 0, NULL, NULL) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, hook_fd) == 0);
}
static void hook_exit_works(void)
{
    char c = 0;
    CHECK(atomic_load(&net_up)); /* the network the hook's EXIT needs is still initialized */
    CHECK(send(hook_fd[0], "X", 1, MSG_NOSIGNAL) == 1 && recv(hook_fd[1], &c, 1, 0) == 1 && c == 'X');
}

/* ---- runtime helpers ------------------------------------------------------------------------------------------- */

static void config(ps3dk_prof_config *c, uint32_t mode)
{
    ps3dk_prof_config_defaults(c);
    c->mode = mode;
    c->app = "liveauto";
    c->build_id = "live-autostart-build";
    c->max_threads = 4;
    c->max_descriptors = 16;
    c->ring_records = 4096;
}

static void hooks_on(void)
{
    ps3dk_prof_test_hooks.live_ephemeral_port = 1;
    ps3dk_prof_test_hooks.live_net_acquire = prof_acquire;
    ps3dk_prof_test_hooks.live_net_release = prof_release;
}

static void file_init(const char *dir, uint32_t mode)
{
    ps3dk_prof_config c;
    config(&c, mode);
    CHECK(ps3dk_prof_set_sink_path(dir) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_init(&c) == PS3DK_PROF_OK);
}

static uint16_t wait_port(void)
{
    for (int i = 0; i < 5000; ++i) {
        uint16_t p = ps3dk_prof__live_port();
        if (p)
            return p;
        usleep(1000);
    }
    return 0;
}

/* The start-up thread has finished: the live lock is free and its outcome is visible. */
static void settle(void)
{
    usleep(20000);
    ps3dk_prof__live_lock();
    ps3dk_prof__live_unlock();
}

static void some_zones(int n)
{
    for (int i = 0; i < n; ++i) {
        PS3DK_PROF_ZONE("auto-zone");
        PS3DK_PROF_FRAME();
    }
}

/* ---- a raw protocol client (as live_api_test.c) --------------------------------------------------------------- */

struct client {
    int fd;
    unsigned char *data;
    uint64_t data_len;
    int got_hello, got_end, ok;
    unsigned char end[LIVE_END_BYTES];
};

static int read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, (char *)buf + got, n - got, 0);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

static int connect_client(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct timeval tv = {.tv_sec = 10};
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0);
    unsigned char h[LIVE_HDR_BYTES + 8];
    live_put16(h, LIVE_T_CLIENT_HELLO);
    live_put16(h + 2, 0);
    live_put32(h + 4, 8);
    live_put32(h + 8, LIVE_MAGIC);
    live_put16(h + 12, LIVE_VERSION);
    live_put16(h + 14, 0);
    CHECK(send(fd, h, sizeof h, MSG_NOSIGNAL) == (ssize_t)sizeof h);
    return fd;
}

static void *client_main(void *arg)
{
    struct client *c = arg;
    unsigned char hdr[LIVE_HDR_BYTES];
    static unsigned char payload[LIVE_MAX_PAYLOAD];
    for (;;) {
        if (read_full(c->fd, hdr, sizeof hdr) != 0)
            return NULL;
        uint32_t type = live_get16(hdr), len = live_get32(hdr + 4);
        if (live_get16(hdr + 2) != 0 || len > LIVE_MAX_PAYLOAD || read_full(c->fd, payload, len) != 0)
            return NULL;
        if (type == LIVE_T_HELLO) {
            c->got_hello = 1;
        } else if (type == LIVE_T_DATA) {
            uint64_t off = live_get64(payload);
            if (len < 8 || off != c->data_len || c->data_len + (len - 8) > (64u << 20))
                return NULL;
            memcpy(c->data + off, payload + 8, len - 8);
            c->data_len += len - 8;
        } else if (type == LIVE_T_END) {
            if (len != LIVE_END_BYTES)
                return NULL;
            memcpy(c->end, payload, len);
            c->got_end = c->ok = 1;
            return NULL;
        } else {
            return NULL;
        }
    }
}

static unsigned char *read_file(const char *path, uint64_t *len)
{
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    CHECK(fseek(f, 0, SEEK_END) == 0);
    long n = ftell(f);
    CHECK(n >= 0 && fseek(f, 0, SEEK_SET) == 0);
    unsigned char *b = malloc((size_t)n + 1);
    CHECK(b && fread(b, 1, (size_t)n, f) == (size_t)n && fclose(f) == 0);
    *len = (uint64_t)n;
    return b;
}

/* ---- cases ----------------------------------------------------------------------------------------------------- */

/* No live call at all: the agent is up after init, a viewer streams the whole capture, shutdown delivers END, and the
 * profiler's network hold is released after END while the hook's network and socket survive.  THREAD and APP mode. */
static void stream(const char *dir, uint32_t mode)
{
    hooks_on();
    hook_up();
    file_init(dir, mode);
    uint16_t port = wait_port();
    CHECK(port != 0);
    CHECK(atomic_load(&acquires) == 1 && atomic_load(&inits) == 1); /* joined the hook's network: no second init */
    struct client c = {.data = calloc(64u << 20, 1)};
    CHECK(c.data != NULL);
    c.fd = connect_client(port);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, client_main, &c) == 0);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    for (int round = 0; round < 20; ++round) {
        some_zones(200);
        CHECK(ps3dk_prof_drain() == PS3DK_PROF_OK || mode == PS3DK_PROF_MODE_THREAD);
        usleep(10000);
    }
    CHECK(atomic_load(&releases) == 0);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK); /* the hook stops the agent after END */
    CHECK(atomic_load(&releases) == 1 && atomic_load(&end_seen_at_release) == 1);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(c.ok && c.got_hello && c.got_end);
    ps3dk_prof_capture_record rec;
    CHECK(ps3dk_prof_get_capture_record(&rec) == PS3DK_PROF_OK);
    uint64_t flen;
    unsigned char *file = read_file(rec.path, &flen);
    CHECK(flen > 0 && c.data_len == flen && memcmp(c.data, file, flen) == 0);
    CHECK((int32_t)live_get32(c.end + 4) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_live_stop(0, 0) == PS3DK_PROF_EALREADY); /* stopped by the runtime */
    ps3dk_prof_live_stats s;
    CHECK(ps3dk_prof_live_get_stats(&s) == PS3DK_PROF_OK && s.connections == 1 && s.drop_end == 1);
    CHECK(atomic_load(&finals) == 0); /* the hook still holds the network */
    hook_exit_works();
    printf("MEASURE stream bytes=%llu\n", (unsigned long long)flen);
    free(file);
    free(c.data);
    CHECK(close(c.fd) == 0);
}
static void case_stream_app(const char *dir) { stream(dir, PS3DK_PROF_MODE_APP); }
static void case_stream_thread(const char *dir) { stream(dir, PS3DK_PROF_MODE_THREAD); }

/* The profiler is the only holder: it brings the network up and finalizes it after the stop. */
static void case_sole_holder(const char *dir)
{
    hooks_on();
    file_init(dir, PS3DK_PROF_MODE_APP);
    CHECK(wait_port() != 0);
    CHECK(atomic_load(&inits) == 1 && atomic_load(&net_up));
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    some_zones(50);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    CHECK(atomic_load(&releases) == 1 && atomic_load(&finals) == 1 && !atomic_load(&net_up));
}

/* The app initializes and tears down networking itself, before and after the profiler: its teardown no longer pulls
 * the network from under the agent; the last release (the profiler's) finalizes it. */
static void case_app_net_around(const char *dir)
{
    hooks_on();
    uint32_t bytes;
    void *pool = __ps3dk_net_default_pool(&bytes);
    CHECK(__ps3dk_net_hold_acquire(PS3DK_NET_HOLD_LEGACY, pool, bytes, 0, NULL, NULL) == 0); /* netInitialize */
    file_init(dir, PS3DK_PROF_MODE_APP);
    CHECK(wait_port() != 0);
    CHECK(__ps3dk_net_hold_release(PS3DK_NET_HOLD_LEGACY) == 0 && atomic_load(&net_up)); /* netDeinitialize */
    int fd = connect_client(ps3dk_prof__live_port()); /* still served */
    CHECK(close(fd) == 0);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    CHECK(atomic_load(&finals) == 1 && !atomic_load(&net_up));
}

/* The network cannot be brought up: one line, no agent, profiling unchanged, nothing released; an app that then
 * brings up its own network can still start the live link itself. */
static void case_net_unavailable(const char *dir)
{
    hooks_on();
    atomic_store(&acquire_result, (int)0x80010224u);
    file_init(dir, PS3DK_PROF_MODE_APP);
    settle();
    CHECK(atomic_load(&acquires) == 1 && ps3dk_prof__live_port() == 0);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    some_zones(50);
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_OK); /* the app's own start, as without auto-start */
    CHECK(ps3dk_prof_live_stop(0, 2000000) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    CHECK(atomic_load(&releases) == 0);
}

/* The listener cannot be created: one line, the hold taken for it is given back at once. */
static void case_listen_fails(const char *dir)
{
    hooks_on();
    int blocker = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t sl = sizeof sa;
    CHECK(blocker >= 0 && bind(blocker, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(blocker, 1) == 0);
    CHECK(getsockname(blocker, (struct sockaddr *)&sa, &sl) == 0);
    ps3dk_prof_test_hooks.live_listen_port = ntohs(sa.sin_port);
    file_init(dir, PS3DK_PROF_MODE_APP);
    settle();
    CHECK(ps3dk_prof__live_port() == 0);
    CHECK(atomic_load(&acquires) == 1 && atomic_load(&releases) == 1 && !atomic_load(&net_up));
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    CHECK(atomic_load(&releases) == 1);
    CHECK(close(blocker) == 0);
}

static long cb_write(void *ctx, const void *buf, unsigned long n)
{
    (void)ctx, (void)buf;
    return (long)n;
}
static int cb_flush(void *ctx) { (void)ctx; return 0; }
static int cb_close(void *ctx) { (void)ctx; return 0; }

/* A capture the agent cannot serve: one line, and the network is never touched. */
static void case_not_servable(const char *dir)
{
    (void)dir;
    hooks_on();
    ps3dk_prof_config c;
    config(&c, PS3DK_PROF_MODE_APP);
    ps3dk_prof_sink_callbacks cb = {cb_write, cb_flush, cb_close, NULL};
    CHECK(ps3dk_prof_set_sink_callbacks(&cb) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_init(&c) == PS3DK_PROF_OK);
    settle();
    CHECK(atomic_load(&acquires) == 0);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    CHECK(atomic_load(&releases) == 0);
}

/* The start-up thread cannot be created: one line, init itself succeeds. */
static void case_thread_fail(const char *dir)
{
    hooks_on();
    ps3dk_prof_test_hooks.thread_create_fail = 1; /* APP mode creates no drain thread: only the start-up thread */
    file_init(dir, PS3DK_PROF_MODE_APP);
    ps3dk_prof_test_hooks.thread_create_fail = 0;
    settle();
    CHECK(atomic_load(&acquires) == 0 && ps3dk_prof__live_port() == 0);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
}

/* An existing explicit live_start caller: OK (a no-op) when it matches, EALREADY otherwise; its explicit stop is
 * honoured and the runtime then only releases its hold. */
static void case_explicit_compat(const char *dir)
{
    hooks_on();
    file_init(dir, PS3DK_PROF_MODE_APP);
    uint16_t port = wait_port();
    CHECK(port != 0);
    ps3dk_prof_live_config lc;
    memset(&lc, 0, sizeof lc);
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_live_start(&lc) == PS3DK_PROF_OK);
    lc.port = 31750; /* the auto-start's requested port (the test binds an ephemeral one) */
    CHECK(ps3dk_prof_live_start(&lc) == PS3DK_PROF_OK);
    lc.port = 31751;
    CHECK(ps3dk_prof_live_start(&lc) == PS3DK_PROF_EALREADY);
    memset(&lc, 0, sizeof lc);
    lc.poll_us = 5000;
    CHECK(ps3dk_prof_live_start(&lc) == PS3DK_PROF_EALREADY);
    memset(&lc, 0, sizeof lc);
    lc.reserved = 1;
    CHECK(ps3dk_prof_live_start(&lc) == PS3DK_PROF_EALREADY);
    CHECK(ps3dk_prof__live_port() == port); /* nothing restarted */
    CHECK(ps3dk_prof_live_stop(0, 2000000) == PS3DK_PROF_OK); /* the app may stop it */
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_EALREADY); /* once per run, as before */
    CHECK(atomic_load(&releases) == 0);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    CHECK(atomic_load(&releases) == 1 && !atomic_load(&net_up));
}

/* The app starts the live link before the start-up thread gets the lock: the runtime leaves it alone, takes no hold,
 * and stops nothing at shutdown; the app keeps its own teardown. */
static void case_app_first(const char *dir)
{
    hooks_on();
    ps3dk_prof__live_lock(); /* the start-up thread waits here */
    file_init(dir, PS3DK_PROF_MODE_APP);
    CHECK(ps3dk_prof__live_start_locked(NULL, 0) == PS3DK_PROF_OK); /* what ps3dk_prof_live_start does inside */
    ps3dk_prof__live_unlock();
    settle();
    CHECK(atomic_load(&acquires) == 0);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_live_stop(1000000, 1000000) == PS3DK_PROF_OK); /* still the app's to stop */
    CHECK(atomic_load(&releases) == 0);
}

struct start_arg {
    int rc;
    atomic_int done;
};
static void *explicit_start(void *arg)
{
    struct start_arg *s = arg;
    s->rc = ps3dk_prof_live_start(NULL);
    atomic_store(&s->done, 1);
    return NULL;
}

/* An explicit start during the bring-up waits for its outcome, then is the no-op OK. */
static void case_start_during_bringup(const char *dir)
{
    hooks_on();
    atomic_store(&block_acquire, 1);
    file_init(dir, PS3DK_PROF_MODE_APP);
    for (int i = 0; i < 5000 && !atomic_load(&acquire_entered); ++i)
        usleep(1000);
    CHECK(atomic_load(&acquire_entered));
    struct start_arg s = {.rc = -100};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, explicit_start, &s) == 0);
    usleep(50000);
    CHECK(!atomic_load(&s.done)); /* waiting for the bring-up */
    atomic_store(&acquire_go, 1);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(s.rc == PS3DK_PROF_OK && ps3dk_prof__live_port() != 0);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    CHECK(atomic_load(&releases) == 1);
}

static void *shutdown_main(void *arg)
{
    *(int *)arg = ps3dk_prof_shutdown();
    return NULL;
}

/* Shutdown while the bring-up is inside the network call: the shutdown waits for the start-up thread, which then
 * sees the shutdown, starts no agent and gives its hold back. */
static void case_shutdown_during_bringup(const char *dir)
{
    hooks_on();
    atomic_store(&block_acquire, 1);
    file_init(dir, PS3DK_PROF_MODE_THREAD);
    for (int i = 0; i < 5000 && !atomic_load(&acquire_entered); ++i)
        usleep(1000);
    CHECK(atomic_load(&acquire_entered));
    int rc = -100;
    pthread_t t;
    CHECK(pthread_create(&t, NULL, shutdown_main, &rc) == 0);
    usleep(50000);
    atomic_store(&acquire_go, 1);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(rc == PS3DK_PROF_OK);
    CHECK(ps3dk_prof__live_port() == 0);
    CHECK(atomic_load(&acquires) == 1 && atomic_load(&releases) == 1 && !atomic_load(&net_up));
}

/* The agent does not stop (held inside a file read): the hold is kept (its thread may still use a socket), one line;
 * the shutdown itself succeeds. */
static void case_stop_timeout_keeps_hold(const char *dir)
{
    hooks_on();
    file_init(dir, PS3DK_PROF_MODE_APP);
    uint16_t port = wait_port();
    CHECK(port != 0);
    live_test_hooks *h = &ps3dk_prof_test_live_hooks;
    atomic_store(&h->pread_block, 1);
    struct client c = {.data = calloc(64u << 20, 1)};
    CHECK(c.data != NULL);
    c.fd = connect_client(port);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, client_main, &c) == 0);
    for (int i = 0; i < 5000 && !atomic_load(&h->pread_entered); ++i)
        usleep(1000);
    CHECK(atomic_load(&h->pread_entered));
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    CHECK(atomic_load(&releases) == 0 && atomic_load(&net_up));
    atomic_store(&h->pread_release, 1);
    CHECK(ps3dk_prof_live_stop(0, 2000000) == PS3DK_PROF_OK); /* the app may finish the stop */
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(close(c.fd) == 0);
    free(c.data);
}

/* A manual-lifecycle program that returns from main without shutdown: the exit destructor cancels the agent and
 * releases the hold (the driver checks the NET_RELEASE line printed after PASS). */
static void case_exit_without_shutdown(const char *dir)
{
    hooks_on();
    hook_up();
    file_init(dir, PS3DK_PROF_MODE_APP);
    CHECK(wait_port() != 0);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    some_zones(20);
    CHECK(atomic_load(&releases) == 0);
}

static const struct {
    const char *name;
    void (*fn)(const char *dir);
} cases[] = {
    {"stream_app", case_stream_app},
    {"stream_thread", case_stream_thread},
    {"sole_holder", case_sole_holder},
    {"app_net_around", case_app_net_around},
    {"net_unavailable", case_net_unavailable},
    {"listen_fails", case_listen_fails},
    {"not_servable", case_not_servable},
    {"thread_fail", case_thread_fail},
    {"explicit_compat", case_explicit_compat},
    {"app_first", case_app_first},
    {"start_during_bringup", case_start_during_bringup},
    {"shutdown_during_bringup", case_shutdown_during_bringup},
    {"stop_timeout_keeps_hold", case_stop_timeout_keeps_hold},
    {"exit_without_shutdown", case_exit_without_shutdown},
};

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--list")) {
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
            puts(cases[i].name);
        return 0;
    }
    for (size_t i = 0; argc == 3 && i < sizeof cases / sizeof cases[0]; ++i)
        if (!strcmp(argv[1], cases[i].name)) {
            cases[i].fn(argv[2]);
            printf("PASS %s\n", cases[i].name);
            return 0;
        }
    fprintf(stderr, "usage: %s --list | <case> <dir>\n", argv[0]);
    return 2;
}
