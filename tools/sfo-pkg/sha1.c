/* sha1.c - SHA-1, implemented from FIPS PUB 180-4 §6.1.
 *
 * Part of the PS3 Custom Toolchain. MIT, same terms as the rest of this
 * repository. See sha1.h for why this is ours rather than adopted, and
 * sha1-test.c for the specification's test vectors.
 *
 * Section references below are to FIPS PUB 180-4 (August 2015).
 */

#include "sha1.h"

#include <string.h>

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

/* §6.1.2 step 1: message schedule, then the 80-round compression. */
static void sha1_compress(uint32_t h[5], const uint8_t block[64])
{
	uint32_t w[80];

	for (int t = 0; t < 16; t++) {
		w[t] = ((uint32_t)block[t * 4 + 0] << 24) |
		       ((uint32_t)block[t * 4 + 1] << 16) |
		       ((uint32_t)block[t * 4 + 2] << 8) |
		       ((uint32_t)block[t * 4 + 3]);
	}
	for (int t = 16; t < 80; t++) {
		w[t] = ROTL32(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
	}

	uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

	for (int t = 0; t < 80; t++) {
		uint32_t f, k;

		/* §4.1.1: the round function and constant change every 20 rounds. */
		if (t < 20) {
			f = (b & c) | (~b & d);
			k = 0x5A827999u;
		} else if (t < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1u;
		} else if (t < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDCu;
		} else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6u;
		}

		uint32_t tmp = ROTL32(a, 5) + f + e + k + w[t];
		e = d;
		d = c;
		c = ROTL32(b, 30);
		b = a;
		a = tmp;
	}

	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
}

/* §5.3.1: initial hash value. */
void ps3_sha1_init(ps3_sha1_ctx *ctx)
{
	ctx->h[0] = 0x67452301u;
	ctx->h[1] = 0xEFCDAB89u;
	ctx->h[2] = 0x98BADCFEu;
	ctx->h[3] = 0x10325476u;
	ctx->h[4] = 0xC3D2E1F0u;
	ctx->total_bits = 0;
	ctx->block_len = 0;
}

void ps3_sha1_update(ps3_sha1_ctx *ctx, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;

	ctx->total_bits += (uint64_t)len * 8;

	/* Top up a partial block first. */
	if (ctx->block_len > 0) {
		size_t want = 64 - ctx->block_len;
		size_t take = len < want ? len : want;
		memcpy(ctx->block + ctx->block_len, p, take);
		ctx->block_len += take;
		p += take;
		len -= take;
		if (ctx->block_len == 64) {
			sha1_compress(ctx->h, ctx->block);
			ctx->block_len = 0;
		}
	}

	while (len >= 64) {
		sha1_compress(ctx->h, p);
		p += 64;
		len -= 64;
	}

	if (len > 0) {
		memcpy(ctx->block, p, len);
		ctx->block_len = len;
	}
}

/* §5.1.1: append 0x80, pad with zeros, then the 64-bit big-endian length. */
void ps3_sha1_final(ps3_sha1_ctx *ctx, uint8_t digest[PS3_SHA1_DIGEST_SIZE])
{
	uint64_t total_bits = ctx->total_bits;

	ctx->block[ctx->block_len++] = 0x80;

	/* The length field needs the last 8 bytes; if it will not fit, flush. */
	if (ctx->block_len > 56) {
		memset(ctx->block + ctx->block_len, 0, 64 - ctx->block_len);
		sha1_compress(ctx->h, ctx->block);
		ctx->block_len = 0;
	}
	memset(ctx->block + ctx->block_len, 0, 56 - ctx->block_len);

	for (int i = 0; i < 8; i++) {
		ctx->block[56 + i] = (uint8_t)(total_bits >> (56 - 8 * i));
	}
	sha1_compress(ctx->h, ctx->block);

	for (int i = 0; i < 5; i++) {
		digest[i * 4 + 0] = (uint8_t)(ctx->h[i] >> 24);
		digest[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
		digest[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
		digest[i * 4 + 3] = (uint8_t)(ctx->h[i]);
	}

	/* Do not leave the tail of the message sitting in the caller's stack. */
	memset(ctx->block, 0, sizeof(ctx->block));
	ctx->block_len = 0;
}

void ps3_sha1(const void *data, size_t len, uint8_t digest[PS3_SHA1_DIGEST_SIZE])
{
	ps3_sha1_ctx ctx;
	ps3_sha1_init(&ctx);
	ps3_sha1_update(&ctx, data, len);
	ps3_sha1_final(&ctx, digest);
}
