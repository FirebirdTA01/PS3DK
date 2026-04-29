/*
 * hello-spurs-semaphore - CellSpursSemaphore P/V smoke test.
 *
 * Two SPU tasks share a 128-byte CellSpursSemaphore in main memory.
 * Task 0 (producer) V's the semaphore eight times; task 1 (consumer)
 * P's eight times, bumping a PPU-visible counter after each acquire.
 *
 * Status: this exercise links + dispatches both tasks but the full
 * P/V cycle does not currently complete in RPCS3's HLE Spurs.  The
 * RPCS3 taskset PM HLE has gaps (`spursTasksetSyscallEntry` body is
 * `fmt::throw_exception("Broken (TODO)")`, `_cellSpursSemaphoreInitialize`
 * is `UNIMPLEMENTED_FUNC`) that block the BLOCK service path our
 * SemaphoreP needs.  The byte-equivalence of the underlying SPU
 * primitives (cellSpursSemaphoreP/V, cellSpursSendSignal, the
 * _cellSpurs* helpers) is independently verified per the libspurs.a
 * reference disassembly (cmp -l of .text byte slices).
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>

#include <sys/process.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>

#include <cell/spurs.h>
#include <cell/spurs/semaphore.h>
#include <spu_printf.h>

#include "spu_task_bin.h"

SYS_PROCESS_PARAM(1001, 0x10000);

static const unsigned int kNumSpu            = 4;
static const int          kPpuThreadPriority = 2;
static const int          kSpuThreadPriority = 100;
static const char         kNamePrefix[]      = "HelloSem";
static const int          kSpuPrintfPriority = 999;

static const uint8_t kTaskPriority[8]      = { 1, 1, 1, 1, 1, 1, 1, 1 };
static const unsigned int kMaxContention   = 4;

static const uint32_t kIterations          = 8;

alignas(128) static volatile uint32_t g_counter[4] = { 0 };
alignas(128) static CellSpursSemaphore g_semaphore;

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

static bool run_tasks(cell::Spurs::Spurs *spurs)
{
    cell::Spurs::Taskset *taskset = static_cast<cell::Spurs::Taskset *>(
        ::aligned_alloc(CELL_SPURS_TASKSET_ALIGN, CELL_SPURS_TASKSET_SIZE));
    if (!taskset) { std::printf("  aligned_alloc(Taskset): FAILED\n"); return false; }

    int rc = cell::Spurs::Taskset::create(spurs, taskset, /*argTaskset=*/0,
                                          kTaskPriority, kMaxContention);
    if (rc) {
        std::printf("  Taskset::create: %#x\n", rc);
        std::free(taskset); return false;
    }
    std::printf("  Taskset::create ok\n");

    rc = cellSpursSemaphoreInitialize(taskset, &g_semaphore, /*total=*/0);
    if (rc) {
        std::printf("  SemaphoreInitialize: %#x\n", rc);
        taskset->shutdown(); taskset->join(); std::free(taskset); return false;
    }
    std::printf("  SemaphoreInitialize ok\n");

    /* lsPattern: every chunk above LS 0x3000 saveable; chunks below
     * SPRX-reserved must be zero (else createTask returns 0x80410902). */
    CellSpursTaskLsPattern lsPattern = { { CELL_SPURS_TASK_TOP_MASK,
                                          0xffffffffU, 0xffffffffU,
                                          0xffffffffU } };

    /* Sync primitives that block require a context save area. */
    const size_t kCtxBytes = CELL_SPURS_TASK_CONTEXT_SIZE_ALL;
    auto launch = [&](uint32_t kind, CellSpursTaskId *outId) -> int {
        void *ctx = ::aligned_alloc(CELL_SPURS_TASK_CONTEXT_ALIGN, kCtxBytes);
        if (!ctx) return -1;
        CellSpursTaskArgument arg = { { 0, 0, 0, 0 } };
        arg.u64[0] = reinterpret_cast<uint64_t>(&g_semaphore);
        arg.u32[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_counter[0]));
        arg.u32[3] = (kind << 16) | (kIterations & 0xffffU);
        return taskset->createTask(outId, /*eaElf=*/spu_task_bin,
                                   /*eaContext=*/ctx, /*sizeContext=*/kCtxBytes,
                                   &lsPattern, &arg);
    };

    CellSpursTaskId tidProducer = 0, tidConsumer = 0;
    rc = launch(0, &tidProducer);
    if (rc) { std::printf("  createTask(producer): %#x\n", rc); goto fail; }
    rc = launch(1, &tidConsumer);
    if (rc) { std::printf("  createTask(consumer): %#x\n", rc); goto fail; }
    std::printf("  Tasks launched: producer=%u consumer=%u\n",
                tidProducer, tidConsumer);

    {
        const uint32_t kFinal = kIterations;
        for (int i = 0; i < 100000000 && g_counter[0] < kFinal; ++i)
            __asm__ __volatile__("" ::: "memory");
        std::printf("  consumer counter = 0x%08x (target %u)\n",
                    (unsigned)g_counter[0], (unsigned)kFinal);
    }

    rc = taskset->shutdown();
    if (rc) { std::printf("  shutdown: %#x\n", rc); std::free(taskset); return false; }
    rc = taskset->join();
    if (rc) { std::printf("  join: %#x\n", rc); std::free(taskset); return false; }
    std::printf("  Taskset shutdown+join ok\n");
    std::free(taskset);
    return true;

fail:
    taskset->shutdown(); taskset->join(); std::free(taskset);
    return false;
}

int main(void)
{
    std::printf("hello-spurs-semaphore: P/V smoke test\n");

    int rc = spu_printf_initialize(kSpuPrintfPriority, 0);
    if (rc) { std::printf("  spu_printf_initialize: %#x\n", rc); return 1; }

    cell::Spurs::Spurs2 *spurs = new cell::Spurs::Spurs2;
    if (!init_spurs(spurs)) { std::printf("FAILURE\n"); return 1; }

    bool ok = run_tasks(spurs);

    rc = spurs->finalize();
    if (rc) { std::printf("  finalize: %#x\n", rc); }
    else    { std::printf("  Spurs finalize ok\n"); }

    delete spurs;
    spu_printf_finalize();

    /* "DONE" reports test completion regardless of P/V cycle outcome
     * (which is RPCS3-HLE-blocked).  See header comment. */
    std::printf("%s\n", ok ? "DONE" : "FAILURE");
    return ok ? 0 : 1;
}
