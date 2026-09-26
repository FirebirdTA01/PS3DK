#!/usr/bin/env bash
# The SPU SPURS headers declare the SPU (effective-address) API, and the PPU
# side of the shared headers is unchanged.
#
# SPU code names SPURS objects (event flag, barrier, semaphore, queue,
# lock-free queue, task exit code, job-queue ports) by their 64-bit main
# memory address, and the blocking / non-blocking pairs are one entry point
# with a flag.  Several shared headers used to hand the SPU the PPU pointer
# prototypes, so every SPU call site failed to compile.  cell/spurs/task.h
# must also make CELL_OK, the common getters, the task types and the
# CELL_SPURS_TASK_* / CELL_SPURS_* constants visible.
#
# The SPU rows compile reference-shaped calls in C and C++ with
# -Wall -Wextra -Werror against the SDK headers (-I, so the headers are
# linted too) and pin signatures by assigning each entry point to a
# function pointer of the expected type.  The PPU row compiles the PPU
# pointer API in both ABIs (-isystem: other PPU system headers are not
# -Wextra clean) and checks the SPU-only macros stay out of PPU code.
#
# usage: spurs-spu-header-surface-test.sh [--ps3dev DIR]
#   headers come from $PS3DK (default DIR/ps3dk)
set -u
ps3dev="${PS3DEV:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "spurs-spu-header-surface: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
sdk="${PS3DK:-$ps3dev/ps3dk}"
spucc="$ps3dev/spu/bin/spu-elf-gcc"
spucxx="$ps3dev/spu/bin/spu-elf-g++"
ppucc="$ps3dev/ppu/bin/powerpc64-ps3-elf-gcc"
ppucxx="$ps3dev/ppu/bin/powerpc64-ps3-elf-g++"
status=0
fail() { echo "spurs-spu-header-surface: FAIL: $*"; status=1; }
ok() { echo "spurs-spu-header-surface: ok   $*"; }
for t in "$spucc" "$spucxx" "$ppucc" "$ppucxx"; do
    [ -x "$t" ] || { fail "missing compiler $t"; exit 1; }
done
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# ---------------------------------------------------------------- SPU, C
cat > "$work/spu.c" <<'EOF'
#include <cell/spurs.h>
#include <cell/spurs/task.h>
#include <cell/spurs/job_queue_port.h>
#include <cell/spurs/job_queue_port2.h>

#define PIN(name, expr) extern char pin_##name[(expr) ? 1 : -1]
PIN(lock_line, CELL_SPURS_LOCK_LINE == 0x80);
PIN(kernel_tag, CELL_SPURS_KERNEL_DMA_TAG_ID == 31);
PIN(poll_task, CELL_SPURS_TASK_POLL_FOUND_TASK == 1);
PIN(poll_wkl, CELL_SPURS_TASK_POLL_FOUND_WORKLOAD == 2);
PIN(again, CELL_SPURS_TASK_ERROR_AGAIN == (int)0x80410901u);
PIN(ctx_all, CELL_SPURS_TASK_CONTEXT_SIZE_ALL == 1024 + 0x3d000);
PIN(save_cfg, sizeof(CellSpursTaskSaveConfig) == 32);
PIN(task_id, sizeof(CellSpursTaskId) == 4);
PIN(attr, sizeof(CellSpursTaskAttribute) == 256 && sizeof(CellSpursTaskAttribute2) == 256);

/* Signatures: each entry point must convert to exactly this type. */
int (*const p_ef_set)(uint64_t, uint16_t) = cellSpursEventFlagSet;
int (*const p_ef_wait)(uint64_t, uint16_t *, CellSpursEventFlagWaitMode, unsigned) = _cellSpursEventFlagWait;
int (*const p_ef_ts)(uint64_t, uint64_t *) = cellSpursEventFlagGetTasksetAddress;
int (*const p_bar_init)(uint64_t, unsigned int) = cellSpursBarrierInitialize;
int (*const p_bar_notify)(uint64_t, unsigned) = _cellSpursBarrierNotify;
int (*const p_sem_init)(uint64_t, int, unsigned) = _cellSpursSemaphoreInitialize;
int (*const p_sem_p)(uint64_t) = cellSpursSemaphoreP;
int (*const p_q_init)(uint64_t, uint64_t, unsigned int, unsigned int, CellSpursQueueDirection, unsigned) = _cellSpursQueueInitialize;
int (*const p_q_push)(uint64_t, const void *, unsigned int, unsigned) = _cellSpursQueuePushBegin;
int (*const p_q_popend)(uint64_t, unsigned int, unsigned) = _cellSpursQueuePopEnd;
int (*const p_lfq_init)(uint64_t, uint64_t, unsigned int, unsigned int, CellSpursLFQueueDirection) = cellSpursLFQueueInitialize;
int (*const p_ec_get)(uint64_t, int *) = cellSpursTaskExitCodeGet;
int (*const p_sig)(uint64_t, CellSpursTaskId) = cellSpursSendSignal;
int (*const p_shutdown)(uint64_t) = cellSpursShutdownTaskset;
int (*const p_wait_sig)(void) = cellSpursWaitSignal;
int (*const p_p2_flush)(uint64_t, unsigned int, unsigned) = cellSpursJobQueuePort2PushFlush;
int (*const p_p2_sync)(uint64_t, unsigned, unsigned int, unsigned) = cellSpursJobQueuePort2PushSync;

