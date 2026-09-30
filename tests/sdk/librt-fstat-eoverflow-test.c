/* librt-fstat-eoverflow-test: host driver for librt-fstat-eoverflow-test.sh.
 *
 * Drives the *real* runtime/lv2/librt/fstat.c (pulled in with
 * #include ST_size_SOURCE, like the librt-heap test) under a fixture
 * that pins off_t to 32 bits (the PPU ILP32 ABI), so the POSIX
 * overflow a 32-bit signed off_t cannot hold is reproduced on any
 * host compiler - 32-bit or 64-bit.
 *
 * Both entry points are covered:
 *   __librt_fstat_r  (fd path)
 *   __librt_stat_r   (path path - goes through __librt_resolve_path first)
 *
 * Cases (in order):
 *   - 3 GiB file: MUST return -1 + EOVERFLOW under the fixed code;
 *     the unfixed code silently truncates the 64-bit size into a 32-bit
 *     signed off_t (so st_size becomes a negative or low-bits value),
 *     and returns 0.  The test fails the unfixed code and passes the
 *     fixed one.
 *   - INT32_MAX (2 GiB - 1): MUST succeed and return st_size verbatim,
 *     because it is the largest value expressible in a 32-bit signed
 *     off_t.
 *   - a small (1 MiB) file: positive control; must succeed.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "eofix-host.h"

/* ---- mock LV2 boundary: a single "file size" the fake syscalls return. ---- */
static uint64_t g_file_size = 0;
static int g_lret = 0;   /* LV2 return: 0 = success, nonzero = error */

int
sysLv2FsFStat(int fd, sysFSStat *sb)
{
	(void)fd;
	if (sb)
	{
		memset(sb, 0, sizeof(*sb));
		sb->st_mode = 0100000;
		sb->st_size = g_file_size;
	}
	return g_lret;
}

int
sysLv2FsStat(const char *path, sysFSStat *sb)
{
	(void)path;
	if (sb)
	{
		memset(sb, 0, sizeof(*sb));
		sb->st_mode = 0100000;
		sb->st_size = g_file_size;
	}
	return g_lret;
}

/* Path resolver: identity stub.  The production code under test calls
 * __librt_resolve_path before sysLv2FsStat; the EOVERFLOW logic is in
 * fstat.c and not the resolver, so the resolver just hands the path
 * back to the fake syscall. */
const char *
__librt_resolve_path(struct _reent *r, const char *path, char buf[PATH_MAX])
{
	(void)r;
	(void)buf;
	return path;
}

/* errno mapping: for this test we only exercise the EOVERFLOW path, so
 * any negative LV2 return maps to EOVERFLOW in r->_errno and -1 is the
 * caller-visible result (mirrors the production lv2errno.c contract). */
int
lv2errno_r(struct _reent *r, s32 error)
{
	if (error >= 0)
		return error;
	r->_errno = EOVERFLOW;
	return -1;
}

/* Pull in the *real* fstat.c as source (path from -DST_size_SOURCE).
   Neutralize the PS3 constructor attribute and any PPC inline-asm in
   case they leak into the host build, per the librt test convention. */
#define constructor(priority)
#define asm(...)
#include ST_size_SOURCE
#undef asm
#undef constructor

/* ---- test driver ---- */
static int failures;

static int
drive(uint64_t size_in, int *r_out, int *err_out,
      struct stat *st_out, const char *path)
{
	static struct _reent re = {0};
	int r;

	g_file_size = size_in;
	g_lret = 0;
	re._errno = 0;
	memset(st_out, 0, sizeof(*st_out));

	if (path == NULL)
		r = __librt_fstat_r(&re, 3, st_out);
	else
		r = __librt_stat_r(&re, path, st_out);

	*r_out = r;
	*err_out = (r < 0) ? re._errno : 0;
	return r;
}

static void
expect_overflow(const char *label, int rc, int errno_actual)
{
	if (rc == -1 && errno_actual == EOVERFLOW)
		printf("PASS %s (rc=-1 errno=%d)\n", label, errno_actual);
	else
	{
		printf("FAIL %s (rc=%d errno=%d)  [want rc=-1 errno=%d/EOVERFLOW]\n",
		       label, rc, errno_actual, EOVERFLOW);
		failures++;
	}
}

static void
expect_ok(const char *label, int rc, int errno_actual,
          long long got, long long want)
{
	if (rc == 0 && errno_actual == 0 && got == want)
		printf("PASS %s (rc=0 errno=0 st_size=%lld)\n", label, got);
	else
	{
		printf("FAIL %s (rc=%d errno=%d st_size=%lld want %lld)\n",
		       label, rc, errno_actual, got, want);
		failures++;
	}
}

int
main(void)
{
	struct stat st;
	int rc, err;

	/* 3 GiB file (0x300000000) - above the 2 GiB - 1 ceiling of a
	 * 32-bit signed off_t.  POSIX requires -1 + EOVERFLOW; an unfixed
	 * fstat.c silently truncates this into a negative/low-bits value
	 * and returns 0, which the test catches. */
	drive(UINT64_C(3) * UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024),
	      &rc, &err, &st, NULL);
	printf("  [fstat 3 GiB] rc=%d errno=%d st_size=%.2f GiB\n",
	      rc, err, (double)st.st_size / (1024.0 * 1024.0 * 1024.0));
	expect_overflow("fstat 3 GiB -> EOVERFLOW", rc, err);

	/* Path-taking entry: same overflow, but through __librt_resolve_path. */
	drive(UINT64_C(3) * UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024),
	      &rc, &err, &st, "/dev_hdd0/data/file");
	expect_overflow("stat 3 GiB -> EOVERFLOW", rc, err);

	/* Boundary: INT32_MAX (2 GiB - 1) is the largest value expressible
	 * in a 32-bit signed off_t; it must succeed and be copied verbatim. */
	drive(INT32_MAX, &rc, &err, &st, NULL);
	expect_ok("fstat INT32_MAX -> ok (largest representable)",
	          rc, err,
	          (long long)st.st_size, (long long)INT32_MAX);

	/* Positive control: 1 MiB file, must succeed. */
	drive(UINT64_C(1024) * UINT64_C(1024), &rc, &err, &st, NULL);
	expect_ok("fstat 1 MiB -> ok", rc, err,
	          (long long)st.st_size,
	          (long long)(UINT64_C(1024) * 1024));

	return failures;
}
