/* Legacy condition variables; syscall pointer operands support ILP32 and LP64. */
#ifndef __SYS_COND_H__
#define __SYS_COND_H__

#include <stdint.h>
#include <sys/lv2_syscall.h>
#include <lv2/cond.h>

#define SYS_COND_ATTR_PSHARED 0x0200

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sys_cond_attr {
    u32 attr_pshared;
    s32 flags;
    u64 key;
    char name[8];
} sys_cond_attr_t;

#define sysCondAttrInitialize(x) do {             \
    (x).attr_pshared = SYS_COND_ATTR_PSHARED;      \
    (x).key = 0;                                 \
    (x).flags = 0;                               \
    (x).name[0] = '\0';                          \
} while (0)

LV2_SYSCALL sysCondCreate(sys_cond_t *cond, sys_mutex_t mutex, const sys_cond_attr_t *attr)
{
    lv2syscall3(105, (u64)(uintptr_t)cond, mutex, (u64)(uintptr_t)attr);
    return_to_user_prog(s32);
}

LV2_SYSCALL sysCondDestroy(sys_cond_t cond)
{
    lv2syscall1(106, cond);
    return_to_user_prog(s32);
}

LV2_SYSCALL sysCondWait(sys_cond_t cond, u64 timeout_usec)
{
    lv2syscall2(107, cond, timeout_usec);
    return_to_user_prog(s32);
}

LV2_SYSCALL sysCondSignal(sys_cond_t cond)
{
    lv2syscall1(108, cond);
    return_to_user_prog(s32);
}

LV2_SYSCALL sysCondBroadcast(sys_cond_t cond)
{
    lv2syscall1(109, cond);
    return_to_user_prog(s32);
}

#ifdef __cplusplus
}
#endif
#endif
