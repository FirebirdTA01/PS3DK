/* Live capture triggers over the real runtime (protocol v2): ps3dk_prof_live_start, a raw v2 client that sends
 * CAPTURE_BEGIN / CAPTURE_END / CAPTURE_FRAMES, and the frame thread that applies them at its frame marks.  Checks
 * every ACK against the capture-window state the public API reports, and every refusal (EBUSY, EINVAL, EPERM,
 * EFULL, ETIMEDOUT, ESTOPPED).  Built WITHOUT the whole-run helper: the windows here are the viewer's.  One case per
 * process (the agent is single-use per run).
 * usage: live-trigger-test --list | <case> <scratch dir> */
#define _GNU_SOURCE
#include <ps3dk/prof.h>
#include "live_api.h"
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
#include <time.h>
#include <unistd.h>

#define CHECK(c)                                                                   \
    do {                                                                           \
        if (!(c)) {                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
            exit(1);                                                               \
        }                                                                          \
    } while (0)

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void init_live(const char *dir, uint32_t max_descriptors)
{
    ps3dk_prof_config c;
    ps3dk_prof_config_defaults(&c);
    c.mode = PS3DK_PROF_MODE_APP;
    c.app = "livetrig";
    c.build_id = "live-trigger-build";
    c.max_threads = 4;
    c.max_descriptors = max_descriptors;
    c.ring_records = 4096;
    CHECK(ps3dk_prof_set_sink_path(dir) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_init(&c) == PS3DK_PROF_OK);
    ps3dk_prof_test_hooks.live_ephemeral_port = 1;
    ps3dk_prof_live_config lc;
    memset(&lc, 0, sizeof lc);
    lc.poll_us = 1000;
    CHECK(ps3dk_prof_live_start(&lc) == PS3DK_PROF_OK);
    CHECK(ps3dk_prof_thread_begin("main") == PS3DK_PROF_OK);
}

/* ---- a raw v2 client: a reader thread records HELLO, ACKs and END ------------------------------------------------- */

static struct {
    int fd;
    atomic_int hello, end, closed, acks;
    uint32_t caps, protocol;
    uint8_t ack[LIVE_ACK_BYTES];
    atomic_int bad; /* a frame the protocol does not allow */
} g_c;

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

static void *reader(void *arg)
{
    (void)arg;
    uint8_t hdr[LIVE_HDR_BYTES];
    static uint8_t p[LIVE_MAX_PAYLOAD];
    for (;;) {
        if (read_full(g_c.fd, hdr, sizeof hdr) != 0)
            break;
        uint32_t type = live_get16(hdr), len = live_get32(hdr + 4);
        if (live_get16(hdr + 2) != 0 || len > LIVE_MAX_PAYLOAD || read_full(g_c.fd, p, len) != 0) {
            atomic_store(&g_c.bad, 1);
            break;
        }
        if (atomic_load(&g_c.end)) {
            atomic_store(&g_c.bad, 1); /* nothing after END */
        } else if (type == LIVE_T_HELLO) {
            g_c.protocol = live_get16(p);
            g_c.caps = live_get32(p + 4);
            atomic_store(&g_c.hello, 1);
        } else if (type == LIVE_T_CAPTURE_ACK) {
            if (len != LIVE_ACK_BYTES)
                atomic_store(&g_c.bad, 1);
            memcpy(g_c.ack, p, LIVE_ACK_BYTES);
            atomic_fetch_add(&g_c.acks, 1);
        } else if (type == LIVE_T_END) {
            atomic_store(&g_c.end, 1);
        } else if (type != LIVE_T_DATA && type != LIVE_T_PONG) {
            atomic_store(&g_c.bad, 1);
        }
    }
    atomic_store(&g_c.closed, 1);
    return NULL;
}

static pthread_t g_reader;

static void client_open(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(ps3dk_prof__live_port()),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0);
    uint8_t h[LIVE_HDR_BYTES + 8];
    live_put16(h, LIVE_T_CLIENT_HELLO);
    live_put16(h + 2, 0);
    live_put32(h + 4, 8);
    live_put32(h + 8, LIVE_MAGIC);
    live_put16(h + 12, 2);
    live_put16(h + 14, 0);
    CHECK(send(fd, h, sizeof h, MSG_NOSIGNAL) == (ssize_t)sizeof h);
    g_c.fd = fd;
    CHECK(pthread_create(&g_reader, NULL, reader, NULL) == 0);
    uint64_t until = now_us() + 3000000;
    while (!atomic_load(&g_c.hello) && now_us() < until)
        usleep(1000);
    CHECK(atomic_load(&g_c.hello) && g_c.protocol == 2 && g_c.caps == LIVE_CAP_TRIGGERS);
}

