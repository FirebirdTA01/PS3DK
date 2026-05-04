/* cell/fiber.h - libfiber umbrella header.
 *
 * Clean-room umbrella that pulls in the full libfiber public surface:
 * error codes, type definitions, and entry-point declarations across
 * all four feature areas (context, fiber, scheduler, worker-control).
 */
#ifndef __PS3DK_CELL_FIBER_H__
#define __PS3DK_CELL_FIBER_H__

#include <cell/fiber/error.h>
#include <cell/fiber/version.h>
#include <cell/fiber/ppu_context_types.h>
#include <cell/fiber/ppu_fiber_types.h>
#include <cell/fiber/ppu_fiber_trace_types.h>
#include <cell/fiber/user_scheduler_types.h>
#include <cell/fiber/ppu_initialize.h>
#include <cell/fiber/ppu_context.h>
#include <cell/fiber/ppu_fiber.h>
#include <cell/fiber/user_scheduler.h>
#include <cell/fiber/ppu_fiber_worker_control.h>

#endif /* __PS3DK_CELL_FIBER_H__ */
