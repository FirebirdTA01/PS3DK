// PPU speed probe: compares two builds of the same program (two compilers, two
// library builds, or two code-generation options) on real hardware.
//
// Each kernel runs once to warm up, then RUNS times timed with the PPU timebase;
// min and median are printed with a checksum of the kernel's output, so builds can
// be compared for speed AND for identical results.  The library kernels exercise
// real code (pixman, libpng, zlib, newlib, libstdc++); the micro kernels in
// micro.cpp each stress one instruction class or calling pattern.
#include <pixman.h>
#include <png.h>
#include <zlib.h>
#include <sys/sys_time.h>
#include <algorithm>
#include <map>
#include <stdexcept>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "micro.h"

enum { RUNS = 7 };
static uint32_t lcg_state = 12345;
static uint32_t lcg() { return lcg_state = lcg_state * 1664525u + 1013904223u; }
static uint32_t fnv(const void *p, size_t n, uint32_t h = 2166136261u)
{
	const unsigned char *b = (const unsigned char *)p;
	for (size_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 16777619u;
	return h;
}

// --- pixman, float combiners (pixman-combine-float.c) --------------------------------------
static uint32_t k_pixman_float()
{
	enum { W = 128, H = 128 };
	static float src[W * H * 4], dst[W * H * 4];
	for (int i = 0; i < W * H * 4; i++) {
		src[i] = (float)(i % 251) / 251.0f;
		dst[i] = (float)(i % 127) / 127.0f;
	}
	pixman_image_t *s = pixman_image_create_bits(PIXMAN_rgba_float, W, H, (uint32_t *)src, W * 16);
	pixman_image_t *d = pixman_image_create_bits(PIXMAN_rgba_float, W, H, (uint32_t *)dst, W * 16);
	static const pixman_op_t ops[] = { PIXMAN_OP_OVER, PIXMAN_OP_ADD, PIXMAN_OP_MULTIPLY, PIXMAN_OP_SCREEN,
	                                   PIXMAN_OP_OVERLAY, PIXMAN_OP_SOFT_LIGHT, PIXMAN_OP_DIFFERENCE };
	for (pixman_op_t op : ops)
		pixman_image_composite32(op, s, NULL, d, 0, 0, 0, 0, 0, 0, W, H);
	pixman_image_unref(s);
	pixman_image_unref(d);
	return fnv(dst, sizeof dst);
}

// --- pixman, 8888 with an a8 mask -----------------------------------------------------------
static uint32_t k_pixman_8888()
{
	enum { W = 256, H = 256 };
	static uint32_t src[W * H], dst[W * H];
	static uint8_t mask[W * H];
	for (int i = 0; i < W * H; i++) {
		src[i] = 0x80000000u | (i * 2654435761u >> 8);
		dst[i] = 0xff000000u | (i * 40503u);
		mask[i] = (uint8_t)(i * 7);
	}
	pixman_image_t *s = pixman_image_create_bits(PIXMAN_a8r8g8b8, W, H, src, W * 4);
	pixman_image_t *m = pixman_image_create_bits(PIXMAN_a8, W, H, (uint32_t *)mask, W);
	pixman_image_t *d = pixman_image_create_bits(PIXMAN_a8r8g8b8, W, H, dst, W * 4);
	for (int r = 0; r < 8; r++)
		pixman_image_composite32(PIXMAN_OP_OVER, s, m, d, 0, 0, 0, 0, 0, 0, W, H);
	pixman_image_unref(s);
	pixman_image_unref(m);
	pixman_image_unref(d);
	return fnv(dst, sizeof dst);
}

// --- libpng decode (pngread.c) --------------------------------------------------------------
static std::vector<uint8_t> png_blob;
static void png_setup()
{
	enum { W = 256, H = 256 };
	std::vector<uint8_t> px(W * H * 4);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			uint8_t *p = &px[(y * W + x) * 4];
			p[0] = (uint8_t)(x ^ y);
			p[1] = (uint8_t)(x + (lcg() >> 29));
			p[2] = (uint8_t)(y * 3);
			p[3] = 255;
		}
	png_image img;
	memset(&img, 0, sizeof img);
	img.version = PNG_IMAGE_VERSION;
	img.width = W;
	img.height = H;
	img.format = PNG_FORMAT_RGBA;
	png_alloc_size_t size = 0;
	png_image_write_to_memory(&img, NULL, &size, 0, px.data(), 0, NULL);
	png_blob.resize(size);
	png_image_write_to_memory(&img, png_blob.data(), &size, 0, px.data(), 0, NULL);
	png_blob.resize(size);
}
static uint32_t k_png_decode()
{
	uint32_t h = 2166136261u;
	for (int r = 0; r < 3; r++) {
		png_image img;
		memset(&img, 0, sizeof img);
		img.version = PNG_IMAGE_VERSION;
		if (!png_image_begin_read_from_memory(&img, png_blob.data(), png_blob.size()))
			return 0;
		img.format = PNG_FORMAT_RGBA;
		std::vector<uint8_t> out(PNG_IMAGE_SIZE(img));
		if (!png_image_finish_read(&img, NULL, out.data(), 0, NULL))
			return 0;
		h = fnv(out.data(), out.size(), h);
	}
	return h;
}

