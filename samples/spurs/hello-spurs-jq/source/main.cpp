/*
 * hello-spurs-jq - SPURS job-queue end-to-end runtime test.
 *
 * Pushes N jobs through a Port2.  Each job carries a per-slot
 * sentinel EA + a per-slot magic value in userData[].  The SPU
 * worker (spu/source/main.c) DMA-PUTs the magic to the sentinel
 * EA.  PPU side polls until every slot's magic shows up, then
 * tears down.
 *
 * On JQ halt or sentinel timeout we sys_process_exit() so the
 * emulator shuts down without the user having to PS-button out.
 */

#include <cstdio>
#include <cstring>

#include <sys/process.h>
#include <cell/spurs.h>
#include <cell/spurs/job_queue.h>
#include <cell/spurs/job_queue_port2.h>
#include <cell/sysmodule.h>
#include <spu_printf.h>

#include "hello_spurs_jq_bin.h"
#include "hello_spurs_jq_jobheader_bin.h"

SYS_PROCESS_PARAM(1001, 0x10000);

static const int      kSpuPrintfPriority = 999;
static const uint32_t kMagicBase         = 0xC0FFEE00u;  /* + slot id */
static const unsigned kNumJobs           = 6;

#define _JQ_DEPTH 16
static CellSpursJobQueue       s_jq    __attribute__((aligned(128)));
static CellSpursJobQueuePort2  s_port  __attribute__((aligned(128)));
static CellSpursJob128         s_jobs[kNumJobs] __attribute__((aligned(128)));
static uint64_t                s_chain[CELL_SPURS_JOBQUEUE_SIZE_COMMAND_BUFFER(_JQ_DEPTH) / sizeof(uint64_t)]
    __attribute__((aligned(CELL_SPURS_JOBQUEUE_COMMAND_BUFFER_ALIGN)));
/* Per-job sentinel slots: each slot is 16 bytes (one DMA quad).  SPU
 * job writes [magic, jobIdx, eaBinary_lo, eaBinary_hi] to its slot. */
static volatile uint32_t       s_out[kNumJobs * 4] __attribute__((aligned(128))) = { 0 };
static const uint8_t           s_priority[8] = { 8, 0, 0, 0, 0, 0, 0, 0 };

#define _JQ_POOL_NUM_JOB128  (kNumJobs + 2)
static uint8_t s_jqPool[
    CELL_SPURS_JOBQUEUE_JOB_DESCRIPTOR_POOL_SIZE(0, _JQ_POOL_NUM_JOB128, 0, 0, 0, 0, 0, 0)
] __attribute__((aligned(CELL_SPURS_JOBQUEUE_JOB_DESCRIPTOR_POOL_ALIGN)));

