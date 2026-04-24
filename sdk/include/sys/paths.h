/* sys/paths.h - PS3 filesystem mount-point path constants.
 *
 * Clean-room constants matching the reference SDK surface.  Sample
 * code and tools use these to address the various storage roots
 * without hard-coding the /dev_* or /host_root strings.
 */
#ifndef __PS3DK_SYS_PATHS_H__
#define __PS3DK_SYS_PATHS_H__

/* Default mount point (app's own content directory). */
#define SYS_APP_HOME    "/app_home"

/* Mount point for a file system on a remote host PC (deci4/fs-host). */
#define SYS_HOST_ROOT   "/host_root"

/* Built-in HDD mount points.  hdd0 is the user-visible partition;
 * hdd1 is reserved for system use. */
#define SYS_DEV_HDD0    "/dev_hdd0"
#define SYS_DEV_HDD1    "/dev_hdd1"

/* Memory Stick slot. */
#define SYS_DEV_MS      "/dev_ms"

/* BD/DVD drive. */
#define SYS_DEV_BDVD    "/dev_bdvd"

/* USB slots (used for dev utilities such as core-dump target). */
#define SYS_DEV_USB     "/dev_usb"

#endif /* __PS3DK_SYS_PATHS_H__ */
