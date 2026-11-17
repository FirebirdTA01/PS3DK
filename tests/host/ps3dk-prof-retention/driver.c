/* Standalone exercise of the same implementation included by both PALs. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#ifdef RETENTION_RED
static int pr_complete(const char *dir, const char *app, uint64_t session,
                       unsigned keep) {
    (void)dir; (void)app; (void)session; (void)keep; return -1;
}
#else
static int io_count;
static int fail_io(void) {
    const char *s=getenv("PR_FAULT");
    if(s && atoi(s)==++io_count) {errno=EIO;return 1;}
    return 0;
}
static ssize_t fault_write(int f,const void *b,size_t n) {return fail_io()?-1:write(f,b,n);}
static int fault_sync(int f) {return fail_io()?-1:fsync(f);}
static int fault_rename(const char *a,const char *b) {return fail_io()?-1:rename(a,b);}
static int fault_unlink(const char *a) {return fail_io()?-1:unlink(a);}
#define write fault_write
#define fsync fault_sync
#define rename fault_rename
#define unlink fault_unlink
#include "capture_retention.h"
#endif
int main(int argc, char **argv) {
    if (argc != 5) return 3;
    int rc=pr_complete(argv[1],argv[2],strtoull(argv[3],0,16),
                       (unsigned)strtoul(argv[4],0,10));
    printf("retention=%d\n",rc);
    return rc ? 1 : 0;
}
