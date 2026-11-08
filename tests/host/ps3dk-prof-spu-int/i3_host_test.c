/* P3.2 I3a host checks (design I3 r2 sections 1-3): the raw atomic-status translation, the core's call sites under
 * PSP_PORT_HOSTRAW (each site names its operation; a wrong raw value latches FAULT; one status read per atomic
 * command, error paths included), the SPU writer's attach validation and its descriptor-index check, built on the
 * host over the port table (PS3DK_PROF_SPU_HOST_TEST).  Host logic only: not target evidence.
 *
 * usage: i3h all | i3h identity (the negative control: an identity translation must FAIL the table) |
 *        i3h no-records (built with -DNO_RECORDS: attach must refuse a missing descriptor section) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prof_spu_lane.h"
#include "psp_as.h"

static int g_bad;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("I3H FAIL line %d: ", __LINE__);                                                                   \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            g_bad++;                                                                                                   \
        }                                                                                                              \
    } while (0)

#ifndef NO_RECORDS
/* five 16-byte records in the descriptor section (contents are irrelevant to the writer) */
static const unsigned char g_recs[5][16] __attribute__((section("ps3dkprof_spudesc"), aligned(16), used)) = {{1}};
#endif

/* ---- the host port: real memory, scripted raw atomic status ---- */
static unsigned g_last_op, g_atomic_cmds, g_status_reads, g_ops, g_blocking;
static int g_override_read;   /* 1-based status read to override (0 = none) */
static uint32_t g_override_raw;
static uint32_t g_dec = 0xFFFFF000u;
static unsigned char *at(uint64_t ea) { return (unsigned char *)(uintptr_t)ea; }
static uint32_t rd_dec(void *c) { (void)c; g_ops++; return g_dec -= 3; }
static void wr_dec(void *c, uint32_t v) { (void)c; g_ops++; g_dec = v; }
static int blocking(void *c) { (void)c; g_ops++; return g_blocking; }
static uint32_t rd_mask(void *c) { (void)c; g_ops++; return 0x5; }
static void wr_mask(void *c, uint32_t m) { (void)c; (void)m; g_ops++; }
static uint32_t tag_wait(void *c) { (void)c; g_ops++; return 0xFFFFFFFFu; }
static void getllar(void *c, uint64_t ea, void *ls) { (void)c; g_ops++; g_atomic_cmds++; g_last_op = PSP_OP_GETLLAR; memcpy(ls, at(ea), 128); }
static void putllc(void *c, uint64_t ea, const void *ls) { (void)c; g_ops++; g_atomic_cmds++; g_last_op = PSP_OP_PUTLLC; memcpy(at(ea), ls, 128); }
static void putlluc(void *c, uint64_t ea, const void *ls) { (void)c; g_ops++; g_atomic_cmds++; g_last_op = PSP_OP_PUTLLUC; memcpy(at(ea), ls, 128); }
static uint32_t rd_stat(void *c)
{
    (void)c;
    g_ops++;
    g_status_reads++;
    if (g_override_read && (int)g_status_reads == g_override_read)
        return g_override_raw;
    return g_last_op == PSP_OP_GETLLAR ? 4u : g_last_op == PSP_OP_PUTLLC ? 0u : g_last_op == PSP_OP_PUTLLUC ? 2u : 0xEEu;
}
static void put(void *c, const void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
    (void)c;
    (void)tag;
    g_ops++;
    memcpy(at(ea), ls, size);
}
static const psp_port g_port = {NULL, rd_dec, wr_dec, blocking, rd_mask, wr_mask, tag_wait, getllar, putllc, putlluc,
                                rd_stat, put};
const psp_port *ps3dk_prof__spu_host_port = &g_port;

static unsigned char g_ring[64 * 8] __attribute__((aligned(128)));
static unsigned char g_ctl[256] __attribute__((aligned(128)));
static unsigned char g_stage[PS3DK_PROF_SPU_STAGING_BYTES(16)] __attribute__((aligned(16)));
static ps3dk_prof_spu_writer g_w __attribute__((aligned(128)));

static void reset(void)
{
    memset(g_ring, 0, sizeof g_ring);
    memset(g_ctl, 0, sizeof g_ctl);
    g_last_op = g_atomic_cmds = g_status_reads = g_ops = 0;
    g_override_read = 0;
    g_blocking = 0;
}
static int attach_default(ps3dk_prof_spu_writer *w)
{
    return ps3dk_prof_spu_attach(w, (uint64_t)(uintptr_t)g_ring, 64, (uint64_t)(uintptr_t)g_ctl, 3, g_stage, 16, 0);
}

