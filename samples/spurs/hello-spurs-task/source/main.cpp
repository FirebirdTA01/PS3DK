/*
 * hello-ppu-spurs-task - Spurs taskset lifecycle smoke test (PPU only).
 *
 * Stands up a Spurs2 instance, creates a Taskset2 on top of it, then
 * destroys the taskset and finalizes.  No SPU-side task code is run -
 * the SPU-side task runtime (the moral equivalent of Sony's
 * -mspurs-task crt) is a future work item.  This sample exercises
 * the PPU-level cell::Spurs::Taskset2 / TasksetAttribute2 C++ surface
 * and verifies that cellSpursCreateTaskset2 + cellSpursDestroyTaskset2
 * NIDs resolve and round-trip against libspurs_stub.a.
 *
 * Expected output:
 *   hello-ppu-spurs-task: taskset lifecycle smoke test
 *     spu_printf_initialize ok
 *     SpursAttribute::initialize ok
 *     Spurs2::initialize ok
 *     Taskset2::create ok
 *     Taskset2::destroy ok
 *     finalize ok
 *   SUCCESS
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <sys/process.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>

#include <cell/spurs.h>
#include <spu_printf.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static const unsigned int kNumSpu            = 4;
static const int          kPpuThreadPriority = 2;
static const int          kSpuThreadPriority = 100;
static const char         kNamePrefix[]      = "HelloTsk";
static const int          kSpuPrintfPriority = 999;

static bool init_spurs(cell::Spurs::Spurs2 *spurs)
{
    cell::Spurs::SpursAttribute attr;

    int rc = cell::Spurs::SpursAttribute::initialize(
        &attr, kNumSpu, kSpuThreadPriority, kPpuThreadPriority,
        /* exitIfNoWork = */ false);
    if (rc) { std::printf("  SpursAttribute::initialize: %#x\n", rc); return false; }
    std::printf("  SpursAttribute::initialize ok\n");

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

static bool run_taskset_lifecycle(cell::Spurs::Spurs2 *spurs)
{
    cell::Spurs::Taskset2 *taskset = static_cast<cell::Spurs::Taskset2 *>(
        ::aligned_alloc(CELL_SPURS_TASKSET2_ALIGN, CELL_SPURS_TASKSET2_SIZE));
    if (!taskset) { std::printf("  aligned_alloc(Taskset2) -- FAILED\n"); return false; }

    cell::Spurs::TasksetAttribute2 attr;
    cell::Spurs::TasksetAttribute2::initialize(&attr);
    attr.name = "hello_taskset";

    int rc = cell::Spurs::Taskset2::create(spurs, taskset, &attr);
    if (rc) {
        std::printf("  Taskset2::create: %#x\n", rc);
        std::free(taskset);
        return false;
    }
    std::printf("  Taskset2::create ok\n");

    rc = taskset->destroy();
    if (rc) {
        std::printf("  Taskset2::destroy: %#x\n", rc);
        std::free(taskset);
        return false;
    }
    std::printf("  Taskset2::destroy ok\n");

    std::free(taskset);
    return true;
}

int main(void)
{
    std::printf("hello-ppu-spurs-task: taskset lifecycle smoke test\n");

    int rc = spu_printf_initialize(kSpuPrintfPriority, 0);
    if (rc) { std::printf("  spu_printf_initialize: %#x -- FAILED\n", rc); return 1; }
    std::printf("  spu_printf_initialize ok\n");

    cell::Spurs::Spurs2 *spurs = new cell::Spurs::Spurs2;
    if (!spurs) {
        std::printf("  new Spurs2 -- FAILED\n");
        spu_printf_finalize();
        return 1;
    }

    if (!init_spurs(spurs)) {
        std::printf("FAILURE\n");
        delete spurs;
        spu_printf_finalize();
        return 1;
    }

    if (!run_taskset_lifecycle(spurs)) {
        std::printf("FAILURE\n");
        spurs->finalize();
        delete spurs;
        spu_printf_finalize();
        return 1;
    }

    rc = spurs->finalize();
    if (rc) {
        std::printf("  finalize: %#x -- FAILED\n", rc);
        delete spurs;
        spu_printf_finalize();
        return 1;
    }
    std::printf("  finalize ok\n");

    delete spurs;

    rc = spu_printf_finalize();
    if (rc) std::printf("  spu_printf_finalize: %#x (non-fatal)\n", rc);

    std::printf("SUCCESS\n");
    return 0;
}
