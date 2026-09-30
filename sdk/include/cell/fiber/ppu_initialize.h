/* cell/fiber/ppu_initialize.h - libfiber SPRX bootstrap.
 *
 * cellFiberPpuInitialize initializes the libfiber SPRX's global state; the
 * SPRX must already be loaded (cellSysmoduleLoadModule(CELL_SYSMODULE_FIBER)),
 * otherwise it returns CELL_FIBER_ERROR_STAT.  Call it once per process
 * before any other fiber entry point.
 *
 * The underscored _cellFiberPpuInitialize is the raw SPRX export.
 */
#ifndef __PS3DK_CELL_FIBER_PPU_INITIALIZE_H__
#define __PS3DK_CELL_FIBER_PPU_INITIALIZE_H__

#include <cell/fiber/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The SPRX entry takes the address of a 64-byte thread-local area (the
 * libfiber stub's _gCellFiberPpuThreadLocalStorage, in .tbss): the SPRX
 * reaches each thread's copy through the thread pointer.  Always use
 * cellFiberPpuInitialize, which passes it. */
int _cellFiberPpuInitialize(void *tlsArea);
int cellFiberPpuInitialize(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PS3DK_CELL_FIBER_PPU_INITIALIZE_H__ */
