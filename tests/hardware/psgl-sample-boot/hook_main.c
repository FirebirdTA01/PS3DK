/* Entry point that run-hook-inject.cmake adds to a sample: starts the
   ps3dk-run hook, then runs the sample's own main (compiled as
   ps3dk_sample_main).  Built as C or C++ to match the sample, so the name
   links either way.  The hook reports the sample's exit status. */
#include <stdio.h>

#include "ps3dk_run_hook.h"

int ps3dk_sample_main(void);

int main(int argc, const char *argv[])
{
    int hooked = ps3dk_run_hook_start(&argc, &argv);
    printf("PSGL_SAMPLE_BOOT hook=%d\n", hooked);
    fflush(stdout);
    return ps3dk_sample_main();
}
