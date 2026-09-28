/* SPU side of the task LS patterns.
 *
 * An LS pattern is a 128-bit set of 2 KB local-storage blocks, block 0 in
 * the most significant bit.  A task's pattern names the blocks the taskset
 * policy module saves to (and restores from) the task's context save area
 * when the task is switched out.  The first six blocks (LS 0..0x3000)
 * belong to SPURS and may never appear in a pattern.
 *
 * While a task runs, the policy module keeps a copy of the task's record
 * (CellSpursTaskset + 0x80 + id * 0x30) at dispatch_base + 0x80:
 *   +0x9c  low word of the context field; bits 0..6 = allocated LS blocks
 *   +0xa0  LS pattern (16)
 * and at dispatch_base + 0xb8 the taskset EA, + 0xd0 its DMA tag, + 0xd4
 * the task id.  Setting the pattern updates the LS copy and puts it back
 * to the task's record, which the policy module reads when it saves the
 * context.  The policy module marks itself with 'TK' at LS 0x1e8.
 *
 * The ELF helpers read the task image's ELF and program headers (at most
 * 0x300 bytes) into a caller buffer, or a stack buffer when none is given.
 * Independently written from the published object layouts and semantics.
 */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

#define TASK_INVAL   0x80410902u
#define TASK_NOMEM   0x80410904u
#define TASK_NOEXEC  0x80410907u
#define TASK_PERM    0x80410909u
#define TASK_ALIGN   0x80410910u
#define TASK_NULL    0x80410911u

#define LS_SIZE        0x40000u
#define BLOCK_SHIFT    11
#define BLOCK_MASK     2047u
#define SPURS_AREA     0xfc000000u      /* blocks 0..5 in word 0 */
#define CONTEXT_EXEC   1024u            /* CELL_SPURS_TASK_EXECUTION_CONTEXT_SIZE */
#define HEADERS_MAX    0x300u

#define TASK_CTRL      0x2fb0u
#define PM_MODULE_ID   0x1e8u
#define CTX_ALLOC      0x9c
#define CTX_PATTERN    0xa0
#define CTX_TASKSET    0xb8
#define CTX_TAG        0xd0
#define CTX_TASK_ID    0xd4
#define TASK_RECORD    0x80
#define TASK_RECORD_SIZE 0x30
#define RECORD_PATTERN 0x20

#define PT_LOAD 1
#define PF_W    2

static inline uint32_t context_base(void)
{
	return *(volatile uint32_t *)(uintptr_t)(TASK_CTRL + 8);
}

static unsigned blocks(vec_uint4 v)
{
	unsigned n = 0, i;
	for (i = 0; i < 4; ++i)
		n += (unsigned)__builtin_popcount(spu_extract(v, i));
	return n;
}

/* blocks [first, end), end exclusive, both 0..128 */
static vec_uint4 block_range(unsigned first, unsigned end)
{
	uint32_t w[4];
	unsigned k;
	for (k = 0; k < 4; ++k) {
		int lo = (int)first - 32 * (int)k, hi = (int)end - 32 * (int)k;
		if (lo < 0)  lo = 0;
		if (hi > 32) hi = 32;
		w[k] = lo >= hi ? 0 : (0xffffffffu >> lo) & ~(hi == 32 ? 0u : 0xffffffffu >> hi);
	}
	return (vec_uint4){ w[0], w[1], w[2], w[3] };
}

vec_uint4 cellSpursContextGetLsPattern(void)
{
	return *(volatile vec_uint4 *)(uintptr_t)(context_base() + CTX_PATTERN);
}

int cellSpursContextSetLsPattern(vec_uint4 lsPattern)
{
	uint32_t base;
	uint64_t ea;
	if (*(volatile uint16_t *)(uintptr_t)PM_MODULE_ID != 0x544b)    /* 'TK' */
		return (int)TASK_PERM;
	base = context_base();
	if ((spu_extract(lsPattern, 0) & SPURS_AREA)
	    || blocks(lsPattern) > (*(volatile uint32_t *)(uintptr_t)(base + CTX_ALLOC) & 0x7f))
		return (int)TASK_INVAL;
	*(volatile vec_uint4 *)(uintptr_t)(base + CTX_PATTERN) = lsPattern;
	ea = *(volatile uint64_t *)(uintptr_t)(base + CTX_TASKSET) + TASK_RECORD + RECORD_PATTERN
	   + (uint64_t)*(volatile uint32_t *)(uintptr_t)(base + CTX_TASK_ID) * TASK_RECORD_SIZE;
	spu_dsync();
	mfc_put((volatile void *)(uintptr_t)(base + CTX_PATTERN), ea, 16,
	        *(volatile uint32_t *)(uintptr_t)(base + CTX_TAG), 0, 0);
	return 0;
}