static void send_cmd(uint32_t type, uint32_t req, uint32_t frames, const char *name, uint32_t name_len)
{
    uint8_t f[LIVE_HDR_BYTES + LIVE_CMD_MAX_BYTES];
    uint8_t *p = f + LIVE_HDR_BYTES;
    uint32_t len;
    live_put32(p, req);
    if (type == LIVE_T_CAPTURE_END) {
        len = 4;
    } else if (type == LIVE_T_CAPTURE_BEGIN) {
        live_put16(p + 4, name_len);
        live_put16(p + 6, 0);
        memcpy(p + 8, name, name_len);
        len = 8 + name_len;
    } else {
        live_put32(p + 4, frames);
        live_put16(p + 8, name_len);
        live_put16(p + 10, 0);
        memcpy(p + 12, name, name_len);
        len = 12 + name_len;
    }
    live_put16(f, type);
    live_put16(f + 2, 0);
    live_put32(f + 4, len);
    CHECK(send(g_c.fd, f, LIVE_HDR_BYTES + len, MSG_NOSIGNAL) == (ssize_t)(LIVE_HDR_BYTES + len));
}

static uint32_t g_req;

/* Sends one command and waits for its ACK.  With `frames`, the frame thread marks one frame after the agent had
 * time to queue the command (20 ms), then waits 200 ms for the ACK before it marks another, so the command is
 * normally applied by exactly one mark.  Returns the result; *window gets the window id, *marked the marks made. */
static int32_t command(uint32_t type, uint32_t n, const char *name, int frames, uint32_t *window, int *marked)
{
    int before = atomic_load(&g_c.acks), m = 0;
    uint32_t req = ++g_req;
    send_cmd(type, req, n, name, name ? (uint32_t)strlen(name) : 0);
    usleep(20000);
    uint64_t until = now_us() + 10000000;
    while (atomic_load(&g_c.acks) == before && now_us() < until) {
        if (frames) {
            PS3DK_PROF_FRAME();
            m++;
            CHECK(ps3dk_prof_drain() == PS3DK_PROF_OK);
        }
        uint64_t wait = now_us() + 200000;
        while (atomic_load(&g_c.acks) == before && now_us() < wait)
            usleep(1000);
    }
    CHECK(atomic_load(&g_c.acks) == before + 1 && !atomic_load(&g_c.bad));
    CHECK(live_get32(g_c.ack) == req && live_get16(g_c.ack + 12) == type && live_get16(g_c.ack + 14) == 0);
    int32_t rc = (int32_t)live_get32(g_c.ack + 4);
    if (window)
        *window = live_get32(g_c.ack + 8);
    if (marked)
        *marked = m;
    if (rc != 0)
        CHECK(live_get32(g_c.ack + 8) == 0); /* a refusal names no window */
    return rc;
}

static void frames(int n)
{
    for (int i = 0; i < n; ++i) {
        PS3DK_PROF_ZONE("work");
        PS3DK_PROF_FRAME();
    }
    CHECK(ps3dk_prof_drain() == PS3DK_PROF_OK);
}

static void finish(void)
{
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    uint64_t until = now_us() + 10000000;
    while (!atomic_load(&g_c.end) && now_us() < until)
        usleep(1000);
    CHECK(atomic_load(&g_c.end) && !atomic_load(&g_c.bad));
    CHECK(ps3dk_prof_live_stop(1000000, 1000000) == PS3DK_PROF_OK);
    CHECK(pthread_join(g_reader, NULL) == 0);
    CHECK(close(g_c.fd) == 0);
}

/* ---- cases ------------------------------------------------------------------------------------------------------- */

/* BEGIN opens a MANUAL window named as sent, at the frame mark that took it; END closes it after the current frame;
 * FRAMES opens one that closes itself after n frames.  The ACK's window id is the API's. */
