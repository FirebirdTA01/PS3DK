/* Lifecycle integration fixture, not the unchanged-source compiler-hook gate.
 * Manual zones provide real records until Task4 supplies compiler adapters.
 * Host workers explicitly model the SDK's post-cleanup notification; target
 * workers rely exclusively on the installed SDK notification. */
#define PS3DK_PROF 1
#include <ps3dk/prof.h>
#include <pthread.h>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

extern "C" void __ps3dk_prof_thread_exit(void);
static const char *mode;
static int early_ok;
static pthread_key_t cleanup_key;

/* First ordinary user priority: libc locks and profiler startup must already
 * be live. Check both allocation and capture, on each PPU ABI. */
__attribute__((constructor(101))) static void early_user()
{
    void *p = std::malloc(37);
    if (!p) _exit(41);
    std::memset(p, 0x5a, 37);
    early_ok = static_cast<unsigned char *>(p)[36] == 0x5a;
    std::free(p);
    PS3DK_PROF_ZONE("early-user");
}

__attribute__((destructor(101))) static void late_user()
{
    PS3DK_PROF_ZONE("late-user");
}

struct UserObject {
    UserObject() { PS3DK_PROF_ZONE("user-object-ctor"); }
    ~UserObject() { PS3DK_PROF_ZONE("user-object-dtor"); }
};
static UserObject user_object;

struct ThreadCleanup {
    int touched;
    ~ThreadCleanup() { PS3DK_PROF_ZONE("worker-tls-cleanup"); }
};
static thread_local ThreadCleanup thread_cleanup;

static void key_cleanup(void *round)
{
    { PS3DK_PROF_ZONE("worker-key-cleanup"); }
    if (reinterpret_cast<uintptr_t>(round) == 1) {
        if (pthread_setspecific(cleanup_key, reinterpret_cast<void *>(2))) _exit(44);
        return;
    }
#ifdef PS3PROF_HOST_EXIT_MODEL
    /* Host libc has no SDK notification. Model it only after the fixture's
     * TLS destructor and both key rounds; target builds use the actual SDK. */
    if (std::strcmp(mode, "abandoned") != 0) {
        __ps3dk_prof_thread_exit();
        __ps3dk_prof_thread_exit();
    }
#endif
}

static void *worker(void *)
{
    if (std::strcmp(mode, "unregistered") != 0) {
        thread_cleanup.touched = 1;
        if (pthread_setspecific(cleanup_key, reinterpret_cast<void *>(1))) _exit(45);
        ps3dk_prof_zone z = PS3DK_PROF_ZONE_BEGIN("worker");
        if (std::strcmp(mode, "open") != 0) PS3DK_PROF_ZONE_END(z);
    } else {
#ifdef PS3PROF_HOST_EXIT_MODEL
        __ps3dk_prof_thread_exit();
        __ps3dk_prof_thread_exit(); /* idempotent, never registers again */
#endif
    }
    return nullptr;
}

int main(int argc, char **argv)
{
    if (!early_ok) return 42;
    mode = argc > 1 ? argv[1] : "normal";
    { PS3DK_PROF_ZONE("main"); }
    if (pthread_key_create(&cleanup_key, key_cleanup)) return 46;
    pthread_t t;
    if (pthread_create(&t, nullptr, worker, nullptr) || pthread_join(t, nullptr)) return 43;
    if (std::strcmp(mode, "immediate") == 0) _exit(0);
    if (std::strcmp(mode, "quick") == 0) ::quick_exit(0);
    if (std::strcmp(mode, "exit") == 0) std::exit(0);
    return 0;
}
