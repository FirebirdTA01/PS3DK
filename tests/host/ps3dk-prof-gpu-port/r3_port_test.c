/* P3.3 R3 host tests of the GCM device binding (gpu_port_gcm.c) over a fake context and fake report/label memory.
 * Pins the CHOSEN words (not the hardware-accepted encoding), the checked 6-word reservation, and prepare's
 * validation (zero device writes on refusal).  usage: r3_port_test [CASE]  (no CASE: all; exit 0 only if all pass) */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_port_gcm.h"

static int fails, checks;
static const char *cur_case;
#define CHECK(c)                                                                                                      \
    do {                                                                                                              \
        checks++;                                                                                                     \
        if (!(c)) {                                                                                                   \
            fails++;                                                                                                  \
            printf("  FAIL %s: %s (line %d)\n", cur_case, #c, __LINE__);                                             \
        }                                                                                                             \
    } while (0)

/* ---- fake device memory ---- */
typedef struct rep_ent {
    uint64_t timer;
    uint32_t value, zero;
} rep_ent;
static rep_ent rep[GPP_REPORTS + 2] __attribute__((aligned(16)));
static uint32_t lab[GPP_LABEL_LAST + 2] __attribute__((aligned(16)));
static rep_ent rep_snap[GPP_REPORTS + 2];
static uint32_t lab_snap[GPP_LABEL_LAST + 2];
static unsigned rep_calls, lab_calls, flush_calls;
static gpp_ctx *flush_ctx;
static int64_t rep_fail = -1, rep_misalign = -1, rep_skew = -1, lab_fail, lab_misalign, rep_shift_all;

static void *f_report(uint32_t i)
{
    rep_calls++;
    if ((int64_t)i == rep_fail || i >= GPP_REPORTS)
        return NULL;
    if ((int64_t)i == rep_misalign)
        return (char *)&rep[i] + 4;
    if ((int64_t)i == rep_skew)
        return &rep[i + 1];
    if (rep_shift_all) /* every entry 4 bytes off: a consistent 16-byte stride, misaligned timers */
        return (char *)&rep[i] + 4;
    return &rep[i];
}
static void *f_label(uint32_t i)
{
    lab_calls++;
    if (lab_fail || i > GPP_LABEL_LAST)
        return NULL;
    if (lab_misalign)
        return (char *)&lab[i] + 2;
    return &lab[i];
}
static void f_flush(gpp_ctx *c)
{
    flush_calls++;
    flush_ctx = c;
}
static const gpp_dev fdev = {f_report, f_label, f_flush};

static void mem_fill(void)
{
    for (unsigned i = 0; i < GPP_REPORTS + 2; i++)
        rep[i] = (rep_ent){0x1111000000000000ull | i, 0x22220000u | i, 0x33330000u | i};
    for (unsigned i = 0; i < GPP_LABEL_LAST + 2; i++)
        lab[i] = 0x44440000u | i;
    memcpy(rep_snap, rep, sizeof rep);
    memcpy(lab_snap, lab, sizeof lab);
}
static int mem_untouched(void)
{
    return memcmp(rep_snap, rep, sizeof rep) == 0 && memcmp(lab_snap, lab, sizeof lab) == 0;
}
static void dev_reset(void)
{
    rep_calls = lab_calls = flush_calls = 0;
    flush_ctx = NULL;
    rep_fail = rep_misalign = rep_skew = -1;
    lab_fail = lab_misalign = rep_shift_all = 0;
    mem_fill();
}

/* ---- fake command buffer: one array, guarded regions, every pointer in-object ---- */
#define BUF 96u
#define GUARD 0xC0DEC0DEu
#define FILL 0xA5A5A5A5u
static uint32_t buf[BUF] __attribute__((aligned(16)));
static uint32_t buf_snap[BUF];
static gpp_ctx ctx;
static unsigned cb_calls;
static uint32_t cb_count;
static int (*cb_action)(gpp_ctx *);

static int32_t cb(gpp_ctx *c, uint32_t count)
{
    cb_calls++;
    cb_count = count;
    return cb_action ? cb_action(c) : 0;
}
/* region A: buf[4..4+n), guards at 0..3 and after; region B: buf[64..80) */
static void buf_setup(unsigned avail, int with_cb)
{
    for (unsigned i = 0; i < BUF; i++)
        buf[i] = GUARD;
    for (unsigned i = 4; i < 40; i++)
        buf[i] = FILL;
    for (unsigned i = 64; i < 80; i++)
        buf[i] = FILL;
    ctx.begin = &buf[4];
    ctx.current = &buf[4 + 10];
    ctx.end = &buf[4 + 10 + avail];
    ctx.callback = with_cb ? cb : NULL;
    cb_calls = 0;
    cb_count = 0;
    cb_action = NULL;
    memcpy(buf_snap, buf, sizeof buf);
}
static int buf_same_except(unsigned from, unsigned n)
{
    for (unsigned i = 0; i < BUF; i++)
        if ((i < from || i >= from + n) && buf[i] != buf_snap[i])
            return 0;
    return 1;
}
static int a_room_wrap(gpp_ctx *c) /* "published" earlier commands: current back to begin */
{
    c->current = c->begin;
    return 0;
}
static int a_room_extend(gpp_ctx *c)
{
    c->end = &buf[4 + 30];
    return 0;
}
static int a_refuse(gpp_ctx *c)
{
    (void)c;
    return -1;
}
static int a_nothing(gpp_ctx *c)
{
    (void)c;
    return 0;
}
static int a_relocate(gpp_ctx *c)
{
    c->begin = &buf[64];
    c->current = &buf[66];
    c->end = &buf[80];
    return 0;
}
static int a_past_end(gpp_ctx *c)
{
    c->current = &buf[4 + 30];
    c->end = &buf[4 + 20];
    return 0;
}
static int a_misalign(gpp_ctx *c)
{
    c->current = (uint32_t *)(void *)((char *)&buf[4] + 2);
    c->end = &buf[4 + 30];
    return 0;
}
static int a_null_begin(gpp_ctx *c)
{
    c->begin = NULL;
    c->current = &buf[4];
    c->end = &buf[4 + 30];
    return 0;
}

static gpp_port pc; /* large (the cache): static */

static void prepared(uint32_t first, uint32_t count, uint32_t label)
{
    dev_reset();
    CHECK(gpp_prepare(&pc, &fdev, &ctx, first, count, label) == PRG_OK);
}
static uint32_t perm(uint32_t v) /* written out independently of the port */
{
    return ((v >> 24) << 24) | (((v >> 0) & 0xffu) << 16) | (((v >> 8) & 0xffu) << 8) | ((v >> 16) & 0xffu);
}
static int words_at(unsigned at, uint32_t report, uint32_t label, uint32_t value)
{
    return buf[at + 0] == 0x00041800u && buf[at + 1] == (0x01000000u | report * 16u) && buf[at + 2] == 0x00041d6cu &&
           buf[at + 3] == label * 16u && buf[at + 4] == 0x00041d70u && buf[at + 5] == perm(value);
}

static prg_port bound(void)
{
    prg_port p;
    memset(&p, 0, sizeof p);
    gpp_bind(&p, &pc);
    return p;
}

/* ---- cases ---- */
static void c_words(void)
{
    static const uint32_t t[][3] = {{0, 64, 1}, {5, 69, 0x11223344u}, {2047, 254, 0xffffff00u}, {1000, 200, 0x00ff00ffu},
                                    {7, 100, 0x01020304u}};
    for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++) {
        buf_setup(40 - 14, 1);
        dev_reset();
        CHECK(gpp_prepare(&pc, &fdev, &ctx, t[k][0], 1, t[k][1]) == PRG_OK);
        prg_port p = bound();
        CHECK(p.reserve_pair(p.ctx, t[k][0], t[k][1], t[k][2]) == 0);
        CHECK(words_at(14, t[k][0], t[k][1], t[k][2]));
        CHECK(ctx.current == &buf[20]);
        CHECK(cb_calls == 0);
        CHECK(buf_same_except(14, 6));
        CHECK(mem_untouched());
    }
    CHECK(perm(0x11223344u) == 0x11443322u); /* the helper itself */
}
static void c_exact6(void)
{
    buf_setup(6, 1);
    prepared(10, 4, 64);
    prg_port p = bound();
    CHECK(p.reserve_pair(p.ctx, 12, 64, 9) == 0);
    CHECK(cb_calls == 0);
    CHECK(words_at(14, 12, 64, 9));
    CHECK(ctx.current == &buf[20] && ctx.current == ctx.end);
    CHECK(buf_same_except(14, 6));
}
static void c_five_room(void)
{
    buf_setup(5, 1);
    cb_action = a_room_wrap;
    prepared(10, 4, 64);
    prg_port p = bound();
    CHECK(p.reserve_pair(p.ctx, 10, 64, 3) == 0);
    CHECK(cb_calls == 1 && cb_count == 6);
    CHECK(words_at(4, 10, 64, 3));
    CHECK(ctx.current == &buf[10]);
    CHECK(buf_same_except(4, 6));
}
static void c_zero_room(void)
{
    buf_setup(0, 1);
    cb_action = a_room_extend;
    prepared(10, 4, 64);
    prg_port p = bound();
    CHECK(p.reserve_pair(p.ctx, 11, 64, 4) == 0);
    CHECK(cb_calls == 1 && cb_count == 6);
    CHECK(words_at(14, 11, 64, 4));
    CHECK(ctx.current == &buf[20]);
    CHECK(buf_same_except(14, 6));
}
static void refused(const char *what, unsigned avail, int with_cb, int (*act)(gpp_ctx *), unsigned want_calls)
{
    buf_setup(avail, with_cb);
    cb_action = act;
    prepared(10, 4, 64);
    gpp_ctx before = ctx;
    prg_port p = bound();
    int r = p.reserve_pair(p.ctx, 10, 64, 5);
    CHECK(r != 0);
    CHECK(cb_calls == want_calls);
    CHECK(buf_same_except(BUF, 0)); /* no profiler word anywhere, guards intact */
    if (act == NULL || act == a_refuse || act == a_nothing)
        CHECK(ctx.current == before.current && ctx.begin == before.begin && ctx.end == before.end);
    CHECK(mem_untouched());
    if (fails)
        printf("  (%s)\n", what);
}
static void c_no_callback(void)
{
    refused("no callback, 5 words", 5, 0, NULL, 0);
    refused("no callback, 0 words", 0, 0, NULL, 0);
}
static void c_cb_refuse(void) { refused("callback refuses", 5, 1, a_refuse, 1); }
static void c_cb_no_space(void) { refused("callback succeeds without room", 5, 1, a_nothing, 1); }
static void c_cb_invalid(void)
{
    refused("rewrite current > end", 5, 1, a_past_end, 1);
    refused("rewrite misaligned current", 5, 1, a_misalign, 1);
    refused("rewrite NULL begin", 5, 1, a_null_begin, 1);
}
static void c_relocate(void)
{
    buf_setup(5, 1);
    cb_action = a_relocate;
    prepared(10, 4, 64);
    prg_port p = bound();
    CHECK(p.reserve_pair(p.ctx, 13, 64, 0x0a0b0c0du) == 0);
    CHECK(cb_calls == 1);
    CHECK(words_at(66, 13, 64, 0x0a0b0c0du));
    CHECK(ctx.current == &buf[72]);
    CHECK(buf_same_except(66, 6)); /* region A untouched */
}
static void c_invalid_ctx_no_callback_call(void)
{
    /* an invalid context with too little room: refused WITHOUT calling the callback */
    buf_setup(5, 1);
    prepared(10, 4, 64);
    ctx.current = &buf[30];
    ctx.end = &buf[20]; /* current > end */
    prg_port p = bound();
    CHECK(p.reserve_pair(p.ctx, 10, 64, 1) != 0);
    CHECK(cb_calls == 0);
    ctx.current = (uint32_t *)(void *)((char *)&buf[14] + 2);
    ctx.end = &buf[40];
    CHECK(p.reserve_pair(p.ctx, 10, 64, 1) != 0);
    CHECK(cb_calls == 0);
    CHECK(buf_same_except(BUF, 0));
}
static void c_wrong_args(void)
{
    buf_setup(30, 1);
    prepared(10, 4, 64);
    prg_port p = bound();
    CHECK(p.reserve_pair(p.ctx, 9, 64, 1) != 0);  /* below the range */
    CHECK(p.reserve_pair(p.ctx, 14, 64, 1) != 0); /* past the range */
    CHECK(p.reserve_pair(p.ctx, 10, 65, 1) != 0); /* not the prepared label */
    CHECK(p.read_timer(p.ctx, 14) == PRG_POISON);
    CHECK(p.read_label(p.ctx, 70) == 0);
    p.write_timer(p.ctx, 9, 7);
    p.write_timer(p.ctx, 14, 7);
    p.write_label(p.ctx, 70, 7);
    CHECK(buf_same_except(BUF, 0) && ctx.current == &buf[14]);
    CHECK(mem_untouched());
}
static void c_memory_ops(void)
{
    buf_setup(30, 1);
    prepared(100, 8, 200);
    CHECK(mem_untouched()); /* prepare writes nothing */
    prg_port p = bound();
    for (uint32_t i = 0; i < 8; i++) /* stride/offset: entry i at base + 16 i, timer at offset 0 */
        CHECK(p.read_timer(p.ctx, 100 + i) == rep[100 + i].timer);
    for (uint32_t i = 0; i < 8; i++)
        p.write_timer(p.ctx, 100 + i, PRG_POISON);
    for (uint32_t i = 0; i < GPP_REPORTS + 2; i++) {
        int in = i >= 100 && i < 108;
        CHECK(rep[i].timer == (in ? PRG_POISON : rep_snap[i].timer));
        CHECK(rep[i].value == rep_snap[i].value && rep[i].zero == rep_snap[i].zero); /* adjacent fields untouched */
    }
    CHECK(p.read_label(p.ctx, 200) == lab_snap[200]);
    p.write_label(p.ctx, 200, 0);
    CHECK(lab[200] == 0 && lab[199] == lab_snap[199] && lab[201] == lab_snap[201]);
    p.flush(p.ctx);
    CHECK(flush_calls == 1 && flush_ctx == &ctx);
    p.sync(p.ctx);
    CHECK(buf_same_except(BUF, 0));
}
static void c_bind_fields(void)
{
    prg_port p;
    memset(&p, 0x5a, sizeof p);
    prg_port q = p;
    gpp_bind(&p, &pc);
    CHECK(p.ctx == &pc && p.reserve_pair && p.flush && p.read_label && p.write_label && p.read_timer &&
          p.write_timer && p.sync);
    CHECK(memcmp(&p.mftb, &q.mftb, sizeof p - offsetof(prg_port, mftb)) == 0); /* the runtime's fields untouched */
}
static int not_ready_refuses(void) /* every op refuses without a device access or callback */
{
    prg_port p = bound();
    unsigned f0 = flush_calls, c0 = cb_calls;
    uint32_t *cur0 = ctx.current;
    int ok = 1;
    if (pc.ready != 0) /* published readiness is itself the failure; the ops of such a port are not exercised */
        return 0;
    ok &= p.reserve_pair(p.ctx, pc.report_first, pc.label, 1) != 0;
    ok &= p.reserve_pair(p.ctx, 0, 64, 1) != 0;
    ok &= p.read_timer(p.ctx, 0) == PRG_POISON;
    ok &= p.read_label(p.ctx, 64) == 0;
    p.write_timer(p.ctx, 0, 1);
    p.write_label(p.ctx, 64, 1);
    p.flush(p.ctx);
    ok &= flush_calls == f0 && cb_calls == c0 && ctx.current == cur0 && mem_untouched();
    return ok;
}
static void fails_clean(uint32_t first, uint32_t count, uint32_t label, unsigned want_rep_calls, unsigned want_lab_calls)
{
    buf_setup(30, 1);
    /* start from a READY port so a refusal must also clear it */
    dev_reset();
    CHECK(gpp_prepare(&pc, &fdev, &ctx, 0, 4, 64) == PRG_OK);
    rep_calls = lab_calls = 0;
    CHECK(gpp_prepare(&pc, &fdev, &ctx, first, count, label) == PRG_EINVAL);
    if (want_rep_calls != ~0u)
        CHECK(rep_calls == want_rep_calls);
    if (want_lab_calls != ~0u)
        CHECK(lab_calls == want_lab_calls);
    CHECK(pc.ready == 0 && pc.ctx == NULL && pc.dev == NULL && pc.label_word == NULL && pc.timer[0] == NULL &&
          pc.report_count == 0);
    CHECK(not_ready_refuses());
    CHECK(mem_untouched());
}
static void c_ranges(void)
{
    fails_clean(0, 0, 64, 0, 0);                      /* count 0 */
    fails_clean(0, GPP_CACHE_MAX + 1, 64, 0, 0);      /* above the cache */
    fails_clean(GPP_REPORTS - 3, 4, 64, 0, 0);        /* past 2048 */
    fails_clean(GPP_REPORTS, 1, 64, 0, 0);            /* first index 2048 */
    fails_clean(UINT32_MAX, 1, 64, 0, 0);             /* no wrap */
    fails_clean(UINT32_MAX - 2, 4, 64, 0, 0);
    fails_clean(0x80000000u, 0x80000000u, 64, 0, 0);
    buf_setup(30, 1);
    prepared(GPP_REPORTS - 1, 1, 64); /* 2047 */
    prepared(GPP_REPORTS - GPP_CACHE_MAX, GPP_CACHE_MAX, 64);
    CHECK(rep_calls == GPP_CACHE_MAX);
    prepared(0, GPP_CACHE_MAX, 64);
}
static void c_labels(void)
{
    static const uint32_t bad[] = {0, 63, 65, 66, 67, 68, 255, 256, 0x140u /* narrows to 64 */, 0x1ffu, UINT32_MAX};
    static const uint32_t good[] = {64, 69, 128, 240, 254};
    for (unsigned k = 0; k < sizeof bad / sizeof bad[0]; k++)
        fails_clean(0, 1, bad[k], 0, 0);
    for (unsigned k = 0; k < sizeof good / sizeof good[0]; k++) {
        buf_setup(30, 1);
        prepared(0, 1, good[k]);
        CHECK(pc.label_word == &lab[good[k]]);
    }
}
static void c_mapping(void)
{
    static const int64_t at[] = {0, 2, 3}; /* first / middle / last of a 4-entry range at 20 */
    for (unsigned k = 0; k < 3; k++) {
        for (int kind = 0; kind < 3; kind++) {
            buf_setup(30, 1);
            dev_reset();
            CHECK(gpp_prepare(&pc, &fdev, &ctx, 0, 4, 64) == PRG_OK);
            rep_calls = 0;
            if (kind == 0)
                rep_fail = 20 + at[k];
            else if (kind == 1)
                rep_misalign = 20 + at[k];
            else
                rep_skew = 20 + at[k]; /* off the 16-byte stride (a skewed first entry breaks entry 1's) */
            CHECK(gpp_prepare(&pc, &fdev, &ctx, 20, 4, 64) == PRG_EINVAL);
            CHECK(pc.ready == 0 && pc.timer[0] == NULL && pc.label_word == NULL);
            CHECK(not_ready_refuses());
            CHECK(mem_untouched());
            /* retry with good mappings: ready and working */
            rep_fail = rep_misalign = rep_skew = -1;
            CHECK(gpp_prepare(&pc, &fdev, &ctx, 20, 4, 64) == PRG_OK);
            prg_port p = bound();
            p.write_timer(p.ctx, 23, 77);
            CHECK(rep[23].timer == 77 && rep[23].value == rep_snap[23].value);
            CHECK(p.reserve_pair(p.ctx, 23, 64, 2) == 0 && words_at(14, 23, 64, 2));
        }
    }
    /* a single-entry range whose only mapping is NULL (no stride to catch it) */
    buf_setup(30, 1);
    dev_reset();
    rep_fail = 20;
    CHECK(gpp_prepare(&pc, &fdev, &ctx, 20, 1, 64) == PRG_EINVAL);
    CHECK(not_ready_refuses() && mem_untouched());
    /* consistently misaligned: the stride holds, the alignment check must refuse */
    buf_setup(30, 1);
    dev_reset();
    rep_shift_all = 1;
    CHECK(gpp_prepare(&pc, &fdev, &ctx, 20, 4, 64) == PRG_EINVAL);
    CHECK(not_ready_refuses() && mem_untouched());
    /* the label mapping */
    buf_setup(30, 1);
    dev_reset();
    lab_fail = 1;
    CHECK(gpp_prepare(&pc, &fdev, &ctx, 0, 2, 64) == PRG_EINVAL && rep_calls == 0);
    CHECK(not_ready_refuses() && mem_untouched());
    lab_fail = 0;
    lab_misalign = 1;
    CHECK(gpp_prepare(&pc, &fdev, &ctx, 0, 2, 64) == PRG_EINVAL && rep_calls == 0);
    CHECK(not_ready_refuses() && mem_untouched());
    lab_misalign = 0;
    CHECK(gpp_prepare(&pc, &fdev, &ctx, 0, 2, 64) == PRG_OK);
}
static void c_bad_args(void)
{
    buf_setup(30, 1);
    dev_reset();
    gpp_dev nd = fdev;
    CHECK(gpp_prepare(NULL, &fdev, &ctx, 0, 1, 64) == PRG_EINVAL);
    CHECK(gpp_prepare(&pc, NULL, &ctx, 0, 1, 64) == PRG_EINVAL && pc.ready == 0);
    CHECK(gpp_prepare(&pc, &fdev, NULL, 0, 1, 64) == PRG_EINVAL && pc.ready == 0);
    nd.flush = NULL;
    CHECK(gpp_prepare(&pc, &nd, &ctx, 0, 1, 64) == PRG_EINVAL && pc.ready == 0);
    ctx.current = &buf[30];
    ctx.end = &buf[20];
    CHECK(gpp_prepare(&pc, &fdev, &ctx, 0, 1, 64) == PRG_EINVAL && pc.ready == 0);
    CHECK(rep_calls == 0 && lab_calls == 0 && mem_untouched());
}
static void c_release(void)
{
    buf_setup(30, 1);
    prepared(0, 4, 64);
    gpp_release(&pc);
    CHECK(not_ready_refuses());
    gpp_release(NULL);
}

