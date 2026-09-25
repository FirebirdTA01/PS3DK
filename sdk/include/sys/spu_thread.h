/*
 * PS3 Custom Toolchain — <sys/spu_thread.h>
 *
 * SPU thread management Lv2 syscall interface.
 * Implements canonical reference-SDK prototypes, types and constants,
 * with PSL1GHT compatibility aliases.
 */

#ifndef __PS3DK_SYS_SPU_THREAD_H__
#define __PS3DK_SYS_SPU_THREAD_H__

#include <stdint.h>
#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/return_code.h>
#include <sys/lv2_types.h>
#include <sys/lv2_syscall.h>
#include <sys/sys_types.h>
#include <sys/spu_image.h>
#include <sys/spu_thread_group.h>
#include <sys/spu.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPU thread ID invalid */
#define SYS_SPU_THREAD_ID_INVALID          0xFFFFFFFFU

/* SPU thread event type and key constants */
#define SYS_SPU_THREAD_EVENT_USER          0x1
#define SYS_SPU_THREAD_EVENT_DMA           0x2
#define SYS_SPU_THREAD_EVENT_USER_KEY      0xFFFFFFFF53505501ULL
#define SYS_SPU_THREAD_EVENT_DMA_KEY       0xFFFFFFFF53505502ULL

/* SPU thread DMA completion options */
#define SYS_SPU_THREAD_DMA_COMPLETION_STOP 0x0U
#define SYS_SPU_THREAD_DMA_COMPLETION_ANY  0x1U
#define SYS_SPU_THREAD_DMA_COMPLETION_ALL  0x2U

/* SPU thread argument helpers */
#define SYS_SPU_THREAD_ARGUMENT_LET_8(x)   (((uint64_t)(x)) << 32U)
#define SYS_SPU_THREAD_ARGUMENT_LET_16(x)  (((uint64_t)(x)) << 32U)
#define SYS_SPU_THREAD_ARGUMENT_LET_32(x)  (((uint64_t)(x)) << 32U)
#define SYS_SPU_THREAD_ARGUMENT_LET_64(x)  ((uint64_t)(x))

/* ------------------------------------------------------------------ *
 * sys_spu_thread_group_* Lv2 syscalls
 * ------------------------------------------------------------------ */

