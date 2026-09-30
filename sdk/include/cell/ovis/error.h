/* cell/ovis/error.h - libovis return codes (PPU side). */
#ifndef PS3TC_CELL_OVIS_ERROR_H
#define PS3TC_CELL_OVIS_ERROR_H

#include <cell/error.h>

#define CELL_ERROR_MINOR_FACILITY_OVIS 0x4
#define CELL_ERROR_MAKE_OVIS_ERROR(id) \
	(CELL_ERROR_MAKE_ERROR(CELL_ERROR_FACILITY_SPU, (CELL_ERROR_MINOR_FACILITY_OVIS << 8) | (id)))

/* not an SPU ELF, or a bad argument */
#define CELL_OVIS_ERROR_INVAL  CELL_ERROR_CAST(0x80410402)
/* the overlay table address is not 128-byte aligned */
#define CELL_OVIS_ERROR_ALIGN  CELL_ERROR_CAST(0x80410416)

#endif /* PS3TC_CELL_OVIS_ERROR_H */
