/* Host test of the SPU libsheap layer (src/spu over src/core) and the SPU
 * libsync sources it calls, all compiled unchanged against the simulated
 * MFC in tests/sdk/fixtures/spu-mfc-mock.  Built and run by
 * tests/sdk/sheap-core-host-test.sh.
 *
 *  - Header contents written by both Initialize functions.
 *  - Canary: a refused Initialize writes nothing at all, and an accepted
 *    one writes nothing outside [ea, ea + size).
 *  - Allocate/Free/Query through the LS line cache, and a large heap run
 *    whose tree bytes must equal the portable core run on a flat array.
 *  - Keyed objects: attach/detach counts, last-reference free, creator
 *    rollback (shortage, barrier count 0 and 32768), the queue and rwm
 *    descriptors surviving their initialisation, semaphore P/V/TryP.
 *  - Reservation loss: another processor takes the heap lock, becomes a
 *    key's creator, or attaches, between the SPU's getllar and putllc;
 *    the SPU re-reads and converges without writing a stale lock word or
 *    key entry.  The plain and keyed sections are rerun with every other
 *    putllc lost; that covers the heap lock under cellSheapAllocate,
 *    Free, QueryMax and QueryFree, the header snapshot and all four
 *    key-table steps under the six keyed New/Delete pairs, the
 *    semaphore store in SemaphoreNew, the semaphore P/V/TryP inlines, and
 *    the libsync Mutex/Queue/Rwm calls the keyed section makes.  Initialize
 *    publishes with putlluc (unconditional), so it has no loop to lose.
 *
 * Limits of the simulation: DMA completes synchronously, so no tree-line
 * put can still be in flight when the lock is released, and ordering
 * between DMA and reservations is not tested.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cell/sheap.h>
#include "core/sheap_core.h"
#include "sheap_layout.h"
#include "mfc_mock.h"

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

#define MEM mfc_mock_memory

static sheap_header *H(uint64_t ea) { return (sheap_header *)(MEM + ea); }

static uint64_t entry(uint64_t ks, unsigned key)
{
    uint64_t e;

    memcpy(&e, MEM + H(ks)->ea_keytable + 8 * key, 8);
    return e;
}

/* ---- canary --------------------------------------------------------- */

static uint8_t snapshot[MFC_MOCK_MEMORY];

/* Fill memory with a pattern, run init, and count changed bytes outside
 * [ea, ea + inside) (and inside it too when inside is 0). */
static unsigned long changed_outside(uint64_t ea, uint64_t inside)
{
    unsigned long n = 0;
    uint64_t i;

    for (i = 0; i < MFC_MOCK_MEMORY; ++i)
        if (MEM[i] != snapshot[i] && (i < ea || i >= ea + inside))
            ++n;
    return n;
}

static void arm_canary(void)
{
    memset(MEM, 0x5a, MFC_MOCK_MEMORY);
    memcpy(snapshot, MEM, MFC_MOCK_MEMORY);
}

static void test_canary(void)
{
    const uint64_t ea = 0x10000;
    int rc;

    arm_canary();
    rc = cellKeySheapInitialize(ea, 2048, 3);
    CHECK(rc == (int)CELL_SHEAP_ERROR_INVAL, "keyed 2048 refused (%08x)", (unsigned)rc);
    CHECK(changed_outside(ea, 0) == 0, "refused keyed init wrote %lu bytes", changed_outside(ea, 0));

    arm_canary();
    CHECK(cellKeySheapInitialize(ea, 2176, 3) == (int)CELL_SHEAP_ERROR_INVAL
          && changed_outside(ea, 0) == 0, "keyed 2176 refused, nothing written");
    arm_canary();
    CHECK(cellSheapInitialize(ea, 2047, 3) == (int)CELL_SHEAP_ERROR_INVAL
          && changed_outside(ea, 0) == 0, "plain 2047 refused, nothing written");

    arm_canary();
    CHECK(cellKeySheapInitialize(ea, 2560, 3) == CELL_OK, "keyed 2560 accepted");
    CHECK(changed_outside(ea, 2560) == 0, "keyed 2560 wrote %lu bytes outside", changed_outside(ea, 2560));
    arm_canary();
    CHECK(cellSheapInitialize(ea, 2048, 3) == CELL_OK && changed_outside(ea, 2048) == 0,
          "plain 2048 stays inside its region");
    arm_canary();
    CHECK(cellKeySheapInitialize(ea, 10240, 3) == CELL_OK && changed_outside(ea, 10240) == 0,
          "keyed 10240 stays inside its region");
    memset(MEM, 0, MFC_MOCK_MEMORY);
}

