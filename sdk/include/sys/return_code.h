/*
 * PS3 Custom Toolchain — <sys/return_code.h> cell-SDK compat shim.
 *
 * Cell-SDK samples test `cell*` return values against `CELL_OK` (= 0)
 * and the LV2 system libraries return `int32_t` status codes; those
 * are the only two symbols this header has ever provided in practice.
 * The wider CELL_E* error-code table is module-specific and lives
 * alongside each subsystem's header (cell/gcm/gcm_error.h, etc.).
 */

#ifndef PS3TC_COMPAT_SYS_RETURN_CODE_H
#define PS3TC_COMPAT_SYS_RETURN_CODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CELL_OK
#define CELL_OK 0
#endif

typedef int32_t CellReturnCode;

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_COMPAT_SYS_RETURN_CODE_H */
