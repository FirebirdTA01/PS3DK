/* spurs-task-cpp-statics (row spurs-task-cpp-statics): a C++ SPURS task's
 * static objects are constructed before its main runs, in declaration
 * order.  Checks the values the task reports. */
#include "harness.h"
#include "../task_statics.h"
#include SUITE_SPU_HEADER

alignas(16) static volatile ts_box s_box;

static int row_main()
{
    suite::watchdog(20);
    auto *spurs = new cell::Spurs::Spurs2;
    int rc = suite::spurs_up(spurs, "SuiteCtor");
    if (rc) return suite::invalid("spurs", rc);
    cell::Spurs::Taskset *ts = suite::taskset_up(spurs, &rc);
    if (!ts) return suite::invalid("taskset", rc);

    suite::activity("C++ SPU task with static objects");
    if ((rc = suite::launch(ts, SUITE_SPU_BIN, reinterpret_cast<uintptr_t>(&s_box), 0, 0)))
        return suite::invalid("launch", rc);
    int result = 0;
    if (!suite::wait_for([] { return s_box.state == 2; })) result = suite::fail("task done", s_box.state, 2);
    else if (s_box.value != 3 * TS_SEED) result = suite::fail("static constructed", s_box.value, 3 * TS_SEED);
    else if (s_box.marks != 2) result = suite::fail("constructors run", s_box.marks, 2);
    else if (s_box.order != 0x0102) result = suite::fail("declaration order", s_box.order, 0x0102);
    suite::taskset_down(ts);
    spurs->finalize();
    return result ? result : suite::ok();
}

SUITE_ENTRY_POINT(row_main)
