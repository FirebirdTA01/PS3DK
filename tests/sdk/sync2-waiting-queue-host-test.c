/* Host test of the SPU libsync2 waiting queue (sdk/libsync2/src/
 * waiting_queue.c) over the simulated MFC (tests/sdk/fixtures/spu-mfc-mock).
 * Built and run by tests/sdk/sync2-waiting-queue-host-test.sh.
 *
 * The queue source is compiled twice under different names, A_* and B_*,
 * so two SPUs with separate local stores share one queue: A enters (the
 * waiter), B wakes (the waker).  Each scenario runs thousands of steps on a
 * queue of 2 and of 3 entries, so both indices wrap and both phase bits
 * flip hundreds of times while entries are reused:
 *
 *   enter      A enters with a free waiter slot; it must block (its
 *              waitSignal runs) and nothing is signalled
 *   wake       B wakes the oldest blocked waiter; exactly that waiter's
 *              receiver is signalled (FIFO)
 *   early      with no waiter blocked, A announces itself and B's wake-up
 *              runs before A claims an entry (the mock's before-GETLLAR
 *              hook): B finds the entry unwritten and leaves the E marker;
 *              A then claims it, sees E and does not block, and nobody is
 *              signalled
 *
 * After every step both in-progress counts are back to 0.  The scenarios
 * run again with every other conditional store lost, so every retry path
 * of enter and wake-up is taken.
 *
 * Limits: DMA completes synchronously and the two sides never overlap
 * inside one reservation; host byte order (the queue is only touched by
 * this code, so no PPU layout is checked here).
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cell/sync2/thread_types.h>
#include "mfc_mock.h"

typedef int (*wait_fn)(CellSync2SignalReceiverId, CellSync2ObjectTypeId, uint64_t, uint64_t);

int A_enter(uint32_t eaQueue, uint16_t threadTypeId, uint64_t receiver, wait_fn waitSignal,
            CellSync2ObjectTypeId objectType, uint64_t eaObject, uint64_t callbackArg, unsigned int dmaTag);
int B_wakeup(uint32_t eaQueue, CellSync2Notifier *const *notifiers, unsigned int numNotifier,
             unsigned int dmaTag);

#define QUEUE  0x1000u
#define BUFFER 0x2000u
#define TYPE_TASK 4u

typedef struct {
    uint16_t wake_ctl, wake_idx, enter_ctl, enter_idx;
    uint32_t buffer;
    uint16_t entries;
} header;

static int failures, checks;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        ++checks;                                                           \
        if (!(cond)) {                                                      \
            ++failures;                                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

/* a protocol fault can leave the queue code spinning: fail instead of hanging */
static void on_timeout(int sig)
{
    (void)sig;
    static const char msg[] = "FAIL: the waiting queue did not finish within 60 s (spinning)\n";
    if (write(1, msg, sizeof msg - 1) < 0) { /* nothing more to do */ }
    _exit(1);
}

void sync2_host_halt(void)
{
    printf("FAIL: the queue code halted\n");
    exit(2);
}

static header *hdr(void)
{
    return (header *)(mfc_mock_memory + QUEUE);
}

/* ---- callbacks ------------------------------------------------------ */

static unsigned long s_waits;
static uint64_t s_waitReceiver;

static int wait_cb(CellSync2SignalReceiverId receiver, CellSync2ObjectTypeId type, uint64_t ea, uint64_t arg)
{
    (void)type; (void)ea; (void)arg;
    ++s_waits;
    s_waitReceiver = receiver;
    return 0;
}

static uint64_t s_sent[1u << 16];
static unsigned long s_nsent;

static int send_cb(CellSync2SignalReceiverId receiver, uint64_t arg)
{
    (void)arg;
    s_sent[s_nsent++ & 0xffff] = receiver;
    return 0;
}

static CellSync2Notifier s_notifier = { TYPE_TASK, send_cb, 0 };
static CellSync2Notifier *const s_table[1] = { &s_notifier };

/* ---- the early wake: B runs before A reserves its entry -------------- */

static int s_armed, s_earlyWakeRc;

static void on_reserve(uint64_t line_ea)
{
    if (s_armed && line_ea >= BUFFER) {
        s_armed = 0;
        s_earlyWakeRc = B_wakeup(QUEUE, s_table, 1, 3);
    }
}

/* ---- scenario ------------------------------------------------------- */

static uint32_t s_rand = 12345;

static uint32_t next_rand(void)
{
    s_rand = s_rand * 1103515245u + 12345u;
    return s_rand >> 16;
}

