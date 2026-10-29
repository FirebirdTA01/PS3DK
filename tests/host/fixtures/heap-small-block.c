/* Exercise the real block allocator; only the host spinlock is stubbed. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ppu-asm.h>
#include <sys/heap.h>

#ifdef HEAP_HOST_TEST
void sysSpinlockInitialize(s32 *p) { *p = 0; }
void sysSpinlockLock(s32 *p) { (void)p; }
void sysSpinlockUnlock(s32 *p) { (void)p; }
#endif

static unsigned char arena[262144] __attribute__((aligned(4096)));
static heap_cntrl heap;
static uintptr_t capacity;

#define REQUIRE(x) do { if (!(x)) { \
    printf("FAIL line=%d: %s\n", __LINE__, #x); return 0; } } while (0)

static int init(void)
{
    memset(arena, 0xa5, sizeof(arena));
    capacity = heapInit(&heap, arena, sizeof(arena));
    REQUIRE(capacity != 0);
    return 1;
}

static int free_list_valid(void)
{
    heap_block *sentinel = &heap.free_list, *previous = sentinel;
    heap_block *block = sentinel->next;
    unsigned count = 0;
    while (block != sentinel) {
        uintptr_t address = (uintptr_t)block;
        REQUIRE(++count < 16384);
        REQUIRE(address >= (uintptr_t)heap.first_block);
        REQUIRE(address < (uintptr_t)heap.last_block);
        /* Check before following either link: RED must report corruption
           rather than dereference the overwritten predecessor (0x18). */
        REQUIRE(block->prev == previous);
        previous = block;
        block = block->next;
    }
    REQUIRE(sentinel->prev == previous);
    return 1;
}

static int completely_free(void)
{
    REQUIRE(free_list_valid());
    REQUIRE(heap.free_list.next == heap.first_block);
    REQUIRE(heap.free_list.prev == heap.first_block);
    REQUIRE(heap.first_block->next == &heap.free_list);
    REQUIRE((heap.first_block->size & ~HEAP_BLOCK_PREV_USED) == capacity);
    return 1;
}

static int isolated_tiny(void)
{
    REQUIRE(init());
    void *small = heapAllocate(&heap, 8);
    void *guard = heapAllocate(&heap, 8);
    REQUIRE(small && guard);
    memset(guard, 0x76, 8);
    REQUIRE(heapFree(&heap, small));
    REQUIRE(free_list_valid());
    for (unsigned i = 0; i < 8; ++i) REQUIRE(((unsigned char *)guard)[i] == 0x76);
    REQUIRE(heapFree(&heap, guard));
    return completely_free();
}

static int boundaries(void)
{
    void *p[40];
    REQUIRE(init());
    for (unsigned i = 0; i < 40; ++i) {
        p[i] = heapAllocate(&heap, i + 1);
        REQUIRE(p[i]);
        memset(p[i], i + 1, i + 1);
    }
    for (unsigned parity = 0; parity < 2; ++parity)
        for (unsigned i = parity; i < 40; i += 2) {
            for (unsigned j = 0; j <= i; ++j)
                REQUIRE(((unsigned char *)p[i])[j] == i + 1);
            REQUIRE(heapFree(&heap, p[i]));
            REQUIRE(free_list_valid());
        }
    return completely_free();
}

static int coalescing(unsigned permutation)
{
    static const unsigned order[6][3] = {
        {0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0}
    };
    void *p[3], *guard;
    REQUIRE(init());
    for (unsigned i = 0; i < 3; ++i) {
        p[i] = heapAllocate(&heap, 8);
        REQUIRE(p[i]);
    }
    guard = heapAllocate(&heap, 8);
    REQUIRE(guard);
    for (unsigned i = 0; i < 3; ++i) {
        REQUIRE(heapFree(&heap, p[order[permutation][i]]));
        REQUIRE(free_list_valid());
    }
    REQUIRE(heapFree(&heap, guard));
    return completely_free();
}

static uint32_t random_state = 0x52b7440d;
static uint32_t next_random(void)
{
    random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
    return random_state;
}

static int churn(int aligned)
{
    void *p[32] = {0};
    unsigned sizes[32] = {0}, values[32] = {0};
    REQUIRE(init());
    for (unsigned step = 0; step < 3000; ++step) {
        unsigned slot = (next_random() >> 16) % 32;
        for (unsigned i = 0; i < 32; ++i)
            if (p[i])
                for (unsigned j = 0; j < sizes[i]; ++j)
                    REQUIRE(((unsigned char *)p[i])[j] == values[i]);
        if (p[slot]) {
            REQUIRE(heapFree(&heap, p[slot]));
            p[slot] = NULL;
            REQUIRE(free_list_valid());
        }
        sizes[slot] = 1 + (next_random() >> 16) % 257;
        unsigned alignment = aligned ? 8u << ((next_random() >> 16) % 6) : 0;
        p[slot] = heapAllocateAligned(&heap, sizes[slot], alignment);
        REQUIRE(p[slot]);
        if (alignment) REQUIRE((uintptr_t)p[slot] % alignment == 0);
        values[slot] = step % 251;
        memset(p[slot], values[slot], sizes[slot]);
        REQUIRE(free_list_valid());
    }
    for (unsigned i = 0; i < 32; ++i) {
        REQUIRE(heapFree(&heap, p[i]));
        REQUIRE(free_list_valid());
    }
    return completely_free();
}

int main(int argc, char **argv)
{
    unsigned first = 0, end = 10, failed = 0;
    if (argc == 2) { first = (unsigned)atoi(argv[1]); end = first + 1; }
    if (end > 10) return 2;
    for (unsigned test = first; test < end; ++test) {
        int ok = test == 0 ? isolated_tiny() : test == 1 ? boundaries() :
                 test < 8 ? coalescing(test - 2) : churn(test == 9);
        printf("HEAP_SMALL_CASE id=%u result=%s\n", test, ok ? "PASS" : "FAIL");
        failed += !ok;
    }
    printf("HEAP_SMALL_%s tests=%u pass=%u fail=%u\n", failed ? "FAIL" : "OK",
           end - first, end - first - failed, failed);
    return failed ? 1 : 0;
}
