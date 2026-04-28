/*
 * hello-spu-job - PPU side.
 *
 * STATUS: header surface + libspurs_job.a + spurs_job.ld + raw-image
 * objcopy chain are all in place and produce a byte-correct SPU job
 * binary.  The SPRX dispatcher however still rejects the descriptor
 * with JOB_DESCRIPTOR (0x80410a0b) - the BINARY2 binaryInfo[10] wire
 * format expected by the runtime contains additional fields whose
 * exact layout the reference SDK's job_elf-to-bin tool produces from
 * inputs that depend on the -mspurs-job GCC patch (e_flags=1 in the
 * SPU ELF header, plus a _cell_spu_ls_param symbol layout we don't
 * synthesise).  Resolving that needs either RPCS3 dispatcher source
 * cross-reference or a job_elf-to-bin reimplementation; both are out
 * of scope for this iteration.
 *
 * The sample below still drives the JobChain machinery end-to-end -
 * Spurs2 init, attribute/chain create + run + halt-diagnose + join +
 * teardown - it just times out waiting for the SPU sentinel rather
 * than printing SUCCESS.  The diagnostic loop reports the real halt
 * statusCode + dispatcher PC so this stays useful as a reproducer.
 */

#include <cstdio>
#include <cstring>

#include <sys/process.h>
#include <cell/spurs.h>
#include <spu_printf.h>

#include "hello_spu_job_bin.h"

SYS_PROCESS_PARAM(1001, 0x10000);

static const int kSpuPrintfPriority = 999;
static const uint32_t kMagic        = 0xC0FFEE99u;

/* The job descriptor and the chain entry both need 128-byte alignment. */
static CellSpursJob256 s_job __attribute__((aligned(128)));

/* Sentinel buffer the SPU writes to. */
static volatile uint32_t s_out[4] __attribute__((aligned(128))) = { 0, 0, 0, 0 };

/* Job-chain command list: one CELL_SPURS_JOB_COMMAND_JOB(&s_job) entry
 * followed by END.  Aligned to 16 because the runtime DMAs entries in
 * 8-byte chunks. */
static uint64_t s_chain[4] __attribute__((aligned(16)));

/* Per-SPU priority table for the workload backing this chain. */
static const uint8_t s_priorityTable[8] = { 8, 0, 0, 0, 0, 0, 0, 0 };

