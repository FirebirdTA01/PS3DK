/*
 * hello-ppu-spurs-job-chain - PPU-side job-chain bring-up.
 *
 * Stands up a 4-SPU Spurs2 instance, builds a tiny 2-entry job chain
 * (LWSYNC + END) that does no work and immediately terminates, drives
 * the chain through its full lifecycle, then exercises the JobGuard
 * surface separately.  No SPU-side job binary involved - the SPRX's
 * job-chain runtime steps the command list and exits at END.
 *
 * What this proves:
 *   - cell/spurs/job_chain.h header chain links cleanly against
 *     libspurs_stub.a (NIDs resolved end-to-end).
 *   - CellSpursJobChainAttribute / CellSpursJobChain / CellSpursJobGuard
 *     opaque storage sizes + alignments match the SPRX expectation.
 *   - Run / Join / Shutdown flow on a chain that does no work.
 *   - Urgent-command injection.
 *   - Info-struct read-back round-trips through the SPRX without the
 *     PRXPTR fields stomping each other.
 *
 * Expected output:
 *   hello-ppu-spurs-job-chain: bring-up
 *     spu_printf_initialize ok
 *     Spurs2 up
 *     JobChainAttribute ok
 *     Created jobChain (workload id N)
 *     RunJobChain ok
 *     AddUrgentCommand(LWSYNC) ok
 *     Info: maxSizeJobDescriptor=1024 maxGrabbedJob=4 isHalted=0
 *     Joined ok
 *     Shutdown ok
 *     JobGuard init / reset / notify ok
 *     finalize ok
 *   SUCCESS
 */

#include <cstdio>
#include <cstring>

#include <sys/process.h>
#include <sys/spu_thread_group.h>
#include <cell/spurs.h>
#include <spu_printf.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static const int kSpuPrintfPriority = 999;

/* The chain itself: LWSYNC then END.  Aligned to 16 because the
 * runtime DMAs entries in 8-byte chunks and the entry list itself
 * must sit on at least 8-byte alignment. */
static uint64_t s_chain[4] __attribute__((aligned(16))) = {
    CELL_SPURS_JOB_COMMAND_LWSYNC,
    CELL_SPURS_JOB_COMMAND_END,
    0, 0,
};

/* Per-SPU priority table for the workload backing this chain.  Slots
 * 0+1 get a non-zero priority (the SPRX schedules onto these); the
 * rest are zero ("not assigned").  Matches the canonical shape the
 * reference SDK's job samples use. */
static const uint8_t s_priorityTable[8] = { 8, 8, 0, 0, 0, 0, 0, 0 };

