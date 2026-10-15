// cxx-thread-basic: the C++17 thread library on the posix gthread layer (GCC 16 with --enable-threads=posix).
// Each check prints a line; the row passes when every one does.
//  - std::thread runs, joins and passes arguments
//  - std::mutex mutual exclusion; std::recursive_mutex re-entry; std::timed_mutex try_lock_for times out and wakes
//  - std::condition_variable: wait_for times out, notify_one wakes a waiter
//  - std::call_once: a throwing callable is retried by the next caller, then runs once
//  - std::async / std::future: a value and an exception cross threads
//  - std::shared_mutex: concurrent shared holders, exclusive excludes
//  - std::shared_ptr: cross-thread copies and releases leave the count exact
//  - a function-local static is initialised once when many threads reach it together
//  - a thread_local with a non-trivial destructor is destroyed when its thread ends
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static int fails;

static void check(const char *what, bool ok)
{
	std::printf("cxx-thread-basic: %s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		fails++;
}

static std::atomic<int> ctor_count{0}, tl_dtors{0};

struct Counted {
	int v;
	Counted() : v(7) { ++ctor_count; std::this_thread::sleep_for(20ms); }
};

static int use_static()
{
	static Counted c;
	return c.v;
}

struct TlNote {
	~TlNote() { ++tl_dtors; }
	int x = 0;
};
static thread_local TlNote tl_note;

int main()
{
	// thread + args
	{
		int out = 0;
		std::thread t([&out](int a, int b) { out = a + b; }, 40, 2);
		t.join();
		check("thread runs with arguments", out == 42);
	}
	// mutex
	{
		std::mutex m;
		long counter = 0;
		std::vector<std::thread> ts;
		for (int i = 0; i < 4; i++)
			ts.emplace_back([&] {
				for (int k = 0; k < 2000; k++) {
					std::lock_guard<std::mutex> g(m);
					++counter;
				}
			});
		for (auto &t : ts)
			t.join();
		check("mutex excludes (8000 increments)", counter == 8000);
	}
	// recursive mutex
	{
		std::recursive_mutex rm;
		rm.lock();
		bool again = rm.try_lock();
		if (again)
			rm.unlock();
		rm.unlock();
		check("recursive_mutex re-entry", again);
	}
	// timed mutex: timeout, then wake
	{
		std::timed_mutex tm;
		tm.lock();
		bool got = true;
		auto t0 = std::chrono::steady_clock::now();
		std::thread t([&] { got = tm.try_lock_for(30ms); });
		t.join();
		auto waited = std::chrono::steady_clock::now() - t0;
		check("timed_mutex try_lock_for times out", !got && waited >= 25ms);
		bool later = false;
		std::thread t2([&] { later = tm.try_lock_for(2s); if (later) tm.unlock(); });
		std::this_thread::sleep_for(20ms);
		tm.unlock();
		t2.join();
		check("timed_mutex try_lock_for wakes on unlock", later);
	}
	// condition variable
	{
		std::mutex m;
		std::condition_variable cv;
		std::unique_lock<std::mutex> l(m);
		auto t0 = std::chrono::steady_clock::now();
		bool timed_out = cv.wait_for(l, 30ms) == std::cv_status::timeout;
		check("condition_variable wait_for times out", timed_out && std::chrono::steady_clock::now() - t0 >= 25ms);
		bool ready = false;
		std::thread t([&] {
			std::this_thread::sleep_for(20ms);
			{
				std::lock_guard<std::mutex> g(m);
				ready = true;
			}
			cv.notify_one();
		});
		bool woke = cv.wait_for(l, 2s, [&] { return ready; });
		l.unlock();
		t.join();
		check("condition_variable notify_one wakes", woke);
	}
	// call_once with a throwing callable
	{
		std::once_flag f;
		std::atomic<int> runs{0}, threw{0}, done{0};
		std::vector<std::thread> ts;
		for (int i = 0; i < 4; i++)
			ts.emplace_back([&] {
				for (;;) {
					try {
						std::call_once(f, [&] {
							int r = ++runs;
							std::this_thread::sleep_for(10ms);
							if (r <= 2)
								throw std::runtime_error("retry");
						});
						++done;
						return;
					} catch (const std::runtime_error &) {
						++threw;
					}
				}
			});
		for (auto &t : ts)
			t.join();
		check("call_once retried after throws, ran 3 times", runs == 3 && threw == 2 && done == 4);
	}
	// async / future
	{
		auto ok = std::async(std::launch::async, [] { return 6 * 7; });
		auto bad = std::async(std::launch::async, []() -> int { throw std::runtime_error("from async"); });
		bool caught = false;
		try {
			bad.get();
		} catch (const std::runtime_error &) {
			caught = true;
		}
		check("async value and exception", ok.get() == 42 && caught);
	}
	// shared_mutex
	{
		std::shared_mutex sm;
		std::atomic<int> inside{0}, peak{0};
		std::vector<std::thread> ts;
		for (int i = 0; i < 3; i++)
			ts.emplace_back([&] {
				std::shared_lock<std::shared_mutex> s(sm);
				int n = ++inside;
				int p = peak.load();
				while (n > p && !peak.compare_exchange_weak(p, n)) {
				}
				std::this_thread::sleep_for(30ms);
				--inside;
			});
		for (auto &t : ts)
			t.join();
		check("shared_mutex shared holders overlap", peak >= 2);
		sm.lock_shared();
		bool excl = sm.try_lock();
		sm.unlock_shared();
		bool excl2 = sm.try_lock();
		if (excl2)
			sm.unlock();
		check("shared_mutex exclusive excludes readers", !excl && excl2);
	}
	// shared_ptr counts across threads
	{
		auto p = std::make_shared<int>(5);
		std::vector<std::thread> ts;
		for (int i = 0; i < 4; i++)
			ts.emplace_back([p] {
				for (int k = 0; k < 5000; k++) {
					std::shared_ptr<int> q = p;
					(void)q;
				}
			});
		for (auto &t : ts)
			t.join();
		check("shared_ptr use_count exact after cross-thread copies", p.use_count() == 1);
	}
	// function-local static
	{
		std::vector<std::thread> ts;
		std::atomic<int> sum{0};
		for (int i = 0; i < 6; i++)
			ts.emplace_back([&] { sum += use_static(); });
		for (auto &t : ts)
			t.join();
		check("function-local static constructed once", ctor_count == 1 && sum == 42);
	}
	// thread_local destructor at thread end
	{
		int before = tl_dtors;
		std::thread t([] { tl_note.x = 1; });
		t.join();
		check("thread_local destroyed when its thread ends", tl_dtors == before + 1);
	}
	if (fails)
		std::printf("CXX_THREAD_BASIC_FAIL %d\n", fails);
	else
		std::printf("CXX_THREAD_BASIC_OK\n");
	return fails;
}