/* ---- plain heap ----------------------------------------------------- */

static void test_plain(void)
{
    const uint64_t ps = 0x1000;
    uint64_t a;

    memset(MEM + ps, 0xee, 10240);
    CHECK(cellSheapInitialize(ps, 10240, 8) == CELL_OK, "init");
    CHECK(H(ps)->lock == 0 && H(ps)->reserved0 == 0 && H(ps)->ea_self == ps && H(ps)->ea_tree == ps + 128
          && H(ps)->s_tree == 128 && H(ps)->n_nodes == 205 && H(ps)->ea_heap == ps + 256
          && H(ps)->s_root == 16384 && H(ps)->s_buffer == 9984 && H(ps)->spu_tag1 == 8
          && H(ps)->spu_tag2 == 8 && H(ps)->ea_keytable == 0, "plain header");
    CHECK(cellSheapQueryFree(ps) == 9984 && cellSheapQueryMax(ps) == 8192, "fresh queries");
    a = cellSheapAllocate(ps, 2048);
    CHECK(a == ps + 256, "2048 B at +256");
    CHECK(cellSheapQueryFree(ps) == 9984 - 2048 && H(ps)->lock == 0, "after allocate, unlocked");
    CHECK(cellSheapFree(ps, a + 64) == (int)CELL_SHEAP_ERROR_INVAL, "unaligned pointer");
    CHECK(cellSheapFree(ps + 64, a) == (int)CELL_SHEAP_ERROR_ALIGN, "misaligned heap");
    CHECK(cellSheapAllocate(ps + 64, 128) == 0, "misaligned heap allocate");
    CHECK(cellSheapFree(ps, a) == CELL_OK && cellSheapQueryFree(ps) == 9984, "free");
    CHECK(cellSheapAllocate(ps, 8192) == ps + 256 && cellSheapQueryMax(ps) == 4096
          && cellSheapQueryFree(ps) == 9984 - 8192,
          "QueryMax on the firmware rule counts FREE node 4 inside allocated node 2");
}

/* ---- large heap vs the flat core ------------------------------------ */

static uint32_t model_words[1 << 16];

static unsigned mget(void *c, uint32_t n)
{
    (void)c;
    return (model_words[n >> 4] >> SHEAP_TREE_SHIFT(n)) & 3u;
}

static void mset(void *c, uint32_t n, unsigned s)
{
    uint32_t *w = &model_words[n >> 4];

    (void)c;
    *w = (*w & ~(3u << SHEAP_TREE_SHIFT(n))) | ((uint32_t)s << SHEAP_TREE_SHIFT(n));
}

static const sheap_tree_access model = { mget, mset, 0 };

