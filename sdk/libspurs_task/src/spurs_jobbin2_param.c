/* SPU side of cellSpursJobHeaderSetJobbin2Param: fill a job header's
 * binary fields from a jobbin2 image in main memory.
 *
 * A jobbin2 image is a 32-bit big-endian SPU ELF wrapper whose program
 * headers (right after the ELF header) are either
 *   LOAD r-x, LOAD rw-, NOTE                          (3 headers), or
 *   LOAD r-x, LOAD r-x, LOAD r-x, LOAD rw-, NOTE      (5 headers).
 * The first LOAD's file offset is where the LS image starts; its byte
 * 0x20 carries "bin2" (or "BIN2").  The header gets, in binaryInfo:
 *   +0x02 u16  bss size (memsz - filesz of the writable LOAD) >> 4
 *   +0x04 u32  EA of the LS image
 *   +0x08 u16  loaded size without the bss >> 4
 * and jobType gains CELL_SPURS_JOB_TYPE_BINARY2.
 * Independently written from the published formats and semantics.
 */
#include <stdint.h>
#include <spu_mfcio.h>

#define JOB_INVAL 0x80410A02u
#define JOB_ALIGN 0x80410A10u
#define JOB_NULL  0x80410A11u

#define PT_LOAD 1
#define PT_NOTE 4
#define PF_RX   5
#define PF_RW   6
#define JOB_TYPE_BINARY2 4

static uint8_t elf[0xe0] __attribute__((aligned(128)));
static uint8_t image[0x60] __attribute__((aligned(128)));

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void fetch(void *ls, uint64_t ea, uint32_t size)
{
	mfc_get(ls, ea, size, 0, 0, 0);
	mfc_write_tag_mask(1u << 0);
	(void)mfc_read_tag_status_all();
}

/* program header i: type, flags */
#define PH(i)          (elf + 0x34 + (i) * 0x20)
#define PH_IS(i, t, f) (be32(PH(i)) == (t) && (f < 0 || (be32(PH(i) + 0x18) & 7) == (uint32_t)(f)))

int cellSpursJobHeaderSetJobbin2Param(void *jobHeader, uint64_t eaJobbin2)
{
	uint8_t *h = (uint8_t *)jobHeader;
	uint32_t lo = 0xffffffffu, hi = 0, base, magic;
	int bss = -1;
	unsigned n, i;
	if (!jobHeader || !eaJobbin2)
		return (int)JOB_NULL;
	if (eaJobbin2 & 15)
		return (int)JOB_ALIGN;
	fetch(elf, eaJobbin2, sizeof elf);
	if (be32(elf + 0x1c) != 0x34)
		return (int)JOB_INVAL;
	n = (elf[0x2c] << 8) | elf[0x2d];
	if (n == 3) {
		if (!(PH_IS(0, PT_LOAD, PF_RX) && PH_IS(1, PT_LOAD, PF_RW) && PH_IS(2, PT_NOTE, -1)))
			return (int)JOB_INVAL;
	} else if (n == 5) {
		if (!(PH_IS(0, PT_LOAD, PF_RX) && PH_IS(1, PT_LOAD, PF_RX) && PH_IS(2, PT_LOAD, PF_RX)
		      && PH_IS(3, PT_LOAD, PF_RW) && PH_IS(4, PT_NOTE, -1)))
			return (int)JOB_INVAL;
	} else
		return (int)JOB_INVAL;

	for (i = 0; i < n; ++i) {
		const uint8_t *ph = PH(i);
		const uint32_t vaddr = be32(ph + 8), filesz = be32(ph + 16), memsz = be32(ph + 20);
		if (be32(ph) != PT_LOAD)
			continue;
		if (vaddr < lo)
			lo = vaddr;
		if (vaddr + memsz > hi)
			hi = vaddr + memsz;
		if (be32(ph + 24) & 2) {
			if (bss != -1)
				return (int)JOB_INVAL;      /* one writable segment only */
			bss = (int)(memsz - filesz);
		}
	}

	base = (uint32_t)eaJobbin2 + be32(PH(0) + 4);
	if (base & 15)
		return (int)JOB_INVAL;
	fetch(image, base, sizeof image);
	magic = be32(image + 0x20);
	if (magic != 0x62696e32u /* "bin2" */ && magic != 0x42494e32u /* "BIN2" */)
		return (int)JOB_INVAL;

	{
		const uint16_t sizeBss = (uint16_t)(bss / 16);
		const uint16_t sizeBinary = (uint16_t)((hi - lo - (uint32_t)bss) >> 4);
		h[2] = (uint8_t)(sizeBss >> 8);
		h[3] = (uint8_t)sizeBss;
		h[4] = (uint8_t)(base >> 24);
		h[5] = (uint8_t)(base >> 16);
		h[6] = (uint8_t)(base >> 8);
		h[7] = (uint8_t)base;
		h[8] = (uint8_t)(sizeBinary >> 8);
		h[9] = (uint8_t)sizeBinary;
		h[0x2c] |= JOB_TYPE_BINARY2;
	}
	return 0;
}
