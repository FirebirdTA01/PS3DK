/* RSX frame capture runtime (rsxf_runtime.c) on the host, threads racing shutdown: the owner (draws, binds, frame
 * boundaries), an app requester and a live requester claim and re-claim the slot, and the drainer drains, then seals
 * while they run. Built with ThreadSanitizer and with AddressSanitizer by rsxf-race-test.py, one round per process.
 *
 *   rsxf-race <seed>   one round: the owner either keeps running through the seal (it must observe STOP, leave, and
 *                      the staging is freed exactly once) or stops before it (the staging stays allocated: nothing
 *                      may free it under an owner that could still run); every written capture is checked (each
 *                      BEGIN has its END, every END count matches the records), every free is of a live block, and
 *                      after the seal every request is refused ESTOPPED.
 *   rsxf-race bench    the FP bind hook's cost: the first bind of a 35 KiB program, a rebind (cache hit) and a bind
 *                      whose sampled bytes changed (a new hash).
 * The runtime's dependencies are stubbed here: no core profiler is linked. */
#define _POSIX_C_SOURCE 200809L
#include "../../../sdk/libps3dkprof/src/core/prof_internal.h"
#include "../../../sdk/libps3dkprof/src/rsx/gpu_port_gcm.h"
#include "../../../sdk/libps3dkprof/src/rsx_frame/rsxf.h"
#include <ps3dk/prof_auto.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);                                    \
            abort();                                                                                                   \
        }                                                                                                              \
    } while (0)

PS3DK_PROF_DEFINE_AUTOMATIC_RSX_FRAME_OPTIONS(PS3DK_PROF_RSX_FRAME_OPTIONS_INIT(4, 2));
enum { BLOCK = 4096, NBLOCKS = 2 };

