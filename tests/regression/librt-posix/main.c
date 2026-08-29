#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <unistd.h>
#include <utime.h>

#ifdef PS3TC_REGRESSION_HOST_STUB
#define SOCKET_FD_MASK 0x40000000U
#endif

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

static long timeval_delta_us(const struct timeval *a, const struct timeval *b)
{
    long sec = (long)(b->tv_sec - a->tv_sec);
    long usec = (long)(b->tv_usec - a->tv_usec);
    return sec * 1000000L + usec;
}

static void test_time(void)
{
    struct timeval a;
    struct timeval b;
    struct timeval set_to;
    struct tms times_buf;

    memset(&a, 0xA5, sizeof(a));
    memset(&b, 0x5A, sizeof(b));

    errno = 0;
    check_int("gettimeofday first", gettimeofday(&a, NULL), 0);
    usleep(100000);
    check_int("gettimeofday second", gettimeofday(&b, NULL), 0);

    long delta = timeval_delta_us(&a, &b);
    printf("INFO gettimeofday delta_us=%ld a_usec=%ld b_usec=%ld\n",
           delta, (long)a.tv_usec, (long)b.tv_usec);
    check_true("gettimeofday delta near 100ms", delta >= 50000 && delta <= 500000);
    check_true("gettimeofday tv_usec populated", a.tv_usec >= 0 && a.tv_usec < 1000000 &&
                                               b.tv_usec >= 0 && b.tv_usec < 1000000);

    set_to.tv_sec = a.tv_sec;
    set_to.tv_usec = a.tv_usec;
    errno = 0;
    int set_rc = settimeofday(&set_to, NULL);
    printf("INFO settimeofday ret=%d errno=%d\n", set_rc, errno);

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
    const char *base = "/dev_hdd0/tmp/ps3tc_regression_librt_posix";
    const char *dir = "/dev_hdd0/tmp/ps3tc_regression_librt_posix/dir";
    const char *file = "/dev_hdd0/tmp/ps3tc_regression_librt_posix/file.txt";
    char cwd[256];
    struct stat st;
    struct utimbuf ut;

    mkdir("/dev_hdd0/tmp", 0777);
    mkdir(base, 0777);
    mkdir(dir, 0777);
    unlink(file);

    mode_t old_umask = umask(0177777);
    mode_t masked = umask(old_umask);
    printf("INFO umask masked=0%lo old=0%lo\n", (unsigned long)masked, (unsigned long)old_umask);
    check_true("umask masks permission bits", (masked & ~0777) == 0);

    check_int("open create write", write_file(file, "one", O_CREAT | O_WRONLY | O_TRUNC, 0644), 0);
    check_int("stat created file", stat(file, &st), 0);
    printf("INFO created mode=0%lo\n", (unsigned long)(st.st_mode & 0777));

    ut.actime = 11;
    ut.modtime = 22;
    check_int("utime explicit", utime(file, &ut), 0);
    check_int("utime now", utime(file, NULL), 0);

    errno = 0;
    int excl_fd = open(file, O_CREAT | O_EXCL | O_WRONLY, 0644);
    check_true("open O_EXCL existing fails", excl_fd < 0 && errno == EEXIST);
    if (excl_fd >= 0) {
        close(excl_fd);
    }

    errno = 0;
    check_true("chdir nonexistent fails", chdir("/dev_hdd0/tmp/ps3tc_regression_librt_posix/nope") < 0);
    check_int("chdir normalized path", chdir("/dev_hdd0/tmp/../tmp"), 0);
    check_true("getcwd after normalize", getcwd(cwd, sizeof(cwd)) != NULL &&
                                      strcmp(cwd, "/dev_hdd0/tmp") == 0);
    check_int("chdir root", chdir("/"), 0);
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
    const char *dir = "/dev_hdd0/tmp/ps3tc_regression_librt_posix/dir";
    char path[256];
    char first[256] = {0};
    char second[256] = {0};
    char third[256] = {0};
    char rewind_first[256] = {0};
    char seek_third[256] = {0};

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

    rewinddir(d);
    check_true("rewinddir returns first entry", read_one(d, rewind_first, sizeof(rewind_first)) != NULL &&
                                           strcmp(first, rewind_first) == 0);

    seekdir(d, pos2);
    check_true("seekdir returns third entry", read_one(d, seek_third, sizeof(seek_third)) != NULL &&
                                         strcmp(third, seek_third) == 0);
    closedir(d);

    check_int("chdir dir", chdir(dir), 0);
    d = opendir(".");
    check_true("opendir relative", d != NULL);
    if (d) {
        rewinddir(d);
        closedir(d);
    }
    check_int("chdir root after dirent", chdir("/"), 0);
}

