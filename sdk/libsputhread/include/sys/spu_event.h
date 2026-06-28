/* sys/spu_event.h - SPU event-port lv2 syscall wrappers.
 *
 * Send / receive events between SPU threads and PPU threads via the
 * lv2 sys_event subsystem.  Wire encoding:
 *   event word = (spup << 24) | (data0 & 0x00FFFFFF) [| 0x40000000 throw bit]
 *   spup must be 0..63 (EVENT_PORT_MAX_NUM)
 *
 * Implementations live in libsputhread.a (sdk/libsputhread/src/
 * event_port.S).  Declarations match the reference SDK so existing
 * SPU code that uses these syscalls links unchanged.
 */
#ifndef __PS3DK_SYS_SPU_EVENT_H__
#define __PS3DK_SYS_SPU_EVENT_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVENT_DATA0_MASK   0x00FFFFFFU
#define EVENT_PORT_SHIFT   24
#define EVENT_PORT_MAX_NUM 63

#define EVENT_PRINTF_PORT  1U

extern int sys_spu_thread_send_event     (uint8_t  spup,
                                          uint32_t data0,
                                          uint32_t data1);
extern int sys_spu_thread_throw_event    (uint8_t  spup,
                                          uint32_t data0,
                                          uint32_t data1);
extern int sys_spu_thread_receive_event  (uint32_t  spuq,
                                          uint32_t *d1,
                                          uint32_t *d2,
                                          uint32_t *d3);
extern int sys_spu_thread_tryreceive_event(uint32_t  spuq,
                                          uint32_t *d1,
                                          uint32_t *d2,
                                          uint32_t *d3);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* --- PSL1GHT-compat aliases (deprecated) ----------------------------
 * Upstream PSL1GHT names these without the sys_ prefix.  libsputhread
 * provides weak alias symbols (see src/event_port.S); these deprecated
 * extern decls make each call site warn to switch to the modern
 * sys_spu_thread_* name.  Real symbols (not static inline) so a sample
 * that self-declares the legacy name does not conflict.  Define
 * __PS3DK_NO_PSL1GHT_COMPAT__ to drop the legacy aliases. */
#ifndef __PS3DK_NO_PSL1GHT_COMPAT__
extern int spu_thread_send_event(uint8_t spup, uint32_t data0, uint32_t data1)
    __attribute__((deprecated("PSL1GHT-compat: use sys_spu_thread_send_event()")));
extern int spu_thread_throw_event(uint8_t spup, uint32_t data0, uint32_t data1)
    __attribute__((deprecated("PSL1GHT-compat: use sys_spu_thread_throw_event()")));
extern int spu_thread_receive_event(uint32_t spuq, uint32_t *d1, uint32_t *d2, uint32_t *d3)
    __attribute__((deprecated("PSL1GHT-compat: use sys_spu_thread_receive_event()")));
extern int spu_thread_tryreceive_event(uint32_t spuq, uint32_t *d1, uint32_t *d2, uint32_t *d3)
    __attribute__((deprecated("PSL1GHT-compat: use sys_spu_thread_tryreceive_event()")));
#endif /* __PS3DK_NO_PSL1GHT_COMPAT__ */

#endif /* __PS3DK_SYS_SPU_EVENT_H__ */
