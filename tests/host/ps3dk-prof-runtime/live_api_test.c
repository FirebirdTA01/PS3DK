/* The public live API (ps3dk_prof_live_start/stop, live_api.c) over the real
 * runtime, the agent and the host OS shim: every refusal, the shutdown-claim
 * interval, a full stream to a raw protocol client with END checked against
 * the file the runtime wrote, and a cancelling stop. One case per process
 * (the agent is single-use per run).
 * usage: live-api-test --list | <case> <scratch dir> */
#define _GNU_SOURCE
#include <ps3dk/prof.h>
#include "live_api.h"
#include "live_path.h"
#include "live_proto.h"
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

#define CHECK(c)                                                                   \
    do {                                                                           \
        if (!(c)) {                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
            exit(1);                                                               \
        }                                                                          \
    } while (0)

static void config(ps3dk_prof_config *c, uint32_t mode)
{
    ps3dk_prof_config_defaults(c);
    c->mode = mode;
    c->app = "liveapi";
    c->build_id = "live-api-build";
    c->max_threads = 4;
    c->max_descriptors = 16;
    c->ring_records = 4096;
}

static void file_init(const char *dir, uint32_t mode)
{
    ps3dk_prof_config c;
    config(&c, mode);
    CHECK(ps3dk_prof_set_sink_path(dir) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_init(&c) == PS3DK_PROF_OK);
}

static void some_zones(int n)
{
    for (int i = 0; i < n; ++i) {
        PS3DK_PROF_ZONE("api-zone");
        PS3DK_PROF_FRAME();
    }
}

/* ---- refusals ------------------------------------------------------------------------------------------------------ */

static int case_before_init(const char *dir)
{
    (void)dir;
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_ENOTINIT);
    CHECK(ps3dk_prof_live_stop(0, 0) == PS3DK_PROF_ENOTINIT);
    CHECK(ps3dk_prof__live_port() == 0);
    return 0;
}

static long cb_write(void *ctx, const void *buf, unsigned long n)
{
    (void)ctx, (void)buf;
    return (long)n;
}
static int cb_flush(void *ctx) { (void)ctx; return 0; }
static int cb_close(void *ctx) { (void)ctx; return 0; }

static int case_callback_sink(const char *dir)
{
    (void)dir;
    ps3dk_prof_config c;
    config(&c, PS3DK_PROF_MODE_APP);
    ps3dk_prof_sink_callbacks cb = {cb_write, cb_flush, cb_close, NULL};
    CHECK(ps3dk_prof_set_sink_callbacks(&cb) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_init(&c) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_ENOTSUP);
    CHECK(ps3dk_prof_live_stop(0, 0) == PS3DK_PROF_ENOTINIT);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    return 0;
}

/* Rejected configs do not use up the one start. */
static int case_bad_config(const char *dir)
{
    file_init(dir, PS3DK_PROF_MODE_APP);
    ps3dk_prof_test_hooks.live_ephemeral_port = 1;
    ps3dk_prof_live_config lc;
    memset(&lc, 0, sizeof lc);
    lc.reserved = 1;
    CHECK(ps3dk_prof_live_start(&lc) == PS3DK_PROF_EINVAL);
    memset(&lc, 0, sizeof lc);
    lc.reserved2 = 1;
    CHECK(ps3dk_prof_live_start(&lc) == PS3DK_PROF_EINVAL);
    memset(&lc, 0, sizeof lc);
    lc.poll_us = 5; /* below 1000 */
    CHECK(ps3dk_prof_live_start(&lc) == PS3DK_PROF_EINVAL);
    memset(&lc, 0, sizeof lc);
    lc.stack_bytes = 2u << 20; /* above 1 MiB */
    CHECK(ps3dk_prof_live_start(&lc) == PS3DK_PROF_EINVAL);
    CHECK(ps3dk_prof__live_port() == 0);
    memset(&lc, 0, sizeof lc); /* all zero: every default */
    CHECK(ps3dk_prof_live_start(&lc) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof__live_port() != 0);
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_EALREADY);
    CHECK(ps3dk_prof_live_stop(0, 2000000) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_live_stop(0, 0) == PS3DK_PROF_EALREADY);
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_EALREADY); /* once per run */
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    return 0;
}

