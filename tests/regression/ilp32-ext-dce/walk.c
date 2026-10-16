/* The loop shape that GCC 16's ext-dce miscompiled under ILP32 (built at -O2 in its own translation unit): the
   Adler-32 checksum, written out here.  A pointer is walked in long blocks of 16-byte steps, then in 16-byte steps,
   then byte by byte.  Final value replacement forms the pointer after the 16-byte loop as a 32-bit expression that
   wraps; ext-dce dropped its zero-extension into the 64-bit address register, and the byte loop then read from an
   address with its upper half set. */
#include <stdint.h>

#define BASE  65521u
#define BLOCK 5552u	/* largest n with 255n(n+1)/2 + (n+1)(BASE-1) < 2^32 */

#define STEP(i) { a += buf[i]; b += a; }
#define STEP16  STEP(0) STEP(1) STEP(2) STEP(3) STEP(4) STEP(5) STEP(6) STEP(7) \
                STEP(8) STEP(9) STEP(10) STEP(11) STEP(12) STEP(13) STEP(14) STEP(15)

uint32_t walk_sum(uint32_t seed, const unsigned char *buf, unsigned int len)
{
	uint32_t a = seed & 0xffff, b = seed >> 16;

	if (len < 16) {
		while (len--) {
			a += *buf++;
			b += a;
		}
		return (a % BASE) | ((b % BASE) << 16);
	}
	while (len >= BLOCK) {
		unsigned int n = BLOCK / 16;

		len -= BLOCK;
		do {
			STEP16
			buf += 16;
		} while (--n);
		a %= BASE;
		b %= BASE;
	}
	if (len) {
		while (len >= 16) {
			len -= 16;
			STEP16
			buf += 16;
		}
		while (len--) {
			a += *buf++;
			b += a;
		}
		a %= BASE;
		b %= BASE;
	}
	return a | (b << 16);
}
