/*
 * Host test for the RSX heap last-error diagnostic (t_9eaa8392).
 *
 * Compiles the real allocator object, sdk/librsx/src/mm.c (byte-identical
 * to sdk/libgcm_cmd/src/rsx_legacy/mm.c), against a small stub heap and stub
 * gcmGetConfiguration, then drives the public surface (rsxHeapInit /
 * rsxMalloc / rsxMemalign / rsxFree) and asserts the SDK-only query
 * (ps3tc_rsx_mm_last_error / _string) reports the right code for each
 * failure mode.  This proves mm.c interprets the heap allocator's results
 * correctly:
 *
 *   UNINIT      rsxMalloc/rsxMemalign/rsxFree before rsxHeapInit
 *   GCM_CONFIG  rsxHeapInit when gcmGetConfiguration fails
 *   OK          rsxHeapInit success, a successful allocate/free, and
 *               rsxFree(NULL) which is a legal no-op (free(NULL))
 *   ALLOCATION  rsxMalloc of a size the heap cannot satisfy (NULL)
 *   FREE        rsxFree of an interior pointer, and
 *               rsxFree of an out-of-range pointer (heapFree returned false)
 *
 * The stub heap is deliberately honest: it only says "false" for a free
 * whose pointer is not the base of a block it allocated, and NULL for an
 * oversize allocate - the same contracts mm.c relies on.  The test is the
 * consumer, not the stub, that must pass.
 *
 * Built and run by tests/sdk/rsx-mm-lasterror-test.sh with a host C
 * compiler; no PS3 toolchain is required.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include <ppu-types.h>
#include <rsx/gcm_sys.h>
#include <rsx/mm.h>
#include <rsx/ps3tc_mm_lasterror.h>
#include <sys/heap.h>

/* ---- stub heap state ------------------------------------------------- */

static unsigned char arena[1 << 20];          /* 1 MiB is plenty           */
static size_t arena_off = 0;
static size_t capacity  = 0;                   /* bytes we may hand out     */

/* Registry of live block bases so heapFree can judge "valid free". */
#define MAX_BLOCKS 4096
static const void *blocks[MAX_BLOCKS];
static size_t nblocks = 0;

static int gcfg_force_fail = 0;               /* stub gcmGetConfiguration  */

/* The stub heap is shared by the hammer threads.  Guard its arena pointer
 * and block registry so the ONLY unsynchronised shared word left in the
 * process is __rsx_mm_last_error (owned by mm.c) -- which is the word the
 * atomicity row is checking.  If the stub itself raced, an ASan/UBSAN
 * report would name the harness, not the code under test. */
static pthread_mutex_t stub_mu = PTHREAD_MUTEX_INITIALIZER;

static void register_block(const void *p)
{
    if (nblocks < MAX_BLOCKS) blocks[nblocks++] = p;
}

static int is_block_base(const void *p)
{
    for (size_t i = 0; i < nblocks; i++)
        if (blocks[i] == p) return 1;
    return 0;
}

static void stub_reset(void)
{
    arena_off = 0;
    nblocks = 0;
}

/* ---- the four stubbed dependencies mm.c calls ------------------------ */

s32 gcmGetConfiguration(gcmConfiguration *config)
{
    if (gcfg_force_fail) return 1;             /* non-zero = failure */
    config->localAddress = arena;
    config->localSize    = (u32)(1 << 14);     /* 16 KiB capacity */
    return 0;                                  /* zero = success   */
}

uintptr_t heapInit(heap_cntrl *theheap, void *start_addr, uintptr_t size)
{
    (void)theheap;
    (void)start_addr;                          /* stub uses its own arena */
    pthread_mutex_lock(&stub_mu);
    capacity = (size_t)size;
    stub_reset();
    pthread_mutex_unlock(&stub_mu);
    return (uintptr_t)1;
}

void *heapAllocate(heap_cntrl *theheap, uintptr_t size)
{
    (void)theheap;
    pthread_mutex_lock(&stub_mu);
    if (nblocks >= MAX_BLOCKS) { pthread_mutex_unlock(&stub_mu); return NULL; }
    if (size > capacity || arena_off + size > capacity) { pthread_mutex_unlock(&stub_mu); return NULL; }
    void *p = arena + arena_off;
    arena_off += size;
    register_block(p);
    pthread_mutex_unlock(&stub_mu);
    return p;
}

void *heapAllocateAligned(heap_cntrl *theheap, uintptr_t size, uintptr_t alignment)
{
    (void)theheap;
    if (alignment == 0) alignment = 1;
    pthread_mutex_lock(&stub_mu);
    size_t base = arena_off;
    size_t aligned = (base + (alignment - 1)) & ~(size_t)(alignment - 1);
    if (aligned >= capacity || aligned + size > capacity) { pthread_mutex_unlock(&stub_mu); return NULL; }
    if (nblocks >= MAX_BLOCKS) { pthread_mutex_unlock(&stub_mu); return NULL; }
    void *p = arena + aligned;
    arena_off = aligned + size;
    register_block(p);
    pthread_mutex_unlock(&stub_mu);
    return p;
}

