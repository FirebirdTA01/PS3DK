/* SPU job-queue runtime, part 7: the size of the buffer a job needs to be
 * suspended in (cellSpursJobQueueGetSuspendedJobSize).
 *
 * The descriptor is validated like a pushed job (its DMA list must fit
 * the input buffer, the cache list must be well formed, the whole job
 * must fit the local store), the job binary must carry a job CRT of
 * version 0x12 or later ("JOBCRT Ver" + two hex digits at 0x30), and the
 * size is the sum of the job's LS areas, each 128-byte aligned, plus a
 * fixed 0x500.  With the memory checker on (jobType bit 1) every buffer
 * gets a 16-byte guard.  Attribute 1 counts the binary's own save area,
 * read from the binary, instead of its loadable image and the cache.
 * Independently written from the published object layouts and semantics.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <spu_mfcio.h>

#define JOB_INVAL    0x80410A02u
#define JOB_PERM     0x80410A09u
#define JOB_ALIGN    0x80410A10u
#define JOB_NULL     0x80410A11u
#define JOB_TOO_BIG  0x80410A17u

#define TAG          31
#define A128(x)      (((x) + 127u) & ~127u)
#define A16(x)       (((x) + 15u) & ~15u)

static uint8_t bin[0x60] __attribute__((aligned(128)));
static uint8_t aux[0x20] __attribute__((aligned(128)));

static inline uint32_t w32(const void *p) { return *(const uint32_t *)p; }
static inline uint16_t w16(const void *p) { return *(const uint16_t *)p; }

static void dma_get(void *ls, uint64_t ea, unsigned size)
{
	mfc_get(ls, ea, size, TAG, 0, 0);
	mfc_write_tag_mask(1u << TAG);
	(void)mfc_read_tag_status_all();
}

static unsigned hexdigit_pair(const uint8_t *p)
{
	return (unsigned)(p[0] - '0') * 16 + (unsigned)(p[1] - '0');
}

/* the binary's own save area (attribute 1) */
static uint32_t binary_area(uint64_t eaBin)
{
	static uint32_t pair[4] __attribute__((aligned(16)));
	uint32_t area, off;
	unsigned i;
	const uint32_t magic = w32(bin + 0x20);
	if (magic == 0x62696e32u || magic == 0x42494e32u) {      /* "bin2", "BIN2" */
		area = (uint32_t)w16(bin + 0x2e) * 16;
		off = 0x50;
	} else {
		area = w32(bin + 0x3c) - w32(bin + 0x40);
		off = w32(bin + 0x3c) + 32;
	}
	for (i = 0; i < 256; ++i, off += 8) {
		const uint64_t ea = eaBin + off;
		mfc_get(pair, ea & ~15ull, 16, TAG, 0, 0);
		mfc_write_tag_mask(1u << TAG);
		(void)mfc_read_tag_status_all();
		const uint32_t *e = pair + ((ea & 15) >> 2);
		if (e[0] == 0xffffffffu)
			break;
		area += e[1];
	}
	return area;
}

