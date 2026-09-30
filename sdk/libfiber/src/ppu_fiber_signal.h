/* ppu_fiber_signal.h - SPU side of PPU fiber signalling (libfiber.a).
 *
 * A PPU fiber and its scheduler are 128-byte-aligned objects in main
 * memory, shared with the PPU fiber runtime, so every update goes through
 * a line reservation (GETLLAR / PUTLLC).
 *
 * Fiber line:
 *   +0x00 u32 state: 0x08 waiting for a signal, 0x10 signal pending,
 *             0x01 woken; 0 or 0x40 = not signalable
 *   +0x04 u32 next fiber in the scheduler's ready list
 *   +0x08 u32 EA of the fiber's scheduler
 *   +0x0c u8  ready list (priority) index
 *   +0x10 u32 1 when the fiber runs under a worker control
 *   +0x18 u64 EA of that worker control
 * Scheduler line:
 *   +0x00 u32[8] ready list heads, by index
 *   +0x40 u8  worker count
 *   +0x48 u8[8] ready flags, by index
 * Worker control = scheduler (0x200 bytes), then its control line at +0x200:
 *   +0x02 u8  wake-up pending   +0x03 u8 bit 6: running
 *   +0x04 u32 sleeping-worker mask
 *   +0x08 u32 lv2 event flag the sleeping workers wait on
 */
#ifndef FIBER_PPU_SIGNAL_H
#define FIBER_PPU_SIGNAL_H

#include <stdint.h>
#include <spu_mfcio.h>

#define FIBER_ERROR_STAT         ((int)0x8076000Fu)
#define FIBER_ERROR_ALIGN        ((int)0x80760010u)
#define FIBER_ERROR_NULL_POINTER ((int)0x80760011u)

#define FIBER_WAITING  0x08u
#define FIBER_PENDING  0x10u
#define FIBER_WOKEN    0x01u
#define FIBER_CLOSED   0x40u

#define WORKER_CONTROL_LINE 0x200

/* What signalling a fiber found out. */
typedef struct fiber_signal_result {
	uint32_t numWorker;      /* the scheduler's worker count */
	uint32_t controlled;     /* fiber word 0x10 */
	uint64_t eaControl;      /* fiber u64 0x18 */
} fiber_signal_result;

/* Signal the fiber at eaFiber: a waiting fiber is put on its scheduler's
 * ready list, any other is marked signal-pending.  readWorkers: also read
 * the worker count when the fiber was not waiting. */
int __fiber_ppu_signal(uint32_t eaFiber, int readWorkers, fiber_signal_result *out);

/* Wake one sleeping worker of the worker control at eaControl. */
int __fiber_worker_control_wakeup(uint32_t eaControl);

#endif