/* ---- 1. the translation table ---- */
static uint32_t identity(int op, uint32_t raw) { (void)op; return raw; }
static uint32_t expect_tr(int op, uint32_t raw)
{
    if (op == PSP_OP_GETLLAR)
        return raw == 4 ? PSP_AS_GETLLAR : 0;
    if (op == PSP_OP_PUTLLC)
        return raw == 0 ? PSP_AS_PUTLLC_OK : raw == 1 ? PSP_AS_PUTLLC_LOST : 0;
    if (op == PSP_OP_PUTLLUC)
        return raw == 2 ? PSP_AS_PUTLLUC : 0;
    return 0;
}
static int table(uint32_t (*tr)(int, uint32_t))
{
    int ops[] = {0, PSP_OP_GETLLAR, PSP_OP_PUTLLC, PSP_OP_PUTLLUC, 4, -1}, bad = 0, rows = 0;
    for (unsigned k = 0; k < sizeof ops / sizeof ops[0]; k++)
        for (unsigned v = 0; v < 16 + 28; v++) {
            uint32_t raw = v < 16 ? v : 1u << (v - 12); /* 0..15, then each of bits 4..31 alone */
            rows++;
            if (tr(ops[k], raw) != expect_tr(ops[k], raw))
                bad++;
        }
    printf("TABLE rows %d mismatches %d\n", rows, bad);
    return bad;
}

/* ---- 2. the core's call sites under PSP_PORT_HOSTRAW ---- */
static void core_sites(void)
{
    static const uint32_t raws[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 0x10, 0x100, 0x80000000u, 0xFFFFFFFFu};
    static const char *site[] = {"", "getllar", "putllc", "putlluc"};
    psp_spu s;
    int rc, cases = 0;
    reset();
    rc = psp_spu_init(&s, &g_port, (uint64_t)(uintptr_t)g_ring, 64, (uint64_t)(uintptr_t)g_ctl, 3, g_stage, 16, 0);
    CHECK(rc == PSP_OK && g_atomic_cmds == 3 && g_status_reads == 3, "normal raws: init OK, 3 commands, 3 reads (%d)", rc);
    for (int read = 1; read <= 3; read++)
        for (unsigned k = 0; k < sizeof raws / sizeof raws[0]; k++) {
            uint32_t raw = raws[k];
            int accepted = (read == 1 && raw == 4) || (read == 2 && raw <= 1) || (read == 3 && raw == 2);
            reset();
            g_override_read = read;
            g_override_raw = raw;
            rc = psp_spu_init(&s, &g_port, (uint64_t)(uintptr_t)g_ring, 64, (uint64_t)(uintptr_t)g_ctl, 3, g_stage, 16, 0);
            cases++;
            CHECK(g_atomic_cmds == g_status_reads, "read %d raw 0x%x: commands %u != status reads %u", read, raw,
                  g_atomic_cmds, g_status_reads);
            if (accepted)
                CHECK(rc == PSP_OK, "%s raw 0x%x must be accepted (%d)", site[read], raw, rc);
            else
                CHECK(rc == PSP_EFAULT && (s.flags & PSP_F_FAULT), "%s raw 0x%x must latch FAULT (%d)", site[read], raw, rc);
        }
    printf("CORE-SITES cases %d\n", cases);
}