/* ---- the shutdown-claim interval ------------------------------------------------------------------------------------ */

static atomic_int g_in_claim, g_release_claim;

static void hold_claim(void)
{
    atomic_store(&g_in_claim, 1);
    for (int i = 0; i < 5000 && !atomic_load(&g_release_claim); ++i)
        usleep(1000);
}

static void *shutdown_thread(void *arg)
{
    *(int *)arg = ps3dk_prof_shutdown();
    return NULL;
}

/* Shutdown has won its claim but not yet set the stop latch: start is already ESTOPPED. */
static int case_shutdown_claim_interval(const char *dir)
{
    file_init(dir, PS3DK_PROF_MODE_THREAD);
    ps3dk_prof_test_hooks.after_shutdown_claim = hold_claim;
    int rc = -100;
    pthread_t t;
    CHECK(pthread_create(&t, NULL, shutdown_thread, &rc) == 0);
    for (int i = 0; i < 5000 && !atomic_load(&g_in_claim); ++i)
        usleep(1000);
    CHECK(atomic_load(&g_in_claim));
    CHECK(ps3dk_prof_test_stat(PS3DK_PROF_TEST_STAT_LATCH) == 0); /* the interval: claimed, not latched */
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_ESTOPPED);
    atomic_store(&g_release_claim, 1);
    CHECK(pthread_join(t, NULL) == 0);
    ps3dk_prof_test_hooks.after_shutdown_claim = NULL;
    CHECK(rc == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_ESTOPPED);
    CHECK(ps3dk_prof_live_stop(0, 0) == PS3DK_PROF_ENOTINIT);
    return 0;
}

static int case_after_shutdown(const char *dir)
{
    file_init(dir, PS3DK_PROF_MODE_APP);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    some_zones(10);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_ESTOPPED);
    return 0;
}

/* ---- a raw protocol client ------------------------------------------------------------------------------------------ */

struct client {
    int fd;
    unsigned char *data;  /* every DATA payload, at its offset */
    uint64_t data_len;
    int got_hello, got_end;
    unsigned char end[LIVE_END_BYTES];
    int ok;
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

static int connect_client(uint16_t port, int send_hello)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct timeval tv = {.tv_sec = 10};
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0);
    if (send_hello) {
        unsigned char h[LIVE_HDR_BYTES + 8];
        live_put16(h, LIVE_T_CLIENT_HELLO);
        live_put16(h + 2, 0);
        live_put32(h + 4, 8);
        live_put32(h + 8, LIVE_MAGIC);
        live_put16(h + 12, LIVE_VERSION);
        live_put16(h + 14, 0);
        CHECK(send(fd, h, sizeof h, MSG_NOSIGNAL) == (ssize_t)sizeof h);
    }
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
            if (len < 8)
                return NULL;
            uint64_t off = live_get64(payload);
            if (off != c->data_len || c->data_len + (len - 8) > (64u << 20))
                return NULL;
            memcpy(c->data + off, payload + 8, len - 8);
            c->data_len += len - 8;
        } else if (type == LIVE_T_END) {
            if (len != LIVE_END_BYTES)
                return NULL;
            memcpy(c->end, payload, len);
            c->got_end = 1;
            c->ok = 1;
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

/* The whole path: start before the work, a viewer from the start, the capture growing, then shutdown, END and stop. */
static int case_stream(const char *dir)
{
    file_init(dir, PS3DK_PROF_MODE_APP);
    ps3dk_prof_test_hooks.live_ephemeral_port = 1;
    ps3dk_prof_live_config lc;
    memset(&lc, 0, sizeof lc);
    lc.poll_us = 2000;
    CHECK(ps3dk_prof_live_start(&lc) == PS3DK_PROF_OK);
    uint16_t port = ps3dk_prof__live_port();
    CHECK(port != 0);
    CHECK(ps3dk_prof_live_start(&lc) == PS3DK_PROF_EALREADY);

    struct client c = {.data = calloc(64u << 20, 1)};
    CHECK(c.data != NULL);
    c.fd = connect_client(port, 1);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, client_main, &c) == 0);

    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    for (int round = 0; round < 40; ++round) {
        some_zones(200);
        CHECK(ps3dk_prof_drain() == PS3DK_PROF_OK);
        usleep(10000);
    }
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(c.ok && c.got_hello && c.got_end);

    ps3dk_prof_capture_record rec;
    CHECK(ps3dk_prof_get_capture_record(&rec) == PS3DK_PROF_OK);
    uint64_t flen;
    unsigned char *file = read_file(rec.path, &flen);
    CHECK(flen > 0 && c.data_len == flen && memcmp(c.data, file, flen) == 0);
    CHECK(c.end[0] == 1 && c.end[1] == 1 && c.end[2] == 0);              /* io finalized, trailer, no join failure */
    CHECK((int32_t)live_get32(c.end + 4) == PS3DK_PROF_OK);              /* shutdown result */
    CHECK(live_get64(c.end + 8) == flen && live_get64(c.end + 16) == flen); /* accepted and sent bytes */

    const live_agent *a = ps3dk_prof__live_agent();
    CHECK(atomic_load(&a->drops[LIVE_DROP_END]) == 1 && atomic_load(&a->bytes_delivered) == flen);
    CHECK(ps3dk_prof_live_stop(1000000, 1000000) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_live_stop(0, 0) == PS3DK_PROF_EALREADY);
    CHECK(close(c.fd) == 0);
    printf("MEASURE stream bytes=%llu\n", (unsigned long long)flen);
    free(file);
    free(c.data);
    return 0;
}

