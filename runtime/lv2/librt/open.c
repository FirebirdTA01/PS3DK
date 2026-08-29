#include <_ansi.h>
#include <fcntl.h>
#include <sys/reent.h>
#include <sys/lv2errno.h>
#include <sys/file.h>

extern mode_t g_umask;

int
__librt_open_r(struct _reent *r, const char *file, int flags, int mode)
{
	/* Translate POSIX open(2) flag bits into the CellOS LV2
	 * sys_fs_open flag set.  The access-mode bits and SYS_O_MSELF
	 * share bit positions between the two ABIs and pass through
	 * unchanged.  The CREAT, EXCL, TRUNC, and APPEND bits sit at
	 * different bit positions in the two ABIs and must be remapped
	 * or the kernel rejects the open with EINVAL.  */
	int oflag = flags & (O_ACCMODE | SYS_O_MSELF);
	if (flags & O_CREAT)  oflag |= SYS_O_CREAT;
	if (flags & O_EXCL)   oflag |= SYS_O_EXCL;
	if (flags & O_TRUNC)  oflag |= SYS_O_TRUNC;
	if (flags & O_APPEND) oflag |= SYS_O_APPEND;
	if (flags & O_CREAT)
		mode &= ~g_umask;
	else
		mode = 0;

	s32 fd;
	s32 ret = sysLv2FsOpen(file, oflag, &fd, mode, NULL, 0);
	if (lv2errno_r(r, ret) < 0)
		return -1;

	if (flags & O_CREAT)
		sysLv2FsChmod(file, mode);

	return fd;
}
