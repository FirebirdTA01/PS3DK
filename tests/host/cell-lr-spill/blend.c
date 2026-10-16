/* Register pressure of the shape that made GCC 16 park values in LR on the Cell PPE: a masked "over" blend of
   packed 8-bit RGBA pixels, two channels per 32-bit word at a time, with a rounding divide by 255. */
#include <stdint.h>

#define LO 0x00ff00ffu
#define HALF 0x00800080u

static inline uint32_t mul_un8x4(uint32_t x, uint32_t a)
{
	uint32_t lo = (x & LO) * a + HALF;
	uint32_t hi = ((x >> 8) & LO) * a + HALF;

	lo = ((lo + ((lo >> 8) & LO)) >> 8) & LO;
	hi = ((hi + ((hi >> 8) & LO)) >> 8) & LO;
	return lo | (hi << 8);
}

/* x * a + y per channel, saturating. */
static inline uint32_t mul_add_un8x4(uint32_t x, uint32_t a, uint32_t y)
{
	uint32_t lo = (x & LO) * a + HALF;
	uint32_t hi = ((x >> 8) & LO) * a + HALF;

	lo = ((lo + ((lo >> 8) & LO)) >> 8) & LO;
	hi = ((hi + ((hi >> 8) & LO)) >> 8) & LO;
	lo += y & LO;
	hi += (y >> 8) & LO;
	lo = (lo | (0x01000100u - ((lo >> 8) & 0x00010001u))) & LO;
	hi = (hi | (0x01000100u - ((hi >> 8) & 0x00010001u))) & LO;
	return lo | (hi << 8);
}

void blend_over_masked(uint32_t *dst, const uint32_t *src, const uint32_t *mask, int width)
{
	for (int i = 0; i < width; i++) {
		uint32_t m = mask[i] >> 24;

		if (m) {
			uint32_t s = mul_un8x4(src[i], m);
			uint32_t a = s >> 24;

			if (a == 0xff)
				dst[i] = s;
			else if (s)
				dst[i] = mul_add_un8x4(dst[i], a ^ 0xff, s);
		}
	}
}