/* ---- 3. attach validation: EINVAL before any write to w or any port operation ---- */
static void attach_checks(void)
{
    static unsigned char raw_w[sizeof(ps3dk_prof_spu_writer) + 256] __attribute__((aligned(128)));
    ps3dk_prof_spu_writer *mis = (ps3dk_prof_spu_writer *)(void *)(raw_w + 16);
    uint64_t ring = (uint64_t)(uintptr_t)g_ring, ctl = (uint64_t)(uintptr_t)g_ctl;
    struct { const char *what; int rc; } r[32];
    int n = 0;
    unsigned char snap[sizeof g_w];
#define TRY(label, call)                                                                                                \
    do {                                                                                                               \
        reset();                                                                                                       \
        memset(&g_w, 0xAB, sizeof g_w);                                                                                \
        memcpy(snap, &g_w, sizeof g_w);                                                                                \
        r[n].what = label;                                                                                              \
        r[n].rc = (call);                                                                                              \
        CHECK(r[n].rc == PS3DK_PROF_SPU_EINVAL, "%s: rc %d", label, r[n].rc);                                         \
        CHECK(!memcmp(snap, &g_w, sizeof g_w), "%s: the writer was written", label);                                  \
        CHECK(g_ops == 0, "%s: %u port operations", label, g_ops);                                                    \
        n++;                                                                                                           \
    } while (0)
    TRY("null writer", ps3dk_prof_spu_attach(NULL, ring, 64, ctl, 3, g_stage, 16, 0));
    TRY("misaligned writer", ps3dk_prof_spu_attach(mis, ring, 64, ctl, 3, g_stage, 16, 0));
    TRY("null staging", ps3dk_prof_spu_attach(&g_w, ring, 64, ctl, 3, NULL, 16, 0));
    TRY("misaligned staging", ps3dk_prof_spu_attach(&g_w, ring, 64, ctl, 3, g_stage + 8, 16, 0));
    TRY("tag 32", ps3dk_prof_spu_attach(&g_w, ring, 64, ctl, 32, g_stage, 16, 0));
    TRY("ls_cap 3", ps3dk_prof_spu_attach(&g_w, ring, 64, ctl, 3, g_stage, 3, 0));
    TRY("ls_cap 5", ps3dk_prof_spu_attach(&g_w, ring, 64, ctl, 3, g_stage, 5, 0));
    TRY("ls_cap 4098", ps3dk_prof_spu_attach(&g_w, ring, 64, ctl, 3, g_stage, 4098, 0));
    TRY("ring 4", ps3dk_prof_spu_attach(&g_w, ring, 4, ctl, 3, g_stage, 16, 0));
    TRY("ring 48", ps3dk_prof_spu_attach(&g_w, ring, 48, ctl, 3, g_stage, 16, 0));
    TRY("ring 2^21", ps3dk_prof_spu_attach(&g_w, ring, 1u << 21, ctl, 3, g_stage, 16, 0));
    TRY("ea_ring +16", ps3dk_prof_spu_attach(&g_w, ring + 16, 64, ctl, 3, g_stage, 16, 0));
    TRY("ea_ctl +64", ps3dk_prof_spu_attach(&g_w, ring, 64, ctl + 64, 3, g_stage, 16, 0));
    TRY("flags 2", ps3dk_prof_spu_attach(&g_w, ring, 64, ctl, 3, g_stage, 16, 2));
    TRY("flags 0x80000000", ps3dk_prof_spu_attach(&g_w, ring, 64, ctl, 3, g_stage, 16, 0x80000000u));
    printf("ATTACH-EINVAL cases %d\n", n);
    /* writer/staging overlap, inside ONE aligned backing array (every address is a valid interior pointer):
     * staging (ls_cap 4: 96 bytes) starting inside the writer, at it, ending inside it, and both adjacent placements */
    {
        static unsigned char back[4096] __attribute__((aligned(128)));
        ps3dk_prof_spu_writer *bw = (ps3dk_prof_spu_writer *)(void *)(back + 1024);
        unsigned char *wb = back + 1024;
        size_t ws = sizeof(ps3dk_prof_spu_writer), ss = PS3DK_PROF_SPU_STAGING_BYTES(4);
        struct {
            const char *what;
            unsigned char *st;
            int want;
        } ov[] = {
            {"staging starts inside the writer", wb + 256, PS3DK_PROF_SPU_EINVAL},
            {"staging starts at the writer", wb, PS3DK_PROF_SPU_EINVAL},
            {"staging ends inside the writer", wb - 16, PS3DK_PROF_SPU_EINVAL},
            {"staging ends one slot into the writer", wb - ss + 16, PS3DK_PROF_SPU_EINVAL},
            {"staging adjacent before the writer", wb - ss, PS3DK_PROF_SPU_OK},
            {"staging adjacent after the writer", wb + ws, PS3DK_PROF_SPU_OK},
        };
        for (unsigned k = 0; k < sizeof ov / sizeof ov[0]; k++) {
            int rc;
            reset();
            memset(bw, 0xAB, ws);
            memcpy(snap, bw, ws);
            rc = ps3dk_prof_spu_attach(bw, ring, 64, ctl, 3, ov[k].st, 4, 0);
            CHECK(rc == ov[k].want, "%s: rc %d, want %d", ov[k].what, rc, ov[k].want);
            if (ov[k].want == PS3DK_PROF_SPU_EINVAL)
                CHECK(!memcmp(snap, bw, ws) && g_ops == 0, "%s: the writer was written or a port op ran", ov[k].what);
        }
        printf("ATTACH-OVERLAP cases %u\n", (unsigned)(sizeof ov / sizeof ov[0]));
    }
    /* EBUSY: blocking events enabled -> nothing published; the same writer attaches after they are disabled */
    reset();
    g_blocking = 1;
    CHECK(attach_default(&g_w) == PS3DK_PROF_SPU_EBUSY && g_atomic_cmds == 0, "EBUSY with no atomic command");
    g_blocking = 0;
    CHECK(attach_default(&g_w) == PS3DK_PROF_SPU_OK && g_w.desc_count == 5, "retry on the same writer: OK, count 5");
    CHECK(attach_default(&g_w) == PS3DK_PROF_SPU_ECLOSED, "a second attach to the same lane: ECLOSED");
    CHECK(g_dec != 0x12345678u, "observe-only");
}

