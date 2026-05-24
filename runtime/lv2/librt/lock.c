/*
 * lock.c — LwMutex wrappers for Lv-2 newlib reentrancy support.
 *
 * Replaces the libsysbase syscall wrappers previously provided by the
 * upstream prefix.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <errno.h>
#include <sys/reent.h>
#include <sys/mutex.h>
#include <sys/lv2errno.h>

int
__librt_sys_lwmutex_create_r(struct _reent *r, sys_lwmutex_t *lwmutex,
                             const sys_lwmutex_attr_t *attr)
{
	return lv2errno_r(r, sysLwMutexCreate(lwmutex, attr));
}

int
__librt_sys_lwmutex_destroy_r(struct _reent *r, sys_lwmutex_t *lwmutex)
{
	return lv2errno_r(r, sysLwMutexDestroy(lwmutex));
}

int
__librt_sys_lwmutex_lock_r(struct _reent *r, sys_lwmutex_t *lwmutex,
                           unsigned long long timeout)
{
	return lv2errno_r(r, sysLwMutexLock(lwmutex, timeout));
}

int
__librt_sys_lwmutex_trylock_r(struct _reent *r, sys_lwmutex_t *lwmutex)
{
	return lv2errno_r(r, sysLwMutexTryLock(lwmutex));
}

int
__librt_sys_lwmutex_unlock_r(struct _reent *r, sys_lwmutex_t *lwmutex)
{
	return lv2errno_r(r, sysLwMutexUnlock(lwmutex));
}
