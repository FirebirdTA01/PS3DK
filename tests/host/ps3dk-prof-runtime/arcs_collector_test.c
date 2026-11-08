/* Exact caller-owned collector tests. The fixture alone seeds large, valid
 * counters to exercise overflow without billions of events; no test API is
 * added to production. Invariants are checked after every record call.
 */
#include "arcs.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)
struct fixture {
    struct ps3dk_prof_arc_collector c;
    struct ps3prof_arc_row rows[8];
    uint32_t index[16];
};
static int init(struct fixture *f, uint32_t capacity) {
    memset(f, 0, sizeof *f);
    int rc = ps3dk_prof__arcs_init(&f->c, f->rows, capacity, f->index, 16);
    CHECK(rc == 0);
    return rc == 0;
}
static void invariant(const struct fixture *f) {
    unsigned __int128 sum = 0;
    CHECK(f->c.row_count <= f->c.capacity);
    for (uint32_t i = 0; i < f->c.row_count && i < 8; ++i)
        sum += f->rows[i].count;
    CHECK(sum == f->c.recorded);
    CHECK((unsigned __int128)f->c.recorded + f->c.refused == f->c.attempted);
}
static void event(struct fixture *f, uint64_t caller, uint64_t callee,
                  uint32_t flags, int expected) {
    CHECK(ps3dk_prof__arcs_record(&f->c, caller, callee, flags) == expected);
    invariant(f);
}
static uint64_t count(const struct fixture *f, uint64_t caller, uint64_t callee) {
    for (uint32_t i = 0; i < f->c.row_count; ++i)
        if (f->rows[i].caller_return_pc == caller && f->rows[i].callee_hook_pc == callee)
            return f->rows[i].count;
    return 0;
}
static int write_bytes(void *ctx, const void *data, size_t n) {
    return fwrite(data, 1, n, ctx) == n ? 0 : -1;
}
static void capture(const char *directory, const char *name,
                    const struct fixture *a, const struct fixture *b) {
    struct ps3prof_arc_thread threads[2] = {
        {1, 0, 42, "worker-a", a->rows, a->c.row_count, 2, a->c.limit_flags,
         a->c.attempted, a->c.recorded, a->c.refused, 0},
        {2, 1, 43, "worker-b", b ? b->rows : NULL, b ? b->c.row_count : 0, 2,
         b ? b->c.limit_flags : 0, b ? b->c.attempted : 0,
         b ? b->c.recorded : 0, b ? b->c.refused : 0, 0}
    };
    struct ps3prof_arc_snapshot snapshot = {
        "collector", (const unsigned char *)"build", 5, "sdk", "host-fixture", "-pg", name,
        1, 1, 1, 17, 1, 0, UINT64_MAX, 0, 2, 8, 1000, 10, threads, b ? 2 : 1, 0, 0
    };
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/%s.arcs", directory, name);
    CHECK(n > 0 && (size_t)n < sizeof path);
    FILE *out = fopen(path, "wb");
    CHECK(out != NULL);
    if (out) { CHECK(ps3prof_arcs_write(&snapshot, write_bytes, out) == 0); CHECK(fclose(out) == 0); }
}
static void initialization(void) {
    struct fixture f = {0}, saved;
    CHECK(ps3dk_prof__arcs_init(NULL, f.rows, 1, f.index, 16) == PS3DK_PROF_ARC_INVALID);
    CHECK(ps3dk_prof__arcs_init(&f.c, NULL, 1, f.index, 16) == PS3DK_PROF_ARC_INVALID);
    CHECK(ps3dk_prof__arcs_init(&f.c, f.rows, 0, f.index, 16) == PS3DK_PROF_ARC_INVALID);
    CHECK(ps3dk_prof__arcs_init(&f.c, f.rows, 65537, f.index, 16) == PS3DK_PROF_ARC_INVALID);
    CHECK(ps3dk_prof__arcs_init(&f.c, f.rows, 8, NULL, 16) == PS3DK_PROF_ARC_INVALID);
    CHECK(ps3dk_prof__arcs_init(&f.c, f.rows, 8, f.index, 15) == PS3DK_PROF_ARC_INVALID);
    CHECK(ps3dk_prof__arcs_init(&f.c, f.rows, 8, f.index, 8) == PS3DK_PROF_ARC_INVALID);
    CHECK(ps3dk_prof__arcs_record(&f.c, 4, 8, 0) == PS3DK_PROF_ARC_STATE);
    CHECK(ps3dk_prof__arcs_record(NULL, 4, 8, 0) == PS3DK_PROF_ARC_STATE);
    if (!init(&f, 8)) return;
    saved = f;
    CHECK(ps3dk_prof__arcs_init(&f.c, f.rows, 8, f.index, 16) == PS3DK_PROF_ARC_STATE);
    CHECK(memcmp(&f, &saved, sizeof f) == 0);
}
static void workload(const char *out) {
    struct fixture a, b;
    if (!init(&a, 8) || !init(&b, 8)) return;
    for (unsigned worker = 0; worker < 2; ++worker) {
        struct fixture *f = worker ? &b : &a;
        for (unsigned i = 0; i < 10; ++i) {
            event(f, 0x1004, 0x2004, 0, 0);
            event(f, 0x2008, 0x3004, 0, 0);
            event(f, 0x2008, 0x3004, 0, 0);
        }
        CHECK(count(f, 0x1004, 0x2004) == 10);
        CHECK(count(f, 0x2008, 0x3004) == 20);
        event(f, 0x3008, 0x3004, 0, 0); /* recursion */
        event(f, 0x2008, 0x4004, 0, 0); /* indirect target */
        event(f, 0, 0x4004, 1, 0);      /* unknown caller remains distinct */
        CHECK(f->c.row_count == 5 && f->c.attempted == 33 && f->c.refused == 0);
    }
    capture(out, "workers", &a, &b);
}
static void capacity(const char *out) {
    struct fixture f;
    if (!init(&f, 1)) return;
    event(&f, 4, 8, 0, 0);
    event(&f, 4, 12, 0, PS3DK_PROF_ARC_REFUSED);
    event(&f, 4, 8, 0, 0); /* existing key still admitted at capacity */
    CHECK(f.c.row_count == 1 && f.rows[0].count == 2 && f.c.refused == 1);
    capture(out, "capacity", &f, NULL);
    event(&f, 0, 8, 0, PS3DK_PROF_ARC_REFUSED);
    event(&f, 4, 8, 1, PS3DK_PROF_ARC_REFUSED);
    event(&f, 5, 8, 0, PS3DK_PROF_ARC_REFUSED);
    event(&f, 4, 9, 0, PS3DK_PROF_ARC_REFUSED);
    event(&f, 4, 0, 0, PS3DK_PROF_ARC_REFUSED);
    event(&f, 4, 8, 2, PS3DK_PROF_ARC_REFUSED);
    CHECK(f.c.refused == 7 && f.c.recorded == 2 && f.c.attempted == 9);
    CHECK(f.c.limit_flags == 0);
}
static void freeze(struct fixture *f, uint64_t caller, uint64_t callee) {
    struct fixture before = *f;
    event(f, caller, callee, 0, PS3DK_PROF_ARC_LIMIT);
    CHECK(f->c.limit_flags == 1);
    before.c.limit_flags = 1;
    CHECK(memcmp(&before, f, sizeof before) == 0);
    event(f, 0, 0, 7, PS3DK_PROF_ARC_LIMIT);
    event(f, 12, 16, 0, PS3DK_PROF_ARC_LIMIT);
    CHECK(memcmp(&before, f, sizeof before) == 0);
}
static void limits(const char *out) {
    struct fixture f;
    if (!init(&f, 8)) return;
    event(&f, 4, 8, 0, 0);
    if (f.c.row_count != 1) return;
    f.c.attempted = f.c.recorded = f.rows[0].count = UINT64_MAX - 1;
    invariant(&f);
    event(&f, 4, 8, 0, 0);
    CHECK(f.rows[0].count == UINT64_MAX && f.c.limit_flags == 0);
    capture(out, "max-exact", &f, NULL);
    freeze(&f, 4, 8);
    capture(out, "hot-limit", &f, NULL);
    if (!init(&f, 8)) return;
    event(&f, 4, 8, 0, 0);
    event(&f, 4, 12, 0, 0);
    if (f.c.row_count != 2) return;
    f.rows[0].count = UINT64_MAX - 2;
    f.c.attempted = f.c.recorded = UINT64_MAX - 1;
    invariant(&f);
    event(&f, 4, 12, 0, 0);
    CHECK(f.rows[1].count == 2 && f.c.recorded == UINT64_MAX);
    freeze(&f, 4, 8);
    if (!init(&f, 1)) return;
    f.c.attempted = f.c.refused = UINT64_MAX - 1;
    invariant(&f);
    event(&f, 0, 0, 0, PS3DK_PROF_ARC_REFUSED);
    CHECK(f.c.refused == UINT64_MAX && f.c.row_count == 0);
    freeze(&f, 0, 0);
    capture(out, "refusal-limit", &f, NULL);
    if (!init(&f, 1)) return;
    event(&f, 4, 8, 0, 0);
    f.c.attempted = UINT64_MAX - 1; f.c.refused = UINT64_MAX - 2;
    invariant(&f);
    event(&f, 4, 8, 0, 0);
    CHECK(f.c.recorded == 2 && f.c.attempted == UINT64_MAX);
    freeze(&f, 4, 12);
}
static void collisions(void) {
    /* Discover a collision from actual bucket placement, without duplicating
     * the implementation's hash. Seventeen keys in sixteen buckets guarantee
     * a pair; the second lookup must still compare the full key. */
    uint64_t seen[16] = {0};
    struct fixture f;
    int found = 0;
    for (uint64_t key = 4; key <= 68 && !found; key += 4) {
        if (!init(&f, 8)) return;
        event(&f, key, 0x1004, 0, 0);
        for (unsigned bucket = 0; bucket < 16; ++bucket) {
            if (!f.index[bucket]) continue;
            if (seen[bucket]) {
                uint64_t other = seen[bucket];
                if (!init(&f, 8)) return;
                event(&f, other, 0x1004, 0, 0);
                event(&f, key, 0x1004, 0, 0);
                event(&f, key, 0x1004, 0, 0);
                CHECK(f.c.row_count == 2 && count(&f, other, 0x1004) == 1 && count(&f, key, 0x1004) == 2);
                found = 1;
            } else seen[bucket] = key;
            break;
        }
    }
    CHECK(found);
    if (!init(&f, 8)) return;
    for (unsigned i = 1; i <= 8; ++i) event(&f, i * 4, 0x1004, 0, 0);
    event(&f, 4, 0x1004, 0, 0);
    event(&f, 36, 0x1004, 0, PS3DK_PROF_ARC_REFUSED);
    CHECK(f.c.row_count == 8 && f.c.recorded == 9 && f.c.refused == 1);
    if (!init(&f, 8)) return;
    event(&f, 4, 8, 0, 0);
    event(&f, UINT64_C(0x100000004), 8, 0, 0);
    event(&f, 4, UINT64_C(0x100000008), 0, 0);
    CHECK(f.c.row_count == 3); /* never truncate either PC to pointer width */
}
static void max_capacity(void) {
    struct ps3dk_prof_arc_collector c = {0};
    struct ps3prof_arc_row *rows = calloc(65536, sizeof *rows);
    uint32_t *index = calloc(131072, sizeof *index);
    CHECK(rows != NULL && index != NULL);
    if (rows && index) {
        CHECK(ps3dk_prof__arcs_init(&c, rows, 65536, index, 131072) == 0);
        CHECK(ps3dk_prof__arcs_record(&c, 4, 8, 0) == 0);
        CHECK(c.row_count == 1 && rows[0].count == 1 && c.attempted == 1 && c.recorded == 1 && c.refused == 0);
    }
    free(rows); free(index);
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    initialization(); workload(argv[1]); capacity(argv[1]); limits(argv[1]);
    collisions(); max_capacity();
    printf("collector tests=%u pass=%u fail=%u\n", checks, checks-failures, failures);
    return failures ? 1 : 0;
}
