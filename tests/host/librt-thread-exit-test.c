/* Controlled boundary for the verbatim thread-exit and module thread bodies
 * (librt-thread-exit-test.py fills in the @...@ markers).  The order string
 * records every boundary call in sequence:
 *
 *   T  r13 read (pt_tls_pointer)        S  TLS read (pt_self_cookie)
 *   C  librt exit cleanup               L  liblv2's sys_ppu_thread_exit
 *   R  bare exit system call 41         N  thread created (sysThreadCreate)
 *   E  user entry ran
 */
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int32_t s32;
typedef uint64_t u64;
typedef uint64_t sys_ppu_thread_t;

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; fprintf(stderr, "FAIL line %d: %s (order %s)\n", __LINE__, #x, order); } } while (0)

static char order[128];
static size_t order_n;
static void note(char c) { if (order_n < sizeof(order) - 1) order[order_n++] = c; order[order_n] = 0; }
static void reset(void) { order_n = 0; order[0] = 0; }

/* --- r13 and TLS ---------------------------------------------------------- */
static uint64_t fake_r13;
static uint64_t pt_tls_pointer(void) { note('T'); return fake_r13; }
static void *cookie_value = (void *)&cookie_value;
static void **tls_access(void) { note('S'); return &cookie_value; }
#define pt_self_cookie (*tls_access())

/* --- exits ---------------------------------------------------------------- */
static jmp_buf exit_jump;
static uint64_t exit_code_seen;
static void *cleanup_self;
static void pt_thread_cleanup(void *self, void *retval) { (void)retval; cleanup_self = self; note('C'); }
static void __attribute__((noreturn)) pt_raw_thread_exit(uint64_t code) { note('R'); exit_code_seen = code; longjmp(exit_jump, 1); }
static void __attribute__((noreturn)) sysThreadExit(uint64_t code) { note('L'); exit_code_seen = code; longjmp(exit_jump, 1); }

/* --- thread creation -------------------------------------------------------- */
static s32 create_rc;
static unsigned creates;
static void (*last_tramp)(void *);
static void *last_arg;
static uint64_t last_flags;
static s32 sysThreadCreate(sys_ppu_thread_t *id, void (*entry)(void *), void *arg, s32 prio,
                           u64 stacksize, u64 flags, char *name)
{
	(void)prio; (void)stacksize; (void)name;
	note('N');
	++creates;
	last_tramp = entry;
	last_arg = arg;
	last_flags = flags;
	if (create_rc == 0)
		*id = 0x0100 + creates;
	return create_rc;
}

/* @TLS_CHECK@ */

/* Executable: runtime/lv2/librt/pthread.c */
/* @EXE_EXIT@ */

/* Module: runtime/lv2/crt/lv2-prx-thread.c */
/* @PRX_EXIT@ */
/* @PRX_POOL@ */

static uint64_t exit_code_wanted;

static int exit_exe(uint64_t r13, uint64_t code)
{
	reset(); exit_code_seen = 0; fake_r13 = r13; exit_code_wanted = code;
	if (!setjmp(exit_jump))
		__lv2_thread_exit(exit_code_wanted);
	return exit_code_seen == exit_code_wanted;
}

static int exit_prx(uint64_t r13, uint64_t code)
{
	reset(); exit_code_seen = 0; fake_r13 = r13; exit_code_wanted = code;
	if (!setjmp(exit_jump))
		prx___lv2_thread_exit(exit_code_wanted);
	return exit_code_seen == exit_code_wanted;
}

static uint64_t entry_arg;
static void user_entry(uint64_t arg) { note('E'); entry_arg = arg; }

/* Start the thread the last create made: run its trampoline to the exit. */
static void run_started(void)
{
	if (!setjmp(exit_jump))
		last_tramp(last_arg);
}

static int pool_free(void)
{
	int n = 0;
	for (unsigned i = 0; i < PRX_START_RECORDS; i++)
		n += prx_start_records[i].busy == 0;
	return n;
}

int main(void)
{
	static const uint64_t no_tls[] = { 0, 1, 0x6fff, 0x100000000ull, 0xffffffffffff7000ull };
	static const uint64_t with_tls[] = { 0x7000, 0x10007030, 0xd0007030, 0xffffffff };

	/* A thread with no TLS area: r13 is read, nothing else, then the bare
	 * system call ends it with its exit code. */
	for (size_t i = 0; i < sizeof(no_tls) / sizeof(no_tls[0]); i++) {
		CHECK(exit_exe(no_tls[i], 0x1234 + i));
		CHECK(strcmp(order, "TR") == 0);
		CHECK(exit_prx(no_tls[i], 0x5678 + i));
		CHECK(strcmp(order, "TR") == 0);
	}
	/* A thread with TLS: the executable keeps its cleanup (on the thread's
	 * own slot) and ends through liblv2; a module ends through liblv2. */
	for (size_t i = 0; i < sizeof(with_tls) / sizeof(with_tls[0]); i++) {
		cleanup_self = NULL;
		CHECK(exit_exe(with_tls[i], 0xabcdef0123456789ull));
		CHECK(strcmp(order, "TSCL") == 0);
		CHECK(cleanup_self == cookie_value);
		CHECK(exit_prx(with_tls[i], 42));
		CHECK(strcmp(order, "TL") == 0);
	}

	/* Module create: the full 64-bit argument arrives, the record is free
	 * again once the thread starts, and a return from the entry is exit 0. */
	sys_ppu_thread_t id = 0;
	fake_r13 = 0x10007030;
	reset(); create_rc = 0;
	CHECK(__lv2_ppu_thread_create(&id, user_entry, 0x8877665544332211ull, 1000, 0x4000, 1, "t") == 0);
	CHECK(id != 0 && last_flags == 1);
	CHECK(pool_free() == PRX_START_RECORDS - 1);
	run_started();
	CHECK(entry_arg == 0x8877665544332211ull);
	CHECK(exit_code_seen == 0);
	CHECK(strcmp(order, "NETL") == 0);
	CHECK(pool_free() == PRX_START_RECORDS);

	/* The same on a creator and a thread with no TLS area. */
	fake_r13 = 0;
	reset(); entry_arg = 0; exit_code_seen = 99;
	CHECK(__lv2_ppu_thread_create(&id, user_entry, 7, 1000, 0x4000, 1, "t") == 0);
	run_started();
	CHECK(entry_arg == 7 && exit_code_seen == 0);
	CHECK(strcmp(order, "NETR") == 0);
	CHECK(pool_free() == PRX_START_RECORDS);

	/* A failed create gives its record back. */
	reset(); create_rc = (s32)0x80010004;
	CHECK(__lv2_ppu_thread_create(&id, user_entry, 1, 1000, 0x4000, 1, "t") == (int)0x80010004);
	CHECK(pool_free() == PRX_START_RECORDS);
	create_rc = 0;

	/* Every record held by a created, not yet started thread: EAGAIN without
	 * a create; one starting frees a record for the next. */
	for (unsigned i = 0; i < PRX_START_RECORDS; i++)
		CHECK(__lv2_ppu_thread_create(&id, user_entry, i, 1000, 0x4000, 1, "t") == 0);
	CHECK(pool_free() == 0);
	unsigned before = creates;
	CHECK(__lv2_ppu_thread_create(&id, user_entry, 99, 1000, 0x4000, 1, "t") == (int)PRX_LV2_EAGAIN);
	CHECK(creates == before);
	fake_r13 = 0x10007030;
	run_started();
	CHECK(pool_free() == 1);
	CHECK(__lv2_ppu_thread_create(&id, user_entry, 100, 1000, 0x4000, 1, "t") == 0);
	for (unsigned i = 0; i < PRX_START_RECORDS; i++)
		prx_start_records[i].busy = 0;

	/* Interrupt threads take no record and get the entry directly. */
	reset();
	CHECK(__lv2_ppu_thread_create(&id, user_entry, 5, 1000, 0x4000, 2, "irq") == 0);
	CHECK(last_flags == 2 && last_arg == (void *)(uintptr_t)5);
	CHECK(pool_free() == PRX_START_RECORDS);

	printf("librt-thread-exit: checks=%u failures=%u\n", checks, failures);
	if (failures == 0)
		printf("LIBRT_THREAD_EXIT_OK\n");
	return failures ? 1 : 0;
}