static inline int sys_spu_thread_group_create(sys_spu_thread_group_t *id,
                                              unsigned int num,
                                              unsigned int prio,
                                              sys_spu_thread_group_attribute_t *attr)
{
    lv2syscall4(170, (uint64_t)(uintptr_t)id, (uint64_t)num, (uint64_t)prio, (uint64_t)(uintptr_t)attr);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_group_destroy(sys_spu_thread_group_t id)
{
    lv2syscall1(171, (uint64_t)id);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_group_start(sys_spu_thread_group_t id)
{
    lv2syscall1(173, (uint64_t)id);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_group_suspend(sys_spu_thread_group_t id)
{
    lv2syscall1(174, (uint64_t)id);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_group_resume(sys_spu_thread_group_t id)
{
    lv2syscall1(175, (uint64_t)id);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_group_yield(sys_spu_thread_group_t id)
{
    lv2syscall1(176, (uint64_t)id);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_group_terminate(sys_spu_thread_group_t id, int value)
{
    lv2syscall2(177, (uint64_t)id, (uint64_t)value);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_group_join(sys_spu_thread_group_t gid,
                                            int *cause,
                                            int *status)
{
    lv2syscall3(178, (uint64_t)gid, (uint64_t)(uintptr_t)cause, (uint64_t)(uintptr_t)status);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_group_set_priority(sys_spu_thread_group_t id, unsigned int prio)
{
    lv2syscall2(179, (uint64_t)id, (uint64_t)prio);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_group_get_priority(sys_spu_thread_group_t id, unsigned int *prio)
{
    lv2syscall2(180, (uint64_t)id, (uint64_t)(uintptr_t)prio);
    return_to_user_prog(int);
}

/* ------------------------------------------------------------------ *
 * sys_spu_thread_* Lv2 syscalls
 * ------------------------------------------------------------------ */

static inline int sys_spu_thread_initialize(sys_spu_thread_t *thread,
                                            sys_spu_thread_group_t group,
                                            unsigned int spu_num,
                                            sys_spu_image_t *img,
                                            sys_spu_thread_attribute_t *attr,
                                            sys_spu_thread_argument_t *arg)
{
    lv2syscall6(172, (uint64_t)(uintptr_t)thread, (uint64_t)group, (uint64_t)spu_num,
                (uint64_t)(uintptr_t)img, (uint64_t)(uintptr_t)attr, (uint64_t)(uintptr_t)arg);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_set_argument(sys_spu_thread_t id,
                                              sys_spu_thread_argument_t *arg)
{
    lv2syscall2(160, (uint64_t)id, (uint64_t)(uintptr_t)arg);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_get_exit_status(sys_spu_thread_t id, int *status)
{
    lv2syscall2(165, (uint64_t)id, (uint64_t)(uintptr_t)status);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_connect_event(sys_spu_thread_t id,
                                               sys_event_queue_t eq,
                                               sys_event_type_t et,
                                               uint8_t spup)
{
    lv2syscall4(191, (uint64_t)id, (uint64_t)eq, (uint64_t)et, (uint64_t)spup);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_disconnect_event(sys_spu_thread_t id,
                                                  sys_event_type_t et,
                                                  uint8_t spup)
{
    lv2syscall3(192, (uint64_t)id, (uint64_t)et, (uint64_t)spup);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_bind_queue(sys_spu_thread_t id,
                                            sys_event_queue_t spuq,
                                            uint32_t spuq_num)
{
    lv2syscall3(193, (uint64_t)id, (uint64_t)spuq, (uint64_t)spuq_num);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_unbind_queue(sys_spu_thread_t id,
                                              uint32_t spuq_num)
{
    lv2syscall2(194, (uint64_t)id, (uint64_t)spuq_num);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_write_ls(sys_spu_thread_t id,
                                          uint32_t address,
                                          uint64_t value,
                                          size_t type)
{
    lv2syscall4(181, (uint64_t)id, (uint64_t)address, value, (uint64_t)type);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_read_ls(sys_spu_thread_t id,
                                         uint32_t address,
                                         uint64_t *value,
                                         size_t type)
{
    lv2syscall4(182, (uint64_t)id, (uint64_t)address, (uint64_t)(uintptr_t)value, (uint64_t)type);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_write_snr(sys_spu_thread_t id,
                                           int number,
                                           uint32_t value)
{
    lv2syscall3(184, (uint64_t)id, (uint64_t)number, (uint64_t)value);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_set_spu_cfg(sys_spu_thread_t id,
                                             uint64_t value)
{
    lv2syscall2(187, (uint64_t)id, value);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_get_spu_cfg(sys_spu_thread_t id,
                                             uint64_t *value)
{
    lv2syscall2(188, (uint64_t)id, (uint64_t)(uintptr_t)value);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_write_spu_mb(sys_spu_thread_t id,
                                              uint32_t value)
{
    lv2syscall2(190, (uint64_t)id, (uint64_t)value);
    return_to_user_prog(int);
}

static inline int sys_spu_thread_recover_page_fault(sys_spu_thread_t id)
{
    lv2syscall1(198, (uint64_t)id);
    return_to_user_prog(int);
}

/* ------------------------------------------------------------------ *
 * Compatibility forwarders & aliases
 * ------------------------------------------------------------------ */

static inline int sys_spu_thread_write_to_ls(sys_spu_thread_t id,
                                             uint32_t address,
                                             uint64_t value,
                                             size_t type)
{
    return sys_spu_thread_write_ls(id, address, value, type);
}

static inline int sys_spu_thread_read_from_ls(sys_spu_thread_t id,
                                              uint32_t address,
                                              uint64_t *value,
                                              size_t type)
{
    return sys_spu_thread_read_ls(id, address, value, type);
}

static inline int sys_spu_thread_write_signal(sys_spu_thread_t id,
                                              int number,
                                              uint32_t value)
{
    return sys_spu_thread_write_snr(id, number, value);
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SYS_SPU_THREAD_H__ */