static void test_large(void)
{
    const uint64_t big = 0x100000, size = 6u << 20;
    sheap_geometry g;
    static uint64_t live[4096];
    unsigned n = 0, step;
    uint64_t rnd = 88172645463325252ull, ea_heap;

    CHECK(cellSheapInitialize(big, size, 5) == CELL_OK, "large init");
    CHECK(__sheap_init_geometry(0, size, &g) == 0, "large geometry");
    ea_heap = big + 128 + g.s_tree;
    memset(model_words, 0, sizeof(model_words));
    __sheap_tree_fence(&model, g.n_nodes);
    for (step = 0; step < 20000; ++step) {
        rnd ^= rnd << 13; rnd ^= rnd >> 7; rnd ^= rnd << 17;
        if (n == 0 || ((rnd & 3) != 0 && n < 4096)) {
            uint64_t sz = (rnd >> 8) % ((rnd & 16) ? 65536 : 1024);
            uint64_t a = cellSheapAllocate(big, sz);
            uint64_t b = __sheap_heap_allocate(&model, g.n_nodes, g.s_root, ea_heap, sz);

            if (a != b) {
                CHECK(0, "step %u: allocate %" PRIu64 " gave %" PRIx64 ", core %" PRIx64, step, sz, a, b);
                break;
            }
            if (a)
                live[n++] = a;
        } else {
            unsigned k = (unsigned)((rnd >> 8) % n);
            int rc = cellSheapFree(big, live[k]);

            if (rc != __sheap_heap_free(&model, g.n_nodes, g.s_root, ea_heap, live[k])) {
                CHECK(0, "step %u: free rc", step);
                break;
            }
            live[k] = live[--n];
        }
    }
    CHECK(memcmp(MEM + big + 128, model_words, g.s_tree) == 0, "tree bytes equal the core model");
    CHECK(cellSheapQueryFree(big) == (int)__sheap_tree_query_free(&model, g.n_nodes, g.s_root), "QueryFree");
    CHECK(cellSheapQueryMax(big) == (int)__sheap_tree_query_max(&model, g.n_nodes, g.s_root), "QueryMax");
    printf("  large heap: %u steps, %u live blocks, %u nodes, %" PRIu64 " tree lines through the cache\n",
           step, n, g.n_nodes, g.s_tree / 128);
}

/* ---- keyed objects -------------------------------------------------- */

