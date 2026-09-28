/* spurs-event-flag: roles and result layout shared by PPU and SPU. */
#ifndef SPURS_EVENT_FLAG_VALUES_H
#define SPURS_EVENT_FLAG_VALUES_H

/* task kinds (argTask.u32[3]) */
#define KIND_SET_SPU2PPU  0   /* waits for the PPU to block, then sets 0x0003 */
#define KIND_WAIT_PPU2SPU 1   /* blocks on 0x00f0 (OR); the PPU sets 0x0010 */
#define KIND_WAIT_SPU2SPU 2   /* blocks on 0x0300 (AND), then TryWait/Clear/getters */
#define KIND_SET_SPU2SPU  3   /* sets 0x0100 then 0x0200 */

/* syscall diagnostics, run before the event flag scenarios */
#define KIND_DIAG_YIELD        4   /* cellSpursYield returns */
#define KIND_DIAG_SIGNAL_SELF  5   /* signal self, then WaitSignal returns without switching */
#define KIND_DIAG_WAIT_SIGNAL  6   /* WaitSignal blocks until the PPU signals */

#define RESULT_MAGIC 0xc0de0000u

/* one 16-byte result slot per kind: magic|kind, status, bits, extra */
typedef struct result_slot {
    unsigned int magic, status, bits, extra;
} __attribute__((aligned(16))) result_slot;

/* extra for KIND_WAIT_SPU2SPU: TryWait rc low 16 bits << 16 | direction << 8 | clear mode */

#endif
