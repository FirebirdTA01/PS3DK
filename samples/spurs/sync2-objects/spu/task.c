/* sync2-objects SPU task: runs one sync2 step against an object the PPU
 * program owns and reports through its box. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/spurs/task.h>
#include <cell/sync2.h>
#include "../source/common.h"

#define CFG (&gCellSync2ThreadConfigSpursTask)
#define TAG 2

static box b;
static uint32_t elem[4] __attribute__((aligned(16)));

static void dma_wait(void)
{
    mfc_write_tag_mask(1u << 1);
    mfc_read_tag_status_all();
}

static void report(uint64_t ea, uint32_t state, int rc, uint32_t v0, uint32_t v1)
{
    b.state = state;
    b.rc = (uint32_t)rc;
    b.v0 = v0;
    b.v1 = v1;
    mfc_put(&b.state, ea + 64, 16, 1, 0, 0);
    dma_wait();
}

static uint32_t get_go(uint64_t ea)
{
    mfc_get(&b, ea, 64, 1, 0, 0);
    dma_wait();
    return b.go;
}

int cellSpursTaskMain(qword argTask, uint64_t argTaskset)
{
    struct { uint64_t box; uint32_t a2, kind; } arg __attribute__((aligned(16)));
    int rc;
    unsigned int u0 = 0, u1 = 0;
    int count = 0;
    (void)argTaskset;
    *(qword *)&arg = argTask;
    get_go(arg.box);
    const uint64_t obj = b.obj, mutex = b.mutex;
    const uint32_t n = b.n;

    switch (arg.kind) {
    case K_MUTEX_HOLD:
    case K_MUTEX_LOCK:
        rc = cellSync2MutexLock(obj, CFG, TAG);
        report(arg.box, rc ? 0x80000001u : 1u, rc, 0, 0);
        if (rc)
            return 1;
        if (arg.kind == K_MUTEX_HOLD)
            while (!get_go(arg.box))
                ;
        rc = cellSync2MutexUnlock(obj, CFG, TAG);
        report(arg.box, 2, rc, 0, 0);
        break;
    case K_MUTEX_TRY:
        rc = cellSync2MutexTryLock(obj, CFG, TAG);
        if (rc == 0)
            cellSync2MutexUnlock(obj, CFG, TAG);
        report(arg.box, 2, rc, 0, 0);
        break;
    case K_SEM_ACQ:
        rc = cellSync2SemaphoreAcquire(obj, n, CFG, TAG);
        report(arg.box, rc ? 0x80000001u : 1u, rc, 0, 0);
        if (rc)
            return 1;
        while (!get_go(arg.box))
            ;
        rc = cellSync2SemaphoreRelease(obj, n, CFG, TAG);
        report(arg.box, 2, rc, 0, 0);
        break;
    case K_SEM_TRY:
        rc = cellSync2SemaphoreTryAcquire(obj, n, CFG, TAG);
        report(arg.box, 2, rc, 0, 0);
        break;
    case K_SEM_COUNT:
        rc = cellSync2SemaphoreGetCount(obj, &count);
        report(arg.box, 2, rc, (uint32_t)count, 0);
        break;
    case K_COND_WAIT:
        rc = cellSync2MutexLock(mutex, CFG, TAG);
        report(arg.box, rc ? 0x80000001u : 1u, rc, 0, 0);
        if (rc)
            return 1;
        rc = cellSync2CondWait(obj, CFG, TAG);
        if (rc) {
            report(arg.box, 0x80000003u, rc, 0, 0);
            return 1;
        }
        rc = cellSync2MutexUnlock(mutex, CFG, TAG);
        report(arg.box, 2, rc, 0, 0);
        break;
    case K_COND_SIGNAL:
        rc = cellSync2MutexLock(mutex, CFG, TAG);
        if (!rc)
            rc = n ? cellSync2CondSignalAll(obj, CFG, TAG) : cellSync2CondSignal(obj, CFG, TAG);
        if (!rc)
            rc = cellSync2MutexUnlock(mutex, CFG, TAG);
        report(arg.box, 2, rc, 0, 0);
        break;
    case K_Q_POP:
        elem[0] = 0xdead;
        rc = cellSync2QueuePop(obj, elem, CFG, TAG);
        report(arg.box, 2, rc, elem[0], elem[3]);
        break;
    case K_Q_PUSH:
        elem[0] = n;
        elem[1] = n + 1;
        elem[2] = n + 2;
        elem[3] = n + 3;
        rc = cellSync2QueuePush(obj, elem, CFG, TAG);
        report(arg.box, 2, rc, 0, 0);
        break;
    case K_Q_TRYPOP:
        elem[0] = 0xdead;
        rc = cellSync2QueueTryPop(obj, elem, CFG, TAG);
        report(arg.box, 2, rc, elem[0], 0);
        break;
    case K_Q_INFO:
        rc = cellSync2QueueGetSize(obj, &u0);
        if (!rc)
            rc = cellSync2QueueGetDepth(obj, &u1);
        report(arg.box, 2, rc, u0, u1);
        break;
    default:
        report(arg.box, 0x800000ffu, -1, arg.kind, 0);
        return 1;
    }
    return 0;
}
