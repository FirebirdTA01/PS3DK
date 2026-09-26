#!/usr/bin/env bash
# librt's relative-path resolver, compiled on the host with stub Lv-2
# headers: a relative path becomes the working directory, '/', and the path
# joined verbatim (no "." or ".." collapsing), the length limit counts the
# separator only when one is added, absolute and empty paths pass through,
# and a NULL path sets EFAULT.
#
# usage: librt-resolve-path-host-test.sh
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
cc=${CC:-cc}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/sys"

cat > "$work/sys/reent.h" <<'EOF'
#ifndef STUB_REENT_H
#define STUB_REENT_H
struct _reent { int _errno; };
#endif
EOF
cat > "$work/sys/syslimits.h" <<'EOF'
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
EOF
cat > "$work/sys/lv2errno.h" <<'EOF'
#ifndef STUB_LV2ERRNO_H
#define STUB_LV2ERRNO_H
#include <sys/reent.h>
static inline int lv2errno_r(struct _reent *r, int ret) { if (ret) { r->_errno = ret; return -1; } return 0; }
#endif
EOF
cat > "$work/sys/file.h" <<'EOF'
#ifndef STUB_FILE_H
#define STUB_FILE_H
typedef int s32;
static inline s32 sysLv2FsOpenDir(const char *path, s32 *fd) { (void)path; *fd = 3; return 0; }
static inline s32 sysLv2FsCloseDir(s32 fd) { (void)fd; return 0; }
#endif
EOF

cat > "$work/probe.c" <<'EOF'
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/reent.h>
#include "librt_path.h"

int __librt_chdir_r(struct _reent *r, const char *dirname);

static int failures;
static void check(const char *label, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", label);
    failures += !ok;
}

static char name[PATH_MAX + 8];
static const char *fill(size_t n)
{
    memset(name, 'a', n);
    name[n] = '\0';
    return name;
}

int main(void)
{
    struct _reent r = {0};
    char buf[PATH_MAX];
    const char *p;

    p = "/dev_hdd0/x";
    check("absolute passes through", __librt_resolve_path(&r, p, buf) == p);
    p = "";
    check("empty passes through", __librt_resolve_path(&r, p, buf) == p);
    r._errno = 0;
    check("NULL sets EFAULT", __librt_resolve_path(&r, NULL, buf) == NULL && r._errno == EFAULT);

    /* Working directory "/": no separator is added. */
    p = __librt_resolve_path(&r, "a/../b/./c/", buf);
    check("root join is verbatim", p && strcmp(p, "/a/../b/./c/") == 0);
    r._errno = 0;
    p = __librt_resolve_path(&r, fill(PATH_MAX - 2), buf);
    check("root: PATH_MAX-2 name fits (total PATH_MAX-1)", p && strlen(p) == PATH_MAX - 1);
    r._errno = 0;
    p = __librt_resolve_path(&r, fill(PATH_MAX - 1), buf);
    check("root: PATH_MAX-1 name is ENAMETOOLONG", !p && r._errno == ENAMETOOLONG);

    /* Working directory "/dev_hdd0/tmp" (13 chars): one separator added. */
    check("chdir", __librt_chdir_r(&r, "/dev_hdd0/tmp") == 0);
    p = __librt_resolve_path(&r, "victim/.", buf);
    check("non-root join is verbatim", p && strcmp(p, "/dev_hdd0/tmp/victim/.") == 0);
    p = __librt_resolve_path(&r, "missing/../victim", buf);
    check("missing/.. kept for the kernel", p && strcmp(p, "/dev_hdd0/tmp/missing/../victim") == 0);
    r._errno = 0;
    p = __librt_resolve_path(&r, fill(PATH_MAX - 15), buf);
    check("non-root: longest name fits", p && strlen(p) == PATH_MAX - 1);
    r._errno = 0;
    p = __librt_resolve_path(&r, fill(PATH_MAX - 14), buf);
    check("non-root: one more is ENAMETOOLONG", !p && r._errno == ENAMETOOLONG);

    printf("librt-resolve-path: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
EOF

"$cc" -std=c11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE -I"$work" -I"$root/runtime/lv2/librt" \
    "$root/runtime/lv2/librt/globfile.c" "$work/probe.c" -o "$work/probe" > "$work/cc.log" 2>&1 \
    || { cat "$work/cc.log"; echo "librt-resolve-path: FAIL (host compile)"; exit 1; }
"$work/probe"