static int case_begin_end_frames(const char *dir)
{
    init_live(dir, 16);
    frames(3);
    ps3dk_prof_capture_info info;
    uint32_t w1, w2, w3;
    int marked;
    CHECK(ps3dk_prof_capture_state(&info) == PS3DK_PROF_OK && info.state == 0 && info.id == 0);
    client_open();

    CHECK(command(LIVE_T_CAPTURE_BEGIN, 0, "pc-window", 1, &w1, &marked) == 0);
    CHECK(w1 == 1 && marked >= 1);
    CHECK(ps3dk_prof_capture_state(&info) == PS3DK_PROF_OK);
    CHECK(info.state == 2 && info.id == w1 && info.kind == PS3DK_PROF_WINDOW_MANUAL);
    CHECK(info.name && strcmp(info.name, "pc-window") == 0);
    frames(5);
    /* one window at a time */
    CHECK(command(LIVE_T_CAPTURE_BEGIN, 0, "second", 1, NULL, NULL) == PS3DK_PROF_EBUSY);
    CHECK(command(LIVE_T_CAPTURE_FRAMES, 4, "second", 1, NULL, NULL) == PS3DK_PROF_EBUSY);
    uint32_t we;
    CHECK(command(LIVE_T_CAPTURE_END, 0, NULL, 1, &we, NULL) == 0 && we == w1);
    /* the END took effect in the frame mark that applied it */
    CHECK(ps3dk_prof_capture_state(&info) == PS3DK_PROF_OK && info.state == 0 && info.id == w1);
    CHECK(command(LIVE_T_CAPTURE_END, 0, NULL, 1, NULL, NULL) == PS3DK_PROF_EINVAL); /* nothing open */

    CHECK(command(LIVE_T_CAPTURE_FRAMES, 4, "pc-frames", 1, &w2, NULL) == 0 && w2 == w1 + 1);
    CHECK(ps3dk_prof_capture_state(&info) == PS3DK_PROF_OK && info.state == 2 && info.id == w2);
    CHECK(info.kind == PS3DK_PROF_WINDOW_FRAMES && strcmp(info.name, "pc-frames") == 0);
    CHECK(info.frames >= 1 && info.frames < 4);
    frames((int)(4 - info.frames)); /* up to the window's 4th frame */
    CHECK(ps3dk_prof_capture_state(&info) == PS3DK_PROF_OK && info.state == 2 && info.frames == 4);
    frames(1);
    CHECK(ps3dk_prof_capture_state(&info) == PS3DK_PROF_OK && info.state == 0); /* closed itself: COUNT */

    /* the same name again reuses its descriptor; bad names are refused before anything is queued */
    CHECK(command(LIVE_T_CAPTURE_BEGIN, 0, "pc-window", 1, &w3, NULL) == 0 && w3 == w2 + 1);
    CHECK(command(LIVE_T_CAPTURE_END, 0, NULL, 1, NULL, NULL) == 0);
    CHECK(command(LIVE_T_CAPTURE_BEGIN, 0, "\xff", 0, NULL, NULL) == PS3DK_PROF_EINVAL);
    CHECK(command(LIVE_T_CAPTURE_BEGIN, 0, "\xc0\xaf", 0, NULL, NULL) == PS3DK_PROF_EINVAL); /* overlong */
    CHECK(command(LIVE_T_CAPTURE_BEGIN, 0, "\xed\xa0\x80", 0, NULL, NULL) == PS3DK_PROF_EINVAL); /* surrogate */
    CHECK(command(LIVE_T_CAPTURE_BEGIN, 0, "caf\xc3\xa9", 1, NULL, NULL) == 0);                  /* valid UTF-8 */
    CHECK(command(LIVE_T_CAPTURE_END, 0, NULL, 1, NULL, NULL) == 0);
    CHECK(ps3dk_prof_capture_state(&info) == PS3DK_PROF_OK && info.windows == 4 && info.refused == 2);
    frames(2);
    finish();
    const live_agent *a = ps3dk_prof__live_agent();
    printf("MEASURE acks=%llu refusals=%llu\n", (unsigned long long)atomic_load(&a->acks),
           (unsigned long long)atomic_load(&a->ack_refusals));
    CHECK(atomic_load(&a->acks) == 13 && atomic_load(&a->ack_refusals) == 6);
    return 0;
}

