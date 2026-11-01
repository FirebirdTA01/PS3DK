// Runtime boundary only. This does not link or validate a profiler.
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <pthread.h>
#include <sys/ppu_thread.h>
#include <sys/thread.h>
#include <sys/timer.h>
#include <sys/mutex.h>

struct State {
    int route;
    unsigned tls_calls = 0, key_calls = 0, hook_calls = 0;
    unsigned lock_faults = 0;
    bool ordered = true;
    std::atomic<bool> ready{false}, release{false};
};
static thread_local State *current;
static pthread_key_t key;
static unsigned tests, failures;

#if PROFILER_EXIT_FAULT_TEST
static thread_local bool fail_next_lock;
extern "C" s32 profiler_exit_test_mutex_lock(sys_mutex_t mutex, u64 timeout_usec)
{
    if (fail_next_lock) {
        fail_next_lock = false;
        ++current->lock_faults;
        return EBUSY;
    }
    return sysMutexLock(mutex, timeout_usec);
}
#endif

static void check(const char *name, bool ok)
{
    ++tests;
    if (!ok) ++failures;
    std::printf("profiler-thread-exit: %s %s\n", name, ok ? "ok" : "FAIL");
}

struct ThreadLocal {
    int value;
    ~ThreadLocal()
    {
        ++current->tls_calls;
#if PROFILER_EXIT_FAULT_TEST
        if (current->route == 10 && pthread_setspecific(key, current) != 0)
            current->ordered = false; // create the first slot during TLS cleanup
        if (current->route == 9 || current->route == 10)
            fail_next_lock = true; // fail TSD iteration or late-slot adoption
#endif
        if (current->route == 6)
            sys_ppu_thread_exit(0); // cleanup reentry: abandon the remaining callbacks
    }
};
static thread_local ThreadLocal local;

static void key_destructor(void *p)
{
    State *s = static_cast<State *>(p);
    ++s->key_calls;
    s->ordered = s->ordered && s->tls_calls == 1 && s->hook_calls == 0;
    if (s->route == 7)
        sys_ppu_thread_exit(0); // hook must not acknowledge an abandoned cleanup
    if (s->key_calls == 1 && pthread_setspecific(key, s) != 0)
        s->ordered = false; // require the second destructor round before the hook
}

#if EXPECT_EXIT_HOOK
extern "C" void __ps3dk_prof_thread_exit(void)
{
    if (!current) return;
    ++current->hook_calls;
    unsigned want_tls = current->route == 11 ? 0u : 1u;
    unsigned want_keys = current->route == 11 ? 0u : 2u;
    current->ordered = current->ordered && current->tls_calls == want_tls &&
                       current->key_calls == want_keys && current->hook_calls == 1;
    if (current->route == 8)
        sys_ppu_thread_exit(0); // pt_in_cleanup must suppress a second notification
}
#endif

static void body(State *s)
{
    current = s;
    if (s->route == 11) return; // no SDK thread slot or TLS/key destructor
    local.value = 42;
    if (s->route != 10 && pthread_setspecific(key, s) != 0) s->ordered = false;
    s->ready.store(true, std::memory_order_release);
    if (s->route == 4)
        while (!s->release.load(std::memory_order_acquire)) sys_timer_usleep(1000);
    if (s->route == 1) pthread_exit(nullptr);
    if (s->route == 3) sys_ppu_thread_exit(0);
    if (s->route == 5) sysThreadExit(0); // deliberate raw bypass
}
static void *posix_entry(void *p) { body(static_cast<State *>(p)); return nullptr; }
static void cell_entry(uint64_t p) { body(reinterpret_cast<State *>(static_cast<uintptr_t>(p))); }
static void raw_entry(void *p) { body(static_cast<State *>(p)); sysThreadExit(0); }

int main()
{
    int rc = pthread_key_create(&key, key_destructor);
    check("create destructor key", rc == 0);
    if (rc != 0) return 1;
    for (int route = 0; route != 12; ++route) {
#if !PROFILER_EXIT_FAULT_TEST
        if (route == 9 || route == 10) continue; // candidate-only lock injection
#endif
        State state;
        state.route = route;
        if (route == 2 || route == 3 || route >= 5) {
            sys_ppu_thread_t tid;
            uint64_t result = 99;
            if (route == 5)
                rc = sysThreadCreate(&tid, raw_entry, &state, 1000, 65536,
                                      THREAD_JOINABLE, (char *)"prof-raw-exit");
            else
                rc = sys_ppu_thread_create(&tid, cell_entry, (uint64_t)(uintptr_t)&state,
                                           1000, 65536, SYS_PPU_THREAD_CREATE_JOINABLE, "prof-cell-exit");
            check("create Cell/raw thread", rc == 0);
            if (rc != 0) continue;
            check("join Cell/raw thread", sys_ppu_thread_join(tid, &result) == 0 && result == 0);
        } else {
            pthread_t tid;
            rc = pthread_create(&tid, nullptr, posix_entry, &state);
            check("create pthread", rc == 0);
            if (rc != 0) continue;
            if (route == 4) {
                while (!state.ready.load(std::memory_order_acquire)) sys_timer_usleep(1000);
                check("cancellation is refused", pthread_cancel(tid) == ENOSYS);
                check("cancel refusal does not retire running thread", state.hook_calls == 0);
                state.release.store(true, std::memory_order_release);
            }
            check("join pthread", pthread_join(tid, nullptr) == 0);
        }
        unsigned want_tls = route == 5 || route == 11 ? 0u : 1u;
        unsigned want_keys = route == 5 || route == 6 || route >= 9 ? 0u : route == 7 ? 1u : 2u;
        unsigned want_hook = EXPECT_EXIT_HOOK && (route <= 4 || route == 8 || route == 11) ? 1u : 0u;
        std::printf("ROUTE %d tls=%u keys=%u hook=%u\n", route, state.tls_calls, state.key_calls, state.hook_calls);
        check("TLS cleanup count", state.tls_calls == want_tls);
        check("all applicable key rounds", state.key_calls == want_keys);
        check("one hook only after complete cleanup", state.hook_calls == want_hook);
        check("cleanup ordering", state.ordered);
        if (route == 9 || route == 10) check("cleanup lock failure injected", state.lock_faults == 1);
    }
    check("delete key", pthread_key_delete(key) == 0);
    std::printf("PROFILER_THREAD_EXIT tests=%u pass=%u fail=%u hook=%d\n",
                tests, tests - failures, failures, EXPECT_EXIT_HOOK);
    return failures ? 1 : 0;
}