static void test_keyed(void)
{
    static uint8_t in[128] __attribute__((aligned(128)));
    static uint8_t out[128] __attribute__((aligned(128)));
    const uint64_t ks = 0x20000;
    CellKeySheapBuffer b1, b2;
    CellKeySheapMutex m;
    CellKeySheapBarrier br;
    CellKeySheapQueue q;
    CellKeySheapRwm r;
    CellKeySheapSemaphore s;
    uint64_t v;
    int rc;

    CHECK(cellKeySheapInitialize(ks, 10240, 3) == CELL_OK, "keyed init");
    CHECK(H(ks)->ea_keytable == ks + 128 && H(ks)->s_keytable == 2048 && H(ks)->ea_tree == ks + 2176
          && H(ks)->ea_heap == ks + 2304 && H(ks)->n_nodes == 125 && H(ks)->s_root == 8192, "keyed header");

    CHECK(cellKeySheapBufferNew(&b1, ks, 11, 2048) == CELL_OK && b1.ea == ks + 2304 && b1.size == 2048,
          "creator");
    CHECK(entry(ks, 11) == SHEAP_KEY_ENTRY(1, 1), "entry (1,1)");
    CHECK(cellKeySheapBufferNew(&b2, ks, 11, 0) == CELL_OK && b2.ea == b1.ea && b2.size == 0, "attacher");
    CHECK(entry(ks, 11) == SHEAP_KEY_ENTRY(2, 1), "entry (2,1)");
    cellKeySheapBufferDelete(&b2);
    CHECK(entry(ks, 11) == SHEAP_KEY_ENTRY(1, 1) && cellKeySheapQueryFree(ks) == 7936 - 2048, "detach keeps it");
    cellKeySheapBufferDelete(&b1);
    CHECK(entry(ks, 11) == 0 && cellKeySheapQueryFree(ks) == 7936, "last delete frees and empties");
    CHECK(cellKeySheapBufferNew(&b1, ks, 256, 1) == (int)CELL_SHEAP_ERROR_INVAL, "key 256");
    CHECK(cellKeySheapBufferNew(NULL, ks, 1, 1) == (int)CELL_SHEAP_ERROR_INVAL, "NULL record");
    CHECK(cellKeySheapBufferNew(&b1, ks + 8, 1, 1) == (int)CELL_SHEAP_ERROR_ALIGN, "misaligned heap");
    CHECK(cellKeySheapBufferNew(&b1, ks, 1, 8192) == (int)CELL_SHEAP_ERROR_SHORTAGE && entry(ks, 1) == 0,
          "shortage cancels the key");

    CHECK(cellKeySheapMutexNew(&m, ks, 1) == CELL_OK && m.ea == ks + 2304, "mutex");
    CHECK(cellKeySheapMutexTryLock(&m) == CELL_OK && cellKeySheapMutexTryLock(&m) == (int)CELL_SYNC_ERROR_BUSY
          && cellKeySheapMutexUnlock(&m) == CELL_OK, "mutex try/unlock");

    CHECK(cellKeySheapBarrierNew(&br, ks, 2, 0) == (int)CELL_SYNC_ERROR_INVAL, "barrier count 0 refused");
    CHECK(entry(ks, 2) == 0 && cellKeySheapQueryFree(ks) == 7936 - 128, "count 0: block freed, key empty");
    CHECK(cellKeySheapBarrierNew(&br, ks, 2, 32768) == (int)CELL_SYNC_ERROR_INVAL, "barrier 32768 refused");
    CHECK(entry(ks, 2) == 0 && cellKeySheapQueryFree(ks) == 7936 - 128, "count 32768: block freed, key empty");
    CHECK(cellKeySheapBarrierNew(&br, ks, 2, 32767) == CELL_OK && br.ea == ks + 2304 + 128, "barrier 32767");

    rc = cellKeySheapQueueNew(&q, ks, 3, 16, 8);
    CHECK(rc == CELL_OK && q.ea == ks + 2304 + 256, "queue at node 66 (+%" PRIu64 ")", q.ea - ks);
    memcpy(&v, MEM + q.ea + 8, 8);
    CHECK(v == ((uint64_t)16 << 32 | 8), "queue size/depth survive");
    memcpy(&v, MEM + q.ea + 16, 8);
    CHECK(v == q.ea + 128, "queue ring address survives");
    memset(in, 0x42, 16);
    CHECK(cellKeySheapQueuePush(&q, in, 4) == CELL_OK && cellKeySheapQueueSize(&q) == 1, "push");
    CHECK(cellKeySheapQueuePop(&q, out, 4) == CELL_OK && out[0] == 0x42 && out[15] == 0x42, "pop");

    CHECK(cellKeySheapRwmNew(&r, ks, 4, 64) == CELL_OK, "rwm");
    memcpy(&v, MEM + r.ea + 8, 8);
    CHECK(v == r.ea + 128, "rwm buffer address survives");
    memset(in, 0x24, 64);
    CHECK(cellKeySheapRwmWrite(&r, in, 4) == CELL_OK && cellKeySheapRwmReadBegin(&r, out, 4) == CELL_OK
          && out[63] == 0x24 && cellKeySheapRwmReadEnd(&r, 4) == CELL_OK, "rwm write/read");

    CHECK(cellKeySheapSemaphoreNew(&s, ks, 5, 2) == CELL_OK, "semaphore");
    CHECK(cellKeySheapSemaphoreTryP(&s) == CELL_OK && cellKeySheapSemaphoreTryP(&s) == CELL_OK
          && cellKeySheapSemaphoreTryP(&s) == (int)CELL_SYNC_ERROR_BUSY, "TryP");
    cellKeySheapSemaphoreV(&s);
    cellKeySheapSemaphoreP(&s);
    memcpy(&v, MEM + s.ea, 4);
    CHECK((uint32_t)v == 0, "P/V balance");

    cellKeySheapMutexDelete(&m);
    cellKeySheapBarrierDelete(&br);
    cellKeySheapQueueDelete(&q);
    cellKeySheapRwmDelete(&r);
    cellKeySheapSemaphoreDelete(&s);
    CHECK(cellKeySheapQueryFree(ks) == 7936 && entry(ks, 1) == 0 && entry(ks, 5) == 0, "all deleted");
}

