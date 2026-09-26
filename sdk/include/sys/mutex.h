/* Legacy mutex API. Pointer operands widen through uintptr_t for both PPU ABIs. */
#ifndef __SYS_MUTEX_H__
#define __SYS_MUTEX_H__

#include <stdint.h>
#include <sys/lv2_syscall.h>
#include <lv2/mutex.h>

#define SYS_MUTEX_PROTOCOL_FIFO         1
#define SYS_MUTEX_PROTOCOL_PRIO         2
#define SYS_MUTEX_PROTOCOL_PRIO_INHERIT  3
#define SYS_MUTEX_ATTR_RECURSIVE         0x0010
#define SYS_MUTEX_ATTR_NOT_RECURSIVE     0x0020
#define SYS_MUTEX_ATTR_NOT_PSHARED       0x0200
#define SYS_MUTEX_ATTR_ADAPTIVE          0x1000
#define SYS_MUTEX_ATTR_NOT_ADAPTIVE      0x2000

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sys_mutex_attr {
    u32 attr_protocol;
    u32 attr_recursive;
    u32 attr_pshared;
    u32 attr_adaptive;
    u64 key;
    s32 flags;
    u32 _pad;
    char name[8];
} sys_mutex_attr_t;

/* Preserve the legacy initializer, including its untouched padding bytes. */
#define sysMutexAttrInitialize(x) do {                   \
    (x).attr_protocol = SYS_MUTEX_PROTOCOL_PRIO;          \
    (x).attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;     \
    (x).attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;         \
    (x).attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;       \
    (x).key = 0;                                         \
    (x).flags = 0;                                       \
    (x).name[0] = '\0';                                  \
} while (0)

LV2_SYSCALL sysMutexCreate(sys_mutex_t *mutex, const sys_mutex_attr_t *attr)
{
    lv2syscall2(100, (u64)(uintptr_t)mutex, (u64)(uintptr_t)attr);
    return_to_user_prog(s32);
}

LV2_SYSCALL sysMutexDestroy(sys_mutex_t mutex)
{
    lv2syscall1(101, mutex);
    return_to_user_prog(s32);
}

LV2_SYSCALL sysMutexLock(sys_mutex_t mutex, u64 timeout_usec)
{
    lv2syscall2(102, mutex, timeout_usec);
    return_to_user_prog(s32);
}

LV2_SYSCALL sysMutexTryLock(sys_mutex_t mutex)
{
    lv2syscall1(103, mutex);
    return_to_user_prog(s32);
}

LV2_SYSCALL sysMutexUnlock(sys_mutex_t mutex)
{
    lv2syscall1(104, mutex);
    return_to_user_prog(s32);
}

#ifdef __cplusplus
}
#endif
#endif