int cellSpursTaskGenerateLsPattern(vec_uint4 *lsPattern, uint32_t start, uint32_t size)
{
	if (!lsPattern)
		return (int)TASK_NULL;
	if ((start | size) & BLOCK_MASK)
		return (int)TASK_ALIGN;
	if (start + size > LS_SIZE)
		return (int)TASK_INVAL;
	*lsPattern = block_range(start >> BLOCK_SHIFT, (start + size) >> BLOCK_SHIFT);
	return 0;
}

int cellSpursTaskGetContextSaveAreaSize(uint32_t *size, vec_uint4 lsPattern)
{
	if (spu_extract(lsPattern, 0) & SPURS_AREA)
		return (int)TASK_INVAL;
	*size = (blocks(lsPattern) << BLOCK_SHIFT) + CONTEXT_EXEC;
	return 0;
}

/* ---- patterns from the task ELF ----------------------------------------- */

static const unsigned char s_ident[16] __attribute__((aligned(16))) = {
	0x7f, 'E', 'L', 'F', 1 /* 32-bit */, 2 /* big endian */, 1 /* version */
};

static void dma_get_wait(void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
	mfc_get(ls, ea, size, tag, 0, 0);
	mfc_write_tag_mask(1u << tag);
	(void)mfc_read_tag_status_all();
}

static int be16(const unsigned char *p) { return (p[0] << 8) | p[1]; }
static uint32_t be32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int _cellSpursTaskDmaGetProgramHeader(uint64_t eaElf, void *buf, uint32_t tag)
{
	unsigned char *h = (unsigned char *)buf;
	uint32_t total, i;
	if (tag > 31)
		return (int)TASK_INVAL;
	if (!eaElf || !buf)
		return (int)TASK_NULL;
	if (((uintptr_t)buf & 15) || (eaElf & 15))
		return (int)TASK_ALIGN;
	dma_get_wait(h, eaElf, 128, tag);
	for (i = 0; i < 16; ++i)
		if (h[i] != s_ident[i])
			return (int)TASK_NOEXEC;
	if (be16(h + 0x12) != 23 /* EM_SPU */ || be32(h + 0x18) < 0x3000)
		return (int)TASK_NOEXEC;
	total = ((uint32_t)be16(h + 0x2c) * (uint32_t)be16(h + 0x2a) + be32(h + 0x1c) + 127) & ~127u;
	if (total > HEADERS_MAX)
		return (int)TASK_NOMEM;
	if (total > 128)
		dma_get_wait(h + 128, eaElf + 128, total - 128, tag);
	return 0;
}

/* Walk the PT_LOAD program headers of an image fetched above.  `loadable`
 * selects every block a segment's memory image touches; otherwise only the
 * blocks wholly inside a read-only segment's file image. */
static int headers_pattern(vec_uint4 *lsPattern, uint64_t eaElf, void *buf, uint32_t tag, int loadable)
{
	unsigned char local[HEADERS_MAX + 32] __attribute__((aligned(16)));
	const unsigned char *h, *ph;
	vec_uint4 pattern = { 0, 0, 0, 0 };
	unsigned n, i, step;
	int rc;
	if (!lsPattern)
		return (int)TASK_NULL;
	if (!buf)
		buf = local;
	if ((rc = _cellSpursTaskDmaGetProgramHeader(eaElf, buf, tag)) != 0)
		return rc;
	h = (const unsigned char *)buf;
	n = (unsigned)be16(h + 0x2c);
	step = (unsigned)be16(h + 0x2a);
	ph = h + be32(h + 0x1c);
	for (i = 0; i < n; ++i, ph += step) {
		uint32_t vaddr = be32(ph + 8), filesz = be32(ph + 16), memsz = be32(ph + 20);
		if (vaddr + memsz > LS_SIZE)
			return (int)TASK_INVAL;
		if (be32(ph) != PT_LOAD || be32(ph + 12) >= LS_SIZE)
			continue;
		if (loadable)
			pattern = spu_or(pattern, block_range(vaddr >> BLOCK_SHIFT,
			                                      (vaddr + memsz + BLOCK_MASK) >> BLOCK_SHIFT));
		else if (!(be32(ph + 24) & PF_W))
			pattern = spu_or(pattern, block_range((vaddr + BLOCK_MASK) >> BLOCK_SHIFT,
			                                      (vaddr + filesz) >> BLOCK_SHIFT));
	}
	*lsPattern = pattern;
	return 0;
}

int cellSpursTaskGetReadOnlyAreaPattern(vec_uint4 *lsPattern, uint64_t eaElf, void *buf, uint32_t tag)
{
	return headers_pattern(lsPattern, eaElf, buf, tag, 0);
}

int cellSpursTaskGetLoadableSegmentPattern(vec_uint4 *lsPattern, uint64_t eaElf, void *buf, uint32_t tag)
{
	return headers_pattern(lsPattern, eaElf, buf, tag, 1);
}
