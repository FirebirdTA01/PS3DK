// cxx-thread-exit-order: a thread's thread_locals are destroyed before notify_all_at_thread_exit notifies and before
// promise::set_value_at_thread_exit makes the future ready ([thread.condition.nonmember], [futures.promise]), whichever
// order the thread registered them in.  librt's exit cleanup runs the thread_local destructors (registered through
// __cxa_thread_atexit_impl) before the pthread key destructors that carry libstdc++'s at-thread-exit list.
//
// For notify and for promise, and with the thread_local first used BEFORE and, in a second run, AFTER the
// at-thread-exit call: the thread_local's destructor sleeps 20 ms and then records itself; the waiter checks the record
// as soon as it is woken.  The worker keeps the mutex until it exits, so a correct order always passes.  One more run
// has a destructor that first uses another thread_local, which must also be destroyed before the notification.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

static int fails;
static std::atomic<int> dtor_done{0}, dtor2_done{0};

static void check(const char *what, bool ok)
{
	std::printf("cxx-thread-exit-order: %s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		fails++;
}

struct Late {
	~Late() { std::this_thread::sleep_for(20ms); dtor2_done = 1; }
	int x = 0;
};
static thread_local Late late;

struct Tl {
	bool chain = false;
	int x = 0;
	~Tl()
	{
		if (chain)
			late.x = 1;	// first use, inside a thread_local destructor: registers another destructor
		std::this_thread::sleep_for(20ms);
		dtor_done = 1;
	}
};
static thread_local Tl tl;

static void notify_case(const char *name, bool tl_first, bool chain)
{
	std::mutex m;
	std::condition_variable cv;
	bool ready = false;
	dtor_done = 0;
	dtor2_done = 0;
	std::thread t([&] {
		if (tl_first) {
			tl.x = 1;
			tl.chain = chain;
		}
		std::unique_lock<std::mutex> l(m);
		ready = true;
		std::notify_all_at_thread_exit(cv, std::move(l));
		if (!tl_first) {
			tl.x = 1;
			tl.chain = chain;
		}
	});
	{
		std::unique_lock<std::mutex> l(m);
		cv.wait(l, [&] { return ready; });
		check(name, dtor_done == 1 && (!chain || dtor2_done == 1));
	}
	t.join();
}

static void promise_case(const char *name, bool tl_first)
{
	std::promise<int> p;
	std::future<int> f = p.get_future();
	dtor_done = 0;
	std::thread t([&] {
		if (tl_first)
			tl.x = 1;
		p.set_value_at_thread_exit(7);
		if (!tl_first)
			tl.x = 1;
	});
	int v = f.get();
	check(name, v == 7 && dtor_done == 1);
	t.join();
}

int main()
{
	notify_case("notify_all_at_thread_exit after thread_local first used before", true, false);
	notify_case("notify_all_at_thread_exit after thread_local first used after", false, false);
	notify_case("notify waits for a thread_local made inside a thread_local destructor", true, true);
	promise_case("set_value_at_thread_exit after thread_local first used before", true);
	promise_case("set_value_at_thread_exit after thread_local first used after", false);
	if (fails)
		std::printf("CXX_THREAD_EXIT_ORDER_FAIL %d\n", fails);
	else
		std::printf("CXX_THREAD_EXIT_ORDER_OK\n");
	return fails;
}
