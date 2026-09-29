/* sync2-objects: SPURS tasks and PPU threads sharing cellSync2
 * objects.  The PPU side runs in the libsync2 system module; the SPU side
 * is libsync2.a.  Each row prints "SYNC2 <row> PASS" or the first check
 * that failed, and the program ends with "SYNC2_OBJECTS DONE passed=N of M".
 *
 *   mutex      a task holds and hands over to a PPU thread; the PPU holds and
 *              hands over to a task; a task hands over to another task;
 *              TryLock on a held mutex is BUSY
 *   semaphore  a task waits for 1 and gets it from a PPU release; a PPU
 *              thread waits and gets it from the task; TryAcquire past the
 *              count is AGAIN; GetCount from the SPU; a lone waiter
 *              collects 3 over two releases
 *   cond       a task waits and is signalled by the PPU; a PPU thread waits
 *              and is signalled by a task; SignalAll wakes two tasks
 *   queue      a task pops from an empty queue and gets the PPU's push; a
 *              task pushes into a full queue and completes when the PPU
 *              pops; elements keep their order; TryPop on empty is AGAIN;
 *              GetSize / GetDepth from the SPU
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cell/sysmodule.h>
#include <cell/spurs.h>
#include <cell/sync2.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include "common.h"
#include "sync2_task_bin.h"

#define TYPES (CELL_SYNC2_THREAD_TYPE_PPU_THREAD | CELL_SYNC2_THREAD_TYPE_SPURS_TASK)

alignas(128) static volatile box s_box[2];
alignas(128) static CellSync2Mutex s_mutex;
alignas(128) static CellSync2Semaphore s_sem;
alignas(128) static CellSync2Cond s_cond;
alignas(128) static CellSync2Queue s_queue;
static cell::Spurs::Spurs2 *s_spurs;
static cell::Spurs::Taskset *s_ts;
static volatile int s_helper;
static const char *s_row;
static int s_failed;

static uint64_t ea(const volatile void *p) { return reinterpret_cast<uintptr_t>(p); }

static bool check(bool ok, const char *what, unsigned got, unsigned want)
{
    if (!ok && !s_failed) {
        std::printf("SYNC2 %s FAIL %s: got %#x want %#x\n", s_row, what, got, want);
        s_failed = 1;
    }
    return ok;
}

template <typename F> static bool wait_for(F pred, int ms = 5000)
{
    for (int i = 0; i < ms; ++i) {
        __sync_synchronize();
        if (pred())
            return true;
        sys_timer_usleep(1000);
    }
    return false;
}

/* Launch the task image with kind `kind` on box `i`. */
static int start(int i, uint32_t kind, uint64_t obj, uint32_t n = 0)
{
    volatile box &b = s_box[i];
    b.state = 0; b.rc = 0; b.v0 = 0; b.v1 = 0; b.go = 0;
    b.obj = obj; b.n = n; b.mutex = ea(&s_mutex);
    __sync_synchronize();
    CellSpursTaskLsPattern ls = { { CELL_SPURS_TASK_TOP_MASK, 0xffffffffU, 0xffffffffU, 0xffffffffU } };
    void *ctx = ::aligned_alloc(CELL_SPURS_TASK_CONTEXT_ALIGN, CELL_SPURS_TASK_CONTEXT_SIZE_ALL);
    CellSpursTaskArgument arg;
    CellSpursTaskId id;
    std::memset(&arg, 0, sizeof arg);
    arg.u64[0] = ea(&b);
    arg.u32[3] = kind;
    return s_ts->createTask(&id, sync2_task_bin, ctx, CELL_SPURS_TASK_CONTEXT_SIZE_ALL, &ls, &arg);
}

/* Wait until box `i` reports `state` (or an error). */
static bool reported(int i, uint32_t state, const char *what)
{
    volatile box &b = s_box[i];
    bool ok = wait_for([&] { return b.state == state || (b.state & 0x80000000u); }) && b.state == state;
    return check(ok, what, b.state, state) && check(b.rc == 0 || state == 0, what, b.rc, 0);
}

static void end_row()
{
    if (!s_failed)
        std::printf("SYNC2 %s PASS\n", s_row);
    std::fflush(stdout);
}

/* ---- PPU helper threads ---- */

static void mutex_helper(uint64_t)
{
    s_helper = 1;
    int rc = cellSync2MutexLock(&s_mutex, nullptr);
    if (!rc)
        rc = cellSync2MutexUnlock(&s_mutex, nullptr);
    s_helper = rc ? 0x100 | rc : 2;
    sys_ppu_thread_exit(0);
}

static void sem_helper(uint64_t)
{
    s_helper = 1;
    int rc = cellSync2SemaphoreAcquire(&s_sem, 1, nullptr);
    s_helper = rc ? 0x100 | rc : 2;
    sys_ppu_thread_exit(0);
}

