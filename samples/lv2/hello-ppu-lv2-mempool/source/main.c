/*
 * hello-ppu-lv2-mempool - LV2 random-number, mempool and spinlock surface.
 */
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

#include <sys/process.h>
#include <sys/random_number.h>
#include <sys/mempool.h>
#include <sys/spinlock.h>

SYS_PROCESS_PARAM(1001, 0x10000);

/* ----  step helpers ------------------------------------------------------ */

static void pass(const char *step) { printf("PASS %s\n", step); }
static void fail(const char *step, unsigned rc)
{
    printf("FAIL %s rc=0x%08x\n", step, rc);
}

/* ----  (a) random number ------------------------------------------------- */
static int step_random(void)
{
    uint8_t buf[64];
    int      rc = sys_get_random_number(buf, sizeof(buf));
    if (rc != 0) { fail("random-number", (unsigned)rc); return rc; }

    int all_zero = 1;
    for (size_t i = 0; i < sizeof(buf); i++)
        if (buf[i]) { all_zero = 0; break; }
    if (all_zero)
    {
        printf("FAIL random-number (all zero)\n");
        return -1;
    }

    printf("  ");
    for (size_t i = 0; i < sizeof(buf); i++) printf("%02x", buf[i]);
    printf("\n");
    pass("random-number");
    return 0;
}

/* ----  (b) mempool ------------------------------------------------------- */
static int step_mempool(void)
{
    const uint64_t BLOCKS  = 8;
    const uint64_t BSIZE   = 128;
    const uint64_t CHUNK   = BLOCKS * BSIZE;
    static uint8_t chunk[CHUNK] __attribute__((aligned(128)));

    sys_mempool_t pool = 0;
    int rc = sys_mempool_create(&pool, chunk, CHUNK, BSIZE, 0);
    if (rc != 0) { fail("mempool-create", (unsigned)rc); return rc; }

    void *slots[BLOCKS];
    for (size_t i = 0; i < BLOCKS; i++) {
        slots[i] = sys_mempool_allocate_block(pool);
        if (!slots[i]) {
            fail("mempool-allocate", 0);
            for (size_t j = 0; j < i; j++) sys_mempool_free_block(pool, slots[j]);
            sys_mempool_destroy(pool);
            return -1;
        }
    }

    /* try_allocate must return NULL when the pool is empty */
    if (sys_mempool_try_allocate_block(pool) != NULL) {
        printf("  mempool: try_allocate returned non-NULL on an empty pool\n");
        fail("mempool-try-allocate-exhausted", 0);
        for (size_t i = 0; i < BLOCKS; i++) sys_mempool_free_block(pool, slots[i]);
        sys_mempool_destroy(pool);
        return -1;
    }
    pass("mempool-try-allocate-exhausted");

    /* get_count must report 0 free blocks */
    if (sys_mempool_get_count(pool) != 0) {
        printf("  mempool: get_count = %llu, want 0 free\n",
               (unsigned long long)sys_mempool_get_count(pool));
        fail("mempool-get-count-exhausted", 0);
        for (size_t i = 0; i < BLOCKS; i++) sys_mempool_free_block(pool, slots[i]);
        sys_mempool_destroy(pool);
        return -1;
    }
    pass("mempool-get-count-exhausted");

    /* free one and re-allocate it */
    if (sys_mempool_free_block(pool, slots[0]) != 0) {
        fail("mempool-free", 0);
        for (size_t i = 1; i < BLOCKS; i++) sys_mempool_free_block(pool, slots[i]);
        sys_mempool_destroy(pool);
        return -1;
    }
    void *again = sys_mempool_allocate_block(pool);
    if (!again) {
        printf("  mempool: re-allocate returned NULL after freeing one\n");
        fail("mempool-reallocate", 0);
        for (size_t i = 1; i < BLOCKS; i++) sys_mempool_free_block(pool, slots[i]);
        sys_mempool_destroy(pool);
        return -1;
    }
    pass("mempool-reallocate");

    /* free the rest and destroy */
    for (size_t i = 1; i < BLOCKS; i++) sys_mempool_free_block(pool, slots[i]);
    sys_mempool_destroy(pool);
    pass("mempool-destroy");
    return 0;
}

/* ----  (c) spinlock ------------------------------------------------------ */
static int      spinlock       = 0;
static int      shared_counter = 0;
static const int INCREMENTS   = 100000;

static void *bump_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < INCREMENTS; i++)
    {
        sys_spinlock_lock(&spinlock);
        shared_counter++;
        sys_spinlock_unlock(&spinlock);
    }
    return NULL;
}

static int step_spinlock(void)
{
    sys_spinlock_initialize(&spinlock);

    pthread_t t0, t1;
    int rc0 = pthread_create(&t0, NULL, bump_worker, NULL);
    if (rc0) { fail("spinlock-pthread-create-0", 0); return rc0; }
    int rc1 = pthread_create(&t1, NULL, bump_worker, NULL);
    if (rc1) { fail("spinlock-pthread-create-1", 0); return rc1; }

    pthread_join(t0, NULL);
    pthread_join(t1, NULL);

    if (shared_counter != 2 * INCREMENTS)
    {
        printf("  spinlock: counter = %d, want %d\n",
               shared_counter, 2 * INCREMENTS);
        fail("spinlock-counter", 0);
        return -1;
    }
    pass("spinlock-counter");
    return 0;
}

int main(void)
{
    printf("hello-ppu-lv2-mempool: LV2 random-number / mempool / spinlock\n");

    int rcs = 0;
    if (step_random()   != 0) rcs = 1;
    if (step_mempool()  != 0) rcs = 1;
    if (step_spinlock() != 0) rcs = 1;

    if (rcs == 0) {
        printf("hello-ppu-lv2-mempool: PASS\n");
        return 0;
    }
    printf("hello-ppu-lv2-mempool: FAIL\n");
    return rcs;
}
