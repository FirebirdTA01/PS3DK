#ifndef __SYS_SYSTIME_H__
#define __SYS_SYSTIME_H__
#include <ppu-lv2.h>
#include <lv2/systime.h>
#ifdef __cplusplus
extern "C" {
#endif
LV2_INLINE u64 sysGetTimebaseFrequency(void)
{
	lv2syscall0(147);
	return_to_user_prog(u64);
}
LV2_SYSCALL sysGetCurrentTime(u64 *sec, u64 *nsec)
{
	lv2syscall2(145, (u64)(uintptr_t)sec, (u64)(uintptr_t)nsec);
	return_to_user_prog(s32);
}
#ifdef __cplusplus
}
#endif
#endif