extern uint64_t ea_obj, ea_buf, ea_elf;
extern const CellSpursJobHeader job_desc;

/* Kept as several small functions: one very large SPU function can put a
 * branch hint out of range, which is an unrelated compiler limit. */
int probe_task(qword arg, uint64_t eaTaskset)
{
    int rc = CELL_OK;
    CellSpursTaskId id = cellSpursGetTaskId();
    CellSpursTaskAttribute attr;
    CellSpursTaskSaveConfig cfg;

    cfg.eaContext = ea_buf;
    cfg.sizeContext = CELL_SPURS_TASK_CONTEXT_SIZE_ALL;
    cfg.lsPattern = CELL_SPURS_TASK_LS_ALL;
    rc |= cellSpursTaskAttributeInitialize(&attr, ea_elf, &cfg, arg);
    rc |= cellSpursCreateTaskWithAttribute(eaTaskset, &id, &attr);
    rc |= cellSpursCreateTask(eaTaskset, &id, ea_elf, ea_buf, 0x4000,
                              cellSpursContextGenerateLsPattern(0x3000, 0x4000), arg);
    rc |= cellSpursSendSignal(eaTaskset, id);
    rc |= cellSpursWaitSignal();
    if (cellSpursTaskPoll() & CELL_SPURS_TASK_POLL_FOUND_WORKLOAD)
        rc |= cellSpursYield();
    return rc | (int)cellSpursGetCurrentSpuId() | (int)CELL_SPURS_PPU_SYM(ppu_side_symbol)
              | (int)spu_read_decrementer();
}

int probe_event_flag(void)
{
    int rc = CELL_OK;
    uint16_t bits = 1;
    uint64_t ts = 0;
    CellSpursEventFlagDirection dir;
    CellSpursEventFlagClearMode clr;

    rc |= cellSpursEventFlagInitializeIWL(ea_obj, CELL_SPURS_EVENT_FLAG_CLEAR_AUTO,
                                          CELL_SPURS_EVENT_FLAG_SPU2PPU);
    rc |= cellSpursEventFlagSet(ea_obj, bits);
    rc |= cellSpursEventFlagClear(ea_obj, bits);
    rc |= cellSpursEventFlagWait(ea_obj, &bits, CELL_SPURS_EVENT_FLAG_OR);
    rc |= cellSpursEventFlagTryWait(ea_obj, &bits, CELL_SPURS_EVENT_FLAG_AND);
    rc |= cellSpursEventFlagGetDirection(ea_obj, &dir);
    rc |= cellSpursEventFlagGetClearMode(ea_obj, &clr);
    rc |= cellSpursEventFlagGetTasksetAddress(ea_obj, &ts);
    return rc | (int)ts | (int)dir | (int)clr;
}

int probe_barrier_semaphore(void)
{
    int rc = CELL_OK;
    uint64_t ts = 0;
    int code = 0;

    rc |= cellSpursBarrierInitialize(ea_obj, 4);
    rc |= cellSpursBarrierNotify(ea_obj);
    rc |= cellSpursBarrierTryNotify(ea_obj);
    rc |= cellSpursBarrierWait(ea_obj);
    rc |= cellSpursBarrierTryWait(ea_obj);
    rc |= cellSpursBarrierGetTasksetAddress(ea_obj, &ts);
    rc |= cellSpursSemaphoreInitialize(ea_obj, 1);
    rc |= cellSpursSemaphoreInitializeIWL(ea_obj, 1);
    rc |= cellSpursSemaphoreP(ea_obj);
    rc |= cellSpursSemaphoreV(ea_obj);
    rc |= cellSpursSemaphoreGetTasksetAddress(ea_obj, &ts);
    rc |= cellSpursTaskExitCodeGet(ea_obj, &code);
    return rc | (int)ts | code;
}