static void cond_helper(uint64_t)
{
    s_helper = 1;
    int rc = cellSync2MutexLock(&s_mutex, nullptr);
    if (!rc) {
        s_helper = 3;
        rc = cellSync2CondWait(&s_cond, nullptr);
    }
    if (!rc)
        rc = cellSync2MutexUnlock(&s_mutex, nullptr);
    s_helper = rc ? 0x100 | rc : 2;
    sys_ppu_thread_exit(0);
}

static sys_ppu_thread_t helper(void (*fn)(uint64_t))
{
    sys_ppu_thread_t t;
    s_helper = 0;
    sys_ppu_thread_create(&t, fn, 0, 1000, 0x4000, SYS_PPU_THREAD_CREATE_JOINABLE, "sync2-helper");
    return t;
}

static void join(sys_ppu_thread_t t)
{
    uint64_t ex;
    sys_ppu_thread_join(t, &ex);
}

static void *buffer(size_t size)
{
    void *p = ::aligned_alloc(128, (size + 127) & ~size_t(127));
    std::memset(p, 0, size);
    return p;
}

/* ---- rows ---- */

static void row_mutex()
{
    s_row = "mutex";
    s_failed = 0;
    CellSync2MutexAttribute attr;
    cellSync2MutexAttributeInitialize(&attr);
    attr.threadTypes = TYPES;
    attr.maxWaiters = 4;
    std::strcpy(attr.name, "sync2-mutex");
    size_t size;
    cellSync2MutexEstimateBufferSize(&attr, &size);
    int rc = cellSync2MutexInitialize(&s_mutex, buffer(size), &attr);
    if (!check(rc == 0, "initialize", rc, 0))
        return end_row();

    /* task holds, PPU thread waits, the task hands over */
    if (!check(start(0, K_MUTEX_HOLD, ea(&s_mutex)) == 0, "launch", 1, 0) || !reported(0, 1, "task locked"))
        return end_row();
    sys_ppu_thread_t t = helper(mutex_helper);
    sys_timer_usleep(50000);
    check(s_helper == 1, "PPU thread waits", s_helper, 1);
    s_box[0].go = 1;
    join(t);
    reported(0, 2, "task unlocked");
    check(s_helper == 2, "PPU thread got the mutex", s_helper, 2);

    /* PPU holds, the task waits, the PPU hands over */
    check(cellSync2MutexLock(&s_mutex, nullptr) == 0, "PPU lock", 1, 0);
    start(0, K_MUTEX_LOCK, ea(&s_mutex));
    sys_timer_usleep(100000);
    check(s_box[0].state == 0, "task waits", s_box[0].state, 0);
    check(cellSync2MutexUnlock(&s_mutex, nullptr) == 0, "PPU unlock", 1, 0);
    reported(0, 2, "task got the mutex");

    /* one task hands over to another */
    start(0, K_MUTEX_HOLD, ea(&s_mutex));
    if (reported(0, 1, "task 0 locked")) {
        start(1, K_MUTEX_LOCK, ea(&s_mutex));
        sys_timer_usleep(100000);
        check(s_box[1].state == 0, "task 1 waits", s_box[1].state, 0);
        s_box[0].go = 1;
        reported(1, 2, "task 1 got the mutex");
        reported(0, 2, "task 0 unlocked");
    }

    /* TryLock on a held mutex */
    check(cellSync2MutexLock(&s_mutex, nullptr) == 0, "PPU lock", 1, 0);
    start(0, K_MUTEX_TRY, ea(&s_mutex));
    wait_for([] { return s_box[0].state == 2; });
    check(s_box[0].rc == static_cast<uint32_t>(CELL_SYNC2_ERROR_BUSY), "TryLock on a held mutex", s_box[0].rc, CELL_SYNC2_ERROR_BUSY);
    cellSync2MutexUnlock(&s_mutex, nullptr);
    end_row();
}

