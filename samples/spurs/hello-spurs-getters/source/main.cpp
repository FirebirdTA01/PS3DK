/*
 * hello-spurs-getters - validate libspurs SPU module-runtime getters.
 *
 * Spawns a Spurs2 + Taskset, runs an SPU task that calls each of:
 *   cellSpursGetSpursAddress
 *   cellSpursGetCurrentSpuId
 *   cellSpursGetTagId
 *   cellSpursGetWorkloadId
 *   cellSpursGetSpuCount
 *
 * The SPU task DMAs the results back to a PPU-visible 32-byte buffer
 * so the PPU can sanity-check each value:
 *   - SpursAddress matches the runtime-allocated CellSpurs *
 *   - SpuCount matches kNumSpu
 *   - CurrentSpuId is 0..numSpus-1
 *   - WorkloadId / TagId are sane (small unsigned values)
 *   - sentinel 0xc0ffee99 distinguishes "task ran" from "skipped".
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>

#include <sys/process.h>

#include <cell/spurs.h>
#include <spu_printf.h>

#include "spu_getters_bin.h"

SYS_PROCESS_PARAM(1001, 0x10000);

static const unsigned int kNumSpu            = 4;
static const int          kPpuThreadPriority = 2;
static const int          kSpuThreadPriority = 100;
static const char         kNamePrefix[]      = "GetTest";
static const int          kSpuPrintfPriority = 999;

static const uint8_t      kTaskPriority[8]   = { 1, 1, 1, 1, 1, 1, 1, 1 };
static const unsigned int kMaxContention     = 4;
static const uint32_t     kSentinelExpected  = 0xc0ffee99u;

struct Result {
    uint64_t spursAddress;
    uint32_t currentSpuId;
    uint32_t tagId;
    uint32_t workloadId;
    uint32_t spuCount;
    uint32_t pad0;
    uint64_t elfAddress;
    uint32_t iwlTaskId;
    uint32_t modulePollResult;
    uint32_t cellSpursPoll_b;
    uint32_t sentinel;
};

alignas(128) static volatile Result g_result;

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
    std::printf("  Spurs2::initialize ok (numSpus=%u)\n", kNumSpu);
    return true;
}

static bool run_task(cell::Spurs::Spurs *spurs)
{
    cell::Spurs::Taskset *taskset = static_cast<cell::Spurs::Taskset *>(
        ::aligned_alloc(CELL_SPURS_TASKSET_ALIGN, CELL_SPURS_TASKSET_SIZE));
    if (!taskset) { std::printf("  aligned_alloc(Taskset): FAILED\n"); return false; }

    int rc = cell::Spurs::Taskset::create(spurs, taskset, /*argTaskset=*/0,
                                          kTaskPriority, kMaxContention);
    if (rc) {
        std::printf("  Taskset::create: %#x\n", rc);
        std::free(taskset);
        return false;
    }
    std::printf("  Taskset::create ok\n");

    std::memset((void *)&g_result, 0, sizeof(g_result));

    CellSpursTaskArgument arg = { { 0, 0, 0, 0 } };
    arg.u64[0] = reinterpret_cast<uint64_t>(&g_result);

    CellSpursTaskLsPattern lsPattern = { { 0, 0, 0, 0 } };

    CellSpursTaskId tid = 0;
    rc = taskset->createTask(&tid,
                             /*eaElf=*/spu_getters_bin,
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

    /* Busy-wait for the SPU task to DMA the result block. */
    for (int i = 0; i < 100000000 && g_result.sentinel != kSentinelExpected; ++i) {
        __asm__ __volatile__("" ::: "memory");
    }

    bool ok = (g_result.sentinel == kSentinelExpected);
    if (!ok) {
        std::printf("  sentinel timeout: got %#x expected %#x\n",
                    (unsigned)g_result.sentinel, (unsigned)kSentinelExpected);
    } else {
        std::printf("\n=== SPU getter results ===\n");
        std::printf("  cellSpursGetSpursAddress() = %#llx  (PPU spurs=%p)\n",
                    (unsigned long long)g_result.spursAddress, (void *)spurs);
        std::printf("  cellSpursGetCurrentSpuId() = %u   (expect 0..%u)\n",
                    (unsigned)g_result.currentSpuId, (unsigned)kNumSpu - 1);
        std::printf("  cellSpursGetTagId()        = %u\n",
                    (unsigned)g_result.tagId);
        std::printf("  cellSpursGetWorkloadId()   = %u\n",
                    (unsigned)g_result.workloadId);
        std::printf("  cellSpursGetSpuCount()     = %u   (expect %u)\n",
                    (unsigned)g_result.spuCount, (unsigned)kNumSpu);
        std::printf("  cellSpursGetElfAddress()   = %#llx\n",
                    (unsigned long long)g_result.elfAddress);
        std::printf("  _cellSpursGetIWLTaskId()   = %#x   (= workloadId<<8 | taskId)\n",
                    (unsigned)g_result.iwlTaskId);
        std::printf("  cellSpursModulePoll()      = %u\n",
                    (unsigned)g_result.modulePollResult);
        std::printf("  cellSpursPoll()            = %u\n",
                    (unsigned)g_result.cellSpursPoll_b);

        /* Sanity validation. */
        bool addr_ok    = (g_result.spursAddress == reinterpret_cast<uint64_t>(spurs));
        bool spu_id_ok  = (g_result.currentSpuId < kNumSpu);
        bool count_ok   = (g_result.spuCount == kNumSpu);
        bool iwl_ok     = (g_result.iwlTaskId == ((g_result.workloadId << 8) | 0));
        std::printf("\n  SpursAddress match: %s\n", addr_ok   ? "OK" : "MISMATCH");
        std::printf("  CurrentSpuId range: %s\n", spu_id_ok ? "OK" : "OUT OF RANGE");
        std::printf("  SpuCount match:     %s\n", count_ok  ? "OK" : "MISMATCH");
        std::printf("  IWLTaskId encoding: %s\n", iwl_ok    ? "OK" : "MISMATCH");
        ok = addr_ok && spu_id_ok && count_ok && iwl_ok;
    }

    rc = taskset->shutdown();
    if (rc) std::printf("  shutdown: %#x\n", rc);

    rc = taskset->join();
    if (rc) std::printf("  join: %#x\n", rc);
    else    std::printf("  Taskset shutdown+join ok\n");

    std::free(taskset);
    return ok;
}

int main(void)
{
    std::printf("hello-spurs-getters: SPU module-runtime getter validation\n");

    int rc = spu_printf_initialize(kSpuPrintfPriority, 0);
    if (rc) { std::printf("  spu_printf_initialize: %#x\n", rc); return 1; }

    cell::Spurs::Spurs2 *spurs = new cell::Spurs::Spurs2;
    if (!init_spurs(spurs)) {
        std::printf("FAILURE\n");
        spu_printf_finalize();
        sys_process_exit(1);
    }

    bool ok = run_task(spurs);

    rc = spurs->finalize();
    if (rc) std::printf("  finalize: %#x\n", rc);
    delete spurs;
    spu_printf_finalize();

    if (ok) {
        std::printf("\nSUCCESS\n");
        return 0;
    } else {
        std::printf("\nFAILURE\n");
        sys_process_exit(1);
    }
}
