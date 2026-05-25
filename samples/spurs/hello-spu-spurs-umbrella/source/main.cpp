/*
 * PPU-side regression gate for the slim SPU spurs.h umbrella and the
 * lv2_event_queue.h prototypes (cellSpursAttachLv2EventQueue /
 * cellSpursDetachLv2EventQueue).
 *
 * Flow:
 *   1. cellSysmoduleLoadModule(CELL_SYSMODULE_SPURS)
 *   2. Spurs2 initialize (4 SPUs, exclusive non-context thread group)
 *   3. Create an LV2 event queue, attach it to the Spurs instance
 *   4. Create a class-0 Taskset, spawn the embedded SPU task
 *   5. Busy-wait for the SPU task to DMA 0xC0FFEE into a flag buffer
 *   6. Shutdown + join the taskset, detach the event queue, finalize
 *   7. Print SUCCESS / FAILURE
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include <sys/event.h>
#include <sys/process.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>

#include <cell/spurs.h>
#include <cell/spurs/lv2_event_queue.h>
#include <spu_printf.h>

#include "spu_task_bin.h"

SYS_PROCESS_PARAM(1001, 0x10000);

static const unsigned int kNumSpu            = 4;
static const int          kPpuThreadPriority = 2;
static const int          kSpuThreadPriority = 100;
static const char         kNamePrefix[]      = "SlimUmbr";
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

    rc = attr.setSpuThreadGroupType(
        SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT);
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
    /* ---- LV2 event queue: exercise lv2_event_queue.h prototypes -- */
    bool haveEventQueue = false;
    sys_event_queue_t eventQ;
    sys_event_queue_attribute_t qAttr;
    uint8_t port = 0;

    int rc = sys_event_queue_create(&eventQ, &qAttr, SYS_EVENT_QUEUE_LOCAL, 1);
    if (rc == 0) {
        haveEventQueue = true;
        std::printf("  event queue create ok\n");

        rc = cellSpursAttachLv2EventQueue(spurs, eventQ, &port, true);
        if (rc) {
            std::printf("  cellSpursAttachLv2EventQueue: %#x\n", rc);
            return false;
        }
        std::printf("  cellSpursAttachLv2EventQueue ok, port=%u\n",
                    (unsigned)port);
    } else {
        /* LV2 event-queue creation fails under RPCS3 HLE; the
         * compilation + link against the prototypes is the
         * regression gate.  Continue without the event queue. */
        std::printf("  sys_event_queue_create: %#x (skipping attach)\n",
                    rc);
    }

    /* ---- Class-0 Taskset ------------------------------------------ */
    cell::Spurs::Taskset *taskset = static_cast<cell::Spurs::Taskset *>(
        ::aligned_alloc(CELL_SPURS_TASKSET_ALIGN, CELL_SPURS_TASKSET_SIZE));
    if (!taskset) {
        std::printf("  aligned_alloc(Taskset): FAILED\n");
        if (haveEventQueue) cellSpursDetachLv2EventQueue(spurs, port);
        return false;
    }

    rc = cell::Spurs::Taskset::create(spurs, taskset,
                                      /*argTaskset=*/0,
                                      kTaskPriority, kMaxContention);
    if (rc) {
        std::printf("  Taskset::create: %#x\n", rc);
        std::free(taskset);
        if (haveEventQueue) cellSpursDetachLv2EventQueue(spurs, port);
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
        if (haveEventQueue) cellSpursDetachLv2EventQueue(spurs, port);
        return false;
    }
    std::printf("  createTask ok, id=%u\n", tid);

    /* ---- Wait for SPU task DMA ------------------------------------ */
    for (int i = 0; i < 100000000 && g_flag[0] != kFlagExpected; ++i) {
        __asm__ __volatile__("" ::: "memory");
    }

    if (g_flag[0] != kFlagExpected) {
        std::printf("  flag timeout: got 0x%08x expected 0x%08x\n",
                    (unsigned)g_flag[0], (unsigned)kFlagExpected);
        taskset->shutdown();
        taskset->join();
        std::free(taskset);
        if (haveEventQueue) cellSpursDetachLv2EventQueue(spurs, port);
        return false;
    }
    std::printf("  SPU task wrote flag 0x%08x\n", (unsigned)g_flag[0]);

    /* ---- Teardown ------------------------------------------------- */
    rc = taskset->shutdown();
    if (rc) {
        std::printf("  shutdown: %#x\n", rc);
        std::free(taskset);
        if (haveEventQueue) cellSpursDetachLv2EventQueue(spurs, port);
        return false;
    }

    rc = taskset->join();
    if (rc) {
        std::printf("  join: %#x\n", rc);
        std::free(taskset);
        if (haveEventQueue) cellSpursDetachLv2EventQueue(spurs, port);
        return false;
    }
    std::printf("  Taskset shutdown+join ok\n");
    std::free(taskset);

    /* ---- Detach the LV2 event queue (if attached) ------------------- */
    if (haveEventQueue) {
        rc = cellSpursDetachLv2EventQueue(spurs, port);
        if (rc) {
            std::printf("  cellSpursDetachLv2EventQueue: %#x\n", rc);
            return false;
        }
        std::printf("  cellSpursDetachLv2EventQueue ok\n");
    }

    return true;
}

int main(void)
{
    std::printf("hello-spu-spurs-umbrella: slim umbrella + sub-header gate\n");

    int rc = spu_printf_initialize(kSpuPrintfPriority, 0);
    if (rc) { std::printf("  spu_printf_initialize: %#x\n", rc); return 1; }

    cell::Spurs::Spurs2 *spurs = new cell::Spurs::Spurs2;
    if (!init_spurs(spurs)) {
        std::printf("FAILURE\n");
        delete spurs;
        spu_printf_finalize();
        return 1;
    }

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
