// errno-lv2: the LV2 errno values (newlib 0020) as EXECUTED by the rebuilt runtime, both ABIs.
//
// On the PS3 LV2 target the E* names are the LV2 status codes (EBUSY == 0x8001000A, ...).  The macros alone prove
// nothing about the libraries that produce and consume errno, so this row runs them (review: codex):
//   1. the names carry the LV2 codes;
//   2. pthreads: a held mutex's trylock returns EBUSY by name, the self-held (EDEADLK -> EBUSY fold) case too, a
//      timed lock that cannot get a held mutex returns ETIMEDOUT, a rwlock trywrlock under the caller's own write
//      hold returns EBUSY;
//   3. libc calls that set errno: open of a missing file (ENOENT), strtol overflow (ERANGE), an impossible malloc
//      (ENOMEM); strerror names them (not "Unknown");
//   4. C++: std::error_code / std::errc / generic_category agree with the names; std::thread::join on a
//      non-joinable thread throws std::system_error carrying EINVAL; std::mutex::try_lock on a held mutex fails;
//   5. sockets: recv(MSG_DONTWAIT) with nothing queued sets EAGAIN, closing a closed socket sets EBADF.
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <system_error>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <net/net.h>
#include <sys/process.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

SYS_PROCESS_PARAM(1001, 0x10000);

static const char *g_failed = nullptr;
static void check(bool ok, const char *name, long got = 0)
{
	printf("errno-lv2: %s %s", name, ok ? "ok" : "FAIL");
	if (!ok)
		printf(" (got %ld = 0x%08lx)", got, (unsigned long)(uint32_t)got);
	printf("\n");
	fflush(stdout);
	if (!ok && !g_failed)
		g_failed = name;
}

static pthread_mutex_t g_held = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_state;   // 1: holder has the mutex, 2: main is done with it
static void *holder(void *)
{
	pthread_mutex_lock(&g_held);
	pthread_mutex_lock(&g_gate);
	g_state = 1;
	pthread_cond_broadcast(&g_cv);
	while (g_state != 2)
		pthread_cond_wait(&g_cv, &g_gate);
	pthread_mutex_unlock(&g_gate);
	pthread_mutex_unlock(&g_held);
	return nullptr;
}

