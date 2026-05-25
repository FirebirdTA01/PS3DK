/* cell/spurs/policy_module.h - SPURS policy-module surface.
 *
 * A policy module (PM) intercepts SPURS scheduling events and can
 * override or extend default behaviour.  The SPU-side PM receives
 * a CellSpursModulePollStatus each poll tick; the PPU side ships
 * the PM binary embedded in the workload.
 */
#ifndef __PS3DK_CELL_SPURS_POLICY_MODULE_H__
#define __PS3DK_CELL_SPURS_POLICY_MODULE_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CellSpursModulePollStatus — opaque poll-status word passed to the
 * PM's main entry point each tick.  The PM inspects bits to decide
 * which event callbacks to invoke.
 */
typedef uint32_t CellSpursModulePollStatus;

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SPURS_POLICY_MODULE_H__ */
