/* libsha384: SHA-384 (FIPS 180-4): the SHA-512 compression with its own
 * initial state, digest truncated to 48 bytes. */
#include <cell/hash/libsha384.h>
#include "sha512_core.h"

static void sha384_init(unsigned long long *h)
{
	h[0] = 0xcbbb9d5dc1059ed8ULL;
	h[1] = 0x629a292a367cd507ULL;
	h[2] = 0x9159015a3070dd17ULL;
	h[3] = 0x152fecd8f70e5939ULL;
	h[4] = 0x67332667ffc00b31ULL;
	h[5] = 0x8eb44a8768581511ULL;
	h[6] = 0xdb0c2e0d64f98fa7ULL;
	h[7] = 0x47b5481dbefa4fa4ULL;
}

#define SHA384_STORE(h, out) ps3tc_hash_store64(h, out, CELL_SHA384_DIGEST_SIZE)

PS3TC_DEFINE_HASH(Sha384, CELL_SHA384_BLOCK_SIZE, 16, 0, sha512_block, sha384_init, SHA384_STORE)
