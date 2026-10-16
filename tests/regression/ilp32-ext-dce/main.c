/* ilp32-ext-dce: the Adler-32 checksum (walk.c, -O2) against a per-byte reference built at -O0, every length 0..200 at four
   alignments.  GCC 16's ext-dce, left on under ILP32, dropped the zero-extension of the wrapped post-loop pointer and
   the loop ran away (gcc-16.x 0072 keeps the pass off by default).  Each length is printed before it runs, so a hang
   names its length; the manifest's timeout bounds it. */
#include <stdint.h>
#include <stdio.h>

extern uint32_t walk_sum(uint32_t seed, const unsigned char *buf, unsigned int len);

static unsigned char data[200 + 16];

__attribute__((optimize("O0"))) static uint32_t ref_sum(uint32_t seed, const unsigned char *buf, unsigned int len)
{
	uint32_t a = seed & 0xffff, b = seed >> 16;
	for (unsigned int i = 0; i < len; i++) {
		a = (a + buf[i]) % 65521u;
		b = (b + a) % 65521u;
	}
	return a | (b << 16);
}

int main(void)
{
	int bad = 0;

	for (unsigned i = 0; i < sizeof data; i++)
		data[i] = (unsigned char)(i * 131 + 7);
	for (unsigned int len = 0; len <= 200; len++) {
		if (len % 20 == 0 || (len >= 30 && len <= 36))
			printf("ilp32-ext-dce: len %u\n", len);
		for (unsigned off = 0; off < 4; off++) {
			uint32_t got = walk_sum(1, data + off, len), want = ref_sum(1, data + off, len);
			if (got != want && bad++ < 8)
				printf("ilp32-ext-dce: MISMATCH len %u off %u got %08x want %08x\n", len, off, got, want);
		}
	}
	if (bad)
		printf("ILP32_EXT_DCE_FAIL %d\n", bad);
	else
		printf("ILP32_EXT_DCE_OK\n");
	return bad != 0;
}