static const struct {
    const char *name;
    void (*fn)(void);
} cases[] = {{"words", c_words},
             {"exact6", c_exact6},
             {"five-room", c_five_room},
             {"zero-room", c_zero_room},
             {"no-callback", c_no_callback},
             {"cb-refuse", c_cb_refuse},
             {"cb-no-space", c_cb_no_space},
             {"cb-invalid", c_cb_invalid},
             {"relocate", c_relocate},
             {"invalid-ctx", c_invalid_ctx_no_callback_call},
             {"wrong-args", c_wrong_args},
             {"memory-ops", c_memory_ops},
             {"bind-fields", c_bind_fields},
             {"ranges", c_ranges},
             {"labels", c_labels},
             {"mapping", c_mapping},
             {"bad-args", c_bad_args},
             {"release", c_release}};

int main(int argc, char **argv)
{
    int total = 0, ran = 0;
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        if (argc > 1 && strcmp(argv[1], cases[k].name) != 0)
            continue;
        ran++;
        cur_case = cases[k].name;
        fails = 0;
        int c0 = checks;
        cases[k].fn();
        printf("%s: %s (%d checks)\n", cases[k].name, fails ? "FAIL" : "PASS", checks - c0);
        total += fails;
    }
    if (ran == 0) {
        printf("unknown case\n");
        return 2;
    }
    printf("R3 PORT %s: %d checks, %d failed\n", total ? "FAIL" : "PASS", checks, total);
    return total ? 1 : 0;
}
