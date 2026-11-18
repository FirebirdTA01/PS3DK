/* Actual AUTO/core frame integration; real native wrap protocol on a host FIFO.
 * The device does not execute commands: this is owner accounting only. */
#define PS3TC_FIFO_OBSERVE 1
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include "auto_internal.h"
#include "fifo_internal.h"
#include "prof_test_hooks.h"
#include "ps3tc_prof_auto.h"
#include "ps3tc_fifo_wrap_protocol.h"
extern int auto_test_fifo_native;
extern volatile uint32_t auto_test_fifo_put;
extern const ps3tc_fifo_observer *auto_test_fifo_observer;
static uint64_t reports[2048][2];
static uint32_t labels[256], words[1024];
static gpp_ctx context;
static volatile uint32_t get;
static atomic_int race_ready, race_done, race_attaching, race_observed;
static void race_frame_probe(void)
{
#ifdef PS3DK_PROF_FIFO_NATIVE_ACCOUNT
    /* Exercise the metadata-admission seam without adopting the capture
     * window or assigning public frame IDs on this foreign thread. */
    ps3dk_prof__fifo_account_native_frame(0, 0, 0);
#endif
}
static void attach_gate(unsigned point, unsigned value)
{
    (void)value;
    if (point == 5) {
        assert(LD_ACQ(&g_fifo.state) == FS_ATTACHED && "FIFO frame metadata requires attached publication");
        return;
    }
    if (point != 3) return;
    /* Metadata is assigned but ATTACHED has not published it. Relaxed gates
     * schedule the test without manufacturing a happens-before edge for ctx. */
    atomic_store_explicit(&race_attaching, 1, memory_order_relaxed);
    while (!atomic_load_explicit(&race_observed, memory_order_relaxed)) { }
}
static void *frame_racer(void *unused)
{
    (void)unused;
    assert(ps3dk_prof_thread_begin("FIFO attach frame race") == 0);
    atomic_store_explicit(&race_ready, 1, memory_order_release);
    while (!atomic_load_explicit(&race_attaching, memory_order_relaxed)) race_frame_probe();
    assert(LD_ACQ(&g_fifo.state) == FS_ATTACHING);
    for (unsigned i = 0; i < 8; ++i) race_frame_probe();
    atomic_store_explicit(&race_observed, 1, memory_order_relaxed);
    while (!atomic_load_explicit(&race_done, memory_order_acquire)) race_frame_probe();
    ps3dk_prof_thread_end();
    return NULL;
}
#ifdef PS3DK_PROF_FIFO_END_KNOWN
static void *foreign_end_query(void *unused)
{
    (void)unused;
    assert(!ps3dk_prof__auto_fifo_end_known(&context, words + 1024, words + 1024));
    return NULL;
}
#endif
static void *report(uint32_t n) { return reports[n]; }
static void *label(uint32_t n) { return &labels[n]; }
static uint32_t offset(void) { return 4096u + (uint32_t)(context.current - context.begin) * 4u; }
static void publish(uint32_t off)
{
    uint32_t serial = ps3dk_prof__auto_publish_before((void *)&context);
    auto_test_fifo_put = off;
    ps3dk_prof__auto_publish_after((void *)&context, serial);
#ifdef PS3DK_PROF_FIFO_NATIVE_ACCOUNT
    ps3dk_prof__fifo_account_native_publish(&context, off);
#endif
}
static void flush(gpp_ctx *ctx) { assert(ctx == &context); publish(offset()); }
static uint64_t ld64(volatile uint64_t *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static uint32_t ld32(volatile uint32_t *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void st64(volatile uint64_t *p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static void st32(volatile uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static const gpp_dev device = {report, label, flush, ld64, st64, ld32, st32};
static void barrier(void *p) { (void)p; }
static uint64_t clock_now(void *p) { (void)p; return 1; }
static void pause_get(void *p, uint32_t want, uint32_t spins, unsigned phase, uint64_t elapsed)
{ (void)p; (void)spins; (void)phase; (void)elapsed; get = want; }
static ps3tc_fifo_obs_state observer;
static ps3tc_fifo_obs_wrap observed_wrap;
static void boundary(void *p, unsigned kind, uint64_t before, uint64_t after)
{ (void)p; ps3tc_fifo_obs_emit(&observer, kind, before, after, &observed_wrap); }
static void wrap(unsigned n)
{
    for (unsigned i = 0; i < n; ++i) *context.current++ = 0x00040000;
    observed_wrap = (ps3tc_fifo_obs_wrap){4096, offset(), 64, 0};
    ps3tc_fifo_port port = {&auto_test_fifo_put, &get, barrier, clock_now, pause_get, NULL, boundary, clock_now};
    boundary(NULL, 1, 1, 1);
    ps3tc_fifo_wrap_protocol(context.current, observed_wrap.tail_off, 4096, &port);
    context.current = context.begin;
    boundary(NULL, 6, 1, 1);
}
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);
int main(void)
{
    context = (gpp_ctx){words, words + 1024, words, NULL};
    auto_test_fifo_native = 1;
    auto_test_fifo_put = 4096;
    ps3dk_prof_test_hooks.gpu_gpp_dev = &device;
    ps3dk_prof__fifo_test_hook = attach_gate;
    pthread_t racer;
    assert(pthread_create(&racer, NULL, frame_racer, NULL) == 0);
    while (!atomic_load_explicit(&race_ready, memory_order_acquire)) { }
    ps3dk_prof__auto_gcm_init_result(&context, sizeof words, sizeof words, words, 0);
    atomic_store_explicit(&race_done, 1, memory_order_release);
    assert(pthread_join(racer, NULL) == 0);
    ps3dk_prof__fifo_test_hook = NULL;
    assert(g_fifo.account && g_fifo.account->enabled && "native FIFO accounting must activate at AUTO attach");
#ifdef PS3DK_PROF_FIFO_END_KNOWN
    gpp_ctx other = context;
    assert(ps3dk_prof__auto_fifo_end_known(&context, words + 1024, words + 1024));
    assert(!ps3dk_prof__auto_fifo_end_known(&other, words + 1024, words + 1024));
    assert(!ps3dk_prof__auto_fifo_end_known(&context, words + 1023, words + 1023));
    assert(!ps3dk_prof__auto_fifo_end_known(&context, words + 1024, words + 1000));
    assert(!ps3dk_prof__auto_fifo_end_known(NULL, words + 1024, words + 1024));
    assert(pthread_create(&racer, NULL, foreign_end_query, NULL) == 0);
    assert(pthread_join(racer, NULL) == 0);
#else
    assert(0 && "FIFO end identity accessor is unavailable");
#endif
    observer.reg = *auto_test_fifo_observer;
    ps3dk_prof_frame();
    assert(context.current == words + 24); /* clock pair and frame-begin pair:96 bytes */
    wrap(16); wrap(8);
    for (unsigned i = 0; i < 4; ++i) *context.current++ = 0x00040000;
    publish(4104); publish(4104);
    ps3dk_prof_frame(); /* closes0 before emitting AUTO frame-end/begin markers for1 */
    assert(g_fifo.account->rows[0].prepared == 216 && g_fifo.account->rows[0].published == 208 &&
           g_fifo.account->rows[0].wraps == 2 && g_fifo.account->rows[0].reasons == 0);
    publish(offset());
    ps3dk_prof_token token = ps3dk_prof_frame_token();
    assert(ps3dk_prof_auto_frame_end(1, token) == 0);
    assert(g_fifo.account->rows[1].prepared == 144 && g_fifo.account->rows[1].published == 104 &&
           g_fifo.account->rows[1].wraps == 0 && g_fifo.account->rows[1].reasons == 0);
    ps3dk_prof_frame();
    auto_test_fifo_native = 0;
#ifdef PS3DK_PROF_FIFO_NATIVE_ACCOUNT
    ps3dk_prof__fifo_account_check_context(&context); /* custom callback before it can mutate the owner cursor */
#endif
    auto_test_fifo_native = 1;
    token = ps3dk_prof_frame_token();
    assert(ps3dk_prof_auto_frame_end(1, token) == 0);
    assert(g_fifo.account->rows[2].reasons & FIFO_ACCOUNT_FLOW);
    puts("AUTO native FIFO accounting PASS");
}
