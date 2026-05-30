/*
 * PS3 Custom Toolchain -- <cell/dbgrsx/dbgrsx_error.h>
 *
 * Error codes for the cellDbgRsx library (libdbgrsx.a).
 *
 * Facility: CELL_ERROR_FACILITY_GFX (0x021)
 * Range:    0x8021_0501 - 0x8021_05ff
 */

#ifndef __PS3DK_CELL_DBGRSX_ERROR_H__
#define __PS3DK_CELL_DBGRSX_ERROR_H__

#include <cell/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The reference SDK maps libdbgrsx errors to the GFX facility (0x021)
 * but aliases the facility as CELL_ERROR_FACILITY_DBG_RSX for clarity.
 */
#ifndef CELL_ERROR_FACILITY_DBG_RSX
#define CELL_ERROR_FACILITY_DBG_RSX  0x021
#endif

#define CELL_DBG_RSX_MAKE_ERROR(status) \
	CELL_ERROR_MAKE_ERROR(CELL_ERROR_FACILITY_DBG_RSX, status)

#define CELL_DBG_RSX_ERROR_FAILURE              CELL_ERROR_CAST(0x802105ff)
#define CELL_DBG_RSX_ERROR_INVALID_ENUM         CELL_ERROR_CAST(0x80210501)
#define CELL_DBG_RSX_ERROR_INVALID_VALUE        CELL_ERROR_CAST(0x80210502)
#define CELL_DBG_RSX_ERROR_INVALID_ALIGNMENT    CELL_ERROR_CAST(0x80210503)

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_DBGRSX_ERROR_H__ */
