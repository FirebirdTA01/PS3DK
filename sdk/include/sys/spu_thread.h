/* sys/spu_thread.h - PPU-side reference-SDK forwarders over PSL1GHT
 * SPU-thread syscalls.
 *
 * Pulls in PSL1GHT's <sys/spu.h> for the underlying types and inline
 * sysSpu* syscall wrappers, then re-exports them under the
 * reference-SDK snake_case names so the reference SDK source compiles
 * unchanged.  Sample code can mix sysSpu* and sys_spu_* freely; the
 * forwarders compile down to direct branches with no runtime cost.
 */
#ifndef __PS3DK_SYS_SPU_THREAD_H__
#define __PS3DK_SYS_SPU_THREAD_H__

#include <stdint.h>
#include <stddef.h>
#include <ppu-lv2.h>
#include <sys/spu.h>
#include <sys/spu_image.h>
#include <sys/spu_thread_group.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sys_spu_image_* live in <sys/spu_image.h>, which we include above. */

/* SPU thread argument-passing helpers — pack a host-side scalar into
 * the high 32 bits of the 64-bit argument register the SPU sees in
 * its r3..r6 entry-point parameters. */
#define SYS_SPU_THREAD_ARGUMENT_LET_8(x)   (((uint64_t)(x)) << 32U)
#define SYS_SPU_THREAD_ARGUMENT_LET_16(x)  (((uint64_t)(x)) << 32U)
#define SYS_SPU_THREAD_ARGUMENT_LET_32(x)  (((uint64_t)(x)) << 32U)
#define SYS_SPU_THREAD_ARGUMENT_LET_64(x)  ((uint64_t)(x))

/* ------------------------------------------------------------------ *
 * sys_spu_thread_group_*
 * ------------------------------------------------------------------ */

static inline int sys_spu_thread_group_create(sys_spu_thread_group_t *id,
                                              unsigned int num,
                                              unsigned int prio,
                                              sys_spu_thread_group_attribute_t *attr)
{
    return (int)sysSpuThreadGroupCreate(id, num, prio, attr);
}

static inline int sys_spu_thread_group_start(sys_spu_thread_group_t id)
{
    return (int)sysSpuThreadGroupStart(id);
}

static inline int sys_spu_thread_group_join(sys_spu_thread_group_t gid,
                                            int *cause,
                                            int *status)
{
    return (int)sysSpuThreadGroupJoin(gid, (u32 *)cause, (u32 *)status);
}

static inline int sys_spu_thread_group_destroy(sys_spu_thread_group_t id)
{
    return (int)sysSpuThreadGroupDestroy(id);
}

static inline int sys_spu_thread_group_terminate(sys_spu_thread_group_t id, int value)
{
    return (int)sysSpuThreadGroupTerminate(id, (u32)value);
}

/* ------------------------------------------------------------------ *
 * sys_spu_thread_*
 * ------------------------------------------------------------------ */

static inline int sys_spu_thread_initialize(sys_spu_thread_t *thread,
                                            sys_spu_thread_group_t group,
                                            unsigned int spu_num,
                                            sys_spu_image_t *img,
                                            sys_spu_thread_attribute_t *attr,
                                            sys_spu_thread_argument_t *arg)
{
    return (int)sysSpuThreadInitialize(thread, group, spu_num,
                                       (sysSpuImage *)img,
                                       attr, (sysSpuThreadArgument *)arg);
}

static inline int sys_spu_thread_get_exit_status(sys_spu_thread_t id, int *status)
{
    return (int)sysSpuThreadGetExitStatus(id, (s32 *)status);
}

static inline int sys_spu_thread_write_snr(sys_spu_thread_t id,
                                           int number,
                                           uint32_t value)
{
    return (int)sysSpuThreadWriteSignal(id, (u32)number, (u32)value);
}

/* The same operation under its other reference spelling. */
static inline int sys_spu_thread_write_signal(sys_spu_thread_t id, int number, uint32_t value)
{
    return sys_spu_thread_write_snr(id, number, value);
}

/* ------------------------------------------------------------------ *
 * SPU thread context, events and queues (Lv-2 syscalls 166..198)
 * ------------------------------------------------------------------ */

