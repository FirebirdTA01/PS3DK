/* libmd5: MD5 (RFC 1321). */
#include <cell/hash/libmd5.h>
#include "hash_internal.h"

/* Per-step additive constants: floor(abs(sin(i + 1)) * 2^32). */
static const unsigned int md5_k[64] = {
	0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
	0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
	0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
	0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
	0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
	0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
	0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
	0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

/* Rotation amounts, four per round. */
static const unsigned char md5_r[16] = {
	7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21,
};

static void md5_block(void *state, const unsigned char *p)
{
	unsigned int *h = (unsigned int *)state;
	unsigned int m[16], a = h[0], b = h[1], c = h[2], d = h[3];
	unsigned int i;

	for (i = 0; i < 16; i++)
		m[i] = ps3tc_load_le32(p + 4 * i);
	for (i = 0; i < 64; i++) {
		unsigned int f, g, t;
		switch (i >> 4) {
		case 0:  f = (b & c) | (~b & d); g = i;                break;
		case 1:  f = (d & b) | (~d & c); g = (5 * i + 1) & 15; break;
		case 2:  f = b ^ c ^ d;          g = (3 * i + 5) & 15; break;
		default: f = c ^ (b | ~d);       g = (7 * i) & 15;     break;
		}
		t = d;
		d = c;
		c = b;
		b = b + ps3tc_rol32(a + f + md5_k[i] + m[g], md5_r[(i >> 4) * 4 + (i & 3)]);
		a = t;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
}

static void md5_init(unsigned int *h)
{
	h[0] = 0x67452301;
	h[1] = 0xefcdab89;
	h[2] = 0x98badcfe;
	h[3] = 0x10325476;
}

#define MD5_STORE(h, out) ps3tc_hash_store32(h, out, CELL_MD5_DIGEST_SIZE, 1)

PS3TC_DEFINE_HASH(Md5, CELL_MD5_BLOCK_SIZE, 8, 1, md5_block, md5_init, MD5_STORE)
