/*! \file file.h
 \brief File management functions.

 Our copy, derived from the PSL1GHT <sys/file.h> wrapper surface with the
 pointer-cast regression fixed: in the default PPU (ILP32) ABI every pointer
 is 32 bits, so a bare `(u64)ptr` widens a 32-bit value into a 64-bit
 register and the compiler warns `cast from pointer to integer of different
 size` under -Werror.  We instead go through `uintptr_t` first
 (`(u64)(uintptr_t)ptr`): truncate-to-pointer-width, then widen-to-u64,
 which is the value the kernel actually sees.  All signatures are
 otherwise unchanged.
 */

#ifndef __SYS_FILE_H__
#define __SYS_FILE_H__

#include <stdint.h>
#include <ppu-lv2.h>
#include <lv2/sysfs.h>

#ifdef __cplusplus
	extern "C" {
#endif

LV2_SYSCALL sysLv2FsOpen(const char *path,s32 oflags,s32 *fd,u32 mode,const void *arg,u64 argsize)
{
	lv2syscall6(801,(u64)(uintptr_t)path,oflags,(u64)(uintptr_t)fd,mode,(u64)(uintptr_t)arg,argsize);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsClose(s32 fd)
{
	lv2syscall1(804,fd);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsRead(s32 fd,void *ptr,u64 len,u64 *read)
{
	lv2syscall4(802,fd,(u64)(uintptr_t)ptr,len,(u64)(uintptr_t)read);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsWrite(s32 fd,const void *ptr,u64 len,u64 *written)
{
	lv2syscall4(803,fd,(u64)(uintptr_t)ptr,len,(u64)(uintptr_t)written);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsStat(const char *path,sysFSStat *stat)
{
	lv2syscall2(808,(u64)(uintptr_t)path,(u64)(uintptr_t)stat);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsFStat(s32 fd,sysFSStat *stat)
{
	lv2syscall2(809,fd,(u64)(uintptr_t)stat);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsChmod(const char *path,s32 mode)
{
	lv2syscall2(834,(u64)(uintptr_t)path,mode);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsOpenDir(const char *path,s32 *fd)
{
	lv2syscall2(805,(u64)(uintptr_t)path,(u64)(uintptr_t)fd);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsReadDir(s32 fd,sysFSDirent *entry,u64 *read)
{
	lv2syscall3(806,fd,(u64)(uintptr_t)entry,(u64)(uintptr_t)read);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsCloseDir(s32 fd)
{
	lv2syscall1(807,fd);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsMkdir(const char *path,s32 mode)
{
	lv2syscall2(811,(u64)(uintptr_t)path,mode);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsRename(const char *path,const char *newpath)
{
	lv2syscall2(812,(u64)(uintptr_t)path,(u64)(uintptr_t)newpath);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsRmdir(const char *path)
{
	lv2syscall1(813,(u64)(uintptr_t)path);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsLSeek64(s32 fd,u64 offset,s32 dir,u64 *pos)
{
	lv2syscall4(818,fd,offset,dir,(u64)(uintptr_t)pos);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsUnlink(const char *path)
{
	lv2syscall1(814,(u64)(uintptr_t)path);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsFsync(s32 fd)
{
	lv2syscall1(820,fd);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsTruncate(const char *path,u64 size)
{
	lv2syscall2(831,(u64)(uintptr_t)path,size);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsFtruncate(s32 fd,u64 size)
{
	lv2syscall2(832,fd,size);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsUtime(const char *path,const sysFSUtimbuf *times)
{
	lv2syscall2(815,(u64)(uintptr_t)path,(u64)(uintptr_t)times);
	return_to_user_prog(s32);
}

LV2_SYSCALL sysLv2FsLink(const char *oldpath,const char *newpath)
{
	lv2syscall2(810,(u64)(uintptr_t)oldpath,(u64)(uintptr_t)newpath);
	return_to_user_prog(s32);
}

/* Change a path's owner and group.  uid/gid are Lv-2 ids, not POSIX ones. */
LV2_SYSCALL sysLv2FsChown(const char *path,s32 uid,s32 gid)
{
	lv2syscall3(835,(u64)(uintptr_t)path,(u64)uid,(u64)gid);
	return_to_user_prog(s32);
}

/*
 * Mount a device.  `writeProt' non-zero mounts read-only.
 *
 * The syscall takes eight arguments; the four we have no names for are
 * passed as zero -- guessing at their meaning would be worse than leaving
 * them documented as unknown.
 */
LV2_SYSCALL sysLv2FsMount(const char *deviceName,const char *deviceFileSystem,const char *devicePath,s32 writeProt)
{
	lv2syscall8(837,(u64)(uintptr_t)deviceName,(u64)(uintptr_t)deviceFileSystem,(u64)(uintptr_t)devicePath,0,(u64)writeProt,0,0,0);
	return_to_user_prog(s32);
}

/* Unmount whatever is mounted at `devicePath'. */
LV2_SYSCALL sysLv2FsUnmount(const char *devicePath)
{
	lv2syscall3(838,(u64)(uintptr_t)devicePath,0,0);
	return_to_user_prog(s32);
}

#ifdef __cplusplus
	}
#endif

#endif
