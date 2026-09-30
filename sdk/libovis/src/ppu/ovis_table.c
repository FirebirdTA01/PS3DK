/* cellOvisGetOverlayTableSize / cellOvisInitializeOverlayTable - build the
 * overlay table in main memory from an SPU ELF image.
 *
 * Overlay sections are the loadable segments whose load (physical) address
 * is at or above the end of local store (0x40000); the overlay linker
 * scripts put them there.  The table is those segments laid out by load
 * address minus 0x40000, which is where the SPU mapper fetches them from. */
#include <stdint.h>
#include <string.h>
#include <cell/ovis.h>

#define OVIS_LS_SIZE 0x40000u

/* ELF32 big-endian, read by hand so the image may sit at any alignment. */
static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t rd16(const unsigned char *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

#define EM_SPU   23
#define PT_LOAD  1

struct ovis_elf {
	const unsigned char *base;
	const unsigned char *ph;
	unsigned phnum, phentsize;
};

static int ovis_open(struct ovis_elf *e, const char *elf)
{
	const unsigned char *h = (const unsigned char *)elf;
	if (!h || memcmp(h, "\177ELF", 4) != 0 || h[4] != 1 /* ELFCLASS32 */ ||
	    h[5] != 2 /* big-endian */ || rd16(h + 18) != EM_SPU)
		return CELL_OVIS_ERROR_INVAL;
	e->base = h;
	e->ph = h + rd32(h + 28);
	e->phentsize = rd16(h + 42);
	e->phnum = rd16(h + 44);
	if (e->phentsize < 32)
		return CELL_OVIS_ERROR_INVAL;
	return CELL_OK;
}

/* p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz at 0,4,8,12,16,20 */
#define PH(e, i, off) rd32((e)->ph + (size_t)(i) * (e)->phentsize + (off))

int cellOvisGetOverlayTableSize(const char *elf)
{
	struct ovis_elf e;
	uint32_t end = OVIS_LS_SIZE;
	unsigned i;
	int ret = ovis_open(&e, elf);
	if (ret != CELL_OK)
		return ret;
	for (i = 0; i < e.phnum; i++) {
		uint32_t pa = PH(&e, i, 12), mem = PH(&e, i, 20);
		if (PH(&e, i, 0) == PT_LOAD && pa >= OVIS_LS_SIZE && pa + mem > end)
			end = pa + mem;
	}
	/* whole 128-byte blocks, so the last section's transfer stays inside */
	return (int)(((end - OVIS_LS_SIZE) + 127) & ~127u);
}

int cellOvisInitializeOverlayTable(void *ea_ovly_table, const char *elf)
{
	struct ovis_elf e;
	unsigned char *table = (unsigned char *)ea_ovly_table;
	unsigned i;
	int ret;
	if ((uintptr_t)ea_ovly_table & 127)
		return CELL_OVIS_ERROR_ALIGN;
	ret = ovis_open(&e, elf);
	if (ret != CELL_OK)
		return ret;
	for (i = 0; i < e.phnum; i++) {
		uint32_t pa = PH(&e, i, 12), file = PH(&e, i, 16), mem = PH(&e, i, 20);
		if (PH(&e, i, 0) != PT_LOAD || pa < OVIS_LS_SIZE)
			continue;
		memcpy(table + (pa - OVIS_LS_SIZE), e.base + PH(&e, i, 4), file);
		if (mem > file)
			memset(table + (pa - OVIS_LS_SIZE) + file, 0, mem - file);
	}
	return CELL_OK;
}
