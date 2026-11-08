/* Link-time stand-ins for the profile writer variants: defines ps3dk_sample_main (as the run-hook injection does)
 * and/or ps3dk_prof_live_start (as the live members do), selected by -DSTUB_HOOK / -DSTUB_LIVE. */
#include <ps3dk/prof.h>

#ifdef STUB_HOOK
int ps3dk_sample_main(int argc, char **argv);
int ps3dk_sample_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 0;
}
#endif
#ifdef STUB_LIVE
int ps3dk_prof_live_start(const ps3dk_prof_live_config *cfg)
{
    (void)cfg;
    return 0;
}
#endif
int profile_test_stub_unit;
