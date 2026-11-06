/* overhead-bench GPU section on the REAL runtime and the REAL GCM port over fake device memory (the pattern of
 * tests/host/ps3dk-prof-rsxcap/rsxcap_rt.c): the production section code (common/ob_gpu_run.c: open, the seeded G
 * blocks, close) with its zone calls on ps3dk_prof_gpu_begin/end, its calibrate/detach/release on the real API,
 * and a fake command buffer whose words a small executor decodes (the port's 6-word endpoint pairs, the bench's own
 * report and label methods, NOPs).  The runtime is in APP mode; the drain thread is main and drains only between
 * blocks, as on the target.
 *
 * usage: test_ob_gpu_rt CASE OUT_DIR   CASE: prod (OB_GPU_REPORT_COUNT), min (OB_GPU_BLOCK_SLOTS), under
 *        (OB_GPU_BLOCK_SLOTS - 1), old (32, the reviewed A1 binding)
 * prod/min must show, for every G-g1 block, all 128 endpoints executed into 128 distinct domain report slots and
 * every zone an EVENT (stats: events = zones submitted, lost 0, no refusal), no endpoint in any other block, the
 * emitted sizes g1 - g0 = 64 x 2 x GPP_PAIR_WORDS x 4 bytes and g2 = g1, a clean close.  under/old are the
 * exhaustion negatives: the zone calls still return success, but zones are LOST (lost > 0): API success is not an
 * EVENT receipt.  Prints "GPURT <case> PASS" or FAIL lines. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps3dk/prof.h>

#include "gpu_port_gcm.h"
#include "prof_gpu.h"
#include "prof_spu.h"
#include "prof_test_hooks.h"

#include "../common/ob_gpu_run.h"

static const char *g_case;
static int g_bad;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("GPURT %s FAIL line %d: ", g_case, __LINE__);                                                      \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            g_bad++;                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ---- fake device memory and command buffer ---- */
typedef struct rep_ent {
    uint64_t timer;
    uint32_t value, zero;
} rep_ent;
static rep_ent REP[GPP_REPORTS] __attribute__((aligned(16)));
static uint32_t LAB[GPP_LABEL_LAST + 1];
static void *rep_addr(uint32_t i) { return i < GPP_REPORTS ? &REP[i] : NULL; }
static void *lab_addr(uint32_t i) { return i <= GPP_LABEL_LAST ? &LAB[i] : NULL; }

#define CBUF 65536u
static struct {
    gpp_ctx c;
    uint32_t buf[CBUF] __attribute__((aligned(16)));
    uint32_t *exec; /* executed up to here */
    uint32_t sem;
    unsigned long wraps, unknown, endpoints, bench_reports, labels;
} F;
static uint64_t g_clock = 1000;
static unsigned long slot_hits[GPP_REPORTS];
static uint32_t dom_first, dom_count;

static uint32_t swap02(uint32_t v) { return (v & 0xff00ff00u) | ((v >> 16) & 0xffu) | ((v & 0xffu) << 16); }

/* the GPU: execute every word from F.exec to current */
static void execute(void)
{
    uint32_t *w = F.exec, *end = F.c.current;
    while (w < end) {
        uint32_t h = *w;
        if (h == 0) { /* NOP */
            ++w;
            continue;
        }
        uint32_t count = (h >> 18) & 0x7ffu, m = h & 0xfffcu;
        if (count != 1 || w + 1 >= end) {
            ++F.unknown;
            w += 1 + count;
            continue;
        }
        uint32_t a = w[1];
        if (m == 0x1800u && (a >> 24) == 1u) { /* timestamp report */
            uint32_t r = (a & 0x00ffffffu) / 16u;
            if (r < GPP_REPORTS) {
                REP[r].timer = (g_clock += 37);
                if (r >= dom_first && r < dom_first + dom_count) {
                    ++slot_hits[r];
                    ++F.endpoints;
                } else {
                    ++F.bench_reports;
                }
            } else {
                ++F.unknown;
            }
        } else if (m == 0x1d6cu) {
            F.sem = a / 16u;
        } else if (m == 0x1d70u) {
            if (F.sem <= GPP_LABEL_LAST)
                LAB[F.sem] = swap02(a);
            ++F.labels;
        } else {
            ++F.unknown;
        }
        w += 2;
    }
    F.exec = end;
}
static int32_t cb(gpp_ctx *c, uint32_t count) /* the buffer is full: execute it, wrap to the start */
{
    (void)count;
    execute();
    c->current = c->begin;
    F.exec = c->begin;
    ++F.wraps;
    return 0;
}
static void dev_flush(gpp_ctx *c)
{
    (void)c;
    execute();
}
static const gpp_dev DEV = {rep_addr, lab_addr, dev_flush};
#define CTX ((ps3dk_prof_gcm_ctx *)(void *)&F.c)