/* ---- 4. the descriptor index check before narrowing ---- */
static void index_checks(void)
{
    static const uint32_t badv[] = {5, 65535, 65536, UINT32_MAX};
    uint64_t q;
    reset();
    CHECK(attach_default(&g_w) == PS3DK_PROF_SPU_OK, "attach");
    q = g_w.s.seq;
    ps3dk_prof_spu_begin(&g_w, 4); /* count - 1 */
    ps3dk_prof_spu_end(&g_w, 4);
    ps3dk_prof_spu_mark(&g_w, 0);
    CHECK(g_w.s.seq == q + 3 && g_w.bad_index == 0, "count-1 and 0 are recorded (seq %llu)", (unsigned long long)g_w.s.seq);
    CHECK(ps3dk_prof_spu_flush(&g_w) == PS3DK_PROF_SPU_OK, "flush");
    for (unsigned k = 0; k < sizeof badv / sizeof badv[0]; k++)
        for (int fn = 0; fn < 3; fn++) {
            reset();
            CHECK(attach_default(&g_w) == PS3DK_PROF_SPU_OK, "attach");
            q = g_w.s.seq;
            if (fn == 0)
                ps3dk_prof_spu_begin(&g_w, badv[k]);
            else if (fn == 1)
                ps3dk_prof_spu_end(&g_w, badv[k]);
            else
                ps3dk_prof_spu_mark(&g_w, badv[k]);
            CHECK(g_w.s.seq == q && g_w.s.n == 0, "index %u fn %d: nothing recorded", badv[k], fn);
            CHECK(g_w.bad_index == 1 && (g_w.s.flags & PSP_F_FAULT), "index %u fn %d: counted and latched", badv[k], fn);
            ps3dk_prof_spu_mark(&g_w, 0);
            CHECK(g_w.s.seq == q, "index %u fn %d: a later valid event is not admitted", badv[k], fn);
            CHECK(ps3dk_prof_spu_flush(&g_w) == PS3DK_PROF_SPU_EFAULT, "index %u fn %d: flush EFAULT", badv[k], fn);
            CHECK(ps3dk_prof_spu_fini(&g_w) == PS3DK_PROF_SPU_EFAULT, "index %u fn %d: fini EFAULT", badv[k], fn);
        }
    printf("INDEX cases %u\n", (unsigned)(sizeof badv / sizeof badv[0]) * 3u + 1u);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "all";
    if (!strcmp(mode, "identity")) {
        if (table(identity)) {
            printf("I3H FAIL: the identity translation disagrees with the table\n");
            return 1;
        }
        printf("I3H PASS\n");
        return 0;
    }
#ifdef NO_RECORDS
    (void)core_sites;
    (void)attach_checks;
    (void)index_checks;
    reset();
    CHECK(attach_default(&g_w) == PS3DK_PROF_SPU_EINVAL && g_ops == 0, "no descriptor section: EINVAL, no port op");
#else
    CHECK(table(psp_as_translate) == 0, "the translation table");
    core_sites();
    attach_checks();
    index_checks();
#endif
    if (g_bad)
        return 1;
    printf("I3H PASS\n");
    return 0;
}
