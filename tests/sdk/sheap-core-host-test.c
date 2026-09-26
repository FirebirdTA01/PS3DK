/* Host unit test for the portable libsheap core (sdk/libsheap/src/core).
 *
 * The core is pure arithmetic over a node-state accessor, so it runs here
 * over a flat array of tree words.  Checked: the heap geometry for the
 * sizes the samples use, the argument edges, the fence marks as raw tree
 * words (which pins the two-bit packing), the documented four-step
 * allocation example, the never-allocated rightmost node, the Free error
 * paths, the key-table state machine, and a long random alloc/free run
 * against an independent leaf-bitmap model with full tree invariants.
 *
 * Built and run by tests/sdk/sheap-core-host-test.sh.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cell/sheap/error.h>
#include "core/sheap_core.h"
#include "sheap_layout.h"

static int failures, checks;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        ++checks;                                                           \
        if (!(cond)) {                                                      \
            ++failures;                                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

#define ERR(code) ((int)(code))

/* ---- flat tree ------------------------------------------------------ */

#define MAX_WORDS (1u << 16)
static uint32_t words[MAX_WORDS];

static unsigned flat_get(void *ctx, uint32_t n)
{
    (void)ctx;
    if (SHEAP_TREE_WORD(n) >= MAX_WORDS) {
        printf("FAIL: node %u outside the test tree\n", n);
        exit(1);
    }
    return (words[SHEAP_TREE_WORD(n)] >> SHEAP_TREE_SHIFT(n)) & 3u;
}

static void flat_set(void *ctx, uint32_t n, unsigned s)
{
    uint32_t *w;

    (void)ctx;
    if (SHEAP_TREE_WORD(n) >= MAX_WORDS) {
        printf("FAIL: node %u outside the test tree\n", n);
        exit(1);
    }
    w = &words[SHEAP_TREE_WORD(n)];
    *w = (*w & ~(3u << SHEAP_TREE_SHIFT(n))) | ((uint32_t)s << SHEAP_TREE_SHIFT(n));
}

static const sheap_tree_access flat = { flat_get, flat_set, NULL };

static void fresh_tree(uint32_t n_nodes)
{
    memset(words, 0, sizeof(words));
    __sheap_tree_fence(&flat, n_nodes);
}

/* ---- 1. layout ------------------------------------------------------ */

struct layout_case {
    const char *name;
    int keyed;
    uint64_t size;
    uint32_t n_nodes;
    uint64_t s_tree, s_buffer, s_root, tree_at, heap_at;
};

