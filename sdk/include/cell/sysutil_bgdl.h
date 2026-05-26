/*
 * PS3 Custom Toolchain - cell/sysutil_bgdl.h
 *
 * cellBGDLUtility surface: background-download mode control.
 *
 * Two exported entry points backed by libsysutil_bgdl_stub.a:
 *   cellBGDLSetMode     set download mode (AUTO or ALWAYS_ALLOW)
 *   cellBGDLGetMode     query current download mode
 */

#ifndef PS3TC_CELL_SYSUTIL_BGDL_H
#define PS3TC_CELL_SYSUTIL_BGDL_H

#include <sys/types.h>
#include <sysutil/sysutil_common.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (CELL_SYSUTIL_ERROR_BASE_BGDL = 0x8002ce00 family). */
#define CELL_BGDL_UTIL_RET_OK             (0)

#define CELL_BGDL_UTIL_ERROR_BUSY         (CELL_SYSUTIL_ERROR_BASE_BGDL + 1)
#define CELL_BGDL_UTIL_ERROR_INTERNAL     (CELL_SYSUTIL_ERROR_BASE_BGDL + 2)
#define CELL_BGDL_UTIL_ERROR_PARAM        (CELL_SYSUTIL_ERROR_BASE_BGDL + 3)
#define CELL_BGDL_UTIL_ERROR_ACCESS_ERROR (CELL_SYSUTIL_ERROR_BASE_BGDL + 4)
#define CELL_BGDL_UTIL_ERROR_INITIALIZE   (CELL_SYSUTIL_ERROR_BASE_BGDL + 5)

/* Background download operation mode. */
typedef enum {
    CELL_BGDL_MODE_AUTO         = 0,
    CELL_BGDL_MODE_ALWAYS_ALLOW = 1,
} CellBGDLMode;

int cellBGDLSetMode(CellBGDLMode mode);
int cellBGDLGetMode(CellBGDLMode *mode);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_SYSUTIL_BGDL_H */
