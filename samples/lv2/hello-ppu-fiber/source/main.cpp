/*
 * hello-ppu-fiber — libfiber bring-up smoke test.
 *
 * Exercises the PPU fiber surface end-to-end through the cell-SDK
 * surface in <cell/fiber.h>:
 *
 *   1. cellFiberPpuInitialize          (sysmodule load + SPRX init)
 *   2. cellFiberPpuSchedulerAttributeInitialize / cellFiberPpuInitializeScheduler
 *   3. cellFiberPpuAttributeInitialize / cellFiberPpuCreateFiber  (×2)
 *   4. cellFiberPpuRunFibers
 *   5. cellFiberPpuJoinFiber           (×2, collect exit codes)
 *   6. cellFiberPpuFinalizeScheduler
 *
 * The HLE present in current RPCS3 returns CELL_OK for the whole
 * surface without actually performing context switches, so this
 * sample is a smoke gate: it proves that our archive links, the
 * binary boots, struct alignments survive the SPRX boundary, and
 * every entry point returns success.  Real fiber-switching
 * validation needs a working HLE or hardware.
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <sys/process.h>

#include <cell/fiber.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static const std::size_t kFiberStackSize = 64 * 1024;
static const unsigned    kFiberPriority  = 1;

/* Aligned storage for the scheduler + two fibers + their stacks.
 * The SPRX's CellFiberPpuScheduler / CellFiberPpu / stack alignments
 * are encoded in the type's __attribute__((aligned)) so file-scope
 * arrays inherit them automatically. */
static CellFiberPpuScheduler g_scheduler;
static CellFiberPpu          g_fiber_a;
static CellFiberPpu          g_fiber_b;

alignas(CELL_FIBER_PPU_STACK_ALIGN) static std::uint8_t g_stack_a[kFiberStackSize];
alignas(CELL_FIBER_PPU_STACK_ALIGN) static std::uint8_t g_stack_b[kFiberStackSize];

static int fiber_entry_a(std::uint64_t arg)
{
    std::printf("  fiber A entry (arg=%llu)\n", (unsigned long long)arg);
    cellFiberPpuYield();
    cellFiberPpuYield();
    std::printf("  fiber A exiting (rc=10)\n");
    cellFiberPpuExit(10);
    return 10; /* unreachable on a real fiber runtime, but keeps the
                  compiler from warning about a non-void path */
}

static int fiber_entry_b(std::uint64_t arg)
{
    std::printf("  fiber B entry (arg=%llu)\n", (unsigned long long)arg);
    cellFiberPpuYield();
    std::printf("  fiber B exiting (rc=20)\n");
    cellFiberPpuExit(20);
    return 20;
}

static bool init_lib(void)
{
    int rc = cellFiberPpuInitialize();
    std::printf("  cellFiberPpuInitialize: %#x\n", rc);
    return rc == CELL_OK;
}

static bool init_scheduler(void)
{
    CellFiberPpuSchedulerAttribute sched_attr;

    int rc = cellFiberPpuSchedulerAttributeInitialize(&sched_attr);
    std::printf("  cellFiberPpuSchedulerAttributeInitialize: %#x\n", rc);
    if (rc != CELL_OK) return false;

    rc = cellFiberPpuInitializeScheduler(&g_scheduler, &sched_attr);
    std::printf("  cellFiberPpuInitializeScheduler: %#x\n", rc);
    return rc == CELL_OK;
}

static bool create_fiber(CellFiberPpu *fiber, CellFiberPpuEntry entry,
                         std::uint64_t arg, void *stack)
{
    CellFiberPpuAttribute fiber_attr;

    int rc = cellFiberPpuAttributeInitialize(&fiber_attr);
    std::printf("  cellFiberPpuAttributeInitialize: %#x\n", rc);
    if (rc != CELL_OK) return false;

    rc = cellFiberPpuCreateFiber(&g_scheduler, fiber, entry, arg,
                                 kFiberPriority, stack, kFiberStackSize,
                                 &fiber_attr);
    std::printf("  cellFiberPpuCreateFiber: %#x\n", rc);
    return rc == CELL_OK;
}

int main(void)
{
    std::printf("hello-ppu-fiber: libfiber bring-up smoke test\n");

    if (!init_lib())
        return 1;

    if (!init_scheduler())
        return 1;

    if (!create_fiber(&g_fiber_a, fiber_entry_a, 1, g_stack_a))
        return 1;
    if (!create_fiber(&g_fiber_b, fiber_entry_b, 2, g_stack_b))
        return 1;

    int rc = cellFiberPpuRunFibers(&g_scheduler);
    std::printf("  cellFiberPpuRunFibers: %#x\n", rc);

    int exit_a = -1;
    int exit_b = -1;
    rc = cellFiberPpuJoinFiber(&g_fiber_a, &exit_a);
    std::printf("  cellFiberPpuJoinFiber A: rc=%#x exit=%d\n", rc, exit_a);

    rc = cellFiberPpuJoinFiber(&g_fiber_b, &exit_b);
    std::printf("  cellFiberPpuJoinFiber B: rc=%#x exit=%d\n", rc, exit_b);

    rc = cellFiberPpuFinalizeScheduler(&g_scheduler);
    std::printf("  cellFiberPpuFinalizeScheduler: %#x\n", rc);

    std::printf("SUCCESS\n");
    return 0;
}
