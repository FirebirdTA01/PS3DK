/* libsha1: SHA-1 (FIPS 180-4, section 6.1). */
#include <cell/hash/libsha1.h>
#include "hash_internal.h"

static void sha1_block(void *state, const unsigned char *p)
{
	unsigned int *h = (unsigned int *)state;
	unsigned int w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
	unsigned int i;

	for (i = 0; i < 16; i++)
		w[i] = ps3tc_load_be32(p + 4 * i);
	for (; i < 80; i++)
		w[i] = ps3tc_rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	for (i = 0; i < 80; i++) {
		unsigned int f, k, t;
		if (i < 20) {
			f = (b & c) | (~b & d);
			k = 0x5a827999;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ed9eba1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8f1bbcdc;
		} else {
			f = b ^ c ^ d;
			k = 0xca62c1d6;
		}
		t = ps3tc_rol32(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = ps3tc_rol32(b, 30);
		b = a;
		a = t;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
}

static void sha1_init(unsigned int *h)
{
	h[0] = 0x67452301;
	h[1] = 0xefcdab89;
	h[2] = 0x98badcfe;
	h[3] = 0x10325476;
	h[4] = 0xc3d2e1f0;
}

#define SHA1_STORE(h, out) ps3tc_hash_store32(h, out, CELL_SHA1_DIGEST_SIZE, 0)

PS3TC_DEFINE_HASH(Sha1, CELL_SHA1_BLOCK_SIZE, 8, 0, sha1_block, sha1_init, SHA1_STORE)
