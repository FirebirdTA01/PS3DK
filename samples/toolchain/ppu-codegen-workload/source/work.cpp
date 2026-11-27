// Per-frame PPU work for ppu-codegen-workload.  Each phase is a fixed amount of
// work dominated by one code-generation pattern, so a compiler or library
// change that targets that pattern moves that phase's frame time.
//
// The inputs are built once (work_init) and never change; every phase returns
// a checksum so two builds can be compared for identical results.
#include "work.h"

#include <sys/synchronization.h>
#include <new>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

enum { N = 16384 };

char g_text[N * 8];          // decimal numbers separated by commas
int16_t g_h16[N];
uint32_t g_keys[N];
uint8_t g_trip[N];

typedef float v4sf __attribute__((vector_size(16)));
v4sf g_pos[N / 4], g_vel[N / 4];

uint32_t g_seed = 12345;
uint32_t rnd() { return g_seed = g_seed * 1664525u + 1013904223u; }

uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

// --- phase 0: text parsing (byte loads, sign extension, flag tests) --------
__attribute__((noinline)) uint32_t parse_ints(const char *s, const int16_t *w, int n)
{
	uint32_t h = 2166136261u;
	int idx = 0;
	while (*s && idx < n) {
		int neg = 0, v = 0;
		if (*s == '-') {
			neg = 1;
			s++;
		}
		while ((unsigned)(*s - '0') < 10u)
			v = v * 10 + (*s++ - '0');
		if (*s == ',')
			s++;
		int32_t x = (neg ? -v : v) + w[idx & (N - 1)];
		if (x & 0x40)
			x ^= 0x155;
		if ((x & 0x3000) == 0)
			x += 3;
		h = mix(h, (uint32_t)x);
		idx++;
	}
	return h;
}

// --- phase 1: comparison-heavy code (sorting with a predicate) -------------
struct Rec { uint32_t key; int16_t pri; uint16_t id; };
Rec g_recs[2048];

__attribute__((noinline)) bool rec_less(const Rec &a, const Rec &b)
{
	if (a.pri != b.pri)
		return a.pri < b.pri;
	return a.key < b.key;
}

__attribute__((noinline)) uint32_t sort_recs(int n)
{
	// Insertion sort on a fresh copy: many short, data-dependent compares.
	static Rec tmp[2048];
	memcpy(tmp, g_recs, sizeof(Rec) * n);
	for (int i = 1; i < n; i++) {
		Rec x = tmp[i];
		int j = i - 1;
		while (j >= 0 && rec_less(x, tmp[j])) {
			tmp[j + 1] = tmp[j];
			j--;
		}
		tmp[j + 1] = x;
	}
	uint32_t h = 2166136261u;
	for (int i = 0; i < n; i += 7)
		h = mix(h, tmp[i].key ^ tmp[i].id);
	return h;
}

// --- phase 2: many counted loops with short trip counts ---------------------
__attribute__((noinline)) uint32_t short_loops(const uint8_t *trip, const uint32_t *k, int n)
{
	uint32_t s = 0;
	for (int i = 0; i < n; i++) {
		uint32_t x = k[i];
		for (uint32_t j = 0; j < trip[i]; j++)
			x = x * 2654435761u + j;
		s += x;
	}
	return s;
}

// --- phase 3: vector math with per-lane tests --------------------------------
__attribute__((noinline)) uint32_t particles(v4sf *pos, const v4sf *vel, int n)
{
	const v4sf dt = { 0.016f, 0.016f, 0.016f, 0.016f };
	uint32_t hits = 0;
	for (int i = 0; i < n; i++) {
		v4sf p = pos[i] + vel[i] * dt;
		if (p[1] > 100.0f)        // y above the ceiling
			p[1] = 0.0f;
		if (p[3] == 0.0f)         // marker lane
			hits++;
		pos[i] = p;
	}
	return hits;
}

// --- phase 4: SPRX import calls (lightweight mutex, uncontended) ------------
alignas(128) sys_lwmutex_t g_mtx;
bool g_mtx_ok = false;

__attribute__((noinline)) uint32_t import_calls(int n)
{
	uint32_t ok = 0;
	for (int i = 0; i < n; i++) {
		ok += sys_lwmutex_lock(&g_mtx, 0) == 0;
		ok += sys_lwmutex_unlock(&g_mtx) == 0;
	}
	return ok;
}

// --- phase 5: C++ objects: vector-aligned allocation and virtual calls -----
struct Shape {
	virtual ~Shape() {}
	virtual float area() const = 0;
};
struct alignas(16) Box : Shape {
	v4sf extent;
	float area() const override { return extent[0] * extent[1]; }
};
struct alignas(16) Disc : Shape {
	v4sf centre_r;
	float area() const override { return 3.14159f * centre_r[3] * centre_r[3]; }
};

__attribute__((noinline)) uint32_t objects(int n)
{
	static Shape *live[256];
	float total = 0;
	for (int i = 0; i < n; i++) {
		Shape *&slot = live[i & 255];
		delete slot;
		if (i & 1) {
			Box *b = new Box;
			b->extent = (v4sf){ (float)(i & 15), 2.0f, 0.0f, 0.0f };
			slot = b;
		} else {
			Disc *d = new Disc;
			d->centre_r = (v4sf){ 0.0f, 0.0f, 0.0f, (float)(i & 7) };
			slot = d;
		}
		total += slot->area();
	}
	return (uint32_t)(int32_t)total;
}

} // namespace

const char *const work_phase_names[WORK_PHASES] = {
	"parse", "sort-preds", "short-loops", "vector-lanes", "imports", "cxx-objects",
};

int work_init(void)
{
	char *t = g_text;
	for (int i = 0; i < N; i++) {
		int v = (int)(rnd() >> 16) - 32768;
		t += snprintf(t, 16, "%d,", v % 100000);
		g_h16[i] = (int16_t)rnd();
		g_keys[i] = rnd();
		g_trip[i] = (uint8_t)(1 + (rnd() >> 30));
	}
	*t = 0;
	for (int i = 0; i < 2048; i++) {
		g_recs[i].key = rnd();
		g_recs[i].pri = (int16_t)((rnd() >> 28) - 8);
		g_recs[i].id = (uint16_t)i;
	}
	for (int i = 0; i < N / 4; i++) {
		g_pos[i] = (v4sf){ (float)(rnd() & 255), (float)(rnd() & 127), 0.0f, (float)((rnd() >> 16) & 3) };
		g_vel[i] = (v4sf){ 1.0f, (float)(rnd() & 31), -1.0f, 0.0f };
	}
	sys_lwmutex_attribute_t attr;
	sys_lwmutex_attribute_initialize(attr);
	g_mtx_ok = sys_lwmutex_create(&g_mtx, &attr) == 0;
	return g_mtx_ok ? 0 : -1;
}

uint32_t work_frame(int phase)
{
	switch (phase) {
	case 0: return parse_ints(g_text, g_h16, N) ^ parse_ints(g_text + 1000, g_h16, N / 2);
	case 1: return sort_recs(900);
	case 2: return short_loops(g_trip, g_keys, N) + short_loops(g_trip + 1, g_keys + 1, N - 1);
	case 3: {
		uint32_t h = 0;
		for (int r = 0; r < 12; r++)
			h += particles(g_pos, g_vel, N / 4);
		return h;
	}
	case 4: return import_calls(6000);
	case 5: return objects(6000);
	}
	return 0;
}