#define SYS_SPU_THREAD_ID_INVALID            0xFFFFFFFFU

/* sys_spu_thread_connect_event event types, and the event source keys
 * an event queue receives them under. */
#define SYS_SPU_THREAD_EVENT_USER            0x1
#define SYS_SPU_THREAD_EVENT_DMA             0x2
#define SYS_SPU_THREAD_EVENT_USER_KEY        0xFFFFFFFF53505501ULL
#define SYS_SPU_THREAD_EVENT_DMA_KEY         0xFFFFFFFF53505502ULL

/* DMA completion modes for the SPU DMA event. */
#define SYS_SPU_THREAD_DMA_COMPLETION_STOP   0x0U
#define SYS_SPU_THREAD_DMA_COMPLETION_ANY    0x1U
#define SYS_SPU_THREAD_DMA_COMPLETION_ALL    0x2U

/* Store `value` into the thread's local store at `address`; `type` is
 * the access width in bytes (1, 2, 4 or 8).  The thread's group must be
 * stopped or suspended. */
static inline int sys_spu_thread_write_ls(sys_spu_thread_t id, uint32_t address,
                                          uint64_t value, size_t type)
{
    lv2syscall4(181, id, address, value, type);
    return_to_user_prog(int);
}

/* Load `type` bytes from the thread's local store at `address`. */
static inline int sys_spu_thread_read_ls(sys_spu_thread_t id, uint32_t address,
                                         uint64_t *value, size_t type)
{
    lv2syscall4(182, id, address, (u64)(uintptr_t)value, type);
    return_to_user_prog(int);
}

/* Older spellings of the two calls above. */
static inline int sys_spu_thread_write_to_ls(sys_spu_thread_t id, uint32_t address,
                                             uint64_t value, size_t type)
{
    return sys_spu_thread_write_ls(id, address, value, type);
}

static inline int sys_spu_thread_read_from_ls(sys_spu_thread_t id, uint32_t address,
                                              uint64_t *value, size_t type)
{
    return sys_spu_thread_read_ls(id, address, value, type);
}

/* Write a word to the thread's SPU inbound mailbox. */
static inline int sys_spu_thread_write_spu_mb(sys_spu_thread_t id, uint32_t value)
{
    lv2syscall2(190, id, value);
    return_to_user_prog(int);
}

/* SPU configuration register (signal-notification OR modes). */
static inline int sys_spu_thread_set_spu_cfg(sys_spu_thread_t id, uint64_t value)
{
    lv2syscall2(187, id, value);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_get_spu_cfg(sys_spu_thread_t id, uint64_t *value)
{
    lv2syscall2(188, id, (u64)(uintptr_t)value);
    return_to_user_prog(int);
}

/* Route the thread's user (sys_spu_thread_send_event) or DMA events on
 * SPU port `spup` to event queue `eq`. */
static inline int sys_spu_thread_connect_event(sys_spu_thread_t id, sys_event_queue_t eq,
                                               sys_event_type_t et, uint8_t spup)
{
    lv2syscall4(191, id, eq, et, spup);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_disconnect_event(sys_spu_thread_t id, sys_event_type_t et,
                                                  uint8_t spup)
{
    lv2syscall3(192, id, et, spup);
    return_to_user_prog(int);
}

/* Bind event queue `spuq` so the SPU can receive from it under the
 * SPU-side queue number `spuq_num`. */
static inline int sys_spu_thread_bind_queue(sys_spu_thread_t id, sys_event_queue_t spuq,
                                            uint32_t spuq_num)
{
    lv2syscall3(193, id, spuq, spuq_num);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_unbind_queue(sys_spu_thread_t id, uint32_t spuq_num)
{
    lv2syscall2(194, id, spuq_num);
    return_to_user_prog(int);
}

/* Replace the arguments the thread receives when its group next starts. */
static inline int sys_spu_thread_set_argument(sys_spu_thread_t id, sys_spu_thread_argument_t *arg)
{
    lv2syscall2(166, id, (u64)(uintptr_t)arg);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_recover_page_fault(sys_spu_thread_t id)
{
    lv2syscall1(198, id);
    return_to_user_prog(int);
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SYS_SPU_THREAD_H__ */