static void row_semaphore()
{
    s_row = "semaphore";
    s_failed = 0;
    CellSync2SemaphoreAttribute attr;
    cellSync2SemaphoreAttributeInitialize(&attr);
    attr.threadTypes = TYPES;
    attr.maxWaiters = 4;
    std::strcpy(attr.name, "sync2-sem");
    size_t size;
    cellSync2SemaphoreEstimateBufferSize(&attr, &size);
    int rc = cellSync2SemaphoreInitialize(&s_sem, buffer(size), 0, &attr);
    if (!check(rc == 0, "initialize", rc, 0))
        return end_row();

    start(0, K_SEM_ACQ, ea(&s_sem), 1);
    sys_timer_usleep(100000);
    check(s_box[0].state == 0, "task waits", s_box[0].state, 0);
    check(cellSync2SemaphoreRelease(&s_sem, 1, nullptr) == 0, "PPU release", 1, 0);
    if (reported(0, 1, "task acquired")) {
        sys_ppu_thread_t t = helper(sem_helper);
        sys_timer_usleep(50000);
        check(s_helper == 1, "PPU thread waits", s_helper, 1);
        s_box[0].go = 1;
        join(t);
        reported(0, 2, "task released");
        check(s_helper == 2, "PPU thread acquired", s_helper, 2);
    }

    check(cellSync2SemaphoreRelease(&s_sem, 1, nullptr) == 0, "PPU release", 1, 0);
    start(0, K_SEM_TRY, ea(&s_sem), 2);
    wait_for([] { return s_box[0].state == 2; });
    check(s_box[0].rc == static_cast<uint32_t>(CELL_SYNC2_ERROR_AGAIN), "TryAcquire 2 of 1", s_box[0].rc, CELL_SYNC2_ERROR_AGAIN);
    start(0, K_SEM_COUNT, ea(&s_sem));
    reported(0, 2, "GetCount");
    check(s_box[0].v0 == 1, "count", s_box[0].v0, 1);
    cellSync2SemaphoreFinalize(&s_sem);

    /* one waiter asking for 3: the firmware takes no buffer for it */
    attr.maxWaiters = 1;
    rc = cellSync2SemaphoreInitialize(&s_sem, nullptr, 0, &attr);
    if (check(rc == 0, "one-waiter initialize", rc, 0)) {
        start(0, K_SEM_ACQ, ea(&s_sem), 3);
        sys_timer_usleep(100000);
        check(cellSync2SemaphoreRelease(&s_sem, 1, nullptr) == 0, "release 1", 1, 0);
        sys_timer_usleep(50000);
        check(s_box[0].state == 0, "task still waits after 1 of 3", s_box[0].state, 0);
        check(cellSync2SemaphoreRelease(&s_sem, 2, nullptr) == 0, "release 2", 1, 0);
        if (reported(0, 1, "task acquired 3")) {
            s_box[0].go = 1;
            reported(0, 2, "task released 3");
            int count = -1;
            cellSync2SemaphoreGetCount(&s_sem, &count);
            check(count == 3, "count after the task released", count, 3);
        }
        cellSync2SemaphoreFinalize(&s_sem);
    }
    end_row();
}

static void row_cond()
{
    s_row = "cond";
    s_failed = 0;
    CellSync2CondAttribute attr;
    cellSync2CondAttributeInitialize(&attr);
    attr.maxWaiters = 4;
    std::strcpy(attr.name, "sync2-cond");
    size_t size;
    cellSync2CondEstimateBufferSize(&attr, &size);
    int rc = cellSync2CondInitialize(&s_cond, &s_mutex, buffer(size), &attr);
    if (!check(rc == 0, "initialize", rc, 0))
        return end_row();

    /* a task waits, the PPU signals */
    start(0, K_COND_WAIT, ea(&s_cond));
    if (reported(0, 1, "task locked")) {
        sys_timer_usleep(100000);
        check(cellSync2MutexLock(&s_mutex, nullptr) == 0, "PPU lock", 1, 0);
        check(cellSync2CondSignal(&s_cond, nullptr) == 0, "PPU signal", 1, 0);
        check(cellSync2MutexUnlock(&s_mutex, nullptr) == 0, "PPU unlock", 1, 0);
        reported(0, 2, "task woke and unlocked");
    }

    /* a PPU thread waits, a task signals */
    sys_ppu_thread_t t = helper(cond_helper);
    if (check(wait_for([] { return s_helper == 3; }), "PPU thread locked", s_helper, 3)) {
        sys_timer_usleep(50000);
        start(0, K_COND_SIGNAL, ea(&s_cond), 0);
        reported(0, 2, "task signalled");
    }
    join(t);
    check(s_helper == 2, "PPU thread woke", s_helper, 2);

    /* two tasks wait, one SignalAll */
    start(0, K_COND_WAIT, ea(&s_cond));
    start(1, K_COND_WAIT, ea(&s_cond));
    if (reported(0, 1, "task 0 locked") && reported(1, 1, "task 1 locked")) {
        sys_timer_usleep(100000);
        check(cellSync2MutexLock(&s_mutex, nullptr) == 0, "PPU lock", 1, 0);
        check(cellSync2CondSignalAll(&s_cond, nullptr) == 0, "PPU signal all", 1, 0);
        check(cellSync2MutexUnlock(&s_mutex, nullptr) == 0, "PPU unlock", 1, 0);
        reported(0, 2, "task 0 woke");
        reported(1, 2, "task 1 woke");
    }
    cellSync2CondFinalize(&s_cond);
    end_row();
}

