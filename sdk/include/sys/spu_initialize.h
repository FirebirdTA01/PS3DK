/* sys/spu_initialize.h - SPU subsystem initialization syscall.
 *
 * Reference-SDK-named entry point for syscall 169
 * (SYS_SPU_INITIALIZE).  Must be called once per process before any
 * SPU thread group / raw SPU is created; tells lv2 how many physical
 * SPUs the process intends to use and how many of those should be
 * carved out as raw (direct-MMIO) SPUs.
 *
 *   max_usable_spu : 1..6   total SPUs the process will touch
 *   max_raw_spu    : 0..5   subset of those reserved as raw SPUs
 *
 * Returns 0 on success, lv2 error code otherwise.  The kernel rejects
 * a second call from the same process and the (max_raw_spu >
 * max_usable_spu) case.
 *
 * PSL1GHT exposes the same syscall as `sysSpuInitialize` from
 * <sys/spu.h>; this header ships the reference-SDK snake_case
 * spelling over our own lv2_syscall.h macros so cell-style code
 * builds without dragging PSL1GHT's <ppu-lv2.h> chain in.
 */
#ifndef __PS3DK_SYS_SPU_INITIALIZE_H__
#define __PS3DK_SYS_SPU_INITIALIZE_H__

/* Pull in the lv2 syscall macro family (lv2syscall2, return_to_user_prog,
 * LV2_SYSCALL).  When PSL1GHT's <ppu-lv2.h> has already been included
 * by some earlier header it has already defined the same macro set;
 * skip re-including ours to avoid the spurious redefinition warning. */
#ifndef lv2syscall2
#  include <sys/lv2_syscall.h>
#endif

#ifndef SYS_SPU_INITIALIZE
#define SYS_SPU_INITIALIZE  169
#endif

#ifdef __cplusplus
extern "C" {
#endif

LV2_SYSCALL sys_spu_initialize(unsigned int max_usable_spu,
                               unsigned int max_raw_spu)
{
    lv2syscall2(SYS_SPU_INITIALIZE,
                (u64)max_usable_spu,
                (u64)max_raw_spu);
    return_to_user_prog(int);
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SYS_SPU_INITIALIZE_H__ */
