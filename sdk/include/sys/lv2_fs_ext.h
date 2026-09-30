/* sys/lv2_fs_ext.h - retired, kept for source compatibility.
 *
 * The lv2fs chown/mount/unmount wrappers (sysLv2FsChown, sysLv2FsMount,
 * sysLv2FsUnmount) now live in <sys/file.h>, where they belong.  This
 * header shipped since v0.13 and user code may still include it, so it is
 * kept as a forwarder to <sys/file.h> only.
 */
#ifndef __PS3DK_SYS_LV2_FS_EXT_H__
#define __PS3DK_SYS_LV2_FS_EXT_H__

#include <sys/file.h>

#endif /* __PS3DK_SYS_LV2_FS_EXT_H__ */