static void row_queue()
{
    s_row = "queue";
    s_failed = 0;
    CellSync2QueueAttribute attr;
    cellSync2QueueAttributeInitialize(&attr);
    attr.threadTypes = TYPES;
    attr.elementSize = 16;
    attr.depth = 2;
    attr.maxPushWaiters = 2;
    attr.maxPopWaiters = 2;
    std::strcpy(attr.name, "sync2-queue");
    size_t size;
    cellSync2QueueEstimateBufferSize(&attr, &size);
    int rc = cellSync2QueueInitialize(&s_queue, buffer(size), &attr);
    if (!check(rc == 0, "initialize", rc, 0))
        return end_row();
    alignas(16) uint32_t e[4];
    auto push = [&](uint32_t v) {
        e[0] = v; e[1] = v + 1; e[2] = v + 2; e[3] = v + 3;
        return cellSync2QueuePush(&s_queue, e, nullptr);
    };
    auto pop = [&]() {
        e[0] = 0xdead;
        int r = cellSync2QueuePop(&s_queue, e, nullptr);
        return r ? 0xbad0000u | (r & 0xff) : e[0];
    };

    /* a task pops from an empty queue */
    start(0, K_Q_POP, ea(&s_queue));
    sys_timer_usleep(100000);
    check(s_box[0].state == 0, "task waits to pop", s_box[0].state, 0);
    check(push(0x11) == 0, "PPU push", 1, 0);
    if (reported(0, 2, "task popped")) {
        check(s_box[0].v0 == 0x11, "element", s_box[0].v0, 0x11);
        check(s_box[0].v1 == 0x14, "element's last word", s_box[0].v1, 0x14);
    }

    /* a task pushes into a full queue */
    check(push(0x21) == 0 && push(0x22) == 0, "PPU fills the queue", 1, 0);
    start(0, K_Q_INFO, ea(&s_queue));
    if (reported(0, 2, "GetSize / GetDepth")) {
        check(s_box[0].v0 == 2, "size", s_box[0].v0, 2);
        check(s_box[0].v1 == 2, "depth", s_box[0].v1, 2);
    }
    start(0, K_Q_PUSH, ea(&s_queue), 0x23);
    sys_timer_usleep(100000);
    check(s_box[0].state == 0, "task waits to push", s_box[0].state, 0);
    unsigned v = pop();
    check(v == 0x21, "first element", v, 0x21);
    reported(0, 2, "task pushed");
    v = pop();
    check(v == 0x22, "second element", v, 0x22);
    v = pop();
    check(v == 0x23, "the task's element", v, 0x23);

    /* TryPop on an empty queue */
    start(0, K_Q_TRYPOP, ea(&s_queue));
    wait_for([] { return s_box[0].state == 2; });
    check(s_box[0].rc == static_cast<uint32_t>(CELL_SYNC2_ERROR_AGAIN), "TryPop on empty", s_box[0].rc, CELL_SYNC2_ERROR_AGAIN);
    cellSync2QueueFinalize(&s_queue);
    end_row();
}

int main()
{
    int rc = cellSysmoduleLoadModule(CELL_SYSMODULE_SYNC2);
    if (rc) {
        std::printf("SYNC2_OBJECTS load sync2 module failed %#x\n", rc);
        return 1;
    }
    s_spurs = new cell::Spurs::Spurs2;
    cell::Spurs::SpursAttribute attr;
    rc = cell::Spurs::SpursAttribute::initialize(&attr, 4, 100, 2, false);
    if (!rc) rc = attr.setNamePrefix("Sync2", 5);
    if (!rc) rc = attr.setSpuThreadGroupType(SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT);
    if (!rc) rc = cell::Spurs::Spurs2::initialize(s_spurs, &attr);
    static const uint8_t prio[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    s_ts = static_cast<cell::Spurs::Taskset *>(::aligned_alloc(CELL_SPURS_TASKSET_ALIGN, CELL_SPURS_TASKSET_SIZE));
    if (!rc) rc = cell::Spurs::Taskset::create(s_spurs, s_ts, 0, prio, 4);
    if (rc) {
        std::printf("SYNC2_OBJECTS SPURS setup failed %#x\n", rc);
        return 1;
    }

    void (*rows[])() = { row_mutex, row_semaphore, row_cond, row_queue };
    int passed = 0, total = sizeof rows / sizeof rows[0];
    for (auto row : rows) {
        row();
        passed += !s_failed;
    }

    s_ts->shutdown();
    s_ts->join();
    s_spurs->finalize();
    std::printf("SYNC2_OBJECTS DONE passed=%d of %d\n", passed, total);
    std::fflush(stdout);
    return passed == total ? 0 : 1;
}