static void nop1(void)
{
    if (F.c.current + 1 > F.c.end)
        cb(&F.c, 1);
    *F.c.current++ = 0;
}
static void emit(uint32_t m, uint32_t a)
{
    if (F.c.current + 2 > F.c.end)
        cb(&F.c, 2);
    F.c.current[0] = (1u << 18) | m;
    F.c.current[1] = a;
    F.c.current += 2;
}

/* ---- the ob_rsx port on the fake buffer and the REAL zone calls ---- */
static uint32_t g_gpu;
static ps3dk_prof_gpu_zone g_zone[OB_DRAWS];
static void r_report(void *c, uint32_t i) { (void)c; emit(0x1800u, (1u << 24) | (i * 16u)); }
static void r_label(void *c, uint32_t v)
{
    (void)c;
    emit(0x1d6cu, OB_BENCH_LABEL * 16u);
    emit(0x1d70u, swap02(v));
}
static void r_draw(void *c, uint32_t i)
{
    (void)c;
    (void)i;
    for (int k = 0; k < 4; ++k) /* a stand-in draw: 4 NOP words */
        nop1();
}
static int r_zb(void *c, uint32_t i)
{
    (void)c;
    return PS3DK_PROF_GPU_ZONE_BEGIN(g_gpu, CTX, "ob.g1.draw", g_zone[i]);
}
static int r_ze(void *c, uint32_t i)
{
    (void)c;
    return PS3DK_PROF_GPU_ZONE_END(g_gpu, CTX, g_zone[i]);
}
static void r_nop(void *c, uint32_t n)
{
    (void)c;
    for (uint32_t k = 0; k < n; ++k)
        nop1();
}
static void r_flush(void *c)
{
    (void)c;
    execute();
}
static const ob_rsx_port RSX = {NULL, r_report, r_label, r_draw, r_zb, r_ze, r_nop, r_flush};

/* ---- the ob_gpu port on the real API ---- */
#define MAXB 64
static unsigned long iv_endpoints[MAXB], iv_distinct[MAXB], iv_max_hits[MAXB];
static unsigned niv;
static void interval_close(void) /* the endpoints executed since the previous drain */
{
    unsigned long d = 0, mx = 0;
    for (uint32_t r = dom_first; r < dom_first + dom_count; ++r) {
        d += slot_hits[r] != 0;
        if (slot_hits[r] > mx)
            mx = slot_hits[r];
    }
    if (niv < MAXB) {
        iv_endpoints[niv] = F.endpoints;
        iv_distinct[niv] = d;
        iv_max_hits[niv] = mx;
        ++niv;
    }
    memset(slot_hits, 0, sizeof slot_hits);
    F.endpoints = 0;
}
static int p_init(void *c) { (void)c; return 0; }
static void p_store(void *c, uint32_t v) { (void)c; LAB[OB_BENCH_LABEL] = v; }
static uint32_t p_read(void *c) { (void)c; return LAB[OB_BENCH_LABEL]; }
static void p_pause(void *c) { (void)c; }
static int p_attach(void *c, uint32_t f, uint32_t n, uint32_t l, uint32_t *g)
{
    (void)c;
    int rc = ps3dk_prof_gpu_attach(CTX, f, n, l, g);
    if (rc == 0)
        g_gpu = *g;
    return rc;
}
static int p_cal(void *c, uint32_t g) { (void)c; return ps3dk_prof_gpu_calibrate(g, CTX, 100000); }
static int p_detach(void *c, uint32_t g)
{
    (void)c;
    int rc = 0;
    for (int k = 0; k < 3; ++k) {
        rc = ps3dk_prof_gpu_detach(g, CTX, 100000);
        if (rc != PS3DK_PROF_ETIMEDOUT)
            break;
    }
    return rc;
}
static int p_release(void *c, uint32_t g)
{
    (void)c;
    uint32_t rel = 0;
    int rc = ps3dk_prof_gpu_release(g, &rel);
    return rc ? rc : (rel ? 0 : -1);
}
static int p_drain(void *c)
{
    (void)c;
    interval_close();
    return ps3dk_prof_drain();
}
static uint64_t g_tbv = 5000;
static uint64_t p_tb(void *c) { (void)c; return g_tbv += 11; }
static uint32_t *p_cur(void *c) { (void)c; return F.c.current; }
static uint64_t p_rt(void *c, uint32_t i) { (void)c; return i < GPP_REPORTS ? REP[i].timer : 0; }
static void p_log(void *c, const char *w, int rc) { (void)c; printf("  log: %s rc=%d\n", w, rc); }
static const ob_gpu_port PORT = {NULL,      p_init,  p_store, p_read, p_pause, p_attach, p_cal, p_detach,
                                 p_release, p_drain, p_tb,    p_cur,  p_rt,    p_log,    &RSX};