static void test_layout(void)
{
    static const struct layout_case cases[] = {
        { "Sheap 10240",    0, 10240, 205, 128,  9984, 16384,  128,  256 },
        { "KeySheap 10240", 1, 10240, 125, 128,  7936,  8192, 2176, 2304 },
        { "Sheap 20480",    0, 20480, 413, 128, 20224, 32768,  128,  256 },
        { "Sheap 2048",     0,  2048,  29, 128,  1792,  2048,  128,  256 },
    };
    unsigned i;

    CHECK(sizeof(sheap_header) == 128, "header is %zu bytes", sizeof(sheap_header));
    CHECK(offsetof(sheap_header, spu_tag1) == 64 && offsetof(sheap_header, ea_keytable) == 72,
          "header tag/keytable offsets");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const struct layout_case *c = &cases[i];
        uint64_t prefix = SHEAP_HEADER_BYTES + (c->keyed ? SHEAP_KEYTABLE_BYTES : 0);
        uint64_t span = c->keyed ? __sheap_keyed_span(c->size) : __sheap_plain_span(c->size);
        sheap_geometry g;
        int rc = __sheap_geometry(span, &g);

        CHECK(rc == 0, "%s: geometry failed", c->name);
        if (rc)
            continue;
        CHECK(g.n_nodes == c->n_nodes, "%s: n_nodes %u want %u", c->name, g.n_nodes, c->n_nodes);
        CHECK(g.s_tree == c->s_tree, "%s: s_tree %" PRIu64, c->name, g.s_tree);
        CHECK(g.s_buffer == c->s_buffer, "%s: s_buffer %" PRIu64, c->name, g.s_buffer);
        CHECK(g.s_root == c->s_root, "%s: s_root %" PRIu64, c->name, g.s_root);
        CHECK(prefix == c->tree_at, "%s: tree at +%" PRIu64, c->name, prefix);
        CHECK(prefix + g.s_tree == c->heap_at, "%s: heap at +%" PRIu64, c->name, prefix + g.s_tree);
    }

    /* Argument edges shared by both Initialize functions. */
    CHECK(__sheap_check_init_args(0x10000, 2047, 0) == ERR(CELL_SHEAP_ERROR_INVAL), "2047 bytes");
    CHECK(__sheap_check_init_args(0x10000, 2048, 0) == 0, "2048 bytes");
    CHECK(__sheap_check_init_args(0x10000, UINT64_C(0x8000000001), 0) == ERR(CELL_SHEAP_ERROR_INVAL),
          "size above 0x80_0000_0000");
    CHECK(__sheap_check_init_args(0x10000, UINT64_C(0x8000000000), 0) == 0, "size 0x80_0000_0000 args");
    CHECK(__sheap_check_init_args(0x10000, 4096, 31) == 0, "tag 31");
    CHECK(__sheap_check_init_args(0x10000, 4096, 32) == ERR(CELL_SHEAP_ERROR_INVAL), "tag 32");
    CHECK(__sheap_check_init_args(0, 4096, 0) == ERR(CELL_SHEAP_ERROR_INVAL), "ea 0");
    CHECK(__sheap_check_init_args(0x10040, 4096, 0) == ERR(CELL_SHEAP_ERROR_ALIGN), "misaligned ea");
    {
        sheap_geometry g;

        /* The largest accepted size still has too many nodes. */
        CHECK(__sheap_geometry(__sheap_plain_span(UINT64_C(0x8000000000)), &g) == -1,
              "0x80_0000_0000 node count");
        /* Keyed: 2176 leaves nothing, 2048 wraps. */
        CHECK(__sheap_geometry(__sheap_keyed_span(2176), &g) == -1, "keyed 2176");
        CHECK(__sheap_geometry(__sheap_keyed_span(2048), &g) == -1, "keyed 2048 wraps");
        CHECK(__sheap_geometry(__sheap_keyed_span(2560), &g) == 0 && g.n_nodes == 3,
              "keyed 2560 is usable (n_nodes %u)", g.n_nodes);
    }

    CHECK(__sheap_size_to_row(0, 16384) == 7 && __sheap_size_to_row(1, 16384) == 7
          && __sheap_size_to_row(128, 16384) == 7 && __sheap_size_to_row(129, 16384) == 6
          && __sheap_size_to_row(16384, 16384) == 0 && __sheap_size_to_row(16385, 16384) == -1,
          "size to row");
}

/* ---- 2. fence marks as raw words ------------------------------------ */

struct fence_case {
    uint32_t n_nodes;
    struct { unsigned word; uint32_t value; } w[5];
};

static void test_fence(void)
{
    /* Hand-derived: node 16k+j sits at bit 30-2j of word k; NODE=2, USED=1. */
    static const struct fence_case cases[] = {
        { 125, { { 0, 0x22020002 }, { 1, 0x00000002 }, { 3, 0x00000001 } } },
        { 205, { { 0, 0x22090090 }, { 1, 0x00002000 }, { 3, 0x02000000 }, { 6, 0x00010000 } } },
        { 413, { { 0, 0x22090090 }, { 1, 0x00002000 }, { 3, 0x02000000 }, { 6, 0x00020000 },
                 { 12, 0x00000001 } } },
        {  29, { { 0, 0x22020001 } } },
    };
    unsigned i, k;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint32_t expect[64];

        memset(expect, 0, sizeof(expect));
        for (k = 0; k < 5; ++k)
            expect[cases[i].w[k].word] |= cases[i].w[k].value;
        fresh_tree(cases[i].n_nodes);
        for (k = 0; k < 64; ++k)
            CHECK(words[k] == expect[k], "n_nodes %u word %u: %08x want %08x",
                  cases[i].n_nodes, k, words[k], expect[k]);
    }

    fresh_tree(125);
    CHECK(flat_get(NULL, 63) == SHEAP_NODE_USED, "125: node 63 USED");
    CHECK(flat_get(NULL, 31) == SHEAP_NODE_SPLIT && flat_get(NULL, 15) == SHEAP_NODE_SPLIT
          && flat_get(NULL, 7) == SHEAP_NODE_SPLIT && flat_get(NULL, 3) == SHEAP_NODE_SPLIT
          && flat_get(NULL, 1) == SHEAP_NODE_SPLIT, "125: 31,15,7,3,1 NODE");
}

