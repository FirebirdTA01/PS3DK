/* cell/fiber.h -- SPU-side fiber declarations.
 *
 * SPU fiber contexts (cell/fiber/spu_context.h) are implemented in
 * libfiber.a.  The SPU-to-PPU fiber signalling headers
 * (cell/fiber/ppu_fiber.h, ppuUtilDefine.h, ppuUtilRuntime.h) are not yet
 * shipped; their functions are not in libfiber.a, so code that calls them
 * fails at link time with undefined references rather than silently.
 */
#ifndef __CELL_FIBER_H_SPU__
#define __CELL_FIBER_H_SPU__

#include <cell/fiber/spu_context.h>

#endif /* __CELL_FIBER_H_SPU__ */