bool heapFree(heap_cntrl *theheap, void *ptr)
{
    (void)theheap;
    if (ptr == NULL) return false;
    pthread_mutex_lock(&stub_mu);
    if (!is_block_base(ptr)) { pthread_mutex_unlock(&stub_mu); return false; }  /* interior / out-of-range */
    for (size_t i = 0; i < nblocks; i++)
        if (blocks[i] == ptr) { blocks[i] = blocks[--nblocks]; break; }
    pthread_mutex_unlock(&stub_mu);
    return true;
}

/* ---- the test --------------------------------------------------------- */

static int failures = 0;

/* True for every code the library is documented to produce. */
static int is_defined_code(int c)
{
    if (c == PS3TC_RSX_MM_LASTERROR_OK)            return 1;
    if (c == PS3TC_RSX_MM_LASTERROR_GCM_CONFIG)    return 1;
    if (c == PS3TC_RSX_MM_LASTERROR_UNINIT)        return 1;
    if (c == PS3TC_RSX_MM_LASTERROR_ALLOCATION)    return 1;
    if (c == PS3TC_RSX_MM_LASTERROR_FREE)          return 1;
    return 0;
}

/* ---- concurrent-allocation hammer (codex change 1) --------------------
 * Two pthreads hammer rsxMalloc / rsxFree at the same time, then the
 * test asserts the final query returns a code from the defined set.
 * The only unsynchronised shared word left in the process is the one
 * mm.c owns (the stub heap itself is mutex-protected), so this row
 * exercises the __atomic store/load pair in mm.c and must stay clean
 * under ASan/UBSan.  The stub arena does not reclaim freed space, so
 * after the working set is exhausted rsxMalloc starts returning NULL
 * and interleaves ALLOCATION with OK verdicts; both are defined codes,
 * so the final-query assertion holds regardless of scheduling.  */
#define HAMMER_ITERS 2000
struct hammer {
    int iters;
};
static void *hammer_main(void *arg)
{
    struct hammer *h = (struct hammer *)arg;
    for (int i = 0; i < h->iters; i++) {
        void *p = rsxMalloc(64);
        if (p) rsxFree(p);
        /* also let the query read race the writer -- it is the read path
         * in the row that needs to be clean too */
        (void)ps3tc_rsx_mm_last_error();
    }
    return NULL;
}

static void expect(const char *what, int got, int want)
{
    if (got == want) {
        printf("  PASS  %-28s = %d\n", what, got);
    } else {
        printf("  FAIL  %-28s = %d (want %d)\n", what, got, want);
        failures++;
    }
}

static void expect_nonnull(const char *what, const char *s)
{
    if (s != NULL && s[0] != '\0') {
        printf("  PASS  %-28s = \"%s\"\n", what, s);
    } else {
        printf("  FAIL  %-28s = %s\n", what, s ? "(empty)" : "(null)");
        failures++;
    }
}

