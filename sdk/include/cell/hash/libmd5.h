/*
 * PS3 Custom Toolchain - <cell/hash/libmd5.h> MD5 message digest (RFC 1321).
 *
 * cellMd5Digest hashes one buffer.  For data that arrives in pieces:
 * cellMd5BlockInit, then cellMd5BlockUpdate any number of times, then
 * cellMd5BlockResult.  After BlockResult the work area accepts no more
 * data (CELL_ERROR_HASH_INVALID_STATE) until BlockInit starts it again;
 * BlockResult itself may be repeated and writes the same digest.
 *
 * Every call returns CELL_OK or CELL_ERROR_HASH_NULL_POINTER when a pointer
 * argument is NULL.  The functions keep no global state, so separate work
 * areas may be used from separate threads.
 */

#ifndef PS3TC_CELL_HASH_MD5_H
#define PS3TC_CELL_HASH_MD5_H

#include <cell/hash/libhash.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_MD5_BLOCK_SIZE            64
#define CELL_MD5_BLOCK_SIZE4           16
#define CELL_MD5_DIGEST_SIZE           16

/* Caller-allocated state.  Treat the members as private; the layout is
 * fixed (no pointers or size_t) so it is the same in ILP32 and LP64. */
typedef struct {
	unsigned int h[4];
	unsigned int pad;
	unsigned short usRemains;       /* bytes waiting in buf */
	unsigned short usComputed;      /* non-zero once BlockResult ran */
	__extension__ unsigned long long ulTotalLen;   /* bytes hashed so far */
	unsigned char buf[CELL_MD5_BLOCK_SIZE];
} CellMd5WorkArea;

/* Digest of len bytes at plain into digest (CELL_MD5_DIGEST_SIZE bytes). */
int cellMd5Digest(const void *plain, unsigned int len, unsigned char *digest);

int cellMd5BlockInit(CellMd5WorkArea *workarea);
int cellMd5BlockUpdate(CellMd5WorkArea *workarea, const void *plain, unsigned int len);
int cellMd5BlockResult(CellMd5WorkArea *workarea, unsigned char *digest);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_HASH_MD5_H */
