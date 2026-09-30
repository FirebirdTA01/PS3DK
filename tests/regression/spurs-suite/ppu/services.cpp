/* spurs-suite SPU services (row spurs-services): one task runs the SPU
 * service calls against the live instance (services.h, spu/services.c).
 * The PPU signals it out of WaitSignal2, then checks what it changed in
 * the CellSpurs instance (max contention, priority) and finds its user
 * packets in the trace buffer (one from cellSpursPutTrace, one from
 * cellSpursPutUserTrace), with headers stamped for its SPU and workload. */
#include "harness.h"
#include <cell/spurs/trace.h>
#include "../services.h"
#include SUITE_SPU_HEADER

alignas(128) static volatile result_slot g_result[S_SLOTS];
alignas(128) static s_report s_block;
alignas(128) static uint8_t s_semaphore[128];
alignas(128) static uint8_t s_trace[64 * 1024];

static int row_main()
{
    suite::watch_slots(g_result, S_SLOTS);
    suite::watchdog(30);
    std::memset((void *)g_result, 0, sizeof g_result);
    std::memset(&s_block, 0, sizeof s_block);
    s_block.semaphore = reinterpret_cast<uintptr_t>(s_semaphore);
    auto *spurs = new cell::Spurs::Spurs2;
    int rc = suite::spurs_up(spurs, "SuiteSvc");
    if (rc) return suite::invalid("spurs", rc);
    CellSpurs *cs = reinterpret_cast<CellSpurs *>(spurs);
    const volatile uint8_t *inst = reinterpret_cast<const volatile uint8_t *>(cs);

    suite::activity("starting a SPURS trace");
    if ((rc = cellSpursTraceInitialize(cs, s_trace, sizeof s_trace, CELL_SPURS_TRACE_MODE_FLAG_WRAP_BUFFER)))
        return suite::invalid("trace initialize", rc);
    if ((rc = cellSpursTraceStart(cs))) return suite::invalid("trace start", rc);

    cell::Spurs::Taskset *ts = suite::taskset_up(spurs, &rc);
    if (!ts) return suite::invalid("taskset", rc);
    CellSpursTaskId id;
    if ((rc = suite::launch(ts, SUITE_SPU_BIN, reinterpret_cast<uintptr_t>(g_result),
                            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s_block)), S_TASK, &id)))
        return suite::invalid("launch", rc);

    suite::activity("waiting for the task to block in WaitSignal2");
    if (!suite::wait_for([&] { __sync_synchronize(); return s_block.waiting == 1 || suite::slot_done(g_result[S_TASK], S_TASK); }))
        return suite::fail("task reached WaitSignal2", s_block.waiting, 1);
    sys_timer_usleep(20000);
    if ((rc = ts->sendSignal(id))) return suite::fail("send signal", rc, 0);

    suite::activity("waiting for the service steps");
    if (!suite::wait_for([&] { return suite::slot_done(g_result[S_TASK], S_TASK); }))
        return suite::fail("task report", g_result[S_TASK].magic, RESULT_MAGIC | S_TASK);
    if (g_result[S_TASK].status) {
        std::printf("services: step %u got %#x want %#x\n", g_result[S_TASK].status,
                    g_result[S_TASK].value, g_result[S_TASK].extra);
        return suite::fail("service step", g_result[S_TASK].status, 0);
    }

    /* what the task changed in the instance */
    const unsigned wid = s_block.wid;
    const unsigned contention = wid < 16 ? inst[0x50 + wid] & 15 : inst[0x50 + (wid & 15)] >> 4;
    if (contention != S_CONTENTION)
        return suite::fail("max contention in the instance", contention, S_CONTENTION);
    const unsigned priority = inst[(wid < 16 ? 0xb00 : 0x1000) + (wid & 15) * 0x20 + 0x18];
    if (priority != S_PRIORITY)
        return suite::fail("SPU 0 priority in the instance", priority, S_PRIORITY);
    const unsigned wantIdle = (inst[0x74] & 0x40) ? 0x8041080Fu : 0u;
    if (s_block.idleSpuRc != wantIdle)
        return suite::fail("request idle SPU", s_block.idleSpuRc, wantIdle);

    suite::activity("stopping the trace, looking for the task's packet");
    suite::taskset_down(ts);
    if ((rc = cellSpursTraceStop(cs))) return suite::fail("trace stop", rc, 0);
    /* one packet from cellSpursPutTrace (kind 1), one from
       cellSpursPutUserTrace (kind 2); each header must carry length 2, the
       SPU and workload the task reported in the payload, and a time */
    unsigned found = 0, bad = 0;
    for (size_t off = sizeof(CellSpursTraceInfo); off + 16 <= sizeof s_trace; off += 16) {
        const CellSpursTracePacket *p = reinterpret_cast<const CellSpursTracePacket *>(s_trace + off);
        const uint64_t u = p->data.user;
        if (p->header.tag != CELL_SPURS_TRACE_TAG_USER || static_cast<uint32_t>(u >> 32) != S_TRACE_MAGIC)
            continue;
        const unsigned kind = (u >> 16) & 0xff, spu = (u >> 8) & 0xff, wid = u & 0xff;
        if (kind == 1 || kind == 2)
            found |= 1u << kind;
        const CellSpursTraceHeader &h = p->header;
        if (h.length != 2 || h.spu != spu || h.workload != wid || h.time == 0)
            bad = kind;
    }
    cellSpursTraceFinalize(cs);
    if (found != 6)
        return suite::fail("user trace packets in the buffer (bit1 PutTrace, bit2 PutUserTrace)", found, 6);
    if (bad)
        return suite::fail("trace packet header (length, SPU, workload, time) of kind", bad, 0);

    spurs->finalize();
    return suite::ok();
}

SUITE_ENTRY_POINT(row_main)
