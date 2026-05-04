/* cell/fiber/ppu_initialize.h - libfiber SPRX bootstrap.
 *
 * Clean-room header.  cellFiberPpuInitialize loads / binds the
 * libfiber SPRX (if not already loaded) and initializes internal
 * global state.  Must be called once per process before any other
 * fiber entry point.
 *
 * The underscored _cellFiberPpuInitialize is the raw SPRX export;
 * the public cellFiberPpuInitialize wrapper (in
 * sdk/libfiber_stub_extras/) loads the system module first, then
 * forwards to the underscored entry.
 */
#ifndef __PS3DK_CELL_FIBER_PPU_INITIALIZE_H__
#define __PS3DK_CELL_FIBER_PPU_INITIALIZE_H__

#include <cell/fiber/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The SPRX entry takes a pointer to a per-process TLS scratch area
 * (16-byte aligned, 64 bytes minimum).  The public wrapper supplies
 * one out of its own BSS, so callers do not need to manage the area
 * themselves and should always use cellFiberPpuInitialize. */
int _cellFiberPpuInitialize(void *tlsArea);
int cellFiberPpuInitialize(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __PS3DK_CELL_FIBER_PPU_INITIALIZE_H__ */
