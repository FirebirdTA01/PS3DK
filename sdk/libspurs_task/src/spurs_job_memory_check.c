/* SPU side of the job memory check (CELL_SPURS_JOB_TYPE_MEMORY_CHECK).
 *
 * For a memory-check job the PPU side grows every job buffer by one
 * 16-byte guard.  cellSpursJobMemoryCheckInitialize, called at the start
 * of the job, takes the guards back out of the job header's sizes, writes
 * the guard pattern 0xdeadbeef x4 just past each buffer (read-only cache
 * buffers, the in/in-out buffer, the out buffer), just below the stack,
 * and at LS 0 (a null-pointer write lands there).
 * cellSpursJobMemoryCheckTest reports, as CellSpursJobBufOverrunErrMask
 * bits, which guards were overwritten.
 *
 * Job header fields used: +0x0a sizeDmaList, +0x14 sizeInOrInOut, +0x18
 * sizeOut, +0x1e sizeScratch (quadwords), +0x24 sizeCacheDmaList,
 * +0x2c jobType, and the DMA lists after the header (+0x30: the input
 * list, then the cache list, 8 bytes per element: u32 size, u32 EA).
 * Job context fields used: +0x00 ioBuffer, +0x04 cacheBuffer[4], +0x1c
 * oBuffer.
 * Independently written from the published layouts and semantics.
 */
#include <stdint.h>
#include <spu_intrinsics.h>

#define JOB_PERM            0x80410A09u
#define JOB_STAT            0x80410A0Fu
#define JOB_NULL            0x80410A11u
#define JOB_MEMORY_CORRUPT  0x80410A12u

#define JOB_TYPE_MEMORY_CHECK 2
#define GUARD 0xdeadbeefu
#define MARKERS 8

/* exported like the reference so debuggers find them */
uint32_t _g_pCellSpursJobMemCheckMarker[MARKERS] __attribute__((aligned(16)));
uint8_t  _g_cellSpursJobMemoryCheckJobContext[0x30] __attribute__((aligned(16)));
uint8_t  _g_cellSpursJobMemoryCheckJobHeader[0x30] __attribute__((aligned(16)));
uint32_t _g_cellSpursJobIsBufferReverted;

static inline uint32_t rd32(const void *p) { return *(const volatile uint32_t *)p; }
static inline void wr32(void *p, uint32_t v) { *(volatile uint32_t *)p = v; }
static inline uint32_t round16(uint32_t v) { return (v + 15) & ~15u; }

/* LS accesses by raw address - address 0 included, so no C pointers */
static inline void ls_store(uint32_t addr, vec_uint4 v)
{
	__asm__ volatile("stqd %0,0(%1)" : : "r"(v), "r"(addr) : "memory");
}

static inline vec_uint4 ls_load(uint32_t addr)
{
	vec_uint4 v;
	__asm__ volatile("lqd %0,0(%1)" : "=r"(v) : "r"(addr) : "memory");
	return v;
}

int cellSpursJobMemoryCheckInitialize(const void *jobContext, void *jobHeader)
{
	const uint8_t *ctx = (const uint8_t *)jobContext;
	uint8_t *hdr = (uint8_t *)jobHeader;
	unsigned i, numIo, numCache;
	uint8_t *cacheList;
	qword sp;
	if (!ctx || !hdr)
		return (int)JOB_NULL;
	if (!(hdr[0x2c] & JOB_TYPE_MEMORY_CHECK))
		return (int)JOB_PERM;

	/* the header's buffer sizes, without their guards (once per load) */
	if (!_g_cellSpursJobIsBufferReverted) {
		uint16_t scratch = (uint16_t)((hdr[0x1e] << 8) | hdr[0x1f]);
		if (rd32(hdr + 0x14))
			wr32(hdr + 0x14, rd32(hdr + 0x14) - 16);
		if (rd32(hdr + 0x18))
			wr32(hdr + 0x18, rd32(hdr + 0x18) - 16);
		if (scratch) {
			--scratch;
			hdr[0x1e] = (uint8_t)(scratch >> 8);
			hdr[0x1f] = (uint8_t)scratch;
		}
		_g_cellSpursJobIsBufferReverted = 1;
	}

	/* read-only cache buffers */
	numIo = (((hdr[0x0a] << 8) | hdr[0x0b]) >> 3) & 0x1fff;
	numCache = rd32(hdr + 0x24) >> 3;
	cacheList = hdr + 0x30 + numIo * 8;
	for (i = 0; i < numCache; ++i) {
		uint8_t *entry = cacheList + i * 8;
		const uint32_t size = rd32(entry) - 16;
		wr32(entry, size);
		if (i < 4)
			_g_pCellSpursJobMemCheckMarker[i] = rd32(ctx + 4 + i * 4) + round16(size);
	}
	if (rd32(hdr + 0x14))
		_g_pCellSpursJobMemCheckMarker[4] = rd32(ctx + 0x00) + round16(rd32(hdr + 0x14));
	if (rd32(hdr + 0x18))
		_g_pCellSpursJobMemCheckMarker[5] = rd32(ctx + 0x1c) + round16(rd32(hdr + 0x18));
	/* $1 holds the stack pointer in word 0 and the free stack in word 1 */
	__asm__ volatile("ori %0,$1,0" : "=r"(sp));
	_g_pCellSpursJobMemCheckMarker[6] =
		spu_extract((vec_uint4)sp, 0) - spu_extract((vec_uint4)sp, 1) - 16;
	/* marker 7 stays 0: the guard at LS 0 */

	for (i = 0; i < MARKERS; ++i) {
		const uint32_t p = _g_pCellSpursJobMemCheckMarker[i];
		if (p || i == MARKERS - 1)
			ls_store(p, spu_splats(GUARD));
	}
	for (i = 0; i < 0x30; ++i) {
		_g_cellSpursJobMemoryCheckJobContext[i] = ctx[i];
		_g_cellSpursJobMemoryCheckJobHeader[i] = hdr[i];
	}
	return 0;
}

int cellSpursJobMemoryCheckTest(uint16_t *cause)
{
	const vec_uint4 guard = spu_splats(GUARD);
	unsigned i, mask = 0;
	if (!_g_cellSpursJobIsBufferReverted)
		return (int)JOB_STAT;
	for (i = 0; i < MARKERS; ++i) {
		const uint32_t p = _g_pCellSpursJobMemCheckMarker[i];
		if (!p && i != MARKERS - 1)
			continue;
		if (spu_extract(spu_gather(spu_cmpeq(ls_load(p), guard)), 0) != 0xf)
			mask |= 1u << i;
	}
	if (cause)
		*cause = (uint16_t)mask;
	return mask ? (int)JOB_MEMORY_CORRUPT : 0;
}