static void test_sbrk(void)
{
    errno = 0;
    void *small = malloc(1024 * 1024);
    check_true("malloc 1MiB", small != NULL);
    free(small);

    errno = 0;
    void *large = malloc(70 * 1024 * 1024);
    printf("INFO malloc 70MiB ptr=%p errno=%d\n", large, errno);
    /* Asserts the CURRENT fixed 64 MiB sbrk arena (runtime/lv2/librt/sbrk.c).
       If the arena becomes on-demand (open director decision), this expectation
       inverts: update it deliberately, do not "fix" it. */
    check_true("malloc 70MiB reports ENOMEM", large == NULL && errno == ENOMEM);
    free(large);
}

static void test_socket(void)
{
    errno = 0;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    printf("INFO socket fd=%d errno=%d\n", fd, errno);
    check_true("socket returns real fd", fd >= 0 && errno != ENOSYS);
    check_true("socket fd has mask", fd < 0 || ((unsigned int)fd & SOCKET_FD_MASK) == SOCKET_FD_MASK);

    if (fd >= 0) {
        struct sockaddr_in local_addr;
        struct sockaddr_in peer_addr;
        struct sockaddr_in addr;
        socklen_t local_len = sizeof(local_addr);
        socklen_t peer_len = sizeof(peer_addr);
        fd_set writefds;
        struct timeval tv;
        int lv2_fd = fd & ~((int)SOCKET_FD_MASK);

        memset(&local_addr, 0, sizeof(local_addr));
        errno = 0;
        int local_rc = getsockname(fd, (struct sockaddr *)&local_addr, &local_len);
        printf("INFO getsockname rc=%d errno=%d len=%lu family=%d port=%u\n",
               local_rc, errno, (unsigned long)local_len,
               local_addr.sin_family, (unsigned int)ntohs(local_addr.sin_port));
        check_true("getsockname reaches real syscall", local_rc == 0 || errno != ENOSYS);

        memset(&peer_addr, 0, sizeof(peer_addr));
        errno = 0;
        int peer_rc = getpeername(fd, (struct sockaddr *)&peer_addr, &peer_len);
        printf("INFO getpeername rc=%d errno=%d len=%lu family=%d port=%u\n",
               peer_rc, errno, (unsigned long)peer_len,
               peer_addr.sin_family, (unsigned int)ntohs(peer_addr.sin_port));
        check_true("getpeername reaches real syscall", peer_rc == 0 || errno != ENOSYS);

        FD_ZERO(&writefds);
        if (lv2_fd >= 0 && lv2_fd < FD_SETSIZE) {
            FD_SET(lv2_fd, &writefds);
        }
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        errno = 0;
        int select_rc = select(lv2_fd + 1, NULL, &writefds, NULL, &tv);
        printf("INFO select socket rc=%d errno=%d lv2fd=%d ready=%d\n",
               select_rc, errno, lv2_fd,
               (lv2_fd >= 0 && lv2_fd < FD_SETSIZE) ? FD_ISSET(lv2_fd, &writefds) : -1);
        check_true("select socket descriptor in range", lv2_fd >= 0 && lv2_fd < FD_SETSIZE);
        check_true("select socket reaches real syscall", select_rc >= 0 || errno != ENOSYS);

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(65000);
        addr.sin_addr.s_addr = htonl(0xCB007101U);

        errno = 0;
        int conn = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        printf("INFO connect rc=%d errno=%d\n", conn, errno);
        check_true("connect reaches real syscall", conn == 0 || errno != ENOSYS);

        errno = 0;
        int close_rc = close(fd);
        printf("INFO socket close rc=%d errno=%d\n", close_rc, errno);
        check_true("socket close not unimplemented", close_rc == 0 || errno != ENOSYS);
    }
}

int main(void)
{
    printf("librt-posix regression start\n");

    test_time();
    test_files();
    test_dirent();
    test_sbrk();
    test_socket();

    printf("librt-posix failures=%d\n", failures);
    if (failures == 0) {
        printf("librt-posix: PASS\n");
        return 0;
    }

    printf("librt-posix: FAIL\n");
    return 1;
}
