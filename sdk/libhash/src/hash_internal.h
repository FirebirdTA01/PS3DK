/*
 * Shared engine of the message-digest libraries: block buffering, the
 * Merkle-Damgard length padding and the four public entry points, stamped
 * out per algorithm by PS3TC_DEFINE_HASH.  Each algorithm supplies only its
 * initial state, its block (compression) function and its digest store.
 *
 * Byte order is handled with explicit shifts, so the same sources build and
 * give the same digests on the PPU and on a little-endian host (the host
 * leg of tests/sdk/hash-vectors-test.sh relies on that).
 */

#ifndef PS3TC_LIBHASH_INTERNAL_H
#define PS3TC_LIBHASH_INTERNAL_H

#include <stddef.h>
#include <string.h>
#include <cell/hash/libhash.h>

#ifndef CELL_OK
#define CELL_OK 0
#endif

typedef void (*ps3tc_hash_block_fn)(void *state, const unsigned char *block);

/* Feed len bytes: top up a partly filled buf, run whole blocks straight
 * from the caller's data, keep the tail. */
static inline void ps3tc_hash_update(void *state, unsigned char *buf,
                                     unsigned short *remains,
                                     unsigned long long *total,
                                     unsigned int bs, ps3tc_hash_block_fn block,
                                     const unsigned char *p, unsigned int len)
{
	unsigned int r = *remains;

	*total += len;
	if (r != 0) {
		unsigned int take = bs - r;
		if (take > len)
			take = len;
		memcpy(buf + r, p, take);
		r += take;
		p += take;
		len -= take;
		if (r < bs) {
			*remains = (unsigned short)r;
			return;
		}
		block(state, buf);
	}
	for (; len >= bs; p += bs, len -= bs)
		block(state, p);
	memcpy(buf, p, len);
	*remains = (unsigned short)len;
}

/* Append 0x80, zero fill and the message length in bits: 8 bytes for the
 * 64-byte-block algorithms, 16 for SHA-384/512; little-endian for MD5,
 * big-endian for SHA. */
static inline void ps3tc_hash_pad(void *state, unsigned char *buf,
                                  unsigned int remains, unsigned long long total,
                                  unsigned int bs, unsigned int lenbytes,
                                  int little, ps3tc_hash_block_fn block)
{
	unsigned long long bits = total << 3;
	unsigned int i;

	buf[remains++] = 0x80;
	if (remains > bs - lenbytes) {
		memset(buf + remains, 0, bs - remains);
		block(state, buf);
		remains = 0;
	}
	memset(buf + remains, 0, bs - remains);
	for (i = 0; i < 8; i++) {
		unsigned char b = (unsigned char)(bits >> (8 * i));
		if (little)
			buf[bs - 8 + i] = b;
		else
			buf[bs - 1 - i] = b;
	}
	if (lenbytes == 16)                 /* high bits of the 128-bit length */
		buf[bs - 9] = (unsigned char)(total >> 61);
	block(state, buf);
}

/* The first ds bytes of a state of 32-bit words, in the given byte order. */
static inline void ps3tc_hash_store32(const unsigned int *h, unsigned char *out,
                                      unsigned int ds, int little)
{
	unsigned int i;
	for (i = 0; i < ds; i++) {
		unsigned int sh = little ? 8 * (i & 3) : 24 - 8 * (i & 3);
		out[i] = (unsigned char)(h[i >> 2] >> sh);
	}
}

/* The first ds bytes of a state of 64-bit words, big-endian. */
static inline void ps3tc_hash_store64(const unsigned long long *h, unsigned char *out,
                                      unsigned int ds)
{
	unsigned int i;
	for (i = 0; i < ds; i++)
		out[i] = (unsigned char)(h[i >> 3] >> (56 - 8 * (i & 7)));
}

static inline unsigned int ps3tc_load_be32(const unsigned char *p)
{
	return (unsigned int)p[0] << 24 | (unsigned int)p[1] << 16 |
	       (unsigned int)p[2] << 8 | p[3];
}

static inline unsigned int ps3tc_load_le32(const unsigned char *p)
{
	return (unsigned int)p[3] << 24 | (unsigned int)p[2] << 16 |
	       (unsigned int)p[1] << 8 | p[0];
}

static inline unsigned long long ps3tc_load_be64(const unsigned char *p)
{
	return (unsigned long long)ps3tc_load_be32(p) << 32 | ps3tc_load_be32(p + 4);
}

static inline unsigned int ps3tc_rol32(unsigned int x, unsigned int n)
{
	return (x << n) | (x >> (32 - n));
}

static inline unsigned int ps3tc_ror32(unsigned int x, unsigned int n)
{
	return (x >> n) | (x << (32 - n));
}

static inline unsigned long long ps3tc_ror64(unsigned long long x, unsigned int n)
{
	return (x >> n) | (x << (64 - n));
}

/*
 * The four entry points of one algorithm.  BlockResult pads once; a
 * repeated BlockResult writes the same digest again, and BlockUpdate after
 * it is refused until BlockInit.
 */
#define PS3TC_DEFINE_HASH(Name, BS, LENBYTES, LITTLE, BLOCK, INIT, STORE)          \
int cell##Name##BlockInit(Cell##Name##WorkArea *w)                                 \
{                                                                                  \
	if (w == NULL)                                                                 \
		return (int)CELL_ERROR_HASH_NULL_POINTER;                                  \
	memset(w, 0, sizeof *w);                                                       \
	INIT(w->h);                                                                    \
	return CELL_OK;                                                                \
}                                                                                  \
                                                                                   \
int cell##Name##BlockUpdate(Cell##Name##WorkArea *w, const void *plain,            \
                            unsigned int len)                                      \
{                                                                                  \
	if (w == NULL || plain == NULL)                                                \
		return (int)CELL_ERROR_HASH_NULL_POINTER;                                  \
	if (w->usComputed)                                                             \
		return (int)CELL_ERROR_HASH_INVALID_STATE;                                 \
	ps3tc_hash_update(w->h, w->buf, &w->usRemains, &w->ulTotalLen, BS, BLOCK,      \
	                  (const unsigned char *)plain, len);                          \
	return CELL_OK;                                                                \
}                                                                                  \
                                                                                   \
int cell##Name##BlockResult(Cell##Name##WorkArea *w, unsigned char *digest)        \
{                                                                                  \
	if (w == NULL || digest == NULL)                                               \
		return (int)CELL_ERROR_HASH_NULL_POINTER;                                  \
	if (!w->usComputed) {                                                          \
		ps3tc_hash_pad(w->h, w->buf, w->usRemains, w->ulTotalLen, BS, LENBYTES,    \
		               LITTLE, BLOCK);                                             \
		w->usRemains = 0;                                                          \
		w->usComputed = 1;                                                         \
	}                                                                              \
	STORE(w->h, digest);                                                           \
	return CELL_OK;                                                                \
}                                                                                  \
                                                                                   \
int cell##Name##Digest(const void *plain, unsigned int len, unsigned char *digest) \
{                                                                                  \
	Cell##Name##WorkArea w;                                                        \
	if (plain == NULL || digest == NULL)                                           \
		return (int)CELL_ERROR_HASH_NULL_POINTER;                                  \
	cell##Name##BlockInit(&w);                                                     \
	cell##Name##BlockUpdate(&w, plain, len);                                       \
	return cell##Name##BlockResult(&w, digest);                                    \
}

#endif /* PS3TC_LIBHASH_INTERNAL_H */