/* ---- 3/4/5. allocation behaviour ------------------------------------ */

static void test_allocation(void)
{
    uint64_t base = 0x100000, ea;
    uint32_t n;
    int rc;

    /* The documented example: 8 leaves, 7 real (n_nodes 14), 1 KB root. */
    fresh_tree(14);
    CHECK(__sheap_tree_allocate(&flat, 14, (unsigned)__sheap_size_to_row(100, 1024)) == 8, "100 B -> node 8");
    CHECK(__sheap_tree_allocate(&flat, 14, (unsigned)__sheap_size_to_row(200, 1024)) == 5, "200 B -> node 5");
    CHECK(__sheap_heap_free(&flat, 14, 1024, base, base) == 0, "free node 8's block");
    CHECK(__sheap_tree_allocate(&flat, 14, (unsigned)__sheap_size_to_row(200, 1024)) == 4, "200 B -> node 4");

    /* Sample addresses. */
    fresh_tree(205);
    ea = __sheap_heap_allocate(&flat, 205, 16384, base + 256, 2048);
    CHECK(ea == base + 256, "Sheap 10240: 2048 B at +%" PRIu64, ea - base);
    fresh_tree(125);
    ea = __sheap_heap_allocate(&flat, 125, 8192, base + 2304, 2048);
    CHECK(ea == base + 2304 && flat_get(NULL, 4) == SHEAP_NODE_USED, "KeySheap 10240: 2048 B is node 4 at +2304");
    fresh_tree(125);
    ea = __sheap_heap_allocate(&flat, 125, 8192, base + 2304, 128);
    CHECK(ea == base + 2304 && flat_get(NULL, 64) == SHEAP_NODE_USED, "KeySheap 10240: 128 B is node 64");
    ea = __sheap_heap_allocate(&flat, 125, 8192, base + 2304, 0);
    CHECK(ea == base + 2304 + 128, "size 0 takes a 128 B block");

    /* Rightmost node: a 2048 heap never hands out its root or last leaf. */
    fresh_tree(29);
    CHECK(__sheap_heap_allocate(&flat, 29, 2048, base, 2048) == 0, "2048 heap cannot allocate 2048");
    for (n = 0; n < 32; ++n)
        if (__sheap_tree_allocate(&flat, 29, 4) == 0)
            break;
    CHECK(n == 13, "2048 heap: %u leaves allocated, want 13 (16..28)", n);
    CHECK(flat_get(NULL, 29) == SHEAP_NODE_FREE, "last real leaf 29 stays FREE");
    fresh_tree(125);
    for (n = 0; n < 128; ++n) {
        uint32_t node = __sheap_tree_allocate(&flat, 125, 6);
        if (node == 0)
            break;
        CHECK(node != 125, "leaf 125 handed out");
    }
    CHECK(n == 61, "KeySheap 10240: %u leaves, want 61 (64..124)", n);
    fresh_tree(125);
    CHECK(__sheap_tree_allocate(&flat, 125, 0) == 0, "root never allocated");

    /* Free error paths. */
    fresh_tree(205);
    ea = __sheap_heap_allocate(&flat, 205, 16384, base + 256, 512);
    CHECK(__sheap_heap_free(&flat, 205, 16384, base + 256, ea + 64) == ERR(CELL_SHEAP_ERROR_INVAL),
          "unaligned pointer");
    CHECK(__sheap_heap_free(&flat, 205, 16384, base + 256, base + 128) == ERR(CELL_SHEAP_ERROR_INVAL),
          "pointer below the heap");
    CHECK(__sheap_heap_free(&flat, 205, 16384, base + 256, base + 256 + 9984) == ERR(CELL_SHEAP_ERROR_INVAL),
          "pointer past the last leaf");
    flat_set(NULL, 200, SHEAP_NODE_SPLIT);   /* a leaf can only read SPLIT if corrupted */
    rc = __sheap_heap_free(&flat, 205, 16384, base + 256, base + 256 + (200 - 128) * 128);
    CHECK(rc == ERR(CELL_SHEAP_ERROR_FAULT), "SPLIT leaf -> FAULT (%08x)", (unsigned)rc);
    flat_set(NULL, 200, SHEAP_NODE_FREE);
    CHECK(__sheap_heap_free(&flat, 205, 16384, base + 256, ea) == 0, "free the 512 B block");
    CHECK(__sheap_tree_free(&flat, 205, 0) == ERR(CELL_SHEAP_ERROR_INVAL), "leaf 0");
    fresh_tree(15);                           /* complete tree, root FREE */
    CHECK(__sheap_tree_free(&flat, 15, 9) == ERR(CELL_SHEAP_ERROR_INVAL), "all-free walk runs past the root");

    /* Queries on fresh heaps. */
    fresh_tree(205);
    CHECK(__sheap_tree_query_free(&flat, 205, 16384) == 9984, "Sheap 10240 QueryFree");
    CHECK(__sheap_tree_query_max(&flat, 205, 16384) == 8192, "Sheap 10240 QueryMax");
    fresh_tree(125);
    CHECK(__sheap_tree_query_free(&flat, 125, 8192) == 7936, "KeySheap 10240 QueryFree");
    CHECK(__sheap_tree_query_max(&flat, 125, 8192) == 4096, "KeySheap 10240 QueryMax");
}

