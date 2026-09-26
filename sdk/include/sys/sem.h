/* Legacy semaphore syscalls, including pointer-width-safe output operands. */
#ifndef __SYS_SEM_H__
#define __SYS_SEM_H__
#include <stdint.h>
#include <sys/lv2_syscall.h>

#define SYS_SEM_ATTR_PROTOCOL 0x0002
#define SYS_SEM_ATTR_PSHARED 0x0200
#ifdef __cplusplus
extern "C" {
#endif
typedef struct sys_sem_attr {
    u32 attr_protocol;
    u32 attr_pshared;
    u64 key;
    s32 flags;
    u32 pad;
    char name[8];
} sys_sem_attr_t;

LV2_SYSCALL sysSemCreate(sys_sem_t *sem, const sys_sem_attr_t *attr, s32 initial_val, s32 max_val)
{
    lv2syscall4(90, (u64)(uintptr_t)sem, (u64)(uintptr_t)attr, initial_val, max_val);
    return_to_user_prog(s32);
}
LV2_SYSCALL sysSemDestroy(sys_sem_t sem)
{
    lv2syscall1(91, sem);
    return_to_user_prog(s32);
}
LV2_SYSCALL sysSemWait(sys_sem_t sem, u64 timeout_usec)
{
    lv2syscall2(92, sem, timeout_usec);
    return_to_user_prog(s32);
}
LV2_SYSCALL sysSemTryWait(sys_sem_t sem)
{
    lv2syscall1(93, sem);
    return_to_user_prog(s32);
}
LV2_SYSCALL sysSemPost(sys_sem_t sem, s32 count)
{
    lv2syscall2(94, sem, count);
    return_to_user_prog(s32);
}
LV2_SYSCALL sysSemGetValue(sys_sem_t sem, s32 *count)
{
    lv2syscall2(114, sem, (u64)(uintptr_t)count);
    return_to_user_prog(s32);
}
#ifdef __cplusplus
}
#endif
#endif