/* ---- stubs for the runtime's dependencies ---- */
static prof g_prof_storage;
prof *g_prof = &g_prof_storage;
int p_ready(void) { return 1; }
static uint64_t g_tb;
uint64_t p_tb(void) { return __atomic_add_fetch(&g_tb, 1, __ATOMIC_RELAXED); }
int ps3dk_prof__auto_owner_self(void) { return 1; }
int ps3dk_prof_capture_begin(ps3dk_prof_desc *name, uint32_t kind, uint32_t frames)
{
    (void)name; (void)kind; (void)frames;
    return 0;
}
/* Staging blocks tracked without any lock (relaxed atomics give TSan no ordering to hide a race behind). */
static uintptr_t g_live[64];
static long g_block_allocs, g_block_frees;
void *pal_alloc(size_t bytes)
{
    void *p = malloc(bytes);
    if (p && bytes == BLOCK) {
        int put = 0;
        for (int i = 0; i < 64 && !put; i++) {
            uintptr_t z = 0;
            put = __atomic_compare_exchange_n(&g_live[i], &z, (uintptr_t)p, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        }
        CHECK(put);
        __atomic_add_fetch(&g_block_allocs, 1, __ATOMIC_RELAXED);
    }
    return p;
}
void pal_free(void *p)
{
    int found = 0;
    for (int i = 0; i < 64 && !found; i++) {
        uintptr_t v = (uintptr_t)p;
        found = __atomic_compare_exchange_n(&g_live[i], &v, 0, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    }
    CHECK(found); /* a staging block freed twice, or never allocated */
    __atomic_add_fetch(&g_block_frees, 1, __ATOMIC_RELAXED);
    free(p);
}

void ps3dk_prof__rsxf_bind(void *ctx, const void *ucode, uint32_t bytes, uint32_t offset);
void ps3dk_prof__rsxf_test_bind_stats(uint64_t *hashed, uint64_t *reused);
uint64_t ps3dk_prof__rsxf_test_slot(void);
size_t ps3dk_prof__rsxf_test_state_bytes(void);
static const char *const STATES[] = {"IDLE", "CLAIMING", "REQUESTED", "ARMING", "ARMED", "CLOSING", "?"};

#define M(m, n) (((uint32_t)(n) << 18) | (m))

/* ---- one round ---- */
static uint32_t g_buf[1024];
static gpp_ctx g_ctx = {g_buf, g_buf + 1024, g_buf, NULL};
static int g_sealed;        /* atomic: the seal drain returned */
static int g_owner_stop;    /* atomic: main asks the owner to stop (before-seal mode) */
static int g_owner_done;    /* atomic */
static int g_post;          /* owner iterations after the seal (0: the owner stops before it) */
static unsigned g_seed;

static unsigned rnd(unsigned *s)
{
    *s = *s * 1103515245u + 12345u;
    return *s >> 16;
}

static void put(const uint32_t *w, uint32_t n)
{
    if (g_ctx.current + n > g_ctx.end) {
        ps3dk_prof__rsxf_wrap(&g_ctx);
        g_ctx.current = g_ctx.begin;
    }
    memcpy(g_ctx.current, w, n * 4u);
    g_ctx.current += n;
}

static void *owner(void *arg)
{
    unsigned s = g_seed ^ 0x5bd1e995u;
    uint64_t frame = 1, op = 1;
    int after = 0;
    (void)arg;
    for (uint32_t i = 0;; i++) {
        if (__atomic_load_n(&g_owner_stop, __ATOMIC_ACQUIRE))
            break;
        if (__atomic_load_n(&g_sealed, __ATOMIC_ACQUIRE) && ++after > g_post)
            break;
        if (i % 7 == 0) {
            /* a program container bound, then freed by the app at once: the capture must never read it again */
            uint8_t *u = malloc(64);
            memset(u, 0, 64);
            u[48 + 1] = 1; /* the last instruction's end bit (halfword-swapped word 0, bit 0) */
            ps3dk_prof__rsxf_bind(&g_ctx, u, 64, 0x40);
            free(u);
            const uint32_t vp[] = {M(0x1e9c, 1), 0, M(0xb80, 4), 1, 2, 3, 1, M(0x1ea0, 1), 0, M(0x8e4, 1), 0x41};
            put(vp, 11);
        }
        ps3dk_prof__rsxf_hook_begin(&g_ctx, 1, 5, 0, 3, 0, 0, 0, 0, 0x1000 + i, 0);
        {
            const uint32_t d[] = {M(0x1808, 1), 5, M(0x1814, 1), 2u << 24, M(0x1808, 1), 0};
            put(d, 6);
        }
        ps3dk_prof__rsxf_hook_admitted(&g_ctx, (i % 3) ? op++ : 0);
        ps3dk_prof__rsxf_hook_end(&g_ctx);
        if (rnd(&s) % 4 == 0) {
            const uint32_t raw[] = {M(0x1d94, 1), 0xf0}; /* an unhooked clear */
            put(raw, 2);
        }
        if (i % 5 == 4)
            ps3dk_prof__rsxf_frame(frame++, p_tb(), 1, 1, 1);
        if (rnd(&s) % 8 == 0)
            sched_yield();
    }
    __atomic_store_n(&g_owner_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *requester(void *arg)
{
    int live = arg != NULL;
    const rsxf_live_ops *ops = ps3dk_prof__rsxf_live_ops();
    uint32_t req = 1;
    CHECK(ops);
    for (;;) {
        int sealed = __atomic_load_n(&g_sealed, __ATOMIC_ACQUIRE);
        int rc;
        uint32_t id = 0;
        if (live)
            rc = ops->request(req++, 0, "live\xff", 5, &id); /* an invalid byte: copied as '?' */
        else
            rc = ps3dk_prof_rsx_frame_capture("app", 0);
        CHECK(rc == PS3DK_PROF_OK || rc > 0 || rc == PS3DK_PROF_EBUSY || rc == PS3DK_PROF_ESTOPPED);
        if (sealed) {
            CHECK(rc == PS3DK_PROF_ESTOPPED); /* the seal set STOP before it returned */
            return NULL;
        }
        sched_yield();
    }
}

/* ---- the drainer's sink and the stream check ---- */
static uint8_t *g_out;
static size_t g_len, g_cap;
static int emit(void *user, uint32_t type, const uint8_t *data, size_t bytes)
{
    (void)user;
    if (g_len + 8 + bytes > g_cap) {
        g_cap = (g_len + 8 + bytes) * 2;
        g_out = realloc(g_out, g_cap);
        CHECK(g_out);
    }
    memcpy(g_out + g_len, &type, 4);
    memcpy(g_out + g_len + 4, &bytes, 4);
    memcpy(g_out + g_len + 8, data, bytes);
    g_len += 8 + bytes;
    return 0;
}
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

/* Every BEGIN has its END; every END count is the records written for that capture. Returns the captures. */
static int check_stream(void)
{
    size_t at = 0;
    int open = 0, captures = 0, config = 0;
    uint32_t id = 0, seq = 0, draws = 0, programs = 0, labels = 0, blocks = 0;
    uint64_t bytes = 0;
    while (at < g_len) {
        uint32_t type = rd32(g_out + at), n = rd32(g_out + at + 4);
        const uint8_t *p = g_out + at + 8;
        at += 8 + n;
        if (type == RSXF_CHUNK_CONFIG) {
            CHECK(!config && n == RSXF_CONFIG_BYTES && rd32(p + 8) == RSXF_AVAILABLE);
            config = 1;
        } else if (type == RSXF_CHUNK_BEGIN) {
            CHECK(config && !open && rd32(p) > id);
            id = rd32(p);
            /* a name is valid UTF-8: the live request's invalid byte arrives as '?' */
            if ((p[4] | p[5] << 8) == 1)
                CHECK((p[52] | p[53] << 8) == 5 && !memcmp(p + 56, "live?", 5));
            open = 1;
            seq = draws = programs = labels = blocks = 0;
            bytes = 0;
        } else if (type == RSXF_CHUNK_DRAWS) {
            uint32_t records = rd32(p + 8), rb = rd32(p + 12);
            size_t r = RSXF_DRAWS_HEADER;
            CHECK(open && rd32(p) == id && rd32(p + 4) == seq && rb == n - RSXF_DRAWS_HEADER);
            seq++;
            blocks++;
            bytes += rb;
            for (uint32_t k = 0; k < records; k++) {
                uint32_t ty = p[r] | p[r + 1] << 8, len = p[r + 2] | p[r + 3] << 8;
                CHECK(len >= 8 && r + len <= n);
                draws += ty == RSXF_REC_DRAW;
                programs += ty == RSXF_REC_PROGRAM;
                labels += ty == RSXF_REC_LABEL;
                r += len;
            }
            CHECK(r == n);
        } else if (type == RSXF_CHUNK_END) {
            CHECK(open && n == RSXF_END_BYTES && rd32(p) == id);
            CHECK(rd32(p + 8) == blocks && rd32(p + 20) == draws && rd32(p + 40) == programs &&
                  rd32(p + 44) == labels && rd64(p + 56) == bytes && rd32(p + 16) >= draws);
            CHECK(rd32(p + 96) == 0); /* no lost decoder event */
            open = 0;
            captures++;
        } else {
            CHECK(0);
        }
    }
    CHECK(config && !open);
    return captures;
}

static int round_run(unsigned seed)
{
    pthread_t o, r1, r2;
    unsigned s = seed;
    int drains, before_seal, captures;
    uint32_t aim, at_seal;
    g_seed = seed;
    before_seal = seed % 3 == 0;   /* the owner stops before the seal and never runs again */
    g_post = before_seal ? 0 : 50 + (int)(rnd(&s) % 400);
    drains = 20 + (int)(rnd(&s) % 400);
    ps3dk_prof__rsxf_attach(&g_ctx, 1);
    CHECK(pthread_create(&o, NULL, owner, NULL) == 0);
    CHECK(pthread_create(&r1, NULL, requester, NULL) == 0);
    CHECK(pthread_create(&r2, NULL, requester, (void *)1) == 0);
    for (int i = 0; i < drains; i++) {
        ps3dk_prof__rsxf_drain(emit, NULL, 1, 0);
        sched_yield();
    }
    /* aim the seal (or the owner's stop) at a capture in flight for most seeds; a bounded
     * number of tries (the state is then whatever it is) */
    aim = (uint32_t[]){0, 4, 2, 1}[(seed / 3) % 4]; /* none, ARMED, REQUESTED, CLAIMING */
    for (int i = 0; aim && i < 200000 && (ps3dk_prof__rsxf_test_slot() & 0xff) != aim; i++) {
        ps3dk_prof__rsxf_drain(emit, NULL, 1, 0);
        sched_yield();
    }
    if (before_seal) {
        __atomic_store_n(&g_owner_stop, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&g_owner_done, __ATOMIC_ACQUIRE))
            sched_yield();
    }
    at_seal = (uint32_t)(ps3dk_prof__rsxf_test_slot() & 0xff);
    if (rnd(&s) % 2)
        __atomic_store_n(&g_prof->stop_latch, 1u, __ATOMIC_RELEASE); /* the core's latch first, as at shutdown */
    ps3dk_prof__rsxf_drain(emit, NULL, 1, 1);
    __atomic_store_n(&g_sealed, 1, __ATOMIC_RELEASE);
    CHECK(pthread_join(o, NULL) == 0 && pthread_join(r1, NULL) == 0 && pthread_join(r2, NULL) == 0);
    ps3dk_prof__rsxf_drain(emit, NULL, 1, 0); /* after the seal: nothing more is written */
    captures = check_stream();
    {
        long a = __atomic_load_n(&g_block_allocs, __ATOMIC_RELAXED), f = __atomic_load_n(&g_block_frees, __ATOMIC_RELAXED);
        CHECK(f <= a);
        if (before_seal)
            CHECK(a - f == 0 || a - f == NBLOCKS); /* kept allocated only for an owner that never observed STOP */
        else
            CHECK(a == f);                          /* the owner left: whoever left second freed the staging */
        printf("seed %u: %s, sealed in %s, %d drains, %d captures, %ld block allocations, %ld kept\n", seed,
               before_seal ? "owner stopped before the seal" : "owner ran through the seal",
               STATES[at_seal < 6 ? at_seal : 6], drains, captures, a, a - f);
    }
    return 0;
}

/* ---- bind cost ---- */
static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static int bench(void)
{
    enum { BYTES = 35 * 1024, HITS = 200000, MISSES = 200 };
    uint8_t *u = calloc(1, BYTES);
    uint64_t hashed, reused;
    double t0, t1, t2, t3;
    CHECK(u);
    u[BYTES - 16 + 1] = 1; /* the end bit on the last instruction: the hash walks all 35 KiB */
    ps3dk_prof__rsxf_attach(&g_ctx, 1);
    t0 = now_ns();
    ps3dk_prof__rsxf_bind(&g_ctx, u, BYTES, 0x40);
    t1 = now_ns();
    for (int i = 0; i < HITS; i++)
        ps3dk_prof__rsxf_bind(&g_ctx, u, BYTES, 0x40);
    t2 = now_ns();
    for (int i = 0; i < MISSES; i++) {
        u[5] = (uint8_t)(i + 1); /* a sampled byte changed: the container was rewritten in place */
        ps3dk_prof__rsxf_bind(&g_ctx, u, BYTES, 0x40);
    }
    t3 = now_ns();
    ps3dk_prof__rsxf_test_bind_stats(&hashed, &reused);
    CHECK(hashed == 1 + MISSES && reused == HITS);
    printf("bind cost (host, 35 KiB FP program): first bind %.0f ns, rebind %.1f ns, rewritten %.0f ns\n", t1 - t0,
           (t2 - t1) / HITS, (t3 - t2) / MISSES);
    printf("standing state: %zu bytes (host, 64-bit pointers)\n", ps3dk_prof__rsxf_test_state_bytes());
    free(u);
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "bench"))
        return bench();
    return round_run((unsigned)strtoul(argv[1], NULL, 10));
}
