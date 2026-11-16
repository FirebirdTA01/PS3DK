/* Automatic profiler lifecycle with explicit application seal.
 * PS3DK_PROF_AUTO_SEAL() closes the capture before exit. The destructor must
 * not report a finalization failure when encountering PS3DK_PROF_EALREADY. */
#include <assert.h>
#include <stdio.h>
#include <ps3dk/prof_auto.h>

int main(void)
{
    int rc = PS3DK_PROF_AUTO_SEAL();
    assert(rc == 0);
    puts("AUTO app seal PASS");
    return 0;
}