int cellSpursJobQueueGetSuspendedJobSize(const void *pJob, size_t sizeJobDesc, int attr, unsigned int *pSize)
{
	const uint8_t *h = (const uint8_t *)pJob;
	uint32_t eaBin, sizeDma, sizeCache, sizeIn, sizeOut, guard, nDma, nCache, sum, cache, total, stack, x;
	uint16_t sizeBin, sizeStack, sizeScratch;
	unsigned i, ver;
	const unsigned size = (unsigned)sizeJobDesc;

	if (!pSize)
		return (int)JOB_NULL;
	if ((uintptr_t)pSize & 3)
		return (int)JOB_ALIGN;
	if (attr > 1)
		return (int)JOB_INVAL;
	if (size - 256 > 767 || (size & 0x7f))
		return (int)JOB_INVAL;

	eaBin = w32(h + 4);
	if (!(eaBin & ~1u))
		return (int)JOB_NULL;
	sizeOut = w32(h + 24);
	if (((uintptr_t)pJob & 15) || (eaBin & 14) || (sizeOut & 15))
		return (int)JOB_ALIGN;
	sizeBin = w16(h + 8);
	sizeDma = w16(h + 10);
	sizeIn = w32(h + 20);
	sizeCache = w32(h + 36);
	if ((sizeDma & 7) || (sizeCache & 7) || (sizeIn & 15))
		return (int)JOB_ALIGN;
	if (sizeCache > 39 || sizeDma + sizeCache > size - 48 || !sizeBin)
		return (int)JOB_INVAL;
	guard = (h[44] & 2) ? 16 : 0;

	/* input DMA list */
	nDma = (sizeDma >> 3) & 0x1fff;
	for (sum = 0, i = 0; i < nDma; ++i) {
		const uint32_t hi = w32(h + 48 + 8 * i), ea = w32(h + 52 + 8 * i);
		const uint32_t n = hi & 0x7fff;
		if (!n)
			continue;
		if (!ea)
			return (int)JOB_NULL;
		if ((hi >> 31) || n > 0x4000)
			return (int)JOB_INVAL;
		if (n <= 15 && n != 1 && n != 2 && n != 4 && n != 8)
			return (int)JOB_INVAL;
		if (ea & ((n > 16 ? 16 : n) - 1))
			return (int)JOB_ALIGN;
		sum += A16(n);
	}
	if (sum > sizeIn)
		return (int)JOB_INVAL;

	/* cache DMA list */
	nCache = sizeCache >> 3;
	for (cache = 0, i = 0; i < nCache; ++i) {
		const uint32_t hi = w32(h + 48 + 8 * (nDma + i)), ea = w32(h + 52 + 8 * (nDma + i));
		if (hi) {
			if (!ea)
				return (int)JOB_NULL;
			if (hi <= 15)
				return (int)JOB_INVAL;
			if ((hi & 15) || (ea & 15))
				return (int)JOB_ALIGN;
		}
		cache += A128(hi + guard);
	}

	sizeStack = w16(h + 28);
	sizeScratch = w16(h + 30);
	stack = sizeStack ? (uint32_t)sizeStack * 16 : 0x2000;
	{
		const uint32_t bin16 = sizeBin + ((h[44] & 4) ? w32(h) : 0);
		const uint32_t scr = A128((uint32_t)sizeScratch * 16 + (sizeScratch ? guard : 0));
		total = A128(guard + scr + stack) + A128(bin16 * 16)
		      + A128(sizeOut + (sizeOut ? guard : 0)) + A128(sizeIn + (sizeIn ? guard : 0)) + cache;
		if (total > 0x37400u - 4 * size + 1024)
			return (int)JOB_TOO_BIG;
	}

	/* the binary must carry a job CRT new enough to be suspended */
	dma_get(bin, eaBin & ~3u, sizeof bin);
	if (memcmp(bin + 0x30, "JOBCRT Ver", 10))
		return (int)JOB_PERM;
	ver = hexdigit_pair(bin + 0x3a);
	if (ver <= 0x11)
		return (int)JOB_PERM;
	if (ver == 0x12) {
		/* "%SPURS JOB INFO%", stored with each byte xor 1 so this library
		   never carries the marker itself */
		static const uint8_t info[16] = { 0x24, 0x52, 0x51, 0x54, 0x53, 0x52, 0x21, 0x4b,
		                                  0x4e, 0x43, 0x21, 0x48, 0x4f, 0x47, 0x4e, 0x24 };
		const uint64_t ea = (uint64_t)(eaBin & ~3u) + w32(bin + 0x3c);
		if (ea & 15)
			spu_stop(0);
		dma_get(aux, ea, sizeof aux);
		for (i = 0; i < 16; ++i)
			if (aux[16 + i] != (info[i] ^ 1))
				return (int)JOB_PERM;
	}

	if (attr != 1)
		x = A128(((uint32_t)sizeBin + w32(h)) * 16) + cache;
	else
		x = binary_area(eaBin & ~3u);
	*pSize = size + x + (sizeStack ? A128((uint32_t)sizeStack * 16) : 0x2000) + guard
	       + A128(sizeOut + (sizeOut ? guard : 0)) + A128((uint32_t)sizeScratch * 16 + (sizeScratch ? guard : 0))
	       + A128(sizeIn + (sizeIn ? guard : 0)) + 0x500;
	return 0;
}
