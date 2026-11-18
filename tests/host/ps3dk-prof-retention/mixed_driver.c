/* Test-only private API surface. The runtime remains the completion authority.
 * Default build rejects until the parent observes RED. No shared PAL edits. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "roll_retention_inspect.h"

static unsigned fault_hit, fault_seen, publication_count, lock_violations, early_prune;
static size_t largest_allocation;

#ifdef MIXED_RETENTION_IMPLEMENTED
typedef struct { int fd; unsigned role; } tracked_fd;
static tracked_fd tracked[128];
static unsigned tracked_count, lock_owned;
static const char *role_names[] = {"other", "capture", "index", "temp", "lock"};
static unsigned path_role(const char *path)
{
    size_t n = strlen(path);
    if (n >= 5 && !strcmp(path + n - 5, ".lock")) return 4;
    if (n >= 4 && !strcmp(path + n - 4, ".tmp")) return 3;
    if (n >= 6 && !strcmp(path + n - 6, ".index")) return 2;
    if ((n >= 5 && !strcmp(path + n - 5, ".prof")) || (n >= 6 && !strcmp(path + n - 6, ".profr"))) return 1;
    return 0;
}
static unsigned fd_role(int fd)
{
    unsigned i;
    for (i = 0; i < tracked_count; ++i) if (tracked[i].fd == fd) return tracked[i].role;
    return 0;
}
static int injected(const char *operation, unsigned role)
{
    const char *spec = getenv("PR_MIX_FAULT"); char wanted[80]; unsigned nth;
    int match = 0;
    if (spec && sscanf(spec, "%79[^:]:%u", wanted, &nth) == 2) {
        char label[80];
        (void)snprintf(label, sizeof label, "%s-%s", operation, role_names[role]);
        if (!strcmp(label, wanted) && ++fault_seen == nth) { ++fault_hit; match = 1; }
    }
    fprintf(stderr, "MIX_IO %s %s %s\n", operation, role_names[role], match ? "FAULT" : "ok");
    if (match) errno = EIO;
    return match;
}
static int mixed_open(const char *path, int flags, ...)
{
    int fd; unsigned role = path_role(path); mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }
    if (role == 2 && !lock_owned) ++lock_violations;
    fd = open(path, flags, mode);
    if (fd >= 0) {
        if (tracked_count == sizeof tracked / sizeof tracked[0]) { (void)close(fd); errno = EMFILE; return -1; }
        tracked[tracked_count++] = (tracked_fd){fd, role};
        if (role == 4 && (flags & O_EXCL)) lock_owned = 1;
    }
    return fd;
}
static ssize_t mixed_write(int fd, const void *data, size_t bytes)
{ return injected("write", fd_role(fd)) ? -1 : write(fd, data, bytes); }
static int mixed_sync(int fd)
{ return injected("sync", fd_role(fd)) ? -1 : fsync(fd); }
static int mixed_close(int fd)
{
    unsigned role = fd_role(fd), i; int rc = close(fd), failed;
    for (i = 0; i < tracked_count; ++i) if (tracked[i].fd == fd) { tracked[i] = tracked[--tracked_count]; break; }
    failed = injected("close", role); /* Model failure after the descriptor is closed. */
    return failed ? -1 : rc;
}
static int mixed_rename(const char *from, const char *to)
{
    unsigned role = path_role(to); int rc;
    if (role == 2 && !lock_owned) ++lock_violations;
    if (injected("rename", role)) return -1;
    rc = rename(from, to);
    if (!rc && role == 2) ++publication_count;
    return rc;
}
static int mixed_unlink(const char *path)
{
    unsigned role = path_role(path); int rc;
    if (role == 1) {
        if (!lock_owned) ++lock_violations;
        if (!publication_count) ++early_prune;
    }
    if (injected("unlink", role)) return -1;
    rc = unlink(path);
    if (!rc && role == 4) lock_owned = 0;
    return rc;
}
static void *mixed_malloc(size_t bytes)
{ if (bytes > largest_allocation) largest_allocation = bytes; return malloc(bytes); }
static void *mixed_calloc(size_t count, size_t bytes)
{
    if (bytes && count > SIZE_MAX / bytes) { errno = ENOMEM; return NULL; }
    if (count * bytes > largest_allocation) largest_allocation = count * bytes;
    return calloc(count, bytes);
}
static void *mixed_realloc(void *ptr, size_t bytes)
{ if (bytes > largest_allocation) largest_allocation = bytes; return realloc(ptr, bytes); }
#define open mixed_open
#define write mixed_write
#define fsync mixed_sync
#define close mixed_close
#define rename mixed_rename
#define unlink mixed_unlink
#define malloc mixed_malloc
#define calloc mixed_calloc
#define realloc mixed_realloc
#include "capture_retention.h"
#undef open
#undef write
#undef fsync
#undef close
#undef rename
#undef unlink
#undef malloc
#undef calloc
#undef realloc
#else
static int ps3dk_prof__pal_retain_complete_kind(const char *path, const char *app,
        uint64_t session, unsigned keep, uint32_t format, const pfi_limits *limits)
{
    (void)path; (void)app; (void)session; (void)keep; (void)format; (void)limits;
    return -100; /* Test-only missing implementation, never runtime-linked. */
}
static int ps3dk_prof__pal_retain_complete(const char *path, const char *app, uint64_t session, unsigned keep)
{ (void)path; (void)app; (void)session; (void)keep; return -100; }
#endif

int main(int argc, char **argv)
{
    pfi_limits limits; int rc;
    if (argc != 11) return 2;
    limits = (pfi_limits){strtoull(argv[7], NULL, 10), (uint32_t)strtoul(argv[8], NULL, 10),
                         (uint32_t)strtoul(argv[9], NULL, 10), (uint32_t)strtoul(argv[10], NULL, 10)};
#ifdef MIXED_RETENTION_IMPLEMENTED
    /* Some allocation functions need not occur in a particular implementation. */
    (void)mixed_malloc; (void)mixed_calloc; (void)mixed_realloc;
#endif
    if (!strcmp(argv[1], "legacy"))
        rc = ps3dk_prof__pal_retain_complete(argv[2], argv[3], strtoull(argv[4], NULL, 16), (unsigned)strtoul(argv[5], NULL, 10));
    else if (!strcmp(argv[1], "kind") || !strcmp(argv[1], "kind-no-budget"))
        rc = ps3dk_prof__pal_retain_complete_kind(argv[2], argv[3], strtoull(argv[4], NULL, 16),
             (unsigned)strtoul(argv[5], NULL, 10), (uint32_t)strtoul(argv[6], NULL, 10),
             !strcmp(argv[1], "kind") ? &limits : NULL);
    else return 2;
    printf("{\"rc\":%d,\"fault_hit\":%u,\"fault_seen\":%u,\"publications\":%u,"
           "\"lock_violations\":%u,\"early_prune\":%u,\"largest_allocation\":%llu}\n",
           rc, fault_hit, fault_seen, publication_count, lock_violations, early_prune,
           (unsigned long long)largest_allocation);
    return 0;
}
