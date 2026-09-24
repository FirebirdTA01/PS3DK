#include <reent.h>
struct __syscalls_t { int (*rmdir_r)(struct _reent *, const char *); };
extern struct __syscalls_t __syscalls;
