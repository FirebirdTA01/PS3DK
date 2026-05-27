/*
 * PS3 Custom Toolchain - samples/fw/include/FWDebug.h
 *
 * Minimal sample-framework debug facade for PS3DK fw compatibility.
 */

#ifndef PS3TC_FW_DEBUG_H
#define PS3TC_FW_DEBUG_H

#if defined(__CELLOS_LV2__) || defined(__PS3__) || defined(PS3)
#include <cell/FWCellDebug.h>
#else
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
#endif

#endif /* PS3TC_FW_DEBUG_H */
