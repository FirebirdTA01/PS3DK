/* C++ / C-linkage check for the two sched prototypes declared by
 * sdk/include/sched.h (over newlib's <sched.h>).
 *
 * This translation unit deliberately includes <pthread.h> BEFORE
 * <sched.h> -- the reverse order to main.c -- to prove that the wrapper
 * declarations work from C++ under both include orders and that they
 * carry C linkage (so the C caller in main.c and this C++ caller agree on
 * the symbol at link time, under a single librt definition).
 *
 * The two references are the only SCHED-related calls this TU makes; we
 * expect both to be declared (C linkage) and, for the only policy the
 * shim supports, to yield the portable "default" value 0. */
#include <pthread.h>
#include <sched.h>

extern "C" int sched_cxx_check(void)
{
    return (sched_get_priority_min(SCHED_OTHER) == 0 &&
            sched_get_priority_max(SCHED_OTHER) == 0) ? 0 : -1;
}
