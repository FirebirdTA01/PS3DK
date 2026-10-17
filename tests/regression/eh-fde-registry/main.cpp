// eh-fde-registry: the unwinder's FDE registry under startup, concurrency, registration churn and teardown.
//
// The GCC 16 PPU libgcc looks FDEs up through a lock-free b-tree (gcc-16.x 0074) instead of a mutex-held list, and
// takes its first nodes from a static reserve because crtbegin registers the program from _init, before the
// prioritized constructors create the heap (83) and the syscall table (84).  This row exercises what the registry
// must keep doing, on any toolchain (plain pthreads, so 12.4 builds it too):
//   1. bootstrap   no malloc call is made before the runtime reports the heap ready (malloc is wrapped and counts
//                  the calls made before __ps3dk_heap_ready is set), and - where libgcc exports its counters - one
//                  registration was made before the heap and the reserve's high-water is its two root leaves;
//   2. startup     a throw caught in a priority-101 constructor (after the lock init at 85) and in a
//                  default-priority static constructor;
//   3. concurrent-start stress: four threads released together make their first lookups into a freshly
//                  registered object.  Release-together does not prove that their lazy sorts overlap;
//   4. churn       one thread registers, looks up and deregisters a separate, never-executed FDE range over and
//                  over while four threads throw and catch through real frames;
//   5. growth      thirty more objects grow the trees past their root leaves (nodes from malloc once the heap is
//                  ready), are deregistered, and registered again (reusing the released nodes);
//   6. teardown    throws caught inside a static object's destructor and inside destructor functions, the last
//                  of which prints the pass marker.
// Code that is still running is never deregistered: unloading live code is the caller's error, not a guarantee.
//
// The synthetic objects are hand-built .eh_frame images (one CIE, one FDE, absolute 4-byte pointers, so the
// described address must fit in 32 bits - asserted, true of this target's low static data in both ABIs)
// describing 64 bytes of a static array that is never executed.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <stdexcept>

extern "C" {
struct dwarf_eh_bases { void *tbase; void *dbase; void *func; };
void __register_frame_info(const void *begin, void *ob);
void *__deregister_frame_info(const void *begin);
const void *_Unwind_Find_FDE(void *pc, struct dwarf_eh_bases *bases);
// crt1.c (runtime) and libgcc (gcc-16.x 0074) instrumentation; absent on other toolchains.
extern int __ps3dk_heap_ready __attribute__((weak));
extern unsigned __gcc_ps3_fde_boot_registrations __attribute__((weak));
extern unsigned __gcc_ps3_fde_boot_nodes __attribute__((weak));
extern unsigned __gcc_ps3_fde_heap_nodes __attribute__((weak));
// malloc is wrapped (-Wl,--wrap=malloc): calls before the heap is ready are counted.
void *__real_malloc(size_t n);
static unsigned g_malloc_before_heap;
void *__wrap_malloc(size_t n)
{
	if (&__ps3dk_heap_ready && !__atomic_load_n(&__ps3dk_heap_ready, __ATOMIC_ACQUIRE))
		g_malloc_before_heap++;
	return __real_malloc(n);
}
}

static const char *g_failed = nullptr;
static void check(bool ok, const char *name)
{
	printf("eh-fde-registry: %s %s\n", name, ok ? "ok" : "FAIL");
	fflush(stdout);
	if (!ok && !g_failed)
		g_failed = name;
}

// --- real frames to unwind through ---------------------------------------------------------------
__attribute__((noinline)) static int leaf(int v)
{
	if (v >= 0)
		throw std::runtime_error("leaf");
	return v;
}
__attribute__((noinline)) static int mid(int v) { return leaf(v) + 1; }
__attribute__((noinline)) static int top(int v) { return mid(v) + 1; }
static int throw_catch(int v)
{
	try {
		return top(v);
	} catch (const std::runtime_error &e) {
		return v + (int)strlen(e.what());   // "leaf" -> v + 4
	}
}

// --- 2. startup ----------------------------------------------------------------------------------
static int g_ctor101 = -1, g_ctor_default = -1;
__attribute__((constructor(101))) static void early_ctor() { g_ctor101 = throw_catch(10); }
struct DefaultCtor { DefaultCtor() { g_ctor_default = throw_catch(20); } };
static DefaultCtor g_default_ctor;

// --- synthetic FDE objects -----------------------------------------------------------------------
struct Synth {
	alignas(8) unsigned char code[64];       // the described range; never executed
	alignas(8) unsigned char frame[64];      // CIE + FDE + terminator
	alignas(8) unsigned long long ob[16];    // libgcc's struct object lives here
};
enum { MANY = 30 };
static Synth g_first, g_churn, g_many[MANY];