/* A viewer is streaming but the run has not shut down: a stop with no graceful time cancels it, without END. */
static int case_stop_cancels(const char *dir)
{
    file_init(dir, PS3DK_PROF_MODE_APP);
    ps3dk_prof_test_hooks.live_ephemeral_port = 1;
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_OK);
    struct client c = {.data = calloc(64u << 20, 1)};
    CHECK(c.data != NULL);
    c.fd = connect_client(ps3dk_prof__live_port(), 1);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, client_main, &c) == 0);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    some_zones(200);
    CHECK(ps3dk_prof_drain() == PS3DK_PROF_OK);
    const live_agent *a = ps3dk_prof__live_agent();
    for (int i = 0; i < 5000 && atomic_load(&a->bytes_delivered) == 0; ++i)
        usleep(1000);
    CHECK(atomic_load(&a->bytes_delivered) > 0); /* streaming */
    CHECK(ps3dk_prof_live_stop(0, 2000000) == PS3DK_PROF_OK);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(!c.got_end && c.got_hello);           /* cancelled: the partial download is never claimed complete */
    CHECK(atomic_load(&a->drops[LIVE_DROP_CANCELLED]) == 1 && atomic_load(&a->drops[LIVE_DROP_END]) == 0);
    CHECK(close(c.fd) == 0);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    free(c.data);
    return 0;
}

/* ---- the wrapper's state and error translation, which the sample's teardown relies on ------------------------------- */

/* The port is taken: ENET, nothing consumed; the next start may succeed. */
static int case_enet_then_retry(const char *dir)
{
    file_init(dir, PS3DK_PROF_MODE_APP);
    int blocker = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t sl = sizeof sa;
    CHECK(blocker >= 0 && bind(blocker, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(blocker, 1) == 0);
    CHECK(getsockname(blocker, (struct sockaddr *)&sa, &sl) == 0);
    ps3dk_prof_test_hooks.live_listen_port = ntohs(sa.sin_port);
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_ENET);
    CHECK(ps3dk_prof__live_port() == 0);
    CHECK(ps3dk_prof_live_stop(0, 0) == PS3DK_PROF_ENOTINIT); /* never started */
    ps3dk_prof_test_hooks.live_listen_port = 0;
    ps3dk_prof_test_hooks.live_ephemeral_port = 1;
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_live_stop(0, 2000000) == PS3DK_PROF_OK);
    CHECK(close(blocker) == 0);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    return 0;
}

