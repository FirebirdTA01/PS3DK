/*
 * hello-spurs-task2 - Taskset2 / Task2 API sample.
 *
 * 1. Spurs2 bring-up (4 SPUs, EXCLUSIVE_NON_CONTEXT).
 * 2. Cell::Spurs::Taskset2 on top of Spurs2 (class-1 taskset).
 * 3. Three Task2 tasks of the same SPU program, created with
 *    createTask2 (arg.u32[0] = task number) and joined with
 *    joinTask2.  Each task returns 100 + task number.
 * 4. Expect prints:
 *      hello-spurs-task2: task 0 exit code 100
 *      hello-spurs-task2: task 1 exit code 101
 *      hello-spurs-task2: task 2 exit code 102
 *      hello-spurs-task2: DONE
 *
 * SPU side: int cellSpursTaskMain(qword, uint64) returning 100 + arg.
 * Linked via -nostartfiles + spurs_task.ld + libspurs_task, the same
 * recipe as hello-spurs-task.
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
static const char         kNamePrefix[]      = "HelloT2";
static const unsigned int kNumTasks          = 3;
static const int          kSpuPrintfPriority = 999;

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

static bool run_task2_demo(cell::Spurs::Spurs2 *spurs)
{
    cell::Spurs::Taskset2 *ts2 = static_cast<cell::Spurs::Taskset2 *>(
        ::aligned_alloc(CELL_SPURS_TASKSET2_ALIGN, CELL_SPURS_TASKSET2_SIZE));
    if (!ts2) { std::printf("  aligned_alloc(Taskset2): FAILED\n"); return false; }

    CellSpursTasksetAttribute2 tsAttr;
    cell::Spurs::TasksetAttribute2::initialize(&tsAttr);

    int rc = cell::Spurs::Taskset2::create(spurs, ts2, &tsAttr);
    if (rc) { std::printf("  Taskset2::create: %#x\n", rc); std::free(ts2); return false; }
    std::printf("  Taskset2::create ok\n");

    bool ok = true;
    for (unsigned i = 0; i < kNumTasks && ok; ++i) {
        /* Each task gets its own context save area (the Task2 path may
         * block), so allocate per task rather than sharing one block. */
        CellSpursTaskAttribute2 attr;
        cell::Spurs::TaskAttribute2::initialize(&attr);
        attr.sizeContext = CELL_SPURS_TASK_CONTEXT_SIZE_ALL;
        attr.eaContext   = reinterpret_cast<uintptr_t>(
            ::aligned_alloc(CELL_SPURS_TASK_CONTEXT_ALIGN, CELL_SPURS_TASK_CONTEXT_SIZE_ALL));
        attr.lsPattern   = { { CELL_SPURS_TASK_TOP_MASK,
                               0xffffffffU, 0xffffffffU, 0xffffffffU } };

        CellSpursTaskArgument arg;
        std::memset(&arg, 0, sizeof arg);
        arg.u32[0] = static_cast<uint32_t>(i);

        CellSpursTaskId id;
        rc = ts2->createTask2(&id, spu_task_bin, &arg, &attr);
        if (rc) {
            std::printf("  createTask2(%u): %#x\n", i, rc);
            ts2->shutdown();
            ts2->join();
            ok = false;
            break;
        }
        std::printf("  createTask2(%u) ok, id=%u\n", i, (unsigned)id);

        int code = 0;
        rc = ts2->joinTask2(id, &code);
        if (rc) {
            std::printf("  joinTask2(%u): rc=%#x\n", i, rc);
            ts2->shutdown();
            ts2->join();
            ok = false;
            break;
        }
        std::printf("hello-spurs-task2: task %u exit code %d\n", i, code);
    }

    if (ok) {
        rc = ts2->shutdown();
        if (rc) std::printf("  shutdown: %#x\n", rc);
        rc = ts2->join();
        if (rc) std::printf("  join: %#x\n", rc);
    }
    std::free(ts2);
    return ok;
}

int main(void)
{
    std::printf("hello-spurs-task2: Taskset2 / Task2 sample\n");

    int rc = spu_printf_initialize(kSpuPrintfPriority, 0);
    if (rc) std::printf("  spu_printf_initialize: %#x\n", rc);

    cell::Spurs::Spurs2 *spurs = new cell::Spurs::Spurs2;
    if (!init_spurs(spurs)) {
        std::printf("FAILURE\n");
        spu_printf_finalize();
        return 1;
    }

    if (!run_task2_demo(spurs)) {
        std::printf("FAILURE\n");
        spurs->finalize();
        delete spurs;
        spu_printf_finalize();
        return 1;
    }

    rc = spurs->finalize();
    if (rc) {
        std::printf("  finalize: %#x\n", rc);
        delete spurs;
        spu_printf_finalize();
        return 1;
    }

    delete spurs;
    spu_printf_finalize();
    std::printf("hello-spurs-task2: DONE\n");
    return 0;
}
