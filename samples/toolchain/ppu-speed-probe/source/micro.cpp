// Code-generation microkernels for the PPU speed probe.
//
// Each kernel is a small loop whose hot path is dominated by one class of
// instruction or one calling pattern, so a compiler change that targets that
// class shows up as a timing change here first.  All kernels return a checksum
// so two builds can be compared for identical results as well as for speed.
//
// The data comes from the caller's LCG and is regenerated before each run, so
// nothing here can be folded at compile time.
#include "micro.h"

#include <sys/synchronization.h>
#include <stdint.h>
#include <string.h>

namespace {

enum { N = 8192 };

int16_t g_h16[N];
int32_t g_w32[N];
uint32_t g_u32a[N], g_u32b[N];
uint8_t g_trip[N];
int32_t g_out[N];

typedef int32_t v4si __attribute__((vector_size(16)));
v4si g_vec[N / 4];

// The inputs are generated once, outside the timed runs: each run resets the
// generator, so the data would be identical anyway, and generating it inside
// the timed region made every microkernel's time depend on the generator's
// call path rather than on the kernel.
bool g_filled = false;

void fill(uint32_t (*rng)())
{
	if (g_filled)
		return;
	g_filled = true;
	for (int i = 0; i < N; i++) {
		uint32_t r = rng();
		g_h16[i] = (int16_t)r;
		g_w32[i] = (int32_t)(r ^ (r << 7));
		g_u32a[i] = r;
		g_u32b[i] = rng();
		g_trip[i] = (uint8_t)(1 + (r >> 30)); // 1..4
	}
	for (int i = 0; i < N / 4; i++) {
		v4si v = { g_w32[4 * i] & 15, g_w32[4 * i + 1] & 15, g_w32[4 * i + 2] & 15,
		           g_w32[4 * i + 3] & 15 };
		g_vec[i] = v;
	}
}

uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

// Signed halfword and word loads widened to a larger type.
__attribute__((noinline)) int64_t sext_sum(const int16_t *h, const int32_t *w, int n)
{
	int64_t s = 0;
	for (int i = 0; i < n; i++) {
		int32_t a = h[i];
		int64_t b = w[i];
		s += (a < 0) ? b - a : b + a;
	}
	return s;
}

// AND-with-immediate tests feeding branches (record forms on PowerPC).
__attribute__((noinline)) uint32_t flag_tests(const uint32_t *a, int n)
{
	uint32_t c = 0;
	for (int i = 0; i < n; i++) {
		uint32_t v = a[i];
		if (v & 0x10)
			c += 3;
		if (v & 0x300)
			c ^= v >> 3;
		if ((v & 0xff0000) == 0)
			c += 7;
		if (v & 0x80000000u)
			c = c * 5 + 1;
	}
	return c;
}

// Comparison results stored as integers (set-on-condition).  Kept scalar:
// the vectorizer would otherwise turn the loop into vector compares.
__attribute__((noinline, optimize("no-tree-vectorize"))) void store_preds(const uint32_t *a, const uint32_t *b, int32_t *out, int n)
{
	for (int i = 0; i < n; i++) {
		int32_t sa = (int32_t)a[i], sb = (int32_t)b[i];
		out[i] = (a[i] < b[i]) + 2 * (sa > sb) + 4 * (a[i] == (b[i] & 0xffff)) +
		         8 * (sa != 0);
	}
}

// Many counted loops with very short trip counts (loop-branch prediction).
__attribute__((noinline)) uint32_t short_loops(const uint8_t *trip, const uint32_t *a, int n)
{
	uint32_t s = 0;
	for (int i = 0; i < n; i++) {
		uint32_t t = trip[i];
		uint32_t x = a[i];
		for (uint32_t k = 0; k < t; k++)
			x = x * 2654435761u + k;
		s += x;
	}
	return s;
}

// One vector lane tested against a constant.
__attribute__((noinline)) uint32_t lane_tests(const v4si *v, int n)
{
	// The lane is tested on a vector computed in registers (not one loaded
	// from memory), so the compiler must move it out of the vector unit.
	const v4si one = { 1, 2, 3, 4 };
	uint32_t c = 0;
	for (int i = 0; i < n; i++) {
		v4si x = v[i] * v[i] + one;
		if (x[2] == 52)
			c++;
		if (x[0] > 101)
			c += 2;
	}
	return c;
}

} // namespace

uint32_t k_micro_sext(uint32_t (*rng)())
{
	fill(rng);
	uint32_t h = 2166136261u;
	for (int r = 0; r < 16; r++)
		h = mix(h, (uint32_t)sext_sum(g_h16, g_w32, N));
	return h;
}

uint32_t k_micro_flags(uint32_t (*rng)())
{
	fill(rng);
	uint32_t h = 2166136261u;
	for (int r = 0; r < 16; r++)
		h = mix(h, flag_tests(g_u32a, N));
	return h;
}

uint32_t k_micro_preds(uint32_t (*rng)())
{
	fill(rng);
	uint32_t h = 2166136261u;
	for (int r = 0; r < 16; r++) {
		store_preds(g_u32a, g_u32b, g_out, N);
		for (int i = 0; i < N; i += 61)
			h = mix(h, (uint32_t)g_out[i]);
	}
	return h;
}

uint32_t k_micro_shortloops(uint32_t (*rng)())
{
	fill(rng);
	uint32_t h = 2166136261u;
	for (int r = 0; r < 16; r++)
		h = mix(h, short_loops(g_trip, g_u32a, N));
	return h;
}

uint32_t k_micro_lanes(uint32_t (*rng)())
{
	fill(rng);
	uint32_t h = 2166136261u;
	for (int r = 0; r < 64; r++)
		h = mix(h, lane_tests(g_vec, N / 4));
	return h;
}

// Calls through an SPRX import stub: uncontended lightweight-mutex
// lock/unlock pairs (both are imports from the system PRX).
uint32_t k_micro_imports(uint32_t (*rng)())
{
	(void)rng;
	static sys_lwmutex_t mtx;
	static bool made = false;
	if (!made) {
		sys_lwmutex_attribute_t attr;
		sys_lwmutex_attribute_initialize(attr);
		if (sys_lwmutex_create(&mtx, &attr) != 0)
			return 0;
		made = true;
	}
	uint32_t ok = 0;
	for (int i = 0; i < 20000; i++) {
		ok += sys_lwmutex_lock(&mtx, 0) == 0;
		ok += sys_lwmutex_unlock(&mtx) == 0;
	}
	return ok;
}
