/*
 * hello-ppu-spurs-task - Spurs taskset + SPU task end-to-end sample.
 *
 * 1. Spurs2 bring-up (4 SPUs, non-preemptible thread group).
 * 2. Class-0 Taskset on top of Spurs2.
 * 3. cellSpursCreateTask with our embedded SPU ELF.  The task's arg
 *    u64[0] is the EA of a 16-byte PPU-visible flag buffer.
 * 4. Busy-wait for the SPU task to DMA-put 0xC0FFEE into the flag.
 * 5. Shutdown + Join the taskset, then Spurs finalize.
 *
 * The SPU ELF is linked against libspurs_task.a with our custom
 * spurs_task.ld linker script - text starts at LS 0x3000
 * (CELL_SPURS_TASK_TOP), and the crt (__spurs_task_start) branches
 * into cellSpursMain then falls into cellSpursTaskExit.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>

#include <sys/process.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>

#include <cell/spurs.h>
#include <spu_printf.h>

#include "spu_task_bin.h"

SYS_PROCESS_PARAM(1001, 0x10000);

static const unsigned int kNumSpu            = 4;
static const int          kPpuThreadPriority = 2;
static const int          kSpuThreadPriority = 100;
static const char         kNamePrefix[]      = "HelloTsk";
static const int          kSpuPrintfPriority = 999;

static const uint8_t kTaskPriority[8]      = { 1, 1, 1, 1, 1, 1, 1, 1 };
static const unsigned int kMaxContention   = 4;
static const uint32_t kFlagExpected        = 0xc0ffeeU;

alignas(128) static volatile uint32_t g_flag[4] = { 0, 0, 0, 0 };

static bool init_spurs(cell::Spurs::Spurs2 *spurs)
{
    cell::Spurs::SpursAttribute attr;
    int rc = cell::Spurs::SpursAttribute::initialize(
        &attr, kNumSpu, kSpuThreadPriority, kPpuThreadPriority, false);
    if (rc) { std::printf("  SpursAttribute::initialize: %#x\n", rc); return false; }

    rc = attr.setNamePrefix(kNamePrefix, std::strlen(kNamePrefix));
    if (rc) { std::printf("  setNamePrefix: %#x\n", rc); return false; }

    rc = attr.setSpuThreadGroupType(SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT);
    if (rc) { std::printf("  setSpuThreadGroupType: %#x\n", rc); return false; }

    rc = attr.enableSpuPrintfIfAvailable();
    if (rc) { std::printf("  enableSpuPrintfIfAvailable: %#x\n", rc); return false; }

    rc = cell::Spurs::Spurs2::initialize(spurs, &attr);
    if (rc) { std::printf("  Spurs2::initialize: %#x\n", rc); return false; }
    std::printf("  Spurs2::initialize ok\n");
    return true;
}

static bool run_task(cell::Spurs::Spurs *spurs)
{
    /* Class-0 Taskset lives inline; sizeof matches CELL_SPURS_TASKSET_SIZE. */
    cell::Spurs::Taskset *taskset = static_cast<cell::Spurs::Taskset *>(
        ::aligned_alloc(CELL_SPURS_TASKSET_ALIGN, CELL_SPURS_TASKSET_SIZE));
    if (!taskset) { std::printf("  aligned_alloc(Taskset): FAILED\n"); return false; }

    int rc = cell::Spurs::Taskset::create(spurs, taskset,
                                          /*argTaskset=*/0,
                                          kTaskPriority, kMaxContention);
    if (rc) {
        std::printf("  Taskset::create: %#x\n", rc);
        std::free(taskset);
        return false;
    }
    std::printf("  Taskset::create ok\n");

    CellSpursTaskArgument arg = { { 0, 0, 0, 0 } };
    arg.u64[0] = reinterpret_cast<uint64_t>(&g_flag[0]);

    CellSpursTaskLsPattern lsPattern = { { 0, 0, 0, 0 } };

    CellSpursTaskId tid = 0;
    rc = taskset->createTask(&tid,
                             /*eaElf=*/spu_task_bin,
                             /*eaContext=*/nullptr,
                             /*sizeContext=*/0,
                             &lsPattern,
                             &arg);
    if (rc) {
        std::printf("  createTask: %#x\n", rc);
        taskset->shutdown();
        taskset->join();
        std::free(taskset);
        return false;
    }
    std::printf("  createTask ok, id=%u\n", tid);

    /* Busy-wait for the SPU task to DMA the flag in. */
    for (int i = 0; i < 100000000 && g_flag[0] != kFlagExpected; ++i) {
        __asm__ __volatile__("" ::: "memory");
    }

    if (g_flag[0] != kFlagExpected) {
        std::printf("  flag timeout: got 0x%08x expected 0x%08x\n",
                    (unsigned)g_flag[0], (unsigned)kFlagExpected);
        taskset->shutdown();
        taskset->join();
        std::free(taskset);
        return false;
    }
    std::printf("  SPU task wrote flag 0x%08x\n", (unsigned)g_flag[0]);

    rc = taskset->shutdown();
    if (rc) { std::printf("  shutdown: %#x\n", rc); std::free(taskset); return false; }

    rc = taskset->join();
    if (rc) { std::printf("  join: %#x\n", rc); std::free(taskset); return false; }
    std::printf("  Taskset shutdown+join ok\n");

    std::free(taskset);
    return true;
}

int main(void)
{
    std::printf("hello-ppu-spurs-task: SPU task round-trip\n");

    int rc = spu_printf_initialize(kSpuPrintfPriority, 0);
    if (rc) { std::printf("  spu_printf_initialize: %#x\n", rc); return 1; }

    cell::Spurs::Spurs2 *spurs = new cell::Spurs::Spurs2;
    if (!init_spurs(spurs)) { std::printf("FAILURE\n"); return 1; }

    if (!run_task(spurs)) {
        std::printf("FAILURE\n");
        spurs->finalize();
        delete spurs;
        spu_printf_finalize();
        return 1;
    }

    rc = spurs->finalize();
    if (rc) { std::printf("  finalize: %#x\n", rc); return 1; }
    std::printf("  Spurs finalize ok\n");

    delete spurs;
    spu_printf_finalize();

    std::printf("SUCCESS\n");
    return 0;
}
