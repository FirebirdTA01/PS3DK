/* sys/event_flag.h - SPU-side lv2 event-flag syscall surface.
 *
 * The SPU may directly hit lv2 to set bits on a kernel event flag
 * via two SPU-callable syscalls:
 *
 *   sys_event_flag_set_bit(ef, bit)            blocking — spins on
 *                                              EBUSY until accepted
 *   sys_event_flag_set_bit_impatient(ef, bit)  non-blocking — returns
 *                                              EBUSY immediately if
 *                                              the receiver FIFO is
 *                                              full
 *
 * Both go through the SPU mailbox channels (ch28 / ch29 / ch30) into
 * the kernel.  Implementations live in libsputhread.a (SPU).  PPU-side
 * code uses sys_event_flag_create / wait / set / clear from
 * <sys/synchronization.h> instead - the SPU side here is the
 * fast-path producer that lets a worker SPU notify a PPU waiter
 * without bouncing through the SPU thread group.
 */
#ifndef __PS3DK_SYS_EVENT_FLAG_H__
#define __PS3DK_SYS_EVENT_FLAG_H__

#ifndef __SPU__
#error "sys/event_flag.h is SPU-only — use sys/synchronization.h on PPU"
#endif

#include <stdint.h>
#include <sys/return_code.h>     /* CELL_OK */

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t sys_event_flag_t;

extern int sys_event_flag_set_bit(sys_event_flag_t ef, uint8_t bitn);
extern int sys_event_flag_set_bit_impatient(sys_event_flag_t ef, uint8_t bitn);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SYS_EVENT_FLAG_H__ */
