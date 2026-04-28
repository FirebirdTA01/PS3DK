/*
 * hello-spurs-jq - SPURS job-queue end-to-end runtime test.
 *
 * PPU side: spin up a CellSpurs2 instance, create a JobQueue + Port2,
 * push a single CellSpursJob256 carrying a sentinel buffer EA + magic
 * value in userData[], wait for the SPU side to write the magic back,
 * then tear down.
 *
 * SPU side (spu/source/main.c): cellSpursJobQueueMain reads the EA +
 * magic out of the descriptor and DMAs them back.
 *
 * Currently a probe of how far our libspurs_jq runtime skeleton +
 * SysCall::__initialize stub get before the SPURS LLE in RPCS3 trips
 * a check.  Iteration target.
 */

#include <cstdio>
#include <cstring>

#include <sys/process.h>
#include <cell/spurs.h>
#include <cell/spurs/job_queue.h>
#include <cell/spurs/job_queue_port2.h>
#include <cell/sysmodule.h>
#include <spu_printf.h>

/* JOBBIN_WRAP produces two bin2s blobs:
 *   <name>_bin               - the wrapped jobbin2 blob (0x100-byte ELF
 *                              prefix + SPU LS image bytes)
 *   <name>_jobheader_bin     - the 48-byte CellSpursJobHeader template
 *                              the reference packager generated, with
 *                              eaBinary low32 = 0x100 (unresolved reloc
 *                              addend; we add the runtime blob start)
 *
 * Build glue is in cmake/ps3-self.cmake (ps3_add_spu_image / JOBBIN_WRAP). */
#include "hello_spurs_jq_bin.h"
#include "hello_spurs_jq_jobheader_bin.h"

SYS_PROCESS_PARAM(1001, 0x10000);

static const int      kSpuPrintfPriority = 999;
static const uint32_t kMagic             = 0xC0FFEE99u;

/* Queue command-buffer needs to be sized via CELL_SPURS_JOBQUEUE_SIZE_COMMAND_BUFFER
 * - the runtime reserves a per-client tail-pointer region in addition
 * to the depth*sizeof(uint64_t) command slots. */
#define _JQ_DEPTH 16
static CellSpursJobQueue       s_jq    __attribute__((aligned(128)));
static CellSpursJobQueuePort2  s_port  __attribute__((aligned(128)));
/* Reference JQ sample uses CellSpursJob128 (smaller; userData[10]).
 * Keep maxSizeJobDescriptor=256 for both create + check, but the
 * actual descriptor is 128 bytes. */
static CellSpursJob128         s_job   __attribute__((aligned(128)));
static uint64_t                s_chain[CELL_SPURS_JOBQUEUE_SIZE_COMMAND_BUFFER(_JQ_DEPTH) / sizeof(uint64_t)]
    __attribute__((aligned(CELL_SPURS_JOBQUEUE_COMMAND_BUFFER_ALIGN)));
static volatile uint32_t       s_out[4] __attribute__((aligned(128))) = { 0, 0, 0, 0 };
static const uint8_t           s_priority[8] = { 8, 0, 0, 0, 0, 0, 0, 0 };

/* JobDescriptor pool the JQ uses to hold copy-pushed descriptors. */
#define _JQ_POOL_NUM_JOB256  3
static uint8_t s_jqPool[
    CELL_SPURS_JOBQUEUE_JOB_DESCRIPTOR_POOL_SIZE(0, 0, _JQ_POOL_NUM_JOB256, 0, 0, 0, 0, 0)
] __attribute__((aligned(CELL_SPURS_JOBQUEUE_JOB_DESCRIPTOR_POOL_ALIGN)));

static void dump_hex(const char *tag, const void *p, unsigned bytes)
{
    const uint8_t *b = (const uint8_t *)p;
    std::printf("  %s [%u bytes]:\n", tag, bytes);
    for (unsigned row = 0; row < bytes; row += 16) {
        std::printf("    %04x:", row);
        for (unsigned col = 0; col < 16 && row + col < bytes; ++col)
            std::printf(" %02x", b[row + col]);
        std::printf("\n");
    }
}

