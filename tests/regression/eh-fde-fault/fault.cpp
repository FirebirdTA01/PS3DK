// eh-fde-fault: the GCC 16 PPU unwinder registry's allocation-failure contract (gcc-16.x 0074).  NOT a manifest
// row: two of the four modes are EXPECTED TO DIE, and are judged by their own driver.
//
//   EH_FDE_MODE_EXHAUST      a priority-82 constructor (before the heap, 83, and the syscall table, 84) registers
//                            11 synthetic objects.  The root leaves hold 10, so the 11th needs a node the 2-node
//                            boot reserve does not have: the process must TRAP before main (no "SURVIVED").
//   EH_FDE_MODE_EXHAUST_CTL  the same with 9 objects: fits the root leaves, reaches main ("SURVIVED").
//   EH_FDE_MODE_OOM          after the heap is ready: print "ARMED", make malloc fail, then register up to 30
//                            objects; the root leaves fill after 9 more, so a node allocation is forced, fails,
//                            and the process must ABORT (no "SURVIVED").
//   EH_FDE_MODE_OOM_CTL      the same without making malloc fail ("SURVIVED").
// Nothing here prints before main: stdio needs the syscall table.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
void __register_frame_info(const void *begin, void *ob);
void *__real_malloc(size_t n);
static volatile int g_fail_malloc;
void *__wrap_malloc(size_t n) { return g_fail_malloc ? nullptr : __real_malloc(n); }
}

struct Synth {
	alignas(8) unsigned char code[64];
	alignas(8) unsigned char frame[64];
	alignas(8) unsigned long long ob[16];
};
enum { N = 30 };
static Synth g_s[N];

static void put32(unsigned char *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void build_frame(Synth &s)
{
	// Same image as eh-fde-registry: CIE "zR" udata4, one FDE over s.code; no library calls (runs pre-heap).
	static const unsigned char cie[] = { 0, 0, 0, 0, 1, 'z', 'R', 0, 0x04, 0x78, 0x41, 0x01, 0x03,
	                                     0x0c, 0x01, 0x00 };
	unsigned char *f = s.frame;
	for (unsigned i = 0; i < sizeof s.frame; i++)
		f[i] = 0;
	put32(f, sizeof cie);
	for (unsigned i = 0; i < sizeof cie; i++)
		f[4 + i] = cie[i];
	unsigned char *fde = f + 4 + sizeof cie;
	put32(fde, 16);
	put32(fde + 4, (uint32_t)(fde + 4 - f));
	put32(fde + 8, (uint32_t)(uintptr_t)s.code);
	put32(fde + 12, sizeof s.code);
}

#if defined(EH_FDE_MODE_EXHAUST) || defined(EH_FDE_MODE_EXHAUST_CTL)
#ifdef EH_FDE_MODE_EXHAUST
enum { PRE_HEAP = 11 };
#else
enum { PRE_HEAP = 9 };
#endif
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wprio-ctor-dtor"
__attribute__((constructor(82))) static void pre_heap_registrations()
{
	for (int i = 0; i < PRE_HEAP; i++) {
		build_frame(g_s[i]);
		__register_frame_info(g_s[i].frame, g_s[i].ob);
	}
}
#pragma GCC diagnostic pop
int main()
{
	printf("eh-fde-fault: %d pre-heap registrations\nEH_FDE_FAULT_SURVIVED\n", (int)PRE_HEAP);
	fflush(stdout);
	return 0;
}
#else
int main()
{
	printf("EH_FDE_FAULT_ARMED\n");
	fflush(stdout);
#ifdef EH_FDE_MODE_OOM
	g_fail_malloc = 1;
#endif
	for (int i = 0; i < N; i++) {
		build_frame(g_s[i]);
		__register_frame_info(g_s[i].frame, g_s[i].ob);
	}
	g_fail_malloc = 0;
	printf("eh-fde-fault: %d registrations after the heap\nEH_FDE_FAULT_SURVIVED\n", (int)N);
	fflush(stdout);
	return 0;
}
#endif
