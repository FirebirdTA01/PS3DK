/*
 * header-path-forwarders-test.c
 *
 * The ten forwarder headers (cell/sync/{error,lfqueue,mutex,queue,rwm}.h,
 * cell/rtc/{error,rtcsvc}.h, cell/ssl/error.h, cell/keyboard/libkb.h,
 * cell/mouse/libmouse.h) each forward to the one header that already
 * carries the named surface.  Compile this TU against a PPU toolchain
 * and:
 *   (a) every named symbol resolves (a forwarder pointing at the wrong
 *       header, or at no header, fails to compile here);
 *   (b) every "Check:" equality the brief called out still holds.
 *
 * Pin strategy:
 *   - a type: a sizeof() reference needs no call;
 *   - a macro: a use that depends on its expansion;
 *   - a function: assign to a pointer of the right signature.
 *
 * header-path-forwarders-test.sh drives the compiler matrix
 * (gnu99, c11, c++17; ILP32 and LP64; -Wall -Wextra -Werror) and the
 * self-check.  A bad forwarder (one pointed at an unrelated header) must
 * fail to compile on exactly the row that references the target symbol.
 */
#include <stddef.h>
#include <stdint.h>

#include <cell/sync/error.h>
#include <cell/sync/lfqueue.h>
#include <cell/sync/mutex.h>
#include <cell/sync/queue.h>
#include <cell/sync/rwm.h>
#include <cell/rtc/error.h>
#include <cell/rtc/rtcsvc.h>
#include <cell/ssl/error.h>
#include <cell/keyboard/libkb.h>
#include <cell/mouse/libmouse.h>

#ifdef __cplusplus
#define STATIC_ASSERT static_assert
#else
#define STATIC_ASSERT _Static_assert
#endif

/* Pin one symbol from each forwarder.  A forwarder pointing at the wrong
 * target header (sync/rtc/ssl/keyboard/mouse are all distinct surfaces)
 * makes the corresponding symbol undeclared and this file fails to
 * compile. */
typedef int cell_sync_barrier_fp(CellSyncBarrier *, uint16_t);
typedef int cell_sync_mutex_fp(CellSyncMutex *);
typedef int cell_sync_rwm_fp(CellSyncRwm *, void *);
typedef int cell_sync_queue_fp(CellSyncQueue *, const void *);
typedef int cell_sync_lfqueue_fp(CellSyncLFQueue *);
typedef int cell_rtc_tick_fp(CellRtcTick *);
typedef int cell_ssl_init_fp(void *, size_t);
typedef int32_t cell_kb_read_fp(uint32_t, CellKbData *);
typedef int32_t cell_mouse_info_fp(CellMouseInfo *);

/* one type reference per sync/rtc/ssl forwarder (sizeof needs no body) */
static volatile size_t pin_cell_sync_barrier = sizeof(CellSyncBarrier);
static volatile size_t pin_cell_sync_mutex   = sizeof(CellSyncMutex);
static volatile size_t pin_cell_sync_rwm     = sizeof(CellSyncRwm);
static volatile size_t pin_cell_sync_queue   = sizeof(CellSyncQueue);
static volatile size_t pin_cell_sync_lfqueue = sizeof(CellSyncLFQueue);
static volatile size_t pin_cell_rtc_tick     = sizeof(CellRtcTick);

/* one function reference via a function-pointer of the right shape */
static cell_sync_mutex_fp *volatile p_cell_sync_mutex = &cellSyncMutexInitialize;
static cell_sync_rwm_fp *volatile p_cell_sync_rwm = &cellSyncRwmRead;
static cell_sync_lfqueue_fp *volatile p_cell_sync_lfqueue = &cellSyncLFQueueClear;
static cell_rtc_tick_fp *volatile p_cell_rtc_tick = &cellRtcGetCurrentTick;
static cell_ssl_init_fp *volatile p_cell_ssl_init = &cellSslInit;
static cell_kb_read_fp *volatile p_cell_kb_read = &cellKbRead;
static cell_mouse_info_fp *volatile p_cell_mouse_info = &cellMouseGetInfo;
static cell_sync_barrier_fp *volatile p_cell_sync_barrier = &cellSyncBarrierInitialize;
static cell_sync_queue_fp *volatile p_cell_sync_queue = &cellSyncQueuePush;

/* one macro reference: the three error constructors + READ_FAILED */
static volatile int cell_sync_error_again = (int)CELL_SYNC_ERROR_AGAIN;
static volatile int cell_sync_make_0x01   = CELL_ERROR_MAKE_SYNC_ERROR(0x01u);
static volatile int cell_rtc_make_1       = CELL_RTC_MAKE_ERROR(1);
static volatile int cell_ssl_make_1       = CELL_SSL_MAKE_ERROR(1);
static volatile int cell_ssl_read_failed  = CELL_SSL_ERROR_READ_FAILED;

int main(void)
{
    /* Reference the pins so -Werror -Wextra does not drop them. */
    (void)pin_cell_sync_barrier; (void)pin_cell_sync_mutex;
    (void)pin_cell_sync_rwm;     (void)pin_cell_sync_queue;
    (void)pin_cell_sync_lfqueue; (void)pin_cell_rtc_tick;
    (void)p_cell_sync_mutex;       (void)p_cell_sync_rwm;
    (void)p_cell_sync_lfqueue;     (void)p_cell_rtc_tick;
    (void)p_cell_ssl_init;         (void)p_cell_kb_read;
    (void)p_cell_mouse_info;       (void)p_cell_sync_barrier;
    (void)p_cell_sync_queue;
    (void)cell_sync_error_again;   (void)cell_sync_make_0x01;
    (void)cell_rtc_make_1;         (void)cell_ssl_make_1;
    (void)cell_ssl_read_failed;
    return 0;
}

/* The four equalities the brief called out. */
STATIC_ASSERT(CELL_ERROR_MAKE_SYNC_ERROR(0x01u) == (int)CELL_SYNC_ERROR_AGAIN,
              "CELL_ERROR_MAKE_SYNC_ERROR(0x01) == CELL_SYNC_ERROR_AGAIN (0x80410101)");
STATIC_ASSERT(CELL_RTC_MAKE_ERROR(1) == (int)CELL_RTC_ERROR_NOT_INITIALIZED,
              "CELL_RTC_MAKE_ERROR(1) == CELL_RTC_ERROR_NOT_INITIALIZED (0x80010601)");
STATIC_ASSERT(CELL_SSL_MAKE_ERROR(1) == (int)CELL_SSL_ERROR_NOT_INITIALIZED,
              "CELL_SSL_MAKE_ERROR(1) == CELL_SSL_ERROR_NOT_INITIALIZED (0x80740001)");
STATIC_ASSERT(CELL_SSL_ERROR_READ_FAILED == (int)0x80740009,
              "CELL_SSL_ERROR_READ_FAILED == 0x80740009");