/* ---- reservation loss ----------------------------------------------- */

/* Another processor working on the same heap, acting between the SPU's
 * getllar and putllc (on_putllc) or while the SPU spins (on_reserve).
 * It uses the portable core directly on the simulated memory, which holds
 * the tree in the same word order the SPU layer writes. */
static uint64_t other_heap;
static int other_step, other_countdown;
static uint64_t other_block;

static unsigned mem_get(void *c, uint32_t n)
{
    uint32_t w;

    (void)c;
    memcpy(&w, MEM + H(other_heap)->ea_tree + 4 * (n >> 4), 4);
    return (w >> SHEAP_TREE_SHIFT(n)) & 3u;
}

static void mem_set(void *c, uint32_t n, unsigned s)
{
    uint64_t ea = H(other_heap)->ea_tree + 4 * (n >> 4);
    uint32_t w;

    (void)c;
    memcpy(&w, MEM + ea, 4);
    w = (w & ~(3u << SHEAP_TREE_SHIFT(n))) | ((uint32_t)s << SHEAP_TREE_SHIFT(n));
    mfc_mock_foreign_store(ea, &w, 4);
}

static const sheap_tree_access mem_tree = { mem_get, mem_set, 0 };

static void other_store_lock(uint32_t value)
{
    mfc_mock_foreign_store(other_heap, &value, 4);
}

/* Heap lock: the other processor takes the lock just as the SPU tries to,
 * allocates 2 KB while holding it, and releases after the SPU has spun. */
static int grab_heap_lock(uint64_t line)
{
    if (line == other_heap && other_step == 0) {
        other_step = 1;
        other_store_lock(1);
        other_block = __sheap_heap_allocate(&mem_tree, H(other_heap)->n_nodes, H(other_heap)->s_root,
                                            H(other_heap)->ea_heap, 2048);
    }
    return 0;
}

static void release_heap_lock(uint64_t line)
{
    if (line == other_heap && other_step == 1 && --other_countdown == 0) {
        other_store_lock(0);
        other_step = 2;
    }
}

/* Key entry: the other processor becomes the creator just as the SPU
 * tries to, and publishes its block after the SPU has spun. */
static uint64_t key_entry_ea(unsigned key)
{
    return H(other_heap)->ea_keytable + 8 * key;
}

static void store_entry(unsigned key, uint64_t value)
{
    mfc_mock_foreign_store(key_entry_ea(key), &value, 8);
}

static int become_creator(uint64_t line)
{
    if (line == (key_entry_ea(7) & ~(uint64_t)127) && other_step == 0) {
        other_step = 1;
        store_entry(7, SHEAP_KEY_ENTRY(1, 0));
    }
    return 0;
}

static void publish_later(uint64_t line)
{
    if (line == (key_entry_ea(7) & ~(uint64_t)127) && other_step == 1 && --other_countdown == 0) {
        uint32_t nval = (uint32_t)(((other_block - H(other_heap)->ea_heap) >> 7) + 1);

        store_entry(7, SHEAP_KEY_ENTRY(1, nval));
        other_step = 2;
    }
}

/* Key entry: the other processor attaches just as the SPU detaches. */
static int attach_meanwhile(uint64_t line)
{
    if (line == (key_entry_ea(7) & ~(uint64_t)127) && other_step == 0) {
        uint64_t e;

        memcpy(&e, MEM + key_entry_ea(7), 8);
        store_entry(7, SHEAP_KEY_ENTRY(SHEAP_KEY_COUNT(e) + 1, SHEAP_KEY_NVAL(e)));
        other_step = 1;
    }
    return 0;
}