int main()
{
	printf("errno-lv2: abi %s\n", sizeof(void *) == 8 ? "lp64" : "ilp32");

	// 1. names
	check(EBUSY == (int)0x8001000A, "EBUSY is 0x8001000A", EBUSY);
	check(EAGAIN == (int)0x80010001, "EAGAIN is 0x80010001", EAGAIN);
	check(EINVAL == (int)0x80010002, "EINVAL is 0x80010002", EINVAL);
	check(ENOMEM == (int)0x80010004, "ENOMEM is 0x80010004", ENOMEM);
	check(ENOENT == (int)0x80010006, "ENOENT is 0x80010006", ENOENT);
	check(EDEADLK == (int)0x80010008, "EDEADLK is 0x80010008", EDEADLK);
	check(ETIMEDOUT == (int)0x8001000B, "ETIMEDOUT is 0x8001000B", ETIMEDOUT);
	check(ERANGE == (int)0x8001001C, "ERANGE is 0x8001001C", ERANGE);
	check(EBADF == (int)0x8001002A, "EBADF is 0x8001002A", EBADF);

	// 2. pthreads, executed
	pthread_t t;
	int rc = pthread_create(&t, nullptr, holder, nullptr);
	check(rc == 0, "holder thread created", rc);
	if (rc == 0) {
		// Only with a holder thread: waiting for one that was never created would never end.
		pthread_mutex_lock(&g_gate);
		while (g_state != 1)
			pthread_cond_wait(&g_cv, &g_gate);
		pthread_mutex_unlock(&g_gate);
		rc = pthread_mutex_trylock(&g_held);
		check(rc == EBUSY, "trylock on a mutex another thread holds returns EBUSY", rc);
		struct timespec past;
		clock_gettime(CLOCK_REALTIME, &past);
		past.tv_sec -= 1;
		rc = pthread_mutex_timedlock(&g_held, &past);
		check(rc == ETIMEDOUT, "timedlock with a passed deadline on a held mutex returns ETIMEDOUT", rc);
		pthread_mutex_lock(&g_gate);
		g_state = 2;
		pthread_cond_broadcast(&g_cv);
		pthread_mutex_unlock(&g_gate);
		rc = pthread_join(t, nullptr);
		check(rc == 0, "holder thread joined", rc);
	}

	pthread_mutex_t self = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_lock(&self);
	rc = pthread_mutex_trylock(&self);
	check(rc == EBUSY, "trylock on a mutex the caller holds returns EBUSY (EDEADLK folded)", rc);
	pthread_mutex_unlock(&self);
	pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
	pthread_rwlock_wrlock(&rw);
	rc = pthread_rwlock_trywrlock(&rw);
	check(rc == EBUSY, "rwlock trywrlock under the caller's own write hold returns EBUSY", rc);
	pthread_rwlock_unlock(&rw);

	// 3. libc errno producers and strerror
	errno = 0;
	int fd = open("/dev_hdd0/tmp/errno-lv2-does-not-exist", O_RDONLY);
	int e = errno;
	check(fd == -1 && e == ENOENT, "open of a missing file sets ENOENT", e);
	errno = 0;
	long big = strtol("999999999999999999999999", nullptr, 10);
	e = errno;
	check(big == LONG_MAX && e == ERANGE, "strtol overflow sets ERANGE", e);
	// The size and the result go through volatiles: at -O2 GCC may delete a malloc/free pair whose result is only
	// tested against NULL, and then no allocation (and no errno) happens at all.
	static volatile size_t huge = (size_t)-1 / 2;
	static void *volatile sink;
	errno = 0;
	sink = malloc(huge);
	e = errno;
	check(sink == nullptr && e == ENOMEM, "an impossible malloc sets ENOMEM", e);
	free(sink);
	for (int code : {ENOENT, EBUSY, ERANGE, EAGAIN}) {
		const char *s = strerror(code);
		bool named = s && *s && !strstr(s, "Unknown") && !strstr(s, "unknown");
		check(named, "strerror names an LV2 errno value", code);
		printf("errno-lv2: strerror(0x%08x) = \"%s\"\n", (unsigned)code, s ? s : "(null)");
	}

	// 4. C++
	std::error_code ec(ENOENT, std::generic_category());
	check(ec == std::errc::no_such_file_or_directory, "error_code(ENOENT) == errc::no_such_file_or_directory", ec.value());
	check(std::make_error_code(std::errc::resource_unavailable_try_again).value() == EAGAIN,
	      "errc::resource_unavailable_try_again carries EAGAIN");
	check(std::make_error_code(std::errc::device_or_resource_busy).value() == EBUSY, "errc::device_or_resource_busy carries EBUSY");
	std::string msg = ec.message();
	check(!msg.empty() && msg.find("Unknown") == std::string::npos, "generic_category names ENOENT");
	printf("errno-lv2: error_code(ENOENT).message() = \"%s\"\n", msg.c_str());
	bool threw = false;
	try {
		std::thread none;
		none.join();
	} catch (const std::system_error &se) {
		threw = se.code() == std::errc::invalid_argument && se.code().value() == EINVAL;
		printf("errno-lv2: join() threw %d: %s\n", se.code().value(), se.what());
	}
	check(threw, "thread::join on a non-joinable thread throws system_error(EINVAL)");
	std::mutex m;
	m.lock();
	bool got = true;
	std::thread other([&] { got = m.try_lock(); });
	other.join();
	m.unlock();
	check(!got, "std::mutex::try_lock on a mutex another thread holds fails");

	// 5. sockets
	check(netInitialize() == 0, "netInitialize");
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	check(s >= 0, "UDP socket", s);
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_len = sizeof(addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(0x7f000001);
	check(bind(s, (struct sockaddr *)&addr, sizeof(addr)) == 0, "bind to loopback");
	char buf[16];
	errno = 0;
	long r = recv(s, buf, sizeof(buf), MSG_DONTWAIT);
	e = errno;
	check(r == -1 && e == EAGAIN, "recv(MSG_DONTWAIT) with nothing queued sets EAGAIN", e);
	check(close(s) == 0, "close the socket");
	errno = 0;
	r = socketclose(s);
	e = errno;
	check(r == -1 && e == EBADF, "closing a closed socket sets EBADF", e);
	netDeinitialize();

	printf(g_failed ? "ERRNO_LV2_FAIL (%s)\n" : "ERRNO_LV2_OK\n", g_failed);
	fflush(stdout);
	return 0;
}