static void scenario(uint16_t entries, unsigned steps)
{
    uint64_t pending[8];
    unsigned npending = 0, head = 0;
    unsigned long enters = 0, wakes = 0, earlies = 0, enterFlips = 0, wakeFlips = 0;
    uint64_t receiver = 0x10000;

    memset(mfc_mock_memory + QUEUE, 0, 0x2000);
    hdr()->wake_ctl = 0x8000;
    hdr()->enter_ctl = 0x8000;
    hdr()->buffer = BUFFER;
    hdr()->entries = entries;
    mfc_mock_on_reserve = on_reserve;

    for (unsigned step = 0; step < steps; ++step) {
        const uint16_t enterPhase = hdr()->enter_ctl & 0x8000, wakePhase = hdr()->wake_ctl & 0x8000;
        const unsigned long waitsBefore = s_waits, sentBefore = s_nsent;
        const uint32_t r = next_rand() % 8;

        if (npending == 0 && r < 2) {
            /* early wake-up: B runs between A's announce and its claim */
            ++receiver;
            s_armed = 1;
            s_earlyWakeRc = -1;
            int rc = A_enter(QUEUE, TYPE_TASK, receiver, wait_cb, CELL_SYNC2_OBJECT_TYPE_MUTUEX, 0, 0, 3);
            CHECK(rc == 0, "early: enter rc %d", rc);
            CHECK(s_armed == 0 && s_earlyWakeRc == 0, "early: wake-up ran inside enter (rc %d)", s_earlyWakeRc);
            CHECK(s_waits == waitsBefore, "early: the waiter must not block (step %u)", step);
            CHECK(s_nsent == sentBefore, "early: nobody is signalled (step %u)", step);
            ++earlies;
        } else if (npending + 1 < entries && (npending == 0 || r < 5)) {
            ++receiver;
            int rc = A_enter(QUEUE, TYPE_TASK, receiver, wait_cb, CELL_SYNC2_OBJECT_TYPE_MUTUEX, 0, 0, 3);
            CHECK(rc == 0, "enter rc %d", rc);
            CHECK(s_waits == waitsBefore + 1 && s_waitReceiver == receiver, "enter: blocks with its receiver (step %u)",
                  step);
            CHECK(s_nsent == sentBefore, "enter: nobody is signalled (step %u)", step);
            pending[(head + npending++) % 8] = receiver;
            ++enters;
        } else {
            int rc = B_wakeup(QUEUE, s_table, 1, 3);
            CHECK(rc == 0, "wake rc %d", rc);
            CHECK(s_nsent == sentBefore + 1, "wake: exactly one signal (step %u)", step);
            if (s_nsent == sentBefore + 1)
                CHECK(s_sent[sentBefore & 0xffff] == pending[head],
                      "wake: FIFO order (step %u): got %llx want %llx", step,
                      (unsigned long long)s_sent[sentBefore & 0xffff], (unsigned long long)pending[head]);
            head = (head + 1) % 8;
            --npending;
            ++wakes;
        }
        CHECK((hdr()->enter_ctl & 0x7fff) == 0 && (hdr()->wake_ctl & 0x7fff) == 0,
              "in-progress counts return to 0 (step %u): enter %04x wake %04x", step, hdr()->enter_ctl,
              hdr()->wake_ctl);
        enterFlips += (hdr()->enter_ctl & 0x8000) != enterPhase;
        wakeFlips += (hdr()->wake_ctl & 0x8000) != wakePhase;
        if (failures > 20)
            break;
    }
    /* drain */
    while (npending) {
        const unsigned long sentBefore = s_nsent;
        CHECK(B_wakeup(QUEUE, s_table, 1, 3) == 0, "drain wake");
        CHECK(s_nsent == sentBefore + 1 && s_sent[sentBefore & 0xffff] == pending[head], "drain: FIFO order");
        head = (head + 1) % 8;
        --npending;
    }
    CHECK(hdr()->enter_idx == hdr()->wake_idx && (hdr()->enter_ctl & 0x8000) == (hdr()->wake_ctl & 0x8000),
          "drained: enter and wake positions meet");
    CHECK(enterFlips >= 100 && wakeFlips >= 100, "phase flips: enter %lu wake %lu", enterFlips, wakeFlips);
    CHECK(earlies >= 100, "early wake-ups exercised: %lu", earlies);
    printf("  entries %u: %lu enters, %lu wakes, %lu early wake-ups, %lu/%lu phase flips, %lu putllc lost\n",
           (unsigned)entries, enters, wakes, earlies, enterFlips, wakeFlips, mfc_mock_putllc_lost);
    mfc_mock_on_reserve = 0;
}

int main(void)
{
    signal(SIGALRM, on_timeout);
    alarm(60);
    mfc_mock_swap32 = 0;
    printf("sync2 waiting queue (host):\n");
    scenario(2, 3000);
    scenario(3, 3000);
    printf("  with every other conditional store lost:\n");
    mfc_mock_on_putllc = mfc_mock_lose_alternate;
    scenario(2, 3000);
    scenario(3, 3000);
    mfc_mock_on_putllc = 0;
    printf("sync2-waiting-queue-host: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
