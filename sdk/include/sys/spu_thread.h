/* sys/spu_thread.h - PPU-side reference-SDK forwarders over PSL1GHT
 * SPU-thread syscalls.
 *
 * Pulls in PSL1GHT's <sys/spu.h> for the underlying types and inline
 * sysSpu* syscall wrappers, then re-exports them under the
 * reference-SDK snake_case names so cell-SDK source compiles
 * unchanged.  Sample code can mix sysSpu* and sys_spu_* freely; the
 * forwarders compile down to direct branches with no runtime cost.
 */
#ifndef __PS3DK_SYS_SPU_THREAD_H__
#define __PS3DK_SYS_SPU_THREAD_H__

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
                                       attr, arg);
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

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SYS_SPU_THREAD_H__ */