int main(void)
{
    std::printf("hello-spurs-jq: bring-up (%u jobs)\n", kNumJobs);

    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_SPURS_JQ);
    if (rc) { std::printf("  SysmoduleLoad SPURS_JQ: %#x\n", rc); return 1; }

    rc = spu_printf_initialize(kSpuPrintfPriority, 0);
    if (rc) { std::printf("  spu_printf_initialize: %#x\n", rc); return 1; }

    cell::Spurs::SpursAttribute sattr;
    rc = cell::Spurs::SpursAttribute::initialize(&sattr, 4, 100, 2, false);
    if (rc) { std::printf("  SpursAttribute init: %#x\n", rc); return 1; }
    sattr.setNamePrefix("SpursJq", 7);
    sattr.setSpuThreadGroupType(SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT);
    sattr.enableSpuPrintfIfAvailable();

    cell::Spurs::Spurs *spurs = new cell::Spurs::Spurs;
    rc = cell::Spurs::Spurs::initialize(spurs, &sattr);
    if (rc) { std::printf("  Spurs::initialize: %#x\n", rc); return 1; }
    std::printf("  Spurs2 up\n");

    CellSpursJobQueueAttribute jqAttr;
    rc = cellSpursJobQueueAttributeInitialize(&jqAttr);
    if (rc) { std::printf("  JQ AttrInit: %#x\n", rc); goto teardown_spurs; }
    cellSpursJobQueueAttributeSetMaxSizeJobDescriptor(&jqAttr, 256);
    cellSpursJobQueueAttributeSetIsHaltOnError(&jqAttr, true);

    {
        CellSpursJobQueueJobDescriptorPool poolDesc = {0};
        poolDesc.nJob128 = _JQ_POOL_NUM_JOB128;
        rc = cellSpursCreateJobQueueWithJobDescriptorPool(
            (CellSpurs *)spurs, &s_jq, &jqAttr, s_jqPool, &poolDesc,
            "hello-jq", s_chain, _JQ_DEPTH, /*numSpus*/2, s_priority);
    }
    if (rc) { std::printf("  CreateJobQueue: %#x\n", rc); goto teardown_spurs; }
    std::printf("  JobQueue created\n");

    rc = cellSpursJobQueuePort2Create(&s_port, &s_jq);
    if (rc) { std::printf("  Port2Create: %#x\n", rc); goto teardown_jq; }
    std::printf("  Port2 created\n");

    /* --- Build N descriptors, one per job --- */
    for (unsigned i = 0; i < kNumJobs; ++i) {
        std::memset(&s_jobs[i], 0, sizeof(s_jobs[i]));
        std::memcpy(&s_jobs[i].header, hello_spurs_jq_jobheader_bin,
                    sizeof(CellSpursJobHeader));
        s_jobs[i].header.eaBinary += (uint64_t)(uintptr_t)hello_spurs_jq_bin;
        s_jobs[i].workArea.userData[0] = (uint64_t)(uintptr_t)&s_out[i * 4];
        s_jobs[i].workArea.userData[1] = (uint64_t)(kMagicBase + i);
    }

    /* --- Push them all --- */
    for (unsigned i = 0; i < kNumJobs; ++i) {
        rc = cellSpursJobQueuePort2PushJob(&s_port, &s_jobs[i].header,
                                           sizeof(s_jobs[i]),
                                           /*tag*/0, /*flag*/0);
        if (rc) {
            std::printf("  Port2PushJob[%u]: %#x\n", i, rc);
            goto teardown_port;
        }
    }
    std::printf("  %u jobs pushed\n", kNumJobs);

    /* --- Wait for every slot's magic --- */
    {
        bool jq_halted = false;
        unsigned slots_filled = 0;
        for (int round = 0; round < 400 && slots_filled < kNumJobs; ++round) {
            for (int spins = 0; spins < 200000; ++spins) {
                __asm__ volatile ("" ::: "memory");
                slots_filled = 0;
                for (unsigned i = 0; i < kNumJobs; ++i) {
                    if (s_out[i * 4] == kMagicBase + i) ++slots_filled;
                }
                if (slots_filled >= kNumJobs) break;
            }
            if (slots_filled >= kNumJobs) break;

            int err = 0; void *cause = 0;
            int erc = cellSpursJobQueueGetError(&s_jq, &err, &cause);
            if (erc == 0 && err) {
                std::printf("  [round %d] JQ halted: err=%#x cause=%p (slots=%u/%u)\n",
                            round, err,
                            (void *)(uintptr_t)(unsigned)(uintptr_t)cause,
                            slots_filled, kNumJobs);
                jq_halted = true;
                break;
            }
        }

        std::printf("  slots filled: %u/%u\n", slots_filled, kNumJobs);
        for (unsigned i = 0; i < kNumJobs; ++i) {
            std::printf("    job[%u] magic=%#x dmaTag=%#x eaBin=%#x_%#x\n",
                        i,
                        (unsigned)s_out[i * 4 + 0],
                        (unsigned)s_out[i * 4 + 1],
                        (unsigned)s_out[i * 4 + 3],
                        (unsigned)s_out[i * 4 + 2]);
        }

        if (jq_halted || slots_filled < kNumJobs) {
            std::printf("FAILURE: bypassing teardown to force exit\n");
            spu_printf_finalize();
            cellSysmoduleUnloadModule(CELL_SYSMODULE_SPURS_JQ);
            sys_process_exit(1);
        }
    }

    cellSpursJobQueuePort2Sync(&s_port, 0);

teardown_port:
    cellSpursJobQueuePort2Destroy(&s_port);
teardown_jq:
    cellSpursShutdownJobQueue(&s_jq);
    {
        int exitCode = 0;
        rc = cellSpursJoinJobQueue(&s_jq, &exitCode);
        if (rc) std::printf("  JoinJobQueue: %#x\n", rc);
        else    std::printf("  JoinJobQueue ok (exit=%#x)\n", exitCode);
    }
teardown_spurs:
    rc = spurs->finalize();
    if (rc) std::printf("  Spurs::finalize: %#x\n", rc);
    delete spurs;
    spu_printf_finalize();
    cellSysmoduleUnloadModule(CELL_SYSMODULE_SPURS_JQ);

    /* Final pass-fail */
    {
        unsigned ok = 0;
        for (unsigned i = 0; i < kNumJobs; ++i)
            if (s_out[i * 4] == kMagicBase + i) ++ok;
        if (ok == kNumJobs) { std::printf("SUCCESS\n"); return 0; }
        std::printf("FAILURE: only %u/%u sentinels\n", ok, kNumJobs);
        return 1;
    }
}