int probe_queues(void)
{
    int rc = CELL_OK;
    unsigned int n = 0;
    CellSpursQueueDirection qdir;
    CellSpursLFQueueDirection lfdir;
    CellSpursLFQueuePushContainer push;
    CellSpursLFQueuePopContainer pop;
    static char buf[128] __attribute__((aligned(128)));

    rc |= cellSpursQueueInitialize(ea_obj, ea_buf, 16, 8, CELL_SPURS_QUEUE_SPU2PPU);
    rc |= cellSpursQueuePushBegin(ea_obj, buf, 1);
    rc |= cellSpursQueuePushEnd(ea_obj, 1);
    rc |= cellSpursQueueTryPopBegin(ea_obj, buf, 2);
    rc |= cellSpursQueuePopEnd(ea_obj, 2);
    rc |= cellSpursQueuePeekBegin(ea_obj, buf, 2);
    rc |= cellSpursQueuePeekEnd(ea_obj, 2);
    rc |= cellSpursQueueSize(ea_obj, &n);
    rc |= cellSpursQueueGetDirection(ea_obj, &qdir);

    cellSpursLFQueuePushContainerInitialize(&push, buf, 3);
    cellSpursLFQueuePopContainerInitialize(&pop, buf, 3);
    rc |= cellSpursLFQueueInitialize(ea_obj, ea_buf, 16, 8, CELL_SPURS_LFQUEUE_SPU2SPU);
    rc |= cellSpursLFQueueTryPushBegin(ea_obj, &push);
    rc |= cellSpursLFQueuePushEnd(ea_obj, &push);
    rc |= cellSpursLFQueuePopBegin(ea_obj, &pop);
    rc |= cellSpursLFQueuePopEnd(ea_obj, &pop);
    rc |= cellSpursLFQueueDepth(ea_obj, &n);
    rc |= cellSpursLFQueueGetDirection(ea_obj, &lfdir);
    return rc | (int)n | (int)qdir | (int)lfdir;
}

int probe_job_queue(void)
{
    int rc = CELL_OK;
    rc |= cellSpursJobQueuePortCopyPushJob(ea_obj, &job_desc, 64, 0, 4, 1);
    rc |= cellSpursJobQueuePortPushFlush(ea_obj, 4);
    rc |= cellSpursJobQueuePort2CopyPushJob(ea_obj, &job_desc, 64, 64, 0, 4,
                                            CELL_SPURS_JOBQUEUE_FLAG_SYNC_JOB);
    rc |= cellSpursJobQueuePort2PushFlush(ea_obj, 4, 0);
    return rc | (int)cellSpursJobQueueDmaWaitTagStatusAll(1u << 4);
}
EOF
for std in -std=gnu99 -std=c11; do
    if "$spucc" $std -O2 -Wall -Wextra -Werror -I"$sdk/spu/include" \
            -c "$work/spu.c" -o "$work/spu.o" > "$work/e.log" 2>&1; then
        ok "SPU C EA API ($std)"
    else
        fail "SPU C EA API ($std): $(grep -m1 -E 'error' "$work/e.log")"
    fi
done

# -------------------------------------------------------------- SPU, C++
cat > "$work/spu.cpp" <<'EOF'
#include <cell/spurs.h>
#include <cell/spurs/job_queue_port.h>
#include <cell/spurs/job_queue_port2.h>

extern uint64_t ea_obj, ea_port;
extern const CellSpursJob256 job;

int probe_cpp(uint64_t eaTaskset)
{
    int rc = CELL_OK;
    uint16_t bits = 0;

    cell::Spurs::EventFlagStub ef;
    ef.setObject(ea_obj);
    rc |= ef.initializeIWL(cell::Spurs::EventFlagStub::kClearAuto,
                           cell::Spurs::EventFlag::kSpu2Ppu);
    rc |= ef.set(1);
    rc |= ef.wait(&bits, cell::Spurs::EventFlagStub::kOr);
    rc |= ef.tryWait(&bits, cell::Spurs::EventFlagStub::kAnd);

    cell::Spurs::SemaphoreStub sem;
    sem.setObject(ea_obj);
    rc |= sem.initialize(1);
    rc |= sem.p();
    rc |= sem.v();
    uint64_t ts = 0;
    rc |= sem.getTasksetAddress(&ts);

    using namespace cell::Spurs::JobQueue;
    Port2Stub port2;
    port2.setObject(ea_port);
    rc |= port2.pushJob(ea_obj, sizeof(job), 0, 5, Port2Stub::kFlagSyncJob);
    rc |= port2.copyPushJob(&job.header, sizeof(job), sizeof(job), 0, 5, 0);
    rc |= port2.pushFlush(5, 0);
    rc |= port2.pushSync(1, 5, 0);

    PortContainer port(ea_port);
    rc |= port.copyPushJob(&job.header, sizeof(job), 0, 5, 1);
    rc |= port.pushFlush(5);
    PortWithDescriptorBufferContainer<CellSpursJob256, 8> pdb(ea_port);
    rc |= pdb.initialize(ea_obj);
    rc |= pdb.copyPushJob(&job, sizeof(job), 0, 5, 1);

    rc |= cellSpursSendSignal(eaTaskset, cellSpursGetTaskId());
    return rc | (int)cellSpursJobQueueDmaWaitTagStatusAll(1u << 5) | (int)ts;
}
EOF
for std in -std=c++98 -std=c++17; do
    if "$spucxx" $std -O2 -Wall -Wextra -Werror -I"$sdk/spu/include" \
            -c "$work/spu.cpp" -o "$work/spu-cpp.o" > "$work/e.log" 2>&1; then
        ok "SPU C++ stubs and job-queue ports ($std)"
    else
        fail "SPU C++ ($std): $(grep -m1 -E 'error' "$work/e.log")"
    fi
