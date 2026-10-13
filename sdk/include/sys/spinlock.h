/*
 * PS3 Custom Toolchain - <sys/spinlock.h>
 *
 * LV2 spinlock (sysPrxForUser imports, liblv2_stub): an initialize / lock /
 * trylock / unlock triple over a 32-bit lock word.  These are imports, not
 * inline syscalls — the module owns the implementation.
 */

#ifndef PS3TC_SYS_SPINLOCK_H
#define PS3TC_SYS_SPINLOCK_H

#ifdef __cplusplus
extern "C" {
#endif

extern void sys_spinlock_initialize(int *lock);
extern void sys_spinlock_lock(int *lock);
extern int  sys_spinlock_trylock(int *lock);
extern void sys_spinlock_unlock(int *lock);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_SYS_SPINLOCK_H */