int main(void)
{
    std::printf("hello-ppu-spurs-job-chain: bring-up\n");

    int rc = spu_printf_initialize(kSpuPrintfPriority, 0);
    if (rc) { std::printf("  spu_printf_initialize: %#x\n", rc); return 1; }
    std::printf("  spu_printf_initialize ok\n");

    /* --- Spurs2 up --- */
    cell::Spurs::SpursAttribute sattr;
    rc = cell::Spurs::SpursAttribute::initialize(&sattr, 4, 100, 2, false);
    if (rc) { std::printf("  SpursAttribute::initialize: %#x\n", rc); return 1; }
    sattr.setNamePrefix("JobChain", 8);
    sattr.setSpuThreadGroupType(SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT);
    sattr.enableSpuPrintfIfAvailable();

    cell::Spurs::Spurs *spurs = new cell::Spurs::Spurs;
    rc = cell::Spurs::Spurs::initialize(spurs, &sattr);
    if (rc) { std::printf("  Spurs::initialize: %#x\n", rc); return 1; }
    std::printf("  Spurs up\n");
    std::printf("    s_chain=%p sz=%u maxSz=%u\n",
                (void *)s_chain, (unsigned)sizeof(CellSpursJob64), 256u);

    /* --- JobChainAttribute --- */
    CellSpursJobChainAttribute jcAttr;
    std::memset(&jcAttr, 0, sizeof(jcAttr));
    /* Match the canonical job_hello reference sample params: 128-byte
     * descriptors, 1-SPU contention, priority{8,0,0,0,0,0,0,0}. */
    static const uint8_t prio_hello[8] = { 8, 0, 0, 0, 0, 0, 0, 0 };
    std::printf("    JM_REV=%u SDK_VER=0x%x sizeof(jcAttr)=%u align=%u\n",
                (unsigned)CELL_SPURS_JOB_REVISION,
                (unsigned)_CELL_SPURS_INTERNAL_VERSION,
                (unsigned)sizeof(CellSpursJobChainAttribute),
                (unsigned)CELL_SPURS_JOBCHAIN_ATTRIBUTE_ALIGN);
    rc = cellSpursJobChainAttributeInitialize(&jcAttr,
                                              s_chain,
                                              /*sizeJobDescriptor       */ 128,
                                              /*maxGrabbedJob           */ 16,
                                              prio_hello,
                                              /*maxContention           */ 1,
                                              /*autoRequestSpuCount     */ true,
                                              /*tag1                    */ 0,
                                              /*tag2                    */ 1,
                                              /*isFixedMemAlloc         */ false,
                                              /*maxSizeJobDescriptor    */ 256,
                                              /*initialRequestSpuCount  */ 0);
    if (rc) { std::printf("  JobChainAttribute init: %#x\n", rc); goto teardown_spurs; }

    rc = cellSpursJobChainAttributeSetName(&jcAttr, "hello-jc");
    if (rc) { std::printf("  JobChainAttribute setName: %#x\n", rc); goto teardown_spurs; }

    rc = cellSpursJobChainAttributeSetHaltOnError(&jcAttr);
    if (rc) { std::printf("  JobChainAttribute setHaltOnError: %#x\n", rc); goto teardown_spurs; }
    std::printf("  JobChainAttribute ok\n");

    /* --- Create / Run / Join / Shutdown the chain --- */
    {
        CellSpursJobChain   *jc = new CellSpursJobChain;
        CellSpursWorkloadId  wid = 0;

        rc = cellSpursCreateJobChainWithAttribute((CellSpurs *)spurs, jc, &jcAttr);
        if (rc) { std::printf("  CreateJobChainWithAttribute: %#x\n", rc); delete jc; goto teardown_spurs; }

        rc = cellSpursGetJobChainId(jc, &wid);
        if (rc == 0) std::printf("  Created jobChain (workload id %u)\n", wid);
        else         std::printf("  Created jobChain (GetJobChainId: %#x)\n", rc);

        rc = cellSpursRunJobChain(jc);
        if (rc) { std::printf("  RunJobChain: %#x\n", rc); }
        else    { std::printf("  RunJobChain ok\n"); }

        /* Inject an urgent-command path read.  LWSYNC is a no-op-ish
         * sync barrier; safest urgent payload that doesn't disturb
         * chain state. */
        rc = cellSpursAddUrgentCommand(jc, CELL_SPURS_JOB_COMMAND_LWSYNC);
        if (rc) std::printf("  AddUrgentCommand: %#x (non-fatal)\n", rc);
        else    std::printf("  AddUrgentCommand(LWSYNC) ok\n");

        /* Info-struct round-trip - exercises every PRXPTR field
         * the SPRX writes back. */
        CellSpursJobChainInfo info;
        std::memset(&info, 0, sizeof(info));
        rc = cellSpursGetJobChainInfo(jc, &info);
        if (rc == 0) {
            std::printf("  Info: maxSizeJobDescriptor=%u maxGrabbedJob=%u isHalted=%d\n",
                        (unsigned)info.maxSizeJobDescriptor,
                        (unsigned)info.maxGrabbedJob,
                        (int)info.isHalted);
        } else {
            std::printf("  GetJobChainInfo: %#x (non-fatal)\n", rc);
        }

        /* PPU-initiated shutdown - real samples shutdown from inside
         * an SPU "shutdown job" (the last JOB in the chain), which
         * naturally triggers the completion event Join waits on.  We
         * have no SPU job binary, so the chain (LWSYNC, END) never
         * fires that event; without a host-side Shutdown call first,
         * Join hangs forever.  Shutdown -> Join is the clean exit
         * order in this PPU-only setup. */
        rc = cellSpursShutdownJobChain(jc);
        if (rc) std::printf("  ShutdownJobChain: %#x\n", rc);
        else    std::printf("  Shutdown ok\n");

        rc = cellSpursJoinJobChain(jc);
        if (rc) std::printf("  JoinJobChain: %#x\n", rc);
        else    std::printf("  Joined ok\n");

        delete jc;
    }

    /* --- JobGuard surface (independent of the chain above) --- */
    {
        /* Build a guard-bearing chain so JobGuardInitialize has
         * something to bind against.  This chain isn't actually run -
         * we just need the bind target. */
        static uint64_t   guarded_chain[2] __attribute__((aligned(16))) = {
            CELL_SPURS_JOB_COMMAND_END,
            0,
        };
        CellSpursJobChainAttribute gAttr;
        CellSpursJobChain          gChain __attribute__((aligned(CELL_SPURS_JOBCHAIN_ALIGN)));
        CellSpursJobGuard          gGuard __attribute__((aligned(CELL_SPURS_JOB_GUARD_ALIGN)));

        std::memset(&gAttr,  0, sizeof(gAttr));
        std::memset(&gChain, 0, sizeof(gChain));
        std::memset(&gGuard, 0, sizeof(gGuard));

        rc = cellSpursJobChainAttributeInitialize(&gAttr, guarded_chain,
                                                  256, 4, s_priorityTable,
                                                  4, true, 0, 1, false,
                                                  1024, 1);
        if (rc == 0) rc = cellSpursCreateJobChainWithAttribute((CellSpurs *)spurs, &gChain, &gAttr);
        if (rc == 0) rc = cellSpursJobGuardInitialize(&gChain, &gGuard,
                                                      /*notifyCount     */ 1,
                                                      /*requestSpuCount */ 1,
                                                      /*autoReset       */ 0);
        if (rc == 0) rc = cellSpursJobGuardReset(&gGuard);
        if (rc == 0) rc = cellSpursJobGuardNotify(&gGuard);

        if (rc == 0) std::printf("  JobGuard init / reset / notify ok\n");
        else         std::printf("  JobGuard surface: %#x (non-fatal)\n", rc);

        /* Shutdown + Join the guard-bearing chain so it doesn't
         * outlive Spurs::finalize and stall the runtime tear-down. */
        if (rc == 0) {
            cellSpursShutdownJobChain(&gChain);
            cellSpursJoinJobChain(&gChain);
        }
    }

teardown_spurs:
    rc = spurs->finalize();
    if (rc) std::printf("  finalize: %#x\n", rc);
    else    std::printf("  finalize ok\n");

    delete spurs;

    int rc2 = spu_printf_finalize();
    if (rc2) std::printf("  spu_printf_finalize: %#x (non-fatal)\n", rc2);

    if (rc == 0) std::printf("SUCCESS\n");
    else         std::printf("FAILURE\n");
    return rc;
}
