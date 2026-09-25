#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* Lv-2 does not know the process-local cwd. A host stat accepting relative
 * paths would hide the target failure. The second build wraps that boundary. */
#ifdef MODEL_LV2_STAT
int __real_stat(const char *, struct stat *);
int __wrap_stat(const char *path, struct stat *st)
{
    if (!path || path[0] != '/') {
        errno = ENOENT;
        return -1;
    }
    return __real_stat(path, st);
}
#endif

/* Renamed at compile time so the host libc cannot satisfy the test. */
static int failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s (errno=%d)\n", __LINE__, #expr, errno); \
    ++failures; } } while (0)

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    CHECK(pathconf(argv[1], _PC_PATH_MAX) == PATH_MAX);
    CHECK(pathconf(argv[1], _PC_NAME_MAX) == NAME_MAX);
    CHECK(pathconf(".", _PC_PATH_MAX) == PATH_MAX);
    errno = 0;
    CHECK(pathconf(argv[1], -1234) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(pathconf(argv[2], _PC_PATH_MAX) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(pathconf(argv[2], _PC_NAME_MAX) == -1 && errno == ENOENT);
    errno = 0;
    CHECK(pathconf("", _PC_PATH_MAX) == -1 && errno == ENOENT);
    CHECK(chdir(argv[1]) == 0);
    CHECK(pathconf(".", _PC_PATH_MAX) == PATH_MAX);
    errno = 0;
    CHECK(pathconf("missing", _PC_PATH_MAX) == -1 && errno == ENOENT);
    if (!failures) puts("librt-pathconf: PASS (limits, relative path, lookup errors, invalid selector)");
    return failures ? 1 : 0;
}
