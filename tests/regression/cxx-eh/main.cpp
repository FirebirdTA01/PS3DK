// C++ exception regression for the PPU, built for both ABIs.
//
// Guards the ILP32 link-register / EH-data-register width fix (GCC patch
// 0037) and per-thread exception state (patch 0038). Each check prints its
// name; the run ends with CXX_EH_OK, or CXX_EH_FAIL naming the first check
// that failed. A regression in either patch shows up as a terminate() or a
// wrong value here rather than as a silent pass.
#include <atomic>
#include <cstdio>
#include <exception>
#include <new>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>

static const char *g_failed = nullptr;

static void check(bool ok, const char *name)
{
    std::printf("CXX_EH %s %s\n", name, ok ? "pass" : "FAIL");
    if (!ok && !g_failed)
        g_failed = name;
}

// 1. Throw and catch inside a static constructor (runs before main).
static int g_static_caught = 0;
struct StaticThrower {
    StaticThrower()
    {
        try {
            throw std::bad_alloc();
        } catch (const std::bad_alloc &) {
            g_static_caught = 1;
        }
    }
};
static StaticThrower g_static_thrower;

// 2. Typed throw from a called function.
__attribute__((noinline)) static void thrower(int v)
{
    if (v > 0)
        throw 42;
}

// 3. Destructors during unwinding, then rethrow.
static int g_dtors = 0;
struct Guard {
    ~Guard() { ++g_dtors; }
};

__attribute__((noinline)) static void inner(int v)
{
    Guard g;
    if (v > 0)
        throw std::runtime_error("inner");
}

__attribute__((noinline)) static void middle(int v)
{
    Guard g;
    inner(v);
}

__attribute__((noinline)) static void rethrower(int v)
{
    Guard g;
    try {
        middle(v);
    } catch (const std::runtime_error &) {
        throw;
    }
}

// 4. Per-thread exception state: A stays inside its handler while B throws
//    and catches; each must still see its own exception.
static std::atomic<int> g_step{0};
static int g_result_a = -1, g_result_b = -1;

static void wait_for(int s)
{
    while (g_step.load() < s)
        sched_yield();
}

static int held_value()
{
    std::exception_ptr p = std::current_exception();
    if (!p)
        return -100;
    try {
        std::rethrow_exception(p);
    } catch (int v) {
        return v;
    } catch (...) {
        return -200;
    }
}

static void *thread_a(void *)
{
    try {
        throw 1;
    } catch (int) {
        g_step.store(1);
        wait_for(2);
        g_result_a = held_value() * 10 + std::uncaught_exceptions();
        g_step.store(3);
    }
    return nullptr;
}

static void *thread_b(void *)
{
    wait_for(1);
    try {
        throw 2;
    } catch (int) {
        g_step.store(2);
        wait_for(3);
        g_result_b = held_value() * 10 + std::uncaught_exceptions();
    }
    return nullptr;
}

int main()
{
    std::printf("CXX_EH start ptr=%u\n", (unsigned)sizeof(void *));

    check(g_static_caught == 1, "static-ctor");

    int typed = 0;
    try {
        thrower(1);
    } catch (int v) {
        typed = v;
    }
    check(typed == 42, "typed");

    bool outer = false;
    try {
        rethrower(1);
    } catch (const std::exception &) {
        outer = true;
    }
    check(outer && g_dtors == 3, "raii-rethrow");

    pthread_t a, b;
    bool threads = pthread_create(&a, nullptr, thread_a, nullptr) == 0 &&
                   pthread_create(&b, nullptr, thread_b, nullptr) == 0;
    if (threads) {
        threads = pthread_join(a, nullptr) == 0;
        threads = pthread_join(b, nullptr) == 0 && threads;
    }
    check(threads && g_result_a == 10 && g_result_b == 20, "per-thread");

    if (g_failed) {
        std::printf("CXX_EH_FAIL %s\n", g_failed);
        return 1;
    }
    std::puts("CXX_EH_OK");
    return 0;
}
