#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <unistd.h>

static int failures;

static void check_true(const char *label, int ok)
{
    if (ok) {
        printf("PASS %s\n", label);
    } else {
        printf("FAIL %s\n", label);
        failures++;
    }
}

static void check_int(const char *label, long actual, long expected)
{
    if (actual == expected) {
        printf("PASS %s = %ld\n", label, actual);
    } else {
        printf("FAIL %s got %ld expected %ld errno=%d\n",
               label, actual, expected, errno);
        failures++;
    }
}

static void test_clock(void)
{
    struct tms times_buf;

    check_int("sleep zero", sleep(0), 0);
    clock_t ticks = times(&times_buf);
    printf("INFO times ticks=%ld\n", (long)ticks);
}

static int write_file(const char *path, const char *text, int flags, mode_t mode)
{
    int fd = open(path, flags, mode);
    if (fd < 0) {
        return -1;
    }
    ssize_t wrote = write(fd, text, strlen(text));
    int saved_errno = errno;
    int close_rc = close(fd);
    if (wrote != (ssize_t)strlen(text)) {
        errno = saved_errno;
        return -1;
    }
    return close_rc;
}

static void test_files(void)
{
    const char *base = "/dev_hdd0/tmp/ps3tc_smoke_librt_posix";
    const char *dir = "/dev_hdd0/tmp/ps3tc_smoke_librt_posix/dir";
    const char *file = "/dev_hdd0/tmp/ps3tc_smoke_librt_posix/file.txt";
    struct stat st;

    mkdir("/dev_hdd0/tmp", 0777);
    mkdir(base, 0777);
    mkdir(dir, 0777);
    unlink(file);

    check_int("open create write", write_file(file, "one", O_CREAT | O_WRONLY | O_TRUNC, 0644), 0);
    check_int("stat created file", stat(file, &st), 0);
    printf("INFO created mode=0%lo\n", (unsigned long)(st.st_mode & 0777));

    errno = 0;
    int excl_fd = open(file, O_CREAT | O_EXCL | O_WRONLY, 0644);
    check_true("open O_EXCL existing fails", excl_fd < 0 && errno == EEXIST);
    if (excl_fd >= 0) {
        close(excl_fd);
    }
}

static struct dirent *read_one(DIR *d, char *out, size_t out_size)
{
    struct dirent *ent = readdir(d);
    if (ent && out_size > 0) {
        snprintf(out, out_size, "%s", ent->d_name);
    }
    return ent;
}

static void test_dirent(void)
{
    const char *dir = "/dev_hdd0/tmp/ps3tc_smoke_librt_posix/dir";
    char path[256];
    char first[256] = {0};
    char second[256] = {0};
    char third[256] = {0};

    snprintf(path, sizeof(path), "%s/a.txt", dir);
    write_file(path, "a", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    snprintf(path, sizeof(path), "%s/b.txt", dir);
    write_file(path, "b", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    snprintf(path, sizeof(path), "%s/c.txt", dir);
    write_file(path, "c", O_CREAT | O_WRONLY | O_TRUNC, 0644);

    DIR *d = opendir(dir);
    check_true("opendir absolute", d != NULL);
    if (!d) {
        return;
    }

    check_true("readdir first", read_one(d, first, sizeof(first)) != NULL);
    check_true("readdir second", read_one(d, second, sizeof(second)) != NULL);
    long pos2 = telldir(d);
    check_true("telldir after second", pos2 >= 2);
    check_true("readdir third", read_one(d, third, sizeof(third)) != NULL);
    closedir(d);
}

static void test_sbrk(void)
{
    errno = 0;
    void *small = malloc(1024 * 1024);
    check_true("malloc 1MiB", small != NULL);
    free(small);
}

int main(void)
{
    printf("librt-posix smoke start\n");

    test_clock();
    test_files();
    test_dirent();
    test_sbrk();

    printf("librt-posix failures=%d\n", failures);
    if (failures == 0) {
        printf("librt-posix: PASS\n");
        return 0;
    }

    printf("librt-posix: FAIL\n");
    return 1;
}
