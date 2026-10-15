// cxx-thread-cxx20: the C++20 thread library on the posix gthread layer (GCC 16 with --enable-threads=posix).
//  - std::jthread joins on destruction and std::stop_token stops a worker
//  - std::latch releases every waiter once the count reaches zero
//  - std::barrier runs its completion function once per phase, every phase
//  - std::counting_semaphore bounds concurrency and try_acquire_for times out
//  - std::atomic wait/notify (libstdc++'s proxy-wait fallback here: no futex on Lv-2) wakes a waiter
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <latch>
#include <semaphore>
#include <stop_token>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static int fails;

static void check(const char *what, bool ok)
{
	std::printf("cxx-thread-cxx20: %s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		fails++;
}

int main()
{
	// jthread + stop_token
	{
		std::atomic<int> loops{0};
		std::atomic<bool> stopped{false};
		{
			std::jthread t([&](std::stop_token st) {
				while (!st.stop_requested()) {
					++loops;
					std::this_thread::sleep_for(1ms);
				}
				stopped = true;
			});
			std::this_thread::sleep_for(20ms);
		}	// ~jthread: request_stop + join
		check("jthread stops and joins on destruction", stopped && loops > 0);
	}
	// latch
	{
		std::latch l(3);
		std::atomic<int> through{0};
		std::vector<std::thread> ts;
		for (int i = 0; i < 3; i++)
			ts.emplace_back([&] {
				l.arrive_and_wait();
				++through;
			});
		for (auto &t : ts)
			t.join();
		check("latch releases all three waiters", through == 3);
	}
	// barrier with completion
	{
		std::atomic<int> completions{0};
		std::barrier b(3, [&]() noexcept { ++completions; });
		std::vector<std::thread> ts;
		for (int i = 0; i < 3; i++)
			ts.emplace_back([&] {
				for (int phase = 0; phase < 4; phase++)
					b.arrive_and_wait();
			});
		for (auto &t : ts)
			t.join();
		check("barrier completion ran once per phase (4)", completions == 4);
	}
	// counting semaphore
	{
		std::counting_semaphore<2> sem(2);
		std::atomic<int> inside{0}, peak{0};
		std::vector<std::thread> ts;
		for (int i = 0; i < 5; i++)
			ts.emplace_back([&] {
				sem.acquire();
				int n = ++inside;
				int p = peak.load();
				while (n > p && !peak.compare_exchange_weak(p, n)) {
				}
				std::this_thread::sleep_for(10ms);
				--inside;
				sem.release();
			});
		for (auto &t : ts)
			t.join();
		check("counting_semaphore<2> admits at most two", peak >= 1 && peak <= 2);
		std::binary_semaphore one(0);
		auto t0 = std::chrono::steady_clock::now();
		bool got = one.try_acquire_for(30ms);
		check("binary_semaphore try_acquire_for times out", !got && std::chrono::steady_clock::now() - t0 >= 25ms);
	}
	// atomic wait / notify
	{
		std::atomic<int> flag{0};
		std::atomic<bool> woke{false};
		std::thread t([&] {
			flag.wait(0);
			woke = flag.load() == 1;
		});
		std::this_thread::sleep_for(20ms);
		flag.store(1);
		flag.notify_one();
		t.join();
		check("atomic wait woken by notify_one", woke);
	}
	if (fails)
		std::printf("CXX_THREAD_CXX20_FAIL %d\n", fails);
	else
		std::printf("CXX_THREAD_CXX20_OK\n");
	return fails;
}
