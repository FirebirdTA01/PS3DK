/*
 * hello-spu-job - PPU side.
 *
 * STATUS: two of three known dispatcher gates cleared.
 *   - JOB_DESCRIPTOR (0x80410a0b) gate: cleared by setting
 *     binaryInfo[0..3] = "bin2" magic.
 *   - SPU ELF acceptance gate: the SPU ELF passes the reference
 *     wrapping tool cleanly (see sdk/libspurs_job linker script).
 *   - MEMORY_SIZE (0x80410a17) gate: STILL FIRING.  Reported value
 *     at jobChain+0x84 is constant 0x0003F700 across every
 *     descriptor variation we've tried.  The dispatcher rejects
 *     ANY non-zero binaryInfo[4..7] regardless of binary content
 *     (flat raw / ELF / jobbin2-wrapped), sizeBinary, or the other
 *     descriptor size fields.  See docs/spurs-job-binary2-re.md
 *     Sessions 5 + 6 for the full probe matrix and decoded
 *     dispatcher disassembly; next-session approaches need either
 *     a memory-write watchpoint at &jobChain.error via RPCS3's GDB
 *     stub, or SPU instruction trace logging to capture which of
 *     the four 0xa17 sites fires and what register state preceded
 *     it.
 *
 * Sample drives the JobChain machinery end-to-end and prints a
 * halt diagnostic + the full job-descriptor + chain-info bytes so
 * it stays useful as a reproducer until the dispatcher's
 * MEMORY_SIZE trigger is decoded.
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

/* Dump 0x70..0xa0 of the CellSpursJobChain to confirm what value is
 * actually parked at +0x80 (RPCS3's CellSpursJobChain.error field) and
 * +0x88 (cause).  Helps separate "struct-offset misread" from "real
 * write of 0x80410a0b". */
static void dump_jc(const void *jc_, const char *tag)
{
    const uint8_t *jc = (const uint8_t *)jc_;
    std::printf("  jc[%s] raw bytes [0x70..0xa0]:\n", tag);
    for (int row = 0; row < 3; ++row) {
        std::printf("    %02x:", 0x70 + row * 16);
        for (int col = 0; col < 16; ++col)
            std::printf(" %02x", jc[0x70 + row * 16 + col]);
        std::printf("\n");
    }
}

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
    /* CellSpursJobHeader.binaryInfo[10] is a UNION over the leading
     * fields of the BINARY2-format header:
     *   offset 0..7  uint64_t eaBinary    (full 64-bit EA of the image)
     *   offset 8..9  uint16_t sizeBinary  (image size in 16-byte units)
     * Setting binaryInfo[0..3] to a magic word writes the high 32 bits
     * of eaBinary, parking the EA in unmapped memory and tripping the
     * dispatcher's MEMORY_SIZE gate when it tries to DMA the image.
     * Use the union member directly. */
    s_job.header.eaBinary   = (uint64_t)(uintptr_t)hello_spu_job_bin;
    s_job.header.sizeBinary = CELL_SPURS_GET_SIZE_BINARY(hello_spu_job_bin_size);
    s_job.header.jobType    = CELL_SPURS_JOB_TYPE_BINARY2;
    std::printf("  BINARY2 jobbin fill: eaBin=%#x sizeBin16=%u (=%u bytes) jobType=%#x\n",
                (unsigned)(uintptr_t)hello_spu_job_bin,
                (unsigned)CELL_SPURS_GET_SIZE_BINARY(hello_spu_job_bin_size),
                (unsigned)hello_spu_job_bin_size,
                (unsigned)s_job.header.jobType);

    /* Dump the first 64 bytes of the job descriptor so we can see the
     * exact CellSpursJobHeader layout the dispatcher will DMA. */
    {
        const uint8_t *jb = (const uint8_t *)&s_job;
        std::printf("  s_job raw bytes [0x00..0x40]:\n");
        for (int row = 0; row < 4; ++row) {
            std::printf("    %02x:", row * 16);
            for (int col = 0; col < 16; ++col)
                std::printf(" %02x", jb[row * 16 + col]);
            std::printf("\n");
        }
        std::printf("  CellSpursJobHeader sizeof=%u  offsets:\n",
                    (unsigned)sizeof(CellSpursJobHeader));
        std::printf("    binaryInfo=%u sizeDmaList=%u eaHighInput=%u\n",
                    (unsigned)__builtin_offsetof(CellSpursJobHeader, binaryInfo),
                    (unsigned)__builtin_offsetof(CellSpursJobHeader, sizeDmaList),
                    (unsigned)__builtin_offsetof(CellSpursJobHeader, eaHighInput));
        std::printf("    useInOutBuffer=%u sizeInOrInOut=%u sizeOut=%u\n",
                    (unsigned)__builtin_offsetof(CellSpursJobHeader, useInOutBuffer),
                    (unsigned)__builtin_offsetof(CellSpursJobHeader, sizeInOrInOut),
                    (unsigned)__builtin_offsetof(CellSpursJobHeader, sizeOut));
        std::printf("    sizeStack=%u sizeScratch=%u eaHighCache=%u\n",
                    (unsigned)__builtin_offsetof(CellSpursJobHeader, sizeStack),
                    (unsigned)__builtin_offsetof(CellSpursJobHeader, sizeScratch),
                    (unsigned)__builtin_offsetof(CellSpursJobHeader, eaHighCache));
        std::printf("    sizeCacheDmaList=%u jobType=%u\n",
                    (unsigned)__builtin_offsetof(CellSpursJobHeader, sizeCacheDmaList),
                    (unsigned)__builtin_offsetof(CellSpursJobHeader, jobType));
    }

    /* SPU side reads workArea.userData[0..1] for {out_ea, magic}.
     * sizeDmaList stays 0 - no input list this round, so the SPU job
     * never goes through cellSpursJobGetPointerList. */
    s_job.workArea.userData[0] = (uint64_t)(uintptr_t)&s_out[0];
    s_job.workArea.userData[1] = kMagic;

    /* Job stack / scratch: the dispatcher allocates per-job, but the
     * descriptor needs minimum quadword counts.  4 KB stack + no
     * scratch is plenty for our 4-instruction job. */
    /* Reference job_hello descriptor leaves these all at zero (the
     * dispatcher provisions stack/scratch from the job buffer it
     * already sized).  Setting sizeStack=256 above pushed the
     * dispatcher past its budget and reproduced as MEMORY_SIZE. */
    s_job.header.sizeStack   = 0;
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
                                              /*maxGrabbedJob           */ 16,
                                              s_priorityTable,
                                              /*maxContention           */ 4,
                                              /*autoRequestSpuCount     */ true,
                                              /*tag1                    */ 0,
                                              /*tag2                    */ 1,
                                              /*isFixedMemAlloc         */ false,
                                              /*maxSizeJobDescriptor    */ 256,
                                              /*initialRequestSpuCount  */ 0);
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
        dump_jc(jc, "post-Run");

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
                if (info.isHalted) {
                    dump_jc(jc, "halted");
                    break;
                }
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
