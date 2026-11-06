/* ps3dk_prof_live_stats from C and C++, in an enabled and an off build (live-api-test.py compiles this file four
 * ways).  Enabled: the type and the function are usable as declared.  Off: the macro takes its argument
 * unevaluated, is ENOTSUP, and references nothing (the off executables link with no profiler sources).
 * usage: live-stats-header  -> prints PASS and exits 0 */
#include <ps3dk/prof.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
static_assert(sizeof(ps3dk_prof_live_stats) == 8 + 14 * 8, "fixed width");
static_assert(offsetof(ps3dk_prof_live_stats, connections) == 8, "counters follow version and reserved");
#else
_Static_assert(sizeof(ps3dk_prof_live_stats) == 8 + 14 * 8, "fixed width");
_Static_assert(offsetof(ps3dk_prof_live_stats, connections) == 8, "counters follow version and reserved");
#endif

int main(void)
{
    ps3dk_prof_live_stats s;
    memset(&s, 0xa5, sizeof s);
#if defined(PS3DK_PROF) && PS3DK_PROF
    int (*fn)(ps3dk_prof_live_stats *) = ps3dk_prof_live_get_stats;
    if (fn(NULL) != PS3DK_PROF_EINVAL || PS3DK_PROF_LIVE_GET_STATS(&s) != PS3DK_PROF_ENOTINIT) {
        puts("FAIL enabled");
        return 1;
    }
#else
    int calls = 0;
    if (PS3DK_PROF_LIVE_GET_STATS((++calls, &s)) != PS3DK_PROF_ENOTSUP || calls != 0) {
        puts("FAIL off");
        return 1;
    }
#endif
    unsigned char poison[sizeof s];
    memset(poison, 0xa5, sizeof poison);
    if (memcmp(&s, poison, sizeof s) != 0) {
        puts("FAIL output changed");
        return 1;
    }
    puts("PASS");
    return 0;
}
