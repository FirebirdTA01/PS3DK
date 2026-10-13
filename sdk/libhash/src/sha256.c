/* libsha256: SHA-256 (FIPS 180-4). */
#include <cell/hash/libsha256.h>
#include "sha256_core.h"

static void sha256_init(unsigned int *h)
{
	h[0] = 0x6a09e667;
	h[1] = 0xbb67ae85;
	h[2] = 0x3c6ef372;
	h[3] = 0xa54ff53a;
	h[4] = 0x510e527f;
	h[5] = 0x9b05688c;
	h[6] = 0x1f83d9ab;
	h[7] = 0x5be0cd19;
}

#define SHA256_STORE(h, out) ps3tc_hash_store32(h, out, CELL_SHA256_DIGEST_SIZE, 0)

PS3TC_DEFINE_HASH(Sha256, CELL_SHA256_BLOCK_SIZE, 8, 0, sha256_block, sha256_init, SHA256_STORE)
