/* _cellOvisStartMapping / _cellOvisWaitMapping - move an overlay section
 * from the overlay table in main memory into local store.
 *
 * The overlay table (built on the PPU by cellOvisInitializeOverlayTable)
 * holds each section at its load address minus sOvisLsSize, so the source
 * of a section is ea_table + lma - 0x40000.  The LS table (_ovly_table)
 * records per section whether it is resident; a section is marked
 * resident only once its transfer has completed (WaitMapping), and every
 * other section overlapping its LS range is marked not resident as soon
 * as the transfer starts. */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <cell/ovis/mapper.h>

void _ovly_debug_event(void);

#define OVIS_VMA(e)    spu_extract((e), 0)
#define OVIS_SIZE(e)   spu_extract((e), 1)
#define OVIS_LMA(e)    spu_extract((e), 2)
#define OVIS_MAPPED(e) spu_extract((e), 3)

/* Largest single MFC transfer. */
#define OVIS_DMA_MAX 16384u

static void ovis_get(uint32_t ls, uint64_t ea, uint32_t size, uint32_t tag)
{
	while (size) {
		uint32_t n;
		if (size >= OVIS_DMA_MAX)
			n = OVIS_DMA_MAX;
		else if (size >= 16)
			n = size & ~15u;
		else
			/* 1, 2, 4 or 8 bytes, naturally aligned: the largest the
			 * remaining size and both addresses allow */
			for (n = 8; n > 1 && (n > size || ((ls | (uint32_t)ea) & (n - 1))); n >>= 1)
				;
		mfc_get((volatile void *)ls, ea, n, tag, 0, 0);
		ls += n; ea += n; size -= n;
	}
}

int _cellOvisStartMapping(_ovly_table_t info, uint64_t ea_ovly_table,
                          uint32_t tag, uint32_t offset)
{
	const uint32_t vma = OVIS_VMA(info), size = OVIS_SIZE(info), lma = OVIS_LMA(info);
	const uint32_t end = vma + size;
	int ret = CELL_OVIS_ERROR_ABORT;
	unsigned long i;

	for (i = 0; i < _novlys; i++) {
		_ovly_table_t e = _ovly_table[i];
		if (OVIS_LMA(e) == lma) {
			if (OVIS_MAPPED(e))
				continue; /* already resident */
			if (size) {
				uint32_t ls = vma + offset;
				uint64_t ea = ea_ovly_table + lma - sOvisLsSize;
				/* misaligned addresses or a bad tag are a programming
				 * error the MFC would fault on; stop here instead */
				spu_hcmpeq((((ls | (uint32_t)ea) & 15) == 0 && tag < 32), 0);
				ovis_get(ls, ea, size, tag);
			}
			ret = CELL_OK;
		} else if (OVIS_VMA(e) < end && vma < OVIS_VMA(e) + OVIS_SIZE(e)) {
			/* shares LS with the incoming section: no longer resident */
			_ovly_table[i] = spu_insert(0u, e, 3);
		}
	}
	_ovly_debug_event();
	return ret;
}

void _cellOvisWaitMapping(_ovly_table_t *info, uint32_t tag)
{
	mfc_write_tag_mask(1u << tag);
	mfc_read_tag_status_all();
	/* the transfer brought in code: order it before instruction fetch */
	spu_sync();
	*info = spu_insert(1u, *info, 3);
	_ovly_debug_event();
}
