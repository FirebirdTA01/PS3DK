/* sha1.h - SHA-1, implemented from FIPS PUB 180-4.
 *
 * Part of the PS3 Custom Toolchain. MIT, same terms as the rest of this
 * repository.
 *
 * Written for this project rather than adopted: upstream PSL1GHT's
 * tools/sfo_pkg ships a SHA-1 by Paul E. Jones (1998) under a bespoke
 * "freeware" notice that grants distribution but is silent on modification
 * and sublicensing, which does not sit comfortably inside an MIT tree that
 * we build into shipped binaries. SHA-1 is a published standard, so the
 * clean answer is to implement it from the specification and prove it with
 * the specification's own test vectors (see sha1-test.c).
 *
 * PKG generation is the only consumer here. SHA-1 is cryptographically
 * broken for collision resistance and must not be used for anything
 * security-bearing; it is required here purely because the PKG container
 * format specifies it.
 */

#ifndef PS3TC_SHA1_H
#define PS3TC_SHA1_H

#include <stddef.h>
#include <stdint.h>

#define PS3_SHA1_DIGEST_SIZE 20

typedef struct {
	uint32_t h[5];        /* running state */
	uint64_t total_bits;  /* message length, in bits */
	uint8_t  block[64];   /* partial block */
	size_t   block_len;   /* bytes currently buffered in `block` */
} ps3_sha1_ctx;

void ps3_sha1_init(ps3_sha1_ctx *ctx);
void ps3_sha1_update(ps3_sha1_ctx *ctx, const void *data, size_t len);
void ps3_sha1_final(ps3_sha1_ctx *ctx, uint8_t digest[PS3_SHA1_DIGEST_SIZE]);

/* One-shot convenience wrapper. */
void ps3_sha1(const void *data, size_t len, uint8_t digest[PS3_SHA1_DIGEST_SIZE]);

#endif /* PS3TC_SHA1_H */
