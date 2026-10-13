/* libsha512: SHA-512 (FIPS 180-4). */
#include <cell/hash/libsha512.h>
#include "sha512_core.h"

static void sha512_init(unsigned long long *h)
{
	h[0] = 0x6a09e667f3bcc908ULL;
	h[1] = 0xbb67ae8584caa73bULL;
	h[2] = 0x3c6ef372fe94f82bULL;
	h[3] = 0xa54ff53a5f1d36f1ULL;
	h[4] = 0x510e527fade682d1ULL;
	h[5] = 0x9b05688c2b3e6c1fULL;
	h[6] = 0x1f83d9abfb41bd6bULL;
	h[7] = 0x5be0cd19137e2179ULL;
}

#define SHA512_STORE(h, out) ps3tc_hash_store64(h, out, CELL_SHA512_DIGEST_SIZE)

PS3TC_DEFINE_HASH(Sha512, CELL_SHA512_BLOCK_SIZE, 16, 0, sha512_block, sha512_init, SHA512_STORE)
