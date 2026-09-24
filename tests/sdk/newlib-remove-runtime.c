/* Direct C regression for t_0d5902da; creates and removes only its own tree. */
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/process.h>
#include <sys/sys_time.h>

SYS_PROCESS_PARAM(1001, 0x10000);
static int failures;
static void check(int ok, const char *what)
{
    printf("%s remove: %s (errno=%d)\n", ok ? "PASS" : "FAIL", what, errno);
    if (!ok) ++failures;
}
int main(void)
{
    char root[160], child[192], file[192], missing[192];
    snprintf(root, sizeof root, "/dev_hdd0/tmp/ps3dk-remove-%lu-%llu",
             (unsigned long)sysProcessGetPid(),
             (unsigned long long)sys_time_get_system_time());
    snprintf(child, sizeof child, "%s/empty", root);
    snprintf(file, sizeof file, "%s/file", root);
    snprintf(missing, sizeof missing, "%s/missing", root);
    if (mkdir(root, 0700) != 0) { check(0, "create fresh root"); return 1; }
    check(mkdir(child, 0700) == 0, "create empty child");
    FILE *fp = fopen(file, "wb");
    check(fp != NULL, "create file");
    if (fp) check(fclose(fp) == 0, "close file");
    errno = 0;
    const int full_rc = remove(root);
    check(full_rc == -1 && (errno == ENOTEMPTY || errno == EEXIST),
          "nonempty directory preserves error");
    check(remove(child) == 0, "remove empty directory");
    check(remove(file) == 0, "remove regular file");
    errno = 0;
    const int missing_rc = remove(missing);
    check(missing_rc == -1 && errno == ENOENT, "missing path preserves ENOENT");
    check(remove(root) == 0, "remove owned root");
    printf("REMOVE_RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}