/* The agent thread cannot be created: ETHREAD, the listener released, nothing consumed; a retry succeeds. */
static int case_thread_fail_then_retry(const char *dir)
{
    file_init(dir, PS3DK_PROF_MODE_APP);
    ps3dk_prof_test_hooks.live_ephemeral_port = 1;
    ps3dk_prof_test_hooks.thread_create_fail = 1;
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_ETHREAD);
    CHECK(ps3dk_prof__live_port() == 0);
    CHECK(ps3dk_prof_live_stop(0, 0) == PS3DK_PROF_ENOTINIT);
    ps3dk_prof_test_hooks.thread_create_fail = 0;
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof__live_port() != 0);
    CHECK(ps3dk_prof_live_stop(0, 2000000) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    return 0;
}

/* The agent is held inside a file read (an LV2 file call has no deadline): stop times out, again, and once the read
 * returns a later stop succeeds; the cancelled viewer gets no END. */
static int case_stop_timeout_then_ok(const char *dir)
{
    file_init(dir, PS3DK_PROF_MODE_APP);
    ps3dk_prof_test_hooks.live_ephemeral_port = 1;
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_OK);
    live_test_hooks *h = &ps3dk_prof_test_live_hooks;
    atomic_store(&h->pread_block, 1);
    struct client c = {.data = calloc(64u << 20, 1)};
    CHECK(c.data != NULL);
    c.fd = connect_client(ps3dk_prof__live_port(), 1);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, client_main, &c) == 0);
    for (int i = 0; i < 5000 && !atomic_load(&h->pread_entered); ++i)
        usleep(1000);
    CHECK(atomic_load(&h->pread_entered));
    CHECK(ps3dk_prof_live_stop(0, 100000) == PS3DK_PROF_ETIMEDOUT);
    CHECK(ps3dk_prof_live_stop(0, 100000) == PS3DK_PROF_ETIMEDOUT); /* a repeated stop only waits */
    atomic_store(&h->pread_release, 1);
    CHECK(ps3dk_prof_live_stop(0, 2000000) == PS3DK_PROF_OK);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(!c.got_end);
    CHECK(close(c.fd) == 0);
    free(c.data);
    CHECK(ps3dk_prof_live_stop(0, 0) == PS3DK_PROF_EALREADY);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    return 0;
}

/* The clock fails: stop is ECLOCK and changes nothing; with the clock back, a later stop succeeds. */
static int case_stop_clock_then_ok(const char *dir)
{
    file_init(dir, PS3DK_PROF_MODE_APP);
    ps3dk_prof_test_hooks.live_ephemeral_port = 1;
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_OK);
    atomic_store(&ps3dk_prof_test_live_hooks.clock_fail, 1);
    CHECK(ps3dk_prof_live_stop(1000000, 1000000) == PS3DK_PROF_ECLOCK);
    atomic_store(&ps3dk_prof_test_live_hooks.clock_fail, 0);
    CHECK(ps3dk_prof_live_stop(0, 2000000) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_live_stop(0, 0) == PS3DK_PROF_EALREADY);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    return 0;
}

/* A deadline that would overflow is EINVAL and changes nothing: the agent keeps running and a later stop works. */
static int case_stop_overflow_keeps_state(const char *dir)
{
    file_init(dir, PS3DK_PROF_MODE_APP);
    ps3dk_prof_test_hooks.live_ephemeral_port = 1;
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_live_stop(UINT64_MAX, 1) == PS3DK_PROF_EINVAL);
    CHECK(ps3dk_prof_live_stop(1, UINT64_MAX) == PS3DK_PROF_EINVAL);
    int fd = connect_client(ps3dk_prof__live_port(), 0); /* still listening */
    CHECK(close(fd) == 0);
    CHECK(ps3dk_prof_live_stop(0, 2000000) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    return 0;
}