static void test_reservation_loss(void)
{
    const uint64_t ps = 0x40000, ks = 0x50000;
    CellKeySheapBuffer buf;
    uint64_t a, nval;
    int free_before;

    /* Heap lock taken by the other processor between the SPU's getllar and
     * putllc: the SPU's store is lost, it re-reads, spins until the lock is
     * released, and only then reads the tree, so it sees the other
     * processor's 2 KB block and allocates the next one. */
    CHECK(cellSheapInitialize(ps, 10240, 8) == CELL_OK, "plain init");
    other_heap = ps;
    other_step = 0;
    other_countdown = 5;
    mfc_mock_putllc_attempts = mfc_mock_putllc_lost = 0;
    mfc_mock_on_putllc = grab_heap_lock;
    mfc_mock_on_reserve = release_heap_lock;
    a = cellSheapAllocate(ps, 2048);
    mfc_mock_on_putllc = NULL;
    mfc_mock_on_reserve = NULL;
    CHECK(other_step == 2 && other_block == ps + 256, "other processor held the lock and took +256");
    CHECK(a == ps + 256 + 2048, "SPU allocated after the release: +%" PRIu64 " want +2304", a - ps);
    CHECK(H(ps)->lock == 0 && mfc_mock_putllc_lost == 1, "lock free again, one store lost");
    CHECK(cellSheapQueryFree(ps) == 9984 - 4096, "both blocks accounted");

    /* Key New racing a creator on the other processor: the SPU's
     * reference is lost, it re-reads a NEWING entry, waits, and attaches
     * to the published block instead of creating its own. */
    CHECK(cellKeySheapInitialize(ks, 10240, 3) == CELL_OK, "keyed init");
    other_heap = ks;
    other_block = cellSheapAllocate(ks, 512);
    free_before = cellSheapQueryFree(ks);
    other_step = 0;
    other_countdown = 5;
    mfc_mock_putllc_attempts = mfc_mock_putllc_lost = 0;
    mfc_mock_on_putllc = become_creator;
    mfc_mock_on_reserve = publish_later;
    CHECK(cellKeySheapBufferNew(&buf, ks, 7, 128) == CELL_OK, "BufferNew against a racing creator");
    mfc_mock_on_putllc = NULL;
    mfc_mock_on_reserve = NULL;
    nval = ((other_block - H(ks)->ea_heap) >> 7) + 1;
    CHECK(other_step == 2 && buf.ea == other_block, "attached to the other creator's block");
    CHECK(entry(ks, 7) == SHEAP_KEY_ENTRY(2, nval), "entry (2, v): no stale (1, 0) written");
    CHECK(cellSheapQueryFree(ks) == free_before, "the SPU allocated nothing");

    /* Delete while the other processor attaches: the count is re-read, the
     * object stays alive with both remaining references. */
    other_step = 0;
    mfc_mock_on_putllc = attach_meanwhile;
    cellKeySheapBufferDelete(&buf);
    mfc_mock_on_putllc = NULL;
    CHECK(entry(ks, 7) == SHEAP_KEY_ENTRY(2, nval) && cellSheapQueryFree(ks) == free_before,
          "delete re-read the count: (2, v), nothing freed");

    /* Every retry loop again with every other reservation lost. */
    mfc_mock_putllc_attempts = mfc_mock_putllc_lost = 0;
    mfc_mock_on_putllc = mfc_mock_lose_alternate;
    test_plain();
    test_keyed();
    mfc_mock_on_putllc = NULL;
    CHECK(mfc_mock_putllc_lost > 20, "alternate-loss rerun lost %lu reservations", mfc_mock_putllc_lost);
    printf("  reservation loss: heap lock, key New and key Delete raced by another processor;\n"
           "       plain and keyed sections rerun losing %lu of %lu putllc\n",
           mfc_mock_putllc_lost, mfc_mock_putllc_attempts);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_canary();
    test_plain();
    test_large();
    test_keyed();
    test_reservation_loss();
    if (failures) {
        printf("sheap-spu: FAIL (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("sheap-spu: PASS (%d checks)\n", checks);
    return 0;
}
