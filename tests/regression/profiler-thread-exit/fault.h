/* Test-only interposition in the candidate pthread.c translation unit. */
#include <sys/mutex.h>
extern s32 profiler_exit_test_mutex_lock(sys_mutex_t mutex, u64 timeout_usec);
#define sysMutexLock profiler_exit_test_mutex_lock