int main(void)
{
    std::printf("hello-spu-job: bring-up\n");

    int rc = spu_printf_initialize(kSpuPrintfPriority, 0);
    if (rc) { std::printf("  spu_printf_initialize: %#x\n", rc); return 1; }

    /* --- Spurs2 up --- */
    cell::Spurs::SpursAttribute sattr;
    rc = cell::Spurs::SpursAttribute::initialize(&sattr, 4, 100, 2, false);
    if (rc) { std::printf("  SpursAttribute: %#x\n", rc); return 1; }
    sattr.setNamePrefix("SpuJob", 6);
    sattr.setSpuThreadGroupType(SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT);
    sattr.enableSpuPrintfIfAvailable();

    cell::Spurs::Spurs *spurs = new cell::Spurs::Spurs;
    rc = cell::Spurs::Spurs::initialize(spurs, &sattr);
    if (rc) { std::printf("  Spurs::initialize: %#x\n", rc); return 1; }
    std::printf("  Spurs2 up\n");

    /* --- Build the CellSpursJob256 descriptor --- */
    std::memset(&s_job, 0, sizeof(s_job));
    /* cellSpursJobHeaderSetJobbin2Param validates against a
     * make_jobbin2-wrapped binary (with magic + metadata header).
     * Our toolchain embeds a flat raw image instead — so the helper
     * returns CELL_SPURS_JOB_ERROR_INVAL (0x80410a02) and we fall
     * back to filling binaryInfo[] by hand.  This is the canonical
     * path until make_jobbin2 ships. */
    /* BINARY2-format job descriptor.  Our raw image starts with the
     * canonical xori-dance signature in .before_text, which the SPRX
     * recognises as a valid jobbin2 entry stub.  Layout for BINARY2:
     *   binaryInfo[0..3] = additional bss/extension size, in 16-byte
     *                       units (we don't need any extra space)
     *   binaryInfo[4..7] = eaBinary (32-bit EA of the raw image)
     *   binaryInfo[8..9] = sizeBinary = file size in 16-byte units
     * Legacy jobType=0 was reproducibly returning CELL_SPURS_JOB_ERROR_PERM
     * from the SPRX dispatcher; BINARY2 is what cellSpursJobHeaderSetJobbin2Param
     * produces and what the dispatcher seems to want. */
    {
        uint8_t *bi = s_job.header.binaryInfo;
        const uint32_t extraSize16 = 0;
        const uint32_t eaBin       = (uint32_t)(uintptr_t)hello_spu_job_bin;
        const uint16_t sizeBin16   = CELL_SPURS_GET_SIZE_BINARY(hello_spu_job_bin_size);
        bi[0] = (uint8_t)(extraSize16 >> 24);
        bi[1] = (uint8_t)(extraSize16 >> 16);
        bi[2] = (uint8_t)(extraSize16 >> 8);
        bi[3] = (uint8_t)(extraSize16);
        bi[4] = (uint8_t)(eaBin >> 24);
        bi[5] = (uint8_t)(eaBin >> 16);
        bi[6] = (uint8_t)(eaBin >> 8);
        bi[7] = (uint8_t)(eaBin);
        bi[8] = (uint8_t)(sizeBin16 >> 8);
        bi[9] = (uint8_t)(sizeBin16);
    }
    s_job.header.jobType = CELL_SPURS_JOB_TYPE_BINARY2;
    std::printf("  BINARY2 jobbin fill: eaBin=%#x sizeBin16=%u (=%u bytes) jobType=%#x\n",
                (unsigned)(uintptr_t)hello_spu_job_bin,
                (unsigned)CELL_SPURS_GET_SIZE_BINARY(hello_spu_job_bin_size),
                (unsigned)hello_spu_job_bin_size,
                (unsigned)s_job.header.jobType);

    /* SPU side reads workArea.userData[0..1] for {out_ea, magic}.
     * sizeDmaList stays 0 - no input list this round, so the SPU job
     * never goes through cellSpursJobGetPointerList. */
    s_job.workArea.userData[0] = (uint64_t)(uintptr_t)&s_out[0];
    s_job.workArea.userData[1] = kMagic;

    /* Job stack / scratch: the dispatcher allocates per-job, but the
     * descriptor needs minimum quadword counts.  4 KB stack + no
     * scratch is plenty for our 4-instruction job. */
    s_job.header.sizeStack   = 256;  /* in 16-byte units => 4096 bytes */
    s_job.header.sizeScratch = 0;
    s_job.header.useInOutBuffer = 0;
    s_job.header.sizeInOrInOut  = 0;
    s_job.header.sizeOut        = 0;

    /* --- Job-chain command list: run s_job, then END. --- */
    s_chain[0] = CELL_SPURS_JOB_COMMAND_JOB(&s_job);
    s_chain[1] = CELL_SPURS_JOB_COMMAND_LWSYNC;
    s_chain[2] = CELL_SPURS_JOB_COMMAND_END;
    s_chain[3] = 0;

    /* --- JobChainAttribute --- */
    CellSpursJobChainAttribute jcAttr;
    std::memset(&jcAttr, 0, sizeof(jcAttr));
    rc = cellSpursJobChainAttributeInitialize(&jcAttr, s_chain,
                                              /*sizeJobDescriptor       */ 256,
                                              /*maxGrabbedJob           */ 1,
                                              s_priorityTable,
                                              /*maxContention           */ 1,
                                              /*autoRequestSpuCount     */ true,
                                              /*tag1                    */ 0,
                                              /*tag2                    */ 1,
                                              /*isFixedMemAlloc         */ false,
                                              /*maxSizeJobDescriptor    */ 256,
                                              /*initialRequestSpuCount  */ 1);
    if (rc) { std::printf("  JobChainAttribute init: %#x\n", rc); goto teardown; }
    cellSpursJobChainAttributeSetName(&jcAttr, "spu-job");
    cellSpursJobChainAttributeSetHaltOnError(&jcAttr);

    /* --- Create / Run the chain.  END terminates the chain naturally
     * after our job runs - no Shutdown needed before Join. --- */
    {
        CellSpursJobChain *jc = new CellSpursJobChain;
        rc = cellSpursCreateJobChainWithAttribute((CellSpurs *)spurs, jc, &jcAttr);
        if (rc) { std::printf("  CreateJobChainWithAttribute: %#x\n", rc); delete jc; goto teardown; }

        rc = cellSpursRunJobChain(jc);
        if (rc) { std::printf("  RunJobChain: %#x\n", rc); delete jc; goto teardown; }
        std::printf("  RunJobChain ok; polling for sentinel ...\n");

        /* Poll on s_out[0] but also peek at chain info so a silent
         * dispatcher reject (isHalted=1) tells us why nothing's
         * happening instead of leaving us spinning. */
        bool got_sentinel = false;
        for (int round = 0; round < 50; ++round) {
            for (int spins = 0; spins < 500000 && s_out[0] != kMagic; ++spins) {
                __asm__ volatile ("" ::: "memory");
            }
            if (s_out[0] == kMagic) { got_sentinel = true; break; }

            CellSpursJobChainInfo info;
            std::memset(&info, 0, sizeof(info));
            int qrc = cellSpursGetJobChainInfo(jc, &info);
            if (qrc == 0) {
                /* info.cause is a 32-bit PRX-mode pointer (offset into
                 * the SPRX); info.statusCode is the actual numeric
                 * halt code.  Also print programCounter (LS PC of the
                 * trapping SPU instruction) and the link-register
                 * trio so we can locate the crash site in our own
                 * binary.  Fall back to cellSpursJobChainGetError to
                 * cross-check the cause field. */
                void *errCause = nullptr;
                int   gerc     = cellSpursJobChainGetError(jc, &errCause);
                std::printf("  [round %d] isHalted=%d statusCode=%#x cause=%p "
                            "PC=%p LR={%#x,%#x,%#x} grabbed=%u\n",
                            round, (int)info.isHalted,
                            (unsigned)info.statusCode,
                            (void *)(uintptr_t)(unsigned)(uintptr_t)info.cause,
                            (void *)(uintptr_t)(unsigned)(uintptr_t)info.programCounter,
                            (unsigned)info.linkRegister[0],
                            (unsigned)info.linkRegister[1],
                            (unsigned)info.linkRegister[2],
                            (unsigned)info.maxGrabbedJob);
                if (gerc == 0)
                    std::printf("           GetError: cause=%p\n", errCause);
                else
                    std::printf("           GetError: %#x\n", gerc);
                if (info.isHalted) break;
            } else {
                std::printf("  [round %d] no sentinel; GetJobChainInfo: %#x\n",
                            round, qrc);
            }
        }

        /* If the dispatcher halted on us, Shutdown unblocks Join.  If
         * the chain ended normally Join is fine on its own. */
        cellSpursShutdownJobChain(jc);
        rc = cellSpursJoinJobChain(jc);
        if (rc) std::printf("  JoinJobChain: %#x\n", rc);
        else    std::printf("  Joined ok\n");

        if (!got_sentinel) {
            std::printf("  TIMED OUT waiting for sentinel\n");
        }

        delete jc;
    }

    std::printf("\n=== SPU job sentinel buffer ===\n");
    std::printf("  s_out[0] (magic)           = %#x  (expected %#x)\n",
                (unsigned)s_out[0], (unsigned)kMagic);
    std::printf("  s_out[1] (jobContext.dmaTag) = %u\n", (unsigned)s_out[1]);
    std::printf("  s_out[2] (eaBinary lo)       = %#x\n", (unsigned)s_out[2]);
    std::printf("  s_out[3] (eaBinary hi)       = %#x\n", (unsigned)s_out[3]);

teardown:
    rc = spurs->finalize();
    if (rc) std::printf("  finalize: %#x\n", rc);
    delete spurs;
    spu_printf_finalize();

    if (s_out[0] == kMagic) {
        std::printf("SUCCESS\n");
        return 0;
    } else {
        std::printf("FAILURE: SPU job did not produce expected sentinel\n");
        return 1;
    }
}