// --- zlib round trip ------------------------------------------------------------------------
static uint32_t k_zlib()
{
	enum { N = 256 * 1024 };
	static unsigned char in[N], comp[N + N / 8 + 64], back[N];
	static const char *words[] = { "cell ", "spu ", "ppu ", "rsx ", "lv2 ", "gcm ", "dma ", "ls " };
	size_t o = 0;
	while (o + 8 < N) {
		const char *w = words[lcg() >> 29];
		size_t l = strlen(w);
		memcpy(in + o, w, l);
		o += l;
	}
	memset(in + o, ' ', N - o);
	uLongf cl = sizeof comp, bl = N;
	compress2(comp, &cl, in, N, 6);
	uncompress(back, &bl, comp, cl);
	return fnv(comp, cl) ^ fnv(back, bl) ^ (uint32_t)cl;
}

// --- newlib printf / strtod -----------------------------------------------------------------
static uint32_t k_printf()
{
	char buf[128];
	uint32_t h = 2166136261u;
	double acc = 0;
	for (int i = 0; i < 4000; i++) {
		double v = (double)(int32_t)lcg() / 1000.0;
		int n = snprintf(buf, sizeof buf, "%d %.6f %e %s %x", i, v, v * 1e-7, "tok", (unsigned)i * 2654435761u);
		h = fnv(buf, n, h);
		acc += strtod(buf + n / 3, NULL);
	}
	return h ^ (uint32_t)(int64_t)acc;
}

// --- libstdc++ containers -------------------------------------------------------------------
static uint32_t k_cxx()
{
	std::vector<uint32_t> v(30000);
	for (auto &x : v)
		x = lcg();
	std::sort(v.begin(), v.end());
	std::map<uint32_t, uint32_t> m;
	for (int i = 0; i < 8000; i++)
		m[lcg() % 20000] += i;
	uint32_t h = fnv(v.data(), v.size() * 4);
	for (auto &kv : m)
		h = (h ^ kv.first ^ kv.second) * 16777619u;
	std::string s;
	for (int i = 0; i < 3000; i++)
		s += std::to_string(i) + ",";
	return fnv(s.data(), s.size(), h);
}

// --- C++ exceptions -------------------------------------------------------------------------
__attribute__((noinline)) static int thrower(int i)
{
	if (i % 3 != 7)
		throw std::runtime_error("x");
	return i;
}
__attribute__((noinline)) static int mid(int i) { return thrower(i) + 1; }
static uint32_t k_eh()
{
	uint32_t c = 0;
	for (int i = 0; i < 1500; i++) {
		try {
			c += mid(i);
		} catch (const std::exception &e) {
			c += (uint32_t)strlen(e.what()) + i;
		}
	}
	return c;
}

struct Kernel { const char *name; uint32_t (*fn)(); };
static const Kernel kernels[] = {
	{ "pixman-float", k_pixman_float }, { "pixman-8888", k_pixman_8888 }, { "png-decode", k_png_decode },
	{ "zlib", k_zlib }, { "printf-strtod", k_printf }, { "cxx-stl", k_cxx }, { "cxx-eh", k_eh },
	{ "u-sext", [] { return k_micro_sext(lcg); } }, { "u-flags", [] { return k_micro_flags(lcg); } },
	{ "u-preds", [] { return k_micro_preds(lcg); } }, { "u-shortloops", [] { return k_micro_shortloops(lcg); } },
	{ "u-lanes", [] { return k_micro_lanes(lcg); } }, { "u-imports", [] { return k_micro_imports(lcg); } },
};

int main()
{
	uint64_t f = sys_time_get_timebase_frequency();
	printf("BENCH compiler=%s timebase=%llu runs=%d\n", __VERSION__, (unsigned long long)f, RUNS);
	png_setup();
	for (const Kernel &k : kernels) {
		lcg_state = 12345;
		uint32_t sum = k.fn();
		uint64_t t[RUNS];
		bool same = true;
		for (int r = 0; r < RUNS; r++) {
			lcg_state = 12345;
			uint64_t a = __builtin_ppc_get_timebase();
			uint32_t s = k.fn();
			t[r] = __builtin_ppc_get_timebase() - a;
			same = same && s == sum;
		}
		std::sort(t, t + RUNS);
		printf("BENCH %-14s min %10llu med %10llu ticks  min_us %9.1f  sum %08x %s\n", k.name,
		       (unsigned long long)t[0], (unsigned long long)t[RUNS / 2], t[0] * 1e6 / (double)f, sum,
		       same ? "stable" : "UNSTABLE");
	}
	printf("BENCH_DONE\n");
	return 0;
}
