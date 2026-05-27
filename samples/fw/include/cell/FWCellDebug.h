/*
 * PS3 Custom Toolchain - samples/fw/include/cell/FWCellDebug.h
 *
 * Cell-side debug macros for the independent fw compatibility headers.
 */

#ifndef PS3TC_FW_CELL_DEBUG_H
#define PS3TC_FW_CELL_DEBUG_H

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _DEBUG
#define FWASSERT(expr) assert((expr))
#define FWWARNING(msg) do { printf("%s\n", (msg)); } while (0)
#else
#define FWASSERT(expr) ((void)0)
#define FWWARNING(msg) ((void)0)
#endif

#define LITTLE_SHORT(x) ((uint16_t)((((uint16_t)(x) & 0x00ffu) << 8) | (((uint16_t)(x) & 0xff00u) >> 8)))
#define LITTLE_INT(x)   ((((uint32_t)(x) & 0x000000ffu) << 24) | \
                         (((uint32_t)(x) & 0x0000ff00u) << 8)  | \
                         (((uint32_t)(x) & 0x00ff0000u) >> 8)  | \
                         (((uint32_t)(x) & 0xff000000u) >> 24))
#define BIG_SHORT(x)    (x)
#define BIG_INT(x)      (x)

#endif /* PS3TC_FW_CELL_DEBUG_H */