/* ---- 6. random run against a leaf bitmap ---------------------------- */

static uint64_t rng = UINT64_C(0x9e3779b97f4a7c15);

static uint64_t next_random(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

struct fuzz {
    uint32_t n_nodes, first_leaf, leaf_row;
    uint64_t s_root;
    unsigned char *leaf_used;       /* by leaf id */
    unsigned char *expect_used;     /* ids that must read USED */
    uint32_t *live;                 /* allocated node ids */
    unsigned n_live;
    uint32_t max_id;
};

static int leaves_free(const struct fuzz *f, uint32_t node, unsigned row)
{
    unsigned depth = f->leaf_row - row;
    uint64_t lo = (uint64_t)node << depth, hi = ((uint64_t)(node + 1) << depth) - 1, l;

    if (hi > f->n_nodes)
        return 0;
    for (l = lo; l <= hi; ++l)
        if (f->leaf_used[l])
            return 0;
    return 1;
}

static void set_leaves(struct fuzz *f, uint32_t node, unsigned char v)
{
    unsigned depth = f->leaf_row - __sheap_floor_log2(node);
    uint64_t lo = (uint64_t)node << depth, hi = ((uint64_t)(node + 1) << depth) - 1, l;

    for (l = lo; l <= hi; ++l)
        f->leaf_used[l] = v;
}

static uint32_t model_allocate(const struct fuzz *f, unsigned row)
{
    uint64_t first = (uint64_t)1 << row, end = (first << 1) - 1, i;

    if (end > f->n_nodes)
        end = f->n_nodes;
    for (i = first; i < end; ++i)
        if (leaves_free(f, (uint32_t)i, row))
            return (uint32_t)i;
    return 0;
}

static int all_free(const struct fuzz *f, uint32_t node)
{
    if (node > f->max_id)
        return 1;
    if (flat_get(NULL, node) != SHEAP_NODE_FREE)
        return 0;
    if (node >= f->first_leaf)
        return 1;
    return all_free(f, 2 * node) && all_free(f, 2 * node + 1);
}

static int taken(unsigned s) { return s == SHEAP_NODE_USED || s == SHEAP_NODE_FULL; }

/* Structural invariants; returns 0 when consistent. */
static int check_node(const struct fuzz *f, uint32_t node)
{
    unsigned s = flat_get(NULL, node), l, r;

    if ((s == SHEAP_NODE_USED) != (f->expect_used[node] != 0))
        return 1;
    if (node >= f->first_leaf)
        return s == SHEAP_NODE_SPLIT || s == SHEAP_NODE_FULL;
    if (s == SHEAP_NODE_FREE || s == SHEAP_NODE_USED)
        return !(all_free(f, 2 * node) && all_free(f, 2 * node + 1));
    l = flat_get(NULL, 2 * node);
    r = flat_get(NULL, 2 * node + 1);
    if (s == SHEAP_NODE_FULL && !(taken(l) && taken(r)))
        return 1;
    if (s == SHEAP_NODE_SPLIT && ((taken(l) && taken(r)) || (l == SHEAP_NODE_FREE && r == SHEAP_NODE_FREE)))
        return 1;
    return check_node(f, 2 * node) || check_node(f, 2 * node + 1);
}

static uint64_t true_max(const struct fuzz *f)
{
    uint32_t node;

    for (node = 1; node <= f->n_nodes; ++node)
        if (leaves_free(f, node, __sheap_floor_log2(node)))
            return f->s_root >> __sheap_floor_log2(node);
    return 0;
}

static void run_fuzz(const char *name, int keyed, uint64_t size, unsigned steps)
{
    struct fuzz f;
    sheap_geometry g;
    uint64_t ea_heap = UINT64_C(0x30000000), free_leaves;
    unsigned step, overstated = 0, allocs = 0, frees = 0, refused = 0, bad = 0;
    uint32_t id;

    if (__sheap_geometry(keyed ? __sheap_keyed_span(size) : __sheap_plain_span(size), &g)) {
        CHECK(0, "%s: geometry", name);
        return;
    }
    memset(&f, 0, sizeof(f));
    f.n_nodes = g.n_nodes;
    f.s_root = g.s_root;
    f.first_leaf = (uint32_t)(g.s_root >> SHEAP_LEAF_SHIFT);
    f.leaf_row = __sheap_floor_log2(f.first_leaf);
    f.max_id = 2 * f.first_leaf - 1;
    f.leaf_used = calloc(f.max_id + 2, 1);
    f.expect_used = calloc(f.max_id + 2, 1);
    f.live = calloc(f.first_leaf + 1, sizeof(uint32_t));

    fresh_tree(f.n_nodes);
    for (id = 1; id <= f.max_id; ++id)
        if (flat_get(NULL, id) == SHEAP_NODE_USED)
            f.expect_used[id] = 1;          /* fence nodes */
    free_leaves = f.n_nodes - f.first_leaf + 1;

    for (step = 0; step < steps; ++step) {
        uint64_t r = next_random();

        if (f.n_live == 0 || (r & 1)) {
            uint64_t sz;
            unsigned kind = (unsigned)(r >> 8) % 10;
            int row;
            uint32_t want, got;
            uint64_t ea;

            if (kind < 6)
                sz = (r >> 16) % 513;
            else if (kind < 9)
                sz = (r >> 16) % 8193;
            else
                sz = (r >> 16) % (f.s_root + 1);
            row = __sheap_size_to_row(sz, f.s_root);
            want = row < 0 ? 0 : model_allocate(&f, (unsigned)row);
            ea = __sheap_heap_allocate(&flat, f.n_nodes, f.s_root, ea_heap, sz);
            got = 0;
            if (ea) {
                uint64_t off = ea - ea_heap;
                unsigned rrow = (unsigned)row;
                got = (uint32_t)((off / (f.s_root >> rrow)) + ((uint64_t)1 << rrow));
            }
            if (got != want) {
                CHECK(0, "%s step %u: allocate %" PRIu64 " gave node %u, model %u", name, step, sz, got, want);
                break;
            }
            if (got) {
                f.live[f.n_live++] = got;
                f.expect_used[got] = 1;
                set_leaves(&f, got, 1);
                free_leaves -= (uint64_t)1 << (f.leaf_row - __sheap_floor_log2(got));
                ++allocs;
            } else {
                ++refused;
            }
        } else {
            unsigned pick = (unsigned)((r >> 8) % f.n_live);
            uint32_t node = f.live[pick];
            uint64_t ea = ea_heap + __sheap_node_offset(node, f.s_root);
            int rc = __sheap_heap_free(&flat, f.n_nodes, f.s_root, ea_heap, ea);

            if (rc != 0) {
                CHECK(0, "%s step %u: free node %u returned %08x", name, step, node, (unsigned)rc);
                break;
            }
            f.live[pick] = f.live[--f.n_live];
            f.expect_used[node] = 0;
            set_leaves(&f, node, 0);
            free_leaves += (uint64_t)1 << (f.leaf_row - __sheap_floor_log2(node));
            ++frees;
        }

        if (check_node(&f, 1)) {
            CHECK(0, "%s step %u: tree invariant broken", name, step);
            ++bad;
            break;
        }
        {
            uint64_t qf = __sheap_tree_query_free(&flat, f.n_nodes, f.s_root);
            uint64_t qm = __sheap_tree_query_max(&flat, f.n_nodes, f.s_root);
            uint64_t tm = true_max(&f);

            if (qf != free_leaves * SHEAP_LEAF_BYTES) {
                CHECK(0, "%s step %u: QueryFree %" PRIu64 " model %" PRIu64, name, step, qf,
                      free_leaves * SHEAP_LEAF_BYTES);
                break;
            }
            if (qm < tm) {
                CHECK(0, "%s step %u: QueryMax %" PRIu64 " below the largest free block %" PRIu64,
                      name, step, qm, tm);
                break;
            }
            if (qm != tm)
                ++overstated;
        }
    }
    ++checks;
    printf("  fuzz %-15s %u steps: %u allocs, %u frees, %u refused; QueryMax above the true "
           "largest block after %u steps\n", name, step, allocs, frees, refused, overstated);
    free(f.leaf_used);
    free(f.expect_used);
    free(f.live);
    (void)bad;
}

/* ---- 7. key table --------------------------------------------------- */

static void test_keys(void)
{
    const uint64_t heap = UINT64_C(0x40000900), s_buffer = 7936;
    const uint64_t empty = 0, newing = SHEAP_KEY_ENTRY(1, 0);
    uint64_t next = 0xdead, obj = 0xdead, live1, live2, deleting;
    int rc;

    CHECK(__sheap_key_state(empty) == SHEAP_KEY_EMPTY && __sheap_key_state(newing) == SHEAP_KEY_NEWING
          && __sheap_key_state(SHEAP_KEY_ENTRY(3, 5)) == SHEAP_KEY_LIVE
          && __sheap_key_state(SHEAP_KEY_ENTRY(0, 5)) == SHEAP_KEY_DELETING, "state decoding");

    /* EMPTY */
    rc = __sheap_key_new_step(empty, heap, &next, &obj);
    CHECK(rc == SHEAP_KEY_COMMIT && next == newing && obj == 0, "EMPTY + New -> creator, (1,0)");
    CHECK(__sheap_key_delete_step(empty, heap, &next, &obj) == ERR(CELL_SHEAP_ERROR_BUSY), "EMPTY Delete BUSY");
    CHECK(__sheap_key_finalize_new_step(empty, heap, heap, s_buffer, &next) == ERR(CELL_SHEAP_ERROR_BUSY),
          "EMPTY FinalizeNew BUSY");
    CHECK(__sheap_key_finalize_delete_step(empty, &next) == ERR(CELL_SHEAP_ERROR_BUSY), "EMPTY FinalizeDelete BUSY");

    /* NEWING */
    CHECK(__sheap_key_new_step(newing, heap, &next, &obj) == SHEAP_KEY_WAIT, "NEWING New waits");
    CHECK(__sheap_key_delete_step(newing, heap, &next, &obj) == SHEAP_KEY_WAIT, "NEWING Delete waits");
    CHECK(__sheap_key_finalize_delete_step(newing, &next) == ERR(CELL_SHEAP_ERROR_BUSY), "NEWING FinalizeDelete BUSY");
    next = 0xdead;
    CHECK(__sheap_key_finalize_new_step(newing, 0, heap, s_buffer, &next) == SHEAP_KEY_COMMIT && next == 0,
          "NEWING cancel -> EMPTY");
    CHECK(__sheap_key_finalize_new_step(newing, heap - 128, heap, s_buffer, &next) == ERR(CELL_SHEAP_ERROR_INVAL),
          "object below the heap");
    CHECK(__sheap_key_finalize_new_step(newing, heap + s_buffer, heap, s_buffer, &next) == ERR(CELL_SHEAP_ERROR_INVAL),
          "object past the heap");
    CHECK(__sheap_key_finalize_new_step(newing, heap + 64, heap, s_buffer, &next) == ERR(CELL_SHEAP_ERROR_INVAL),
          "misaligned object");
    rc = __sheap_key_finalize_new_step(newing, heap, heap, s_buffer, &next);
    CHECK(rc == SHEAP_KEY_COMMIT && next == SHEAP_KEY_ENTRY(1, 1), "publish first block -> (1,1)");
    live1 = next;
    rc = __sheap_key_finalize_new_step(newing, heap + 7808, heap, s_buffer, &next);
    CHECK(rc == SHEAP_KEY_COMMIT && next == SHEAP_KEY_ENTRY(1, 62), "publish last block -> (1,62)");
    {
        uint64_t big = UINT64_C(1) << 40;
        CHECK(__sheap_key_finalize_new_step(newing, heap + (UINT64_C(0xfffffffe) << 7), heap, big, &next)
              == SHEAP_KEY_COMMIT && SHEAP_KEY_NVAL(next) == 0xffffffffu, "largest encodable address");
        CHECK(__sheap_key_finalize_new_step(newing, heap + (UINT64_C(0xffffffff) << 7), heap, big, &next)
              == SHEAP_KEY_OVERFLOW, "encoded address overflow");
    }

    /* LIVE */
    rc = __sheap_key_new_step(live1, heap, &next, &obj);
    CHECK(rc == SHEAP_KEY_COMMIT && next == SHEAP_KEY_ENTRY(2, 1) && obj == heap, "LIVE New attaches");
    live2 = next;
    CHECK(__sheap_key_finalize_new_step(live1, heap, heap, s_buffer, &next) == ERR(CELL_SHEAP_ERROR_BUSY),
          "LIVE FinalizeNew BUSY");
    CHECK(__sheap_key_finalize_delete_step(live1, &next) == ERR(CELL_SHEAP_ERROR_BUSY), "LIVE FinalizeDelete BUSY");
    rc = __sheap_key_delete_step(live2, heap, &next, &obj);
    CHECK(rc == SHEAP_KEY_COMMIT && next == live1 && obj == 0, "Delete with references left");
    rc = __sheap_key_delete_step(live1, heap, &next, &obj);
    CHECK(rc == SHEAP_KEY_COMMIT && next == SHEAP_KEY_ENTRY(0, 1) && obj == heap, "last Delete returns the block");
    deleting = next;

    /* DELETING */
    CHECK(__sheap_key_new_step(deleting, heap, &next, &obj) == SHEAP_KEY_WAIT, "DELETING New waits");
    CHECK(__sheap_key_delete_step(deleting, heap, &next, &obj) == ERR(CELL_SHEAP_ERROR_BUSY), "DELETING Delete BUSY");
    CHECK(__sheap_key_finalize_new_step(deleting, heap, heap, s_buffer, &next) == ERR(CELL_SHEAP_ERROR_BUSY),
          "DELETING FinalizeNew BUSY");
    rc = __sheap_key_finalize_delete_step(deleting, &next);
    CHECK(rc == SHEAP_KEY_COMMIT && next == 0, "FinalizeDelete empties the entry");
    CHECK(__sheap_key_decode(62, heap) == heap + 7808, "decode");
}

int main(void)
{
    test_layout();
    test_fence();
    test_allocation();
    test_keys();
    run_fuzz("Sheap 100000", 0, 100000, 100000);
    run_fuzz("KeySheap 10240", 1, 10240, 100000);
    run_fuzz("Sheap 2048", 0, 2048, 20000);
    if (failures) {
        printf("sheap-core: FAIL (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("sheap-core: PASS (%d checks)\n", checks);
    return 0;
}
