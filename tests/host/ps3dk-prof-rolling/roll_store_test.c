/* Physical storage foundation only: no semantic restart/checkpoint claims.
 * Breaks caught: cap overflow, newest/oldest confusion, accepting a torn commit,
 * trusting an invalid root/slot, and mutating storage on invalid payload input. */
#ifdef PFR_TEST_RED
#include "contract.h"
#else
#include "roll_store.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define REQUIRE(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)
#define MEM_BYTES 65536u
typedef struct {
    uint8_t bytes[MEM_BYTES];
    uint8_t durable[MEM_BYTES];
    uint64_t length, largest_end;
    uint64_t durable_length;
    unsigned calls, fail_call, sync_fail_call, short_limit;
    size_t tear;
    int io_failed, buffered;
} memory;
typedef struct {
    memory *m;
    pfr_store *s;
    void *scratch;
    pfr_io io;
    pfr_config cfg;
    pfr_layout layout;
} fixture;

static int64_t rd(void *ctx, uint64_t off, void *buf, size_t n)
{
    memory *m = ctx;
    if (off > m->length || (uint64_t)n > m->length - off) return -1;
    memcpy(buf, m->bytes + (size_t)off, n);
    return (int64_t)n;
}
static int64_t wr(void *ctx, uint64_t off, const void *buf, size_t n)
{
    memory *m = ctx;
    size_t take = n;
    ++m->calls;
    if (off > m->length || (uint64_t)n > m->length - off) return -1;
    if (m->io_failed) return -1;
    if (m->fail_call == m->calls) {
        take = m->tear < n ? m->tear : n;
        memcpy(m->bytes + (size_t)off, buf, take);
        m->io_failed = 1;
        return -1;
    }
    if (m->short_limit && take > m->short_limit) take = m->short_limit;
    memcpy(m->bytes + (size_t)off, buf, take);
    if (off + take > m->largest_end) m->largest_end = off + take;
    return (int64_t)take;
}
static int resize_file(void *ctx, uint64_t n)
{
    memory *m = ctx;
    if (n > MEM_BYTES) return -1;
    m->length = n;
    return 0;
}
static int size_file(void *ctx, uint64_t *n)
{ *n = ((memory *)ctx)->length; return 0; }
static int sync_file(void *ctx)
{
    memory *m = ctx;
    ++m->calls;
    if (m->io_failed || m->sync_fail_call == m->calls || m->fail_call == m->calls) {
        m->io_failed = 1;
        return -1;
    }
    if (m->buffered) {
        memcpy(m->durable, m->bytes, sizeof m->bytes);
        m->durable_length = m->length;
    }
    return 0;
}
static pfr_config config(void)
{
    pfr_config c;
    memset(&c, 0, sizeof c);
    c.max_bytes = 28672; /* 3 metadata pages + (3 retained + spare) pages. */
    c.retained_slots = 3;
    c.checkpoint_bytes = 128;
    c.payload_bytes = 256;
    c.session[0] = 0x42;
    return c;
}
static void attach(fixture *f, memory *m)
{
    memset(f, 0, sizeof *f);
    f->m = m;
    f->cfg = config();
    REQUIRE(pfr_plan(&f->cfg, &f->layout) == PFR_OK);
    REQUIRE(f->layout.file_bytes == 28672);
    REQUIRE(f->layout.slot_bytes == 4096);
    f->s = malloc(pfr_state_bytes());
    f->scratch = malloc(f->layout.scratch_bytes);
    REQUIRE(f->s && f->scratch);
    f->io.ctx = m;
    f->io.read_at = rd;
    f->io.write_at = wr;
    f->io.resize = resize_file;
    f->io.size = size_file;
    f->io.sync = sync_file;
}
static void destroy(fixture *f) { free(f->scratch); free(f->s); }
static void reset_fault(memory *m)
{ m->calls = m->fail_call = m->sync_fail_call = m->short_limit = 0; m->tear = 0; m->io_failed = 0; }
static void put32(uint8_t *p, uint32_t n)
{ unsigned i; for (i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (i * 8)); }
static void put64(uint8_t *p, uint64_t n)
{ unsigned i; for (i = 0; i < 8; ++i) p[i] = (uint8_t)(n >> (i * 8)); }
static uint64_t get64(const uint8_t *p)
{ uint64_t n = 0; unsigned i; for (i = 0; i < 8; ++i) n |= (uint64_t)p[i] << (8 * i); return n; }
/* Test encoder independent of storage; fixed IEEE CRC polynomial. */
static uint32_t crc(const uint8_t *p, size_t n)
{
    uint32_t c = UINT32_MAX;
    unsigned j;
    while (n--) {
        c ^= *p++;
        for (j = 0; j < 8; ++j) c = (c >> 1) ^ (UINT32_C(0xedb88320) & (0u - (c & 1u)));
    }
    return ~c;
}
static void chunk(uint8_t out[24], uint64_t seq)
{
    put32(out, 43); /* Opaque family type: physical layer does not own semantics. */
    put32(out + 4, 4);
    put64(out + 8, seq);
    put32(out + 16, (uint32_t)seq ^ UINT32_C(0xaabbccdd));
    put32(out + 20, crc(out, 20));
}
static int commit(fixture *f, uint64_t seq)
{
    uint8_t p[24], cp[8];
    chunk(p, seq); put64(cp, seq);
    return pfr_commit(f->s, cp, sizeof cp, p, sizeof p);
}
static void check_tail(fixture *f, uint64_t next)
{
    pfr_view v;
    uint64_t first = next > 3 ? next - 3 : 0;
    uint32_t i;
    REQUIRE(pfr_get_view(f->s, &v) == PFR_OK);
    REQUIRE(v.first_chunk == first && v.next_chunk == next);
    REQUIRE(v.first_segment == first && v.next_segment == next);
    REQUIRE(v.retained_segments == next - first);
    REQUIRE(v.evicted_segments == first && v.evicted_chunks == first);
    REQUIRE(v.evicted_payload_bytes == first * 24);
    for (i = 0; i < v.retained_segments; ++i) {
        pfr_segment seg;
        uint8_t expected[24];
        uint64_t seq = first + i;
        REQUIRE(pfr_get_segment(f->s, i, &seg) == PFR_OK);
        REQUIRE(seg.segment == seq && seg.first_chunk == seq && seg.next_chunk == seq + 1);
        REQUIRE(seg.checkpoint_bytes == 8 && get64(seg.checkpoint) == seq);
        chunk(expected, seq);
        REQUIRE(seg.payload_bytes == 24 && memcmp(seg.payload, expected, 24) == 0);
    }
    { pfr_segment end; REQUIRE(pfr_get_segment(f->s, v.retained_segments, &end) == PFR_END); }
    REQUIRE(f->m->length == 28672 && f->m->largest_end <= f->cfg.max_bytes);
}

