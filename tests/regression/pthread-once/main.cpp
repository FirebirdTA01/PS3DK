// pthread-once: librt's per-control pthread_once, including an init routine that throws.
//
// std::call_once (GCC 16 with gthreads) calls pthread_once directly, and its callable may throw; the exception
// unwinds through pthread_once, which must put the control back to idle so a later caller runs the routine again.
//  1. Throw and retry: 4 threads call pthread_once on one control; the routine throws on its first two runs.  Each
//     caller retries until a call returns normally.  Checks: the routine ran exactly 3 times, exactly once to
//     completion; every caller saw the value the successful run published; no caller returned before it.
//  2. Independent controls: a routine for control A starts a thread that runs pthread_once on control B, and joins
//     it before returning (a process-wide once lock would deadlock here).
//  3. Plain: a routine that returns normally runs once for 4 concurrent callers.
#include <atomic>
#include <cstdio>
#include <pthread.h>
#include <stdexcept>
#include <unistd.h>

namespace {

int fails;

void check(const char *what, long got, long want)
{
	std::printf("pthread-once: %s = %ld%s\n", what, got, got == want ? " ok" : " FAIL");
	if (got != want)
		fails++;
}

// 1. throw and retry
pthread_once_t once_throw = PTHREAD_ONCE_INIT;
std::atomic<int> throw_runs{0}, throw_completions{0}, throw_after_done{0};
int published;            // written by the successful run only
std::atomic<int> saw_published{0}, threw{0};

void init_throwing()
{
	int run = ++throw_runs;
	usleep(20000);            // let the other callers arrive and wait
	if (run <= 2)
		throw std::runtime_error("init failed");
	published = 42;
	++throw_completions;
}

void *caller_throwing(void *)
{
	for (int attempt = 0; attempt < 20; attempt++) {
		try {
			pthread_once(&once_throw, init_throwing);
			if (published == 42)
				++saw_published;
			return nullptr;
		} catch (const std::runtime_error &) {
			++threw;
		}
	}
	return nullptr;
}

// 2. independent controls
pthread_once_t once_a = PTHREAD_ONCE_INIT, once_b = PTHREAD_ONCE_INIT;
std::atomic<int> b_runs{0}, a_done{0};

void init_b() { ++b_runs; }

void *run_b(void *)
{
	pthread_once(&once_b, init_b);
	return nullptr;
}

void init_a()
{
	pthread_t t;
	if (pthread_create(&t, nullptr, run_b, nullptr) == 0)
		pthread_join(t, nullptr);
	a_done = 1;
}

// 3. plain
pthread_once_t once_plain = PTHREAD_ONCE_INIT;
std::atomic<int> plain_runs{0};

void init_plain()
{
	usleep(10000);
	++plain_runs;
}

void *caller_plain(void *)
{
	pthread_once(&once_plain, init_plain);
	return nullptr;
}

} // namespace

int main()
{
	pthread_t t[4];

	for (auto &th : t)
		pthread_create(&th, nullptr, caller_throwing, nullptr);
	for (auto &th : t)
		pthread_join(th, nullptr);
	check("throwing routine runs", throw_runs, 3);
	check("throwing routine completions", throw_completions, 1);
	check("exceptions seen by callers", threw, 2);
	check("callers that saw the published value", saw_published, 4);

	pthread_once(&once_a, init_a);
	check("A's routine finished after joining B's caller", a_done, 1);
	check("B's routine runs", b_runs, 1);

	for (auto &th : t)
		pthread_create(&th, nullptr, caller_plain, nullptr);
	for (auto &th : t)
		pthread_join(th, nullptr);
	check("plain routine runs", plain_runs, 1);

	if (fails)
		std::printf("PTHREAD_ONCE_FAIL %d\n", fails);
	else
		std::printf("PTHREAD_ONCE_OK\n");
	return fails;
}