static void put32(unsigned char *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static bool build_frame(Synth &s)
{
	if ((uintptr_t)s.code > 0xffffffffu - sizeof s.code)
		return false;                         // udata4 cannot describe it
	unsigned char *f = s.frame;
	memset(f, 0, sizeof s.frame);
	// CIE: id 0, version 1, "zR", code align 4, data align -8, RA r65, aug len 1, FDE encoding udata4,
	// DW_CFA_def_cfa r1 0; 16 bytes after the length.
	static const unsigned char cie[] = { 0, 0, 0, 0, 1, 'z', 'R', 0, 0x04, 0x78, 0x41, 0x01, 0x03,
	                                     0x0c, 0x01, 0x00 };
	put32(f, sizeof cie);
	memcpy(f + 4, cie, sizeof cie);
	unsigned char *fde = f + 4 + sizeof cie;
	// FDE: length 16, CIE pointer (back to the CIE start), pc_begin, pc_range 64, aug len 0, 3 nops.
	put32(fde, 16);
	put32(fde + 4, (uint32_t)(fde + 4 - f));
	put32(fde + 8, (uint32_t)(uintptr_t)s.code);
	put32(fde + 12, sizeof s.code);
	fde[16] = 0;                              // augmentation data length; fde[17..19] = DW_CFA_nop
	return true;                              // the zero terminator follows at fde + 20
}
static bool found(Synth &s, int off)
{
	dwarf_eh_bases b{};
	const void *f = _Unwind_Find_FDE(s.code + off, &b);
	return f != nullptr && b.func == (void *)s.code;
}
static bool absent(Synth &s)
{
	dwarf_eh_bases b{};
	return _Unwind_Find_FDE(s.code + 8, &b) == nullptr;
}

// --- 3. concurrent start + 4. churn --------------------------------------------------------------
enum { THROWERS = 4, ROUNDS = 1500, CHURN = 400 };
static std::atomic<int> g_go{0}, g_ready{0};
static std::atomic<int> g_first_ok{0}, g_throw_bad{0}, g_churn_bad{0};
static std::atomic<bool> g_churn_done{false};

static void *thrower(void *arg)
{
	int id = (int)(intptr_t)arg;
	g_ready.fetch_add(1);
	while (!g_go.load()) {}
	if (found(g_first, 4 * id))
		g_first_ok.fetch_add(1);
	int bad = 0;
	for (int i = 0; i < ROUNDS || !g_churn_done.load(); i++)
		if (throw_catch(id * 100000 + i) != id * 100000 + i + 4)
			bad++;
	g_throw_bad.fetch_add(bad);
	return nullptr;
}
static void *churner(void *)
{
	g_ready.fetch_add(1);
	while (!g_go.load()) {}
	int bad = 0;
	for (int i = 0; i < CHURN; i++) {
		__register_frame_info(g_churn.frame, g_churn.ob);
		if (!found(g_churn, 4 * (i % 16)))
			bad++;
		if (__deregister_frame_info(g_churn.frame) != (void *)g_churn.ob)
			bad++;
		if (!absent(g_churn))
			bad++;
	}
	g_churn_bad.store(bad);
	g_churn_done.store(true);
	return nullptr;
}

// --- 6. teardown ---------------------------------------------------------------------------------
static int g_tear_static = -1, g_tear_default = -1;
struct TearDown { ~TearDown() { g_tear_static = throw_catch(30); } };
static TearDown g_teardown;
__attribute__((destructor)) static void late_dtor_default() { g_tear_default = throw_catch(40); }
__attribute__((destructor(101))) static void late_dtor_101()
{
	// Destructor priority 101 runs after the default ones: the last unwinder use in the process.
	int last = throw_catch(50);
	check(g_tear_static == 34, "a throw caught in a static object's destructor");
	check(g_tear_default == 44, "a throw caught in a default-priority destructor function");
	check(last == 54, "a throw caught in a priority-101 destructor function (last)");
	printf(g_failed ? "EH_FDE_REGISTRY_FAIL (%s)\n" : "EH_FDE_REGISTRY_TEARDOWN_OK\n", g_failed);
	fflush(stdout);
}

int main()
{
	// 1. bootstrap, read before this program allocates anything itself
	unsigned malloc_before_heap = g_malloc_before_heap;
	bool have_flag = &__ps3dk_heap_ready != nullptr;
	bool have_counters = &__gcc_ps3_fde_boot_nodes != nullptr && &__gcc_ps3_fde_boot_registrations != nullptr &&
	                     &__gcc_ps3_fde_heap_nodes != nullptr;
	unsigned boot_regs = have_counters ? __gcc_ps3_fde_boot_registrations : 0;
	unsigned boot_nodes = have_counters ? __gcc_ps3_fde_boot_nodes : 0;
	printf("eh-fde-registry: abi %s, heap-ready flag %s, libgcc reserve counters %s\n",
	       sizeof(void *) == 8 ? "lp64" : "ilp32", have_flag ? "present" : "absent", have_counters ? "present" : "absent");
	if (have_flag)
		check(malloc_before_heap == 0, "bootstrap: no malloc call before the heap is ready");
	if (have_counters) {
		printf("eh-fde-registry: pre-heap registrations %u, reserve high-water %u, heap nodes %u\n",
		       boot_regs, boot_nodes, __gcc_ps3_fde_heap_nodes);
		check(boot_regs == 1, "bootstrap: exactly the crtbegin registration happens before the heap");
		check(boot_nodes == 2, "bootstrap: the reserve high-water is the two root leaves");
	}

	check(g_ctor101 == 14, "startup: a throw caught in a priority-101 constructor");
	check(g_ctor_default == 24, "startup: a throw caught in a static constructor");

	bool built = build_frame(g_first) && build_frame(g_churn);
	for (auto &s : g_many)
		built = build_frame(s) && built;
	check(built, "every synthetic range fits the udata4 encoding");
	if (!built) {
		printf("EH_FDE_REGISTRY_MAIN_FAIL (address width)\n");
		return 1;
	}
	__register_frame_info(g_first.frame, g_first.ob);

	pthread_t t[THROWERS + 1];
	int created = 0;
	for (int i = 0; i < THROWERS + 1; i++) {
		int rc = i < THROWERS ? pthread_create(&t[i], nullptr, thrower, (void *)(intptr_t)(i + 1))
		                      : pthread_create(&t[i], nullptr, churner, nullptr);
		if (rc != 0) {
			printf("eh-fde-registry: pthread_create %d returned %d\n", i, rc);
			break;
		}
		created++;
	}
	check(created == THROWERS + 1, "all five threads created");
	// Bounded wait for the created threads to be ready, then release them together.
	for (long spin = 0; g_ready.load() != created && spin < 2000000000L; spin++) {}
	check(g_ready.load() == created, "every created thread reached the start line");
	g_go.store(1);
	if (created < THROWERS + 1)
		g_churn_done.store(true);            // no churner: let the throwers stop after their rounds
	int joined = 0;
	for (int i = 0; i < created; i++)
		joined += pthread_join(t[i], nullptr) == 0;
	check(joined == created, "every created thread joined");

	if (created == THROWERS + 1) {
		check(g_first_ok.load() == THROWERS, "concurrent-start lookups into a new object all find its FDE");
		check(g_throw_bad.load() == 0, "concurrent throws through real frames all caught with the right value");
		check(g_churn_bad.load() == 0, "register / look up / deregister churn of a separate range while others unwind");
	}
	check(found(g_first, 60), "the persistent object is still found after the churn");

	// 5. growth past the root leaves, release, reuse
	unsigned heap0 = have_counters ? __gcc_ps3_fde_heap_nodes : 0;
	for (auto &s : g_many)
		__register_frame_info(s.frame, s.ob);
	bool all = true;
	for (auto &s : g_many)
		all = found(s, 12) && all;
	check(all, "growth: thirty more objects are all found");
	unsigned heap1 = have_counters ? __gcc_ps3_fde_heap_nodes : 0;
	bool gone = true;
	for (auto &s : g_many)
		gone = __deregister_frame_info(s.frame) == (void *)s.ob && absent(s) && gone;
	check(gone, "growth: all thirty deregister and are gone");
	for (auto &s : g_many)
		__register_frame_info(s.frame, s.ob);
	all = true;
	for (auto &s : g_many)
		all = found(s, 20) && all;
	check(all, "growth: registered again, all found");
	unsigned heap2 = have_counters ? __gcc_ps3_fde_heap_nodes : 0;
	if (have_counters) {
		printf("eh-fde-registry: heap nodes before growth %u, after %u, after re-registration %u\n", heap0, heap1, heap2);
		check(heap1 > heap0, "growth: the trees took nodes from the heap once it was ready");
		check(heap2 == heap1, "growth: re-registration reused the released nodes");
	}
	for (auto &s : g_many)
		__deregister_frame_info(s.frame);

	check(__deregister_frame_info(g_first.frame) == (void *)g_first.ob && absent(g_first),
	      "deregistering the persistent object removes it");
	printf(g_failed ? "EH_FDE_REGISTRY_MAIN_FAIL (%s)\n" : "EH_FDE_REGISTRY_MAIN_OK\n", g_failed);
	fflush(stdout);
	return 0;
}