/* The join fails: ETHREAD, terminal; a later stop is ETHREAD again WITHOUT another join on the consumed handle. */
static int case_join_failure_terminal(const char *dir)
{
    file_init(dir, PS3DK_PROF_MODE_APP);
    ps3dk_prof_test_hooks.live_ephemeral_port = 1;
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_OK);
    ps3dk_prof_test_hooks.join_fail = 1;
    unsigned joins = ps3dk_prof_test_hooks.join_calls;
    CHECK(ps3dk_prof_live_stop(0, 2000000) == PS3DK_PROF_ETHREAD);
    CHECK(ps3dk_prof_test_hooks.join_calls == joins + 1);
    ps3dk_prof_test_hooks.join_fail = 0; /* a retried join would now "succeed": it must not happen */
    CHECK(ps3dk_prof_live_stop(0, 2000000) == PS3DK_PROF_ETHREAD);
    CHECK(ps3dk_prof_live_stop(0, 0) == PS3DK_PROF_ETHREAD);
    CHECK(ps3dk_prof_test_hooks.join_calls == joins + 1);
    CHECK(ps3dk_prof_live_start(NULL) == PS3DK_PROF_EALREADY);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    return 0;
}

/* The measured-device rule the LV2 shim applies (live_path.h). */
static int case_hdd_path_rule(const char *dir)
{
    (void)dir;
    static const char *const accept[] = {"/dev_hdd0/x.prof", "/dev_hdd0/tmp/a/b.prof", "/dev_hdd0/.hidden",
                                         "/dev_hdd0/a..b", "/dev_hdd0/...", "/dev_hdd0/a.b/c"};
    static const char *const reject[] = {"", "/dev_hdd0/", "/dev_hdd0", "/dev_hdd0/../dev_usb000/x",
                                         "/dev_hdd0/./x", "/dev_hdd0/a/../../dev_usb000/x", "/dev_hdd0//x",
                                         "/dev_hdd0/a/", "/dev_usb000/x", "/dev_hdd00/x", "dev_hdd0/x",
                                         "/dev_hdd0/a/..", "/dev_hdd0/.", "/dev_hdd0/..", "/DEV_HDD0/x"};
    for (size_t i = 0; i < sizeof accept / sizeof accept[0]; ++i)
        if (!live_path_under(accept[i], "/dev_hdd0/")) {
            fprintf(stderr, "refused %s\n", accept[i]);
            return 1;
        }
    for (size_t i = 0; i < sizeof reject / sizeof reject[0]; ++i)
        if (live_path_under(reject[i], "/dev_hdd0/")) {
            fprintf(stderr, "admitted %s\n", reject[i]);
            return 1;
        }
    CHECK(!live_path_under(NULL, "/dev_hdd0/"));
    CHECK(!live_path_under("/dev_hdd0/x", "/dev_hdd0")); /* a prefix must end in '/' */
    CHECK(!live_path_under("/dev_hdd0/x", ""));
    return 0;
}

static const struct {
    const char *name;
    int (*fn)(const char *dir);
} cases[] = {
    {"enet_then_retry", case_enet_then_retry},
    {"thread_fail_then_retry", case_thread_fail_then_retry},
    {"stop_timeout_then_ok", case_stop_timeout_then_ok},
    {"stop_clock_then_ok", case_stop_clock_then_ok},
    {"stop_overflow_keeps_state", case_stop_overflow_keeps_state},
    {"join_failure_terminal", case_join_failure_terminal},
    {"hdd_path_rule", case_hdd_path_rule},
    {"before_init", case_before_init},
    {"callback_sink", case_callback_sink},
    {"bad_config", case_bad_config},
    {"shutdown_claim_interval", case_shutdown_claim_interval},
    {"after_shutdown", case_after_shutdown},
    {"stream", case_stream},
    {"stop_cancels", case_stop_cancels},
};

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
            puts(cases[i].name);
        return 0;
    }
    if (argc != 3)
        return 2;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
        if (strcmp(argv[1], cases[i].name) == 0) {
            int rc = cases[i].fn(argv[2]);
            if (rc == 0)
                printf("PASS %s\n", cases[i].name);
            return rc;
        }
    return 2;
}