int main(void)
{
    std::printf("hello-spurs-jq: bring-up\n");

    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_SPURS_JQ);
    if (rc) {
        std::printf("  SysmoduleLoad SPURS_JQ: %#x\n", rc);
        return 1;
    }

    rc = spu_printf_initialize(kSpuPrintfPriority, 0);
    if (rc) { std::printf("  spu_printf_initialize: %#x\n", rc); return 1; }

    /* --- Spurs2 up --- */
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

    /* --- JobQueue attribute + create --- */
    CellSpursJobQueueAttribute jqAttr;
    rc = cellSpursJobQueueAttributeInitialize(&jqAttr);
    if (rc) { std::printf("  JQ AttrInit: %#x\n", rc); goto teardown_spurs; }
    cellSpursJobQueueAttributeSetMaxSizeJobDescriptor(&jqAttr, 256);
    cellSpursJobQueueAttributeSetIsHaltOnError(&jqAttr, true);

    {
        CellSpursJobQueueJobDescriptorPool poolDesc = {0};
        poolDesc.nJob256 = _JQ_POOL_NUM_JOB256;
        rc = cellSpursCreateJobQueueWithJobDescriptorPool(
            (CellSpurs *)spurs, &s_jq, &jqAttr, s_jqPool, &poolDesc,
            "hello-jq", s_chain, _JQ_DEPTH, /*numSpus*/1, s_priority);
    }
    if (rc) {
        std::printf("  CreateJobQueue: %#x\n", rc);
        goto teardown_spurs;
    }
    std::printf("  JobQueue created\n");

    /* --- Port2 create --- */
    rc = cellSpursJobQueuePort2Create(&s_port, &s_jq);
    if (rc) {
        std::printf("  Port2Create: %#x\n", rc);
        goto teardown_jq;
    }
    std::printf("  Port2 created\n");

    /* --- Build the job --- */
    std::memset(&s_job, 0, sizeof(s_job));
    /* Stamp in the reference-packager-produced jobheader template,
     * then patch eaBinary by adding the runtime blob start (the
     * template's low32 carries the +0x100 reloc addend). */
    std::memcpy(&s_job.header, hello_spurs_jq_jobheader_bin,
                sizeof(CellSpursJobHeader));
    s_job.header.eaBinary += (uint64_t)(uintptr_t)hello_spurs_jq_bin;
    s_job.workArea.userData[0] = (uint64_t)(uintptr_t)&s_out[0];
    s_job.workArea.userData[1] = kMagic;

    dump_hex("s_job header", &s_job, 64);

    /* --- Push the job: plain pushJob with flag=0.  Simpler than the
     * copyPushJob+SYNC+pool path; if this also halts we know the
     * issue isn't in the push variant. --- */
    rc = cellSpursJobQueuePort2PushJob(&s_port, &s_job.header,
                                       sizeof(s_job), /*tag*/0,
                                       /*flag*/0);
    if (rc) {
        std::printf("  Port2PushJob: %#x\n", rc);
        goto teardown_port;
    }
    std::printf("  Job pushed (pushJob, flag=0)\n");

    /* --- Wait for the SPU side to write back --- */
    {
        bool got_sentinel = false;
        bool jq_halted    = false;
        for (int round = 0; round < 200; ++round) {
            for (int spins = 0; spins < 200000 && s_out[0] != kMagic; ++spins) {
                __asm__ volatile ("" ::: "memory");
            }
            if (s_out[0] == kMagic) { got_sentinel = true; break; }
            int err = 0; void *cause = 0;
            int erc = cellSpursJobQueueGetError(&s_jq, &err, &cause);
            if (erc == 0 && err) {
                std::printf("  [round %d] JQ halted: err=%#x cause=%p\n",
                            round, err,
                            (void *)(uintptr_t)(unsigned)(uintptr_t)cause);
                jq_halted = true;
                break;
            }
        }
        if (got_sentinel) {
            std::printf("  Sentinel received: s_out=[%#x %#x %#x %#x]\n",
                        (unsigned)s_out[0], (unsigned)s_out[1],
                        (unsigned)s_out[2], (unsigned)s_out[3]);
        } else {
            std::printf("  TIMED OUT waiting for sentinel\n");
        }

        /* If the JQ halted or we timed out, the Shutdown/Join path
         * below may block forever waiting on the stuck SPU kernel.
         * Bail straight to process exit so the emulator can shut down. */
        if (jq_halted || !got_sentinel) {
            std::printf("FAILURE: bypassing teardown to force exit\n");
            spu_printf_finalize();
            cellSysmoduleUnloadModule(CELL_SYSMODULE_SPURS_JQ);
            sys_process_exit(1);
        }
    }

    /* --- Sync-flush the port + drain --- */
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

    if (s_out[0] == kMagic) {
        std::printf("SUCCESS\n");
        return 0;
    }
    std::printf("FAILURE: SPU side did not return the sentinel\n");
    return 1;
}
