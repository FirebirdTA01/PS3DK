/* Entry point that run-hook-inject.cmake adds to a sample: starts the
   ps3dk-run hook, then runs the sample's own main (compiled as
   ps3dk_sample_main) with the arguments the hook leaves: the process's argv
   with the hook's --ps3dk-run= marker removed.  Built as C or C++ to match the
   sample, so the name links either way.  The hook reports the sample's exit
   status.

   The sample's main used to be called with no arguments at all, so a sample
   that reads argc/argv saw whatever the argument registers held (argc 0 on a
   console: fflush's return value), never what the host passed after "--". */
#include <stdio.h>

#include "ps3dk_run_hook.h"

/* run-hook-inject.cmake renames main for the whole target, this file included:
   undo it here, after the headers, so this file defines the real entry point. */
#undef main

#ifdef __cplusplus
/* A renamed C++ main is an ordinary, mangled function, and each parameter list
   mangles differently: declare the usual ones weak and call the one the sample
   defines.  None defined (some other signature) is reported, not skipped. */
int ps3dk_sample_main() __attribute__((weak));
int ps3dk_sample_main(int, char **) __attribute__((weak));
int ps3dk_sample_main(int, const char **) __attribute__((weak));
#else
/* C does not mangle: calling an int main(void) with two arguments is harmless. */
int ps3dk_sample_main(int, char **);
#endif

int main(int argc, const char *argv[])
{
    int hooked = ps3dk_run_hook_start(&argc, &argv);
    printf("PSGL_SAMPLE_BOOT hook=%d\n", hooked);
    fflush(stdout);
#ifdef __cplusplus
    int (*with_args)(int, char **) = ps3dk_sample_main;
    if (with_args)
        return with_args(argc, (char **)argv);
    int (*with_const_args)(int, const char **) = ps3dk_sample_main;
    if (with_const_args)
        return with_const_args(argc, argv);
    int (*no_args)() = ps3dk_sample_main;
    if (no_args)
        return no_args();
    printf("PSGL_SAMPLE_BOOT no sample main: main() / main(int, char **) / main(int, const char **) expected\n");
    fflush(stdout);
    return 127;
#else
    return ps3dk_sample_main(argc, (char **)argv);
#endif
}