int main(void)
{
    printf("rsx mm last-error host test\n");
    printf("capacity after init = %zu bytes\n", (size_t)((1 << 14) - 4096));

    /* --- UNINIT: every call before rsxHeapInit ------------------------ */
    printf("[UNINIT]\n");
    if (rsxMalloc(64) != NULL) { printf("  FAIL  rsxMalloc returned non-NULL before init\n"); failures++; }
    expect("rsxMalloc before init", ps3tc_rsx_mm_last_error(), PS3TC_RSX_MM_LASTERROR_UNINIT);
    if (rsxMemalign(16, 64) != NULL) { printf("  FAIL  rsxMemalign returned non-NULL before init\n"); failures++; }
    expect("rsxMemalign before init", ps3tc_rsx_mm_last_error(), PS3TC_RSX_MM_LASTERROR_UNINIT);
    rsxFree(arena);
    expect("rsxFree before init", ps3tc_rsx_mm_last_error(), PS3TC_RSX_MM_LASTERROR_UNINIT);

    /* --- GCM_CONFIG: rsxHeapInit fails when gcmGetConfiguration fails --- */
    printf("[GCM_CONFIG]\n");
    gcfg_force_fail = 1;
    if (rsxHeapInit() != 0) { printf("  FAIL  rsxHeapInit must return 0 when gcmGetConfiguration fails\n"); failures++; }
    expect("rsxHeapInit failure code", ps3tc_rsx_mm_last_error(), PS3TC_RSX_MM_LASTERROR_GCM_CONFIG);
    gcfg_force_fail = 0;

    /* --- OK: successful init clears to OK ------------------------------ */
    printf("[OK init]\n");
    if (rsxHeapInit() != 1) { printf("  FAIL  rsxHeapInit must return 1 on success\n"); failures++; }
    expect("rsxHeapInit success code", ps3tc_rsx_mm_last_error(), PS3TC_RSX_MM_LASTERROR_OK);

    /* --- ALLOCATION: oversize request -> NULL -> ALLOCATION ------------- */
    printf("[ALLOCATION]\n");
    stub_reset();
    void *big = rsxMalloc((u32)(1 << 24));     /* 16 MiB >> capacity */
    if (big != NULL) { printf("  FAIL  rsxMalloc(16 MiB) should be NULL\n"); failures++; }
    expect("oversize rsxMalloc", ps3tc_rsx_mm_last_error(), PS3TC_RSX_MM_LASTERROR_ALLOCATION);
    stub_reset();
    void *big2 = rsxMemalign(16, (u32)(1 << 24));
    if (big2 != NULL) { printf("  FAIL  rsxMemalign(16 MiB) should be NULL\n"); failures++; }
    expect("oversize rsxMemalign", ps3tc_rsx_mm_last_error(), PS3TC_RSX_MM_LASTERROR_ALLOCATION);

    /* --- FREE: interior / out-of-range -> heapFree false ----------------
       NULL is a legal no-op (free(NULL) semantics) and must stay OK.   */
    printf("[FREE]\n");
    stub_reset();
    void *blk = rsxMalloc(64);
    if (blk == NULL) { printf("  FAIL  setup rsxMalloc(64) must succeed to test FREE\n"); failures++; return 1; }
    expect("valid allocate ok", ps3tc_rsx_mm_last_error(), PS3TC_RSX_MM_LASTERROR_OK);
    rsxFree((char *)blk + 4);                  /* interior pointer */
    expect("interior free", ps3tc_rsx_mm_last_error(), PS3TC_RSX_MM_LASTERROR_FREE);
    rsxFree(NULL);                             /* NULL pointer: legal no-op */
    expect("NULL free is a no-op (OK)", ps3tc_rsx_mm_last_error(), PS3TC_RSX_MM_LASTERROR_OK);
    rsxFree(arena + ((1 << 16) - 8));          /* out-of-range (never handed out) */
    expect("out-of-range free", ps3tc_rsx_mm_last_error(), PS3TC_RSX_MM_LASTERROR_FREE);

    /* --- OK: a valid free clears back to OK (sticky-clear contract) -----
       Force a FREE first, otherwise the preceding successful malloc has
       already set OK and this row would pass even if a valid free failed
       to clear the code.                                      */
    printf("[OK free]\n");
    stub_reset();
    void *blk2 = rsxMalloc(64);
    if (blk2 == NULL) { printf("  FAIL  setup rsxMalloc(64) must succeed\n"); failures++; return 1; }
    rsxFree((char *)blk2 + 4);               /* interior -> FREE first */
    expect("free an interior ptr -> FREE", ps3tc_rsx_mm_last_error(), PS3TC_RSX_MM_LASTERROR_FREE);
    rsxFree(blk2);                           /* now free the real block base */
    expect("valid free clears to OK", ps3tc_rsx_mm_last_error(), PS3TC_RSX_MM_LASTERROR_OK);

    /* --- THREADS: two pthreads hammer malloc/free; the query must observe
       a defined code and the build must stay clean under ASan/UBSan.  --- */
    printf("[THREADS]\n");
    stub_reset();
    struct hammer h = { .iters = HAMMER_ITERS };
    pthread_t t0, t1;
    if (pthread_create(&t0, NULL, hammer_main, &h) != 0 ||
        pthread_create(&t1, NULL, hammer_main, &h) != 0) {
        printf("  FAIL  pthread_create\n"); failures++; return 1;
    }
    pthread_join(t0, NULL);
    pthread_join(t1, NULL);
    int final_code = ps3tc_rsx_mm_last_error();
    if (is_defined_code(final_code)) {
        printf("  PASS  hammer: final code = %d (defined)\n", final_code);
    } else {
        printf("  FAIL  hammer: final code = %d (undefined)\n", final_code);
        failures++;
    }

    /* --- _string: every code has a stable, non-NULL description --------- */
    printf("[string]\n");
    expect_nonnull("OK string", ps3tc_rsx_mm_last_error_string(PS3TC_RSX_MM_LASTERROR_OK));
    expect_nonnull("GCM_CONFIG string", ps3tc_rsx_mm_last_error_string(PS3TC_RSX_MM_LASTERROR_GCM_CONFIG));
    expect_nonnull("UNINIT string", ps3tc_rsx_mm_last_error_string(PS3TC_RSX_MM_LASTERROR_UNINIT));
    expect_nonnull("ALLOCATION string", ps3tc_rsx_mm_last_error_string(PS3TC_RSX_MM_LASTERROR_ALLOCATION));
    expect_nonnull("FREE string", ps3tc_rsx_mm_last_error_string(PS3TC_RSX_MM_LASTERROR_FREE));
    expect_nonnull("unknown string", ps3tc_rsx_mm_last_error_string(-99));

    if (failures) {
        printf("rsx-mm-lasterror-test: FAIL (%d)\n", failures);
        return 1;
    }
    printf("rsx-mm-lasterror-test: PASS\n");
    return 0;
}
