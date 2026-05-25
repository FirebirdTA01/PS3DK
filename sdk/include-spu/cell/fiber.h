/* cell/fiber.h -- SPU-side fiber declarations.
 *
 * The SPU fiber API surface is split across sub-headers under
 * cell/fiber/ (ppu_fiber.h, spu_context.h, ppuUtilDefine.h,
 * ppuUtilRuntime.h).  These sub-headers are not yet shipped.
 *
 * SPU code that needs fiber entry points should include the
 * individual sub-headers directly or provide its own prototypes
 * until the full surface is available.  The libfiber.a archive
 * resolves -lfiber at link time but currently contains no
 * implementations.
 */
#ifndef __CELL_FIBER_H_SPU__
#define __CELL_FIBER_H_SPU__

/* Sub-headers (not yet shipped):
 *   cell/fiber/ppu_fiber.h
 *   cell/fiber/spu_context.h
 *   cell/fiber/ppuUtilDefine.h
 *   cell/fiber/ppuUtilRuntime.h
 */

#endif /* __CELL_FIBER_H_SPU__ */
