/* sys/lv2_fs_ext.h - Lv-2 filesystem syscalls not present in <sys/file.h>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 *
 * We ship PSL1GHT's <sys/file.h> verbatim from the vendored checkout, which
 * is pinned at eca3f99 on purpose. Upstream added chown/mount/unmount to
 * that header after the pin, so rather than move the pin (which drags in
 * unrelated changes, including the NID rename in PR #169) or fork a
 * twenty-wrapper header for the sake of three additions, they live here.
 *
 * These belong in <sys/file.h> the day we shadow it the way we already
 * shadow sys/socket.h, sys/thread.h, sys/memory.h and four others. Until
 * then, include this alongside it.
 *
 * A NOTE ON WHAT THE EMULATOR WILL TELL YOU: RPCS3 binds all three
 * syscalls, but sys_fs_chown (835) is a `todo()` one-liner that returns
 * CELL_OK without doing anything. A test asserting "chown succeeded" would
 * therefore pass forever while nothing happens, so chown is verifiable only
 * on hardware. sys_fs_mount (837) and sys_fs_unmount (838) have real
 * bodies, but mounting a device is not something a sample can meaningfully
 * assert under an emulator either.
 */

#ifndef __PS3DK_SYS_LV2_FS_EXT_H__
#define __PS3DK_SYS_LV2_FS_EXT_H__

#include <ppu-types.h>
#include <sys/lv2_syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Change a path's owner and group.  uid/gid are Lv-2 ids, not POSIX ones. */
LV2_SYSCALL
sysLv2FsChown(const char *path, s32 uid, s32 gid)
{
	lv2syscall3(835, (u64)path, (u64)uid, (u64)gid);
	return_to_user_prog(s32);
}

/*
 * Mount a device.  `writeProt' non-zero mounts read-only.
 *
 * The syscall takes eight arguments; upstream passes zero for the four we
 * have no names for, and so do we -- guessing at their meaning would be
 * worse than leaving them documented as unknown.
 */
LV2_SYSCALL
sysLv2FsMount(const char *deviceName, const char *deviceFileSystem,
	      const char *devicePath, s32 writeProt)
{
	lv2syscall8(837, (u64)deviceName, (u64)deviceFileSystem,
		    (u64)devicePath, 0, (u64)writeProt, 0, 0, 0);
	return_to_user_prog(s32);
}

/* Unmount whatever is mounted at `devicePath'. */
LV2_SYSCALL
sysLv2FsUnmount(const char *devicePath)
{
	lv2syscall3(838, (u64)devicePath, 0, 0);
	return_to_user_prog(s32);
}

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_SYS_LV2_FS_EXT_H__ */
