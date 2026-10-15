// cxx-thread-main-exit: when main returns, the main thread's thread_locals are destroyed before the atexit handlers
// and the static destructors run ([basic.start.term]).  newlib's exit() runs librt's thread_local list first (newlib
// patch 0019).  The atexit handler is registered BEFORE the thread_local is first used, so registration order alone
// would run it first; it and a static object's destructor check that the thread_local is already gone, and the
// handler prints the verdict (it runs after the static destructor).
#include <cstdio>
#include <cstdlib>

static int tl_destroyed, static_saw_it = -1;

struct Tl {
	int x = 0;
	~Tl() { tl_destroyed = 1; }
};
static thread_local Tl tl;

struct Static {
	~Static() { static_saw_it = tl_destroyed; }
};

static void at_exit_check()
{
	std::printf("cxx-thread-main-exit: thread_local destroyed before atexit handler: %s\n", tl_destroyed ? "ok" : "FAIL");
	std::printf("cxx-thread-main-exit: thread_local destroyed before static destructor: %s\n",
	            static_saw_it == 1 ? "ok" : "FAIL");
	std::printf(tl_destroyed && static_saw_it == 1 ? "CXX_THREAD_MAIN_EXIT_OK\n" : "CXX_THREAD_MAIN_EXIT_FAIL\n");
}

int main()
{
	std::atexit(at_exit_check);	// registered first ...
	static Static s;		// ... its destructor is registered after it, so it runs before the handler
	(void)s;
	tl.x = 1;			// ... and the thread_local after both
	std::printf("cxx-thread-main-exit: main returning\n");
	return 0;
}
