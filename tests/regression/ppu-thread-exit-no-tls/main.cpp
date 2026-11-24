// ppu-thread-exit-no-tls: sys_ppu_thread_exit on a thread with no TLS area (card t_cfb77564).
//
//  1. no-tls     A thread made with the bare create system call (52) and no TLS area (r13 = 0), the way the kernel
//                makes the thread Cobra runs a VSH plugin's module_start on.  It ends with sys_ppu_thread_exit.
//                Before the fix the exit ran librt's cleanup, which reads TLS through r13, and the thread faulted;
//                now it ends through system call 41 and the joiner gets its 64-bit exit code.
//  2. raw-tls    A sysThreadCreate thread (TLS area, no librt record) that sets a pthread key with a destructor and
//                first-uses a thread_local with a destructor, then calls sys_ppu_thread_exit: the code arrives and
//                both destructors still run.
//  3. cell-tls   The same on a thread made with the SDK's sys_ppu_thread_create.
//
// One verdict line per case, then PPU_THREAD_EXIT_NO_TLS_OK or PPU_THREAD_EXIT_NO_TLS_FAIL.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <pthread.h>
#include <sys/ppu_thread.h>
#include <sys/thread.h>
#include <sys/lv2_syscall.h>

static int fails;

static void verdict(const char *name, bool ok, uint64_t got, uint64_t want)
{
	std::printf("PPU_THREAD_EXIT_NO_TLS %s case=%s got=0x%016llx want=0x%016llx\n", ok ? "PASS" : "FAIL", name,
	            (unsigned long long)got, (unsigned long long)want);
	std::fflush(stdout);
	if (!ok)
		fails++;
}

// --- 1. a thread with no TLS area --------------------------------------------------------------------------------
// The entry touches nothing but its argument: r13 is 0 here.
static const uint64_t k_no_tls_code = 0xC0FFEE0012345678ull;
static void no_tls_entry(uint64_t arg)
{
	sys_ppu_thread_exit(arg ^ k_no_tls_code);
}

// _sys_ppu_thread_create's parameter block: the entry's function descriptor and the TLS pointer.
struct bare_thread_param {
	uint32_t entry;
	uint32_t tls;
};

static int bare_create(sys_ppu_thread_t *id, void (*entry)(uint64_t), uint64_t arg)
{
	static const char name[] = "no-tls";
	static bare_thread_param param;
	param.entry = (uint32_t)(uintptr_t)entry;
	param.tls = 0;
	uint64_t tid = 0;
	{
		lv2syscall8(52, (uint64_t)(uintptr_t)&tid, (uint64_t)(uintptr_t)&param, arg, 0, 1000, 0x10000,
		            SYS_PPU_THREAD_CREATE_JOINABLE, (uint64_t)(uintptr_t)name);
		if ((int32_t)p1 != 0)
			return (int32_t)p1;
	}
	{
		lv2syscall1(53, tid);
		if ((int32_t)p1 != 0)
			return (int32_t)p1;
	}
	*id = tid;
	return 0;
}

// --- 2 and 3. threads with TLS: the cleanup still runs -------------------------------------------------------------
static pthread_key_t key;
static std::atomic<int> key_dtors{0}, tl_dtors{0};
static void key_dtor(void *) { ++key_dtors; }

struct Tl {
	int x = 0;
	~Tl() { ++tl_dtors; }
};
static thread_local Tl tl;

static void tls_body(uint64_t arg)
{
	pthread_setspecific(key, (void *)(uintptr_t)(arg | 1));
	tl.x = (int)arg;
	sys_ppu_thread_exit(arg * 3);
}
static void raw_entry(void *arg) { tls_body((uint64_t)(uintptr_t)arg); }
static void cell_entry(uint64_t arg) { tls_body(arg); }

int main()
{
	std::printf("ppu-thread-exit-no-tls (%s)\n", sizeof(void *) == 8 ? "lp64" : "ilp32");
	std::fflush(stdout);

	sys_ppu_thread_t id = 0;
	uint64_t v = 0;
	int rc = bare_create(&id, no_tls_entry, 0x1111);
	if (rc != 0) {
		verdict("no-tls-create", false, (uint32_t)rc, 0);
	} else {
		rc = sys_ppu_thread_join(id, &v);
		verdict("no-tls", rc == 0 && v == (0x1111 ^ k_no_tls_code), v, 0x1111 ^ k_no_tls_code);
	}

	if (pthread_key_create(&key, key_dtor) != 0) {
		verdict("key-create", false, 0, 0);
	} else {
		v = 0;
		rc = sysThreadCreate(&id, raw_entry, (void *)(uintptr_t)0x2222, 1000, 0x10000, THREAD_JOINABLE,
		                     (char *)"raw-tls");
		rc = rc == 0 ? sys_ppu_thread_join(id, &v) : rc;
		verdict("raw-tls", rc == 0 && v == 0x2222 * 3, v, 0x2222 * 3);
		verdict("raw-tls-dtors", key_dtors == 1 && tl_dtors == 1, (uint64_t)key_dtors * 16 + tl_dtors, 0x11);

		v = 0;
		rc = sys_ppu_thread_create(&id, cell_entry, 0x3333, 1000, 0x10000, SYS_PPU_THREAD_CREATE_JOINABLE,
		                           "cell-tls");
		rc = rc == 0 ? sys_ppu_thread_join(id, &v) : rc;
		verdict("cell-tls", rc == 0 && v == 0x3333 * 3, v, 0x3333 * 3);
		verdict("cell-tls-dtors", key_dtors == 2 && tl_dtors == 2, (uint64_t)key_dtors * 16 + tl_dtors, 0x22);
	}

	std::printf(fails ? "PPU_THREAD_EXIT_NO_TLS_FAIL fails=%d\n" : "PPU_THREAD_EXIT_NO_TLS_OK fails=%d\n", fails);
	std::fflush(stdout);
	return fails ? 1 : 0;
}
