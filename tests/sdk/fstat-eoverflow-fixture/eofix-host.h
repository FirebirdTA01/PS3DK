/* fstat-eoverflow-fixture: PPU-shaped stand-ins for runtime/lv2/librt/fstat.c.
 *
 * One canonical stub for the system headers fstat.c includes.  The
 * one-line proxy headers in this fixture directory (sys/stat.h,
 * sys/reent.h, ...) all resolve to this file, so whichever system
 * header the source pulls in, it reaches these definitions.
 *
 * What must be preserved from the PPU target:
 *   - off_t is 32-bit signed (newlib typedef long _off_t under the
 *     ILP32 ABI).  On a 64-bit host long is 8 bytes, so this fixture
 *     redefines off_t to int32_t and _Static_assert's that.
 *   - LV2's sysFSStat.st_size is 64-bit, as in the real LV2 API.
 *
 * The comment style avoids a slash immediately followed by an asterisk
 * inside block comments so -Wcomment stays quiet under -Werror.
 */
#ifndef PS3TC_ST_SIZE_EOFIXTURE_V2_H
#define PS3TC_ST_SIZE_EOFIXTURE_V2_H

#include <stdint.h>

/* forward decl; full definition at the bottom so the prototypes below are
 * well-formed before the driver's #include of the real fstat.c. */
struct _reent;

typedef int32_t  s32;
typedef uint32_t u32;
typedef uint64_t u64;

/* 32-bit signed off_t, as on the PPU ILP32 target.  (No host header in
 * this fixture's include chain should declare a conflicting off_t; if one
 * does, the guard below would not have caught it but the _Static_assert
 * would fail.) */
typedef int32_t off_t;
_Static_assert(sizeof(off_t) == 4,
               "st_size-eoverflow fixture assumes a 32-bit signed off_t, "
               "as on the PPU ILP32 target");

#ifndef EOVERFLOW
#define EOVERFLOW 75        /* POSIX: value too large for the data type */
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* newlib/PPU struct stat, st_size is off_t (32-bit signed). */
struct stat {
	unsigned int st_mode;
	off_t        st_uid;
	off_t        st_gid;
	struct { off_t tv_sec; long tv_nsec; } st_atim;
	struct { off_t tv_sec; long tv_nsec; } st_mtim;
	struct { off_t tv_sec; long tv_nsec; } st_ctim;
	off_t        st_size;
	off_t        st_blksize;
};

/* LV2 kernel stat struct (the fields fstat.c reads); st_size is u64. */
typedef struct _sys_fs_stat {
	s32  st_mode;
	u32  st_uid;
	u32  st_gid;
	s32  st_atime;
	s32  st_mtime;
	s32  st_ctime;
	u64  st_ino;
	u64  st_dev;
	u64  st_size;        /* 64-bit: the value the test overflows off_t with */
	u64  st_blksize;
} sysFSStat;

/* LV2 syscall boundary, provided by the test driver. */
int sysLv2FsFStat(int fd, sysFSStat *sb);
int sysLv2FsStat(const char *path, sysFSStat *sb);

/* Path resolver (v0.18.0+), provided by the test driver; returns path
 * unchanged so the production code under test sees a valid path and the
 * EOVERFLOW logic runs on the real code path (not a mocked resolver). */
const char *__librt_resolve_path(struct _reent *r, const char *path,
                                 char buf[PATH_MAX]);

/* reent (driver fills this in) + errno mapping. */
struct _reent { int _errno; };
int lv2errno_r(struct _reent *r, s32 error);

#endif /* PS3TC_ST_SIZE_EOFIXTURE_V2_H */
