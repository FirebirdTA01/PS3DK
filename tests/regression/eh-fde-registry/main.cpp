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
//                  of which prints the pass marker;
//   7. variants    __register_frame_info_bases / _table / _table_bases, __register_frame / __register_frame_table,
//                  each found (with its bases where given) and removed through its matching deregistration.
//   8. lazy sort  four lookup threads contend for the object mutex on a never-looked-up object (one holds it in
//                  the wrapped lock until the others have entered for the same mutex), then four readers look the
//                  published object up: no object-mutex lock under 0074, one per lookup on the list path.
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
#include <sys/timer.h>
#include <unistd.h>

extern "C" {
struct dwarf_eh_bases { void *tbase; void *dbase; void *func; };
void __register_frame_info(const void *begin, void *ob);
void *__deregister_frame_info(const void *begin);
const void *_Unwind_Find_FDE(void *pc, struct dwarf_eh_bases *bases);
void __register_frame_info_bases(const void *begin, void *ob, void *tbase, void *dbase);
void *__deregister_frame_info_bases(const void *begin);
void __register_frame_info_table(void *begin, void *ob);
void __register_frame_info_table_bases(void *begin, void *ob, void *tbase, void *dbase);
void __register_frame(void *begin);
void __deregister_frame(void *begin);
void __register_frame_table(void *begin);
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
static void check(bool ok, const char *name, long got = 0)
{
	if (ok)
		printf("eh-fde-registry: %s ok\n", name);
	else
		printf("eh-fde-registry: %s FAIL (got 0x%lx)\n", name, (unsigned long)got);
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
// Start gate: workers check in and sleep on a condition variable until main releases them all at once.  No
// busy-wait - spinning threads starve each other on LV2 (and a long spin is endless in an interpreter).
static pthread_mutex_t g_gate_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gate_cv = PTHREAD_COND_INITIALIZER;
static int g_go, g_ready;
static void check_in_and_wait()
{
	pthread_mutex_lock(&g_gate_mx);
	g_ready++;
	while (!g_go)
		pthread_cond_wait(&g_gate_cv, &g_gate_mx);
	pthread_mutex_unlock(&g_gate_mx);
}
static int ready_count()
{
	pthread_mutex_lock(&g_gate_mx);
	int r = g_ready;
	pthread_mutex_unlock(&g_gate_mx);
	return r;
}
static std::atomic<int> g_first_ok{0}, g_throw_bad{0}, g_churn_bad{0};
static std::atomic<bool> g_churn_done{false};

static void *thrower(void *arg)
{
	int id = (int)(intptr_t)arg;
	check_in_and_wait();
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
	check_in_and_wait();
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

// --- 7. registration variants -----------------------------------------------------------------------
static Synth g_v[7];
static unsigned char g_empty[16];   // an empty .eh_frame: its first word is the zero terminator

static bool found_with_bases(Synth &s, int off, void *tb, void *db)
{
	dwarf_eh_bases b{};
	const void *f = _Unwind_Find_FDE(s.code + off, &b);
	return f != nullptr && b.func == (void *)s.code && b.tbase == tb && b.dbase == db;
}

// A route whose removal failed leaves its object (and the image it describes) registered.  Registering the same
// storage again would build on that stale registration, so every later check on it stops being an independent
// witness (review: codex).  Such a route is retired: later passes report it skipped and never touch its storage.
static bool g_retired[7];
static bool live(int i, const char *route)
{
	if (g_retired[i])
		printf("eh-fde-registry: variant %s: SKIPPED (an earlier removal failed; its storage stays registered)\n", route);
	return !g_retired[i];
}
static void removed(bool ok, int i, int j = -1)
{
	if (!ok) {
		g_retired[i] = true;
		if (j >= 0)
			g_retired[j] = true;
	}
}

static void variants()
{
	bool built = true;
	for (auto &v : g_v)
		built = build_frame(v) && built;
	check(built, "variants: every synthetic range fits the udata4 encoding");
	if (!built)
		return;   // never register an image whose address udata4 cannot hold
	char tb, db;
	static void *t_tab[2], *t_tabb[2], *t_alloc[2], *t_two[3];
	for (int round = 1; round <= 2; round++) {
		for (int looked = 0; looked <= 1; looked++) {
			printf("eh-fde-registry: variants round %d, %s\n", round,
			       looked ? "deregistered after a lookup (sorted)" : "deregistered with no lookup (unsorted)");
			void *r;
			bool ok;
			// a. __register_frame_info_table: a one-image table, removed by the table address
			if (live(0, "table")) {
				t_tab[0] = g_v[0].frame; t_tab[1] = nullptr;
				__register_frame_info_table(t_tab, g_v[0].ob);
				if (looked)
					check(found(g_v[0], 8), "variant table: found");
				r = __deregister_frame_info(t_tab);
				check(r == (void *)g_v[0].ob, "variant table: deregistration returns its object", (long)(uintptr_t)r);
				check(ok = absent(g_v[0]), "variant table: removed");
				removed(ok && r == (void *)g_v[0].ob, 0);
			}
			// b. __register_frame_info_table_bases
			if (live(1, "table_bases")) {
				t_tabb[0] = g_v[1].frame; t_tabb[1] = nullptr;
				__register_frame_info_table_bases(t_tabb, g_v[1].ob, &tb, &db);
				if (looked)
					check(found_with_bases(g_v[1], 8, &tb, &db), "variant table_bases: found, with its tbase/dbase");
				r = __deregister_frame_info_bases(t_tabb);
				check(r == (void *)g_v[1].ob, "variant table_bases: deregistration returns its object", (long)(uintptr_t)r);
				check(ok = absent(g_v[1]), "variant table_bases: removed");
				removed(ok && r == (void *)g_v[1].ob, 1);
			}
			// c. __register_frame_table: libgcc allocates the object, __deregister_frame frees it
			if (live(2, "frame_table")) {
				t_alloc[0] = g_v[2].frame; t_alloc[1] = nullptr;
				__register_frame_table(t_alloc);
				if (looked)
					check(found(g_v[2], 8), "variant frame_table: found");
				__deregister_frame(t_alloc);
				check(ok = absent(g_v[2]), "variant frame_table: removed by __deregister_frame");
				removed(ok, 2);
			}
			// d. a two-image table: both images found, both gone after one deregistration
			if (live(3, "two-image table")) {
				t_two[0] = g_v[3].frame; t_two[1] = g_v[4].frame; t_two[2] = nullptr;
				__register_frame_info_table(t_two, g_v[3].ob);
				if (looked)
					check(found(g_v[3], 4) && found(g_v[4], 4), "variant two-image table: both images found");
				r = __deregister_frame_info(t_two);
				check(r == (void *)g_v[3].ob, "variant two-image table: deregistration returns its object", (long)(uintptr_t)r);
				check(ok = absent(g_v[3]) && absent(g_v[4]), "variant two-image table: both images removed");
				removed(ok && r == (void *)g_v[3].ob, 3, 4);
			}
			// e. __register_frame_info_bases: the lookup reports the bases it was given
			if (live(5, "info_bases")) {
				__register_frame_info_bases(g_v[5].frame, g_v[5].ob, &tb, &db);
				if (looked)
					check(found_with_bases(g_v[5], 4, &tb, &db), "variant info_bases: found, with its tbase/dbase");
				r = __deregister_frame_info_bases(g_v[5].frame);
				check(ok = r == (void *)g_v[5].ob && absent(g_v[5]), "variant info_bases: removed");
				removed(ok, 5);
			}
			// f. __register_frame: libgcc allocates the object, __deregister_frame frees it
			if (live(6, "register_frame")) {
				__register_frame(g_v[6].frame);
				if (looked)
					check(found(g_v[6], 4), "variant register_frame: found");
				__deregister_frame(g_v[6].frame);
				check(ok = absent(g_v[6]), "variant register_frame: removed by __deregister_frame");
				removed(ok, 6);
			}
		}
	}
	// g. genuine empty .eh_frame images: registering is a no-op, deregistering returns NULL without asserting
	memset(g_empty, 0, sizeof g_empty);
	alignas(8) static unsigned long long empty_ob[16];
	__register_frame_info(g_empty, empty_ob);
	check(__deregister_frame_info(g_empty) == nullptr, "empty .eh_frame: register is a no-op, deregister returns NULL");
	__register_frame(g_empty);
	__deregister_frame(g_empty);
	check(true, "empty .eh_frame: __register_frame / __deregister_frame are no-ops (no assertion)");
}

// --- 8. lazy-sort contention witness ---------------------------------------------------------------------
// In both registry modes a lookup that finds an unsorted object sorts it under libgcc's object mutex; the fast
// path re-checks the sorted flag after taking the mutex and publishes it with a release store (source-derived:
// unwind-dw2-fde.c _Unwind_Find_FDE / init_object, not instrumented here).  The witness makes the contention
// deterministic instead of hoping for it: pthread_mutex_lock/unlock are wrapped, and only calls made by a thread
// inside an explicit _Unwind_Find_FDE probe (thread-local mark), on the FIRST mutex such a call locks, are
// observed.  In the cold phase the first thread to own that mutex is held inside the wrapper - after the real
// lock succeeded, before libgcc gets it back - until every other lookup thread has entered the wrapper for the
// same mutex (contenders reaching the slow path while it is held; not necessarily kernel-blocked), or a 5 s
// deadline releases it and fails coverage.  A separate warm phase of readers then looks the published object up:
// the 0074 fast path must take no object-mutex lock at all, the list path takes one per lookup.
enum { LS_THREADS = 4 };
static Synth g_ls;
static __thread int t_probe;                       // set only around the explicit probe lookups
static std::atomic<void *> g_ls_mutex{nullptr};   // the first mutex a probe lookup locked
static std::atomic<int> g_ls_phase{0};             // 1 cold, 2 warm
static std::atomic<int> g_ls_entries[3], g_ls_acquired[3], g_ls_entered_held[3], g_ls_other_mutex[3];
static std::atomic<int> g_ls_holders{0}, g_ls_max_holders{0}, g_ls_lock_bad{0}, g_ls_unlock_bad{0};
static std::atomic<int> g_ls_owner_taken{0}, g_ls_timeout{0}, g_ls_expected{0}, g_ls_ok[3], g_ls_bad[3];
static std::atomic<int> g_ls_owner_saw_entries{-1}, g_ls_owner_saw_acquired{-1};

extern "C" {
int __real_pthread_mutex_lock(pthread_mutex_t *m);
int __real_pthread_mutex_unlock(pthread_mutex_t *m);
int __wrap_pthread_mutex_lock(pthread_mutex_t *m)
{
	if (!t_probe)
		return __real_pthread_mutex_lock(m);
	int ph = g_ls_phase.load();
	void *want = nullptr;
	g_ls_mutex.compare_exchange_strong(want, (void *)m);
	if (g_ls_mutex.load() != (void *)m) {
		g_ls_other_mutex[ph].fetch_add(1);           // e.g. an allocator lock under init_object: reported only
		return __real_pthread_mutex_lock(m);
	}
	g_ls_entries[ph].fetch_add(1);
	if (g_ls_holders.load() > 0)
		g_ls_entered_held[ph].fetch_add(1);
	int rc = __real_pthread_mutex_lock(m);
	if (rc != 0) {
		g_ls_lock_bad.fetch_add(1);
		return rc;
	}
	g_ls_acquired[ph].fetch_add(1);
	int h = g_ls_holders.fetch_add(1) + 1;
	for (int seen = g_ls_max_holders.load(); h > seen && !g_ls_max_holders.compare_exchange_weak(seen, h);)
		;
	if (ph == 1 && g_ls_owner_taken.exchange(1) == 0) {
		// Hold the mutex until every other lookup thread has entered this wrapper for it (raw LV2 sleeps: no
		// wrapped pthread call while holding), or release at the deadline and fail coverage.
		int ms = 0;
		while (g_ls_entries[1].load() < g_ls_expected.load() && ms < 5000) {
			sys_timer_usleep(1000);
			ms++;
		}
		if (g_ls_entries[1].load() < g_ls_expected.load())
			g_ls_timeout.store(1);
		// What the owner saw while it still held the mutex: the coverage witness.
		g_ls_owner_saw_entries.store(g_ls_entries[1].load());
		g_ls_owner_saw_acquired.store(g_ls_acquired[1].load());
	}
	return rc;
}
int __wrap_pthread_mutex_unlock(pthread_mutex_t *m)
{
	bool mine = t_probe && g_ls_mutex.load() == (void *)m;
	if (mine)
		g_ls_holders.fetch_sub(1);                    // cleared before the real unlock: no false overlap
	int rc = __real_pthread_mutex_unlock(m);
	if (mine && rc != 0)
		g_ls_unlock_bad.fetch_add(1);
	return rc;
}
}

static char g_ls_tb, g_ls_db;
static pthread_mutex_t g_ls_gate_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ls_gate_cv = PTHREAD_COND_INITIALIZER;
static int g_ls_go, g_ls_ready;
static void *ls_reader(void *arg)
{
	int ph = (int)(intptr_t)arg;
	pthread_mutex_lock(&g_ls_gate_mx);          // harness synchronization: unmarked, not counted
	g_ls_ready++;
	while (g_ls_go != ph)
		pthread_cond_wait(&g_ls_gate_cv, &g_ls_gate_mx);
	pthread_mutex_unlock(&g_ls_gate_mx);
	t_probe = 1;
	dwarf_eh_bases b{};
	const void *f = _Unwind_Find_FDE(g_ls.code + 8, &b);
	t_probe = 0;
	bool ok = f != nullptr && b.func == (void *)g_ls.code && b.tbase == &g_ls_tb && b.dbase == &g_ls_db;
	(ok ? g_ls_ok[ph] : g_ls_bad[ph]).fetch_add(1);
	return nullptr;
}
// One phase: create LS_THREADS readers, wait (bounded) until they check in, release them together, join.
static int ls_phase(int ph)
{
	pthread_t t[LS_THREADS];
	int created = 0;
	for (int i = 0; i < LS_THREADS; i++) {
		int rc = pthread_create(&t[i], nullptr, ls_reader, (void *)(intptr_t)ph);
		if (rc != 0) {
			printf("eh-fde-registry: lazy-sort phase %d pthread_create %d returned %d\n", ph, i, rc);
			break;
		}
		created++;
	}
	g_ls_expected.store(created);
	int ready = 0;
	for (int ms = 0; ms < 10000; ms++) {
		pthread_mutex_lock(&g_ls_gate_mx);
		ready = g_ls_ready;
		pthread_mutex_unlock(&g_ls_gate_mx);
		if (ready == created)
			break;
		usleep(1000);
	}
	g_ls_phase.store(ph);
	pthread_mutex_lock(&g_ls_gate_mx);
	g_ls_go = ph;
	g_ls_ready = 0;
	pthread_cond_broadcast(&g_ls_gate_cv);
	pthread_mutex_unlock(&g_ls_gate_mx);
	int joined = 0;
	for (int i = 0; i < created; i++)
		joined += pthread_join(t[i], nullptr) == 0;
	check(created == LS_THREADS, ph == 1 ? "lazy-sort cold phase: all reader threads created"
	                                     : "lazy-sort warm phase: all reader threads created", created);
	check(ready == created, ph == 1 ? "lazy-sort cold phase: every reader reached the start line"
	                                : "lazy-sort warm phase: every reader reached the start line", ready);
	check(joined == created, ph == 1 ? "lazy-sort cold phase: every reader joined"
	                                 : "lazy-sort warm phase: every reader joined", joined);
	return created;
}
static void lazy_sort_witness(bool btree_mode)
{
	if (!build_frame(g_ls)) {
		check(false, "lazy-sort: the synthetic range fits the udata4 encoding");
		return;
	}
	// Registered once, never looked up before the cold phase, kept registered until every reader has joined.
	__register_frame_info_bases(g_ls.frame, g_ls.ob, &g_ls_tb, &g_ls_db);
	int cold = ls_phase(1);
	g_ls_phase.store(0);
	int warm = ls_phase(2);
	g_ls_phase.store(0);
	void *r = __deregister_frame_info_bases(g_ls.frame);
	check(r == (void *)g_ls.ob && absent(g_ls), "lazy-sort: the object deregisters after every reader joined",
	      (long)(uintptr_t)r);
	printf("eh-fde-registry: lazy-sort cold: %d readers, object-mutex lock entries %d, acquisitions %d, entered "
	       "while held %d, max holders %d, other mutexes %d; owner, before releasing, saw %d entries and %d "
	       "acquisition(s)%s\n", cold, g_ls_entries[1].load(), g_ls_acquired[1].load(), g_ls_entered_held[1].load(),
	       g_ls_max_holders.load(), g_ls_other_mutex[1].load(), g_ls_owner_saw_entries.load(),
	       g_ls_owner_saw_acquired.load(), g_ls_timeout.load() ? " - DEADLINE" : "");
	printf("eh-fde-registry: lazy-sort warm: %d readers, object-mutex lock entries %d, acquisitions %d, other "
	       "mutexes %d\n", warm, g_ls_entries[2].load(), g_ls_acquired[2].load(), g_ls_other_mutex[2].load());
	check(g_ls_lock_bad.load() == 0 && g_ls_unlock_bad.load() == 0, "lazy-sort: every observed lock/unlock succeeded");
	check(g_ls_max_holders.load() == 1, "lazy-sort: the object mutex never had two holders", g_ls_max_holders.load());
	check(!g_ls_timeout.load() && g_ls_owner_saw_entries.load() == cold && g_ls_owner_saw_acquired.load() == 1,
	      "lazy-sort cold phase: every other reader reached the object mutex while the first held it",
	      g_ls_owner_saw_entries.load());
	check(g_ls_acquired[1].load() == cold, "lazy-sort cold phase: every reader acquired the object mutex once",
	      g_ls_acquired[1].load());
	check(g_ls_ok[1].load() == cold && g_ls_bad[1].load() == 0,
	      "lazy-sort cold phase: every contended lookup found the FDE with its tbase/dbase", g_ls_bad[1].load());
	check(g_ls_ok[2].load() == warm && g_ls_bad[2].load() == 0,
	      "lazy-sort warm phase: every reader found the FDE with its tbase/dbase", g_ls_bad[2].load());
	if (btree_mode)
		check(g_ls_entries[2].load() == 0, "lazy-sort warm phase (0074): no object-mutex lock - the published fast path",
		      g_ls_entries[2].load());
	else
		check(g_ls_entries[2].load() == warm, "lazy-sort warm phase (list): every lookup takes the object mutex",
		      g_ls_entries[2].load());
}

int main()
{
	// 1. bootstrap, read before this program allocates anything itself
	unsigned malloc_before_heap = g_malloc_before_heap;
	bool have_flag = &__ps3dk_heap_ready != nullptr;
	bool have_counters = &__gcc_ps3_fde_boot_nodes != nullptr && &__gcc_ps3_fde_boot_registrations != nullptr &&
	                     &__gcc_ps3_fde_heap_nodes != nullptr;
	unsigned boot_regs = have_counters ? __atomic_load_n(&__gcc_ps3_fde_boot_registrations, __ATOMIC_ACQUIRE) : 0;
	unsigned boot_nodes = have_counters ? __atomic_load_n(&__gcc_ps3_fde_boot_nodes, __ATOMIC_ACQUIRE) : 0;
	// The mode is the LIBRARY's: a libgcc with the 0074 registry exports its counters and requires the runtime's
	// flag, so in that mode a missing flag is a failure, never a fallback to the list-registry checks.
	printf("eh-fde-registry: abi %s, registry mode %s, heap-ready flag %s\n", sizeof(void *) == 8 ? "lp64" : "ilp32",
	       have_counters ? "0074-btree" : "list (no 0074 counters)", have_flag ? "present" : "absent");
	if (have_counters)
		check(have_flag, "0074 mode: the runtime's heap-ready flag is linked");
	if (have_flag)
		check(malloc_before_heap == 0, "bootstrap: no malloc call before the heap is ready");
	if (have_counters) {
		printf("eh-fde-registry: pre-heap registrations %u, reserve high-water %u, heap nodes %u\n",
		       boot_regs, boot_nodes, __atomic_load_n(&__gcc_ps3_fde_heap_nodes, __ATOMIC_ACQUIRE));
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
	// Bounded wait (10 s, sleeping) for the created threads to check in, then release them together.
	for (int ms = 0; ready_count() != created && ms < 10000; ms++)
		usleep(1000);
	check(ready_count() == created, "every created thread reached the start line");
	pthread_mutex_lock(&g_gate_mx);
	g_go = 1;
	pthread_cond_broadcast(&g_gate_cv);
	pthread_mutex_unlock(&g_gate_mx);
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

	// 7. registration variants (review: codex).  Every route is checked twice per round - deregistered with no
	//    lookup in between, and after a lookup (which sorts the object) - over two rounds (so re-registration after
	//    removal is checked too), plus a two-image table and genuine empty-frame controls.
	variants();

	// 8. lazy-sort contention witness (review: codex): deterministic contention on the object mutex, then a
	//    warm phase that must take the 0074 fast path without the mutex.
	lazy_sort_witness(have_counters);

	// 5. growth past the root leaves, release, reuse
	unsigned heap0 = have_counters ? __atomic_load_n(&__gcc_ps3_fde_heap_nodes, __ATOMIC_ACQUIRE) : 0;
	for (auto &s : g_many)
		__register_frame_info(s.frame, s.ob);
	bool all = true;
	for (auto &s : g_many)
		all = found(s, 12) && all;
	check(all, "growth: thirty more objects are all found");
	unsigned heap1 = have_counters ? __atomic_load_n(&__gcc_ps3_fde_heap_nodes, __ATOMIC_ACQUIRE) : 0;
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
	unsigned heap2 = have_counters ? __atomic_load_n(&__gcc_ps3_fde_heap_nodes, __ATOMIC_ACQUIRE) : 0;
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