static void test_layout(void)
{
    pfr_config c = config(); pfr_layout l;
    /* Break: cap excludes the spare slot, metadata pages, or integer overflow. */
    REQUIRE(pfr_plan(&c, &l) == PFR_OK);
    REQUIRE(l.file_bytes == 28672 && l.slot_bytes == 4096 && l.scratch_bytes >= 4096);
    c.max_bytes = 28671; REQUIRE(pfr_plan(&c, &l) == PFR_SPACE);
    c = config(); c.retained_slots = 1; REQUIRE(pfr_plan(&c, &l) == PFR_INVALID);
    c.retained_slots = 65; REQUIRE(pfr_plan(&c, &l) == PFR_INVALID);
    c = config(); c.checkpoint_bytes = UINT32_MAX; REQUIRE(pfr_plan(&c, &l) != PFR_OK);
    c = config(); c.payload_bytes = UINT32_MAX; REQUIRE(pfr_plan(&c, &l) != PFR_OK);
    c = config(); c.max_bytes = UINT64_MAX;
    REQUIRE(pfr_plan(&c, &l) == PFR_OK && l.file_bytes == 28672);
}
static void test_rotations_and_short_writes(memory *m, const char *dump)
{
    fixture f, reopened; unsigned i;
    memset(m, 0, sizeof *m); attach(&f, m);
    m->short_limit = 7;
    REQUIRE(pfr_format(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_OK);
    for (i = 0; i < 12; ++i) { REQUIRE(commit(&f, i) == PFR_OK); check_tail(&f, i + 1); }
    reset_fault(m); attach(&reopened, m);
    REQUIRE(pfr_open(reopened.s, &reopened.io, &reopened.cfg, reopened.scratch, reopened.layout.scratch_bytes) == PFR_OK);
    check_tail(&reopened, 12);
    if (dump) { FILE *fp = fopen(dump, "wb"); REQUIRE(fp); REQUIRE(fwrite(m->bytes, 1, (size_t)m->length, fp) == m->length); REQUIRE(fclose(fp) == 0); }
    destroy(&reopened); destroy(&f);
}
static void test_bad_inputs(memory *m)
{
    fixture f; uint8_t payload[48], cp[129] = {0}; unsigned before;
    memset(m, 0, sizeof *m); attach(&f, m);
    REQUIRE(pfr_format(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_OK);
    before = m->calls;
    chunk(payload, 0); chunk(payload + 24, 1);
    REQUIRE(pfr_commit(f.s, cp, 129, payload, 24) == PFR_SPACE);
    REQUIRE(pfr_commit(f.s, cp, 8, payload, 23) == PFR_INVALID);
    payload[20] ^= 1; REQUIRE(pfr_commit(f.s, cp, 8, payload, 24) == PFR_INVALID); payload[20] ^= 1;
    chunk(payload, 1); REQUIRE(pfr_commit(f.s, cp, 8, payload, 24) == PFR_INVALID);
    chunk(payload, 0); chunk(payload + 24, 2);
    REQUIRE(pfr_commit(f.s, cp, 8, payload, 48) == PFR_INVALID);
    REQUIRE(m->calls == before); /* Invalid input must not damage a committed root. */
    REQUIRE(commit(&f, 0) == PFR_OK); check_tail(&f, 1);
    destroy(&f);
}
static void test_torn_commits(memory *base, memory *work)
{
    fixture f, r; unsigned i, stage, calls; size_t t;
    const size_t tears[] = {0, 1, 31, 127, 2048, 4095, SIZE_MAX};
    memset(base, 0, sizeof *base); attach(&f, base);
    REQUIRE(pfr_format(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_OK);
    for (i = 0; i < 6; ++i) REQUIRE(commit(&f, i) == PFR_OK);
    reset_fault(base); memcpy(work, base, sizeof *work); destroy(&f);
    attach(&f, work);
    REQUIRE(pfr_open(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_OK);
    REQUIRE(commit(&f, 6) == PFR_OK); calls = work->calls; destroy(&f);
    REQUIRE(calls >= 4); /* Segment write/sync and root write/sync. */
    for (stage = 1; stage <= calls; ++stage) for (t = 0; t < sizeof tears / sizeof tears[0]; ++t) {
        pfr_view v;
        memcpy(work, base, sizeof *work); reset_fault(work); attach(&f, work);
        REQUIRE(pfr_open(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_OK);
        work->fail_call = stage; work->tear = tears[t];
        REQUIRE(commit(&f, 6) == PFR_IO);
        REQUIRE(commit(&f, 6) == PFR_FAILED); /* Do not keep writing after an uncertain commit. */
        destroy(&f); reset_fault(work); attach(&r, work);
        REQUIRE(pfr_open(r.s, &r.io, &r.cfg, r.scratch, r.layout.scratch_bytes) == PFR_OK);
        REQUIRE(pfr_get_view(r.s, &v) == PFR_OK);
        REQUIRE(v.next_chunk == 6 || v.next_chunk == 7);
        check_tail(&r, v.next_chunk);
        destroy(&r);
    }
}
static void test_torn_format(memory *work)
{
    fixture f; unsigned stage, calls; size_t t;
    const size_t tears[] = {0, 1, 2048, 4095, SIZE_MAX};
    memset(work, 0, sizeof *work); attach(&f, work);
    REQUIRE(pfr_format(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_OK);
    calls = work->calls; destroy(&f);
    for (stage = 1; stage <= calls; ++stage) for (t = 0; t < sizeof tears / sizeof tears[0]; ++t) {
        int opened;
        memset(work, 0, sizeof *work); attach(&f, work);
        work->fail_call = stage; work->tear = tears[t];
        REQUIRE(pfr_format(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_IO);
        destroy(&f); reset_fault(work); attach(&f, work);
        opened = pfr_open(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes);
        REQUIRE(opened == PFR_OK || opened == PFR_CORRUPT);
        if (opened == PFR_OK) check_tail(&f, 0);
        destroy(&f);
    }
}
static void test_volatile_power_cuts(memory *base, memory *work)
{
    fixture f; unsigned i, stage, calls;
    memset(base, 0, sizeof *base); base->buffered = 1; attach(&f, base);
    REQUIRE(pfr_format(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_OK);
    for (i = 0; i < 6; ++i) REQUIRE(commit(&f, i) == PFR_OK);
    destroy(&f); reset_fault(base); memcpy(work, base, sizeof *work); attach(&f, work);
    REQUIRE(pfr_open(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_OK);
    REQUIRE(commit(&f, 6) == PFR_OK); calls = work->calls; destroy(&f);
    for (stage = 1; stage <= calls; ++stage) {
        memcpy(work, base, sizeof *work); attach(&f, work);
        REQUIRE(pfr_open(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_OK);
        work->fail_call = stage; work->tear = SIZE_MAX;
        REQUIRE(commit(&f, 6) == PFR_IO); destroy(&f);
        /* Alternate adapter failure model: unsynced writes are volatile and
         * vanish completely on power loss, including a fully copied root. */
        memcpy(work->bytes, work->durable, sizeof work->bytes);
        work->length = work->durable_length;
        reset_fault(work); attach(&f, work);
        REQUIRE(pfr_open(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_OK);
        check_tail(&f, 6); destroy(&f);
    }
}
static void test_corruption(memory *base, memory *work)
{
    fixture f; unsigned i; pfr_view v;
    memset(base, 0, sizeof *base); attach(&f, base);
    REQUIRE(pfr_format(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_OK);
    for (i = 0; i < 6; ++i) REQUIRE(commit(&f, i) == PFR_OK);
    destroy(&f); reset_fault(base);
    /* Either root page may be corrupted: recover only a complete referenced chain. */
    for (i = 1; i <= 2; ++i) {
        memcpy(work, base, sizeof *work); work->bytes[i * 4096 + 64] ^= 0x40;
        attach(&f, work);
        REQUIRE(pfr_open(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_OK);
        REQUIRE(pfr_get_view(f.s, &v) == PFR_OK && v.recovered);
        REQUIRE(v.next_chunk == 5 || v.next_chunk == 6); check_tail(&f, v.next_chunk); destroy(&f);
    }
    memcpy(work, base, sizeof *work); work->bytes[4096] ^= 1; work->bytes[8192] ^= 1;
    attach(&f, work); REQUIRE(pfr_open(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_CORRUPT); destroy(&f);
    memcpy(work, base, sizeof *work);
    {
        unsigned readable = 0, fell_back = 0;
        /* With 3 retained of 4 slots the two roots share exactly 2 slots.
         * Corrupting either shared slot must refuse both roots; corrupting a
         * unique slot leaves exactly one complete root. */
        for (i = 0; i < 4; ++i) {
            int opened;
            memcpy(work, base, sizeof *work); work->bytes[12288 + i * 4096] ^= 1;
            attach(&f, work);
            opened = pfr_open(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes);
            REQUIRE(opened == PFR_OK || opened == PFR_CORRUPT);
            if (opened == PFR_OK) {
                ++readable; REQUIRE(pfr_get_view(f.s, &v) == PFR_OK);
                REQUIRE(v.next_chunk == 5 || v.next_chunk == 6);
                if (v.next_chunk == 5) { ++fell_back; REQUIRE(v.recovered); }
                check_tail(&f, v.next_chunk);
            }
            destroy(&f);
        }
        REQUIRE(readable == 2 && fell_back == 1);
    }
    memcpy(work, base, sizeof *work);
    for (i = 0; i < 4; ++i) work->bytes[12288 + i * 4096] ^= 1;
    attach(&f, work); REQUIRE(pfr_open(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_CORRUPT); destroy(&f);
    memcpy(work, base, sizeof *work);
    for (i = 0; i < 4; ++i) work->bytes[12288 + i * 4096 + 96] ^= 1;
    attach(&f, work); REQUIRE(pfr_open(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_CORRUPT); destroy(&f);
    memcpy(work, base, sizeof *work);
    for (i = 0; i < 4; ++i) work->bytes[12288 + i * 4096 + 104] ^= 1;
    attach(&f, work); REQUIRE(pfr_open(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_CORRUPT); destroy(&f);
    memcpy(work, base, sizeof *work); work->bytes[0] ^= 1;
    attach(&f, work); REQUIRE(pfr_open(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_CORRUPT); destroy(&f);
    memcpy(work, base, sizeof *work); --work->length;
    attach(&f, work); REQUIRE(pfr_open(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_CORRUPT); destroy(&f);
    memcpy(work, base, sizeof *work); attach(&f, work); f.cfg.session[0] ^= 1;
    REQUIRE(pfr_open(f.s, &f.io, &f.cfg, f.scratch, f.layout.scratch_bytes) == PFR_CORRUPT); destroy(&f);
}
int main(int argc, char **argv)
{
    memory *a = malloc(sizeof *a), *b = malloc(sizeof *b);
    REQUIRE(a && b);
    test_layout();
    test_rotations_and_short_writes(a, argc > 1 ? argv[1] : NULL);
    test_bad_inputs(a);
    test_torn_commits(a, b);
    test_torn_format(b);
    test_volatile_power_cuts(a, b);
    test_corruption(a, b);
    free(b); free(a);
    puts("PASS rolling physical-store foundation (semantic checkpoints not tested)");
    return 0;
}
