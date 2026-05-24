#include <_ansi.h>
#include <sys/reent.h>
#include <sys/lv2errno.h>
#include <sys/file.h>

extern mode_t g_umask;

int
__librt_open_r(struct _reent *r, const char *file, int flags, int mode)
{
	s32 fd;
	s32 ret = sysLv2FsOpen(file, flags, &fd, mode & ~g_umask, NULL, 0);
	return lv2errno_r(r, ret) < 0 ? -1 : fd;
}
