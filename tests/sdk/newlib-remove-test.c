#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <reent.h>
#include <sys/syscalls.h>

struct __syscalls_t __syscalls;
static int failures, unlink_calls, rmdir_calls, unlink_error, rmdir_error;
static struct _reent *expected_reent;
static const char *expected_path;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
int _unlink_r(struct _reent *r, const char *path)
{
    ++unlink_calls;
    CHECK(r == expected_reent && path == expected_path);
    if (!unlink_error) return 0;
    r->_errno = unlink_error;
    return -1;
}
static int remove_directory(struct _reent *r, const char *path)
{
    ++rmdir_calls;
    CHECK(r == expected_reent && path == expected_path);
    if (!rmdir_error) return 0;
    r->_errno = rmdir_error;
    return -1;
}
static void row(const char *label, int ue, int re, int hook, int result,
                int final_errno, int calls)
{
    struct _reent alternate = { 0 };
    expected_reent = &alternate;
    expected_path = label;
    unlink_calls = rmdir_calls = 0;
    unlink_error = ue;
    rmdir_error = re;
    __syscalls.rmdir_r = hook ? remove_directory : 0;
    CHECK(_remove_r(&alternate, label) == result);
    CHECK(unlink_calls == 1 && rmdir_calls == calls);
    if (result == -1) CHECK(alternate._errno == final_errno);
}
int main(void)
{
    row("file", 0, 0, 1, 0, 0, 0);
    row("empty-directory", EISDIR, 0, 1, 0, 0, 1);
    row("nonempty-directory", EISDIR, ENOTEMPTY, 1, -1, ENOTEMPTY, 1);
    row("missing", ENOENT, 0, 1, -1, ENOENT, 0);
    row("denied", EACCES, 0, 1, -1, EACCES, 0);
    row("missing-hook", EISDIR, 0, 0, -1, ENOSYS, 0);
    printf("remove-test: %d failures\n", failures);
    return failures ? 1 : 0;
}
