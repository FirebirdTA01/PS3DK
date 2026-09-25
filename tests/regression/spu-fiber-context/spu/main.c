/* spu-fiber-context (SPU side): exercise cell/fiber/spu_context.h.
 *
 * Exits with 0 when every check passes, otherwise with the number of the
 * first failed check (the PPU side prints it).
 */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <sys/spu_thread.h>
#include <cell/fiber/spu_context.h>

static CellFiberSpuContext main_ctx, a_ctx, b_ctx, c_ctx;
static uint8_t a_stack[8192] __attribute__((aligned(16)));
static uint8_t b_stack[8192] __attribute__((aligned(16)));
static uint8_t c_stack[8192] __attribute__((aligned(16)));

static volatile int counter;
static volatile int failed;
static volatile int b_self_ok = 1;

/* Canary check at the minimum stack size: 16 guard bytes below and above a
 * 64-byte stack, which the runtime and a minimal entry must not touch. */
#define MIN_STACK 64
static uint8_t canary_area[16 + MIN_STACK + 16] __attribute__((aligned(16)));
static CellFiberSpuContext d_ctx;
static volatile uintptr_t d_seen;

#define CHECK(n, cond) do { if (!(cond) && !failed) failed = (n); } while (0)

/* Fiber A: ten rounds, handing over to B each time.  Its accumulators live
 * across every switch, so the compiler keeps them in callee-saved
 * registers; a switch that failed to restore $80-$127 corrupts them. */
static void fiber_a(uint64_t rounds)
{
    uint32_t acc0 = 1, acc1 = 3, acc2 = 5, acc3 = 7;
    vec_uint4 v = spu_splats((uint32_t)0x01020304);
    for (uint64_t i = 0; i < rounds; ++i) {
        CHECK(10, cellFiberSpuContextSelf() == &a_ctx);
        counter += 1;
        acc0 = acc0 * 3 + 1; acc1 = acc1 * 5 + 2; acc2 ^= acc0 + acc1; acc3 += acc2;
        v = spu_add(v, spu_splats((uint32_t)1));
        cellFiberSpuContextSwitch(&b_ctx);
    }
    /* Same arithmetic done straight through: the values must match. */
    uint32_t e0 = 1, e1 = 3, e2 = 5, e3 = 7;
    for (uint64_t i = 0; i < rounds; ++i) {
        e0 = e0 * 3 + 1; e1 = e1 * 5 + 2; e2 ^= e0 + e1; e3 += e2;
    }
    CHECK(11, acc0 == e0 && acc1 == e1 && acc2 == e2 && acc3 == e3);
    CHECK(12, spu_extract(v, 0) == 0x01020304u + (uint32_t)rounds
              && spu_extract(v, 3) == 0x01020304u + (uint32_t)rounds);
    /* Returning ends the fiber run: cellFiberSpuContextRun returns. */
}

/* Fiber B: bounces back to A forever; never returns. */
static void fiber_b(uint64_t unused)
{
    (void)unused;
    for (;;) {
        if (cellFiberSpuContextSelf() != &b_ctx)
            b_self_ok = 0;
        counter += 100;
        uint8_t probe[16] __attribute__((aligned(16)));
        CHECK(13, ((uintptr_t)probe & 15) == 0);
        cellFiberSpuContextSwitch(&a_ctx);
    }
}

/* Fiber D: a minimal non-leaf entry (one call, one 32-byte frame). */
static void fiber_d(uint64_t unused)
{
    (void)unused;
    d_seen = (uintptr_t)cellFiberSpuContextSelf();
}

/* Fiber C: calls Run from inside a fiber, which must be refused. */
static void fiber_c(uint64_t unused)
{
    (void)unused;
    CHECK(14, cellFiberSpuContextRun(&a_ctx, &main_ctx) == (int)CELL_FIBER_ERROR_PERM);
}

int main(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    (void)arg1; (void)arg2; (void)arg3; (void)arg4;
    CellFiberSpuContext *mis = (CellFiberSpuContext *)((uintptr_t)&c_ctx + 8);

    /* Argument checks. */
    CHECK(1, cellFiberSpuContextSelf() == NULL);
    CHECK(2, cellFiberSpuContextSwitch(&a_ctx) == (int)CELL_FIBER_ERROR_PERM);
    CHECK(3, cellFiberSpuContextSwitch(NULL) == (int)CELL_FIBER_ERROR_NULL_POINTER);
    CHECK(4, cellFiberSpuContextSwitch(mis) == (int)CELL_FIBER_ERROR_ALIGN);
    CHECK(5, cellFiberSpuContextInitialize(NULL, fiber_a, 0, a_stack, sizeof a_stack)
                 == (int)CELL_FIBER_ERROR_NULL_POINTER);
    CHECK(6, cellFiberSpuContextInitialize(&a_ctx, fiber_a, 0, a_stack + 8, 4096)
                 == (int)CELL_FIBER_ERROR_ALIGN);
    CHECK(7, cellFiberSpuContextRun(&a_ctx, NULL) == (int)CELL_FIBER_ERROR_NULL_POINTER);

    /* Ping-pong: A runs 10 rounds (+1), B answers each (+100). */
    CHECK(8, cellFiberSpuContextInitialize(&a_ctx, fiber_a, 10, a_stack, sizeof a_stack) == CELL_OK);
    CHECK(8, cellFiberSpuContextInitialize(&b_ctx, fiber_b, 0, b_stack, sizeof b_stack) == CELL_OK);
    CHECK(9, cellFiberSpuContextRun(&a_ctx, &main_ctx) == CELL_OK);
    CHECK(15, counter == 1010);
    CHECK(16, b_self_ok);
    CHECK(17, cellFiberSpuContextSelf() == NULL);

    /* Run refused from inside a fiber; a second Run works afterwards. */
    CHECK(18, cellFiberSpuContextInitialize(&c_ctx, fiber_c, 0, c_stack, sizeof c_stack) == CELL_OK);
    CHECK(18, cellFiberSpuContextRun(&c_ctx, &main_ctx) == CELL_OK);
    CHECK(19, cellFiberSpuContextSelf() == NULL);

    /* Below the minimum is refused; exactly the minimum runs and writes
     * nothing outside the supplied stack. */
    for (unsigned i = 0; i < sizeof canary_area; ++i)
        canary_area[i] = 0xa5;
    CHECK(20, cellFiberSpuContextInitialize(&d_ctx, fiber_d, 0, canary_area + 16, MIN_STACK - 16)
                  == (int)CELL_FIBER_ERROR_INVAL);
    CHECK(21, cellFiberSpuContextInitialize(&d_ctx, fiber_d, 0, canary_area + 16, MIN_STACK) == CELL_OK);
    CHECK(21, cellFiberSpuContextRun(&d_ctx, &main_ctx) == CELL_OK);
    CHECK(22, d_seen == (uintptr_t)&d_ctx);
    for (unsigned i = 0; i < 16; ++i) {
        CHECK(23, canary_area[i] == 0xa5);
        CHECK(24, canary_area[16 + MIN_STACK + i] == 0xa5);
    }

    spu_thread_exit(failed);
    return 0;
}