done

# ----------------------------------------------------- PPU, unchanged API
cat > "$work/ppu.cpp" <<'EOF'
#include <cell/spurs.h>
#include <cell/spurs/barrier.h>
#include <cell/spurs/event_flag.h>
#include <cell/spurs/queue.h>
#include <cell/spurs/semaphore.h>
#include <cell/spurs/task_exit_code.h>

#if defined(cellSpursEventFlagWait) || defined(cellSpursBarrierNotify) || \
    defined(cellSpursSemaphoreInitialize) || defined(cellSpursQueuePushBegin)
#error "SPU-only EA macros leaked into PPU code"
#endif

int (*const p_set)(CellSpursEventFlag *, uint16_t) = cellSpursEventFlagSet;
int (*const p_wait)(CellSpursEventFlag *, uint16_t *, CellSpursEventFlagWaitMode) = cellSpursEventFlagWait;
int (*const p_ts)(const CellSpursEventFlag *, CellSpursTaskset **) = cellSpursEventFlagGetTasksetAddress;
int (*const p_bar)(CellSpursTaskset *, CellSpursBarrier *, unsigned int) = cellSpursBarrierInitialize;
int (*const p_sem)(CellSpursTaskset *, CellSpursSemaphore *, int) = cellSpursSemaphoreInitialize;
int (*const p_q)(CellSpursQueue *, const void *) = cellSpursQueuePush;
int (*const p_ec)(CellSpursTaskExitCode *, int *) = cellSpursTaskExitCodeGet;

int probe_ppu(CellSpurs *spurs, CellSpursTaskset *taskset, CellSpursEventFlag *ef,
              CellSpursBarrier *bar, CellSpursSemaphore *sem, CellSpursQueue *q,
              void *buf)
{
    int rc = CELL_OK;
    uint16_t bits = 1;
    rc |= cellSpursEventFlagInitialize(taskset, ef, CELL_SPURS_EVENT_FLAG_CLEAR_AUTO,
                                       CELL_SPURS_EVENT_FLAG_SPU2PPU);
    rc |= cellSpursEventFlagInitializeIWL(spurs, ef, CELL_SPURS_EVENT_FLAG_CLEAR_MANUAL,
                                          CELL_SPURS_EVENT_FLAG_ANY2ANY);
    rc |= cellSpursEventFlagAttachLv2EventQueue(ef);
    rc |= cellSpursEventFlagWait(ef, &bits, CELL_SPURS_EVENT_FLAG_OR);
    rc |= cellSpursBarrierInitialize(taskset, bar, 2);
    rc |= cellSpursSemaphoreInitializeIWL(spurs, sem, 1);
    rc |= cellSpursQueueInitialize(taskset, q, buf, 16, 4, CELL_SPURS_QUEUE_PPU2SPU);
    rc |= cellSpursQueueTryPop(q, buf);
#ifdef __cplusplus
    cell::Spurs::EventFlag *cef = static_cast<cell::Spurs::EventFlag *>(ef);
    rc |= cef->set(bits);
    rc |= cell::Spurs::Barrier::initialize(taskset, bar, 2);
    rc |= cell::Spurs::Semaphore::initialize(taskset, sem, 1);
    rc |= static_cast<cell::Spurs::Queue *>(q)->push(buf);
#endif
    return rc;
}
EOF
for abi in "" -mlp64; do
    for lang in c c++; do
        drv="$ppucc"; std=-std=gnu99
        [ "$lang" = c++ ] && { drv="$ppucxx"; std=-std=c++17; }
        if "$drv" -x "$lang" $std $abi -O2 -Wall -Wextra -Werror -isystem "$sdk/ppu/include" \
                -c "$work/ppu.cpp" -o "$work/ppu.o" > "$work/e.log" 2>&1; then
            ok "PPU pointer API unchanged ($lang ${abi:-ilp32})"
        else
            fail "PPU pointer API ($lang ${abi:-ilp32}): $(grep -m1 -E 'error' "$work/e.log")"
        fi
    done
done

[ "$status" -eq 0 ] && echo "spurs-spu-header-surface: PASS"
exit $status
