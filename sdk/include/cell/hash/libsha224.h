/*
 * PS3 Custom Toolchain - <cell/hash/libsha224.h> SHA-224 message digest (FIPS 180-4).
 *
 * cellSha224Digest hashes one buffer.  For data that arrives in pieces:
 * cellSha224BlockInit, then cellSha224BlockUpdate any number of times, then
 * cellSha224BlockResult.  After BlockResult the work area accepts no more
 * data (CELL_ERROR_HASH_INVALID_STATE) until BlockInit starts it again;
 * BlockResult itself may be repeated and writes the same digest.
 *
 * Every call returns CELL_OK or CELL_ERROR_HASH_NULL_POINTER when a pointer
 * argument is NULL.  The functions keep no global state, so separate work
 * areas may be used from separate threads.
 */

#ifndef PS3TC_CELL_HASH_SHA224_H
#define PS3TC_CELL_HASH_SHA224_H

#include <cell/hash/libhash.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_SHA224_BLOCK_SIZE            64
#define CELL_SHA224_DIGEST_SIZE           28

/* Caller-allocated state.  Treat the members as private; the layout is
 * fixed (no pointers or size_t) so it is the same in ILP32 and LP64. */
typedef struct {
	unsigned int h[8];
	unsigned int pad;
	unsigned short usRemains;       /* bytes waiting in buf */
	unsigned short usComputed;      /* non-zero once BlockResult ran */
	__extension__ unsigned long long ulTotalLen;   /* bytes hashed so far */
	unsigned char buf[CELL_SHA224_BLOCK_SIZE];
} CellSha224WorkArea;

/* Digest of len bytes at plain into digest (CELL_SHA224_DIGEST_SIZE bytes). */
int cellSha224Digest(const void *plain, unsigned int len, unsigned char *digest);

int cellSha224BlockInit(CellSha224WorkArea *workarea);
int cellSha224BlockUpdate(CellSha224WorkArea *workarea, const void *plain, unsigned int len);
int cellSha224BlockResult(CellSha224WorkArea *workarea, unsigned char *digest);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_HASH_SHA224_H */
