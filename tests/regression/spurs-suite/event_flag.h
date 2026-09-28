/* spurs-suite event flag: task kinds and the result slot layout. */
#ifndef SPURS_SUITE_EVENT_FLAG_H
#define SPURS_SUITE_EVENT_FLAG_H

/* task kinds (argTask.u32[3]) */
#define KIND_SET_SPU2PPU  0   /* waits for the PPU to block, then sets 0x0003 */
#define KIND_WAIT_PPU2SPU 1   /* blocks on 0x00f0 (OR); the PPU sets 0x0010 */
#define KIND_WAIT_SPU2SPU 2   /* blocks on 0x0300 (AND), then TryWait/Clear/getters */
#define KIND_SET_SPU2SPU  3   /* sets 0x0100 then 0x0200 */

/* syscall diagnostics, run before the event flag scenarios */
#define KIND_DIAG_YIELD        4   /* cellSpursYield returns */
#define KIND_DIAG_SIGNAL_SELF  5   /* signal self, then WaitSignal returns without switching */
#define KIND_DIAG_WAIT_SIGNAL  6   /* WaitSignal blocks until the PPU signals */

#include "common.h"

/* extra for KIND_WAIT_SPU2SPU: TryWait rc low 16 bits << 16 | direction << 8 | clear mode */

#endif
