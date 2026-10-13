/* libsha224: SHA-224 (FIPS 180-4): the SHA-256 compression with its own
 * initial state, digest truncated to 28 bytes. */
#include <cell/hash/libsha224.h>
#include "sha256_core.h"

static void sha224_init(unsigned int *h)
{
	h[0] = 0xc1059ed8;
	h[1] = 0x367cd507;
	h[2] = 0x3070dd17;
	h[3] = 0xf70e5939;
	h[4] = 0xffc00b31;
	h[5] = 0x68581511;
	h[6] = 0x64f98fa7;
	h[7] = 0xbefa4fa4;
}

#define SHA224_STORE(h, out) ps3tc_hash_store32(h, out, CELL_SHA224_DIGEST_SIZE, 0)

PS3TC_DEFINE_HASH(Sha224, CELL_SHA224_BLOCK_SIZE, 8, 0, sha256_block, sha224_init, SHA224_STORE)
