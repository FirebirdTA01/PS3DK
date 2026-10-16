// pthread-raw-threads: librt's thread support for threads made with the SDK's sys_ppu_thread_create.
//  1. Churn past one slot chunk: 40 raw threads, one after another, each sets a pthread key (with a destructor) and
//     first-uses a thread_local with a destructor, then ends by one of the covered routes in turn: returning from its
//     entry, sys_ppu_thread_exit(code), pthread_exit.  Every key destructor and every thread_local destructor runs,
//     and sys_ppu_thread_join sees the exit code the thread passed.
//  2. Growth while live: 34 pthreads alive at once (more than the first 32-slot chunk) while a joiner thread is
//     blocked in pthread_join on the first; all are released and each join returns its own value.
//  3. Raw threads racing one function-local static (constructed once) and sharing one shared_ptr (count exact).
//  4. 40 raw threads (more than one slot chunk) whose FIRST pthread use is pthread_setspecific inside a thread_local
//     destructor, ending by return or sys_ppu_thread_exit: every key destructor still runs (the cleanup adopts the
//     slot that call creates and releases it).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <pthread.h>
#include <sys/ppu_thread.h>
#include <unistd.h>

static int fails;

static void check(const char *what, long got, long want)
{
	std::printf("pthread-raw-threads: %s = %ld%s\n", what, got, got == want ? " ok" : " FAIL");
	if (got != want)
		fails++;
}

// --- 1. churn ---
static pthread_key_t key;
static std::atomic<int> key_dtors{0}, tl_dtors{0};

struct Tl {
	int x = 0;
	~Tl() { ++tl_dtors; }
};
static thread_local Tl tl;

static void key_dtor(void *) { ++key_dtors; }

static void churn_entry(uint64_t arg)
{
	int route = (int)(arg % 3);
	pthread_setspecific(key, (void *)(uintptr_t)(arg + 1));
	tl.x = (int)arg;
	if (route == 1)
		sys_ppu_thread_exit(1000 + arg);
	if (route == 2)
		pthread_exit(nullptr);
	// route 0: return from the entry
}

// --- 2. growth while live ---
static pthread_mutex_t gm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gc = PTHREAD_COND_INITIALIZER;
static int release_all;
static pthread_t live[34];
static void *joined_value;
static std::atomic<int> joiner_in{0};

static void *live_thread(void *arg)
{
	pthread_mutex_lock(&gm);
	while (!release_all)
		pthread_cond_wait(&gc, &gm);
	pthread_mutex_unlock(&gm);
	return arg;
}

static void *joiner(void *)
{
	joiner_in = 1;
	pthread_join(live[0], &joined_value);
	return nullptr;
}

// --- 3. static + shared_ptr from raw threads ---
static std::atomic<int> ctor_count{0};
struct Counted {
	Counted() { ++ctor_count; usleep(20000); }
	int v = 3;
};
static int use_static()
{
	static Counted c;
	return c.v;
}
static std::shared_ptr<int> shared;
static std::atomic<int> raw_sum{0};

static void race_entry(uint64_t)
{
	raw_sum += use_static();
	for (int k = 0; k < 2000; k++) {
		std::shared_ptr<int> q = shared;
		(void)q;
	}
	sys_ppu_thread_exit(0);
}

// --- 4. first pthread use during thread_local destruction ---
// The thread touches nothing pthread-related until its thread_local's destructor, which makes its first
// pthread_setspecific: the slot that creates must still get its key destructors and be released.
static pthread_key_t late_key;
static std::atomic<int> late_set_ok{0}, late_key_dtors{0};

static void late_key_dtor(void *) { ++late_key_dtors; }

struct LateTl {
	int x = 0;
	~LateTl()
	{
		if (pthread_setspecific(late_key, (void *)1) == 0)
			++late_set_ok;
	}
};
static thread_local LateTl late_tl;

static void late_entry(uint64_t arg)
{
	late_tl.x = (int)arg;
	if (arg % 2)
		sys_ppu_thread_exit(0);
	// even: return from the entry
}

int main()
{
	// 1
	pthread_key_create(&key, key_dtor);
	int code_ok = 0;
	for (uint64_t i = 0; i < 40; i++) {
		sys_ppu_thread_t id;
		uint64_t code = 0;
		if (sys_ppu_thread_create(&id, churn_entry, i, 1000, 0x10000, SYS_PPU_THREAD_CREATE_JOINABLE, "churn") != 0) {
			std::printf("pthread-raw-threads: create %d FAIL\n", (int)i);
			fails++;
			continue;
		}
		sys_ppu_thread_join(id, &code);
		if ((i % 3 == 1 && code == 1000 + i) || (i % 3 != 1 && code == 0))
			code_ok++;
	}
	check("raw threads: key destructors run", key_dtors, 40);
	check("raw threads: thread_local destructors run", tl_dtors, 40);
	check("raw threads: exit codes as passed", code_ok, 40);

	// 2
	int created = 0;
	for (int i = 0; i < 34; i++)
		created += pthread_create(&live[i], nullptr, live_thread, (void *)(intptr_t)(i + 100)) == 0;
	check("34 pthreads alive at once", created, 34);
	pthread_t j;
	pthread_create(&j, nullptr, joiner, nullptr);
	while (!joiner_in)
		usleep(1000);
	usleep(20000);	// the joiner is now blocked in pthread_join
	pthread_mutex_lock(&gm);
	release_all = 1;
	pthread_cond_broadcast(&gc);
	pthread_mutex_unlock(&gm);
	pthread_join(j, nullptr);
	check("blocked joiner got thread 0's value", (long)(intptr_t)joined_value, 100);
	int values_ok = 1;
	for (int i = 1; i < 34; i++) {
		void *v = nullptr;
		if (pthread_join(live[i], &v) != 0 || (intptr_t)v != i + 100)
			values_ok = 0;
	}
	check("every other join returned its own value", values_ok, 1);

	// 3
	shared = std::make_shared<int>(1);
	sys_ppu_thread_t ids[8];
	for (auto &id : ids)
		sys_ppu_thread_create(&id, race_entry, 0, 1000, 0x10000, SYS_PPU_THREAD_CREATE_JOINABLE, "race");
	for (auto id : ids) {
		uint64_t code;
		sys_ppu_thread_join(id, &code);
	}
	check("static constructed once by raw threads", ctor_count, 1);
	check("static value seen by all 8", raw_sum, 24);
	check("shared_ptr count exact after raw-thread copies", shared.use_count(), 1);

	// 4
	pthread_key_create(&late_key, late_key_dtor);
	for (uint64_t i = 0; i < 40; i++) {
		sys_ppu_thread_t id;
		uint64_t code;
		if (sys_ppu_thread_create(&id, late_entry, i, 1000, 0x10000, SYS_PPU_THREAD_CREATE_JOINABLE, "late") != 0) {
			std::printf("pthread-raw-threads: late create %d FAIL\n", (int)i);
			fails++;
			continue;
		}
		sys_ppu_thread_join(id, &code);
	}
	check("first key use in a thread_local destructor: setspecific ok", late_set_ok, 40);
	check("first key use in a thread_local destructor: key destructors run", late_key_dtors, 40);

	if (fails)
		std::printf("PTHREAD_RAW_THREADS_FAIL %d\n", fails);
	else
		std::printf("PTHREAD_RAW_THREADS_OK\n");
	return fails;
}
