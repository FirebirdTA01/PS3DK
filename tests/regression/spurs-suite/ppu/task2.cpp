/* spurs-suite Task2 (row spurs-task2): a Task2 parent task on a Taskset2
 * creates Task2 children from the SPU (plain, with a context and a name,
 * from a binary info block) and joins them, checks the argument errors,
 * and runs a child through an exit-code container.  The PPU then joins
 * the parent and must get its exit code, and itself creates a task with
 * an exit-code container and collects the code (Get, then TryGet finds it
 * consumed). */
#include "harness.h"
#include "../task2.h"
#include SUITE_SPU_HEADER

alignas(128) static volatile result_slot g_result[T2_SLOTS];
alignas(128) static t2_params s_params;
alignas(128) static CellSpursTaskExitCode s_exitCode;
alignas(128) static CellSpursTaskExitCode s_ppuExitCode;   /* collected by the PPU */
alignas(16) static CellSpursTaskBinInfo s_binInfo;

static int row_main()
{
    suite::watch_slots(g_result, T2_SLOTS);
    suite::watchdog(40);
    std::memset((void *)g_result, 0, sizeof g_result);
    auto *spurs = new cell::Spurs::Spurs2;
    int rc = suite::spurs_up(spurs, "SuiteT2");
    if (rc) return suite::invalid("spurs", rc);

    suite::activity("creating a Taskset2");
    auto *ts2 = static_cast<cell::Spurs::Taskset2 *>(::aligned_alloc(CELL_SPURS_TASKSET2_ALIGN, CELL_SPURS_TASKSET2_SIZE));
    CellSpursTasksetAttribute2 tsAttr;
    cell::Spurs::TasksetAttribute2::initialize(&tsAttr);
    if ((rc = cell::Spurs::Taskset2::create(spurs, ts2, &tsAttr))) return suite::invalid("taskset2", rc);
    cell::Spurs::Taskset *plain = suite::taskset_up(spurs, &rc);
    if (!plain) return suite::invalid("taskset", rc);

    std::memset(&s_params, 0, sizeof s_params);
    std::memset(&s_binInfo, 0, sizeof s_binInfo);
    s_binInfo.eaElf = reinterpret_cast<uintptr_t>(SUITE_SPU_BIN);
    s_params.elf = reinterpret_cast<uintptr_t>(SUITE_SPU_BIN);
    s_params.plainTaskset = reinterpret_cast<uintptr_t>(plain);
    s_params.binInfo = reinterpret_cast<uintptr_t>(&s_binInfo);
    s_params.exitCode = reinterpret_cast<uintptr_t>(&s_exitCode);
    s_params.context = reinterpret_cast<uintptr_t>(::aligned_alloc(CELL_SPURS_TASK_CONTEXT_ALIGN, CELL_SPURS_TASK_CONTEXT_SIZE_ALL));

    CellSpursTaskAttribute2 attr;
    cell::Spurs::TaskAttribute2::initialize(&attr);
    attr.sizeContext = CELL_SPURS_TASK_CONTEXT_SIZE_ALL;
    attr.eaContext = reinterpret_cast<uintptr_t>(::aligned_alloc(CELL_SPURS_TASK_CONTEXT_ALIGN, CELL_SPURS_TASK_CONTEXT_SIZE_ALL));
    attr.lsPattern = { { CELL_SPURS_TASK_TOP_MASK, 0xffffffffU, 0xffffffffU, 0xffffffffU } };
    attr.name = "suite-t2-parent";
    CellSpursTaskArgument arg;
    std::memset(&arg, 0, sizeof arg);
    arg.u64[0] = reinterpret_cast<uintptr_t>(g_result);
    arg.u32[2] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_params));
    arg.u32[3] = T2_PARENT;
    CellSpursTaskId id;
    suite::activity("launching the Task2 parent");
    if ((rc = ts2->createTask2(&id, SUITE_SPU_BIN, &arg, &attr))) return suite::invalid("create parent", rc);

    suite::activity("joining the parent (task %u) from the PPU", (unsigned)id);
    int code = 0;
    if ((rc = ts2->joinTask2(id, &code))) return suite::fail("PPU join of the parent", rc, 0);
    if (!suite::slot_done(g_result[T2_PARENT], T2_PARENT))
        return suite::fail("parent report", g_result[T2_PARENT].magic, RESULT_MAGIC | T2_PARENT);
    if (g_result[T2_PARENT].status) {
        std::printf("parent: step %u got %#x want %#x\n", g_result[T2_PARENT].status,
                    g_result[T2_PARENT].value, g_result[T2_PARENT].extra);
        return suite::fail("parent step", g_result[T2_PARENT].status, 0);
    }
    if (code != T2_PARENT_CODE)
        return suite::fail("parent exit code", static_cast<unsigned>(code), T2_PARENT_CODE);

    /* a task the PPU creates with an exit-code container, whose code the
       PPU collects: Get waits for it, a second TryGet finds it consumed */
    suite::activity("PPU exit-code container: create, Get, TryGet");
    if ((rc = cellSpursTaskExitCodeInitialize(&s_ppuExitCode))) return suite::fail("PPU exit code initialize", rc, 0);
    if ((rc = cellSpursTaskExitCodeTryGet(&s_ppuExitCode, &code)) != static_cast<int>(CELL_SPURS_TASK_ERROR_STAT))
        return suite::fail("PPU TryGet with no task attached", rc, CELL_SPURS_TASK_ERROR_STAT);
    {
        CellSpursTaskArgument carg;
        std::memset(&carg, 0, sizeof carg);
        carg.u64[0] = reinterpret_cast<uintptr_t>(g_result);
        carg.u32[2] = T2_PPU_CHILD_CODE;
        carg.u32[3] = T2_CHILD;
        CellSpursTaskAttribute tattr;
        if ((rc = cellSpursTaskAttributeInitialize(&tattr, SUITE_SPU_BIN, nullptr, &carg)))
            return suite::fail("PPU task attribute", rc, 0);
        if ((rc = cellSpursTaskAttributeSetExitCodeContainer(&tattr, &s_ppuExitCode)))
            return suite::fail("PPU set exit-code container", rc, 0);
        CellSpursTaskId cid;
        if ((rc = cellSpursCreateTaskWithAttribute(reinterpret_cast<CellSpursTaskset *>(plain), &cid, &tattr)))
            return suite::fail("PPU create task with exit code", rc, 0);
        code = 0;
        if ((rc = cellSpursTaskExitCodeGet(&s_ppuExitCode, &code))) return suite::fail("PPU exit code Get", rc, 0);
        if (code != static_cast<int>(T2_PPU_CHILD_CODE))
            return suite::fail("PPU collected exit code", static_cast<unsigned>(code), T2_PPU_CHILD_CODE);
        if ((rc = cellSpursTaskExitCodeTryGet(&s_ppuExitCode, &code)) != static_cast<int>(CELL_SPURS_TASK_ERROR_STAT))
            return suite::fail("PPU TryGet after Get (consumed)", rc, CELL_SPURS_TASK_ERROR_STAT);
    }

    suite::activity("shutting down the tasksets");
    suite::taskset_down(plain);
    ts2->shutdown();
    ts2->join();
    spurs->finalize();
    return suite::ok();
}

SUITE_ENTRY_POINT(row_main)