/* A window the app opened is the app's: the viewer cannot end it (EPERM) nor open another (EBUSY). */
static int case_app_window(const char *dir)
{
    init_live(dir, 16);
    client_open();
    CHECK(PS3DK_PROF_CAPTURE_BEGIN("app-window") == PS3DK_PROF_OK);
    frames(2);
    CHECK(command(LIVE_T_CAPTURE_END, 0, NULL, 1, NULL, NULL) == PS3DK_PROF_EPERM);
    CHECK(command(LIVE_T_CAPTURE_BEGIN, 0, "viewer", 1, NULL, NULL) == PS3DK_PROF_EBUSY);
    ps3dk_prof_capture_info info;
    CHECK(ps3dk_prof_capture_state(&info) == PS3DK_PROF_OK && info.state == 2 && strcmp(info.name, "app-window") == 0);
    CHECK(PS3DK_PROF_CAPTURE_END() == PS3DK_PROF_OK);
    frames(2);
    /* after the app's window, the viewer's END has nothing of its own to end: the last window was the app's */
    CHECK(command(LIVE_T_CAPTURE_END, 0, NULL, 1, NULL, NULL) == PS3DK_PROF_EINVAL);
    finish();
    return 0;
}

/* The name table holds 32 distinct names for the run; a 33rd is refused EFULL at once, a known one still works. */
static int case_names_full(const char *dir)
{
    init_live(dir, 64);
    client_open();
    char name[16];
    for (int i = 0; i < 32; ++i) {
        snprintf(name, sizeof name, "w%02d", i);
        CHECK(command(LIVE_T_CAPTURE_BEGIN, 0, name, 1, NULL, NULL) == 0);
        CHECK(command(LIVE_T_CAPTURE_END, 0, NULL, 1, NULL, NULL) == 0);
        frames(1);
    }
    int marked = -1;
    CHECK(command(LIVE_T_CAPTURE_BEGIN, 0, "w32", 0, NULL, &marked) == PS3DK_PROF_EFULL && marked == 0);
    CHECK(command(LIVE_T_CAPTURE_FRAMES, 2, "w07", 1, NULL, NULL) == 0);
    frames(2);
    ps3dk_prof_capture_info info;
    CHECK(ps3dk_prof_capture_state(&info) == PS3DK_PROF_OK && info.state == 0 && info.windows == 33);
    finish();
    return 0;
}

/* No frame mark within the agent's command deadline (5 s by default): withdrawn and refused ETIMEDOUT; nothing
 * opens when frames resume. */
static int case_no_frames_timeout(const char *dir)
{
    init_live(dir, 16);
    client_open();
    uint64_t t0 = now_us();
    CHECK(command(LIVE_T_CAPTURE_BEGIN, 0, "never", 0, NULL, NULL) == PS3DK_PROF_ETIMEDOUT);
    uint64_t waited = now_us() - t0;
    CHECK(waited >= 5000000 && waited < 8000000);
    frames(3);
    ps3dk_prof_capture_info info;
    CHECK(ps3dk_prof_capture_state(&info) == PS3DK_PROF_OK && info.state == 0 && info.windows == 0);
    printf("MEASURE timeout after %llu us\n", (unsigned long long)waited);
    finish();
    return 0;
}

/* A command queued when shutdown begins is refused ESTOPPED before END (never left unanswered). */
static int case_stopped(const char *dir)
{
    init_live(dir, 16);
    client_open();
    frames(2);
    int before = atomic_load(&g_c.acks);
    send_cmd(LIVE_T_CAPTURE_BEGIN, 99, 0, "late", 4);
    usleep(50000); /* queued; no frame mark takes it */
    CHECK(ps3dk_prof_shutdown() == PS3DK_PROF_OK);
    uint64_t until = now_us() + 10000000;
    while (!atomic_load(&g_c.end) && now_us() < until)
        usleep(1000);
    CHECK(atomic_load(&g_c.end) && !atomic_load(&g_c.bad) && atomic_load(&g_c.acks) == before + 1);
    CHECK(live_get32(g_c.ack) == 99 && (int32_t)live_get32(g_c.ack + 4) == PS3DK_PROF_ESTOPPED);
    CHECK(ps3dk_prof_live_stop(1000000, 1000000) == PS3DK_PROF_OK);
    CHECK(pthread_join(g_reader, NULL) == 0);
    CHECK(close(g_c.fd) == 0);
    return 0;
}

struct named { const char *name; int (*fn)(const char *); };
static const struct named cases[] = {
    {"begin_end_frames", case_begin_end_frames},
    {"app_window", case_app_window},
    {"names_full", case_names_full},
    {"no_frames_timeout", case_no_frames_timeout},
    {"stopped", case_stopped},
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