/* ---- files (POSIX) ---- */
static int f_open(void *c, const char *p) { (void)c; return open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644); }
static long f_write(void *c, int fd, const void *b, unsigned long n) { (void)c; return (long)write(fd, b, n); }
static int f_fsync(void *c, int fd) { (void)c; return fsync(fd); }
static int f_close(void *c, int fd) { (void)c; return close(fd); }
static int f_rename(void *c, const char *a, const char *b) { (void)c; return rename(a, b); }
static const ob_fio IO = {NULL, f_open, f_write, f_fsync, f_close, f_rename};

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    g_case = argv[1];
    uint32_t count = !strcmp(g_case, "prod")  ? OB_GPU_REPORT_COUNT
                     : !strcmp(g_case, "min") ? OB_GPU_BLOCK_SLOTS
                     : !strcmp(g_case, "under") ? OB_GPU_BLOCK_SLOTS - 1u
                     : !strcmp(g_case, "old")   ? 32u
                                                : 0u;
    if (!count)
        return 2;
    int negative = count < OB_GPU_BLOCK_SLOTS;
    _Static_assert(OB_ZONE_ENDPOINT_WORDS == GPP_PAIR_WORDS, "the g2 NOP padding equals the real endpoint size");
    memset(&F, 0, sizeof F);
    F.c.begin = F.c.current = F.exec = F.buf;
    F.c.end = F.buf + CBUF;
    F.c.callback = cb;
    dom_first = OB_GPU_REPORT_FIRST, dom_count = count;
    ps3dk_prof_test_hooks.gpu_gpp_dev = &DEV;
    ps3dk_prof_config c;
    ps3dk_prof_config_defaults(&c);
    c.mode = PS3DK_PROF_MODE_APP;
    c.app = "obgpurt";
    c.build_id = "obgpurt";
    ps3dk_prof_ext_config x;
    memset(&x, 0, sizeof x);
    x.size = PS3DK_PROF_EXT_CONFIG_SIZE;
    x.gpu_domains_max = 1;
    x.gpu_report_slots_max = OB_GPU_REPORT_COUNT;
    CHECK(ps3dk_prof_set_sink_path(argv[2]) == 0, "sink");
    CHECK(ps3dk_prof_init_ex(&c, &x) == 0, "init_ex (APP, one GPU domain)");
    CHECK(ps3dk_prof_thread_begin("ob-drain") == 0, "the drain thread registers");
    ob_gpu g;
    CHECK(ob_gpu_open(&g, &PORT, count, 1000) == 0, "ob_gpu_open (fence, owner check, attach of %u slots)", count);
    char tmp[512], fin[512];
    snprintf(tmp, sizeof tmp, "%s/gpu-%s.csv.tmp", argv[2], g_case);
    snprintf(fin, sizeof fin, "%s/gpu-%s.csv", argv[2], g_case);
    ob_fw w;
    CHECK(ob_fw_open(&w, &IO, tmp) == 0, "csv open");
    const unsigned r = 3;
    int run = ob_gpu_run(&g, 4242, r, &w);
    CHECK(run == 0, "ob_gpu_run completes every block (zone calls return success in every case)");
    CHECK(ob_fw_finish(&w) == 0 && rename(tmp, fin) == 0, "csv written");
    interval_close(); /* the last block's endpoints */
    ps3dk_prof_gpu_stats st;
    memset(&st, 0, sizeof st);
    CHECK(ps3dk_prof_drain() == 0, "final drain");
    CHECK(ps3dk_prof_gpu_get_stats(g.gpu, &st) == 0, "stats");
    /* align the intervals with the rows: interval 0 = before block 0 (the open fence), interval i = block i-1 */
    FILE *f = fopen(fin, "r");
    char line[256];
    unsigned long g1_blocks = 0, other_endpoints = 0, cal_endpoints = 0, g1_bad = 0, bytes[3] = {0, 0, 0}, rows = 0;
    int bi = -1;
    while (f && fgets(line, sizeof line, f)) {
        if (strncmp(line, "GPU,", 4) != 0)
            continue;
        ++bi;
        char cs[32];
        unsigned long idx, rep, b;
        unsigned long long r0, r1, t0, t1;
        unsigned rc;
        char blk[16];
        if (sscanf(line, "GPU,%lu,%31[^,],%15[^,],%lu,%llu,%llu,%lu,%llu,%llu,%u", &idx, cs, blk, &rep, &r0, &r1, &b,
                   &t0, &t1, &rc) != 10) {
            CHECK(0, "row parse: %s", line);
            continue;
        }
        ++rows;
        CHECK(rc == 0, "row %lu rc %u", idx, rc);
        unsigned iv = (unsigned)bi + 1u;
        unsigned long ep = iv < niv ? iv_endpoints[iv] : 0, dist = iv < niv ? iv_distinct[iv] : 0;
        if (!strcmp(cs, "G-g1")) {
            ++g1_blocks;
            if (!negative && (ep != 2u * OB_DRAWS || dist != 2u * OB_DRAWS || iv_max_hits[iv] != 1))
                ++g1_bad;
            if (bytes[1] == 0)
                bytes[1] = b;
        } else if (!strncmp(cs, "G-cal", 5)) {
            cal_endpoints += ep;
        } else {
            other_endpoints += ep;
            if (!strcmp(cs, "G-g0") && bytes[0] == 0)
                bytes[0] = b;
            if (!strcmp(cs, "G-g2") && bytes[2] == 0)
                bytes[2] = b;
        }
    }
    if (f)
        fclose(f);
    printf("  case %s: report slots %u; rows %lu; G-g1 blocks %lu; executed endpoints per G-g1 interval:", g_case, count,
           rows, g1_blocks);
    for (unsigned i = 0; i < niv; ++i)
        printf(" %lu/%lu", iv_endpoints[i], iv_distinct[i]);
    printf("\n  stats: events %llu lost %llu lost_ring %llu lost_fifo %llu lost_records %llu refused_api %llu held %llu"
           " brackets %llu cal_bad %llu; bytes g0 %lu g1 %lu g2 %lu; wraps %lu unknown words %lu\n",
           (unsigned long long)st.events, (unsigned long long)st.lost, (unsigned long long)st.lost_ring,
           (unsigned long long)st.lost_fifo, (unsigned long long)st.lost_records, (unsigned long long)st.refused_api,
           (unsigned long long)st.held, (unsigned long long)st.brackets, (unsigned long long)st.cal_bad, bytes[0],
           bytes[1], bytes[2], F.wraps, F.unknown);
    CHECK(rows == 1u + 5u * r, "every planned block has its row");
    CHECK(F.unknown == 0, "every emitted word decoded (no unknown method)");
    const unsigned long full = 2u * OB_DRAWS * GPP_PAIR_WORDS * 4u;
    CHECK(other_endpoints == 0, "no domain endpoint in a G-g0 / G-g2 block (%lu)", other_endpoints);
    printf("  calibrate blocks: %lu domain endpoints over %u calibrate blocks\n", cal_endpoints, 2u * r);
    CHECK(st.refused_api == 0 && st.refused_ctx == 0, "no API refusal");
    int close_rc = ob_gpu_close(&g);
    if (!negative) {
        CHECK(bytes[1] - bytes[0] == full, "emitted size: g1 - g0 = %lu bytes", bytes[1] - bytes[0]);
        CHECK(bytes[2] == bytes[1], "emitted size: g2 = g1 (%lu, %lu)", bytes[2], bytes[1]);
        CHECK(g1_bad == 0, "every G-g1 block: 128 endpoints executed into 128 distinct domain slots, once each");
        CHECK(st.events == (unsigned long long)g1_blocks * 2u * OB_DRAWS, "every endpoint an EVENT (%llu of %lu)",
              (unsigned long long)st.events, g1_blocks * 2u * OB_DRAWS);
        CHECK(st.lost == 0 && st.lost_ring == 0 && st.lost_fifo == 0 && st.lost_records == 0, "nothing lost");
        CHECK(close_rc == OB_LIFE_CLEAN, "close: fence, detach, release (rc %d)", close_rc);
    } else {
        CHECK(st.lost_ring > 0, "exhaustion: endpoints LOST (ring) although every zone call returned success");
        CHECK(st.events < (unsigned long long)g1_blocks * 2u * OB_DRAWS, "exhaustion: fewer EVENTs than endpoints");
        CHECK(bytes[1] - bytes[0] < full, "exhaustion: the G-g1 block emitted fewer endpoint words (%lu < %lu bytes)",
              bytes[1] - bytes[0], full);
    }
    ps3dk_prof_thread_end();
    if (close_rc == OB_LIFE_CLEAN)
        CHECK(ps3dk_prof_shutdown() == 0, "shutdown");
    printf("GPURT %s %s\n", g_case, g_bad ? "FAIL" : "PASS");
    return g_bad ? 1 : 0;
}
